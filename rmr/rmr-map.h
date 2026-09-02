/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Reliable multicast over RTRS (RMR)
 *
 * Copyright (c) 2026 IONOS SE
 */

#ifndef RMR_MAP_H
#define RMR_MAP_H

#include <linux/types.h>
#include <linux/xarray.h>
#include <linux/bitmap.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/math.h>

#include "rmr.h"

/**
 * The dirty map buffer is used to track dirty chunks through bits.
 * The position of the bit denotes the chunk number it tracks.
 *
 * Bitmap structure
 * ----------------
 * The dirty bitmap is stored in a 2 level tree-like structure.
 * The main unit of storage are memory pages; They act as nodes of this structure.
 * The first level pages (FLP) stores the address of the second level pages.
 * There can be a total of 256 first level pages.
 * The second level pages (SLP, also the leaf nodes/pages) stores the bitmap.
 *
 * The first level pages have to store the address of the second level pages.
 * An address being 8B (default/max) long, the addresses of a maximum of 512 pages can
 * be stored in a first level page. This then decides the maximum leaf pages a pool can
 * have, which, for our example, is [(# pages of FLP) * (PAGE_SIZE / address_size)],
 * (256*512)=131072.
 * With the above info, the available space for bitmap is 131072*4KB(PAGE_SIZE)=512MB.
 *
 * A chunk is the smallest unit of data which is tracked for being dirty. A chunk is
 * called dirty/unsynced, even if a single byte in it is dirty/unsynced.
 * To track a chunk, a single byte (1B) is used. The least significant bit is used to signify
 * if the chunk is dirty (set) or not. Other bits can be used for other purposes (for example,
 * filters). The maximum number of chunks RMR can manage are then, (512MB)/1B=536870912.
 * This number is fixed, as one can see from the calculations, and hence the maximum size of
 * metadata RMR can allocate and use is fixed.

 * The user configurable part is the chunk size. Its range is 128KB-1MB, and it has to be a
 * power of 2.
 * The chunk size decides the maximum mapped size for an RMR pool.
 * For example, for chunk size 1MB, and taking the maximum number of chunks RMR can allocate
 * and handle (536870912, see above), the maximum mapped size would be (536870912*1MB)=512TB.
 * The table showing the relation between chunk size and maximum mapped size is as follows,
 * Chunk size	Maximum mapped size
 * 128KB	64TB
 * 256KB	128TB
 * 512KB	256TB
 * 1MB		512TB
 *
 * Calculating chunk number
 * ------------------------
 * Some key points
 * 1) The Linux kernel has a fixed size for sector, which is 512 (or 9 bitshift)
 * 2) The mapped_size provided and stores in the rmr_pool structure is in sectors.
 * 3) The chunk_size provided and stored in the rmr_pool structure is in bytes.
 * 4) The code calculates and stores chunk_size_shift in the rmr_pool structure to do fast
 *    calculation.
 * 5) The IO offset give to RMR (through function rmr_clt_request) is in bytes.
 *
 * --
 * With the above points, lets have a sample scenario with mapped_size 1GB and chunk_size 128KB
 * The numbers would then be,
 *
 * no_of_chunks = (mapped_size / chunk_size)
 * no_of_chunks = 8192
 *
 * chunk_size = 131072
 * chunk_size_shift = 17
 *
 * dirty_map buffer size (in BYTES) = (no_of_chunks / bits in a byte)
 * dirty_map buffer size (in BYTES) = 1024
 *
 * --
 * Lets do a sample calculation of chunk_no from offset and length of an IO
 *
 * For offset 30801920 and length 4096
 *
 * chunk_no = (offset >> chunk_size_shift)
 * chunk_no = 235
 *
 */

#define RMR_KEY_SHIFT 32

// Each chunk requires 1B of metadata
#define PER_CHUNK_MD		1
#define PER_CHUNK_MD_LOG2	ilog2(PER_CHUNK_MD)

