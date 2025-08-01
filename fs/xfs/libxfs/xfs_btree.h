/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2000-2001,2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
#ifndef __XFS_BTREE_H__
#define	__XFS_BTREE_H__

struct xfs_buf;
struct xfs_inode;
struct xfs_mount;
struct xfs_trans;
struct xfs_ifork;
struct xfs_perag;

/*
 * Generic key, ptr and record wrapper structures.
 *
 * These are disk format structures, and are converted where necessary
 * by the btree specific code that needs to interpret them.
 */
union xfs_btree_ptr {
	__be32			s;	/* short form ptr */
	__be64			l;	/* long form ptr */
};

/*
 * The in-core btree key.  Overlapping btrees actually store two keys
 * per pointer, so we reserve enough memory to hold both.  The __*bigkey
 * items should never be accessed directly.
 */
union xfs_btree_key {
	struct xfs_bmbt_key		bmbt;
	xfs_bmdr_key_t			bmbr;	/* bmbt root block */
	xfs_alloc_key_t			alloc;
	struct xfs_inobt_key		inobt;
	struct xfs_rmap_key		rmap;
	struct xfs_rmap_key		__rmap_bigkey[2];
	struct xfs_refcount_key		refc;
};

union xfs_btree_rec {
	struct xfs_bmbt_rec		bmbt;
	xfs_bmdr_rec_t			bmbr;	/* bmbt root block */
	struct xfs_alloc_rec		alloc;
	struct xfs_inobt_rec		inobt;
	struct xfs_rmap_rec		rmap;
	struct xfs_refcount_rec		refc;
};

/*
 * This nonsense is to make -wlint happy.
 */
#define	XFS_LOOKUP_EQ	((xfs_lookup_t)XFS_LOOKUP_EQi)
#define	XFS_LOOKUP_LE	((xfs_lookup_t)XFS_LOOKUP_LEi)
#define	XFS_LOOKUP_GE	((xfs_lookup_t)XFS_LOOKUP_GEi)

struct xfs_btree_ops;
uint32_t xfs_btree_magic(struct xfs_mount *mp, const struct xfs_btree_ops *ops);

/*
 * For logging record fields.
 */
#define	XFS_BB_MAGIC		(1u << 0)
#define	XFS_BB_LEVEL		(1u << 1)
#define	XFS_BB_NUMRECS		(1u << 2)
#define	XFS_BB_LEFTSIB		(1u << 3)
#define	XFS_BB_RIGHTSIB		(1u << 4)
#define	XFS_BB_BLKNO		(1u << 5)
#define	XFS_BB_LSN		(1u << 6)
#define	XFS_BB_UUID		(1u << 7)
#define	XFS_BB_OWNER		(1u << 8)
#define	XFS_BB_NUM_BITS		5
#define	XFS_BB_ALL_BITS		((1u << XFS_BB_NUM_BITS) - 1)
#define	XFS_BB_NUM_BITS_CRC	9
#define	XFS_BB_ALL_BITS_CRC	((1u << XFS_BB_NUM_BITS_CRC) - 1)

/*
 * Generic stats interface
 */
