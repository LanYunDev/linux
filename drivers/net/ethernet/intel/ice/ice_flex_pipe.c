// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2019, Intel Corporation. */

#include "ice_common.h"
#include "ice_flex_pipe.h"
#include "ice_flow.h"
#include "ice.h"

static const u32 ice_sect_lkup[ICE_BLK_COUNT][ICE_SECT_COUNT] = {
	/* SWITCH */
	{
		ICE_SID_XLT0_SW,
		ICE_SID_XLT_KEY_BUILDER_SW,
		ICE_SID_XLT1_SW,
		ICE_SID_XLT2_SW,
		ICE_SID_PROFID_TCAM_SW,
		ICE_SID_PROFID_REDIR_SW,
		ICE_SID_FLD_VEC_SW,
		ICE_SID_CDID_KEY_BUILDER_SW,
		ICE_SID_CDID_REDIR_SW
	},

	/* ACL */
	{
		ICE_SID_XLT0_ACL,
		ICE_SID_XLT_KEY_BUILDER_ACL,
		ICE_SID_XLT1_ACL,
		ICE_SID_XLT2_ACL,
		ICE_SID_PROFID_TCAM_ACL,
		ICE_SID_PROFID_REDIR_ACL,
		ICE_SID_FLD_VEC_ACL,
		ICE_SID_CDID_KEY_BUILDER_ACL,
		ICE_SID_CDID_REDIR_ACL
	},

	/* FD */
	{
		ICE_SID_XLT0_FD,
		ICE_SID_XLT_KEY_BUILDER_FD,
		ICE_SID_XLT1_FD,
		ICE_SID_XLT2_FD,
		ICE_SID_PROFID_TCAM_FD,
		ICE_SID_PROFID_REDIR_FD,
		ICE_SID_FLD_VEC_FD,
		ICE_SID_CDID_KEY_BUILDER_FD,
		ICE_SID_CDID_REDIR_FD
	},

	/* RSS */
	{
		ICE_SID_XLT0_RSS,
		ICE_SID_XLT_KEY_BUILDER_RSS,
		ICE_SID_XLT1_RSS,
		ICE_SID_XLT2_RSS,
		ICE_SID_PROFID_TCAM_RSS,
		ICE_SID_PROFID_REDIR_RSS,
		ICE_SID_FLD_VEC_RSS,
		ICE_SID_CDID_KEY_BUILDER_RSS,
		ICE_SID_CDID_REDIR_RSS
	},

	/* PE */
	{
		ICE_SID_XLT0_PE,
		ICE_SID_XLT_KEY_BUILDER_PE,
		ICE_SID_XLT1_PE,
		ICE_SID_XLT2_PE,
		ICE_SID_PROFID_TCAM_PE,
		ICE_SID_PROFID_REDIR_PE,
		ICE_SID_FLD_VEC_PE,
		ICE_SID_CDID_KEY_BUILDER_PE,
		ICE_SID_CDID_REDIR_PE
	}
};

/**
 * ice_sect_id - returns section ID
 * @blk: block type
 * @sect: section type
 *
 * This helper function returns the proper section ID given a block type and a
 * section type.
 */
static u32 ice_sect_id(enum ice_block blk, enum ice_sect sect)
{
	return ice_sect_lkup[blk][sect];
}

/**
 * ice_hw_ptype_ena - check if the PTYPE is enabled or not
 * @hw: pointer to the HW structure
 * @ptype: the hardware PTYPE
 */
bool ice_hw_ptype_ena(struct ice_hw *hw, u16 ptype)
{
	return ptype < ICE_FLOW_PTYPE_MAX &&
	       test_bit(ptype, hw->hw_ptype);
}

/* Key creation */

#define ICE_DC_KEY	0x1	/* don't care */
#define ICE_DC_KEYINV	0x1
#define ICE_NM_KEY	0x0	/* never match */
#define ICE_NM_KEYINV	0x0
#define ICE_0_KEY	0x1	/* match 0 */
#define ICE_0_KEYINV	0x0
#define ICE_1_KEY	0x0	/* match 1 */
#define ICE_1_KEYINV	0x1

/**
 * ice_gen_key_word - generate 16-bits of a key/mask word
 * @val: the value
 * @valid: valid bits mask (change only the valid bits)
 * @dont_care: don't care mask
 * @nvr_mtch: never match mask
 * @key: pointer to an array of where the resulting key portion
 * @key_inv: pointer to an array of where the resulting key invert portion
 *
 * This function generates 16-bits from a 8-bit value, an 8-bit don't care mask
 * and an 8-bit never match mask. The 16-bits of output are divided into 8 bits
 * of key and 8 bits of key invert.
 *
 *     '0' =    b01, always match a 0 bit
 *     '1' =    b10, always match a 1 bit
 *     '?' =    b11, don't care bit (always matches)
 *     '~' =    b00, never match bit
 *
 * Input:
 *          val:         b0  1  0  1  0  1
 *          dont_care:   b0  0  1  1  0  0
 *          never_mtch:  b0  0  0  0  1  1
 *          ------------------------------
 * Result:  key:        b01 10 11 11 00 00
 */
static int
ice_gen_key_word(u8 val, u8 valid, u8 dont_care, u8 nvr_mtch, u8 *key,
		 u8 *key_inv)
{
	u8 in_key = *key, in_key_inv = *key_inv;
	u8 i;

	/* 'dont_care' and 'nvr_mtch' masks cannot overlap */
	if ((dont_care ^ nvr_mtch) != (dont_care | nvr_mtch))
		return -EIO;

	*key = 0;
	*key_inv = 0;

	/* encode the 8 bits into 8-bit key and 8-bit key invert */
	for (i = 0; i < 8; i++) {
		*key >>= 1;
		*key_inv >>= 1;

		if (!(valid & 0x1)) { /* change only valid bits */
			*key |= (in_key & 0x1) << 7;
			*key_inv |= (in_key_inv & 0x1) << 7;
		} else if (dont_care & 0x1) { /* don't care bit */
			*key |= ICE_DC_KEY << 7;
			*key_inv |= ICE_DC_KEYINV << 7;
		} else if (nvr_mtch & 0x1) { /* never match bit */
			*key |= ICE_NM_KEY << 7;
			*key_inv |= ICE_NM_KEYINV << 7;
		} else if (val & 0x01) { /* exact 1 match */
			*key |= ICE_1_KEY << 7;
			*key_inv |= ICE_1_KEYINV << 7;
		} else { /* exact 0 match */
			*key |= ICE_0_KEY << 7;
			*key_inv |= ICE_0_KEYINV << 7;
		}

		dont_care >>= 1;
		nvr_mtch >>= 1;
		valid >>= 1;
		val >>= 1;
		in_key >>= 1;
		in_key_inv >>= 1;
	}

	return 0;
}

/**
 * ice_bits_max_set - determine if the number of bits set is within a maximum
 * @mask: pointer to the byte array which is the mask
 * @size: the number of bytes in the mask
 * @max: the max number of set bits
 *
 * This function determines if there are at most 'max' number of bits set in an
 * array. Returns true if the number for bits set is <= max or will return false
 * otherwise.
 */
static bool ice_bits_max_set(const u8 *mask, u16 size, u16 max)
{
	u16 count = 0;
	u16 i;

	/* check each byte */
	for (i = 0; i < size; i++) {
		/* if 0, go to next byte */
		if (!mask[i])
			continue;

		/* We know there is at least one set bit in this byte because of
		 * the above check; if we already have found 'max' number of
		 * bits set, then we can return failure now.
		 */
		if (count == max)
			return false;

		/* count the bits in this byte, checking threshold */
		count += hweight8(mask[i]);
		if (count > max)
			return false;
	}

	return true;
}

/**
 * ice_set_key - generate a variable sized key with multiples of 16-bits
 * @key: pointer to where the key will be stored
 * @size: the size of the complete key in bytes (must be even)
 * @val: array of 8-bit values that makes up the value portion of the key
 * @upd: array of 8-bit masks that determine what key portion to update
 * @dc: array of 8-bit masks that make up the don't care mask
 * @nm: array of 8-bit masks that make up the never match mask
 * @off: the offset of the first byte in the key to update
 * @len: the number of bytes in the key update
 *
 * This function generates a key from a value, a don't care mask and a never
 * match mask.
 * upd, dc, and nm are optional parameters, and can be NULL:
 *	upd == NULL --> upd mask is all 1's (update all bits)
 *	dc == NULL --> dc mask is all 0's (no don't care bits)
 *	nm == NULL --> nm mask is all 0's (no never match bits)
 */
static int
ice_set_key(u8 *key, u16 size, u8 *val, u8 *upd, u8 *dc, u8 *nm, u16 off,
	    u16 len)
{
	u16 half_size;
	u16 i;

	/* size must be a multiple of 2 bytes. */
	if (size % 2)
		return -EIO;

	half_size = size / 2;
	if (off + len > half_size)
		return -EIO;

	/* Make sure at most one bit is set in the never match mask. Having more
	 * than one never match mask bit set will cause HW to consume excessive
	 * power otherwise; this is a power management efficiency check.
	 */
#define ICE_NVR_MTCH_BITS_MAX	1
	if (nm && !ice_bits_max_set(nm, len, ICE_NVR_MTCH_BITS_MAX))
		return -EIO;

	for (i = 0; i < len; i++)
		if (ice_gen_key_word(val[i], upd ? upd[i] : 0xff,
				     dc ? dc[i] : 0, nm ? nm[i] : 0,
				     key + off + i, key + half_size + off + i))
			return -EIO;

	return 0;
}

/**
 * ice_acquire_change_lock
 * @hw: pointer to the HW structure
 * @access: access type (read or write)
 *
 * This function will request ownership of the change lock.
 */
int
ice_acquire_change_lock(struct ice_hw *hw, enum ice_aq_res_access_type access)
{
	return ice_acquire_res(hw, ICE_CHANGE_LOCK_RES_ID, access,
			       ICE_CHANGE_LOCK_TIMEOUT);
}

/**
 * ice_release_change_lock
 * @hw: pointer to the HW structure
 *
 * This function will release the change lock using the proper Admin Command.
 */
void ice_release_change_lock(struct ice_hw *hw)
{
	ice_release_res(hw, ICE_CHANGE_LOCK_RES_ID);
}

/**
 * ice_get_open_tunnel_port - retrieve an open tunnel port
 * @hw: pointer to the HW structure
 * @port: returns open port
 * @type: type of tunnel, can be TNL_LAST if it doesn't matter
 */
bool
ice_get_open_tunnel_port(struct ice_hw *hw, u16 *port,
			 enum ice_tunnel_type type)
{
	bool res = false;
	u16 i;

	mutex_lock(&hw->tnl_lock);

	for (i = 0; i < hw->tnl.count && i < ICE_TUNNEL_MAX_ENTRIES; i++)
		if (hw->tnl.tbl[i].valid && hw->tnl.tbl[i].port &&
		    (type == TNL_LAST || type == hw->tnl.tbl[i].type)) {
			*port = hw->tnl.tbl[i].port;
			res = true;
			break;
		}

	mutex_unlock(&hw->tnl_lock);

	return res;
}

/**
 * ice_upd_dvm_boost_entry
 * @hw: pointer to the HW structure
 * @entry: pointer to double vlan boost entry info
 */
static int
ice_upd_dvm_boost_entry(struct ice_hw *hw, struct ice_dvm_entry *entry)
{
	struct ice_boost_tcam_section *sect_rx, *sect_tx;
	int status = -ENOSPC;
	struct ice_buf_build *bld;
	u8 val, dc, nm;

	bld = ice_pkg_buf_alloc(hw);
	if (!bld)
		return -ENOMEM;

	/* allocate 2 sections, one for Rx parser, one for Tx parser */
	if (ice_pkg_buf_reserve_section(bld, 2))
		goto ice_upd_dvm_boost_entry_err;

	sect_rx = ice_pkg_buf_alloc_section(bld, ICE_SID_RXPARSER_BOOST_TCAM,
					    struct_size(sect_rx, tcam, 1));
	if (!sect_rx)
		goto ice_upd_dvm_boost_entry_err;
	sect_rx->count = cpu_to_le16(1);

	sect_tx = ice_pkg_buf_alloc_section(bld, ICE_SID_TXPARSER_BOOST_TCAM,
					    struct_size(sect_tx, tcam, 1));
	if (!sect_tx)
		goto ice_upd_dvm_boost_entry_err;
	sect_tx->count = cpu_to_le16(1);

	/* copy original boost entry to update package buffer */
	memcpy(sect_rx->tcam, entry->boost_entry, sizeof(*sect_rx->tcam));

	/* re-write the don't care and never match bits accordingly */
	if (entry->enable) {
		/* all bits are don't care */
		val = 0x00;
		dc = 0xFF;
		nm = 0x00;
	} else {
		/* disable, one never match bit, the rest are don't care */
		val = 0x00;
		dc = 0xF7;
		nm = 0x08;
	}

	ice_set_key((u8 *)&sect_rx->tcam[0].key, sizeof(sect_rx->tcam[0].key),
		    &val, NULL, &dc, &nm, 0, sizeof(u8));

	/* exact copy of entry to Tx section entry */
	memcpy(sect_tx->tcam, sect_rx->tcam, sizeof(*sect_tx->tcam));

	status = ice_update_pkg_no_lock(hw, ice_pkg_buf(bld), 1);

ice_upd_dvm_boost_entry_err:
	ice_pkg_buf_free(hw, bld);

	return status;
}

/**
 * ice_set_dvm_boost_entries
 * @hw: pointer to the HW structure
 *
 * Enable double vlan by updating the appropriate boost tcam entries.
 */
int ice_set_dvm_boost_entries(struct ice_hw *hw)
{
	u16 i;

	for (i = 0; i < hw->dvm_upd.count; i++) {
		int status;

		status = ice_upd_dvm_boost_entry(hw, &hw->dvm_upd.tbl[i]);
		if (status)
			return status;
	}

	return 0;
}

/**
 * ice_tunnel_idx_to_entry - convert linear index to the sparse one
 * @hw: pointer to the HW structure
 * @type: type of tunnel
 * @idx: linear index
 *
 * Stack assumes we have 2 linear tables with indexes [0, count_valid),
 * but really the port table may be sprase, and types are mixed, so convert
 * the stack index into the device index.
 */
static u16 ice_tunnel_idx_to_entry(struct ice_hw *hw, enum ice_tunnel_type type,
				   u16 idx)
{
	u16 i;

	for (i = 0; i < hw->tnl.count && i < ICE_TUNNEL_MAX_ENTRIES; i++)
		if (hw->tnl.tbl[i].valid &&
		    hw->tnl.tbl[i].type == type &&
		    idx-- == 0)
			return i;

	WARN_ON_ONCE(1);
	return 0;
}

/**
 * ice_create_tunnel
 * @hw: pointer to the HW structure
 * @index: device table entry
 * @type: type of tunnel
 * @port: port of tunnel to create
 *
 * Create a tunnel by updating the parse graph in the parser. We do that by
 * creating a package buffer with the tunnel info and issuing an update package
 * command.
 */
static int
ice_create_tunnel(struct ice_hw *hw, u16 index,
		  enum ice_tunnel_type type, u16 port)
{
	struct ice_boost_tcam_section *sect_rx, *sect_tx;
	struct ice_buf_build *bld;
	int status = -ENOSPC;

	mutex_lock(&hw->tnl_lock);

	bld = ice_pkg_buf_alloc(hw);
	if (!bld) {
		status = -ENOMEM;
		goto ice_create_tunnel_end;
	}

	/* allocate 2 sections, one for Rx parser, one for Tx parser */
	if (ice_pkg_buf_reserve_section(bld, 2))
		goto ice_create_tunnel_err;

	sect_rx = ice_pkg_buf_alloc_section(bld, ICE_SID_RXPARSER_BOOST_TCAM,
					    struct_size(sect_rx, tcam, 1));
	if (!sect_rx)
		goto ice_create_tunnel_err;
	sect_rx->count = cpu_to_le16(1);

	sect_tx = ice_pkg_buf_alloc_section(bld, ICE_SID_TXPARSER_BOOST_TCAM,
					    struct_size(sect_tx, tcam, 1));
	if (!sect_tx)
		goto ice_create_tunnel_err;
	sect_tx->count = cpu_to_le16(1);

	/* copy original boost entry to update package buffer */
	memcpy(sect_rx->tcam, hw->tnl.tbl[index].boost_entry,
	       sizeof(*sect_rx->tcam));

	/* over-write the never-match dest port key bits with the encoded port
	 * bits
	 */
	ice_set_key((u8 *)&sect_rx->tcam[0].key, sizeof(sect_rx->tcam[0].key),
		    (u8 *)&port, NULL, NULL, NULL,
		    (u16)offsetof(struct ice_boost_key_value, hv_dst_port_key),
		    sizeof(sect_rx->tcam[0].key.key.hv_dst_port_key));

	/* exact copy of entry to Tx section entry */
	memcpy(sect_tx->tcam, sect_rx->tcam, sizeof(*sect_tx->tcam));

	status = ice_update_pkg(hw, ice_pkg_buf(bld), 1);
	if (!status)
		hw->tnl.tbl[index].port = port;

ice_create_tunnel_err:
	ice_pkg_buf_free(hw, bld);

ice_create_tunnel_end:
	mutex_unlock(&hw->tnl_lock);

	return status;
}

/**
 * ice_destroy_tunnel
 * @hw: pointer to the HW structure
 * @index: device table entry
 * @type: type of tunnel
 * @port: port of tunnel to destroy (ignored if the all parameter is true)
 *
 * Destroys a tunnel or all tunnels by creating an update package buffer
 * targeting the specific updates requested and then performing an update
 * package.
 */
