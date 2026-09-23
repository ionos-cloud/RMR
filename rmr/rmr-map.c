// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Reliable multicast over RTRS (RMR)
 *
 * Copyright (c) 2026 IONOS SE
 */

#include <linux/slab.h>

#include "rmr-map.h"
#include "rmr-pool.h"

void rmr_map_update_page_params(struct rmr_dirty_id_map *map)
{
	unsigned long remaining_chunks;

	map->no_of_flp = (map->no_of_chunks >> CHUNKS_PER_FLP_LOG2);

	/*
	 * If the number of chunks are not completely filling an FLP (CHUNKS_PER_FLP),
	 * then the remaining would be tracked by the next FLP. Thus the next FLP would
	 * have unused SLP pointers. We will calculate the number of SLP slots which will
	 * be used in the last FLP.
	 */
	remaining_chunks = map->no_of_chunks & (CHUNKS_PER_FLP - 1);
	if (!remaining_chunks) {
		/*
		 * If there are no remaining chunks, then the last FLP is completely full.
		 */
		map->no_of_slp_in_last_flp = NO_OF_SLP_PER_FLP;
		map->no_of_chunk_in_last_slp = NO_OF_CHUNKS_PER_PAGE;
	} else {
		/*
		 * If there are remaining chunks, then we add another FLP for it.
		 * This FLP will not be full, hence we calculate the number of SLP slots
		 * that will be used.
		 */
		map->no_of_flp += 1;
		map->no_of_slp_in_last_flp = (remaining_chunks >> CHUNKS_PER_SLP_LOG2);

		/*
		 * Same as above. It could be that the number of chunks do not fit neatly
		 * in the last SLP (CHUNKS_PER_SLP), and the remaining ones end up in the
		 * SLP with remaining chunk slots.
		 */
		remaining_chunks &= (CHUNKS_PER_SLP - 1);
		if (!remaining_chunks) {
			/*
			 * If there are no remaining chunks, then the last SLP is completely full.
			 */
			map->no_of_chunk_in_last_slp = CHUNKS_PER_SLP;
		} else {
			/*
			 * If there are remaining chunks, then we add another SLP.
			 */
			map->no_of_slp_in_last_flp += 1;
			map->no_of_chunk_in_last_slp = remaining_chunks;
		}
	}

	map->total_slp = ((map->no_of_flp - 1) * NO_OF_SLP_PER_FLP) + map->no_of_slp_in_last_flp;
}

static void rmr_map_update_map_params(struct rmr_pool *pool, struct rmr_dirty_id_map *map)
{
	map->no_of_chunks = pool->no_of_chunks;

	rmr_map_update_page_params(map);

	pr_info("%s: Chunks info %u, %u, %u, %llu\n",
		__func__, pool->chunk_size, ilog2(pool->chunk_size),
		pool->chunk_size_shift, map->no_of_chunks);
	pr_info("%s: FLPs %llu, SLPs in last FLP %llu, Total SLPs %llu, chunks in last SLP %llu\n",
		__func__, map->no_of_flp, map->no_of_slp_in_last_flp, map->total_slp,
		map->no_of_chunk_in_last_slp);
	pr_info("%s: Dirty map size %lldB\n", __func__, (map->total_slp * PAGE_SIZE));
}

static int rmr_map_allocate_pages(struct rmr_pool *pool, struct rmr_dirty_id_map *map)
{
	el_flp *flp_ptr;
	u64 no_of_slps;
	int i, j;

	for (i = 0; i < map->no_of_flp;) {
		map->dirty_bitmap[i] = (void *)get_zeroed_page(GFP_KERNEL);
		if (!map->dirty_bitmap[i])
			goto err_alloc;
		flp_ptr = (el_flp *)map->dirty_bitmap[i];

		if (i == (map->no_of_flp - 1))
			no_of_slps = map->no_of_slp_in_last_flp;
		else
			no_of_slps = NO_OF_SLP_PER_FLP;

		/*
		 * Move the increment to here, so that later in err_alloc: if we have to free,
		 * the index i, is pointing in the correct position.
		 */
		i++;

		for (j = 0; j < no_of_slps; j++, flp_ptr++) {
			*flp_ptr = get_zeroed_page(GFP_KERNEL);
			if (!*flp_ptr)
				goto err_alloc;
		}
	}

	// TODO remove this
	map->bitmap_filter = kcalloc(pool->no_of_chunks, sizeof(*map->bitmap_filter), GFP_KERNEL);
	if (!map->bitmap_filter)
		goto err_alloc;

	return 0;

err_alloc:
	for (--i; i >= 0; i--) {
		flp_ptr = (el_flp *)map->dirty_bitmap[i];

		for (--j; j >= 0; j--)
			free_page((unsigned long)*(flp_ptr + j));

		j = NO_OF_SLP_PER_FLP;
		free_page((unsigned long)map->dirty_bitmap[i]);
	}

	return -ENOMEM;
}

struct rmr_dirty_id_map *rmr_map_create(struct rmr_pool *pool, u8 member_id)
{
	struct rmr_dirty_id_map *map = NULL;
	int ret;

	pr_info("%s: Creating map for member_id %u, in pool %s. Existing map_cnt %u\n",
		__func__, member_id, pool->poolname, pool->maps_cnt);

	if (!pool->no_of_chunks) {
		pr_err("%s: dirty map size cannot be zero\n", __func__);
		return ERR_PTR(-EINVAL);
	}

	mutex_lock(&pool->maps_lock);

	/*
	 * Don't create if already exists
	 */
	map = rmr_pool_find_map(pool, member_id);
	if (map != NULL) {
		pr_err("Map with member_id %u already exists\n", member_id);
		ret = -EEXIST;
		goto err_unlock;
	}