#define XFS_BTREE_STATS_INC(cur, stat)	\
	XFS_STATS_INC_OFF((cur)->bc_mp, \
		(cur)->bc_ops->statoff + __XBTS_ ## stat)
#define XFS_BTREE_STATS_ADD(cur, stat, val)	\
	XFS_STATS_ADD_OFF((cur)->bc_mp, \
		(cur)->bc_ops->statoff + __XBTS_ ## stat, val)

enum xbtree_key_contig {
	XBTREE_KEY_GAP = 0,
	XBTREE_KEY_CONTIGUOUS,
	XBTREE_KEY_OVERLAP,
};

/*
 * Decide if these two numeric btree key fields are contiguous, overlapping,
 * or if there's a gap between them.  @x should be the field from the high
 * key and @y should be the field from the low key.
 */
static inline enum xbtree_key_contig xbtree_key_contig(uint64_t x, uint64_t y)
{
	x++;
	if (x < y)
		return XBTREE_KEY_GAP;
	if (x == y)
		return XBTREE_KEY_CONTIGUOUS;
	return XBTREE_KEY_OVERLAP;
}

#define XFS_BTREE_LONG_PTR_LEN		(sizeof(__be64))
#define XFS_BTREE_SHORT_PTR_LEN		(sizeof(__be32))

enum xfs_btree_type {
	XFS_BTREE_TYPE_AG,
	XFS_BTREE_TYPE_INODE,
	XFS_BTREE_TYPE_MEM,
};

struct xfs_btree_ops {
	const char		*name;

	/* Type of btree - AG-rooted or inode-rooted */
	enum xfs_btree_type	type;

	/* XFS_BTGEO_* flags that determine the geometry of the btree */
	unsigned int		geom_flags;

	/* size of the key, pointer, and record structures */
	size_t			key_len;
	size_t			ptr_len;
	size_t			rec_len;

	/* LRU refcount to set on each btree buffer created */
	unsigned int		lru_refs;

	/* offset of btree stats array */
	unsigned int		statoff;

	/* sick mask for health reporting (not for bmap btrees) */
	unsigned int		sick_mask;

	/* cursor operations */
	struct xfs_btree_cur *(*dup_cursor)(struct xfs_btree_cur *);
	void	(*update_cursor)(struct xfs_btree_cur *src,
				 struct xfs_btree_cur *dst);

	/* update btree root pointer */
	void	(*set_root)(struct xfs_btree_cur *cur,
			    const union xfs_btree_ptr *nptr, int level_change);

	/* block allocation / freeing */
	int	(*alloc_block)(struct xfs_btree_cur *cur,
			       const union xfs_btree_ptr *start_bno,
			       union xfs_btree_ptr *new_bno,
			       int *stat);
	int	(*free_block)(struct xfs_btree_cur *cur, struct xfs_buf *bp);

	/* records in block/level */
	int	(*get_minrecs)(struct xfs_btree_cur *cur, int level);
	int	(*get_maxrecs)(struct xfs_btree_cur *cur, int level);

	/* records on disk.  Matter for the root in inode case. */
	int	(*get_dmaxrecs)(struct xfs_btree_cur *cur, int level);

	/* init values of btree structures */
	void	(*init_key_from_rec)(union xfs_btree_key *key,
				     const union xfs_btree_rec *rec);
	void	(*init_rec_from_cur)(struct xfs_btree_cur *cur,
				     union xfs_btree_rec *rec);
	void	(*init_ptr_from_cur)(struct xfs_btree_cur *cur,
				     union xfs_btree_ptr *ptr);
	void	(*init_high_key_from_rec)(union xfs_btree_key *key,
					  const union xfs_btree_rec *rec);

	/*
	 * Compare key value and cursor value -- positive if key > cur,
	 * negative if key < cur, and zero if equal.
	 */
	int	(*cmp_key_with_cur)(struct xfs_btree_cur *cur,
				    const union xfs_btree_key *key);

	/*
	 * Compare key1 and key2 -- positive if key1 > key2, negative if
	 * key1 < key2, and zero if equal.  If the @mask parameter is non NULL,
	 * each key field to be used in the comparison must contain a nonzero
	 * value.
	 */
	int	(*cmp_two_keys)(struct xfs_btree_cur *cur,
				const union xfs_btree_key *key1,
				const union xfs_btree_key *key2,
				const union xfs_btree_key *mask);

	const struct xfs_buf_ops	*buf_ops;

	/* check that k1 is lower than k2 */
	int	(*keys_inorder)(struct xfs_btree_cur *cur,
				const union xfs_btree_key *k1,
				const union xfs_btree_key *k2);

	/* check that r1 is lower than r2 */
	int	(*recs_inorder)(struct xfs_btree_cur *cur,
				const union xfs_btree_rec *r1,
				const union xfs_btree_rec *r2);

	/*
	 * Are these two btree keys immediately adjacent?
	 *
	 * Given two btree keys @key1 and @key2, decide if it is impossible for
	 * there to be a third btree key K satisfying the relationship
	 * @key1 < K < @key2.  To determine if two btree records are
	 * immediately adjacent, @key1 should be the high key of the first
	 * record and @key2 should be the low key of the second record.
	 * If the @mask parameter is non NULL, each key field to be used in the
	 * comparison must contain a nonzero value.
	 */
	enum xbtree_key_contig (*keys_contiguous)(struct xfs_btree_cur *cur,
			       const union xfs_btree_key *key1,
			       const union xfs_btree_key *key2,
			       const union xfs_btree_key *mask);

	/*
	 * Reallocate the space for if_broot to fit the number of records.
	 * Move the records and pointers in if_broot to fit the new size.  When
	 * shrinking this will eliminate holes between the records and pointers
	 * created by the caller.  When growing this will create holes to be
	 * filled in by the caller.
	 *
	 * The caller must not request to add more records than would fit in
	 * the on-disk inode root.  If the if_broot is currently NULL, then if
	 * we are adding records, one will be allocated.  The caller must also
	 * not request that the number of records go below zero, although it
	 * can go to zero.
	 */
	struct xfs_btree_block *(*broot_realloc)(struct xfs_btree_cur *cur,
				unsigned int new_numrecs);
};

/* btree geometry flags */
#define XFS_BTGEO_OVERLAPPING		(1U << 0) /* overlapping intervals */
#define XFS_BTGEO_IROOT_RECORDS		(1U << 1) /* iroot can store records */

union xfs_btree_irec {
	struct xfs_alloc_rec_incore	a;
	struct xfs_bmbt_irec		b;
	struct xfs_inobt_rec_incore	i;
	struct xfs_rmap_irec		r;
	struct xfs_refcount_irec	rc;
};

struct xfs_btree_level {
	/* buffer pointer */
	struct xfs_buf		*bp;

	/* key/record number */
	uint16_t		ptr;

	/* readahead info */
#define XFS_BTCUR_LEFTRA	(1 << 0) /* left sibling has been read-ahead */
#define XFS_BTCUR_RIGHTRA	(1 << 1) /* right sibling has been read-ahead */
	uint16_t		ra;
};

/*
 * Btree cursor structure.
 * This collects all information needed by the btree code in one place.
 */
struct xfs_btree_cur
{
	struct xfs_trans	*bc_tp;	/* transaction we're in, if any */
	struct xfs_mount	*bc_mp;	/* file system mount struct */
	const struct xfs_btree_ops *bc_ops;
	struct kmem_cache	*bc_cache; /* cursor cache */
	unsigned int		bc_flags; /* btree features - below */
	union xfs_btree_irec	bc_rec;	/* current insert/search record value */
	uint8_t			bc_nlevels; /* number of levels in the tree */
	uint8_t			bc_maxlevels; /* maximum levels for this btree type */
	struct xfs_group	*bc_group;

	/* per-type information */
	union {
		struct {
			struct xfs_inode	*ip;
			short			forksize;
			char			whichfork;
			struct xbtree_ifakeroot	*ifake;	/* for staging cursor */
		} bc_ino;
		struct {
			struct xfs_buf		*agbp;
			struct xbtree_afakeroot	*afake;	/* for staging cursor */
		} bc_ag;
		struct {
			struct xfbtree		*xfbtree;
		} bc_mem;
	};

	/* per-format private data */
	union {
		struct {
			int		allocated;
		} bc_bmap;	/* bmapbt */
		struct {
			unsigned int	nr_ops;		/* # record updates */
			unsigned int	shape_changes;	/* # of extent splits */
		} bc_refc;	/* refcountbt/rtrefcountbt */
	};

	/* Must be at the end of the struct! */
	struct xfs_btree_level	bc_levels[];
};

/*
 * Compute the size of a btree cursor that can handle a btree of a given
 * height.  The bc_levels array handles node and leaf blocks, so its size
 * is exactly nlevels.
 */
static inline size_t
xfs_btree_cur_sizeof(unsigned int nlevels)
{
	return struct_size_t(struct xfs_btree_cur, bc_levels, nlevels);
}

/* cursor state flags */
/*
 * The root of this btree is a fakeroot structure so that we can stage a btree
 * rebuild without leaving it accessible via primary metadata.  The ops struct
 * is dynamically allocated and must be freed when the cursor is deleted.
 */
#define XFS_BTREE_STAGING		(1U << 0)

/* We are converting a delalloc reservation (only for bmbt btrees) */
#define	XFS_BTREE_BMBT_WASDEL		(1U << 1)

/* For extent swap, ignore owner check in verifier (only for bmbt btrees) */
#define	XFS_BTREE_BMBT_INVALID_OWNER	(1U << 2)

/* Cursor is active (only for allocbt btrees) */
#define	XFS_BTREE_ALLOCBT_ACTIVE	(1U << 3)

#define	XFS_BTREE_NOERROR	0
#define	XFS_BTREE_ERROR		1

/*
 * Convert from buffer to btree block header.
 */
#define	XFS_BUF_TO_BLOCK(bp)	((struct xfs_btree_block *)((bp)->b_addr))

xfs_failaddr_t __xfs_btree_check_block(struct xfs_btree_cur *cur,
		struct xfs_btree_block *block, int level, struct xfs_buf *bp);
int __xfs_btree_check_ptr(struct xfs_btree_cur *cur,
		const union xfs_btree_ptr *ptr, int index, int level);

/*
 * Check that block header is ok.
 */
int
xfs_btree_check_block(
	struct xfs_btree_cur	*cur,	/* btree cursor */
	struct xfs_btree_block	*block,	/* generic btree block pointer */
	int			level,	/* level of the btree block */
	struct xfs_buf		*bp);	/* buffer containing block, if any */

/*
 * Delete the btree cursor.
 */
void
xfs_btree_del_cursor(
	struct xfs_btree_cur	*cur,	/* btree cursor */
	int			error);	/* del because of error */

/*
 * Duplicate the btree cursor.
 * Allocate a new one, copy the record, re-get the buffers.
 */
int					/* error */
xfs_btree_dup_cursor(
	struct xfs_btree_cur		*cur,	/* input cursor */
	struct xfs_btree_cur		**ncur);/* output cursor */

/*
 * Compute first and last byte offsets for the fields given.
 * Interprets the offsets table, which contains struct field offsets.
 */
void
xfs_btree_offsets(
	uint32_t		fields,	/* bitmask of fields */
	const short		*offsets,/* table of field offsets */
	int			nbits,	/* number of bits to inspect */
	int			*first,	/* output: first byte offset */
	int			*last);	/* output: last byte offset */

/*
 * Initialise a new btree block header
 */
void xfs_btree_init_buf(struct xfs_mount *mp, struct xfs_buf *bp,
		const struct xfs_btree_ops *ops, __u16 level, __u16 numrecs,
		__u64 owner);
void xfs_btree_init_block(struct xfs_mount *mp,
		struct xfs_btree_block *buf, const struct xfs_btree_ops *ops,
		__u16 level, __u16 numrecs, __u64 owner);

/*
 * Common btree core entry points.
 */
int xfs_btree_increment(struct xfs_btree_cur *, int, int *);
int xfs_btree_decrement(struct xfs_btree_cur *, int, int *);
int xfs_btree_lookup(struct xfs_btree_cur *, xfs_lookup_t, int *);
int xfs_btree_update(struct xfs_btree_cur *, union xfs_btree_rec *);
int xfs_btree_new_iroot(struct xfs_btree_cur *, int *, int *);
int xfs_btree_insert(struct xfs_btree_cur *, int *);
int xfs_btree_delete(struct xfs_btree_cur *, int *);
int xfs_btree_get_rec(struct xfs_btree_cur *, union xfs_btree_rec **, int *);
int xfs_btree_change_owner(struct xfs_btree_cur *cur, uint64_t new_owner,
			   struct list_head *buffer_list);

/*
 * btree block CRC helpers
 */
void xfs_btree_fsblock_calc_crc(struct xfs_buf *);
bool xfs_btree_fsblock_verify_crc(struct xfs_buf *);
void xfs_btree_agblock_calc_crc(struct xfs_buf *);
bool xfs_btree_agblock_verify_crc(struct xfs_buf *);

/*
 * Internal btree helpers also used by xfs_bmap.c.
 */
void xfs_btree_log_block(struct xfs_btree_cur *, struct xfs_buf *, uint32_t);
void xfs_btree_log_recs(struct xfs_btree_cur *, struct xfs_buf *, int, int);

/*
 * Helpers.
 */
static inline int xfs_btree_get_numrecs(const struct xfs_btree_block *block)
{
	return be16_to_cpu(block->bb_numrecs);
}

static inline void xfs_btree_set_numrecs(struct xfs_btree_block *block,
		uint16_t numrecs)
{
	block->bb_numrecs = cpu_to_be16(numrecs);
}

static inline int xfs_btree_get_level(const struct xfs_btree_block *block)
{
	return be16_to_cpu(block->bb_level);
}


/*
 * Min and max functions for extlen, agblock, fileoff, and filblks types.
 */
#define	XFS_EXTLEN_MIN(a,b)	min_t(xfs_extlen_t, (a), (b))
#define	XFS_EXTLEN_MAX(a,b)	max_t(xfs_extlen_t, (a), (b))
#define	XFS_AGBLOCK_MIN(a,b)	min_t(xfs_agblock_t, (a), (b))
#define	XFS_AGBLOCK_MAX(a,b)	max_t(xfs_agblock_t, (a), (b))
#define	XFS_FILEOFF_MIN(a,b)	min_t(xfs_fileoff_t, (a), (b))
#define	XFS_FILEOFF_MAX(a,b)	max_t(xfs_fileoff_t, (a), (b))
#define	XFS_FILBLKS_MIN(a,b)	min_t(xfs_filblks_t, (a), (b))
#define	XFS_FILBLKS_MAX(a,b)	max_t(xfs_filblks_t, (a), (b))

xfs_failaddr_t xfs_btree_agblock_v5hdr_verify(struct xfs_buf *bp);
xfs_failaddr_t xfs_btree_agblock_verify(struct xfs_buf *bp,
		unsigned int max_recs);
xfs_failaddr_t xfs_btree_fsblock_v5hdr_verify(struct xfs_buf *bp,
		uint64_t owner);
xfs_failaddr_t xfs_btree_fsblock_verify(struct xfs_buf *bp,
		unsigned int max_recs);
xfs_failaddr_t xfs_btree_memblock_verify(struct xfs_buf *bp,
		unsigned int max_recs);

unsigned int xfs_btree_compute_maxlevels(const unsigned int *limits,
		unsigned long long records);
unsigned long long xfs_btree_calc_size(const unsigned int *limits,
		unsigned long long records);
unsigned int xfs_btree_space_to_height(const unsigned int *limits,
		unsigned long long blocks);

/*
 * Return codes for the query range iterator function are 0 to continue
 * iterating, and non-zero to stop iterating.  Any non-zero value will be
 * passed up to the _query_range caller.  The special value -ECANCELED can be
 * used to stop iteration, because _query_range never generates that error
 * code on its own.
 */
typedef int (*xfs_btree_query_range_fn)(struct xfs_btree_cur *cur,
		const union xfs_btree_rec *rec, void *priv);

int xfs_btree_query_range(struct xfs_btree_cur *cur,
		const union xfs_btree_irec *low_rec,
		const union xfs_btree_irec *high_rec,
		xfs_btree_query_range_fn fn, void *priv);
int xfs_btree_query_all(struct xfs_btree_cur *cur, xfs_btree_query_range_fn fn,
		void *priv);

typedef int (*xfs_btree_visit_blocks_fn)(struct xfs_btree_cur *cur, int level,
		void *data);
/* Visit record blocks. */
#define XFS_BTREE_VISIT_RECORDS		(1 << 0)
/* Visit leaf blocks. */
#define XFS_BTREE_VISIT_LEAVES		(1 << 1)
/* Visit all blocks. */
#define XFS_BTREE_VISIT_ALL		(XFS_BTREE_VISIT_RECORDS | \
					 XFS_BTREE_VISIT_LEAVES)
int xfs_btree_visit_blocks(struct xfs_btree_cur *cur,
		xfs_btree_visit_blocks_fn fn, unsigned int flags, void *data);

int xfs_btree_count_blocks(struct xfs_btree_cur *cur, xfs_filblks_t *blocks);

union xfs_btree_rec *xfs_btree_rec_addr(struct xfs_btree_cur *cur, int n,
		struct xfs_btree_block *block);
union xfs_btree_key *xfs_btree_key_addr(struct xfs_btree_cur *cur, int n,
		struct xfs_btree_block *block);
union xfs_btree_key *xfs_btree_high_key_addr(struct xfs_btree_cur *cur, int n,
		struct xfs_btree_block *block);
union xfs_btree_ptr *xfs_btree_ptr_addr(struct xfs_btree_cur *cur, int n,
		struct xfs_btree_block *block);
int xfs_btree_lookup_get_block(struct xfs_btree_cur *cur, int level,
		const union xfs_btree_ptr *pp, struct xfs_btree_block **blkp);
struct xfs_btree_block *xfs_btree_get_block(struct xfs_btree_cur *cur,
		int level, struct xfs_buf **bpp);
bool xfs_btree_ptr_is_null(struct xfs_btree_cur *cur,
		const union xfs_btree_ptr *ptr);
int xfs_btree_cmp_two_ptrs(struct xfs_btree_cur *cur,
			   const union xfs_btree_ptr *a,
			   const union xfs_btree_ptr *b);
void xfs_btree_get_sibling(struct xfs_btree_cur *cur,
			   struct xfs_btree_block *block,
			   union xfs_btree_ptr *ptr, int lr);
void xfs_btree_get_keys(struct xfs_btree_cur *cur,
		struct xfs_btree_block *block, union xfs_btree_key *key);
union xfs_btree_key *xfs_btree_high_key_from_key(struct xfs_btree_cur *cur,
		union xfs_btree_key *key);
typedef bool (*xfs_btree_key_gap_fn)(struct xfs_btree_cur *cur,
		const union xfs_btree_key *key1,
		const union xfs_btree_key *key2);

int xfs_btree_has_records(struct xfs_btree_cur *cur,
		const union xfs_btree_irec *low,
		const union xfs_btree_irec *high,
		const union xfs_btree_key *mask,
		enum xbtree_recpacking *outcome);

bool xfs_btree_has_more_records(struct xfs_btree_cur *cur);
struct xfs_ifork *xfs_btree_ifork_ptr(struct xfs_btree_cur *cur);

/* Key comparison helpers */
static inline bool
xfs_btree_keycmp_lt(
	struct xfs_btree_cur		*cur,
	const union xfs_btree_key	*key1,
	const union xfs_btree_key	*key2)
{
	return cur->bc_ops->cmp_two_keys(cur, key1, key2, NULL) < 0;
}

static inline bool
xfs_btree_keycmp_gt(
	struct xfs_btree_cur		*cur,
	const union xfs_btree_key	*key1,
	const union xfs_btree_key	*key2)
{
	return cur->bc_ops->cmp_two_keys(cur, key1, key2, NULL) > 0;
}

static inline bool
xfs_btree_keycmp_eq(
	struct xfs_btree_cur		*cur,
	const union xfs_btree_key	*key1,
	const union xfs_btree_key	*key2)
{
	return cur->bc_ops->cmp_two_keys(cur, key1, key2, NULL) == 0;
}

static inline bool
xfs_btree_keycmp_le(
	struct xfs_btree_cur		*cur,
	const union xfs_btree_key	*key1,
	const union xfs_btree_key	*key2)
{
	return !xfs_btree_keycmp_gt(cur, key1, key2);
}

static inline bool
xfs_btree_keycmp_ge(
	struct xfs_btree_cur		*cur,
	const union xfs_btree_key	*key1,
	const union xfs_btree_key	*key2)
{
	return !xfs_btree_keycmp_lt(cur, key1, key2);
}

static inline bool
xfs_btree_keycmp_ne(
	struct xfs_btree_cur		*cur,
	const union xfs_btree_key	*key1,
	const union xfs_btree_key	*key2)
{
	return !xfs_btree_keycmp_eq(cur, key1, key2);
}

/* Masked key comparison helpers */
static inline bool
xfs_btree_masked_keycmp_lt(
	struct xfs_btree_cur		*cur,
	const union xfs_btree_key	*key1,
	const union xfs_btree_key	*key2,
	const union xfs_btree_key	*mask)
{
	return cur->bc_ops->cmp_two_keys(cur, key1, key2, mask) < 0;
}

static inline bool
xfs_btree_masked_keycmp_gt(
	struct xfs_btree_cur		*cur,
	const union xfs_btree_key	*key1,
	const union xfs_btree_key	*key2,
	const union xfs_btree_key	*mask)
{
	return cur->bc_ops->cmp_two_keys(cur, key1, key2, mask) > 0;
}

static inline bool
xfs_btree_masked_keycmp_ge(
	struct xfs_btree_cur		*cur,
	const union xfs_btree_key	*key1,
	const union xfs_btree_key	*key2,
	const union xfs_btree_key	*mask)
{
	return !xfs_btree_masked_keycmp_lt(cur, key1, key2, mask);
}

/* Does this cursor point to the last block in the given level? */
static inline bool
xfs_btree_islastblock(
	struct xfs_btree_cur	*cur,
	int			level)
{
	struct xfs_btree_block	*block;
	struct xfs_buf		*bp;

	block = xfs_btree_get_block(cur, level, &bp);

	if (cur->bc_ops->ptr_len == XFS_BTREE_LONG_PTR_LEN)
		return block->bb_u.l.bb_rightsib == cpu_to_be64(NULLFSBLOCK);
	return block->bb_u.s.bb_rightsib == cpu_to_be32(NULLAGBLOCK);
}

void xfs_btree_set_ptr_null(struct xfs_btree_cur *cur,
		union xfs_btree_ptr *ptr);
int xfs_btree_get_buf_block(struct xfs_btree_cur *cur,
		const union xfs_btree_ptr *ptr, struct xfs_btree_block **block,
		struct xfs_buf **bpp);
int xfs_btree_read_buf_block(struct xfs_btree_cur *cur,
		const union xfs_btree_ptr *ptr, int flags,
		struct xfs_btree_block **block, struct xfs_buf **bpp);
void xfs_btree_set_sibling(struct xfs_btree_cur *cur,
		struct xfs_btree_block *block, const union xfs_btree_ptr *ptr,
		int lr);
void xfs_btree_init_block_cur(struct xfs_btree_cur *cur,
		struct xfs_buf *bp, int level, int numrecs);
void xfs_btree_copy_ptrs(struct xfs_btree_cur *cur,
		union xfs_btree_ptr *dst_ptr,
		const union xfs_btree_ptr *src_ptr, int numptrs);
void xfs_btree_copy_keys(struct xfs_btree_cur *cur,
		union xfs_btree_key *dst_key,
		const union xfs_btree_key *src_key, int numkeys);
void xfs_btree_init_ptr_from_cur(struct xfs_btree_cur *cur,
		union xfs_btree_ptr *ptr);

static inline struct xfs_btree_cur *
xfs_btree_alloc_cursor(
	struct xfs_mount	*mp,
	struct xfs_trans	*tp,
	const struct xfs_btree_ops *ops,
	uint8_t			maxlevels,
	struct kmem_cache	*cache)
{
	struct xfs_btree_cur	*cur;

	ASSERT(ops->ptr_len == XFS_BTREE_LONG_PTR_LEN ||
	       ops->ptr_len == XFS_BTREE_SHORT_PTR_LEN);

	/* BMBT allocations can come through from non-transactional context. */
	cur = kmem_cache_zalloc(cache,
			GFP_KERNEL | __GFP_NOLOCKDEP | __GFP_NOFAIL);
	cur->bc_ops = ops;
	cur->bc_tp = tp;
	cur->bc_mp = mp;
	cur->bc_maxlevels = maxlevels;
	cur->bc_cache = cache;

	return cur;
}

int __init xfs_btree_init_cur_caches(void);
void xfs_btree_destroy_cur_caches(void);

int xfs_btree_goto_left_edge(struct xfs_btree_cur *cur);

/* Does this level of the cursor point to the inode root (and not a block)? */
static inline bool
xfs_btree_at_iroot(
	const struct xfs_btree_cur	*cur,
	int				level)
{
	return cur->bc_ops->type == XFS_BTREE_TYPE_INODE &&
	       level == cur->bc_nlevels - 1;
}

int xfs_btree_alloc_metafile_block(struct xfs_btree_cur *cur,
		const union xfs_btree_ptr *start, union xfs_btree_ptr *newp,
		int *stat);
int xfs_btree_free_metafile_block(struct xfs_btree_cur *cur,
		struct xfs_buf *bp);

#endif	/* __XFS_BTREE_H__ */