static int
ice_destroy_tunnel(struct ice_hw *hw, u16 index, enum ice_tunnel_type type,
		   u16 port)
{
	struct ice_boost_tcam_section *sect_rx, *sect_tx;
	struct ice_buf_build *bld;
	int status = -ENOSPC;

	mutex_lock(&hw->tnl_lock);

	if (WARN_ON(!hw->tnl.tbl[index].valid ||
		    hw->tnl.tbl[index].type != type ||
		    hw->tnl.tbl[index].port != port)) {
		status = -EIO;
		goto ice_destroy_tunnel_end;
	}

	bld = ice_pkg_buf_alloc(hw);
	if (!bld) {
		status = -ENOMEM;
		goto ice_destroy_tunnel_end;
	}

	/* allocate 2 sections, one for Rx parser, one for Tx parser */
	if (ice_pkg_buf_reserve_section(bld, 2))
		goto ice_destroy_tunnel_err;

	sect_rx = ice_pkg_buf_alloc_section(bld, ICE_SID_RXPARSER_BOOST_TCAM,
					    struct_size(sect_rx, tcam, 1));
	if (!sect_rx)
		goto ice_destroy_tunnel_err;
	sect_rx->count = cpu_to_le16(1);

	sect_tx = ice_pkg_buf_alloc_section(bld, ICE_SID_TXPARSER_BOOST_TCAM,
					    struct_size(sect_tx, tcam, 1));
	if (!sect_tx)
		goto ice_destroy_tunnel_err;
	sect_tx->count = cpu_to_le16(1);

	/* copy original boost entry to update package buffer, one copy to Rx
	 * section, another copy to the Tx section
	 */
	memcpy(sect_rx->tcam, hw->tnl.tbl[index].boost_entry,
	       sizeof(*sect_rx->tcam));
	memcpy(sect_tx->tcam, hw->tnl.tbl[index].boost_entry,
	       sizeof(*sect_tx->tcam));

	status = ice_update_pkg(hw, ice_pkg_buf(bld), 1);
	if (!status)
		hw->tnl.tbl[index].port = 0;

ice_destroy_tunnel_err:
	ice_pkg_buf_free(hw, bld);

ice_destroy_tunnel_end:
	mutex_unlock(&hw->tnl_lock);

	return status;
}

int ice_udp_tunnel_set_port(struct net_device *netdev, unsigned int table,
			    unsigned int idx, struct udp_tunnel_info *ti)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_vsi *vsi = np->vsi;
	struct ice_pf *pf = vsi->back;
	enum ice_tunnel_type tnl_type;
	int status;
	u16 index;

	tnl_type = ti->type == UDP_TUNNEL_TYPE_VXLAN ? TNL_VXLAN : TNL_GENEVE;
	index = ice_tunnel_idx_to_entry(&pf->hw, tnl_type, idx);

	status = ice_create_tunnel(&pf->hw, index, tnl_type, ntohs(ti->port));
	if (status) {
		netdev_err(netdev, "Error adding UDP tunnel - %d\n",
			   status);
		return -EIO;
	}

	udp_tunnel_nic_set_port_priv(netdev, table, idx, index);
	return 0;
}

int ice_udp_tunnel_unset_port(struct net_device *netdev, unsigned int table,
			      unsigned int idx, struct udp_tunnel_info *ti)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_vsi *vsi = np->vsi;
	struct ice_pf *pf = vsi->back;
	enum ice_tunnel_type tnl_type;
	int status;

	tnl_type = ti->type == UDP_TUNNEL_TYPE_VXLAN ? TNL_VXLAN : TNL_GENEVE;

	status = ice_destroy_tunnel(&pf->hw, ti->hw_priv, tnl_type,
				    ntohs(ti->port));
	if (status) {
		netdev_err(netdev, "Error removing UDP tunnel - %d\n",
			   status);
		return -EIO;
	}

	return 0;
}

/**
 * ice_find_prot_off - find prot ID and offset pair, based on prof and FV index
 * @hw: pointer to the hardware structure
 * @blk: hardware block
 * @prof: profile ID
 * @fv_idx: field vector word index
 * @prot: variable to receive the protocol ID
 * @off: variable to receive the protocol offset
 */
int
ice_find_prot_off(struct ice_hw *hw, enum ice_block blk, u8 prof, u16 fv_idx,
		  u8 *prot, u16 *off)
{
	struct ice_fv_word *fv_ext;

	if (prof >= hw->blk[blk].es.count)
		return -EINVAL;

	if (fv_idx >= hw->blk[blk].es.fvw)
		return -EINVAL;

	fv_ext = hw->blk[blk].es.t + (prof * hw->blk[blk].es.fvw);

	*prot = fv_ext[fv_idx].prot_id;
	*off = fv_ext[fv_idx].off;

	return 0;
}

/* PTG Management */

/**
 * ice_ptg_find_ptype - Search for packet type group using packet type (ptype)
 * @hw: pointer to the hardware structure
 * @blk: HW block
 * @ptype: the ptype to search for
 * @ptg: pointer to variable that receives the PTG
 *
 * This function will search the PTGs for a particular ptype, returning the
 * PTG ID that contains it through the PTG parameter, with the value of
 * ICE_DEFAULT_PTG (0) meaning it is part the default PTG.
 */
static int
ice_ptg_find_ptype(struct ice_hw *hw, enum ice_block blk, u16 ptype, u8 *ptg)
{
	if (ptype >= ICE_XLT1_CNT || !ptg)
		return -EINVAL;

	*ptg = hw->blk[blk].xlt1.ptypes[ptype].ptg;
	return 0;
}

/**
 * ice_ptg_alloc_val - Allocates a new packet type group ID by value
 * @hw: pointer to the hardware structure
 * @blk: HW block
 * @ptg: the PTG to allocate
 *
 * This function allocates a given packet type group ID specified by the PTG
 * parameter.
 */
static void ice_ptg_alloc_val(struct ice_hw *hw, enum ice_block blk, u8 ptg)
{
	hw->blk[blk].xlt1.ptg_tbl[ptg].in_use = true;
}

/**
 * ice_ptg_remove_ptype - Removes ptype from a particular packet type group
 * @hw: pointer to the hardware structure
 * @blk: HW block
 * @ptype: the ptype to remove
 * @ptg: the PTG to remove the ptype from
 *
 * This function will remove the ptype from the specific PTG, and move it to
 * the default PTG (ICE_DEFAULT_PTG).
 */
static int
ice_ptg_remove_ptype(struct ice_hw *hw, enum ice_block blk, u16 ptype, u8 ptg)
{
	struct ice_ptg_ptype **ch;
	struct ice_ptg_ptype *p;

	if (ptype > ICE_XLT1_CNT - 1)
		return -EINVAL;

	if (!hw->blk[blk].xlt1.ptg_tbl[ptg].in_use)
		return -ENOENT;

	/* Should not happen if .in_use is set, bad config */
	if (!hw->blk[blk].xlt1.ptg_tbl[ptg].first_ptype)
		return -EIO;

	/* find the ptype within this PTG, and bypass the link over it */
	p = hw->blk[blk].xlt1.ptg_tbl[ptg].first_ptype;
	ch = &hw->blk[blk].xlt1.ptg_tbl[ptg].first_ptype;
	while (p) {
		if (ptype == (p - hw->blk[blk].xlt1.ptypes)) {
			*ch = p->next_ptype;
			break;
		}

		ch = &p->next_ptype;
		p = p->next_ptype;
	}

	hw->blk[blk].xlt1.ptypes[ptype].ptg = ICE_DEFAULT_PTG;
	hw->blk[blk].xlt1.ptypes[ptype].next_ptype = NULL;

	return 0;
}

/**
 * ice_ptg_add_mv_ptype - Adds/moves ptype to a particular packet type group
 * @hw: pointer to the hardware structure
 * @blk: HW block
 * @ptype: the ptype to add or move
 * @ptg: the PTG to add or move the ptype to
 *
 * This function will either add or move a ptype to a particular PTG depending
 * on if the ptype is already part of another group. Note that using a
 * destination PTG ID of ICE_DEFAULT_PTG (0) will move the ptype to the
 * default PTG.
 */
static int
ice_ptg_add_mv_ptype(struct ice_hw *hw, enum ice_block blk, u16 ptype, u8 ptg)
{
	u8 original_ptg;
	int status;

	if (ptype > ICE_XLT1_CNT - 1)
		return -EINVAL;

	if (!hw->blk[blk].xlt1.ptg_tbl[ptg].in_use && ptg != ICE_DEFAULT_PTG)
		return -ENOENT;

	status = ice_ptg_find_ptype(hw, blk, ptype, &original_ptg);
	if (status)
		return status;

	/* Is ptype already in the correct PTG? */
	if (original_ptg == ptg)
		return 0;

	/* Remove from original PTG and move back to the default PTG */
	if (original_ptg != ICE_DEFAULT_PTG)
		ice_ptg_remove_ptype(hw, blk, ptype, original_ptg);

	/* Moving to default PTG? Then we're done with this request */
	if (ptg == ICE_DEFAULT_PTG)
		return 0;

	/* Add ptype to PTG at beginning of list */
	hw->blk[blk].xlt1.ptypes[ptype].next_ptype =
		hw->blk[blk].xlt1.ptg_tbl[ptg].first_ptype;
	hw->blk[blk].xlt1.ptg_tbl[ptg].first_ptype =
		&hw->blk[blk].xlt1.ptypes[ptype];

	hw->blk[blk].xlt1.ptypes[ptype].ptg = ptg;
	hw->blk[blk].xlt1.t[ptype] = ptg;

	return 0;
}

/* Block / table size info */
struct ice_blk_size_details {
	u16 xlt1;			/* # XLT1 entries */
	u16 xlt2;			/* # XLT2 entries */
	u16 prof_tcam;			/* # profile ID TCAM entries */
	u16 prof_id;			/* # profile IDs */
	u8 prof_cdid_bits;		/* # CDID one-hot bits used in key */
	u16 prof_redir;			/* # profile redirection entries */
	u16 es;				/* # extraction sequence entries */
	u16 fvw;			/* # field vector words */
	u8 overwrite;			/* overwrite existing entries allowed */
	u8 reverse;			/* reverse FV order */
};

static const struct ice_blk_size_details blk_sizes[ICE_BLK_COUNT] = {
	/**
	 * Table Definitions
	 * XLT1 - Number of entries in XLT1 table
	 * XLT2 - Number of entries in XLT2 table
	 * TCAM - Number of entries Profile ID TCAM table
	 * CDID - Control Domain ID of the hardware block
	 * PRED - Number of entries in the Profile Redirection Table
	 * FV   - Number of entries in the Field Vector
	 * FVW  - Width (in WORDs) of the Field Vector
	 * OVR  - Overwrite existing table entries
	 * REV  - Reverse FV
	 */
	/*          XLT1        , XLT2        ,TCAM, PID,CDID,PRED,   FV, FVW */
	/*          Overwrite   , Reverse FV */
	/* SW  */ { ICE_XLT1_CNT, ICE_XLT2_CNT, 512, 256,   0,  256, 256,  48,
		    false, false },
	/* ACL */ { ICE_XLT1_CNT, ICE_XLT2_CNT, 512, 128,   0,  128, 128,  32,
		    false, false },
	/* FD  */ { ICE_XLT1_CNT, ICE_XLT2_CNT, 512, 128,   0,  128, 128,  24,
		    false, true  },
	/* RSS */ { ICE_XLT1_CNT, ICE_XLT2_CNT, 512, 128,   0,  128, 128,  24,
		    true,  true  },
	/* PE  */ { ICE_XLT1_CNT, ICE_XLT2_CNT,  64,  32,   0,   32,  32,  24,
		    false, false },
};

enum ice_sid_all {
	ICE_SID_XLT1_OFF = 0,
	ICE_SID_XLT2_OFF,
	ICE_SID_PR_OFF,
	ICE_SID_PR_REDIR_OFF,
	ICE_SID_ES_OFF,
	ICE_SID_OFF_COUNT,
};

/* Characteristic handling */

/**
 * ice_match_prop_lst - determine if properties of two lists match
 * @list1: first properties list
 * @list2: second properties list
 *
 * Count, cookies and the order must match in order to be considered equivalent.
 */
static bool
ice_match_prop_lst(struct list_head *list1, struct list_head *list2)
{
	struct ice_vsig_prof *tmp1;
	struct ice_vsig_prof *tmp2;
	u16 chk_count = 0;
	u16 count = 0;

	/* compare counts */
	list_for_each_entry(tmp1, list1, list)
		count++;
	list_for_each_entry(tmp2, list2, list)
		chk_count++;
	if (!count || count != chk_count)
		return false;

	tmp1 = list_first_entry(list1, struct ice_vsig_prof, list);
	tmp2 = list_first_entry(list2, struct ice_vsig_prof, list);

	/* profile cookies must compare, and in the exact same order to take
	 * into account priority
	 */
	while (count--) {
		if (tmp2->profile_cookie != tmp1->profile_cookie)
			return false;

		tmp1 = list_next_entry(tmp1, list);
		tmp2 = list_next_entry(tmp2, list);
	}

	return true;
}

/* VSIG Management */

/**
 * ice_vsig_find_vsi - find a VSIG that contains a specified VSI
 * @hw: pointer to the hardware structure
 * @blk: HW block
 * @vsi: VSI of interest
 * @vsig: pointer to receive the VSI group
 *
 * This function will lookup the VSI entry in the XLT2 list and return
 * the VSI group its associated with.
 */
static int
ice_vsig_find_vsi(struct ice_hw *hw, enum ice_block blk, u16 vsi, u16 *vsig)
{
	if (!vsig || vsi >= ICE_MAX_VSI)
		return -EINVAL;

	/* As long as there's a default or valid VSIG associated with the input
	 * VSI, the functions returns a success. Any handling of VSIG will be
	 * done by the following add, update or remove functions.
	 */
	*vsig = hw->blk[blk].xlt2.vsis[vsi].vsig;

	return 0;
}

/**
 * ice_vsig_alloc_val - allocate a new VSIG by value
 * @hw: pointer to the hardware structure
 * @blk: HW block
 * @vsig: the VSIG to allocate
 *
 * This function will allocate a given VSIG specified by the VSIG parameter.
 */
static u16 ice_vsig_alloc_val(struct ice_hw *hw, enum ice_block blk, u16 vsig)
{
	u16 idx = vsig & ICE_VSIG_IDX_M;

	if (!hw->blk[blk].xlt2.vsig_tbl[idx].in_use) {
		INIT_LIST_HEAD(&hw->blk[blk].xlt2.vsig_tbl[idx].prop_lst);
		hw->blk[blk].xlt2.vsig_tbl[idx].in_use = true;
	}

	return ICE_VSIG_VALUE(idx, hw->pf_id);
}

/**
 * ice_vsig_alloc - Finds a free entry and allocates a new VSIG
 * @hw: pointer to the hardware structure
 * @blk: HW block
 *
 * This function will iterate through the VSIG list and mark the first
 * unused entry for the new VSIG entry as used and return that value.
 */
static u16 ice_vsig_alloc(struct ice_hw *hw, enum ice_block blk)
{
	u16 i;

	for (i = 1; i < ICE_MAX_VSIGS; i++)
		if (!hw->blk[blk].xlt2.vsig_tbl[i].in_use)
			return ice_vsig_alloc_val(hw, blk, i);

	return ICE_DEFAULT_VSIG;
}

/**
 * ice_find_dup_props_vsig - find VSI group with a specified set of properties
 * @hw: pointer to the hardware structure
 * @blk: HW block
 * @chs: characteristic list
 * @vsig: returns the VSIG with the matching profiles, if found
 *
 * Each VSIG is associated with a characteristic set; i.e. all VSIs under
 * a group have the same characteristic set. To check if there exists a VSIG
 * which has the same characteristics as the input characteristics; this
 * function will iterate through the XLT2 list and return the VSIG that has a
 * matching configuration. In order to make sure that priorities are accounted
 * for, the list must match exactly, including the order in which the
 * characteristics are listed.
 */
static int
ice_find_dup_props_vsig(struct ice_hw *hw, enum ice_block blk,
			struct list_head *chs, u16 *vsig)
{
	struct ice_xlt2 *xlt2 = &hw->blk[blk].xlt2;
	u16 i;

	for (i = 0; i < xlt2->count; i++)
		if (xlt2->vsig_tbl[i].in_use &&
		    ice_match_prop_lst(chs, &xlt2->vsig_tbl[i].prop_lst)) {
			*vsig = ICE_VSIG_VALUE(i, hw->pf_id);
			return 0;
		}

	return -ENOENT;
}

/**
 * ice_vsig_free - free VSI group
 * @hw: pointer to the hardware structure
 * @blk: HW block
 * @vsig: VSIG to remove
 *
 * The function will remove all VSIs associated with the input VSIG and move
 * them to the DEFAULT_VSIG and mark the VSIG available.
 */
static int ice_vsig_free(struct ice_hw *hw, enum ice_block blk, u16 vsig)
{
	struct ice_vsig_prof *dtmp, *del;
	struct ice_vsig_vsi *vsi_cur;
	u16 idx;

	idx = vsig & ICE_VSIG_IDX_M;
	if (idx >= ICE_MAX_VSIGS)
		return -EINVAL;

	if (!hw->blk[blk].xlt2.vsig_tbl[idx].in_use)
		return -ENOENT;

	hw->blk[blk].xlt2.vsig_tbl[idx].in_use = false;

	vsi_cur = hw->blk[blk].xlt2.vsig_tbl[idx].first_vsi;
	/* If the VSIG has at least 1 VSI then iterate through the
	 * list and remove the VSIs before deleting the group.
	 */
	if (vsi_cur) {
		/* remove all vsis associated with this VSIG XLT2 entry */
		do {
			struct ice_vsig_vsi *tmp = vsi_cur->next_vsi;

			vsi_cur->vsig = ICE_DEFAULT_VSIG;
			vsi_cur->changed = 1;
			vsi_cur->next_vsi = NULL;
			vsi_cur = tmp;
		} while (vsi_cur);

		/* NULL terminate head of VSI list */
		hw->blk[blk].xlt2.vsig_tbl[idx].first_vsi = NULL;
	}

	/* free characteristic list */
	list_for_each_entry_safe(del, dtmp,
				 &hw->blk[blk].xlt2.vsig_tbl[idx].prop_lst,
				 list) {
		list_del(&del->list);
		devm_kfree(ice_hw_to_dev(hw), del);
	}

	/* if VSIG characteristic list was cleared for reset
	 * re-initialize the list head
	 */
	INIT_LIST_HEAD(&hw->blk[blk].xlt2.vsig_tbl[idx].prop_lst);

	return 0;
}

/**
 * ice_vsig_remove_vsi - remove VSI from VSIG
 * @hw: pointer to the hardware structure
 * @blk: HW block
 * @vsi: VSI to remove
 * @vsig: VSI group to remove from
 *
 * The function will remove the input VSI from its VSI group and move it
 * to the DEFAULT_VSIG.
 */
