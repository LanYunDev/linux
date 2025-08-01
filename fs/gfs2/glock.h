/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) Sistina Software, Inc.  1997-2003 All rights reserved.
 * Copyright (C) 2004-2006 Red Hat, Inc.  All rights reserved.
 */

#ifndef __GLOCK_DOT_H__
#define __GLOCK_DOT_H__

#include <linux/sched.h>
#include <linux/parser.h>
#include "incore.h"
#include "util.h"

/* Options for hostdata parser */

enum {
	Opt_jid,
	Opt_id,
	Opt_first,
	Opt_nodir,
	Opt_err,
};

/*
 * lm_lockname types
 */

#define LM_TYPE_RESERVED	0x00
#define LM_TYPE_NONDISK		0x01
#define LM_TYPE_INODE		0x02
#define LM_TYPE_RGRP		0x03
#define LM_TYPE_META		0x04
#define LM_TYPE_IOPEN		0x05
#define LM_TYPE_FLOCK		0x06
#define LM_TYPE_PLOCK		0x07
#define LM_TYPE_QUOTA		0x08
#define LM_TYPE_JOURNAL		0x09

/*
 * lm_lock() states
 *
 * SHARED is compatible with SHARED, not with DEFERRED or EX.
 * DEFERRED is compatible with DEFERRED, not with SHARED or EX.
 */

#define LM_ST_UNLOCKED		0
#define LM_ST_EXCLUSIVE		1
#define LM_ST_DEFERRED		2
#define LM_ST_SHARED		3

/*
 * lm_lock() flags
 *
 * LM_FLAG_TRY
 * Don't wait to acquire the lock if it can't be granted immediately.
 *
 * LM_FLAG_TRY_1CB
 * Send one blocking callback if TRY is set and the lock is not granted.
 *
 * LM_FLAG_NOEXP
 * GFS sets this flag on lock requests it makes while doing journal recovery.
 * These special requests should not be blocked due to the recovery like
 * ordinary locks would be.
 *
 * LM_FLAG_ANY
 * A SHARED request may also be granted in DEFERRED, or a DEFERRED request may
 * also be granted in SHARED.  The preferred state is whichever is compatible
 * with other granted locks, or the specified state if no other locks exist.
 *
 * LM_FLAG_NODE_SCOPE
 * This holder agrees to share the lock within this node. In other words,
 * the glock is held in EX mode according to DLM, but local holders on the
 * same node can share it.
 */

#define LM_FLAG_TRY		0x0001
#define LM_FLAG_TRY_1CB		0x0002
#define LM_FLAG_NOEXP		0x0004
#define LM_FLAG_ANY		0x0008
#define LM_FLAG_NODE_SCOPE	0x0020
#define GL_ASYNC		0x0040
#define GL_EXACT		0x0080
#define GL_SKIP			0x0100
#define GL_NOPID		0x0200
#define GL_NOCACHE		0x0400
#define GL_NOBLOCK		0x0800
  
/*
 * lm_async_cb return flags
 *
 * LM_OUT_ST_MASK
 * Masks the lower two bits of lock state in the returned value.
 *
 * LM_OUT_TRY_AGAIN
 * The trylock request failed.
 *
 * LM_OUT_DEADLOCK
 * The lock request failed because it would deadlock.
 *
 * LM_OUT_CANCELED
 * The lock request was canceled.
 *
 * LM_OUT_ERROR
 * The lock request timed out or failed.
 */

#define LM_OUT_ST_MASK		0x00000003
#define LM_OUT_TRY_AGAIN	0x00000020
#define LM_OUT_DEADLOCK		0x00000010
#define LM_OUT_CANCELED		0x00000008
#define LM_OUT_ERROR		0x00000004

/*
 * lm_recovery_done() messages
 */

#define LM_RD_GAVEUP		308
#define LM_RD_SUCCESS		309

#define GLR_TRYFAILED		13