	if (pool->maps_cnt >= RMR_POOL_MAX_SESS) {
		pr_err("pool %s can not create new map, max number of sessions %d achieved\n",
		       pool->poolname, RMR_POOL_MAX_SESS);
		ret = -EINVAL;
		goto err_unlock;
	}

	/*
	 * Allocate memory and init the structure
	 */
	map = (struct rmr_dirty_id_map *)get_zeroed_page(GFP_KERNEL);
	if (!map) {
		pr_err("cannot allocate map for member_id %u\n", member_id);
		ret = -ENOMEM;
		goto err_unlock;
	}
	rmr_map_update_map_params(pool, map);

	ret = rmr_map_allocate_pages(pool, map);
	if (ret) {
		pr_err("cannot allocate memory for member_id %u\n", member_id);
		goto err_map;
	}

	/*
	 * Every acquisition of this lock excludes interrupts, so the index can
	 * be updated from IO completion, which may run in interrupt context.
	 * Lockdep puts this lock in one class with the sub-block cache's
	 * xa_lock, both being set up through xa_init_flags() here, so a cache
	 * update from IO completion needs this as well. Inserts allocate with
	 * GFP_KERNEL and run in process context through xa_insert_irq(); with
	 * XA_FLAGS_LOCK_IRQ the allocation retry drops the lock with interrupts
	 * enabled and retakes it with them disabled.
	 */
	xa_init_flags(&map->rmr_id_map, XA_FLAGS_ALLOC | XA_FLAGS_LOCK_IRQ);
	xa_init(&map->subblk_cache);
	spin_lock_init(&map->subblk_cache_lock);
	map->pool = pool;
	map->member_id = member_id;
	map->ts = jiffies;

	rmr_pool_maps_append(pool, map);

	mutex_unlock(&pool->maps_lock);

	return map;

err_map:
	free_page((unsigned long)map);
err_unlock:
	mutex_unlock(&pool->maps_lock);
	return ERR_PTR(ret);
}

void rmr_map_destroy(struct rmr_dirty_id_map *map)
{
	el_flp *flp_ptr;
	int i, j;
	u64 no_of_slps;

	/*
	 * Callers drain the cache before this, through
	 * rmr_map_unset_dirty_all(), or never fill it (client maps). Drain
	 * again so that no slot outlives the map, should one have been
	 * installed after that drain.
	 */
	rmr_subblk_drain_all(map);

	WARN_ON(!xa_empty(&map->rmr_id_map));
	map->ts = jiffies;

	pr_info("%s: member_id %u\n", __func__, map->member_id);
	xa_destroy(&map->subblk_cache);
	kfree(map->bitmap_filter);

	for (i = 0; i < map->no_of_flp; i++) {
		flp_ptr = (el_flp *)map->dirty_bitmap[i];

		if (i == (map->no_of_flp - 1))
			no_of_slps = map->no_of_slp_in_last_flp;
		else
			no_of_slps = NO_OF_SLP_PER_FLP;

		for (j = 0; j < no_of_slps; j++)
			free_page((unsigned long)*(flp_ptr + j));

		free_page((unsigned long)map->dirty_bitmap[i]);
	}

	free_page((unsigned long)map);
}

/*
 * Bounded sub-block slot cache helpers.
 *
 * The cache holds struct rmr_subblk_slot keyed by chunk id in
 * map->subblk_cache. A slot exists for a chunk if and only if the chunk is in
 * the PARTIAL state.
 *
 * The cap is enforced by refusing to admit a new slot. A caller that cannot get
 * one converges the chunk instead.
 *
 * Lock discipline:
 *   map->subblk_cache_lock serializes all mutations. xarray operations
 *   use GFP_NOWAIT under the spinlock to avoid sleeping; allocation
 *   failure causes the chunk to fall back to FULLY_DIRTY. Slot teardown
 *   is RCU-deferred via kfree_rcu so readers under rcu_read_lock()
 *   remain safe.
 */

/* Slots erased per critical section by rmr_subblk_drain_all(). */
#define RMR_SUBBLK_DRAIN_BATCH 512

/**
 * rmr_subblk_lookup() - look up a chunk's slot.
 *
 * @map:	the dirty-id map owning the cache.
 * @chunk:	id.b of the chunk to look up.
 *
 * Description:
 *	A miss means the chunk has no refinement and is treated as
 *	FULLY_DIRTY.
 *
 * Context:
 *	Call with rcu_read_lock() held. Slots are freed with kfree_rcu(), so
 *	that read section keeps the returned slot valid even if it is removed
 *	from the cache meanwhile.
 *
 * Return:
 *	the slot for @chunk, or NULL on a miss.
 */
struct rmr_subblk_slot *rmr_subblk_lookup(struct rmr_dirty_id_map *map,
					  u64 chunk)
{
	return xa_load(&map->subblk_cache, chunk);
}

/**
 * rmr_subblk_get_cached() - return a chunk's slot, installing one if missing.
 *
 * @map:	the dirty-id map owning the cache.
 * @chunk:	id.b of the chunk.
 *
 * Description:
 *	An existing slot is returned even when the cache is at its cap; the
 *	cap only limits how many new slots are admitted.
 *	Refuses rather than discards when the cap is reached, so a caller that
 *	gets NULL must converge the chunk instead of relying on a refinement.
 *	A caller that loses the race to install a chunk's first slot frees
 *	its own and returns the winner's, also at the cap.
 *
 * Context:
 *	Call with rcu_read_lock() held; does not sleep.
 *
 * Return:
 *	the existing or new slot on success; NULL when the cap is reached or
 *	on allocation/insertion failure (caller treats the chunk as
 *	FULLY_DIRTY).
 */