static int
ice_vsig_remove_vsi(struct ice_hw *hw, enum ice_block blk, u16 vsi, u16 vsig)
{
	struct ice_vsig_vsi **vsi_head, *vsi_cur, *vsi_tgt;
	u16 idx;

	idx = vsig & ICE_VSIG_IDX_M;

	if (vsi >= ICE_MAX_VSI || idx >= ICE_MAX_VSIGS)
		return -EINVAL;

	if (!hw->blk[blk].xlt2.vsig_tbl[idx].in_use)
		return -ENOENT;

	/* entry already in default VSIG, don't have to remove */
	if (idx == ICE_DEFAULT_VSIG)
		return 0;

	vsi_head = &hw->blk[blk].xlt2.vsig_tbl[idx].first_vsi;
	if (!(*vsi_head))
		return -EIO;

	vsi_tgt = &hw->blk[blk].xlt2.vsis[vsi];
	vsi_cur = (*vsi_head);

	/* iterate the VSI list, skip over the entry to be removed */
	while (vsi_cur) {
		if (vsi_tgt == vsi_cur) {
			(*vsi_head) = vsi_cur->next_vsi;
			break;
		}
		vsi_head = &vsi_cur->next_vsi;
		vsi_cur = vsi_cur->next_vsi;
	}

	/* verify if VSI was removed from group list */
	if (!vsi_cur)
		return -ENOENT;

	vsi_cur->vsig = ICE_DEFAULT_VSIG;
	vsi_cur->changed = 1;
	vsi_cur->next_vsi = NULL;

	return 0;
}

/**
 * ice_vsig_add_mv_vsi - add or move a VSI to a VSI group
 * @hw: pointer to the hardware structure
 * @blk: HW block
 * @vsi: VSI to move
 * @vsig: destination VSI group
 *
 * This function will move or add the input VSI to the target VSIG.
 * The function will find the original VSIG the VSI belongs to and
 * move the entry to the DEFAULT_VSIG, update the original VSIG and
 * then move entry to the new VSIG.
 */
static int
ice_vsig_add_mv_vsi(struct ice_hw *hw, enum ice_block blk, u16 vsi, u16 vsig)
{
	struct ice_vsig_vsi *tmp;
	u16 orig_vsig, idx;
	int status;

	idx = vsig & ICE_VSIG_IDX_M;

	if (vsi >= ICE_MAX_VSI || idx >= ICE_MAX_VSIGS)
		return -EINVAL;

	/* if VSIG not in use and VSIG is not default type this VSIG
	 * doesn't exist.
	 */
	if (!hw->blk[blk].xlt2.vsig_tbl[idx].in_use &&
	    vsig != ICE_DEFAULT_VSIG)
		return -ENOENT;

	status = ice_vsig_find_vsi(hw, blk, vsi, &orig_vsig);
	if (status)
		return status;

	/* no update required if vsigs match */
	if (orig_vsig == vsig)
		return 0;

	if (orig_vsig != ICE_DEFAULT_VSIG) {
		/* remove entry from orig_vsig and add to default VSIG */
		status = ice_vsig_remove_vsi(hw, blk, vsi, orig_vsig);
		if (status)
			return status;
	}

	if (idx == ICE_DEFAULT_VSIG)
		return 0;

	/* Create VSI entry and add VSIG and prop_mask values */
	hw->blk[blk].xlt2.vsis[vsi].vsig = vsig;
	hw->blk[blk].xlt2.vsis[vsi].changed = 1;

	/* Add new entry to the head of the VSIG list */
	tmp = hw->blk[blk].xlt2.vsig_tbl[idx].first_vsi;
	hw->blk[blk].xlt2.vsig_tbl[idx].first_vsi =
		&hw->blk[blk].xlt2.vsis[vsi];
	hw->blk[blk].xlt2.vsis[vsi].next_vsi = tmp;
	hw->blk[blk].xlt2.t[vsi] = vsig;

	return 0;
}

/**
 * ice_prof_has_mask_idx - determine if profile index masking is identical
 * @hw: pointer to the hardware structure
 * @blk: HW block
 * @prof: profile to check
 * @idx: profile index to check
 * @mask: mask to match
 */
static bool
ice_prof_has_mask_idx(struct ice_hw *hw, enum ice_block blk, u8 prof, u16 idx,
		      u16 mask)
{
	bool expect_no_mask = false;
	bool found = false;
	bool match = false;
	u16 i;

	/* If mask is 0x0000 or 0xffff, then there is no masking */
	if (mask == 0 || mask == 0xffff)
		expect_no_mask = true;

	/* Scan the enabled masks on this profile, for the specified idx */
	for (i = hw->blk[blk].masks.first; i < hw->blk[blk].masks.first +
	     hw->blk[blk].masks.count; i++)
		if (hw->blk[blk].es.mask_ena[prof] & BIT(i))
			if (hw->blk[blk].masks.masks[i].in_use &&
			    hw->blk[blk].masks.masks[i].idx == idx) {
				found = true;
				if (hw->blk[blk].masks.masks[i].mask == mask)
					match = true;
				break;
			}

	if (expect_no_mask) {
		if (found)
			return false;
	} else {
		if (!match)
			return false;
	}

	return true;
}

/**
 * ice_prof_has_mask - determine if profile masking is identical
 * @hw: pointer to the hardware structure
 * @blk: HW block
 * @prof: profile to check
 * @masks: masks to match
 */
static bool
ice_prof_has_mask(struct ice_hw *hw, enum ice_block blk, u8 prof, u16 *masks)
{
	u16 i;

	/* es->mask_ena[prof] will have the mask */
	for (i = 0; i < hw->blk[blk].es.fvw; i++)
		if (!ice_prof_has_mask_idx(hw, blk, prof, i, masks[i]))
			return false;

	return true;
}

/**
 * ice_find_prof_id_with_mask - find profile ID for a given field vector
 * @hw: pointer to the hardware structure
 * @blk: HW block
 * @fv: field vector to search for
 * @masks: masks for FV
 * @symm: symmetric setting for RSS flows
 * @prof_id: receives the profile ID
 */
static int
ice_find_prof_id_with_mask(struct ice_hw *hw, enum ice_block blk,
			   struct ice_fv_word *fv, u16 *masks, bool symm,
			   u8 *prof_id)
{
	struct ice_es *es = &hw->blk[blk].es;
	u8 i;

	/* For FD, we don't want to re-use a existed profile with the same
	 * field vector and mask. This will cause rule interference.
	 */
	if (blk == ICE_BLK_FD)
		return -ENOENT;

	for (i = 0; i < (u8)es->count; i++) {
		u16 off = i * es->fvw;

		if (blk == ICE_BLK_RSS && es->symm[i] != symm)
			continue;

		if (memcmp(&es->t[off], fv, es->fvw * sizeof(*fv)))
			continue;

		/* check if masks settings are the same for this profile */
		if (masks && !ice_prof_has_mask(hw, blk, i, masks))
			continue;

		*prof_id = i;
		return 0;
	}

	return -ENOENT;
}

/**
 * ice_prof_id_rsrc_type - get profile ID resource type for a block type
 * @blk: the block type
 * @rsrc_type: pointer to variable to receive the resource type
 */
static bool ice_prof_id_rsrc_type(enum ice_block blk, u16 *rsrc_type)
{
	switch (blk) {
	case ICE_BLK_FD:
		*rsrc_type = ICE_AQC_RES_TYPE_FD_PROF_BLDR_PROFID;
		break;
	case ICE_BLK_RSS:
		*rsrc_type = ICE_AQC_RES_TYPE_HASH_PROF_BLDR_PROFID;
		break;
	default:
		return false;
	}
	return true;
}

/**
 * ice_tcam_ent_rsrc_type - get TCAM entry resource type for a block type
 * @blk: the block type
 * @rsrc_type: pointer to variable to receive the resource type
 */
static bool ice_tcam_ent_rsrc_type(enum ice_block blk, u16 *rsrc_type)
{
	switch (blk) {
	case ICE_BLK_FD:
		*rsrc_type = ICE_AQC_RES_TYPE_FD_PROF_BLDR_TCAM;
		break;
	case ICE_BLK_RSS:
		*rsrc_type = ICE_AQC_RES_TYPE_HASH_PROF_BLDR_TCAM;
		break;
	default:
		return false;
	}
	return true;
}

/**
 * ice_alloc_tcam_ent - allocate hardware TCAM entry
 * @hw: pointer to the HW struct
 * @blk: the block to allocate the TCAM for
 * @btm: true to allocate from bottom of table, false to allocate from top
 * @tcam_idx: pointer to variable to receive the TCAM entry
 *
 * This function allocates a new entry in a Profile ID TCAM for a specific
 * block.
 */
static int
ice_alloc_tcam_ent(struct ice_hw *hw, enum ice_block blk, bool btm,
		   u16 *tcam_idx)
{
	u16 res_type;

	if (!ice_tcam_ent_rsrc_type(blk, &res_type))
		return -EINVAL;

	return ice_alloc_hw_res(hw, res_type, 1, btm, tcam_idx);
}

/**
 * ice_free_tcam_ent - free hardware TCAM entry
 * @hw: pointer to the HW struct
 * @blk: the block from which to free the TCAM entry
 * @tcam_idx: the TCAM entry to free
 *
 * This function frees an entry in a Profile ID TCAM for a specific block.
 */
static int
ice_free_tcam_ent(struct ice_hw *hw, enum ice_block blk, u16 tcam_idx)
{
	u16 res_type;

	if (!ice_tcam_ent_rsrc_type(blk, &res_type))
		return -EINVAL;

	return ice_free_hw_res(hw, res_type, 1, &tcam_idx);
}

/**
 * ice_alloc_prof_id - allocate profile ID
 * @hw: pointer to the HW struct
 * @blk: the block to allocate the profile ID for
 * @prof_id: pointer to variable to receive the profile ID
 *
 * This function allocates a new profile ID, which also corresponds to a Field
 * Vector (Extraction Sequence) entry.
 */
static int ice_alloc_prof_id(struct ice_hw *hw, enum ice_block blk, u8 *prof_id)
{
	u16 res_type;
	u16 get_prof;
	int status;

	if (!ice_prof_id_rsrc_type(blk, &res_type))
		return -EINVAL;

	status = ice_alloc_hw_res(hw, res_type, 1, false, &get_prof);
	if (!status)
		*prof_id = (u8)get_prof;

	return status;
}

/**
 * ice_free_prof_id - free profile ID
 * @hw: pointer to the HW struct
 * @blk: the block from which to free the profile ID
 * @prof_id: the profile ID to free
 *
 * This function frees a profile ID, which also corresponds to a Field Vector.
 */
static int ice_free_prof_id(struct ice_hw *hw, enum ice_block blk, u8 prof_id)
{
	u16 tmp_prof_id = (u16)prof_id;
	u16 res_type;

	if (!ice_prof_id_rsrc_type(blk, &res_type))
		return -EINVAL;

	return ice_free_hw_res(hw, res_type, 1, &tmp_prof_id);
}

/**
 * ice_prof_inc_ref - increment reference count for profile
 * @hw: pointer to the HW struct
 * @blk: the block from which to free the profile ID
 * @prof_id: the profile ID for which to increment the reference count
 */
static int ice_prof_inc_ref(struct ice_hw *hw, enum ice_block blk, u8 prof_id)
{
	if (prof_id > hw->blk[blk].es.count)
		return -EINVAL;

	hw->blk[blk].es.ref_count[prof_id]++;

	return 0;
}

/**
 * ice_write_prof_mask_reg - write profile mask register
 * @hw: pointer to the HW struct
 * @blk: hardware block
 * @mask_idx: mask index
 * @idx: index of the FV which will use the mask
 * @mask: the 16-bit mask
 */
static void
ice_write_prof_mask_reg(struct ice_hw *hw, enum ice_block blk, u16 mask_idx,
			u16 idx, u16 mask)
{
	u32 offset;
	u32 val;

	switch (blk) {
	case ICE_BLK_RSS:
		offset = GLQF_HMASK(mask_idx);
		val = FIELD_PREP(GLQF_HMASK_MSK_INDEX_M, idx);
		val |= FIELD_PREP(GLQF_HMASK_MASK_M, mask);
		break;
	case ICE_BLK_FD:
		offset = GLQF_FDMASK(mask_idx);
		val = FIELD_PREP(GLQF_FDMASK_MSK_INDEX_M, idx);
		val |= FIELD_PREP(GLQF_FDMASK_MASK_M, mask);
		break;
	default:
		ice_debug(hw, ICE_DBG_PKG, "No profile masks for block %d\n",
			  blk);
		return;
	}

	wr32(hw, offset, val);
	ice_debug(hw, ICE_DBG_PKG, "write mask, blk %d (%d): %x = %x\n",
		  blk, idx, offset, val);
}

/**
 * ice_write_prof_mask_enable_res - write profile mask enable register
 * @hw: pointer to the HW struct
 * @blk: hardware block
 * @prof_id: profile ID
 * @enable_mask: enable mask
 */
static void
ice_write_prof_mask_enable_res(struct ice_hw *hw, enum ice_block blk,
			       u16 prof_id, u32 enable_mask)
{
	u32 offset;

	switch (blk) {
	case ICE_BLK_RSS:
		offset = GLQF_HMASK_SEL(prof_id);
		break;
	case ICE_BLK_FD:
		offset = GLQF_FDMASK_SEL(prof_id);
		break;
	default:
		ice_debug(hw, ICE_DBG_PKG, "No profile masks for block %d\n",
			  blk);
		return;
	}

	wr32(hw, offset, enable_mask);
	ice_debug(hw, ICE_DBG_PKG, "write mask enable, blk %d (%d): %x = %x\n",
		  blk, prof_id, offset, enable_mask);
}

/**
 * ice_init_prof_masks - initial prof masks
 * @hw: pointer to the HW struct
 * @blk: hardware block
 */
static void ice_init_prof_masks(struct ice_hw *hw, enum ice_block blk)
{
	u16 per_pf;
	u16 i;

	mutex_init(&hw->blk[blk].masks.lock);

	per_pf = ICE_PROF_MASK_COUNT / hw->dev_caps.num_funcs;

	hw->blk[blk].masks.count = per_pf;
	hw->blk[blk].masks.first = hw->pf_id * per_pf;

	memset(hw->blk[blk].masks.masks, 0, sizeof(hw->blk[blk].masks.masks));

	for (i = hw->blk[blk].masks.first;
	     i < hw->blk[blk].masks.first + hw->blk[blk].masks.count; i++)
		ice_write_prof_mask_reg(hw, blk, i, 0, 0);
}

/**
 * ice_init_all_prof_masks - initialize all prof masks
 * @hw: pointer to the HW struct
 */
static void ice_init_all_prof_masks(struct ice_hw *hw)
{
	ice_init_prof_masks(hw, ICE_BLK_RSS);
	ice_init_prof_masks(hw, ICE_BLK_FD);
}

/**
 * ice_alloc_prof_mask - allocate profile mask
 * @hw: pointer to the HW struct
 * @blk: hardware block
 * @idx: index of FV which will use the mask
 * @mask: the 16-bit mask
 * @mask_idx: variable to receive the mask index
 */
static int
ice_alloc_prof_mask(struct ice_hw *hw, enum ice_block blk, u16 idx, u16 mask,
		    u16 *mask_idx)
{
	bool found_unused = false, found_copy = false;
	u16 unused_idx = 0, copy_idx = 0;
	int status = -ENOSPC;
	u16 i;

	if (blk != ICE_BLK_RSS && blk != ICE_BLK_FD)
		return -EINVAL;

	mutex_lock(&hw->blk[blk].masks.lock);

	for (i = hw->blk[blk].masks.first;
	     i < hw->blk[blk].masks.first + hw->blk[blk].masks.count; i++)
		if (hw->blk[blk].masks.masks[i].in_use) {
			/* if mask is in use and it exactly duplicates the
			 * desired mask and index, then in can be reused
			 */
			if (hw->blk[blk].masks.masks[i].mask == mask &&
			    hw->blk[blk].masks.masks[i].idx == idx) {
				found_copy = true;
				copy_idx = i;
				break;
			}
		} else {
			/* save off unused index, but keep searching in case
			 * there is an exact match later on
			 */
			if (!found_unused) {
				found_unused = true;
				unused_idx = i;
			}
		}

	if (found_copy)
		i = copy_idx;
	else if (found_unused)
		i = unused_idx;
	else
		goto err_ice_alloc_prof_mask;

	/* update mask for a new entry */
	if (found_unused) {
		hw->blk[blk].masks.masks[i].in_use = true;
		hw->blk[blk].masks.masks[i].mask = mask;
		hw->blk[blk].masks.masks[i].idx = idx;
		hw->blk[blk].masks.masks[i].ref = 0;
		ice_write_prof_mask_reg(hw, blk, i, idx, mask);
	}

	hw->blk[blk].masks.masks[i].ref++;
	*mask_idx = i;
	status = 0;

err_ice_alloc_prof_mask:
	mutex_unlock(&hw->blk[blk].masks.lock);

	return status;
}

/**
 * ice_free_prof_mask - free profile mask
 * @hw: pointer to the HW struct
 * @blk: hardware block
 * @mask_idx: index of mask
 */
static int
ice_free_prof_mask(struct ice_hw *hw, enum ice_block blk, u16 mask_idx)
{
	if (blk != ICE_BLK_RSS && blk != ICE_BLK_FD)
		return -EINVAL;

	if (!(mask_idx >= hw->blk[blk].masks.first &&
	      mask_idx < hw->blk[blk].masks.first + hw->blk[blk].masks.count))
		return -ENOENT;

	mutex_lock(&hw->blk[blk].masks.lock);

	if (!hw->blk[blk].masks.masks[mask_idx].in_use)
		goto exit_ice_free_prof_mask;

	if (hw->blk[blk].masks.masks[mask_idx].ref > 1) {
		hw->blk[blk].masks.masks[mask_idx].ref--;
		goto exit_ice_free_prof_mask;
	}

	/* remove mask */
	hw->blk[blk].masks.masks[mask_idx].in_use = false;
	hw->blk[blk].masks.masks[mask_idx].mask = 0;
	hw->blk[blk].masks.masks[mask_idx].idx = 0;

	/* update mask as unused entry */
	ice_debug(hw, ICE_DBG_PKG, "Free mask, blk %d, mask %d\n", blk,
		  mask_idx);
	ice_write_prof_mask_reg(hw, blk, mask_idx, 0, 0);

exit_ice_free_prof_mask:
	mutex_unlock(&hw->blk[blk].masks.lock);

	return 0;
}

/**
 * ice_free_prof_masks - free all profile masks for a profile
 * @hw: pointer to the HW struct
 * @blk: hardware block
 * @prof_id: profile ID
 */
static int
ice_free_prof_masks(struct ice_hw *hw, enum ice_block blk, u16 prof_id)
{
	u32 mask_bm;
	u16 i;

	if (blk != ICE_BLK_RSS && blk != ICE_BLK_FD)
		return -EINVAL;

	mask_bm = hw->blk[blk].es.mask_ena[prof_id];
	for (i = 0; i < BITS_PER_BYTE * sizeof(mask_bm); i++)
		if (mask_bm & BIT(i))
			ice_free_prof_mask(hw, blk, i);

	return 0;
}

