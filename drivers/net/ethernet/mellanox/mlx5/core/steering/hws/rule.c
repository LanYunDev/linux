// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/* Copyright (c) 2024 NVIDIA Corporation & Affiliates */

#include "internal.h"

void mlx5hws_rule_skip(struct mlx5hws_matcher *matcher, u32 flow_source,
		       bool *skip_rx, bool *skip_tx)
{
	/* By default FDB rules are added to both RX and TX */
	*skip_rx = false;
	*skip_tx = false;

	if (flow_source == MLX5_FLOW_CONTEXT_FLOW_SOURCE_LOCAL_VPORT) {
		*skip_rx = true;
		return;
	}

	if (flow_source == MLX5_FLOW_CONTEXT_FLOW_SOURCE_UPLINK) {
		*skip_tx = true;
		return;
	}

	/* If no flow source was set for current rule,
	 * check for flow source in matcher attributes.
	 */
	*skip_tx = matcher->attr.optimize_flow_src ==
			MLX5HWS_MATCHER_FLOW_SRC_WIRE;
	*skip_rx = matcher->attr.optimize_flow_src ==
			MLX5HWS_MATCHER_FLOW_SRC_VPORT;
}

static void
hws_rule_update_copy_tag(struct mlx5hws_rule *rule,
			 struct mlx5hws_wqe_gta_data_seg_ste *wqe_data,
			 bool is_jumbo)
{
	struct mlx5hws_rule_match_tag *tag;

	if (!mlx5hws_matcher_is_resizable(rule->matcher)) {
		tag = &rule->tag;
	} else {
		struct mlx5hws_wqe_gta_data_seg_ste *data_seg =
			(struct mlx5hws_wqe_gta_data_seg_ste *)(void *)rule->resize_info->data_seg;
		tag = (struct mlx5hws_rule_match_tag *)(void *)data_seg->action;
	}

	if (is_jumbo)
		memcpy(wqe_data->jumbo, tag->jumbo, MLX5HWS_JUMBO_TAG_SZ);
	else
		memcpy(wqe_data->tag, tag->match, MLX5HWS_MATCH_TAG_SZ);
}

static void hws_rule_init_dep_wqe(struct mlx5hws_send_ring_dep_wqe *dep_wqe,
				  struct mlx5hws_rule *rule,
				  struct mlx5hws_match_template *mt,
				  struct mlx5hws_rule_attr *attr)
{
	struct mlx5hws_matcher *matcher = rule->matcher;
	struct mlx5hws_table *tbl = matcher->tbl;
	bool skip_rx, skip_tx;

	dep_wqe->rule = rule;
	dep_wqe->user_data = attr->user_data;
	dep_wqe->direct_index = mlx5hws_matcher_is_insert_by_idx(matcher) ?
				attr->rule_idx : 0;

	if (tbl->type == MLX5HWS_TABLE_TYPE_FDB) {
		mlx5hws_rule_skip(matcher, attr->flow_source,
				  &skip_rx, &skip_tx);

		if (!skip_rx) {
			dep_wqe->rtc_0 = matcher->match_ste.rtc_0_id;
			dep_wqe->retry_rtc_0 = matcher->col_matcher ?
					       matcher->col_matcher->match_ste.rtc_0_id : 0;
		} else {
			dep_wqe->rtc_0 = 0;
			dep_wqe->retry_rtc_0 = 0;
		}

		if (!skip_tx) {
			dep_wqe->rtc_1 = matcher->match_ste.rtc_1_id;
			dep_wqe->retry_rtc_1 = matcher->col_matcher ?
					       matcher->col_matcher->match_ste.rtc_1_id : 0;
		} else {
			dep_wqe->rtc_1 = 0;
			dep_wqe->retry_rtc_1 = 0;
		}
	} else {
		pr_warn("HWS: invalid tbl->type: %d\n", tbl->type);
	}
}

static void hws_rule_move_get_rtc(struct mlx5hws_rule *rule,
				  struct mlx5hws_send_ste_attr *ste_attr)
{
	struct mlx5hws_matcher *dst_matcher = rule->matcher->resize_dst;

	if (rule->resize_info->rtc_0) {
		ste_attr->rtc_0 = dst_matcher->match_ste.rtc_0_id;
		ste_attr->retry_rtc_0 = dst_matcher->col_matcher ?
					dst_matcher->col_matcher->match_ste.rtc_0_id : 0;
	}
	if (rule->resize_info->rtc_1) {
		ste_attr->rtc_1 = dst_matcher->match_ste.rtc_1_id;
		ste_attr->retry_rtc_1 = dst_matcher->col_matcher ?
					dst_matcher->col_matcher->match_ste.rtc_1_id : 0;
	}
}