struct rmr_subblk_slot *rmr_subblk_get_cached(struct rmr_dirty_id_map *map,
					      u64 chunk)
{
	u32 nr_subblk = 1U << (map->pool->chunk_size_shift - RMR_SUBBLK_SHIFT);
	struct rmr_subblk_slot *slot, *new_slot;
	unsigned long flags;

	slot = rmr_subblk_lookup(map, chunk);
	if (slot)
		return slot;

	new_slot = rmr_subblk_slot_alloc(nr_subblk);
	if (!new_slot)
		return NULL;

	spin_lock_irqsave(&map->subblk_cache_lock, flags);
	/*
	 * Every insert holds this lock, so a slot another caller installed
	 * since the lookup above is found here, before the cap can refuse.
	 */
	slot = xa_load(&map->subblk_cache, chunk);
	if (slot)
		goto unlock;

	/*
	 * Checked under the lock that guards the count, so the cap admits
	 * exactly as many slots as it says. It bounds the slots themselves;
	 * the xarray nodes indexing them are not counted.
	 */
	if (map->subblk_nr >= READ_ONCE(map->pool->subblk_slots_max) ||
	    xa_insert(&map->subblk_cache, chunk, new_slot, GFP_NOWAIT))
		goto unlock;

	map->subblk_nr++;
	slot = new_slot;
unlock:
	spin_unlock_irqrestore(&map->subblk_cache_lock, flags);

	/* Free the new slot unless the cache took it. */
	if (slot != new_slot)
		kfree(new_slot);

	return slot;
}

/* rmr_subblk_remove() with subblk_cache_lock already held. */
static void __rmr_subblk_remove(struct rmr_dirty_id_map *map, u64 chunk)
{
	struct rmr_subblk_slot *s;

	s = xa_erase(&map->subblk_cache, chunk);
	if (!s)
		return;
	map->subblk_nr--;
	kfree_rcu(s, rcu);
}

/**
 * rmr_subblk_remove() - drop the cache slot for @chunk.
 *
 * @map:	the dirty-id map owning the cache.
 * @chunk:	id.b of the chunk whose slot is to be removed.
 *
 * Description:
 *	Used when rmr_map_unset_dirty() clears a chunk, since a slot only
 *	describes a dirty chunk. A re-dirty and a clean verdict do not use it:
 *	each drops the slot in the same subblk_cache_lock section as its
 *	dirty-bit update.
 */
static void rmr_subblk_remove(struct rmr_dirty_id_map *map, u64 chunk)
{
	unsigned long flags;

	spin_lock_irqsave(&map->subblk_cache_lock, flags);
	__rmr_subblk_remove(map, chunk);
	spin_unlock_irqrestore(&map->subblk_cache_lock, flags);
}

/**
 * rmr_subblk_snapshot_bits() - copy a slot's dirty bitmap into @dst_bits.
 *
 * @map:	the dirty-id map owning the slot cache.
 * @chunk:	id.b of the chunk to look up.
 * @dst_bits:	caller-provided buffer; filled under the slot's spinlock.
 * @nr_subblk:	set to the slot's sub-block count on success.
 *
 * Description:
 *	After return the caller may walk @dst_bits without RCU held. The
 *	chunk's sync claim keeps writes from marking sub-blocks in sync behind
 *	the copy, so the walk never under-syncs what was stale. A sub-block
 *	marked stale after the copy is not in it, so finishing the walk alone
 *	does not prove the chunk converged.
 *
 * Context:
 *	Call with rcu_read_lock() and the chunk's sync claim held.
 *
 * Return:
 *	true if a slot existed for @chunk, false otherwise.
 */
bool rmr_subblk_snapshot_bits(struct rmr_dirty_id_map *map, u64 chunk,
			      unsigned long *dst_bits, u32 *nr_subblk)
{
	struct rmr_subblk_slot *s;
	unsigned long flags;

	s = xa_load(&map->subblk_cache, chunk);
	if (!s)
		return false;

	spin_lock_irqsave(&s->lock, flags);
	*nr_subblk = s->nr_subblk;
	bitmap_copy(dst_bits, s->bits, s->nr_subblk);
	spin_unlock_irqrestore(&s->lock, flags);

	return true;
}

/**
 * rmr_subblk_drain_all() - drop every slot from the sub-block cache.
 *
 * @map:	the dirty-id map owning the cache.
 *
 * Description:
 *	Called from teardown and mass-clear paths where all refinement
 *	records are stale. Slots are RCU-freed so concurrent readers
 *	remain safe.
 */
void rmr_subblk_drain_all(struct rmr_dirty_id_map *map)
{
	struct rmr_subblk_slot *s;
	unsigned long index;
	unsigned long flags;
	unsigned int n;

	if (xa_empty(&map->subblk_cache))
		return;

	/*
	 * Bounded batches: the lock is held with interrupts off, and the cap
	 * admits enough slots that erasing all of them in one critical section
	 * would disable interrupts for the length of the walk. Keep going until
	 * a batch comes up short, since dropping the lock between batches lets
	 * a concurrent write admit a slot.
	 */
	do {
		n = 0;
		spin_lock_irqsave(&map->subblk_cache_lock, flags);
		xa_for_each(&map->subblk_cache, index, s) {
			/*
			 * xa_erase takes xa_lock internally; safe under our own
			 * cache lock and inside xa_for_each (the iterator
			 * tolerates removal of the current entry). kfree_rcu()
			 * does not sleep, so the slot is freed here.
			 */
			__rmr_subblk_remove(map, index);
			if (++n == RMR_SUBBLK_DRAIN_BATCH)
				break;
		}
		spin_unlock_irqrestore(&map->subblk_cache_lock, flags);
	} while (n == RMR_SUBBLK_DRAIN_BATCH);
}