/**
 * ice_shutdown_prof_masks - releases lock for masking
 * @hw: pointer to the HW struct
 * @blk: hardware block
 *
 * This should be called before unloading the driver
 */
static void ice_shutdown_prof_masks(struct ice_hw *hw, enum ice_block blk)
{
	u16 i;

	mutex_lock(&hw->blk[blk].masks.lock);

	for (i = hw->blk[blk].masks.first;
	     i < hw->blk[blk].masks.first + hw->blk[blk].masks.count; i++) {
		ice_write_prof_mask_reg(hw, blk, i, 0, 0);

		hw->blk[blk].masks.masks[i].in_use = false;
		hw->blk[blk].masks.masks[i].idx = 0;
		hw->blk[blk].masks.masks[i].mask = 0;
	}

	mutex_unlock(&hw->blk[blk].masks.lock);
	mutex_destroy(&hw->blk[blk].masks.lock);
}

/**
 * ice_shutdown_all_prof_masks - releases all locks for masking
 * @hw: pointer to the HW struct
 *
 * This should be called before unloading the driver
 */
static void ice_shutdown_all_prof_masks(struct ice_hw *hw)
{
	ice_shutdown_prof_masks(hw, ICE_BLK_RSS);
	ice_shutdown_prof_masks(hw, ICE_BLK_FD);
}

/**
 * ice_update_prof_masking - set registers according to masking
 * @hw: pointer to the HW struct
 * @blk: hardware block
 * @prof_id: profile ID
 * @masks: masks
 */
static int
ice_update_prof_masking(struct ice_hw *hw, enum ice_block blk, u16 prof_id,
			u16 *masks)
{
	bool err = false;
	u32 ena_mask = 0;
	u16 idx;
	u16 i;

	/* Only support FD and RSS masking, otherwise nothing to be done */
	if (blk != ICE_BLK_RSS && blk != ICE_BLK_FD)
		return 0;

	for (i = 0; i < hw->blk[blk].es.fvw; i++)
		if (masks[i] && masks[i] != 0xFFFF) {
			if (!ice_alloc_prof_mask(hw, blk, i, masks[i], &idx)) {
				ena_mask |= BIT(idx);
			} else {
				/* not enough bitmaps */
				err = true;
				break;
			}
		}

	if (err) {
		/* free any bitmaps we have allocated */
		for (i = 0; i < BITS_PER_BYTE * sizeof(ena_mask); i++)
			if (ena_mask & BIT(i))
				ice_free_prof_mask(hw, blk, i);

		return -EIO;
	}

	/* enable the masks for this profile */
	ice_write_prof_mask_enable_res(hw, blk, prof_id, ena_mask);

	/* store enabled masks with profile so that they can be freed later */
	hw->blk[blk].es.mask_ena[prof_id] = ena_mask;

	return 0;
}

/**
 * ice_write_es - write an extraction sequence and symmetric setting to hardware
 * @hw: pointer to the HW struct
 * @blk: the block in which to write the extraction sequence
 * @prof_id: the profile ID to write
 * @fv: pointer to the extraction sequence to write - NULL to clear extraction
 * @symm: symmetric setting for RSS profiles
 */
static void
ice_write_es(struct ice_hw *hw, enum ice_block blk, u8 prof_id,
	     struct ice_fv_word *fv, bool symm)
{
	u16 off;

	off = prof_id * hw->blk[blk].es.fvw;
	if (!fv) {
		memset(&hw->blk[blk].es.t[off], 0,
		       hw->blk[blk].es.fvw * sizeof(*fv));
		hw->blk[blk].es.written[prof_id] = false;
	} else {
		memcpy(&hw->blk[blk].es.t[off], fv,
		       hw->blk[blk].es.fvw * sizeof(*fv));
	}

	if (blk == ICE_BLK_RSS)
		hw->blk[blk].es.symm[prof_id] = symm;
}

/**
 * ice_prof_dec_ref - decrement reference count for profile
 * @hw: pointer to the HW struct
 * @blk: the block from which to free the profile ID
 * @prof_id: the profile ID for which to decrement the reference count
 */
static int
ice_prof_dec_ref(struct ice_hw *hw, enum ice_block blk, u8 prof_id)
{
	if (prof_id > hw->blk[blk].es.count)
		return -EINVAL;

	if (hw->blk[blk].es.ref_count[prof_id] > 0) {
		if (!--hw->blk[blk].es.ref_count[prof_id]) {
			ice_write_es(hw, blk, prof_id, NULL, false);
			ice_free_prof_masks(hw, blk, prof_id);
			return ice_free_prof_id(hw, blk, prof_id);
		}
	}

	return 0;
}

/* Block / table section IDs */
static const u32 ice_blk_sids[ICE_BLK_COUNT][ICE_SID_OFF_COUNT] = {
	/* SWITCH */
	{	ICE_SID_XLT1_SW,
		ICE_SID_XLT2_SW,
		ICE_SID_PROFID_TCAM_SW,
		ICE_SID_PROFID_REDIR_SW,
		ICE_SID_FLD_VEC_SW
	},

	/* ACL */
	{	ICE_SID_XLT1_ACL,
		ICE_SID_XLT2_ACL,
		ICE_SID_PROFID_TCAM_ACL,
		ICE_SID_PROFID_REDIR_ACL,
		ICE_SID_FLD_VEC_ACL
	},

	/* FD */
	{	ICE_SID_XLT1_FD,
		ICE_SID_XLT2_FD,
		ICE_SID_PROFID_TCAM_FD,
		ICE_SID_PROFID_REDIR_FD,
		ICE_SID_FLD_VEC_FD
	},

	/* RSS */
	{	ICE_SID_XLT1_RSS,
		ICE_SID_XLT2_RSS,
		ICE_SID_PROFID_TCAM_RSS,
		ICE_SID_PROFID_REDIR_RSS,
		ICE_SID_FLD_VEC_RSS
	},

	/* PE */
	{	ICE_SID_XLT1_PE,
		ICE_SID_XLT2_PE,
		ICE_SID_PROFID_TCAM_PE,
		ICE_SID_PROFID_REDIR_PE,
		ICE_SID_FLD_VEC_PE
	}
};

/**
 * ice_init_sw_xlt1_db - init software XLT1 database from HW tables
 * @hw: pointer to the hardware structure
 * @blk: the HW block to initialize
 */
static void ice_init_sw_xlt1_db(struct ice_hw *hw, enum ice_block blk)
{
	u16 pt;

	for (pt = 0; pt < hw->blk[blk].xlt1.count; pt++) {
		u8 ptg;

		ptg = hw->blk[blk].xlt1.t[pt];
		if (ptg != ICE_DEFAULT_PTG) {
			ice_ptg_alloc_val(hw, blk, ptg);
			ice_ptg_add_mv_ptype(hw, blk, pt, ptg);
		}
	}
}

/**
 * ice_init_sw_xlt2_db - init software XLT2 database from HW tables
 * @hw: pointer to the hardware structure
 * @blk: the HW block to initialize
 */
static void ice_init_sw_xlt2_db(struct ice_hw *hw, enum ice_block blk)
{
	u16 vsi;

	for (vsi = 0; vsi < hw->blk[blk].xlt2.count; vsi++) {
		u16 vsig;

		vsig = hw->blk[blk].xlt2.t[vsi];
		if (vsig) {
			ice_vsig_alloc_val(hw, blk, vsig);
			ice_vsig_add_mv_vsi(hw, blk, vsi, vsig);
			/* no changes at this time, since this has been
			 * initialized from the original package
			 */
			hw->blk[blk].xlt2.vsis[vsi].changed = 0;
		}
	}
}

/**
 * ice_init_sw_db - init software database from HW tables
 * @hw: pointer to the hardware structure
 */
static void ice_init_sw_db(struct ice_hw *hw)
{
	u16 i;

	for (i = 0; i < ICE_BLK_COUNT; i++) {
		ice_init_sw_xlt1_db(hw, (enum ice_block)i);
		ice_init_sw_xlt2_db(hw, (enum ice_block)i);
	}
}

/**
 * ice_fill_tbl - Reads content of a single table type into database
 * @hw: pointer to the hardware structure
 * @block_id: Block ID of the table to copy
 * @sid: Section ID of the table to copy
 *
 * Will attempt to read the entire content of a given table of a single block
 * into the driver database. We assume that the buffer will always
 * be as large or larger than the data contained in the package. If
 * this condition is not met, there is most likely an error in the package
 * contents.
 */
static void ice_fill_tbl(struct ice_hw *hw, enum ice_block block_id, u32 sid)
{
	u32 dst_len, sect_len, offset = 0;
	struct ice_prof_redir_section *pr;
	struct ice_prof_id_section *pid;
	struct ice_xlt1_section *xlt1;
	struct ice_xlt2_section *xlt2;
	struct ice_sw_fv_section *es;
	struct ice_pkg_enum state;
	u8 *src, *dst;
	void *sect;

	/* if the HW segment pointer is null then the first iteration of
	 * ice_pkg_enum_section() will fail. In this case the HW tables will
	 * not be filled and return success.
	 */
	if (!hw->seg) {
		ice_debug(hw, ICE_DBG_PKG, "hw->seg is NULL, tables are not filled\n");
		return;
	}

	memset(&state, 0, sizeof(state));

	sect = ice_pkg_enum_section(hw->seg, &state, sid);

	while (sect) {
		switch (sid) {
		case ICE_SID_XLT1_SW:
		case ICE_SID_XLT1_FD:
		case ICE_SID_XLT1_RSS:
		case ICE_SID_XLT1_ACL:
		case ICE_SID_XLT1_PE:
			xlt1 = sect;
			src = xlt1->value;
			sect_len = le16_to_cpu(xlt1->count) *
				sizeof(*hw->blk[block_id].xlt1.t);
			dst = hw->blk[block_id].xlt1.t;
			dst_len = hw->blk[block_id].xlt1.count *
				sizeof(*hw->blk[block_id].xlt1.t);
			break;
		case ICE_SID_XLT2_SW:
		case ICE_SID_XLT2_FD:
		case ICE_SID_XLT2_RSS:
		case ICE_SID_XLT2_ACL:
		case ICE_SID_XLT2_PE:
			xlt2 = sect;
			src = (__force u8 *)xlt2->value;
			sect_len = le16_to_cpu(xlt2->count) *
				sizeof(*hw->blk[block_id].xlt2.t);
			dst = (u8 *)hw->blk[block_id].xlt2.t;
			dst_len = hw->blk[block_id].xlt2.count *
				sizeof(*hw->blk[block_id].xlt2.t);
			break;
		case ICE_SID_PROFID_TCAM_SW:
		case ICE_SID_PROFID_TCAM_FD:
		case ICE_SID_PROFID_TCAM_RSS:
		case ICE_SID_PROFID_TCAM_ACL:
		case ICE_SID_PROFID_TCAM_PE:
			pid = sect;
			src = (u8 *)pid->entry;
			sect_len = le16_to_cpu(pid->count) *
				sizeof(*hw->blk[block_id].prof.t);
			dst = (u8 *)hw->blk[block_id].prof.t;
			dst_len = hw->blk[block_id].prof.count *
				sizeof(*hw->blk[block_id].prof.t);
			break;
		case ICE_SID_PROFID_REDIR_SW:
		case ICE_SID_PROFID_REDIR_FD:
		case ICE_SID_PROFID_REDIR_RSS:
		case ICE_SID_PROFID_REDIR_ACL:
		case ICE_SID_PROFID_REDIR_PE:
			pr = sect;
			src = pr->redir_value;
			sect_len = le16_to_cpu(pr->count) *
				sizeof(*hw->blk[block_id].prof_redir.t);
			dst = hw->blk[block_id].prof_redir.t;
			dst_len = hw->blk[block_id].prof_redir.count *
				sizeof(*hw->blk[block_id].prof_redir.t);
			break;
		case ICE_SID_FLD_VEC_SW:
		case ICE_SID_FLD_VEC_FD:
		case ICE_SID_FLD_VEC_RSS:
		case ICE_SID_FLD_VEC_ACL:
		case ICE_SID_FLD_VEC_PE:
			es = sect;
			src = (u8 *)es->fv;
			sect_len = (u32)(le16_to_cpu(es->count) *
					 hw->blk[block_id].es.fvw) *
				sizeof(*hw->blk[block_id].es.t);
			dst = (u8 *)hw->blk[block_id].es.t;
			dst_len = (u32)(hw->blk[block_id].es.count *
					hw->blk[block_id].es.fvw) *
				sizeof(*hw->blk[block_id].es.t);
			break;
		default:
			return;
		}

		/* if the section offset exceeds destination length, terminate
		 * table fill.
		 */
		if (offset > dst_len)
			return;

		/* if the sum of section size and offset exceed destination size
		 * then we are out of bounds of the HW table size for that PF.
		 * Changing section length to fill the remaining table space
		 * of that PF.
		 */
		if ((offset + sect_len) > dst_len)
			sect_len = dst_len - offset;

		memcpy(dst + offset, src, sect_len);
		offset += sect_len;
		sect = ice_pkg_enum_section(NULL, &state, sid);
	}
}

/**
 * ice_fill_blk_tbls - Read package context for tables
 * @hw: pointer to the hardware structure
 *
 * Reads the current package contents and populates the driver
 * database with the data iteratively for all advanced feature
 * blocks. Assume that the HW tables have been allocated.
 */
void ice_fill_blk_tbls(struct ice_hw *hw)
{
	u8 i;

	for (i = 0; i < ICE_BLK_COUNT; i++) {
		enum ice_block blk_id = (enum ice_block)i;

		ice_fill_tbl(hw, blk_id, hw->blk[blk_id].xlt1.sid);
		ice_fill_tbl(hw, blk_id, hw->blk[blk_id].xlt2.sid);
		ice_fill_tbl(hw, blk_id, hw->blk[blk_id].prof.sid);
		ice_fill_tbl(hw, blk_id, hw->blk[blk_id].prof_redir.sid);
		ice_fill_tbl(hw, blk_id, hw->blk[blk_id].es.sid);
	}

	ice_init_sw_db(hw);
}

/**
 * ice_free_prof_map - free profile map
 * @hw: pointer to the hardware structure
 * @blk_idx: HW block index
 */
static void ice_free_prof_map(struct ice_hw *hw, u8 blk_idx)
{
	struct ice_es *es = &hw->blk[blk_idx].es;
	struct ice_prof_map *del, *tmp;

	mutex_lock(&es->prof_map_lock);
	list_for_each_entry_safe(del, tmp, &es->prof_map, list) {
		list_del(&del->list);
		devm_kfree(ice_hw_to_dev(hw), del);
	}
	INIT_LIST_HEAD(&es->prof_map);
	mutex_unlock(&es->prof_map_lock);
}

/**
 * ice_free_flow_profs - free flow profile entries
 * @hw: pointer to the hardware structure
 * @blk_idx: HW block index
 */
static void ice_free_flow_profs(struct ice_hw *hw, u8 blk_idx)
{
	struct ice_flow_prof *p, *tmp;

	mutex_lock(&hw->fl_profs_locks[blk_idx]);
	list_for_each_entry_safe(p, tmp, &hw->fl_profs[blk_idx], l_entry) {
		struct ice_flow_entry *e, *t;

		list_for_each_entry_safe(e, t, &p->entries, l_entry)
			ice_flow_rem_entry(hw, (enum ice_block)blk_idx,
					   ICE_FLOW_ENTRY_HNDL(e));

		list_del(&p->l_entry);

		mutex_destroy(&p->entries_lock);
		devm_kfree(ice_hw_to_dev(hw), p);
	}
	mutex_unlock(&hw->fl_profs_locks[blk_idx]);

	/* if driver is in reset and tables are being cleared
	 * re-initialize the flow profile list heads
	 */
	INIT_LIST_HEAD(&hw->fl_profs[blk_idx]);
}

/**
 * ice_free_vsig_tbl - free complete VSIG table entries
 * @hw: pointer to the hardware structure
 * @blk: the HW block on which to free the VSIG table entries
 */
static void ice_free_vsig_tbl(struct ice_hw *hw, enum ice_block blk)
{
	u16 i;

	if (!hw->blk[blk].xlt2.vsig_tbl)
		return;

	for (i = 1; i < ICE_MAX_VSIGS; i++)
		if (hw->blk[blk].xlt2.vsig_tbl[i].in_use)
			ice_vsig_free(hw, blk, i);
}

/**
 * ice_free_hw_tbls - free hardware table memory
 * @hw: pointer to the hardware structure
 */
void ice_free_hw_tbls(struct ice_hw *hw)
{
	struct ice_rss_cfg *r, *rt;
	u8 i;

	for (i = 0; i < ICE_BLK_COUNT; i++) {
		if (hw->blk[i].is_list_init) {
			struct ice_es *es = &hw->blk[i].es;

			ice_free_prof_map(hw, i);
			mutex_destroy(&es->prof_map_lock);

			ice_free_flow_profs(hw, i);
			mutex_destroy(&hw->fl_profs_locks[i]);

			hw->blk[i].is_list_init = false;
		}
		ice_free_vsig_tbl(hw, (enum ice_block)i);
		devm_kfree(ice_hw_to_dev(hw), hw->blk[i].xlt1.ptypes);
		devm_kfree(ice_hw_to_dev(hw), hw->blk[i].xlt1.ptg_tbl);
		devm_kfree(ice_hw_to_dev(hw), hw->blk[i].xlt1.t);
		devm_kfree(ice_hw_to_dev(hw), hw->blk[i].xlt2.t);
		devm_kfree(ice_hw_to_dev(hw), hw->blk[i].xlt2.vsig_tbl);
		devm_kfree(ice_hw_to_dev(hw), hw->blk[i].xlt2.vsis);
		devm_kfree(ice_hw_to_dev(hw), hw->blk[i].prof.t);
		devm_kfree(ice_hw_to_dev(hw), hw->blk[i].prof_redir.t);
		devm_kfree(ice_hw_to_dev(hw), hw->blk[i].es.t);
		devm_kfree(ice_hw_to_dev(hw), hw->blk[i].es.ref_count);
		devm_kfree(ice_hw_to_dev(hw), hw->blk[i].es.symm);
		devm_kfree(ice_hw_to_dev(hw), hw->blk[i].es.written);
		devm_kfree(ice_hw_to_dev(hw), hw->blk[i].es.mask_ena);
		devm_kfree(ice_hw_to_dev(hw), hw->blk[i].prof_id.id);
	}

	list_for_each_entry_safe(r, rt, &hw->rss_list_head, l_entry) {
		list_del(&r->l_entry);
		devm_kfree(ice_hw_to_dev(hw), r);
	}
	mutex_destroy(&hw->rss_locks);
	ice_shutdown_all_prof_masks(hw);
	memset(hw->blk, 0, sizeof(hw->blk));
}