static void hws_rule_gen_comp(struct mlx5hws_send_engine *queue,
			      struct mlx5hws_rule *rule,
			      bool err,
			      void *user_data,
			      enum mlx5hws_rule_status rule_status_on_succ)
{
	enum mlx5hws_flow_op_status comp_status;

	if (!err) {
		comp_status = MLX5HWS_FLOW_OP_SUCCESS;
		rule->status = rule_status_on_succ;
	} else {
		comp_status = MLX5HWS_FLOW_OP_ERROR;
		rule->status = MLX5HWS_RULE_STATUS_FAILED;
	}

	mlx5hws_send_engine_inc_rule(queue);
	mlx5hws_send_engine_gen_comp(queue, user_data, comp_status);
}

static void
hws_rule_save_resize_info(struct mlx5hws_rule *rule,
			  struct mlx5hws_send_ste_attr *ste_attr)
{
	if (!mlx5hws_matcher_is_resizable(rule->matcher))
		return;

	/* resize_info might already exist (if we're in update flow) */
	if (likely(!rule->resize_info)) {
		rule->resize_info = kzalloc(sizeof(*rule->resize_info), GFP_KERNEL);
		if (unlikely(!rule->resize_info)) {
			pr_warn("HWS: resize info isn't allocated for rule\n");
			return;
		}
	}

	memcpy(rule->resize_info->ctrl_seg, ste_attr->wqe_ctrl,
	       sizeof(rule->resize_info->ctrl_seg));
	memcpy(rule->resize_info->data_seg, ste_attr->wqe_data,
	       sizeof(rule->resize_info->data_seg));
}

void mlx5hws_rule_clear_resize_info(struct mlx5hws_rule *rule)
{
	if (mlx5hws_matcher_is_resizable(rule->matcher) &&
	    rule->resize_info) {
		kfree(rule->resize_info);
		rule->resize_info = NULL;
	}
}

static void
hws_rule_save_delete_info(struct mlx5hws_rule *rule,
			  struct mlx5hws_send_ste_attr *ste_attr)
{
	struct mlx5hws_match_template *mt = rule->matcher->mt;
	bool is_jumbo = mlx5hws_matcher_mt_is_jumbo(mt);

	if (mlx5hws_matcher_is_resizable(rule->matcher))
		return;

	if (is_jumbo)
		memcpy(&rule->tag.jumbo, ste_attr->wqe_data->jumbo, MLX5HWS_JUMBO_TAG_SZ);
	else
		memcpy(&rule->tag.match, ste_attr->wqe_data->tag, MLX5HWS_MATCH_TAG_SZ);
}

static void
hws_rule_clear_delete_info(struct mlx5hws_rule *rule)
{
	/* nothing to do here */
}

static void
hws_rule_load_delete_info(struct mlx5hws_rule *rule,
			  struct mlx5hws_send_ste_attr *ste_attr)
{
	if (unlikely(!mlx5hws_matcher_is_resizable(rule->matcher))) {
		ste_attr->wqe_tag = &rule->tag;
	} else {
		struct mlx5hws_wqe_gta_data_seg_ste *data_seg =
			(struct mlx5hws_wqe_gta_data_seg_ste *)(void *)rule->resize_info->data_seg;
		struct mlx5hws_rule_match_tag *tag =
			(struct mlx5hws_rule_match_tag *)(void *)data_seg->action;
		ste_attr->wqe_tag = tag;
	}
}

static int mlx5hws_rule_alloc_action_ste(struct mlx5hws_rule *rule,
					 u16 queue_id, bool skip_rx,
					 bool skip_tx)
{
	struct mlx5hws_matcher *matcher = rule->matcher;
	struct mlx5hws_context *ctx = matcher->tbl->ctx;

	rule->action_ste.ste.order =
		ilog2(roundup_pow_of_two(matcher->num_of_action_stes));
	return mlx5hws_action_ste_chunk_alloc(&ctx->action_ste_pool[queue_id],
					      skip_rx, skip_tx,
					      &rule->action_ste);
}