/**
 * rmr_map_calc_chunk -	Calculate chunk number from offset and length of IO
 *
 * @pool:		The pool
 * @offset:		Offset of the IO
 * @length:		Length of the IO
 * @id:			rmr_id_t where to populate the chunk details
 *			id.b: chunk number denoted by this entry
 *			id.a: Number of chunks dirty starting (and including) id.b
 *
 *			For example:
 *			if id.a is 1, only id.b is dirty.
 *			if id.a is 2, id.b and (id.b+1) is dirty
 *
 * Context:
 *	srcu pool->map_srcu should be held while calling this function.
 */
void rmr_map_calc_chunk(struct rmr_pool *pool, size_t offset, size_t length, rmr_id_t *id)
{
	u64 off_len = offset + length;

	id->b = GET_CHUNK_NUMBER(offset, pool->chunk_size_shift);
	id->a = GET_FOLLOWING_CHUNKS(off_len, pool->chunk_size_shift, id->b);
}

/**
 * rmr_get_chunk_md_from_id - Get the chunk metadata byte from rmr_id_t
 *
 * @map:	The map to work on
 * @id:		rmr_id_t to use to get the chunk metadata byte
 *
 * Context:
 *	srcu pool->map_srcu should be held while calling this function.
 */
inline u8 *rmr_get_chunk_md_from_id(struct rmr_dirty_id_map *map, rmr_id_t id)
{
	unsigned long idb_slp, idb_slp_index, idb_chunk;
	el_flp *flp_ptr;
	u8 *slp, *chunk_md;

	/*
	 * First get the pointer to first level page (FLP).
	 * To get that, we need to find which first level page the chunk belongs, and it can
	 * be found by dividing the chunk number by the maximum number of chunks 1 FLP can track.
	 *
	 * After that we need to adjust the id.b to go one level down. This is because we just
	 * moved to the desired FLP, and hence that portion of id.b can be dropped.
	 * For this we do the modulo with CHUNKS_PER_FLP.
	 */
	flp_ptr = (el_flp *)(map->dirty_bitmap[id.b >> CHUNKS_PER_FLP_LOG2]);
	idb_slp = id.b & (CHUNKS_PER_FLP - 1);

	/*
	 * Now we need to move to the second level page (SLP).
	 * The addresses to SLPs are stored in the FLP as a list of addresses. Hence we calculate
	 * the desired slp index which has the address to the SLP our chunk md resides in.
	 *
	 * We then adjust our flp_ptr according to the index.
	 * Note that flp_ptr is of type el_flp (flp element), which is unsigned long, since
	 * addresses are of that data type. This lets us move to the slp index easily.
	 */
	idb_slp_index = idb_slp >> CHUNKS_PER_SLP_LOG2;
	flp_ptr += idb_slp_index;

	/*
	 * The location pointed by flp_ptr is storing the address to the SLP we want to move to.
	 * So we dereference it first, and then cast it to relevant pointer (to the chunk metadata
	 * data type, which is u8).
	 *
	 * The last step it to move to the correct chunk metadata in the SLP.
	 *
	 * Each SLP can store metadata for CHUNKS_PER_SLP chunks. So we adjust the idb_slp
	 * accordingly. And then move our slp pointer to the correct chunk metadata byte.
	 */
	slp = (u8 *)(*flp_ptr);
	idb_chunk = idb_slp & (CHUNKS_PER_SLP - 1);
	chunk_md = slp + idb_chunk;

	return chunk_md;
}

static bool rmr_chunk_md_check_dirty(u8 *chunk_md)
{
	return (*chunk_md) & (0x1 << CHUNK_DIRTY_BIT);
}

static void rmr_chunk_md_set_dirty(u8 *chunk_md)
{
	*chunk_md |= (0x1 << CHUNK_DIRTY_BIT);
}

static void rmr_chunk_md_unset_dirty(u8 *chunk_md)
{
	*chunk_md &= ~(0x1 << CHUNK_DIRTY_BIT);
}

/**
 * rmr_subblk_take_if_clean() - classify a chunk's refinement and claim the
 * transition out of PARTIAL.
 *
 * @map:	the dirty-id map owning the cache.
 * @chunk:	id.b of the chunk to classify.
 *
 * Description:
 *	When every sub-block of the cache's current slot is in sync, erases
 *	the slot and clears the chunk's dirty bit in one subblk_cache_lock
 *	section. rmr_map_set_dirty() drops the slot and sets the bit under the
 *	same lock, so a re-dirty shows up here as a missing slot or lands after
 *	the clear. A caller that got CLEAN_TAKEN must not clear the bit again.
 *
 * Return:
 *	RMR_SUBBLK_CLEAN_TAKEN when this call erased an all-clean slot and
 *	cleared the chunk's dirty bit;
 *	RMR_SUBBLK_PARTIAL when sub-blocks are still dirty;
 *	RMR_SUBBLK_ABSENT when no slot is cached for @chunk.
 */
enum rmr_subblk_state rmr_subblk_take_if_clean(struct rmr_dirty_id_map *map,
					       u64 chunk)
{
	rmr_id_t id = { .a = 1, .b = chunk };
	struct rmr_subblk_slot *s;
	enum rmr_subblk_state st;
	unsigned long flags;

	spin_lock_irqsave(&map->subblk_cache_lock, flags);
	s = xa_load(&map->subblk_cache, chunk);
	if (!s) {
		st = RMR_SUBBLK_ABSENT;
	} else if (rmr_subblk_slot_all_clean(s)) {
		__rmr_subblk_remove(map, chunk);
		rmr_chunk_md_unset_dirty(rmr_get_chunk_md_from_id(map, id));
		st = RMR_SUBBLK_CLEAN_TAKEN;
	} else {
		st = RMR_SUBBLK_PARTIAL;
	}

	spin_unlock_irqrestore(&map->subblk_cache_lock, flags);

	return st;
}