#define GL_GLOCK_MAX_HOLD        (long)(HZ / 5)
#define GL_GLOCK_DFT_HOLD        (long)(HZ / 5)
#define GL_GLOCK_MIN_HOLD        (long)(10)
#define GL_GLOCK_HOLD_INCR       (long)(HZ / 20)
#define GL_GLOCK_HOLD_DECR       (long)(HZ / 40)

struct lm_lockops {
	const char *lm_proto_name;
	int (*lm_mount) (struct gfs2_sbd *sdp, const char *table);
	void (*lm_first_done) (struct gfs2_sbd *sdp);
	void (*lm_recovery_result) (struct gfs2_sbd *sdp, unsigned int jid,
				    unsigned int result);
	void (*lm_unmount) (struct gfs2_sbd *sdp);
	void (*lm_withdraw) (struct gfs2_sbd *sdp);
	void (*lm_put_lock) (struct gfs2_glock *gl);
	int (*lm_lock) (struct gfs2_glock *gl, unsigned int req_state,
			unsigned int flags);
	void (*lm_cancel) (struct gfs2_glock *gl);
	const match_table_t *lm_tokens;
};

struct gfs2_glock_aspace {
	struct gfs2_glock glock;
	struct address_space mapping;
};

static inline struct gfs2_holder *gfs2_glock_is_locked_by_me(struct gfs2_glock *gl)
{
	struct gfs2_holder *gh;
	struct pid *pid;

	/* Look in glock's list of holders for one with current task as owner */
	spin_lock(&gl->gl_lockref.lock);
	pid = task_pid(current);
	list_for_each_entry(gh, &gl->gl_holders, gh_list) {
		if (!test_bit(HIF_HOLDER, &gh->gh_iflags))
			break;
		if (gh->gh_owner_pid == pid)
			goto out;
	}
	gh = NULL;
out:
	spin_unlock(&gl->gl_lockref.lock);

	return gh;
}

static inline struct address_space *gfs2_glock2aspace(struct gfs2_glock *gl)
{
	if (gl->gl_ops->go_flags & GLOF_ASPACE) {
		struct gfs2_glock_aspace *gla =
			container_of(gl, struct gfs2_glock_aspace, glock);
		return &gla->mapping;
	}
	return NULL;
}

int gfs2_glock_get(struct gfs2_sbd *sdp, u64 number,
		   const struct gfs2_glock_operations *glops,
		   int create, struct gfs2_glock **glp);
struct gfs2_glock *gfs2_glock_hold(struct gfs2_glock *gl);
void gfs2_glock_put(struct gfs2_glock *gl);
void gfs2_glock_put_async(struct gfs2_glock *gl);

void __gfs2_holder_init(struct gfs2_glock *gl, unsigned int state,
		        u16 flags, struct gfs2_holder *gh,
		        unsigned long ip);
static inline void gfs2_holder_init(struct gfs2_glock *gl, unsigned int state,
				    u16 flags, struct gfs2_holder *gh) {
	__gfs2_holder_init(gl, state, flags, gh, _RET_IP_);
}

void gfs2_holder_reinit(unsigned int state, u16 flags,
		        struct gfs2_holder *gh);
void gfs2_holder_uninit(struct gfs2_holder *gh);
int gfs2_glock_nq(struct gfs2_holder *gh);
int gfs2_glock_poll(struct gfs2_holder *gh);
int gfs2_instantiate(struct gfs2_holder *gh);
int gfs2_glock_holder_ready(struct gfs2_holder *gh);
int gfs2_glock_wait(struct gfs2_holder *gh);
int gfs2_glock_async_wait(unsigned int num_gh, struct gfs2_holder *ghs);
void gfs2_glock_dq(struct gfs2_holder *gh);
void gfs2_glock_dq_wait(struct gfs2_holder *gh);
void gfs2_glock_dq_uninit(struct gfs2_holder *gh);
int gfs2_glock_nq_num(struct gfs2_sbd *sdp, u64 number,
		      const struct gfs2_glock_operations *glops,
		      unsigned int state, u16 flags,
		      struct gfs2_holder *gh);