void mlx5hws_rule_free_action_ste(struct mlx5hws_action_ste_chunk *action_ste)
{
	if (!action_ste->action_tbl)
		return;

	/* This release is safe only when the rule match STE was deleted
	 * (when the rule is being deleted) or replaced with the new STE that
	 * isn't pointing to old action STEs (when the rule is being updated).
	 */
	mlx5hws_action_ste_chunk_free(action_ste);
}

static void hws_rule_create_init(struct mlx5hws_rule *rule,
				 struct mlx5hws_send_ste_attr *ste_attr,
				 struct mlx5hws_actions_apply_data *apply,
				 bool is_update)
{
	struct mlx5hws_matcher *matcher = rule->matcher;
	struct mlx5hws_table *tbl = matcher->tbl;
	struct mlx5hws_context *ctx = tbl->ctx;

	/* Init rule before reuse */
	if (!is_update) {
		/* In update we use these rtc's */
		rule->rtc_0 = 0;
		rule->rtc_1 = 0;

		rule->status = MLX5HWS_RULE_STATUS_CREATING;
	} else {
		rule->status = MLX5HWS_RULE_STATUS_UPDATING;
		/* Save the old action STE info so we can free it after writing
		 * new action STEs and a corresponding match STE.
		 */
		rule->old_action_ste = rule->action_ste;
	}

	rule->pending_wqes = 0;

	/* Init default send STE attributes */
	ste_attr->gta_opcode = MLX5HWS_WQE_GTA_OP_ACTIVATE;
	ste_attr->send_attr.opmod = MLX5HWS_WQE_GTA_OPMOD_STE;
	ste_attr->send_attr.opcode = MLX5HWS_WQE_OPCODE_TBL_ACCESS;
	ste_attr->send_attr.len = MLX5HWS_WQE_SZ_GTA_CTRL + MLX5HWS_WQE_SZ_GTA_DATA;

	/* Init default action apply */
	apply->tbl_type = tbl->type;
	apply->common_res = &ctx->common_res;
	apply->require_dep = 0;
}

static void hws_rule_move_init(struct mlx5hws_rule *rule,
			       struct mlx5hws_rule_attr *attr)
{
	/* Save the old RTC IDs to be later used in match STE delete */
	rule->resize_info->rtc_0 = rule->rtc_0;
	rule->resize_info->rtc_1 = rule->rtc_1;
	rule->resize_info->rule_idx = attr->rule_idx;

	rule->rtc_0 = 0;
	rule->rtc_1 = 0;

	rule->pending_wqes = 0;
	rule->status = MLX5HWS_RULE_STATUS_CREATING;
	rule->resize_info->state = MLX5HWS_RULE_RESIZE_STATE_WRITING;
}

bool mlx5hws_rule_move_in_progress(struct mlx5hws_rule *rule)
{
	return mlx5hws_matcher_is_in_resize(rule->matcher) &&
	       rule->resize_info &&
	       rule->resize_info->state != MLX5HWS_RULE_RESIZE_STATE_IDLE;
}

static int hws_rule_create_hws(struct mlx5hws_rule *rule,
			       struct mlx5hws_rule_attr *attr,
			       u8 mt_idx,
			       u32 *match_param,
			       u8 at_idx,
			       struct mlx5hws_rule_action rule_actions[])
{
	struct mlx5hws_action_template *at = &rule->matcher->at[at_idx];
	struct mlx5hws_match_template *mt = &rule->matcher->mt[mt_idx];
	bool is_jumbo = mlx5hws_matcher_mt_is_jumbo(mt);
	struct mlx5hws_matcher *matcher = rule->matcher;
	struct mlx5hws_context *ctx = matcher->tbl->ctx;
	struct mlx5hws_send_ste_attr ste_attr = {0};
	struct mlx5hws_send_ring_dep_wqe *dep_wqe;
	struct mlx5hws_actions_wqe_setter *setter;
	struct mlx5hws_actions_apply_data apply;
	struct mlx5hws_send_engine *queue;
	u8 total_stes, action_stes;
	bool is_update;
	int i, ret;

	is_update = !match_param;

	setter = &at->setters[at->num_of_action_stes];
	total_stes = at->num_of_action_stes + (is_jumbo && !at->only_term);
	action_stes = total_stes - 1;

	queue = &ctx->send_queue[attr->queue_id];
	if (unlikely(mlx5hws_send_engine_err(queue)))
		return -EIO;

	hws_rule_create_init(rule, &ste_attr, &apply, is_update);