#define GET_CHUNK_NUMBER(offset, shift)			(offset >> shift)
#define GET_FOLLOWING_CHUNKS(offset_len, shift, start)	(((offset_len - 1) >> shift) - start + 1)

#define CHUNK_TO_OFFSET(chunk_no, shift)       (chunk_no << shift)

/*
 * Chunk size bounds. The byte values are the source of truth: chunk_size is a
 * byte-valued module parameter, validated as a power of two within these
 * bounds, and pool->chunk_size_shift is ilog2() of it.
 */
#define RMR_MIN_CHUNK_SIZE		(128 << 10)
#define RMR_MAX_CHUNK_SIZE		(1024 << 10)
#define RMR_MAX_CHUNK_SIZE_SHIFT	ilog2(RMR_MAX_CHUNK_SIZE)
#define RMR_DEFAULT_CHUNK_SIZE		RMR_MIN_CHUNK_SIZE	/* 128 KB */

/*
 * Sub-block dirty tracking.
 *
 * A dirty chunk is tracked at this finer grain so that a write which fully
 * covers a sub-block can be applied directly to the backend and that sub-block
 * marked in sync. The chunk remains the unit that tracks dirty data; the
 * sub-block is only the unit of data that needs to be synced within a dirty
 * chunk.
 *
 * RMR_MAX_SUBBLK sizes rmr_subblk_slot.bits for the largest chunk allowed, so
 * derive it from the same value the parameter is checked against rather than
 * from a second copy of the limit.
 */
#define RMR_SUBBLK_SHIFT	12			/* 4 KiB */
#define RMR_SUBBLK_SIZE		BIT(RMR_SUBBLK_SHIFT)
#define RMR_MAX_SUBBLK		(1U << (RMR_MAX_CHUNK_SIZE_SHIFT - RMR_SUBBLK_SHIFT))

/* Below this, chunk_size_shift - RMR_SUBBLK_SHIFT underflows. */
static_assert(RMR_MIN_CHUNK_SIZE >= RMR_SUBBLK_SIZE,
	      "a chunk must hold at least one sub-block");

// The element type stored in FLP
typedef unsigned long	el_flp;

enum {
	CHUNK_DIRTY_BIT = 0,
	CHUNK_FILTER_BIT,
};

enum {
	MAX_NO_OF_FLP = 256,
	NO_OF_SLP_PER_FLP = (PAGE_SIZE >> ilog2(sizeof(void *))),
	NO_OF_SLP_PER_FLP_LOG2 = ilog2(NO_OF_SLP_PER_FLP),
	MAX_NO_OF_SLP = (MAX_NO_OF_FLP * NO_OF_SLP_PER_FLP),

	NO_OF_CHUNKS_PER_PAGE = (PAGE_SIZE >> PER_CHUNK_MD_LOG2),
	// Chunks data is stored only in SLP
	MAX_NO_OF_CHUNKS = (MAX_NO_OF_SLP * NO_OF_CHUNKS_PER_PAGE),

	CHUNKS_PER_SLP = (PAGE_SIZE >> PER_CHUNK_MD_LOG2),
	CHUNKS_PER_SLP_LOG2 = ilog2(CHUNKS_PER_SLP),
	CHUNKS_PER_FLP = (CHUNKS_PER_SLP * NO_OF_SLP_PER_FLP),
	CHUNKS_PER_FLP_LOG2 = ilog2(CHUNKS_PER_FLP),
};

typedef enum {
	MAP_NO_FILTER = 0,
	MAP_ENTRY_UNSYNCED
} rmr_map_filter;

enum rmr_map_state {
	RMR_MAP_STATE_NO_CHECK = 0,
	RMR_MAP_STATE_CHECKING,
	// do we have some other useful states ?
};

struct rmr_dirty_id_map {
	u8 member_id;
	struct xarray rmr_id_map;
	unsigned long ts;
	atomic_t check_state;