/**
 * ice_init_flow_profs - init flow profile locks and list heads
 * @hw: pointer to the hardware structure
 * @blk_idx: HW block index
 */
static void ice_init_flow_profs(struct ice_hw *hw, u8 blk_idx)
{
	mutex_init(&hw->fl_profs_locks[blk_idx]);
	INIT_LIST_HEAD(&hw->fl_profs[blk_idx]);
}

/**
 * ice_clear_hw_tbls - clear HW tables and flow profiles
 * @hw: pointer to the hardware structure
 */
void ice_clear_hw_tbls(struct ice_hw *hw)
{
	u8 i;

	for (i = 0; i < ICE_BLK_COUNT; i++) {
		struct ice_prof_redir *prof_redir = &hw->blk[i].prof_redir;
		struct ice_prof_id *prof_id = &hw->blk[i].prof_id;
		struct ice_prof_tcam *prof = &hw->blk[i].prof;
		struct ice_xlt1 *xlt1 = &hw->blk[i].xlt1;
		struct ice_xlt2 *xlt2 = &hw->blk[i].xlt2;
		struct ice_es *es = &hw->blk[i].es;

		if (hw->blk[i].is_list_init) {
			ice_free_prof_map(hw, i);
			ice_free_flow_profs(hw, i);
		}

		ice_free_vsig_tbl(hw, (enum ice_block)i);

		memset(xlt1->ptypes, 0, xlt1->count * sizeof(*xlt1->ptypes));
		memset(xlt1->ptg_tbl, 0,
		       ICE_MAX_PTGS * sizeof(*xlt1->ptg_tbl));
		memset(xlt1->t, 0, xlt1->count * sizeof(*xlt1->t));

		memset(xlt2->vsis, 0, xlt2->count * sizeof(*xlt2->vsis));
		memset(xlt2->vsig_tbl, 0,
		       xlt2->count * sizeof(*xlt2->vsig_tbl));
		memset(xlt2->t, 0, xlt2->count * sizeof(*xlt2->t));

		memset(prof->t, 0, prof->count * sizeof(*prof->t));
		memset(prof_redir->t, 0,
		       prof_redir->count * sizeof(*prof_redir->t));

		memset(es->t, 0, es->count * sizeof(*es->t) * es->fvw);
		memset(es->ref_count, 0, es->count * sizeof(*es->ref_count));
		memset(es->symm, 0, es->count * sizeof(*es->symm));
		memset(es->written, 0, es->count * sizeof(*es->written));
		memset(es->mask_ena, 0, es->count * sizeof(*es->mask_ena));

		memset(prof_id->id, 0, prof_id->count * sizeof(*prof_id->id));
	}
}

/**
 * ice_init_hw_tbls - init hardware table memory
 * @hw: pointer to the hardware structure
 */
int ice_init_hw_tbls(struct ice_hw *hw)
{
	u8 i;

	mutex_init(&hw->rss_locks);
	INIT_LIST_HEAD(&hw->rss_list_head);
	ice_init_all_prof_masks(hw);
	for (i = 0; i < ICE_BLK_COUNT; i++) {
		struct ice_prof_redir *prof_redir = &hw->blk[i].prof_redir;
		struct ice_prof_id *prof_id = &hw->blk[i].prof_id;
		struct ice_prof_tcam *prof = &hw->blk[i].prof;
		struct ice_xlt1 *xlt1 = &hw->blk[i].xlt1;
		struct ice_xlt2 *xlt2 = &hw->blk[i].xlt2;
		struct ice_es *es = &hw->blk[i].es;
		u16 j;

		if (hw->blk[i].is_list_init)
			continue;

		ice_init_flow_profs(hw, i);
		mutex_init(&es->prof_map_lock);
		INIT_LIST_HEAD(&es->prof_map);
		hw->blk[i].is_list_init = true;

		hw->blk[i].overwrite = blk_sizes[i].overwrite;
		es->reverse = blk_sizes[i].reverse;

		xlt1->sid = ice_blk_sids[i][ICE_SID_XLT1_OFF];
		xlt1->count = blk_sizes[i].xlt1;

		xlt1->ptypes = devm_kcalloc(ice_hw_to_dev(hw), xlt1->count,
					    sizeof(*xlt1->ptypes), GFP_KERNEL);

		if (!xlt1->ptypes)
			goto err;

		xlt1->ptg_tbl = devm_kcalloc(ice_hw_to_dev(hw), ICE_MAX_PTGS,
					     sizeof(*xlt1->ptg_tbl),
					     GFP_KERNEL);

		if (!xlt1->ptg_tbl)
			goto err;

		xlt1->t = devm_kcalloc(ice_hw_to_dev(hw), xlt1->count,
				       sizeof(*xlt1->t), GFP_KERNEL);
		if (!xlt1->t)
			goto err;

		xlt2->sid = ice_blk_sids[i][ICE_SID_XLT2_OFF];
		xlt2->count = blk_sizes[i].xlt2;

		xlt2->vsis = devm_kcalloc(ice_hw_to_dev(hw), xlt2->count,
					  sizeof(*xlt2->vsis), GFP_KERNEL);

		if (!xlt2->vsis)
			goto err;

		xlt2->vsig_tbl = devm_kcalloc(ice_hw_to_dev(hw), xlt2->count,
					      sizeof(*xlt2->vsig_tbl),
					      GFP_KERNEL);
		if (!xlt2->vsig_tbl)
			goto err;

		for (j = 0; j < xlt2->count; j++)
			INIT_LIST_HEAD(&xlt2->vsig_tbl[j].prop_lst);

		xlt2->t = devm_kcalloc(ice_hw_to_dev(hw), xlt2->count,
				       sizeof(*xlt2->t), GFP_KERNEL);
		if (!xlt2->t)
			goto err;

		prof->sid = ice_blk_sids[i][ICE_SID_PR_OFF];
		prof->count = blk_sizes[i].prof_tcam;
		prof->max_prof_id = blk_sizes[i].prof_id;
		prof->cdid_bits = blk_sizes[i].prof_cdid_bits;
		prof->t = devm_kcalloc(ice_hw_to_dev(hw), prof->count,
				       sizeof(*prof->t), GFP_KERNEL);

		if (!prof->t)
			goto err;

		prof_redir->sid = ice_blk_sids[i][ICE_SID_PR_REDIR_OFF];
		prof_redir->count = blk_sizes[i].prof_redir;
		prof_redir->t = devm_kcalloc(ice_hw_to_dev(hw),
					     prof_redir->count,
					     sizeof(*prof_redir->t),
					     GFP_KERNEL);

		if (!prof_redir->t)
			goto err;

		es->sid = ice_blk_sids[i][ICE_SID_ES_OFF];
		es->count = blk_sizes[i].es;
		es->fvw = blk_sizes[i].fvw;
		es->t = devm_kcalloc(ice_hw_to_dev(hw),
				     (u32)(es->count * es->fvw),
				     sizeof(*es->t), GFP_KERNEL);
		if (!es->t)
			goto err;

		es->ref_count = devm_kcalloc(ice_hw_to_dev(hw), es->count,
					     sizeof(*es->ref_count),
					     GFP_KERNEL);
		if (!es->ref_count)
			goto err;

		es->symm = devm_kcalloc(ice_hw_to_dev(hw), es->count,
					sizeof(*es->symm), GFP_KERNEL);
		if (!es->symm)
			goto err;

		es->written = devm_kcalloc(ice_hw_to_dev(hw), es->count,
					   sizeof(*es->written), GFP_KERNEL);
		if (!es->written)
			goto err;

		es->mask_ena = devm_kcalloc(ice_hw_to_dev(hw), es->count,
					    sizeof(*es->mask_ena), GFP_KERNEL);
		if (!es->mask_ena)
			goto err;

		prof_id->count = blk_sizes[i].prof_id;
		prof_id->id = devm_kcalloc(ice_hw_to_dev(hw), prof_id->count,
					   sizeof(*prof_id->id), GFP_KERNEL);
		if (!prof_id->id)
			goto err;
	}
	return 0;

err:
	ice_free_hw_tbls(hw);
	return -ENOMEM;
}

/**
 * ice_prof_gen_key - generate profile ID key
 * @hw: pointer to the HW struct
 * @blk: the block in which to write profile ID to
 * @ptg: packet type group (PTG) portion of key
 * @vsig: VSIG portion of key
 * @cdid: CDID portion of key
 * @flags: flag portion of key
 * @vl_msk: valid mask
 * @dc_msk: don't care mask
 * @nm_msk: never match mask
 * @key: output of profile ID key
 */
static int
ice_prof_gen_key(struct ice_hw *hw, enum ice_block blk, u8 ptg, u16 vsig,
		 u8 cdid, u16 flags, u8 vl_msk[ICE_TCAM_KEY_VAL_SZ],
		 u8 dc_msk[ICE_TCAM_KEY_VAL_SZ], u8 nm_msk[ICE_TCAM_KEY_VAL_SZ],
		 u8 key[ICE_TCAM_KEY_SZ])
{
	struct ice_prof_id_key inkey;

	inkey.xlt1 = ptg;
	inkey.xlt2_cdid = cpu_to_le16(vsig);
	inkey.flags = cpu_to_le16(flags);

	switch (hw->blk[blk].prof.cdid_bits) {
	case 0:
		break;
	case 2:
#define ICE_CD_2_M 0xC000U
#define ICE_CD_2_S 14
		inkey.xlt2_cdid &= ~cpu_to_le16(ICE_CD_2_M);
		inkey.xlt2_cdid |= cpu_to_le16(BIT(cdid) << ICE_CD_2_S);
		break;
	case 4:
#define ICE_CD_4_M 0xF000U
#define ICE_CD_4_S 12
		inkey.xlt2_cdid &= ~cpu_to_le16(ICE_CD_4_M);
		inkey.xlt2_cdid |= cpu_to_le16(BIT(cdid) << ICE_CD_4_S);
		break;
	case 8:
#define ICE_CD_8_M 0xFF00U
#define ICE_CD_8_S 16
		inkey.xlt2_cdid &= ~cpu_to_le16(ICE_CD_8_M);
		inkey.xlt2_cdid |= cpu_to_le16(BIT(cdid) << ICE_CD_8_S);
		break;
	default:
		ice_debug(hw, ICE_DBG_PKG, "Error in profile config\n");
		break;
	}

	return ice_set_key(key, ICE_TCAM_KEY_SZ, (u8 *)&inkey, vl_msk, dc_msk,
			   nm_msk, 0, ICE_TCAM_KEY_SZ / 2);
}

/**
 * ice_tcam_write_entry - write TCAM entry
 * @hw: pointer to the HW struct
 * @blk: the block in which to write profile ID to
 * @idx: the entry index to write to
 * @prof_id: profile ID
 * @ptg: packet type group (PTG) portion of key
 * @vsig: VSIG portion of key
 * @cdid: CDID portion of key
 * @flags: flag portion of key
 * @vl_msk: valid mask
 * @dc_msk: don't care mask
 * @nm_msk: never match mask
 */
static int
ice_tcam_write_entry(struct ice_hw *hw, enum ice_block blk, u16 idx,
		     u8 prof_id, u8 ptg, u16 vsig, u8 cdid, u16 flags,
		     u8 vl_msk[ICE_TCAM_KEY_VAL_SZ],
		     u8 dc_msk[ICE_TCAM_KEY_VAL_SZ],
		     u8 nm_msk[ICE_TCAM_KEY_VAL_SZ])
{
	struct ice_prof_tcam_entry;
	int status;

	status = ice_prof_gen_key(hw, blk, ptg, vsig, cdid, flags, vl_msk,
				  dc_msk, nm_msk, hw->blk[blk].prof.t[idx].key);
	if (!status) {
		hw->blk[blk].prof.t[idx].addr = cpu_to_le16(idx);
		hw->blk[blk].prof.t[idx].prof_id = prof_id;
	}

	return status;
}

/**
 * ice_vsig_get_ref - returns number of VSIs belong to a VSIG
 * @hw: pointer to the hardware structure
 * @blk: HW block
 * @vsig: VSIG to query
 * @refs: pointer to variable to receive the reference count
 */
static int
ice_vsig_get_ref(struct ice_hw *hw, enum ice_block blk, u16 vsig, u16 *refs)
{
	u16 idx = vsig & ICE_VSIG_IDX_M;
	struct ice_vsig_vsi *ptr;

	*refs = 0;

	if (!hw->blk[blk].xlt2.vsig_tbl[idx].in_use)
		return -ENOENT;

	ptr = hw->blk[blk].xlt2.vsig_tbl[idx].first_vsi;
	while (ptr) {
		(*refs)++;
		ptr = ptr->next_vsi;
	}

	return 0;
}

/**
 * ice_has_prof_vsig - check to see if VSIG has a specific profile
 * @hw: pointer to the hardware structure
 * @blk: HW block
 * @vsig: VSIG to check against
 * @hdl: profile handle
 */
static bool
ice_has_prof_vsig(struct ice_hw *hw, enum ice_block blk, u16 vsig, u64 hdl)
{
	u16 idx = vsig & ICE_VSIG_IDX_M;
	struct ice_vsig_prof *ent;

	list_for_each_entry(ent, &hw->blk[blk].xlt2.vsig_tbl[idx].prop_lst,
			    list)
		if (ent->profile_cookie == hdl)
			return true;

	ice_debug(hw, ICE_DBG_INIT, "Characteristic list for VSI group %d not found.\n",
		  vsig);
	return false;
}

/**
 * ice_prof_bld_es - build profile ID extraction sequence changes
 * @hw: pointer to the HW struct
 * @blk: hardware block
 * @bld: the update package buffer build to add to
 * @chgs: the list of changes to make in hardware
 */
static int
ice_prof_bld_es(struct ice_hw *hw, enum ice_block blk,
		struct ice_buf_build *bld, struct list_head *chgs)
{
	u16 vec_size = hw->blk[blk].es.fvw * sizeof(struct ice_fv_word);
	struct ice_chs_chg *tmp;

	list_for_each_entry(tmp, chgs, list_entry)
		if (tmp->type == ICE_PTG_ES_ADD && tmp->add_prof) {
			u16 off = tmp->prof_id * hw->blk[blk].es.fvw;
			struct ice_pkg_es *p;
			u32 id;

			id = ice_sect_id(blk, ICE_VEC_TBL);
			p = ice_pkg_buf_alloc_section(bld, id,
						      struct_size(p, es, 1) +
						      vec_size -
						      sizeof(p->es[0]));

			if (!p)
				return -ENOSPC;

			p->count = cpu_to_le16(1);
			p->offset = cpu_to_le16(tmp->prof_id);

			memcpy(p->es, &hw->blk[blk].es.t[off], vec_size);
		}

	return 0;
}

/**
 * ice_prof_bld_tcam - build profile ID TCAM changes
 * @hw: pointer to the HW struct
 * @blk: hardware block
 * @bld: the update package buffer build to add to
 * @chgs: the list of changes to make in hardware
 */
static int
ice_prof_bld_tcam(struct ice_hw *hw, enum ice_block blk,
		  struct ice_buf_build *bld, struct list_head *chgs)
{
	struct ice_chs_chg *tmp;

	list_for_each_entry(tmp, chgs, list_entry)
		if (tmp->type == ICE_TCAM_ADD && tmp->add_tcam_idx) {
			struct ice_prof_id_section *p;
			u32 id;

			id = ice_sect_id(blk, ICE_PROF_TCAM);
			p = ice_pkg_buf_alloc_section(bld, id,
						      struct_size(p, entry, 1));

			if (!p)
				return -ENOSPC;

			p->count = cpu_to_le16(1);
			p->entry[0].addr = cpu_to_le16(tmp->tcam_idx);
			p->entry[0].prof_id = tmp->prof_id;

			memcpy(p->entry[0].key,
			       &hw->blk[blk].prof.t[tmp->tcam_idx].key,
			       sizeof(hw->blk[blk].prof.t->key));
		}

	return 0;
}

/**
 * ice_prof_bld_xlt1 - build XLT1 changes
 * @blk: hardware block
 * @bld: the update package buffer build to add to
 * @chgs: the list of changes to make in hardware
 */
static int
ice_prof_bld_xlt1(enum ice_block blk, struct ice_buf_build *bld,
		  struct list_head *chgs)
{
	struct ice_chs_chg *tmp;

	list_for_each_entry(tmp, chgs, list_entry)
		if (tmp->type == ICE_PTG_ES_ADD && tmp->add_ptg) {
			struct ice_xlt1_section *p;
			u32 id;

			id = ice_sect_id(blk, ICE_XLT1);
			p = ice_pkg_buf_alloc_section(bld, id,
						      struct_size(p, value, 1));

			if (!p)
				return -ENOSPC;

			p->count = cpu_to_le16(1);
			p->offset = cpu_to_le16(tmp->ptype);
			p->value[0] = tmp->ptg;
		}

	return 0;
}

/**
 * ice_prof_bld_xlt2 - build XLT2 changes
 * @blk: hardware block
 * @bld: the update package buffer build to add to
 * @chgs: the list of changes to make in hardware
 */
static int
ice_prof_bld_xlt2(enum ice_block blk, struct ice_buf_build *bld,
		  struct list_head *chgs)
{
	struct ice_chs_chg *tmp;

	list_for_each_entry(tmp, chgs, list_entry) {
		struct ice_xlt2_section *p;
		u32 id;

		switch (tmp->type) {
		case ICE_VSIG_ADD:
		case ICE_VSI_MOVE:
		case ICE_VSIG_REM:
			id = ice_sect_id(blk, ICE_XLT2);
			p = ice_pkg_buf_alloc_section(bld, id,
						      struct_size(p, value, 1));

			if (!p)
				return -ENOSPC;

			p->count = cpu_to_le16(1);
			p->offset = cpu_to_le16(tmp->vsi);
			p->value[0] = cpu_to_le16(tmp->vsig);
			break;
		default:
			break;
		}
	}

	return 0;
}

/**
 * ice_upd_prof_hw - update hardware using the change list
 * @hw: pointer to the HW struct
 * @blk: hardware block
 * @chgs: the list of changes to make in hardware
 */