	/* Allocate dependent match WQE since rule might have dependent writes.
	 * The queued dependent WQE can be later aborted or kept as a dependency.
	 * dep_wqe buffers (ctrl, data) are also reused for all STE writes.
	 */
	dep_wqe = mlx5hws_send_add_new_dep_wqe(queue);
	hws_rule_init_dep_wqe(dep_wqe, rule, mt, attr);

	ste_attr.wqe_ctrl = &dep_wqe->wqe_ctrl;
	ste_attr.wqe_data = &dep_wqe->wqe_data;
	apply.wqe_ctrl = &dep_wqe->wqe_ctrl;
	apply.wqe_data = (__force __be32 *)&dep_wqe->wqe_data;
	apply.rule_action = rule_actions;
	apply.queue = queue;

	if (action_stes) {
		/* Allocate action STEs for rules that need more than match STE */
		ret = mlx5hws_rule_alloc_action_ste(rule, attr->queue_id,
						    !!ste_attr.rtc_0,
						    !!ste_attr.rtc_1);
		if (ret) {
			mlx5hws_err(ctx, "Failed to allocate action memory %d", ret);
			mlx5hws_send_abort_new_dep_wqe(queue);
			return ret;
		}
		apply.jump_to_action_stc =
			rule->action_ste.action_tbl->stc.offset;
		/* Skip RX/TX based on the dep_wqe init */
		ste_attr.rtc_0 = dep_wqe->rtc_0 ?
				 rule->action_ste.action_tbl->rtc_0_id : 0;
		ste_attr.rtc_1 = dep_wqe->rtc_1 ?
				 rule->action_ste.action_tbl->rtc_1_id : 0;
		/* Action STEs are written to a specific index last to first */
		ste_attr.direct_index =
			rule->action_ste.ste.offset + action_stes;
		apply.next_direct_idx = ste_attr.direct_index;
	} else {
		apply.next_direct_idx = 0;
	}

	for (i = total_stes; i-- > 0;) {
		mlx5hws_action_apply_setter(&apply, setter--, !i && is_jumbo);

		if (i == 0) {
			/* Handle last match STE.
			 * For hash split / linear lookup RTCs, packets reaching any STE
			 * will always match and perform the specified actions, which
			 * makes the tag irrelevant.
			 */
			if (likely(!mlx5hws_matcher_is_insert_by_idx(matcher) && !is_update))
				mlx5hws_definer_create_tag(match_param, mt->fc, mt->fc_sz,
							   (u8 *)dep_wqe->wqe_data.action);
			else if (is_update)
				hws_rule_update_copy_tag(rule, &dep_wqe->wqe_data, is_jumbo);

			/* Rule has dependent WQEs, match dep_wqe is queued */
			if (action_stes || apply.require_dep)
				break;

			/* Rule has no dependencies, abort dep_wqe and send WQE now */
			mlx5hws_send_abort_new_dep_wqe(queue);
			ste_attr.wqe_tag_is_jumbo = is_jumbo;
			ste_attr.send_attr.notify_hw = !attr->burst;
			ste_attr.send_attr.user_data = dep_wqe->user_data;
			ste_attr.send_attr.rule = dep_wqe->rule;
			ste_attr.rtc_0 = dep_wqe->rtc_0;
			ste_attr.rtc_1 = dep_wqe->rtc_1;
			ste_attr.used_id_rtc_0 = &rule->rtc_0;
			ste_attr.used_id_rtc_1 = &rule->rtc_1;
			ste_attr.retry_rtc_0 = dep_wqe->retry_rtc_0;
			ste_attr.retry_rtc_1 = dep_wqe->retry_rtc_1;
			ste_attr.direct_index = dep_wqe->direct_index;
		} else {
			apply.next_direct_idx = --ste_attr.direct_index;
		}

		mlx5hws_send_ste(queue, &ste_attr);
	}

	/* Backup TAG on the rule for deletion and resize info for
	 * moving rules to a new matcher, only after insertion.
	 */
	if (!is_update)
		hws_rule_save_delete_info(rule, &ste_attr);

	hws_rule_save_resize_info(rule, &ste_attr);
	mlx5hws_send_engine_inc_rule(queue);

	if (!attr->burst)
		mlx5hws_send_all_dep_wqe(queue);

	return 0;
}

static void hws_rule_destroy_failed_hws(struct mlx5hws_rule *rule,
					struct mlx5hws_rule_attr *attr)
{
	struct mlx5hws_context *ctx = rule->matcher->tbl->ctx;
	struct mlx5hws_send_engine *queue;