/**
 * rmr_map_set_dirty -	Set bits from rmr_id_t
 *
 * @map:		Map to work on
 * @id:			rmr_id_t containing the chunk info
 *			id.b: chunk number denoted by this entry
 *			id.a: Number of chunks dirty starting (and including) id.b
 * @filter:		Filter to add to entry
 *
 *
 * Context:
 *	srcu pool->map_srcu should be held while calling this function.
 */
inline void rmr_map_set_dirty(struct rmr_dirty_id_map *map, rmr_id_t id, u8 filter)
{
	unsigned long flags;
	u8 *chunk_md;
	u64 i;

	map->ts = jiffies;

	/*
	 * A slot records which sub-blocks hold the correct local data, which
	 * stops being true once the chunk is re-dirtied. Drop it and set the
	 * dirty bit in one subblk_cache_lock section, so that
	 * rmr_subblk_take_if_clean() cannot clear the bit over this re-dirty.
	 * A reader that looked the slot up before the drop may still update
	 * it. That is harmless, because no verdict is taken from a detached
	 * slot. rmr_subblk_take_if_clean() judges the chunk by the slot the
	 * cache holds.
	 */
	spin_lock_irqsave(&map->subblk_cache_lock, flags);
	for (i = 0; i < id.a; i++)
		__rmr_subblk_remove(map, id.b + i);

	chunk_md = rmr_get_chunk_md_from_id(map, id);
	for (i = 0; i < id.a; i++) {
		if (i && !((id.b + i) & (CHUNKS_PER_SLP - 1))) {
			rmr_id_t next = { .a = 1, .b = id.b + i };

			chunk_md = rmr_get_chunk_md_from_id(map, next);
		}
		rmr_chunk_md_set_dirty(chunk_md);
		chunk_md++;
	}
	spin_unlock_irqrestore(&map->subblk_cache_lock, flags);
}

inline void rmr_map_set_dirty_all(struct rmr_dirty_id_map *map, u8 filter)
{
	el_flp *flp_ptr;
	u64 no_of_slps, no_of_chunks;
	bool is_last_flp;
	u8 *slp;
	int i, j, k;

	/*
	 * Every cached refinement record is now stale. Drain before setting
	 * the bitmap, so a sync snapshot or a clean verdict taken after the
	 * bits are set cannot trust a slot from before this re-dirty.
	 */
	rmr_subblk_drain_all(map);

	for (i = 0; i < map->no_of_flp; i++) {
		flp_ptr = (el_flp *)map->dirty_bitmap[i];
		is_last_flp = (i == (map->no_of_flp - 1));

		if (is_last_flp)
			no_of_slps = map->no_of_slp_in_last_flp;
		else
			no_of_slps = NO_OF_SLP_PER_FLP;

		for (j = 0; j < no_of_slps; j++, flp_ptr++) {
			slp = (u8 *)(*flp_ptr);

			if (is_last_flp && j == (no_of_slps - 1))
				no_of_chunks = map->no_of_chunk_in_last_slp;
			else
				no_of_chunks = NO_OF_CHUNKS_PER_PAGE;

			for (k = 0; k < no_of_chunks; k++, slp++)
				rmr_chunk_md_set_dirty(slp);
		}
	}
}

/**
 * rmr_map_unset_dirty - Clear bits from rmr_id_t, and free entry if any
 *
 * @map:		Map to work on
 * @id:			rmr_id_t containing the chunk info
 *			id.b: chunk number denoted by this entry
 *			id.a: Number of chunks dirty starting (and including) id.b
 * @filter:		Filter to add to entry
 *
 * Description:
 *	This version can be used by both client and server.
 *	If entry is found, the function frees it.
 *	Clears the bit using info from the given rmr_id_t
 *
 * Context:
 *	srcu pool->map_srcu should be held while calling this function.
 */
inline struct rmr_map_entry *rmr_map_unset_dirty(struct rmr_dirty_id_map *map, rmr_id_t id,
						 u8 filter)
{
	struct rmr_map_entry *entry;
	unsigned long flags;
	u8 *chunk_md;
	u64 i;

	map->ts = jiffies;

	chunk_md = rmr_get_chunk_md_from_id(map, id);
	BUG_ON(!chunk_md);
	for (i = 0; i < id.a; i++) {
		if (i && !((id.b + i) & (CHUNKS_PER_SLP - 1))) {
			rmr_id_t next = { .a = 1, .b = id.b + i };

			chunk_md = rmr_get_chunk_md_from_id(map, next);
		}
		rmr_chunk_md_unset_dirty(chunk_md);
		chunk_md++;
	}

	/*
	 * Drop any cached sub-block refinement for every chunk being
	 * cleaned. A slot only describes a dirty chunk; once clean, the
	 * slot would survive into the next dirty cycle and falsely claim
	 * some sub-blocks are in sync. Centralized here so every caller
	 * is correct by construction.
	 */
	for (i = 0; i < id.a; i++)
		rmr_subblk_remove(map, id.b + i);

	xa_lock_irqsave(&map->rmr_id_map, flags);
	entry = __xa_erase(&map->rmr_id_map, rmr_id_to_key(id));
	xa_unlock_irqrestore(&map->rmr_id_map, flags);
	if (!entry) {
		pr_debug("in the member_id %d there is no entry for id [%llu, %llu]\n",
			 map->member_id, id.a, id.b);
	}

	return entry;
}