	/*
	 * The usage of this is restricted to form a linked lised
	 * during mass deletion. Since this is in an RCU list (maps
	 * in rmr_pool), we cannot use this or change any data until
	 * the RCU period completes. So we use this next variable
	 * during mass deletion so we can have a list and don't have
	 * to wait and restart the search on every individual deletion
	 * of a map. Refer destroy_clt_pool().
	 */
	struct rmr_dirty_id_map *next;

	u64		no_of_chunks;
	u64		no_of_flp;
	u64		no_of_slp_in_last_flp;
	u64		no_of_chunk_in_last_slp;
	u64		total_slp;
	u8		*bitmap_filter;
	void		*dirty_bitmap[MAX_NO_OF_FLP];
};

/*
 * Sub-block dirty tracking within a dirty chunk. A set bit in @bits means
 * the sub-block still needs syncing from a clean peer. It is cleared only when
 * the sub-block holds current data, either written whole or pulled by a sync.
 * Guarded by @lock.
 *
 * Held in a bounded cache keyed by chunk id. @nr_dirty reaching 0 does not
 * by itself make the chunk clean; see rmr_subblk_slot_clear().
 */
struct rmr_subblk_slot {
	spinlock_t lock;	/* guards @bits and @nr_dirty */
	u16 nr_subblk;
	u16 nr_dirty;
	DECLARE_BITMAP(bits, RMR_MAX_SUBBLK);
	struct rcu_head rcu;
};

struct rmr_map_entry {
	atomic_t sync_cnt;
	struct llist_head wait_list;
};

/*
 * The header of the bitmap buffer.
 */
struct rmr_map_cbuf_hdr {
	u64		version;
	u8		member_id;

	u64		no_of_chunks;
	u64		no_of_flp;
	u64		no_of_slp_in_last_flp;
	u64		no_of_chunk_in_last_slp;
	u64		total_slp;
} __packed;

static inline unsigned long rmr_id_to_key(rmr_id_t id)
{
	unsigned long res;

	// highest bits for id.a, the rest are for id.b;
	res = ((id.a << RMR_KEY_SHIFT) | id.b);
	return res;
}

static inline u64 key_to_a(unsigned long key)
{
	return key >> RMR_KEY_SHIFT;
}

static inline u64 key_to_b(unsigned long key)
{
	return key & ((1ULL << RMR_KEY_SHIFT) - 1);
}

void rmr_map_update_page_params(struct rmr_dirty_id_map *map);
struct rmr_dirty_id_map *rmr_map_create(struct rmr_pool *pool, u8 member_id);
void rmr_map_destroy(struct rmr_dirty_id_map *map);
void rmr_map_calc_chunk(struct rmr_pool *pool, size_t offset, size_t length, rmr_id_t *id);
void rmr_map_set_dirty(struct rmr_dirty_id_map *map, rmr_id_t id, u8 filter);
void rmr_map_set_dirty_all(struct rmr_dirty_id_map *map, u8 filter);
struct rmr_map_entry *rmr_map_unset_dirty(struct rmr_dirty_id_map *map, rmr_id_t id, u8 filter);
bool rmr_map_check_dirty(struct rmr_dirty_id_map *map, rmr_id_t id);
struct rmr_map_entry *rmr_map_get_dirty_entry(struct rmr_dirty_id_map *map, rmr_id_t id);
void rmr_map_clear_filter_all(struct rmr_dirty_id_map *map, u8 filter);
void rmr_map_unset_dirty_all(struct rmr_dirty_id_map *map);
bool rmr_map_empty(struct rmr_dirty_id_map *map);

void rmr_map_bitwise_or_buf(void *dst_buf, void *src_buf, u32 buf_size);
int rmr_map_create_entries(struct rmr_dirty_id_map *map);