	queue = &ctx->send_queue[attr->queue_id];

	hws_rule_gen_comp(queue, rule, false,
			  attr->user_data, MLX5HWS_RULE_STATUS_DELETED);

	/* Rule failed now we can safely release action STEs */
	mlx5hws_rule_free_action_ste(&rule->action_ste);

	/* Perhaps the rule failed updating - release old action STEs as well */
	mlx5hws_rule_free_action_ste(&rule->old_action_ste);

	/* Clear complex tag */
	hws_rule_clear_delete_info(rule);

	/* Clear info that was saved for resizing */
	mlx5hws_rule_clear_resize_info(rule);

	/* If a rule that was indicated as burst (need to trigger HW) has failed
	 * insertion we won't ring the HW as nothing is being written to the WQ.
	 * In such case update the last WQE and ring the HW with that work
	 */
	if (attr->burst)
		return;

	mlx5hws_send_all_dep_wqe(queue);
	mlx5hws_send_engine_flush_queue(queue);
}

static int hws_rule_destroy_hws(struct mlx5hws_rule *rule,
				struct mlx5hws_rule_attr *attr)
{
	bool is_jumbo = mlx5hws_matcher_mt_is_jumbo(rule->matcher->mt);
	struct mlx5hws_context *ctx = rule->matcher->tbl->ctx;
	struct mlx5hws_matcher *matcher = rule->matcher;
	struct mlx5hws_wqe_gta_ctrl_seg wqe_ctrl = {0};
	struct mlx5hws_send_ste_attr ste_attr = {0};
	struct mlx5hws_send_engine *queue;

	queue = &ctx->send_queue[attr->queue_id];

	if (unlikely(mlx5hws_send_engine_err(queue))) {
		hws_rule_destroy_failed_hws(rule, attr);
		return 0;
	}

	/* Rule is not completed yet */
	if (rule->status == MLX5HWS_RULE_STATUS_CREATING ||
	    rule->status == MLX5HWS_RULE_STATUS_UPDATING)
		return -EBUSY;

	/* Rule failed and doesn't require cleanup */
	if (rule->status == MLX5HWS_RULE_STATUS_FAILED) {
		hws_rule_destroy_failed_hws(rule, attr);
		return 0;
	}

	if (rule->skip_delete) {
		/* Rule shouldn't be deleted in HW.
		 * Generate completion as if write succeeded, and we can
		 * safely release action STEs and clear resize info.
		 */
		hws_rule_gen_comp(queue, rule, false,
				  attr->user_data, MLX5HWS_RULE_STATUS_DELETED);

		mlx5hws_rule_free_action_ste(&rule->action_ste);
		mlx5hws_rule_clear_resize_info(rule);
		return 0;
	}

	mlx5hws_send_engine_inc_rule(queue);

	/* Send dependent WQE */
	if (!attr->burst)
		mlx5hws_send_all_dep_wqe(queue);

	rule->status = MLX5HWS_RULE_STATUS_DELETING;

	ste_attr.send_attr.opmod = MLX5HWS_WQE_GTA_OPMOD_STE;
	ste_attr.send_attr.opcode = MLX5HWS_WQE_OPCODE_TBL_ACCESS;
	ste_attr.send_attr.len = MLX5HWS_WQE_SZ_GTA_CTRL + MLX5HWS_WQE_SZ_GTA_DATA;

	ste_attr.send_attr.rule = rule;
	ste_attr.send_attr.notify_hw = !attr->burst;
	ste_attr.send_attr.user_data = attr->user_data;

	ste_attr.rtc_0 = rule->rtc_0;
	ste_attr.rtc_1 = rule->rtc_1;
	ste_attr.used_id_rtc_0 = &rule->rtc_0;
	ste_attr.used_id_rtc_1 = &rule->rtc_1;
	ste_attr.wqe_ctrl = &wqe_ctrl;
	ste_attr.wqe_tag_is_jumbo = is_jumbo;
	ste_attr.gta_opcode = MLX5HWS_WQE_GTA_OP_DEACTIVATE;
	if (unlikely(mlx5hws_matcher_is_insert_by_idx(matcher)))
		ste_attr.direct_index = attr->rule_idx;

	hws_rule_load_delete_info(rule, &ste_attr);
	mlx5hws_send_ste(queue, &ste_attr);
	hws_rule_clear_delete_info(rule);

	return 0;
}