/**
 * rmr_map_erase_entry() - drop a chunk's entry and leave its dirty bit alone.
 *
 * @map:	map to work on.
 * @id:		the chunk.
 *
 * Description:
 *	For a caller that took the chunk clean with rmr_subblk_take_if_clean(),
 *	which already cleared the bit. Clearing it again could erase a re-dirty
 *	that landed since.
 */
void rmr_map_erase_entry(struct rmr_dirty_id_map *map, rmr_id_t id)
{
	unsigned long flags;

	xa_lock_irqsave(&map->rmr_id_map, flags);
	__xa_erase(&map->rmr_id_map, rmr_id_to_key(id));
	xa_unlock_irqrestore(&map->rmr_id_map, flags);
}

/*
 * rmr_map_check_dirty - Check if the following bits are set or not
 *
 * @map:		Map to work on
 * @id:			rmr_id_t containing the chunk info
 *			id.b: chunk number denoted by this entry
 *			id.a: Number of chunks dirty starting (and including) id.b
 *
 * Context:
 *	srcu pool->map_srcu should be held while calling this function.
 */
inline bool rmr_map_check_dirty(struct rmr_dirty_id_map *map, rmr_id_t id)
{
	u8 *chunk_md;

	chunk_md = rmr_get_chunk_md_from_id(map, id);
	return rmr_chunk_md_check_dirty(chunk_md);
}

/**
 * rmr_map_get_dirty_entry - Check and return entry if the following bits are set
 *
 * @map:		Map to work on
 * @id:			rmr_id_t containing the chunk info
 *			id.b: chunk number denoted by this entry
 *			id.a: Number of chunks dirty starting (and including) id.b
 *
 * Description:
 *	Check if a chunk is dirty or not.
 *	If the particular chunk is dirty, then create an entry for it and return back.
 *
 * Context:
 *	srcu pool->map_srcu should be held while calling this function.
 */
inline struct rmr_map_entry *rmr_map_get_dirty_entry(struct rmr_dirty_id_map *map, rmr_id_t id)
{
	struct rmr_map_entry *entry;
	int err;

	if (rmr_map_check_dirty(map, id)) {
		entry = xa_load(&map->rmr_id_map, rmr_id_to_key(id));
		if (entry) {
			pr_debug("%s: For id [%llu, %llu], entry exists member_id %u\n",
				 __func__, id.a, id.b, map->member_id);
			return entry;
		}

		entry = kmem_cache_zalloc(rmr_map_entry_cachep, GFP_KERNEL);
		if (!entry) {
			pr_err("%s: Cannot allocate entry for member_id %d, id [[%llu, %llu]]\n",
			       __func__, map->member_id, id.a, id.b);
			return ERR_PTR(-ENOMEM);
		}

		atomic_set(&entry->sync_cnt, -1);
		init_llist_head(&entry->wait_list);

		err = xa_insert_irq(&map->rmr_id_map, rmr_id_to_key(id), entry, GFP_KERNEL);
		if (err == 0)
			return entry;

		kmem_cache_free(rmr_map_entry_cachep, entry);

		if (err == -EBUSY)
			return xa_load(&map->rmr_id_map, rmr_id_to_key(id));
		else
			return ERR_PTR(-ENOMEM);
	}

	return NULL;
}

/**
 * rmr_map_clear_filter_all - Clear filter for entire bitmap
 *
 * @map:       Map to work on
 * @filter:    Filter to be cleared
 *
 * Context:
 *	srcu pool->map_srcu should be held while calling this function.
 */
inline void rmr_map_clear_filter_all(struct rmr_dirty_id_map *map, u8 filter)
{
	u64 i;

	for (i = 0; i < map->no_of_chunks; i++)
		map->bitmap_filter[i] &= ~filter;
}

/**
 * rmr_map_unset_dirty_all - Clear all chunk bits (the entire map)
 *
 * @map:       Map to work on
 *
 * Context:
 *	srcu pool->map_srcu should be held while calling this function.
 */
inline void rmr_map_unset_dirty_all(struct rmr_dirty_id_map *map)
{
	el_flp *flp_ptr;
	u64 no_of_slps;
	bool is_last_flp;
	void *slp;
	int i, j;

	map->ts = jiffies;

	/*
	 * All chunk metadata lives in the SLP pages, so clear each SLP page in
	 * one shot rather than walking chunk by chunk. The trailing bytes of
	 * the last SLP track no chunks and are already zero, so memset-ing the
	 * full page is harmless.
	 */
	for (i = 0; i < map->no_of_flp; i++) {
		flp_ptr = (el_flp *)map->dirty_bitmap[i];
		is_last_flp = (i == (map->no_of_flp - 1));

		if (is_last_flp)
			no_of_slps = map->no_of_slp_in_last_flp;
		else
			no_of_slps = NO_OF_SLP_PER_FLP;

		for (j = 0; j < no_of_slps; j++, flp_ptr++) {
			slp = (void *)(*flp_ptr);
			memset(slp, 0, PAGE_SIZE);
		}
	}

	/*
	 * Drop all id->entry mappings. This matches the per-chunk path, which
	 * erased each entry from the xarray without freeing it.
	 */
	xa_destroy(&map->rmr_id_map);

	/* Every chunk is now clean; any cached refinement state is stale. */
	rmr_subblk_drain_all(map);

	rmr_map_clear_filter_all(map, MAP_ENTRY_UNSYNCED);
}

/**
 * rmr_map_empty - Check if there are any chunks dirty
 *
 * @map:       Map to work on
 *
 * Return:
 *	True:	If map is empty
 *	False:	Otherwise
 *
 * Context:
 *	srcu pool->map_srcu should be held while calling this function.
 */