static int
ice_upd_prof_hw(struct ice_hw *hw, enum ice_block blk,
		struct list_head *chgs)
{
	struct ice_buf_build *b;
	struct ice_chs_chg *tmp;
	u16 pkg_sects;
	u16 xlt1 = 0;
	u16 xlt2 = 0;
	u16 tcam = 0;
	u16 es = 0;
	int status;
	u16 sects;

	/* count number of sections we need */
	list_for_each_entry(tmp, chgs, list_entry) {
		switch (tmp->type) {
		case ICE_PTG_ES_ADD:
			if (tmp->add_ptg)
				xlt1++;
			if (tmp->add_prof)
				es++;
			break;
		case ICE_TCAM_ADD:
			tcam++;
			break;
		case ICE_VSIG_ADD:
		case ICE_VSI_MOVE:
		case ICE_VSIG_REM:
			xlt2++;
			break;
		default:
			break;
		}
	}
	sects = xlt1 + xlt2 + tcam + es;

	if (!sects)
		return 0;

	/* Build update package buffer */
	b = ice_pkg_buf_alloc(hw);
	if (!b)
		return -ENOMEM;

	status = ice_pkg_buf_reserve_section(b, sects);
	if (status)
		goto error_tmp;

	/* Preserve order of table update: ES, TCAM, PTG, VSIG */
	if (es) {
		status = ice_prof_bld_es(hw, blk, b, chgs);
		if (status)
			goto error_tmp;
	}

	if (tcam) {
		status = ice_prof_bld_tcam(hw, blk, b, chgs);
		if (status)
			goto error_tmp;
	}

	if (xlt1) {
		status = ice_prof_bld_xlt1(blk, b, chgs);
		if (status)
			goto error_tmp;
	}

	if (xlt2) {
		status = ice_prof_bld_xlt2(blk, b, chgs);
		if (status)
			goto error_tmp;
	}

	/* After package buffer build check if the section count in buffer is
	 * non-zero and matches the number of sections detected for package
	 * update.
	 */
	pkg_sects = ice_pkg_buf_get_active_sections(b);
	if (!pkg_sects || pkg_sects != sects) {
		status = -EINVAL;
		goto error_tmp;
	}

	/* update package */
	status = ice_update_pkg(hw, ice_pkg_buf(b), 1);
	if (status == -EIO)
		ice_debug(hw, ICE_DBG_INIT, "Unable to update HW profile\n");

error_tmp:
	ice_pkg_buf_free(hw, b);
	return status;
}

/**
 * ice_update_fd_mask - set Flow Director Field Vector mask for a profile
 * @hw: pointer to the HW struct
 * @prof_id: profile ID
 * @mask_sel: mask select
 *
 * This function enable any of the masks selected by the mask select parameter
 * for the profile specified.
 */
static void ice_update_fd_mask(struct ice_hw *hw, u16 prof_id, u32 mask_sel)
{
	wr32(hw, GLQF_FDMASK_SEL(prof_id), mask_sel);

	ice_debug(hw, ICE_DBG_INIT, "fd mask(%d): %x = %x\n", prof_id,
		  GLQF_FDMASK_SEL(prof_id), mask_sel);
}

struct ice_fd_src_dst_pair {
	u8 prot_id;
	u8 count;
	u16 off;
};

static const struct ice_fd_src_dst_pair ice_fd_pairs[] = {
	/* These are defined in pairs */
	{ ICE_PROT_IPV4_OF_OR_S, 2, 12 },
	{ ICE_PROT_IPV4_OF_OR_S, 2, 16 },

	{ ICE_PROT_IPV4_IL, 2, 12 },
	{ ICE_PROT_IPV4_IL, 2, 16 },

	{ ICE_PROT_IPV6_OF_OR_S, 8, 8 },
	{ ICE_PROT_IPV6_OF_OR_S, 8, 24 },

	{ ICE_PROT_IPV6_IL, 8, 8 },
	{ ICE_PROT_IPV6_IL, 8, 24 },

	{ ICE_PROT_TCP_IL, 1, 0 },
	{ ICE_PROT_TCP_IL, 1, 2 },

	{ ICE_PROT_UDP_OF, 1, 0 },
	{ ICE_PROT_UDP_OF, 1, 2 },

	{ ICE_PROT_UDP_IL_OR_S, 1, 0 },
	{ ICE_PROT_UDP_IL_OR_S, 1, 2 },

	{ ICE_PROT_SCTP_IL, 1, 0 },
	{ ICE_PROT_SCTP_IL, 1, 2 }
};

#define ICE_FD_SRC_DST_PAIR_COUNT	ARRAY_SIZE(ice_fd_pairs)

/**
 * ice_update_fd_swap - set register appropriately for a FD FV extraction
 * @hw: pointer to the HW struct
 * @prof_id: profile ID
 * @es: extraction sequence (length of array is determined by the block)
 */
static int
ice_update_fd_swap(struct ice_hw *hw, u16 prof_id, struct ice_fv_word *es)
{
	DECLARE_BITMAP(pair_list, ICE_FD_SRC_DST_PAIR_COUNT);
	u8 pair_start[ICE_FD_SRC_DST_PAIR_COUNT] = { 0 };
#define ICE_FD_FV_NOT_FOUND (-2)
	s8 first_free = ICE_FD_FV_NOT_FOUND;
	u8 used[ICE_MAX_FV_WORDS] = { 0 };
	s8 orig_free, si;
	u32 mask_sel = 0;
	u8 i, j, k;

	bitmap_zero(pair_list, ICE_FD_SRC_DST_PAIR_COUNT);

	/* This code assumes that the Flow Director field vectors are assigned
	 * from the end of the FV indexes working towards the zero index, that
	 * only complete fields will be included and will be consecutive, and
	 * that there are no gaps between valid indexes.
	 */

	/* Determine swap fields present */
	for (i = 0; i < hw->blk[ICE_BLK_FD].es.fvw; i++) {
		/* Find the first free entry, assuming right to left population.
		 * This is where we can start adding additional pairs if needed.
		 */
		if (first_free == ICE_FD_FV_NOT_FOUND && es[i].prot_id !=
		    ICE_PROT_INVALID)
			first_free = i - 1;

		for (j = 0; j < ICE_FD_SRC_DST_PAIR_COUNT; j++)
			if (es[i].prot_id == ice_fd_pairs[j].prot_id &&
			    es[i].off == ice_fd_pairs[j].off) {
				__set_bit(j, pair_list);
				pair_start[j] = i;
			}
	}

	orig_free = first_free;

	/* determine missing swap fields that need to be added */
	for (i = 0; i < ICE_FD_SRC_DST_PAIR_COUNT; i += 2) {
		u8 bit1 = test_bit(i + 1, pair_list);
		u8 bit0 = test_bit(i, pair_list);

		if (bit0 ^ bit1) {
			u8 index;

			/* add the appropriate 'paired' entry */
			if (!bit0)
				index = i;
			else
				index = i + 1;

			/* check for room */
			if (first_free + 1 < (s8)ice_fd_pairs[index].count)
				return -ENOSPC;

			/* place in extraction sequence */
			for (k = 0; k < ice_fd_pairs[index].count; k++) {
				es[first_free - k].prot_id =
					ice_fd_pairs[index].prot_id;
				es[first_free - k].off =
					ice_fd_pairs[index].off + (k * 2);

				if (k > first_free)
					return -EIO;

				/* keep track of non-relevant fields */
				mask_sel |= BIT(first_free - k);
			}

			pair_start[index] = first_free;
			first_free -= ice_fd_pairs[index].count;
		}
	}

	/* fill in the swap array */
	si = hw->blk[ICE_BLK_FD].es.fvw - 1;
	while (si >= 0) {
		u8 indexes_used = 1;

		/* assume flat at this index */
#define ICE_SWAP_VALID	0x80
		used[si] = si | ICE_SWAP_VALID;

		if (orig_free == ICE_FD_FV_NOT_FOUND || si <= orig_free) {
			si -= indexes_used;
			continue;
		}

		/* check for a swap location */
		for (j = 0; j < ICE_FD_SRC_DST_PAIR_COUNT; j++)
			if (es[si].prot_id == ice_fd_pairs[j].prot_id &&
			    es[si].off == ice_fd_pairs[j].off) {
				u8 idx;

				/* determine the appropriate matching field */
				idx = j + ((j % 2) ? -1 : 1);

				indexes_used = ice_fd_pairs[idx].count;
				for (k = 0; k < indexes_used; k++) {
					used[si - k] = (pair_start[idx] - k) |
						ICE_SWAP_VALID;
				}

				break;
			}

		si -= indexes_used;
	}

	/* for each set of 4 swap and 4 inset indexes, write the appropriate
	 * register
	 */
	for (j = 0; j < hw->blk[ICE_BLK_FD].es.fvw / 4; j++) {
		u32 raw_swap = 0;
		u32 raw_in = 0;

		for (k = 0; k < 4; k++) {
			u8 idx;

			idx = (j * 4) + k;
			if (used[idx] && !(mask_sel & BIT(idx))) {
				raw_swap |= used[idx] << (k * BITS_PER_BYTE);
#define ICE_INSET_DFLT 0x9f
				raw_in |= ICE_INSET_DFLT << (k * BITS_PER_BYTE);
			}
		}

		/* write the appropriate swap register set */
		wr32(hw, GLQF_FDSWAP(prof_id, j), raw_swap);

		ice_debug(hw, ICE_DBG_INIT, "swap wr(%d, %d): %x = %08x\n",
			  prof_id, j, GLQF_FDSWAP(prof_id, j), raw_swap);

		/* write the appropriate inset register set */
		wr32(hw, GLQF_FDINSET(prof_id, j), raw_in);

		ice_debug(hw, ICE_DBG_INIT, "inset wr(%d, %d): %x = %08x\n",
			  prof_id, j, GLQF_FDINSET(prof_id, j), raw_in);
	}

	/* initially clear the mask select for this profile */
	ice_update_fd_mask(hw, prof_id, 0);

	return 0;
}

/* The entries here needs to match the order of enum ice_ptype_attrib */
static const struct ice_ptype_attrib_info ice_ptype_attributes[] = {
	{ ICE_GTP_PDU_EH,	ICE_GTP_PDU_FLAG_MASK },
	{ ICE_GTP_SESSION,	ICE_GTP_FLAGS_MASK },
	{ ICE_GTP_DOWNLINK,	ICE_GTP_FLAGS_MASK },
	{ ICE_GTP_UPLINK,	ICE_GTP_FLAGS_MASK },
};

/**
 * ice_get_ptype_attrib_info - get PTYPE attribute information
 * @type: attribute type
 * @info: pointer to variable to the attribute information
 */
static void
ice_get_ptype_attrib_info(enum ice_ptype_attrib_type type,
			  struct ice_ptype_attrib_info *info)
{
	*info = ice_ptype_attributes[type];
}

/**
 * ice_add_prof_attrib - add any PTG with attributes to profile
 * @prof: pointer to the profile to which PTG entries will be added
 * @ptg: PTG to be added
 * @ptype: PTYPE that needs to be looked up
 * @attr: array of attributes that will be considered
 * @attr_cnt: number of elements in the attribute array
 */
static int
ice_add_prof_attrib(struct ice_prof_map *prof, u8 ptg, u16 ptype,
		    const struct ice_ptype_attributes *attr, u16 attr_cnt)
{
	bool found = false;
	u16 i;

	for (i = 0; i < attr_cnt; i++)
		if (attr[i].ptype == ptype) {
			found = true;

			prof->ptg[prof->ptg_cnt] = ptg;
			ice_get_ptype_attrib_info(attr[i].attrib,
						  &prof->attr[prof->ptg_cnt]);

			if (++prof->ptg_cnt >= ICE_MAX_PTG_PER_PROFILE)
				return -ENOSPC;
		}

	if (!found)
		return -ENOENT;

	return 0;
}

/**
 * ice_disable_fd_swap - set register appropriately to disable FD SWAP
 * @hw: pointer to the HW struct
 * @prof_id: profile ID
 */
static void
ice_disable_fd_swap(struct ice_hw *hw, u8 prof_id)
{
	u16 swap_val, fvw_num;
	unsigned int i;

	swap_val = ICE_SWAP_VALID;
	fvw_num = hw->blk[ICE_BLK_FD].es.fvw / ICE_FDIR_REG_SET_SIZE;

	/* Since the SWAP Flag in the Programming Desc doesn't work,
	 * here add method to disable the SWAP Option via setting
	 * certain SWAP and INSET register sets.
	 */
	for (i = 0; i < fvw_num ; i++) {
		u32 raw_swap, raw_in;
		unsigned int j;

		raw_swap = 0;
		raw_in = 0;

		for (j = 0; j < ICE_FDIR_REG_SET_SIZE; j++) {
			raw_swap |= (swap_val++) << (j * BITS_PER_BYTE);
			raw_in |= ICE_INSET_DFLT << (j * BITS_PER_BYTE);
		}

		/* write the FDIR swap register set */
		wr32(hw, GLQF_FDSWAP(prof_id, i), raw_swap);

		ice_debug(hw, ICE_DBG_INIT, "swap wr(%d, %d): 0x%x = 0x%08x\n",
			  prof_id, i, GLQF_FDSWAP(prof_id, i), raw_swap);

		/* write the FDIR inset register set */
		wr32(hw, GLQF_FDINSET(prof_id, i), raw_in);

		ice_debug(hw, ICE_DBG_INIT, "inset wr(%d, %d): 0x%x = 0x%08x\n",
			  prof_id, i, GLQF_FDINSET(prof_id, i), raw_in);
	}
}

/*
 * ice_add_prof - add profile
 * @hw: pointer to the HW struct
 * @blk: hardware block
 * @id: profile tracking ID
 * @ptypes: array of bitmaps indicating ptypes (ICE_FLOW_PTYPE_MAX bits)
 * @attr: array of attributes
 * @attr_cnt: number of elements in attr array
 * @es: extraction sequence (length of array is determined by the block)
 * @masks: mask for extraction sequence
 * @symm: symmetric setting for RSS profiles
 * @fd_swap: enable/disable FDIR paired src/dst fields swap option
 *
 * This function registers a profile, which matches a set of PTYPES with a
 * particular extraction sequence. While the hardware profile is allocated
 * it will not be written until the first call to ice_add_flow that specifies
 * the ID value used here.
 */
int
ice_add_prof(struct ice_hw *hw, enum ice_block blk, u64 id,
	     unsigned long *ptypes, const struct ice_ptype_attributes *attr,
	     u16 attr_cnt, struct ice_fv_word *es, u16 *masks, bool symm,
	     bool fd_swap)
{
	DECLARE_BITMAP(ptgs_used, ICE_XLT1_CNT);
	struct ice_prof_map *prof;
	int status;
	u8 prof_id;
	u16 ptype;

	bitmap_zero(ptgs_used, ICE_XLT1_CNT);

	mutex_lock(&hw->blk[blk].es.prof_map_lock);

	/* search for existing profile */
	status = ice_find_prof_id_with_mask(hw, blk, es, masks, symm, &prof_id);
	if (status) {
		/* allocate profile ID */
		status = ice_alloc_prof_id(hw, blk, &prof_id);
		if (status)
			goto err_ice_add_prof;
		if (blk == ICE_BLK_FD && fd_swap) {
			/* For Flow Director block, the extraction sequence may
			 * need to be altered in the case where there are paired
			 * fields that have no match. This is necessary because
			 * for Flow Director, src and dest fields need to paired
			 * for filter programming and these values are swapped
			 * during Tx.
			 */
			status = ice_update_fd_swap(hw, prof_id, es);
			if (status)
				goto err_ice_add_prof;
		} else if (blk == ICE_BLK_FD) {
			ice_disable_fd_swap(hw, prof_id);
		}
		status = ice_update_prof_masking(hw, blk, prof_id, masks);
		if (status)
			goto err_ice_add_prof;

		/* and write new es */
		ice_write_es(hw, blk, prof_id, es, symm);
	}

	ice_prof_inc_ref(hw, blk, prof_id);

	/* add profile info */
	prof = devm_kzalloc(ice_hw_to_dev(hw), sizeof(*prof), GFP_KERNEL);
	if (!prof) {
		status = -ENOMEM;
		goto err_ice_add_prof;
	}

	prof->profile_cookie = id;
	prof->prof_id = prof_id;
	prof->ptg_cnt = 0;
	prof->context = 0;

	/* build list of ptgs */
	for_each_set_bit(ptype, ptypes, ICE_FLOW_PTYPE_MAX) {
		u8 ptg;

		/* The package should place all ptypes in a non-zero
		 * PTG, so the following call should never fail.
		 */
		if (ice_ptg_find_ptype(hw, blk, ptype, &ptg))
			continue;

		/* If PTG is already added, skip and continue */
		if (test_bit(ptg, ptgs_used))
			continue;

		set_bit(ptg, ptgs_used);
		/* Check to see there are any attributes for this ptype, and
		 * add them if found.
		 */
		status = ice_add_prof_attrib(prof, ptg, ptype, attr, attr_cnt);
		if (status == -ENOSPC)
			break;
		if (status) {
			/* This is simple a ptype/PTG with no attribute */
			prof->ptg[prof->ptg_cnt] = ptg;
			prof->attr[prof->ptg_cnt].flags = 0;
			prof->attr[prof->ptg_cnt].mask = 0;

			if (++prof->ptg_cnt >= ICE_MAX_PTG_PER_PROFILE)
				break;
		}
	}

	list_add(&prof->list, &hw->blk[blk].es.prof_map);
	status = 0;

err_ice_add_prof:
	mutex_unlock(&hw->blk[blk].es.prof_map_lock);
	return status;
}

/**
 * ice_search_prof_id - Search for a profile tracking ID
 * @hw: pointer to the HW struct
 * @blk: hardware block
 * @id: profile tracking ID
 *
 * This will search for a profile tracking ID which was previously added.
 * The profile map lock should be held before calling this function.
 */
struct ice_prof_map *
ice_search_prof_id(struct ice_hw *hw, enum ice_block blk, u64 id)
{
	struct ice_prof_map *entry = NULL;
	struct ice_prof_map *map;

	list_for_each_entry(map, &hw->blk[blk].es.prof_map, list)
		if (map->profile_cookie == id) {
			entry = map;
			break;
		}

	return entry;
}

/**
 * ice_vsig_prof_id_count - count profiles in a VSIG
 * @hw: pointer to the HW struct
 * @blk: hardware block
 * @vsig: VSIG to remove the profile from
 */