static int hws_rule_enqueue_precheck(struct mlx5hws_rule *rule,
				     struct mlx5hws_rule_attr *attr)
{
	struct mlx5hws_context *ctx = rule->matcher->tbl->ctx;

	if (unlikely(!attr->user_data))
		return -EINVAL;

	/* Check if there is room in queue */
	if (unlikely(mlx5hws_send_engine_full(&ctx->send_queue[attr->queue_id])))
		return -EBUSY;

	return 0;
}

static int hws_rule_enqueue_precheck_move(struct mlx5hws_rule *rule,
					  struct mlx5hws_rule_attr *attr)
{
	if (unlikely(rule->status != MLX5HWS_RULE_STATUS_CREATED))
		return -EINVAL;

	return hws_rule_enqueue_precheck(rule, attr);
}

static int hws_rule_enqueue_precheck_create(struct mlx5hws_rule *rule,
					    struct mlx5hws_rule_attr *attr)
{
	if (unlikely(mlx5hws_matcher_is_in_resize(rule->matcher)))
		/* Matcher in resize - new rules are not allowed */
		return -EAGAIN;

	return hws_rule_enqueue_precheck(rule, attr);
}

static int hws_rule_enqueue_precheck_update(struct mlx5hws_rule *rule,
					    struct mlx5hws_rule_attr *attr)
{
	struct mlx5hws_matcher *matcher = rule->matcher;

	if (unlikely(!mlx5hws_matcher_is_resizable(rule->matcher) &&
		     !matcher->attr.optimize_using_rule_idx &&
		     !mlx5hws_matcher_is_insert_by_idx(matcher))) {
		return -EOPNOTSUPP;
	}

	if (unlikely(rule->status != MLX5HWS_RULE_STATUS_CREATED))
		return -EBUSY;

	return hws_rule_enqueue_precheck_create(rule, attr);
}

int mlx5hws_rule_move_hws_remove(struct mlx5hws_rule *rule,
				 void *queue_ptr,
				 void *user_data)
{
	bool is_jumbo = mlx5hws_matcher_mt_is_jumbo(rule->matcher->mt);
	struct mlx5hws_wqe_gta_ctrl_seg empty_wqe_ctrl = {0};
	struct mlx5hws_matcher *matcher = rule->matcher;
	struct mlx5hws_send_engine *queue = queue_ptr;
	struct mlx5hws_send_ste_attr ste_attr = {0};

	mlx5hws_send_all_dep_wqe(queue);

	rule->resize_info->state = MLX5HWS_RULE_RESIZE_STATE_DELETING;

	ste_attr.send_attr.fence = 0;
	ste_attr.send_attr.opmod = MLX5HWS_WQE_GTA_OPMOD_STE;
	ste_attr.send_attr.opcode = MLX5HWS_WQE_OPCODE_TBL_ACCESS;
	ste_attr.send_attr.len = MLX5HWS_WQE_SZ_GTA_CTRL + MLX5HWS_WQE_SZ_GTA_DATA;
	ste_attr.send_attr.rule = rule;
	ste_attr.send_attr.notify_hw = 1;
	ste_attr.send_attr.user_data = user_data;
	ste_attr.rtc_0 = rule->resize_info->rtc_0;
	ste_attr.rtc_1 = rule->resize_info->rtc_1;
	ste_attr.used_id_rtc_0 = &rule->resize_info->rtc_0;
	ste_attr.used_id_rtc_1 = &rule->resize_info->rtc_1;
	ste_attr.wqe_ctrl = &empty_wqe_ctrl;
	ste_attr.wqe_tag_is_jumbo = is_jumbo;
	ste_attr.gta_opcode = MLX5HWS_WQE_GTA_OP_DEACTIVATE;

	if (unlikely(mlx5hws_matcher_is_insert_by_idx(matcher)))
		ste_attr.direct_index = rule->resize_info->rule_idx;

	hws_rule_load_delete_info(rule, &ste_attr);
	mlx5hws_send_ste(queue, &ste_attr);

	return 0;
}

int mlx5hws_rule_move_hws_add(struct mlx5hws_rule *rule,
			      struct mlx5hws_rule_attr *attr)
{
	bool is_jumbo = mlx5hws_matcher_mt_is_jumbo(rule->matcher->mt);
	struct mlx5hws_context *ctx = rule->matcher->tbl->ctx;
	struct mlx5hws_matcher *matcher = rule->matcher;
	struct mlx5hws_send_ste_attr ste_attr = {0};
	struct mlx5hws_send_engine *queue;
	int ret;