int gfs2_glock_nq_m(unsigned int num_gh, struct gfs2_holder *ghs);
void gfs2_glock_dq_m(unsigned int num_gh, struct gfs2_holder *ghs);
void gfs2_dump_glock(struct seq_file *seq, struct gfs2_glock *gl,
			    bool fsid);
#define GLOCK_BUG_ON(gl,x) do { if (unlikely(x)) {		\
			gfs2_dump_glock(NULL, gl, true);	\
			BUG(); } } while(0)
#define gfs2_glock_assert_warn(gl, x) do { if (unlikely(!(x))) {	\
			gfs2_dump_glock(NULL, gl, true);		\
			gfs2_assert_warn((gl)->gl_name.ln_sbd, (x)); } } \
	while (0)
#define gfs2_glock_assert_withdraw(gl, x) do { if (unlikely(!(x))) {	\
			gfs2_dump_glock(NULL, gl, true);		\
			gfs2_assert_withdraw((gl)->gl_name.ln_sbd, (x)); } } \
	while (0)

__printf(2, 3)
void gfs2_print_dbg(struct seq_file *seq, const char *fmt, ...);

/**
 * gfs2_glock_nq_init - initialize a holder and enqueue it on a glock
 * @gl: the glock
 * @state: the state we're requesting
 * @flags: the modifier flags
 * @gh: the holder structure
 *
 * Returns: 0, GLR_*, or errno
 */

static inline int gfs2_glock_nq_init(struct gfs2_glock *gl,
				     unsigned int state, u16 flags,
				     struct gfs2_holder *gh)
{
	int error;

	__gfs2_holder_init(gl, state, flags, gh, _RET_IP_);

	error = gfs2_glock_nq(gh);
	if (error)
		gfs2_holder_uninit(gh);

	return error;
}

void gfs2_glock_cb(struct gfs2_glock *gl, unsigned int state);
void gfs2_glock_complete(struct gfs2_glock *gl, int ret);
bool gfs2_queue_try_to_evict(struct gfs2_glock *gl);
bool gfs2_queue_verify_delete(struct gfs2_glock *gl, bool later);
void gfs2_cancel_delete_work(struct gfs2_glock *gl);
void gfs2_flush_delete_work(struct gfs2_sbd *sdp);
void gfs2_gl_hash_clear(struct gfs2_sbd *sdp);
void gfs2_gl_dq_holders(struct gfs2_sbd *sdp);
void gfs2_glock_thaw(struct gfs2_sbd *sdp);
void gfs2_glock_free(struct gfs2_glock *gl);
void gfs2_glock_free_later(struct gfs2_glock *gl);

int __init gfs2_glock_init(void);
void gfs2_glock_exit(void);

void gfs2_create_debugfs_file(struct gfs2_sbd *sdp);
void gfs2_delete_debugfs_file(struct gfs2_sbd *sdp);
void gfs2_register_debugfs(void);
void gfs2_unregister_debugfs(void);

void glock_set_object(struct gfs2_glock *gl, void *object);
void glock_clear_object(struct gfs2_glock *gl, void *object);

extern const struct lm_lockops gfs2_dlm_ops;

static inline void gfs2_holder_mark_uninitialized(struct gfs2_holder *gh)
{
	gh->gh_gl = NULL;
}

static inline bool gfs2_holder_initialized(struct gfs2_holder *gh)
{
	return gh->gh_gl;
}

static inline bool gfs2_holder_queued(struct gfs2_holder *gh)
{
	return !list_empty(&gh->gh_list);
}

void gfs2_inode_remember_delete(struct gfs2_glock *gl, u64 generation);
bool gfs2_inode_already_deleted(struct gfs2_glock *gl, u64 generation);

static inline bool glock_needs_demote(struct gfs2_glock *gl)
{
	return (test_bit(GLF_DEMOTE, &gl->gl_flags) ||
		test_bit(GLF_PENDING_DEMOTE, &gl->gl_flags));
}

#endif /* __GLOCK_DOT_H__ */