inline bool rmr_map_empty(struct rmr_dirty_id_map *map)
{
	el_flp *flp_ptr;
	u64 no_of_slps, no_of_chunks;
	bool is_last_flp;
	u8 *slp;
	int i, j, k;

	for (i = 0; i < map->no_of_flp; i++) {
		flp_ptr = (el_flp *)map->dirty_bitmap[i];
		is_last_flp = (i == (map->no_of_flp - 1));

		if (is_last_flp)
			no_of_slps = map->no_of_slp_in_last_flp;
		else
			no_of_slps = NO_OF_SLP_PER_FLP;

		for (j = 0; j < no_of_slps; j++, flp_ptr++) {
			slp = (u8 *)(*flp_ptr);

			if (is_last_flp && j == (no_of_slps - 1))
				no_of_chunks = map->no_of_chunk_in_last_slp;
			else
				no_of_chunks = NO_OF_CHUNKS_PER_PAGE;

			for (k = 0; k < no_of_chunks; k++, slp++) {
				if (rmr_chunk_md_check_dirty(slp))
					return false;
			}
		}
	}

	return true;
}

inline void rmr_map_bitwise_or_buf(void *dst_buf, void *src_buf, u32 buf_size)
{
	u8 *src_byte, *dst_byte;

	src_byte = src_buf;
	dst_byte = dst_buf;

	while (buf_size--)
		*(dst_byte + buf_size) |= *(src_byte + buf_size);
}

inline int rmr_map_create_entries(struct rmr_dirty_id_map *map)
{
	struct rmr_map_entry *entry;
	rmr_id_t id;
	int err;
	u64 i;

	id.a = 1;
	for (i = 0; i < map->no_of_chunks; i++) {
		id.b = i;

		if (!rmr_map_check_dirty(map, id))
			continue;

		if (xa_load(&map->rmr_id_map, rmr_id_to_key(id)))
			continue;

		entry = kmem_cache_zalloc(rmr_map_entry_cachep, GFP_KERNEL);
		if (!entry) {
			pr_err("%s: Cannot allocate entry for member_id %d, chunk %llu\n",
			       __func__, map->member_id, i);
			return -ENOMEM;
		}

		atomic_set(&entry->sync_cnt, -1);
		init_llist_head(&entry->wait_list);

		pr_debug("%s: Adding entry %p for chunk %llu\n",
			 __func__, entry, i);

		err = xa_insert_irq(&map->rmr_id_map, rmr_id_to_key(id), entry, GFP_KERNEL);
		if (err) {
			pr_err("%s: Cannot insert entry for member_id %d, chunk %llu\n",
			       __func__, map->member_id, i);
			return err;
		}
	}

	return 0;
}

/**
 * rmr_map_slps_to_buf - Copy SLPs to given buf
 *
 * @map:	Map to work on
 * @slp_idx:	SLP number to start copying from
 * @no_of_slp:	Number of SLPs to copy
 * @buf:	Buffer to copy SLPs to
 *
 * Context:
 *     srcu pool->map_srcu should be held while calling this function.
 */
void rmr_map_slps_to_buf(struct rmr_dirty_id_map *map, u64 slp_idx, u64 no_of_slp, u8 *buf)
{
	el_flp *flp_ptr;
	u64 slp_no, flp_no, i = 0;
	void *slp;

	flp_no = slp_idx >> NO_OF_SLP_PER_FLP_LOG2;
	slp_no = slp_idx & (NO_OF_SLP_PER_FLP - 1);

	flp_ptr = (el_flp *)map->dirty_bitmap[flp_no];
	while (i < no_of_slp) {
		slp = (void *)(*(flp_ptr + slp_no));

		memcpy(buf, slp, PAGE_SIZE);
		buf += PAGE_SIZE;

		slp_no++;
		if (slp_no >= NO_OF_SLP_PER_FLP) {
			flp_no += 1;
			slp_no = 0;

			flp_ptr = (el_flp *)map->dirty_bitmap[flp_no];
		}

		i++;
	}

	return;
}

/**
 * rmr_map_buf_to_slps - Copy data from buf to SLPs
 *
 * @map:	Map to work on
 * @buf:	Buffer from which to copy data
 * @buf_size:	Buffer size
 * @slp_idx:	SLP number to start copying to
 * @test:	Whether to compare data or copy
 *
 * Return:
 *	Number of SLPs to which data was copied.
 *	0 in case of failure.
 *
 * Context:
 *     srcu pool->map_srcu should be held while calling this function.
 */
u64 rmr_map_buf_to_slps(struct rmr_dirty_id_map *map, u8 *buf, u32 buf_size, u64 slp_idx,
			bool test)
{
	el_flp *flp_ptr;
	u64 slp_no, flp_no, i = 0;
	u64 no_of_slp;
	void *slp;

	/*
	 * The buf_size should be a factor of PAGE_SIZE
	 */
	if (buf_size % PAGE_SIZE) {
		pr_info("%s: Failed %u\n", __func__, buf_size);
		return 0;
	}

	no_of_slp = buf_size >> PAGE_SHIFT;

	flp_no = slp_idx >> NO_OF_SLP_PER_FLP_LOG2;
	slp_no = slp_idx & (NO_OF_SLP_PER_FLP - 1);

	pr_info("%s: no_of_slp=%llu, flp_no=%llu, slp_no=%llu, slp_idx=%llu\n",
		__func__, no_of_slp, flp_no, slp_no, slp_idx);
	flp_ptr = (el_flp *)map->dirty_bitmap[flp_no];
	while (i < no_of_slp) {
		slp = (void *)(*(flp_ptr + slp_no));

		if (test && memcmp(slp, buf, PAGE_SIZE)) {
			pr_info("%s: Compare failed\n", __func__);
			return 0;
		} else if (!test) {
			memcpy(slp, buf, PAGE_SIZE);
		}
		buf += PAGE_SIZE;

		slp_no++;
		if (slp_no >= NO_OF_SLP_PER_FLP) {
			flp_no += 1;
			slp_no = 0;

			flp_ptr = (el_flp *)map->dirty_bitmap[flp_no];
		}

		i++;
	}

	return no_of_slp;
}