	ret = hws_rule_enqueue_precheck_move(rule, attr);
	if (unlikely(ret))
		return ret;

	queue = &ctx->send_queue[attr->queue_id];

	ret = mlx5hws_send_engine_err(queue);
	if (ret)
		return ret;

	hws_rule_move_init(rule, attr);
	hws_rule_move_get_rtc(rule, &ste_attr);

	ste_attr.send_attr.opmod = MLX5HWS_WQE_GTA_OPMOD_STE;
	ste_attr.send_attr.opcode = MLX5HWS_WQE_OPCODE_TBL_ACCESS;
	ste_attr.send_attr.len = MLX5HWS_WQE_SZ_GTA_CTRL + MLX5HWS_WQE_SZ_GTA_DATA;
	ste_attr.gta_opcode = MLX5HWS_WQE_GTA_OP_ACTIVATE;
	ste_attr.wqe_tag_is_jumbo = is_jumbo;

	ste_attr.send_attr.rule = rule;
	ste_attr.send_attr.fence = 0;
	ste_attr.send_attr.notify_hw = !attr->burst;
	ste_attr.send_attr.user_data = attr->user_data;

	ste_attr.used_id_rtc_0 = &rule->rtc_0;
	ste_attr.used_id_rtc_1 = &rule->rtc_1;
	ste_attr.wqe_ctrl = (struct mlx5hws_wqe_gta_ctrl_seg *)rule->resize_info->ctrl_seg;
	ste_attr.wqe_data = (struct mlx5hws_wqe_gta_data_seg_ste *)rule->resize_info->data_seg;
	ste_attr.direct_index = mlx5hws_matcher_is_insert_by_idx(matcher) ?
				attr->rule_idx : 0;

	mlx5hws_send_ste(queue, &ste_attr);
	mlx5hws_send_engine_inc_rule(queue);

	if (!attr->burst)
		mlx5hws_send_all_dep_wqe(queue);

	return 0;
}

static u8 hws_rule_ethertype_to_matcher_ipv(u32 ethertype)
{
	switch (ethertype) {
	case ETH_P_IP:
		return MLX5HWS_MATCHER_IPV_4;
	case ETH_P_IPV6:
		return MLX5HWS_MATCHER_IPV_6;
	default:
		return MLX5HWS_MATCHER_IPV_UNSET;
	}
}

static u8 hws_rule_ip_version_to_matcher_ipv(u32 ip_version)
{
	switch (ip_version) {
	case 4:
		return MLX5HWS_MATCHER_IPV_4;
	case 6:
		return MLX5HWS_MATCHER_IPV_6;
	default:
		return MLX5HWS_MATCHER_IPV_UNSET;
	}
}

static int hws_rule_check_outer_ip_version(struct mlx5hws_matcher *matcher,
					   u32 *match_param)
{
	struct mlx5hws_context *ctx = matcher->tbl->ctx;
	u8 outer_ipv_ether = MLX5HWS_MATCHER_IPV_UNSET;
	u8 outer_ipv_ip = MLX5HWS_MATCHER_IPV_UNSET;
	u8 outer_ipv, ver;

	if (matcher->matches_outer_ethertype) {
		ver = MLX5_GET(fte_match_param, match_param,
			       outer_headers.ethertype);
		outer_ipv_ether = hws_rule_ethertype_to_matcher_ipv(ver);
	}
	if (matcher->matches_outer_ip_version) {
		ver = MLX5_GET(fte_match_param, match_param,
			       outer_headers.ip_version);
		outer_ipv_ip = hws_rule_ip_version_to_matcher_ipv(ver);
	}

	if (outer_ipv_ether != MLX5HWS_MATCHER_IPV_UNSET &&
	    outer_ipv_ip != MLX5HWS_MATCHER_IPV_UNSET &&
	    outer_ipv_ether != outer_ipv_ip) {
		mlx5hws_err(ctx, "Rule matches on inconsistent outer ethertype and ip version\n");
		return -EINVAL;
	}

	outer_ipv = outer_ipv_ether != MLX5HWS_MATCHER_IPV_UNSET ?
		    outer_ipv_ether : outer_ipv_ip;
	if (outer_ipv != MLX5HWS_MATCHER_IPV_UNSET &&
	    matcher->outer_ip_version != MLX5HWS_MATCHER_IPV_UNSET &&
	    outer_ipv != matcher->outer_ip_version) {
		mlx5hws_err(ctx, "Matcher and rule disagree on outer IP version\n");
		return -EINVAL;
	}
	matcher->outer_ip_version = outer_ipv;

	return 0;
}