static u16
ice_vsig_prof_id_count(struct ice_hw *hw, enum ice_block blk, u16 vsig)
{
	u16 idx = vsig & ICE_VSIG_IDX_M, count = 0;
	struct ice_vsig_prof *p;

	list_for_each_entry(p, &hw->blk[blk].xlt2.vsig_tbl[idx].prop_lst,
			    list)
		count++;

	return count;
}

/**
 * ice_rel_tcam_idx - release a TCAM index
 * @hw: pointer to the HW struct
 * @blk: hardware block
 * @idx: the index to release
 */
static int ice_rel_tcam_idx(struct ice_hw *hw, enum ice_block blk, u16 idx)
{
	/* Masks to invoke a never match entry */
	u8 vl_msk[ICE_TCAM_KEY_VAL_SZ] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
	u8 dc_msk[ICE_TCAM_KEY_VAL_SZ] = { 0xFE, 0xFF, 0xFF, 0xFF, 0xFF };
	u8 nm_msk[ICE_TCAM_KEY_VAL_SZ] = { 0x01, 0x00, 0x00, 0x00, 0x00 };
	int status;

	/* write the TCAM entry */
	status = ice_tcam_write_entry(hw, blk, idx, 0, 0, 0, 0, 0, vl_msk,
				      dc_msk, nm_msk);
	if (status)
		return status;

	/* release the TCAM entry */
	status = ice_free_tcam_ent(hw, blk, idx);

	return status;
}

/**
 * ice_rem_prof_id - remove one profile from a VSIG
 * @hw: pointer to the HW struct
 * @blk: hardware block
 * @prof: pointer to profile structure to remove
 */
static int
ice_rem_prof_id(struct ice_hw *hw, enum ice_block blk,
		struct ice_vsig_prof *prof)
{
	int status;
	u16 i;

	for (i = 0; i < prof->tcam_count; i++)
		if (prof->tcam[i].in_use) {
			prof->tcam[i].in_use = false;
			status = ice_rel_tcam_idx(hw, blk,
						  prof->tcam[i].tcam_idx);
			if (status)
				return -EIO;
		}

	return 0;
}

/**
 * ice_rem_vsig - remove VSIG
 * @hw: pointer to the HW struct
 * @blk: hardware block
 * @vsig: the VSIG to remove
 * @chg: the change list
 */
static int
ice_rem_vsig(struct ice_hw *hw, enum ice_block blk, u16 vsig,
	     struct list_head *chg)
{
	u16 idx = vsig & ICE_VSIG_IDX_M;
	struct ice_vsig_vsi *vsi_cur;
	struct ice_vsig_prof *d, *t;

	/* remove TCAM entries */
	list_for_each_entry_safe(d, t,
				 &hw->blk[blk].xlt2.vsig_tbl[idx].prop_lst,
				 list) {
		int status;

		status = ice_rem_prof_id(hw, blk, d);
		if (status)
			return status;

		list_del(&d->list);
		devm_kfree(ice_hw_to_dev(hw), d);
	}

	/* Move all VSIS associated with this VSIG to the default VSIG */
	vsi_cur = hw->blk[blk].xlt2.vsig_tbl[idx].first_vsi;
	/* If the VSIG has at least 1 VSI then iterate through the list
	 * and remove the VSIs before deleting the group.
	 */
	if (vsi_cur)
		do {
			struct ice_vsig_vsi *tmp = vsi_cur->next_vsi;
			struct ice_chs_chg *p;

			p = devm_kzalloc(ice_hw_to_dev(hw), sizeof(*p),
					 GFP_KERNEL);
			if (!p)
				return -ENOMEM;

			p->type = ICE_VSIG_REM;
			p->orig_vsig = vsig;
			p->vsig = ICE_DEFAULT_VSIG;
			p->vsi = vsi_cur - hw->blk[blk].xlt2.vsis;

			list_add(&p->list_entry, chg);

			vsi_cur = tmp;
		} while (vsi_cur);

	return ice_vsig_free(hw, blk, vsig);
}

/**
 * ice_rem_prof_id_vsig - remove a specific profile from a VSIG
 * @hw: pointer to the HW struct
 * @blk: hardware block
 * @vsig: VSIG to remove the profile from
 * @hdl: profile handle indicating which profile to remove
 * @chg: list to receive a record of changes
 */
static int
ice_rem_prof_id_vsig(struct ice_hw *hw, enum ice_block blk, u16 vsig, u64 hdl,
		     struct list_head *chg)
{
	u16 idx = vsig & ICE_VSIG_IDX_M;
	struct ice_vsig_prof *p, *t;

	list_for_each_entry_safe(p, t,
				 &hw->blk[blk].xlt2.vsig_tbl[idx].prop_lst,
				 list)
		if (p->profile_cookie == hdl) {
			int status;

			if (ice_vsig_prof_id_count(hw, blk, vsig) == 1)
				/* this is the last profile, remove the VSIG */
				return ice_rem_vsig(hw, blk, vsig, chg);

			status = ice_rem_prof_id(hw, blk, p);
			if (!status) {
				list_del(&p->list);
				devm_kfree(ice_hw_to_dev(hw), p);
			}
			return status;
		}

	return -ENOENT;
}

/**
 * ice_rem_flow_all - remove all flows with a particular profile
 * @hw: pointer to the HW struct
 * @blk: hardware block
 * @id: profile tracking ID
 */
static int ice_rem_flow_all(struct ice_hw *hw, enum ice_block blk, u64 id)
{
	struct ice_chs_chg *del, *tmp;
	struct list_head chg;
	int status;
	u16 i;

	INIT_LIST_HEAD(&chg);

	for (i = 1; i < ICE_MAX_VSIGS; i++)
		if (hw->blk[blk].xlt2.vsig_tbl[i].in_use) {
			if (ice_has_prof_vsig(hw, blk, i, id)) {
				status = ice_rem_prof_id_vsig(hw, blk, i, id,
							      &chg);
				if (status)
					goto err_ice_rem_flow_all;
			}
		}

	status = ice_upd_prof_hw(hw, blk, &chg);

err_ice_rem_flow_all:
	list_for_each_entry_safe(del, tmp, &chg, list_entry) {
		list_del(&del->list_entry);
		devm_kfree(ice_hw_to_dev(hw), del);
	}

	return status;
}

/**
 * ice_rem_prof - remove profile
 * @hw: pointer to the HW struct
 * @blk: hardware block
 * @id: profile tracking ID
 *
 * This will remove the profile specified by the ID parameter, which was
 * previously created through ice_add_prof. If any existing entries
 * are associated with this profile, they will be removed as well.
 */
int ice_rem_prof(struct ice_hw *hw, enum ice_block blk, u64 id)
{
	struct ice_prof_map *pmap;
	int status;

	mutex_lock(&hw->blk[blk].es.prof_map_lock);

	pmap = ice_search_prof_id(hw, blk, id);
	if (!pmap) {
		status = -ENOENT;
		goto err_ice_rem_prof;
	}

	/* remove all flows with this profile */
	status = ice_rem_flow_all(hw, blk, pmap->profile_cookie);
	if (status)
		goto err_ice_rem_prof;

	/* dereference profile, and possibly remove */
	ice_prof_dec_ref(hw, blk, pmap->prof_id);

	list_del(&pmap->list);
	devm_kfree(ice_hw_to_dev(hw), pmap);

err_ice_rem_prof:
	mutex_unlock(&hw->blk[blk].es.prof_map_lock);
	return status;
}

/**
 * ice_get_prof - get profile
 * @hw: pointer to the HW struct
 * @blk: hardware block
 * @hdl: profile handle
 * @chg: change list
 */
static int
ice_get_prof(struct ice_hw *hw, enum ice_block blk, u64 hdl,
	     struct list_head *chg)
{
	struct ice_prof_map *map;
	struct ice_chs_chg *p;
	int status = 0;
	u16 i;

	mutex_lock(&hw->blk[blk].es.prof_map_lock);
	/* Get the details on the profile specified by the handle ID */
	map = ice_search_prof_id(hw, blk, hdl);
	if (!map) {
		status = -ENOENT;
		goto err_ice_get_prof;
	}

	for (i = 0; i < map->ptg_cnt; i++)
		if (!hw->blk[blk].es.written[map->prof_id]) {
			/* add ES to change list */
			p = devm_kzalloc(ice_hw_to_dev(hw), sizeof(*p),
					 GFP_KERNEL);
			if (!p) {
				status = -ENOMEM;
				goto err_ice_get_prof;
			}

			p->type = ICE_PTG_ES_ADD;
			p->ptype = 0;
			p->ptg = map->ptg[i];
			p->add_ptg = 0;

			p->add_prof = 1;
			p->prof_id = map->prof_id;

			hw->blk[blk].es.written[map->prof_id] = true;

			list_add(&p->list_entry, chg);
		}

err_ice_get_prof:
	mutex_unlock(&hw->blk[blk].es.prof_map_lock);
	/* let caller clean up the change list */
	return status;
}

/**
 * ice_get_profs_vsig - get a copy of the list of profiles from a VSIG
 * @hw: pointer to the HW struct
 * @blk: hardware block
 * @vsig: VSIG from which to copy the list
 * @lst: output list
 *
 * This routine makes a copy of the list of profiles in the specified VSIG.
 */
static int
ice_get_profs_vsig(struct ice_hw *hw, enum ice_block blk, u16 vsig,
		   struct list_head *lst)
{
	struct ice_vsig_prof *ent1, *ent2;
	u16 idx = vsig & ICE_VSIG_IDX_M;

	list_for_each_entry(ent1, &hw->blk[blk].xlt2.vsig_tbl[idx].prop_lst,
			    list) {
		struct ice_vsig_prof *p;

		/* copy to the input list */
		p = devm_kmemdup(ice_hw_to_dev(hw), ent1, sizeof(*p),
				 GFP_KERNEL);
		if (!p)
			goto err_ice_get_profs_vsig;

		list_add_tail(&p->list, lst);
	}

	return 0;

err_ice_get_profs_vsig:
	list_for_each_entry_safe(ent1, ent2, lst, list) {
		list_del(&ent1->list);
		devm_kfree(ice_hw_to_dev(hw), ent1);
	}

	return -ENOMEM;
}

/**
 * ice_add_prof_to_lst - add profile entry to a list
 * @hw: pointer to the HW struct
 * @blk: hardware block
 * @lst: the list to be added to
 * @hdl: profile handle of entry to add
 */
static int
ice_add_prof_to_lst(struct ice_hw *hw, enum ice_block blk,
		    struct list_head *lst, u64 hdl)
{
	struct ice_prof_map *map;
	struct ice_vsig_prof *p;
	int status = 0;
	u16 i;

	mutex_lock(&hw->blk[blk].es.prof_map_lock);
	map = ice_search_prof_id(hw, blk, hdl);
	if (!map) {
		status = -ENOENT;
		goto err_ice_add_prof_to_lst;
	}

	p = devm_kzalloc(ice_hw_to_dev(hw), sizeof(*p), GFP_KERNEL);
	if (!p) {
		status = -ENOMEM;
		goto err_ice_add_prof_to_lst;
	}

	p->profile_cookie = map->profile_cookie;
	p->prof_id = map->prof_id;
	p->tcam_count = map->ptg_cnt;

	for (i = 0; i < map->ptg_cnt; i++) {
		p->tcam[i].prof_id = map->prof_id;
		p->tcam[i].tcam_idx = ICE_INVALID_TCAM;
		p->tcam[i].ptg = map->ptg[i];
	}

	list_add(&p->list, lst);

err_ice_add_prof_to_lst:
	mutex_unlock(&hw->blk[blk].es.prof_map_lock);
	return status;
}

/**
 * ice_move_vsi - move VSI to another VSIG
 * @hw: pointer to the HW struct
 * @blk: hardware block
 * @vsi: the VSI to move
 * @vsig: the VSIG to move the VSI to
 * @chg: the change list
 */
static int
ice_move_vsi(struct ice_hw *hw, enum ice_block blk, u16 vsi, u16 vsig,
	     struct list_head *chg)
{
	struct ice_chs_chg *p;
	u16 orig_vsig;
	int status;

	p = devm_kzalloc(ice_hw_to_dev(hw), sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	status = ice_vsig_find_vsi(hw, blk, vsi, &orig_vsig);
	if (!status)
		status = ice_vsig_add_mv_vsi(hw, blk, vsi, vsig);

	if (status) {
		devm_kfree(ice_hw_to_dev(hw), p);
		return status;
	}

	p->type = ICE_VSI_MOVE;
	p->vsi = vsi;
	p->orig_vsig = orig_vsig;
	p->vsig = vsig;

	list_add(&p->list_entry, chg);

	return 0;
}

/**
 * ice_rem_chg_tcam_ent - remove a specific TCAM entry from change list
 * @hw: pointer to the HW struct
 * @idx: the index of the TCAM entry to remove
 * @chg: the list of change structures to search
 */
static void
ice_rem_chg_tcam_ent(struct ice_hw *hw, u16 idx, struct list_head *chg)
{
	struct ice_chs_chg *pos, *tmp;

	list_for_each_entry_safe(tmp, pos, chg, list_entry)
		if (tmp->type == ICE_TCAM_ADD && tmp->tcam_idx == idx) {
			list_del(&tmp->list_entry);
			devm_kfree(ice_hw_to_dev(hw), tmp);
		}
}

/**
 * ice_prof_tcam_ena_dis - add enable or disable TCAM change
 * @hw: pointer to the HW struct
 * @blk: hardware block
 * @enable: true to enable, false to disable
 * @vsig: the VSIG of the TCAM entry
 * @tcam: pointer the TCAM info structure of the TCAM to disable
 * @chg: the change list
 *
 * This function appends an enable or disable TCAM entry in the change log
 */
static int
ice_prof_tcam_ena_dis(struct ice_hw *hw, enum ice_block blk, bool enable,
		      u16 vsig, struct ice_tcam_inf *tcam,
		      struct list_head *chg)
{
	struct ice_chs_chg *p;
	int status;

	u8 vl_msk[ICE_TCAM_KEY_VAL_SZ] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
	u8 dc_msk[ICE_TCAM_KEY_VAL_SZ] = { 0xFF, 0xFF, 0x00, 0x00, 0x00 };
	u8 nm_msk[ICE_TCAM_KEY_VAL_SZ] = { 0x00, 0x00, 0x00, 0x00, 0x00 };

	/* if disabling, free the TCAM */
	if (!enable) {
		status = ice_rel_tcam_idx(hw, blk, tcam->tcam_idx);

		/* if we have already created a change for this TCAM entry, then
		 * we need to remove that entry, in order to prevent writing to
		 * a TCAM entry we no longer will have ownership of.
		 */
		ice_rem_chg_tcam_ent(hw, tcam->tcam_idx, chg);
		tcam->tcam_idx = 0;
		tcam->in_use = 0;
		return status;
	}

	/* for re-enabling, reallocate a TCAM */
	/* for entries with empty attribute masks, allocate entry from
	 * the bottom of the TCAM table; otherwise, allocate from the
	 * top of the table in order to give it higher priority
	 */
	status = ice_alloc_tcam_ent(hw, blk, tcam->attr.mask == 0,
				    &tcam->tcam_idx);
	if (status)
		return status;

	/* add TCAM to change list */
	p = devm_kzalloc(ice_hw_to_dev(hw), sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	status = ice_tcam_write_entry(hw, blk, tcam->tcam_idx, tcam->prof_id,
				      tcam->ptg, vsig, 0, tcam->attr.flags,
				      vl_msk, dc_msk, nm_msk);
	if (status)
		goto err_ice_prof_tcam_ena_dis;

	tcam->in_use = 1;

	p->type = ICE_TCAM_ADD;
	p->add_tcam_idx = true;
	p->prof_id = tcam->prof_id;
	p->ptg = tcam->ptg;
	p->vsig = 0;
	p->tcam_idx = tcam->tcam_idx;

	/* log change */
	list_add(&p->list_entry, chg);

	return 0;

err_ice_prof_tcam_ena_dis:
	devm_kfree(ice_hw_to_dev(hw), p);
	return status;
}

/**
 * ice_adj_prof_priorities - adjust profile based on priorities
 * @hw: pointer to the HW struct
 * @blk: hardware block
 * @vsig: the VSIG for which to adjust profile priorities
 * @chg: the change list
 */
static int
ice_adj_prof_priorities(struct ice_hw *hw, enum ice_block blk, u16 vsig,
			struct list_head *chg)
{
	DECLARE_BITMAP(ptgs_used, ICE_XLT1_CNT);
	struct ice_vsig_prof *t;
	int status;
	u16 idx;

	bitmap_zero(ptgs_used, ICE_XLT1_CNT);
	idx = vsig & ICE_VSIG_IDX_M;

	/* Priority is based on the order in which the profiles are added. The
	 * newest added profile has highest priority and the oldest added
	 * profile has the lowest priority. Since the profile property list for
	 * a VSIG is sorted from newest to oldest, this code traverses the list
	 * in order and enables the first of each PTG that it finds (that is not
	 * already enabled); it also disables any duplicate PTGs that it finds
	 * in the older profiles (that are currently enabled).
	 */

	list_for_each_entry(t, &hw->blk[blk].xlt2.vsig_tbl[idx].prop_lst,
			    list) {
		u16 i;

		for (i = 0; i < t->tcam_count; i++) {
			/* Scan the priorities from newest to oldest.
			 * Make sure that the newest profiles take priority.
			 */
			if (test_bit(t->tcam[i].ptg, ptgs_used) &&
			    t->tcam[i].in_use) {
				/* need to mark this PTG as never match, as it
				 * was already in use and therefore duplicate
				 * (and lower priority)
				 */
				status = ice_prof_tcam_ena_dis(hw, blk, false,
							       vsig,
							       &t->tcam[i],
							       chg);
				if (status)
					return status;
			} else if (!test_bit(t->tcam[i].ptg, ptgs_used) &&
				   !t->tcam[i].in_use) {
				/* need to enable this PTG, as it in not in use
				 * and not enabled (highest priority)
				 */
				status = ice_prof_tcam_ena_dis(hw, blk, true,
							       vsig,
							       &t->tcam[i],
							       chg);
				if (status)
					return status;
			}

			/* keep track of used ptgs */
			__set_bit(t->tcam[i].ptg, ptgs_used);
		}
	}