void rmr_map_hexdump_bitmap_buf(u8 member_id, void *buf, u32 buf_size)
{
	u8 *buf_byte;
	u32 size = 0;

	buf_byte = buf;

	pr_info("%s: Starting bitmap dump for member %u in hex, size %u\n",
		__func__, member_id, buf_size);
	pr_info("---------------------------------------------------------\n");
	while (size < buf_size) {
		pr_cont("%02X", *(buf_byte + size));
		size++;
	}

	pr_info("\n");
}

void rmr_map_dump_bitmap(struct rmr_dirty_id_map *map)
{
	el_flp *flp_ptr;
	u64 no_of_slps, no_of_chunks;
	bool is_last_flp;
	u8 *slp;
	int i, j;

	for (i = 0; i < map->no_of_flp; i++) {
		flp_ptr = (el_flp *)map->dirty_bitmap[i];
		is_last_flp = (i == (map->no_of_flp - 1));

		if (is_last_flp)
			no_of_slps = map->no_of_slp_in_last_flp;
		else
			no_of_slps = NO_OF_SLP_PER_FLP;

		for (j = 0; j < no_of_slps; j++, flp_ptr++) {
			slp = (u8 *)(*flp_ptr);

			if (is_last_flp && j == (no_of_slps - 1))
				no_of_chunks = map->no_of_chunk_in_last_slp;
			else
				no_of_chunks = NO_OF_CHUNKS_PER_PAGE;

			/* Each chunk is represented by a byte */
			rmr_map_hexdump_bitmap_buf(map->member_id, slp, no_of_chunks);
		}
	}
}

/**
 * rmr_map_summary_format - Format a per-member dirty-chunk summary into buf
 *
 * @pool:	Pool whose maps to summarise
 * @buf:	Output buffer (must be at least @buf_size bytes)
 * @buf_size:	Size of @buf in bytes
 *
 * Description:
 *	Output format (one line per member that has a map):
 *	member <id>: [<idx0> <idx1> ...] <dirty_count>/<total> dirty
 *	At most 50 dirty chunk indices are listed per member; if there
 *	are more, a "..." marker appears before the closing bracket.
 *
 * Context: caller must hold srcu pool->map_srcu.
 *
 * Return: number of bytes written (excluding trailing NUL).
 */
int rmr_map_summary_format(struct rmr_pool *pool, char *buf, size_t buf_size)
{
	struct rmr_dirty_id_map *map;
	el_flp *flp_ptr;
	u64 no_of_slps, no_of_chunks_in_slp;
	u64 chunk_idx, dirty_count;
	bool is_last_flp;
	u8 *slp;
	int printed_ids;
	int pos = 0;
	int i, fi, si;

	for (i = 0; i < RMR_POOL_MAX_SESS; i++) {
		map = rcu_dereference(pool->maps[i]);
		if (!map)
			continue;

		pos += scnprintf(buf + pos, buf_size - pos,
				 "member %u: [", map->member_id);

		dirty_count = 0;
		chunk_idx = 0;
		printed_ids = 0;
		for (fi = 0; fi < map->no_of_flp; fi++) {
			flp_ptr = (el_flp *)map->dirty_bitmap[fi];
			is_last_flp = (fi == (map->no_of_flp - 1));
			no_of_slps = is_last_flp ?
				map->no_of_slp_in_last_flp : NO_OF_SLP_PER_FLP;

			for (si = 0; si < no_of_slps; si++, flp_ptr++) {
				u64 ci;

				slp = (u8 *)(*flp_ptr);
				no_of_chunks_in_slp =
					(is_last_flp && si == (no_of_slps - 1)) ?
					map->no_of_chunk_in_last_slp :
					NO_OF_CHUNKS_PER_PAGE;

				for (ci = 0; ci < no_of_chunks_in_slp;
				     ci++, chunk_idx++) {
					if (!(slp[ci] & (1 << CHUNK_DIRTY_BIT)))
						continue;
					dirty_count++;
					/* Cap listed IDs to fit all members in PAGE_SIZE */
					if (printed_ids < 50) {
						pos += scnprintf(buf + pos,
								 buf_size - pos,
								 "%llu ", chunk_idx);
						printed_ids++;
					}
				}
			}
		}

		/* Overwrite trailing space before ']' */
		if (pos > 0 && buf[pos - 1] == ' ')
			pos--;
		if (printed_ids < dirty_count)
			pos += scnprintf(buf + pos, buf_size - pos,
					 "...] %llu/%llu dirty\n",
					 dirty_count, map->no_of_chunks);
		else
			pos += scnprintf(buf + pos, buf_size - pos,
					 "] %llu/%llu dirty\n",
					 dirty_count, map->no_of_chunks);
	}

	return pos;
}

void rmr_map_bidump_bitmap_buf(void *buf, u8 member_id, u32 buf_long)
{
	char box[65];
	u64 *buf_byte;
	u64 the_byte;
	int i, j;
	u32 count = 0;

	buf_byte = buf;

	pr_info("%s: bitmap for member %d dump in binary, the size in longs %u\n",
		__func__, member_id, buf_long);
	while (count < buf_long) {
		the_byte = *(buf_byte + count);
		for (i = 63, j = 0; i >= 0; i--, j++)
			box[j] = (the_byte & (1ULL << i)) ? '1' : '0';
		box[j] = '\0';
		pr_cont("[%s]", box);
		count++;
	}

	pr_info("\n");
	pr_info("---------------------------------------------------------\n");
}