void rmr_map_hexdump_bitmap_buf(u8 member_id, void *buf, u32 buf_size);
void rmr_map_slps_to_buf(struct rmr_dirty_id_map *map, u64 slp_idx, u64 no_of_slp, u8 *buf);
u64 rmr_map_buf_to_slps(struct rmr_dirty_id_map *map, u8 *buf, u32 buf_size, u64 slp_idx,
			bool test);
void rmr_map_dump_bitmap(struct rmr_dirty_id_map *map);
int rmr_map_summary_format(struct rmr_pool *pool, char *buf, size_t buf_size);
void rmr_map_bidump_bitmap_buf(void *buf, u8 member_id, u32 buf_size);

/**
 * rmr_subblk_slot_alloc() - allocate a slot for a dirty chunk and initialize
 * every sub-block as dirty.
 *
 * @nr_subblk:	number of sub-blocks in the chunk.
 *
 * Description:
 *	The whole chunk is out of sync when its slot is first created, so all
 *	@nr_subblk sub-blocks start dirty.
 *
 * Return:
 *	pointer to the new slot, or NULL on allocation failure.
 */
static inline struct rmr_subblk_slot *rmr_subblk_slot_alloc(u32 nr_subblk)
{
	struct rmr_subblk_slot *slot;

	/* Cannot sleep since inside rcu_read_lock */
	slot = kmalloc(sizeof(*slot), GFP_NOWAIT);
	if (!slot)
		return NULL;

	spin_lock_init(&slot->lock);
	slot->nr_subblk = nr_subblk;
	slot->nr_dirty = nr_subblk;
	/* Set exactly nr_subblk bits; bitmap_fill() would round up to longs. */
	bitmap_zero(slot->bits, RMR_MAX_SUBBLK);
	bitmap_set(slot->bits, 0, nr_subblk);
	return slot;
}

/**
 * rmr_subblk_covered() - sub-blocks a byte range fully covers within a chunk.
 *
 * @chunk_size_shift:	log2(chunk_size); used to compute chunk-relative offset.
 * @off:		absolute byte offset of the IO.
 * @len:		byte length of the IO.
 * @first:		out: index of the first fully-covered sub-block.
 *			Untouched when the return value is 0.
 *
 * Description:
 *	Rounds inward: a sub-block the range only partly overlaps is not
 *	counted. Contrast rmr_subblk_touched(), which rounds outward.
 *
 * Return:
 *	number of fully-covered sub-blocks (0 if none).
 */
static inline u32 rmr_subblk_covered(u8 chunk_size_shift, u64 off, u32 len,
				     u32 *first)
{
	u64 start = off & ((1ULL << chunk_size_shift) - 1);
	u64 end = start + len;
	u64 fb = round_up(start, RMR_SUBBLK_SIZE) >> RMR_SUBBLK_SHIFT;
	u64 lb = end >> RMR_SUBBLK_SHIFT;	/* exclusive, rounded down */

	if (lb <= fb)
		return 0;
	*first = (u32)fb;
	return (u32)(lb - fb);
}

/**
 * rmr_subblk_slot_clear() - mark @count sub-blocks from @first as in-sync.
 *
 * @slot:	the sub-block slot to update.
 * @first:	index of the first sub-block to clear.
 * @count:	number of consecutive sub-blocks to clear.
 *
 * Description:
 *	Clearing bits does not by itself license clearing the chunk's coarse
 *	dirty bit: the slot may be detached from the cache by a concurrent
 *	re-dirty. Callers ask rmr_subblk_take_if_clean() for that
 *	verdict instead, so no in-sync count is returned here.
 */
static inline void rmr_subblk_slot_clear(struct rmr_subblk_slot *slot,
					 u32 first, u32 count)
{
	unsigned long flags;
	u32 i, end = first + count;

	if (end > slot->nr_subblk)
		end = slot->nr_subblk;

	spin_lock_irqsave(&slot->lock, flags);
	for (i = first; i < end; i++) {
		if (test_and_clear_bit(i, slot->bits) &&
		    !WARN_ON_ONCE(!slot->nr_dirty))
			slot->nr_dirty--;
	}
	spin_unlock_irqrestore(&slot->lock, flags);
}