	return 0;
}

/**
 * ice_add_prof_id_vsig - add profile to VSIG
 * @hw: pointer to the HW struct
 * @blk: hardware block
 * @vsig: the VSIG to which this profile is to be added
 * @hdl: the profile handle indicating the profile to add
 * @rev: true to add entries to the end of the list
 * @chg: the change list
 */
static int
ice_add_prof_id_vsig(struct ice_hw *hw, enum ice_block blk, u16 vsig, u64 hdl,
		     bool rev, struct list_head *chg)
{
	/* Masks that ignore flags */
	u8 vl_msk[ICE_TCAM_KEY_VAL_SZ] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
	u8 dc_msk[ICE_TCAM_KEY_VAL_SZ] = { 0xFF, 0xFF, 0x00, 0x00, 0x00 };
	u8 nm_msk[ICE_TCAM_KEY_VAL_SZ] = { 0x00, 0x00, 0x00, 0x00, 0x00 };
	struct ice_prof_map *map;
	struct ice_vsig_prof *t;
	struct ice_chs_chg *p;
	u16 vsig_idx, i;
	int status = 0;

	/* Error, if this VSIG already has this profile */
	if (ice_has_prof_vsig(hw, blk, vsig, hdl))
		return -EEXIST;

	/* new VSIG profile structure */
	t = devm_kzalloc(ice_hw_to_dev(hw), sizeof(*t), GFP_KERNEL);
	if (!t)
		return -ENOMEM;

	mutex_lock(&hw->blk[blk].es.prof_map_lock);
	/* Get the details on the profile specified by the handle ID */
	map = ice_search_prof_id(hw, blk, hdl);
	if (!map) {
		status = -ENOENT;
		goto err_ice_add_prof_id_vsig;
	}

	t->profile_cookie = map->profile_cookie;
	t->prof_id = map->prof_id;
	t->tcam_count = map->ptg_cnt;

	/* create TCAM entries */
	for (i = 0; i < map->ptg_cnt; i++) {
		u16 tcam_idx;

		/* add TCAM to change list */
		p = devm_kzalloc(ice_hw_to_dev(hw), sizeof(*p), GFP_KERNEL);
		if (!p) {
			status = -ENOMEM;
			goto err_ice_add_prof_id_vsig;
		}

		/* allocate the TCAM entry index */
		/* for entries with empty attribute masks, allocate entry from
		 * the bottom of the TCAM table; otherwise, allocate from the
		 * top of the table in order to give it higher priority
		 */
		status = ice_alloc_tcam_ent(hw, blk, map->attr[i].mask == 0,
					    &tcam_idx);
		if (status) {
			devm_kfree(ice_hw_to_dev(hw), p);
			goto err_ice_add_prof_id_vsig;
		}

		t->tcam[i].ptg = map->ptg[i];
		t->tcam[i].prof_id = map->prof_id;
		t->tcam[i].tcam_idx = tcam_idx;
		t->tcam[i].attr = map->attr[i];
		t->tcam[i].in_use = true;

		p->type = ICE_TCAM_ADD;
		p->add_tcam_idx = true;
		p->prof_id = t->tcam[i].prof_id;
		p->ptg = t->tcam[i].ptg;
		p->vsig = vsig;
		p->tcam_idx = t->tcam[i].tcam_idx;

		/* write the TCAM entry */
		status = ice_tcam_write_entry(hw, blk, t->tcam[i].tcam_idx,
					      t->tcam[i].prof_id,
					      t->tcam[i].ptg, vsig, 0, 0,
					      vl_msk, dc_msk, nm_msk);
		if (status) {
			devm_kfree(ice_hw_to_dev(hw), p);
			goto err_ice_add_prof_id_vsig;
		}

		/* log change */
		list_add(&p->list_entry, chg);
	}

	/* add profile to VSIG */
	vsig_idx = vsig & ICE_VSIG_IDX_M;
	if (rev)
		list_add_tail(&t->list,
			      &hw->blk[blk].xlt2.vsig_tbl[vsig_idx].prop_lst);
	else
		list_add(&t->list,
			 &hw->blk[blk].xlt2.vsig_tbl[vsig_idx].prop_lst);

	mutex_unlock(&hw->blk[blk].es.prof_map_lock);
	return status;

err_ice_add_prof_id_vsig:
	mutex_unlock(&hw->blk[blk].es.prof_map_lock);
	/* let caller clean up the change list */
	devm_kfree(ice_hw_to_dev(hw), t);
	return status;
}

/**
 * ice_create_prof_id_vsig - add a new VSIG with a single profile
 * @hw: pointer to the HW struct
 * @blk: hardware block
 * @vsi: the initial VSI that will be in VSIG
 * @hdl: the profile handle of the profile that will be added to the VSIG
 * @chg: the change list
 */
static int
ice_create_prof_id_vsig(struct ice_hw *hw, enum ice_block blk, u16 vsi, u64 hdl,
			struct list_head *chg)
{
	struct ice_chs_chg *p;
	u16 new_vsig;
	int status;

	p = devm_kzalloc(ice_hw_to_dev(hw), sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	new_vsig = ice_vsig_alloc(hw, blk);
	if (!new_vsig) {
		status = -EIO;
		goto err_ice_create_prof_id_vsig;
	}

	status = ice_move_vsi(hw, blk, vsi, new_vsig, chg);
	if (status)
		goto err_ice_create_prof_id_vsig;

	status = ice_add_prof_id_vsig(hw, blk, new_vsig, hdl, false, chg);
	if (status)
		goto err_ice_create_prof_id_vsig;

	p->type = ICE_VSIG_ADD;
	p->vsi = vsi;
	p->orig_vsig = ICE_DEFAULT_VSIG;
	p->vsig = new_vsig;

	list_add(&p->list_entry, chg);

	return 0;

err_ice_create_prof_id_vsig:
	/* let caller clean up the change list */
	devm_kfree(ice_hw_to_dev(hw), p);
	return status;
}

/**
 * ice_create_vsig_from_lst - create a new VSIG with a list of profiles
 * @hw: pointer to the HW struct
 * @blk: hardware block
 * @vsi: the initial VSI that will be in VSIG
 * @lst: the list of profile that will be added to the VSIG
 * @new_vsig: return of new VSIG
 * @chg: the change list
 */
static int
ice_create_vsig_from_lst(struct ice_hw *hw, enum ice_block blk, u16 vsi,
			 struct list_head *lst, u16 *new_vsig,
			 struct list_head *chg)
{
	struct ice_vsig_prof *t;
	int status;
	u16 vsig;

	vsig = ice_vsig_alloc(hw, blk);
	if (!vsig)
		return -EIO;

	status = ice_move_vsi(hw, blk, vsi, vsig, chg);
	if (status)
		return status;

	list_for_each_entry(t, lst, list) {
		/* Reverse the order here since we are copying the list */
		status = ice_add_prof_id_vsig(hw, blk, vsig, t->profile_cookie,
					      true, chg);
		if (status)
			return status;
	}

	*new_vsig = vsig;

	return 0;
}

/**
 * ice_find_prof_vsig - find a VSIG with a specific profile handle
 * @hw: pointer to the HW struct
 * @blk: hardware block
 * @hdl: the profile handle of the profile to search for
 * @vsig: returns the VSIG with the matching profile
 */
static bool
ice_find_prof_vsig(struct ice_hw *hw, enum ice_block blk, u64 hdl, u16 *vsig)
{
	struct ice_vsig_prof *t;
	struct list_head lst;
	int status;

	INIT_LIST_HEAD(&lst);

	t = kzalloc(sizeof(*t), GFP_KERNEL);
	if (!t)
		return false;

	t->profile_cookie = hdl;
	list_add(&t->list, &lst);

	status = ice_find_dup_props_vsig(hw, blk, &lst, vsig);

	list_del(&t->list);
	kfree(t);

	return !status;
}

/**
 * ice_add_prof_id_flow - add profile flow
 * @hw: pointer to the HW struct
 * @blk: hardware block
 * @vsi: the VSI to enable with the profile specified by ID
 * @hdl: profile handle
 *
 * Calling this function will update the hardware tables to enable the
 * profile indicated by the ID parameter for the VSIs specified in the VSI
 * array. Once successfully called, the flow will be enabled.
 */
int
ice_add_prof_id_flow(struct ice_hw *hw, enum ice_block blk, u16 vsi, u64 hdl)
{
	struct ice_vsig_prof *tmp1, *del1;
	struct ice_chs_chg *tmp, *del;
	struct list_head union_lst;
	struct list_head chg;
	int status;
	u16 vsig;

	INIT_LIST_HEAD(&union_lst);
	INIT_LIST_HEAD(&chg);

	/* Get profile */
	status = ice_get_prof(hw, blk, hdl, &chg);
	if (status)
		return status;

	/* determine if VSI is already part of a VSIG */
	status = ice_vsig_find_vsi(hw, blk, vsi, &vsig);
	if (!status && vsig) {
		bool only_vsi;
		u16 or_vsig;
		u16 ref;

		/* found in VSIG */
		or_vsig = vsig;

		/* make sure that there is no overlap/conflict between the new
		 * characteristics and the existing ones; we don't support that
		 * scenario
		 */
		if (ice_has_prof_vsig(hw, blk, vsig, hdl)) {
			status = -EEXIST;
			goto err_ice_add_prof_id_flow;
		}

		/* last VSI in the VSIG? */
		status = ice_vsig_get_ref(hw, blk, vsig, &ref);
		if (status)
			goto err_ice_add_prof_id_flow;
		only_vsi = (ref == 1);

		/* create a union of the current profiles and the one being
		 * added
		 */
		status = ice_get_profs_vsig(hw, blk, vsig, &union_lst);
		if (status)
			goto err_ice_add_prof_id_flow;

		status = ice_add_prof_to_lst(hw, blk, &union_lst, hdl);
		if (status)
			goto err_ice_add_prof_id_flow;

		/* search for an existing VSIG with an exact charc match */
		status = ice_find_dup_props_vsig(hw, blk, &union_lst, &vsig);
		if (!status) {
			/* move VSI to the VSIG that matches */
			status = ice_move_vsi(hw, blk, vsi, vsig, &chg);
			if (status)
				goto err_ice_add_prof_id_flow;

			/* VSI has been moved out of or_vsig. If the or_vsig had
			 * only that VSI it is now empty and can be removed.
			 */
			if (only_vsi) {
				status = ice_rem_vsig(hw, blk, or_vsig, &chg);
				if (status)
					goto err_ice_add_prof_id_flow;
			}
		} else if (only_vsi) {
			/* If the original VSIG only contains one VSI, then it
			 * will be the requesting VSI. In this case the VSI is
			 * not sharing entries and we can simply add the new
			 * profile to the VSIG.
			 */
			status = ice_add_prof_id_vsig(hw, blk, vsig, hdl, false,
						      &chg);
			if (status)
				goto err_ice_add_prof_id_flow;

			/* Adjust priorities */
			status = ice_adj_prof_priorities(hw, blk, vsig, &chg);
			if (status)
				goto err_ice_add_prof_id_flow;
		} else {
			/* No match, so we need a new VSIG */
			status = ice_create_vsig_from_lst(hw, blk, vsi,
							  &union_lst, &vsig,
							  &chg);
			if (status)
				goto err_ice_add_prof_id_flow;

			/* Adjust priorities */
			status = ice_adj_prof_priorities(hw, blk, vsig, &chg);
			if (status)
				goto err_ice_add_prof_id_flow;
		}
	} else {
		/* need to find or add a VSIG */
		/* search for an existing VSIG with an exact charc match */
		if (ice_find_prof_vsig(hw, blk, hdl, &vsig)) {
			/* found an exact match */
			/* add or move VSI to the VSIG that matches */
			status = ice_move_vsi(hw, blk, vsi, vsig, &chg);
			if (status)
				goto err_ice_add_prof_id_flow;
		} else {
			/* we did not find an exact match */
			/* we need to add a VSIG */
			status = ice_create_prof_id_vsig(hw, blk, vsi, hdl,
							 &chg);
			if (status)
				goto err_ice_add_prof_id_flow;
		}
	}

	/* update hardware */
	if (!status)
		status = ice_upd_prof_hw(hw, blk, &chg);

err_ice_add_prof_id_flow:
	list_for_each_entry_safe(del, tmp, &chg, list_entry) {
		list_del(&del->list_entry);
		devm_kfree(ice_hw_to_dev(hw), del);
	}

	list_for_each_entry_safe(del1, tmp1, &union_lst, list) {
		list_del(&del1->list);
		devm_kfree(ice_hw_to_dev(hw), del1);
	}

	return status;
}

/**
 * ice_flow_assoc_fdir_prof - add an FDIR profile for main/ctrl VSI
 * @hw: pointer to the HW struct
 * @blk: HW block
 * @dest_vsi: dest VSI
 * @fdir_vsi: fdir programming VSI
 * @hdl: profile handle
 *
 * Update the hardware tables to enable the FDIR profile indicated by @hdl for
 * the VSI specified by @dest_vsi. On success, the flow will be enabled.
 *
 * Return: 0 on success or negative errno on failure.
 */
int
ice_flow_assoc_fdir_prof(struct ice_hw *hw, enum ice_block blk,
			 u16 dest_vsi, u16 fdir_vsi, u64 hdl)
{
	u16 vsi_num;
	int status;

	if (blk != ICE_BLK_FD)
		return -EINVAL;

	vsi_num = ice_get_hw_vsi_num(hw, dest_vsi);
	status = ice_add_prof_id_flow(hw, blk, vsi_num, hdl);
	if (status) {
		ice_debug(hw, ICE_DBG_FLOW, "Adding HW profile failed for main VSI flow entry: %d\n",
			  status);
		return status;
	}

	vsi_num = ice_get_hw_vsi_num(hw, fdir_vsi);
	status = ice_add_prof_id_flow(hw, blk, vsi_num, hdl);
	if (status) {
		ice_debug(hw, ICE_DBG_FLOW, "Adding HW profile failed for ctrl VSI flow entry: %d\n",
			  status);
		goto err;
	}

	return 0;

err:
	vsi_num = ice_get_hw_vsi_num(hw, dest_vsi);
	ice_rem_prof_id_flow(hw, blk, vsi_num, hdl);

	return status;
}

/**
 * ice_rem_prof_from_list - remove a profile from list
 * @hw: pointer to the HW struct
 * @lst: list to remove the profile from
 * @hdl: the profile handle indicating the profile to remove
 */
static int
ice_rem_prof_from_list(struct ice_hw *hw, struct list_head *lst, u64 hdl)
{
	struct ice_vsig_prof *ent, *tmp;

	list_for_each_entry_safe(ent, tmp, lst, list)
		if (ent->profile_cookie == hdl) {
			list_del(&ent->list);
			devm_kfree(ice_hw_to_dev(hw), ent);
			return 0;
		}

	return -ENOENT;
}

/**
 * ice_rem_prof_id_flow - remove flow
 * @hw: pointer to the HW struct
 * @blk: hardware block
 * @vsi: the VSI from which to remove the profile specified by ID
 * @hdl: profile tracking handle
 *
 * Calling this function will update the hardware tables to remove the
 * profile indicated by the ID parameter for the VSIs specified in the VSI
 * array. Once successfully called, the flow will be disabled.
 */
int
ice_rem_prof_id_flow(struct ice_hw *hw, enum ice_block blk, u16 vsi, u64 hdl)
{
	struct ice_vsig_prof *tmp1, *del1;
	struct ice_chs_chg *tmp, *del;
	struct list_head chg, copy;
	int status;
	u16 vsig;

	INIT_LIST_HEAD(&copy);
	INIT_LIST_HEAD(&chg);

	/* determine if VSI is already part of a VSIG */
	status = ice_vsig_find_vsi(hw, blk, vsi, &vsig);
	if (!status && vsig) {
		bool last_profile;
		bool only_vsi;
		u16 ref;

		/* found in VSIG */
		last_profile = ice_vsig_prof_id_count(hw, blk, vsig) == 1;
		status = ice_vsig_get_ref(hw, blk, vsig, &ref);
		if (status)
			goto err_ice_rem_prof_id_flow;
		only_vsi = (ref == 1);

		if (only_vsi) {
			/* If the original VSIG only contains one reference,
			 * which will be the requesting VSI, then the VSI is not
			 * sharing entries and we can simply remove the specific
			 * characteristics from the VSIG.
			 */

			if (last_profile) {
				/* If there are no profiles left for this VSIG,
				 * then simply remove the VSIG.
				 */
				status = ice_rem_vsig(hw, blk, vsig, &chg);
				if (status)
					goto err_ice_rem_prof_id_flow;
			} else {
				status = ice_rem_prof_id_vsig(hw, blk, vsig,
							      hdl, &chg);
				if (status)
					goto err_ice_rem_prof_id_flow;

				/* Adjust priorities */
				status = ice_adj_prof_priorities(hw, blk, vsig,
								 &chg);
				if (status)
					goto err_ice_rem_prof_id_flow;
			}

		} else {
			/* Make a copy of the VSIG's list of Profiles */
			status = ice_get_profs_vsig(hw, blk, vsig, &copy);
			if (status)
				goto err_ice_rem_prof_id_flow;

			/* Remove specified profile entry from the list */
			status = ice_rem_prof_from_list(hw, &copy, hdl);
			if (status)
				goto err_ice_rem_prof_id_flow;

			if (list_empty(&copy)) {
				status = ice_move_vsi(hw, blk, vsi,
						      ICE_DEFAULT_VSIG, &chg);
				if (status)
					goto err_ice_rem_prof_id_flow;

			} else if (!ice_find_dup_props_vsig(hw, blk, &copy,
							    &vsig)) {
				/* found an exact match */
				/* add or move VSI to the VSIG that matches */
				/* Search for a VSIG with a matching profile
				 * list
				 */

				/* Found match, move VSI to the matching VSIG */
				status = ice_move_vsi(hw, blk, vsi, vsig, &chg);
				if (status)
					goto err_ice_rem_prof_id_flow;
			} else {
				/* since no existing VSIG supports this
				 * characteristic pattern, we need to create a
				 * new VSIG and TCAM entries
				 */
				status = ice_create_vsig_from_lst(hw, blk, vsi,
								  &copy, &vsig,
								  &chg);
				if (status)
					goto err_ice_rem_prof_id_flow;

				/* Adjust priorities */
				status = ice_adj_prof_priorities(hw, blk, vsig,
								 &chg);
				if (status)
					goto err_ice_rem_prof_id_flow;
			}
		}
	} else {
		status = -ENOENT;
	}

	/* update hardware tables */
	if (!status)
		status = ice_upd_prof_hw(hw, blk, &chg);

err_ice_rem_prof_id_flow:
	list_for_each_entry_safe(del, tmp, &chg, list_entry) {
		list_del(&del->list_entry);
		devm_kfree(ice_hw_to_dev(hw), del);
	}

	list_for_each_entry_safe(del1, tmp1, &copy, list) {
		list_del(&del1->list);
		devm_kfree(ice_hw_to_dev(hw), del1);
	}

	return status;
}