static int hws_rule_check_inner_ip_version(struct mlx5hws_matcher *matcher,
					   u32 *match_param)
{
	struct mlx5hws_context *ctx = matcher->tbl->ctx;
	u8 inner_ipv_ether = MLX5HWS_MATCHER_IPV_UNSET;
	u8 inner_ipv_ip = MLX5HWS_MATCHER_IPV_UNSET;
	u8 inner_ipv, ver;

	if (matcher->matches_inner_ethertype) {
		ver = MLX5_GET(fte_match_param, match_param,
			       inner_headers.ethertype);
		inner_ipv_ether = hws_rule_ethertype_to_matcher_ipv(ver);
	}
	if (matcher->matches_inner_ip_version) {
		ver = MLX5_GET(fte_match_param, match_param,
			       inner_headers.ip_version);
		inner_ipv_ip = hws_rule_ip_version_to_matcher_ipv(ver);
	}

	if (inner_ipv_ether != MLX5HWS_MATCHER_IPV_UNSET &&
	    inner_ipv_ip != MLX5HWS_MATCHER_IPV_UNSET &&
	    inner_ipv_ether != inner_ipv_ip) {
		mlx5hws_err(ctx, "Rule matches on inconsistent inner ethertype and ip version\n");
		return -EINVAL;
	}

	inner_ipv = inner_ipv_ether != MLX5HWS_MATCHER_IPV_UNSET ?
		    inner_ipv_ether : inner_ipv_ip;
	if (inner_ipv != MLX5HWS_MATCHER_IPV_UNSET &&
	    matcher->inner_ip_version != MLX5HWS_MATCHER_IPV_UNSET &&
	    inner_ipv != matcher->inner_ip_version) {
		mlx5hws_err(ctx, "Matcher and rule disagree on inner IP version\n");
		return -EINVAL;
	}
	matcher->inner_ip_version = inner_ipv;

	return 0;
}

static int hws_rule_check_ip_version(struct mlx5hws_matcher *matcher,
				     u32 *match_param)
{
	int ret;

	ret = hws_rule_check_outer_ip_version(matcher, match_param);
	if (unlikely(ret))
		return ret;

	ret = hws_rule_check_inner_ip_version(matcher, match_param);
	if (unlikely(ret))
		return ret;

	return 0;
}

int mlx5hws_rule_create(struct mlx5hws_matcher *matcher,
			u8 mt_idx,
			u32 *match_param,
			u8 at_idx,
			struct mlx5hws_rule_action rule_actions[],
			struct mlx5hws_rule_attr *attr,
			struct mlx5hws_rule *rule_handle)
{
	int ret;

	ret = hws_rule_check_ip_version(matcher, match_param);
	if (unlikely(ret))
		return ret;

	rule_handle->matcher = matcher;

	ret = hws_rule_enqueue_precheck_create(rule_handle, attr);
	if (unlikely(ret))
		return ret;

	if (unlikely(!(matcher->num_of_mt >= mt_idx) ||
		     !(matcher->num_of_at >= at_idx) ||
		     !match_param)) {
		pr_warn("HWS: Invalid rule creation parameters (MTs, ATs or match params)\n");
		return -EINVAL;
	}

	ret = hws_rule_create_hws(rule_handle,
				  attr,
				  mt_idx,
				  match_param,
				  at_idx,
				  rule_actions);

	return ret;
}

int mlx5hws_rule_destroy(struct mlx5hws_rule *rule,
			 struct mlx5hws_rule_attr *attr)
{
	int ret;

	ret = hws_rule_enqueue_precheck(rule, attr);
	if (unlikely(ret))
		return ret;

	ret = hws_rule_destroy_hws(rule, attr);

	return ret;
}

int mlx5hws_rule_action_update(struct mlx5hws_rule *rule,
			       u8 at_idx,
			       struct mlx5hws_rule_action rule_actions[],
			       struct mlx5hws_rule_attr *attr)
{
	int ret;

	ret = hws_rule_enqueue_precheck_update(rule, attr);
	if (unlikely(ret))
		return ret;

	ret = hws_rule_create_hws(rule,
				  attr,
				  0,
				  NULL,
				  at_idx,
				  rule_actions);

	return ret;
}