/**
 * rmr_subblk_slot_set_dirty() - mark @count sub-blocks from @first as needing
 * sync again. Inverse of rmr_subblk_slot_clear().
 *
 * @slot:	the sub-block slot to update.
 * @first:	index of the first sub-block to re-dirty.
 * @count:	number of consecutive sub-blocks to re-dirty.
 *
 * Description:
 *	Used to undo a write the pool did not accept, so its sub-blocks
 *	stop counting as in sync and are re-pulled from the source. Already
 *	dirty sub-blocks stay dirty. The range is clamped to the slot's
 *	sub-block count.
 */
static inline void rmr_subblk_slot_set_dirty(struct rmr_subblk_slot *slot,
					     u32 first, u32 count)
{
	unsigned long flags;
	u32 i, end = first + count;

	if (end > slot->nr_subblk)
		end = slot->nr_subblk;

	spin_lock_irqsave(&slot->lock, flags);
	for (i = first; i < end; i++) {
		if (!test_and_set_bit(i, slot->bits))
			slot->nr_dirty++;
	}
	spin_unlock_irqrestore(&slot->lock, flags);
}

/**
 * rmr_subblk_touched() - count the sub-blocks a byte range overlaps.
 *
 * @chunk_size_shift:	log2 of the chunk size in bytes.
 * @off:		byte offset of the range; only its position within the
 *			chunk (off % chunk_size) matters.
 * @len:		length of the range in bytes.
 * @first:		set to the index of the first overlapped sub-block.
 *
 * Description:
 *	Rounds the start down and the end up, so a range that only partially
 *	covers an edge sub-block still counts it. This is the read-modify-write
 *	footprint of a write. Contrast rmr_subblk_covered(), which rounds
 *	inward to the sub-blocks a write fully covers.
 *
 * Return:
 *	number of sub-blocks the range touches, or 0 for a zero-length range.
 */
static inline u32 rmr_subblk_touched(u8 chunk_size_shift, u64 off, u32 len,
				     u32 *first)
{
	u64 start = off & ((1ULL << chunk_size_shift) - 1);
	u64 end = start + len;
	u64 fb = start >> RMR_SUBBLK_SHIFT;
	u64 lb = round_up(end, RMR_SUBBLK_SIZE) >> RMR_SUBBLK_SHIFT;

	if (!len)
		return 0;
	*first = (u32)fb;
	return (u32)(lb - fb);
}

/**
 * rmr_subblk_slot_all_clean() - check whether all sub-blocks are in sync.
 *
 * @slot:	the sub-block slot to check.
 *
 * Return:
 *	true if no sub-block of @slot still needs syncing.
 */
static inline bool rmr_subblk_slot_all_clean(struct rmr_subblk_slot *slot)
{
	unsigned long flags;
	bool all_clean;

	spin_lock_irqsave(&slot->lock, flags);
	all_clean = (slot->nr_dirty == 0);
	spin_unlock_irqrestore(&slot->lock, flags);
	return all_clean;
}

static inline void map_entry_get_sync(struct rmr_map_entry *entry)
{
	atomic_inc(&entry->sync_cnt);
	pr_debug("after get ref for entry %p, sync cnt %d\n",
		 entry, atomic_read(&entry->sync_cnt));
}

static inline int map_entry_put_sync(struct rmr_map_entry *entry)
{
	pr_debug("before dec_and_test for entry %p, sync cnt %d\n",
		 entry, atomic_read(&entry->sync_cnt));
	return atomic_dec_and_test(&entry->sync_cnt);
}

static inline void rmr_maplist_destroy(struct rmr_dirty_id_map *maplist)
{
	struct rmr_dirty_id_map *mp;

	while (maplist != NULL) {
		mp = maplist;
		maplist = maplist->next;
		rmr_map_destroy(mp);
	}
}
#endif /* RMR_MAP_H */
