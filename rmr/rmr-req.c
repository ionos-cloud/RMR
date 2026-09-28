// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Reliable multicast over RTRS (RMR)
 *
 * Copyright (c) 2026 IONOS SE
 */

#include <linux/slab.h>

#include "rmr-req.h"
#include "rmr-srv.h"
#include "rmr-clt.h"

extern struct kmem_cache *rmr_req_cachep;
extern struct kmem_cache *rmr_map_entry_cachep;
extern struct rmr_store_ops *pstore_ops;

static void rmr_req_complete(struct rmr_srv_req *req);
static void rmr_req_store_done(struct rmr_srv_req *req);
static void rmr_req_sync_failed(struct rmr_srv_req *req);
static void rmr_req_send_map_clear(struct rmr_srv_req *req);
static void rmr_req_sync_complete(struct rmr_srv_req *req);
static void rmr_req_store(struct rmr_srv_req *req);
static void rmr_req_fast_write_done(struct rmr_srv_req *req);

/**
 * rmr_srv_req_resp - Response from the lower level module
 *
 * @req:	Request to be processed
 * @err:	Error value
 *
 * Description:
 *	This function is the return point from the below module
 *	where IO is submitted.
 *
 * Context:
 *	In this function the request should always be in state RMR_REQ_STATE_STORE
 */
void rmr_srv_req_resp(struct rmr_srv_req *req, int err)
{
	/*
	 * Use the error sent from lower layer
	 */
	req->err = err;

	/*
	 * For Normal (non-sync) requests we handle both non-error and error cases from one
	 * place. Since its simple.
	 */
	if (rmr_op(req->flags) != RMR_OP_SYNCREQ) {
		if (req->fast_write)
			rmr_req_fast_write_done(req);
		else
			rmr_req_complete(req);
		return;
	}

	/*
	 * Sync requests are complicated, since it needs extra post-processing
	 * once IO is done for us.
	 *
	 * 1) In case of no failure, we need to send map clear to other nodes,
	 *    since they think we are still dirty for this chunk.
	 *
	 * 2) We need to check for waiting IO in entry->wait_list, and kick them.
	 */
	if (!req->err)
		rmr_req_store_done(req);
	else
		rmr_req_sync_failed(req);
}
EXPORT_SYMBOL(rmr_srv_req_resp);

/**
 * rmr_srv_req_create - Create an rmr server request
 *
 * @msg:	IO message containing information
 * @srv_pool:	Server pool creating this request
 * @rtrs_op:	rtrs IO context
 * @data:	pointer to data buf
 * @datalen:	len of data buf
 * @endreq:	Function to be called at the end of rmr request processing
 *
 * Description:
 *	RMR server request are base structures which holds the IO while they are being processed.
 *	They go through a state machine, while a number of checks are done. IOs which are
 *	destined for a chunk that is dirty, are paused while that chunk is synced.
 *
 * Return:
 *	Pointer to the create rmr server request on success
 *	Error pointer on failure
 */
struct rmr_srv_req *rmr_srv_req_create(const struct rmr_msg_io *msg, struct rmr_srv_pool *srv_pool,
				       struct rtrs_srv_op *rtrs_op, void *data, u32 datalen,
				       void (*endreq)(struct rmr_srv_req *, int))
{
	struct rmr_srv_req *req;
	struct rmr_srv_io_store *store = srv_pool->io_store;
	int i;

	if (!store || !atomic_read(&srv_pool->store_state)) {
		pr_err("%s: store not set, or srv_pool not in correct state %s\n",
		       __func__, srv_pool->pool->poolname);
		return ERR_PTR(-ENODEV);
	}

	req = kmem_cache_zalloc(rmr_req_cachep, GFP_KERNEL);
	if (!req) {
		pr_err("cannot allocate memory for rmr_req.\n");
		return ERR_PTR(-ENOMEM);
	}

	req->id.a = le64_to_cpu(msg->id_a);
	req->id.b = le64_to_cpu(msg->id_b);

	req->offset = le64_to_cpu(msg->offset);
	req->length = le32_to_cpu(msg->length);
	req->flags = le32_to_cpu(msg->flags);
	req->prio = le16_to_cpu(msg->prio);

	req->mem_id = le32_to_cpu(msg->mem_id);
	for (i = 0; i < msg->failed_cnt; i++)
		req->failed_srv_id[i] = msg->failed_id[i];

	req->failed_cnt = msg->failed_cnt;
	req->map_ver = le64_to_cpu(msg->map_ver);
	req->sync = msg->sync;

	req->data = data;
	req->datalen = datalen;
	req->rtrs_op = rtrs_op;
	req->srv_pool = srv_pool;
	req->store = store;
	req->endreq = endreq;

	pr_debug("req %p, chunk_size %u\n", req, req->srv_pool->pool->chunk_size);

	return req;
}

struct rmr_srv_req *rmr_srv_md_req_create(struct rmr_srv_pool *srv_pool,
					  struct rtrs_srv_op *rtrs_op, void *data,
					  u32 offset, u32 len, unsigned long flags,
					  void (*endreq)(struct rmr_srv_req *, int))
{
	struct rmr_srv_req *req;
	struct rmr_srv_io_store *store = srv_pool->io_store;

	if (!store) {
		pr_err("No store_id registered for srv pool %s\n", srv_pool->pool->poolname);
		return ERR_PTR(-ENODEV);
	}

	req = kmem_cache_zalloc(rmr_req_cachep, GFP_KERNEL);
	if (!req) {
		pr_err("cannot allocate memory for rmr_req.\n");
		return ERR_PTR(-ENOMEM);
	}
	req->offset = offset;
	req->length = len;
	req->flags = flags;
	req->sync = false; /* A md req is always non-sync */

	req->data = data;
	req->rtrs_op = rtrs_op;
	req->srv_pool = srv_pool;
	req->store = store;
	req->endreq = endreq;

	pr_debug("md req %p, len %u\n", req, len);

	return req;
}

void rmr_req_submit(struct rmr_srv_req *req);
static void rmr_req_sched(struct work_struct *work)
{
	struct rmr_srv_req *req = container_of(work, struct rmr_srv_req, work);

	pr_debug("scheduled work process for req %p\n", req);
	if (req->err)
		rmr_req_complete(req);
	else
		rmr_req_submit(req);
}

void rmr_process_wait_list(struct rmr_map_entry *entry, int err)
{
	struct rmr_srv_req *req, *next;
	struct llist_node *first;

	pr_debug("processing wait list for entry %p, sync_cnt=%d\n",
		 entry, atomic_read(&entry->sync_cnt));

	while (!llist_empty(&entry->wait_list)) {
		first = llist_del_all(&entry->wait_list);
		llist_for_each_entry_safe(req, next, first, node) {
			/* The sync thread decides what its chunk still needs. */
			if (req->sync_waiter) {
				rmr_srv_sync_waiter_ready(req);
				continue;
			}

			pr_debug("process waiting req %p id (%llu, %llu) flags %u\n",
				 req, req->id.a, req->id.b, req->flags);
			if (err) {
				pr_err("fail waiting req %p id (%llu, %llu) flags %u err %d\n",
				       req, req->id.a, req->id.b, req->flags, err);
				req->err = -EIO;
			}

			pr_debug("schedule processing req %p with err %d\n", req, req->err);
			INIT_WORK(&req->work, rmr_req_sched);
			schedule_work(&req->work);
		}
	}
}

/**
 * __rmr_req_release_claim() - let go of a chunk's claim and wake what parked
 * behind it, leaving parked_for_sync as it is.
 *
 * @entry:	the chunk's map entry; the caller holds its claim.
 * @err:	reported to the parked requests, or 0 to let them retry.
 *
 * Description:
 *	Once the claim is released, another CPU can take it, drive the chunk
 *	clean and free @entry through call_rcu(), so the release and the
 *	wake-up run under rcu_read_lock(). The release is a fully ordered
 *	atomic_xchg(), and a request that parks adds itself to the wait_list
 *	before it tries the claim, so the wake-up below either finds that
 *	request or that request finds the claim free.
 */
static void __rmr_req_release_claim(struct rmr_map_entry *entry, int err)
{
	rcu_read_lock();
	atomic_xchg(&entry->sync_cnt, -1);
	rmr_process_wait_list(entry, err);
	rcu_read_unlock();
}

/**
 * rmr_req_release_claim() - let go of a chunk a sync claimed and wake what
 * parked behind it.
 *
 * @entry:	the chunk's map entry; the caller holds its sync claim.
 * @err:	reported to the parked requests, or 0 to let them retry.
 *
 * Description:
 *	Clears parked_for_sync, since the parked requests come back now and
 *	any that still need a sync set it again, then releases the claim
 *	through __rmr_req_release_claim().
 */
void rmr_req_release_claim(struct rmr_map_entry *entry, int err)
{
	WRITE_ONCE(entry->parked_for_sync, false);
	__rmr_req_release_claim(entry, err);
}

/**
 * rmr_req_fully_covers_subblks() - check whether a write fully covers whole
 * sub-blocks.
 *
 * @req:	the incoming request.
 *
 * Return:
 *	true if every byte of @req falls within completely covered sub-blocks.
 */
static bool rmr_req_fully_covers_subblks(struct rmr_srv_req *req)
{
	return req->length &&
	       !(req->offset & (RMR_SUBBLK_SIZE - 1)) &&
	       !(req->length & (RMR_SUBBLK_SIZE - 1));
}

void rmr_req_submit(struct rmr_srv_req *req)
{
	struct rmr_srv_pool *srv_pool = req->srv_pool;
	struct rmr_map_entry *entry;
	struct rmr_dirty_id_map *map;
	bool needs_sync, claimed;
	rmr_id_t id;
	int ret;

	if (rmr_op(req->flags) == RMR_OP_FLUSH && !req->length) {
		rmr_req_store(req);
		return;
	}

	pr_debug("check map for req %p flag %u request id [%llu, %llu] offset %llu length %u\n",
		 req, req->flags,
		 req->id.a, req->id.b, req->offset, req->length);

	map = rmr_pool_find_map(srv_pool->pool, srv_pool->member_id);
	if (!map) {
		pr_err("no map found for pool_id %u\n", srv_pool->member_id);
		req->err = -EINVAL;
		goto err;
	}

	rcu_read_lock();
	entry = rmr_map_get_dirty_entry_rcu(map, req->id);
	if (IS_ERR(entry)) {
		rcu_read_unlock();
		req->err = PTR_ERR(entry);
		goto err;
	}

	if (!entry) {
		/*
		 * The chunk containing data for this req is NOT dirty for us
		 */
		pr_debug("check map for req %p flags %u request id [%llu, %llu], no entry in the map\n",
			 req, req->flags, req->id.a, req->id.b);
		rcu_read_unlock();
		rmr_req_store(req);
		return;
	}

	/*
	 * The chunk for this data is dirty for us.
	 *
	 * we have 2 cases.
	 *
	 * 1) Its coming from a sync rmr-clt (Its an internal read).
	 * Then, fail the IO, since we do not want to end up in a deadlock,
	 * or go through multiple hops for a single read. The sender can try
	 * some other node itself.
	 */
	if (req->sync) {
		WARN_ON(rmr_op(req->flags) != RMR_OP_READ);
		rcu_read_unlock();
		req->err = -EIO;
		goto err;
	}

	/*
	 * 2a) Write fast-path. If this non-sync write fully covers
	 *     whole sub-blocks of a single dirty chunk, and no sync
	 *     is in flight, apply it straight to the backend and mark
	 *     those sub-blocks in-sync, skipping syncing the data.
	 *     Partial/unaligned writes, multi-chunk writes, and reads
	 *     fall through to the park-and-sync path below. A write
	 *     whose fan-out already lost a peer leg (failed_cnt) also
	 *     falls through: it must converge the whole chunk so a
	 *     complete replica exists before the ack, which only the
	 *     park path does. A covering write also parks while a
	 *     request that needs a sync is parked, so a stream of such
	 *     writes cannot keep the claim from it.
	 */
	needs_sync = req->id.a != 1 || req->failed_cnt ||
		     rmr_op(req->flags) != RMR_OP_WRITE ||
		     !rmr_req_fully_covers_subblks(req);
	if (!needs_sync && !READ_ONCE(entry->parked_for_sync) &&
	    atomic_cmpxchg(&entry->sync_cnt, -1, 0) == -1) {
		/*
		 * Secure the refinement before the write lands. Without
		 * a slot recording which sub-blocks this node holds,
		 * the next whole-chunk resync would pull them from a
		 * peer that may not hold the same write.
		 */
		if (rmr_subblk_get_cached(map, req->id.b)) {
			rcu_read_unlock();
			req->priv = entry;
			req->fast_write = true;
			pr_debug("%s: fast write req %p id (%llu %llu) off %llu len %u\n",
				 __func__, req, req->id.a, req->id.b,
				 req->offset, req->length);
			rmr_req_store(req);
			return;
		}

		/*
		 * No slot, because the cache is at its cap or the slot
		 * could not be allocated or inserted. Without one this
		 * chunk gets no refinement and the fast path is not
		 * available to it. Hand the claim back and fall through
		 * to park-and-converge, which is what every write on a
		 * dirty chunk did before the fast path existed. Kick the
		 * wait_list because reqs may have parked while we held
		 * the claim.
		 */
		__rmr_req_release_claim(entry, 0);
		needs_sync = true;
	}

	/*
	 * 2b) Park on the chunk's wait_list and trigger a demand sync
	 *     from the clean peer. A parked req belongs to whoever kicks
	 *     the wait_list, so it is filled in before it is added.
	 */
	if (needs_sync)
		WRITE_ONCE(entry->parked_for_sync, true);
	req->priv = entry;
	id = req->id;
	pr_debug("%s: req %p flags %u id (%llu %llu) added to wait list. sync_cnt %d\n",
		 __func__, req, req->flags, id.a, id.b,
		 atomic_read(&entry->sync_cnt));
	llist_add(&req->node, &entry->wait_list);

	/* Winning the claim pins the entry, so RCU can end before the sync. */
	claimed = atomic_cmpxchg(&entry->sync_cnt, -1, 0) == -1;
	rcu_read_unlock();
	if (!claimed)
		return;

	ret = rmr_srv_sync_chunk_id(srv_pool, entry, id, false);
	if (ret)
		rmr_req_release_claim(entry, ret);

	return;

err:
	rmr_req_complete(req);
}

static void rmr_req_store(struct rmr_srv_req *req)
{
	int err;

	pr_debug("submit to store req %p flags %u request id [%llu, %llu] offset %llu length %u\n",
		 req, req->flags,
		 req->id.a, req->id.b, req->offset, req->length);

	err = req->store->ops->submit_req(req->store->priv, req->data, req->offset,
					  req->length, req->flags, req->prio, req);
	if (err) {
		pr_err("%s: error submitting req %p, err %d\n", __func__, req, err);
		/* A failed submit completes the same way as a failed store. */
		rmr_srv_req_resp(req, err);
	}
}

/**
 * rmr_map_entry_free_rcu() - RCU callback to wake parked reqs and free a
 * chunk map entry.
 *
 * @head:	the rcu_head embedded in the map entry.
 *
 * Description:
 *	The entry is erased from the map before this is queued, but a reader
 *	that looked it up earlier may still hold the pointer inside its
 *	rcu_read_lock section and park on the wait_list there. Draining the
 *	wait_list here, after every such reader has finished, wakes it.
 */
static void rmr_map_entry_free_rcu(struct rcu_head *head)
{
	struct rmr_map_entry *entry =
		container_of(head, struct rmr_map_entry, rcu);

	rmr_process_wait_list(entry, 0);
	kmem_cache_free(rmr_map_entry_cachep, entry);
}

/**
 * rmr_req_free_idle_entries() - free a map's entries that no request holds.
 *
 * @map:	a map about to be reset or removed.
 *
 * Description:
 *	A chunk that stays dirty keeps its entry after its holder lets go, and
 *	resetting or removing the map drops the index without freeing what it
 *	holds. Each entry nobody holds is claimed, erased and freed after a
 *	grace period, which also wakes a request that parked on it meanwhile.
 *	An entry a request holds is left to that request.
 */
void rmr_req_free_idle_entries(struct rmr_dirty_id_map *map)
{
	struct rmr_map_entry *entry;
	unsigned long index, flags;

	rcu_read_lock();
	xa_for_each(&map->rmr_id_map, index, entry) {
		if (atomic_cmpxchg(&entry->sync_cnt, -1, 0) != -1)
			continue;
		xa_lock_irqsave(&map->rmr_id_map, flags);
		__xa_erase(&map->rmr_id_map, index);
		xa_unlock_irqrestore(&map->rmr_id_map, flags);
		call_rcu(&entry->rcu, rmr_map_entry_free_rcu);
	}
	rcu_read_unlock();
}

/**
 * rmr_req_fast_write_done() - completion handler for a write fast-path store.
 *
 * @req:	the fast-path write request; req->priv holds the map entry.
 *
 * Description:
 *	On success, marks the covered sub-blocks in-sync via the bounded
 *	slot cache. If the chunk's dirty set is now empty, clears the coarse
 *	dirty bit and frees the entry after an RCU grace period. Otherwise
 *	releases the per-chunk claim so remaining dirty sub-blocks can be
 *	synced or overwritten.
 */
static void rmr_req_fast_write_done(struct rmr_srv_req *req)
{
	struct rmr_srv_pool *srv_pool = req->srv_pool;
	struct rmr_map_entry *entry = req->priv;
	struct rmr_dirty_id_map *map;
	struct rmr_subblk_slot *slot;
	enum rmr_subblk_state state;
	u32 first, count;
	bool clean = false;
	int lock_idx;

	if (req->err)
		goto out;

	/*
	 * rmr_pool_find_map() hands back the pointer with no protection, and
	 * rmr_pool_remove_map() frees the map page once
	 * synchronize_srcu(&pool->map_srcu) has passed. It does not wait on
	 * pool->ids_inflight_ref, so being an in-flight I/O does not pin the
	 * map. Hold map_srcu for as long as we touch it.
	 */
	lock_idx = srcu_read_lock(&srv_pool->pool->map_srcu);
	map = rmr_pool_find_map(srv_pool->pool, srv_pool->member_id);
	if (WARN(!map, "%s: no map for pool_id %u\n", __func__,
		 srv_pool->member_id)) {
		req->err = -ENOENT;
		goto unlock;
	}

	count = rmr_subblk_covered(srv_pool->pool->chunk_size_shift,
				   req->offset, req->length, &first);
	if (!count)
		goto unlock;

	/*
	 * Submit secured this slot before the write was applied, so a
	 * missing slot means this node's own map was marked dirty again
	 * or reset meanwhile. The slot is not recreated, so this write
	 * does not take the chunk clean.
	 */
	rcu_read_lock();
	slot = rmr_subblk_lookup(map, req->id.b);
	if (slot)
		rmr_subblk_slot_clear(slot, first, count);
	rcu_read_unlock();
	if (!slot)
		goto unlock;

	/*
	 * Judge the chunk against the slot the cache holds now,
	 * not the one cleared above. A re-dirty between the two
	 * detaches that slot and sets the coarse bit, and judging
	 * the detached bitmap would clear that bit again.
	 */
	state = rmr_subblk_take_if_clean(map, req->id.b);
	if (state != RMR_SUBBLK_CLEAN_TAKEN)
		goto unlock;

	/* The dirty bit went with the slot; only the entry is left. */
	rmr_map_erase_entry(map, req->id);
	/*
	 * The peers still have this node down as behind for the chunk.
	 * Queue the MAP_CLEAR rather than send it here: this runs from
	 * the IO completion, where getting an iu may sleep.
	 */
	rmr_srv_mapclear_queue(srv_pool, req->id.b);
	clean = true;

unlock:
	srcu_read_unlock(&srv_pool->pool->map_srcu, lock_idx);
out:
	if (clean) {
		/*
		 * Keep sync_cnt claimed so a late submitter parks instead of
		 * taking a retired entry; the RCU callback wakes it.
		 */
		call_rcu(&entry->rcu, rmr_map_entry_free_rcu);
	} else {
		/*
		 * A fast write is not a sync, so it leaves parked_for_sync
		 * set, and a request waiting for a sync stays ahead of later
		 * covering writes.
		 */
		__rmr_req_release_claim(entry, req->err);
	}

	req->fast_partial = !req->err && !clean;
	rmr_req_complete(req);
}

static void rmr_md_req_store(struct rmr_srv_req *req)
{
	int err;

	err = req->store->ops->submit_md_req(req->store->priv, req->data, req->offset, req->length,
					     req->flags, req);
	if (err) {
		req->endreq(req, err);
		pr_err("release md req %p, flags %u\n", req, req->flags);
		kmem_cache_free(rmr_req_cachep, req);
	}
}

/* md req submission path*/
void rmr_md_req_submit(struct rmr_srv_req *req)
{
	rmr_md_req_store(req);
}

static void rmr_req_sched_store(struct work_struct *work)
{
	struct rmr_srv_req *req = container_of(work, struct rmr_srv_req, work);

	pr_debug("scheduled store for req %p\n", req);
	rmr_req_store(req);
}

static void rmr_req_remote_io_done(void *priv, int err)
{
	struct rmr_srv_req *req = priv;

	pr_debug("called for req %p, err code %d\n", req, err);

	rmr_clt_put_iu(req->srv_pool->clt, req->iu);

	if (err) {
		req->err = err;
		rmr_req_sync_failed(req);
		return;
	}

	pr_debug("schedule store for req %p with err %d\n", req, req->err);
	INIT_WORK(&req->work, rmr_req_sched_store);
	schedule_work(&req->work);
}

static void rmr_req_remote_read(struct rmr_srv_req *req)
{
	struct rmr_srv_pool *srv_pool = req->srv_pool;
	struct rmr_pool *clt = srv_pool->clt;
	unsigned long flags;
	int err;

	pr_debug("redirecting req id (%llu, %llu)\n",
		 req->id.a, req->id.b);
	if (!clt) {
		pr_err("No srv pool assigned for redirect for %s\n", srv_pool->pool->poolname);
		err = -EINVAL;
		goto err;
	}

	if (rmr_op(req->flags) == RMR_OP_SYNCREQ)
		flags = RMR_OP_READ;
	else
		flags = req->flags;

	req->iu = rmr_clt_get_iu(clt, flags, WAIT);
	if (IS_ERR_OR_NULL(req->iu)) {
		pr_err("Failed to get rmr_iu for req id (%llu, %llu)\n",
		       req->id.a, req->id.b);
		err = -EINVAL;
		goto err;
	}

	sg_init_one(&req->sg, req->data, req->datalen);

	pr_debug("After sg_init_one nents=%d\n", sg_nents(&req->sg));

	/* look at the flags here! */
	err = rmr_clt_request(clt, req->iu, req->offset, req->length, flags,
			      req->prio, req, rmr_req_remote_io_done,
			      &req->sg, sg_nents(&req->sg));
	if (err) {
		pr_err("rmr_clt_request error %d\n", err);
		rmr_clt_put_iu(clt, req->iu);
		err = -EREMOTEIO;
		goto err;
	}

	pr_debug("remote read submitted\n");
	return;

err:
	req->err = err;
	rmr_req_sync_failed(req);
}

static void rmr_sync_req_sched(struct work_struct *work)
{
	struct rmr_srv_req *req = container_of(work, struct rmr_srv_req, work);

	pr_debug("scheduled work process for req %p\n", req);
	rmr_req_sync_complete(req);
}

static void rmr_req_complete(struct rmr_srv_req *req)
{
	pr_debug("send completeion for req %p flags %u request id (%llu, %llu) offset %llu length %u err %d\n",
		 req, req->flags,
		 req->id.a, req->id.b, req->offset, req->length, req->err);

	/* endreq() records the Last IO buffer accordingly. */
	req->endreq(req, req->err);

	pr_debug("release req %p, flags %u\n", req, req->flags);

	kmem_cache_free(rmr_req_cachep, req);
}

static struct rmr_srv_req *rmr_req_create_sync_req(struct rmr_srv_pool *srv_pool, rmr_id_t id,
						   u64 offset, u32 len, bool from_sync,
						   struct rmr_srv_req *parent)
{
	struct rmr_srv_req *req;
	struct rmr_srv_io_store *store = srv_pool->io_store;

	if (!store) {
		pr_err("No store_id registered for srv pool %s\n", srv_pool->pool->poolname);
		return ERR_PTR(-ENODEV);
	}

	req = kmem_cache_zalloc(rmr_req_cachep, GFP_KERNEL);
	if (!req) {
		pr_err("cannot allocate memory for rmr_req.\n");
		return ERR_PTR(-ENOMEM);
	}
	req->id.a = id.a;
	req->id.b = id.b;
	req->flags = RMR_OP_SYNCREQ;
	req->length = len;
	req->offset = offset;
	req->srv_pool = srv_pool;
	req->store = store;
	req->from_sync = from_sync;

	if (parent) {
		req->data = parent->data + offset;
	} else {
		req->data = kmalloc(req->length, GFP_KERNEL);
		if (!req->data) {
			pr_err("cannot allocate memory for sync req id [%llu, %llu]\n",
			       req->id.a, req->id.b);
			kmem_cache_free(rmr_req_cachep, req);
			return ERR_PTR(-ENOMEM);
		}
	}
	req->datalen = len;
	req->parent = parent;

	pr_debug("sync req %p created, flags %u request id (%llu, %llu) offset %llu length %u parent %p\n",
		 req, req->flags, req->id.a, req->id.b, req->offset, req->length, parent);

	return req;
}

/**
 * rmr_srv_sync_chunk_id() - sync the dirty sub-blocks of a chunk.
 *
 * @srv_pool:	the server pool driving the sync.
 * @entry:	the map entry for @id; caller holds the sync claim.
 * @id:		chunk id to sync.
 * @from_sync:	true if from the background sweep, false for demand sync.
 *
 * Description:
 *	With a slot, issues remote_read children for each dirty sub-block run
 *	of a snapshot of the slot; without one, for the whole chunk. The
 *	parent's completion runs when the last reference drops, from the last
 *	child, or from here if every child finished during issuing.
 *
 * Return:
 *	0 once the sync has been launched, which includes the case where it was
 *	launched and then abandoned part way through issuing, since the parent's
 *	completion then reports that error and releases the chunk's sync claim.
 *	A negative errno means nothing was launched and the caller still owns
 *	the claim.
 */
int rmr_srv_sync_chunk_id(struct rmr_srv_pool *srv_pool, struct rmr_map_entry *entry,
			  rmr_id_t id, bool from_sync)
{
	struct rmr_pool *pool = srv_pool->pool;
	struct rmr_dirty_id_map *map;
	struct rmr_srv_req *parent_req;
	DECLARE_BITMAP(bits_snap, RMR_MAX_SUBBLK);
	u64 offset;
	u32 max_io_size, total_len, nr_subblk;
	unsigned int run_first, run_end;
	bool had_slot;
	int lock_idx;

	if (!srv_pool->clt) {
		pr_err("For pool %s no sync pool assigned.\n", pool->poolname);
		return -EINVAL;
	}

	/*
	 * Tile on sub-block boundaries. rmr_subblk_covered() rounds inward, so
	 * a sub-block split across two child slices is fully covered by
	 * neither and would be cleared by neither, leaving it dirty after a
	 * transfer that did write it.
	 */
	max_io_size = ALIGN_DOWN(srv_pool->max_sync_io_size, RMR_SUBBLK_SIZE);
	if (WARN_ON_ONCE(!max_io_size))
		return -EINVAL;

	offset = CHUNK_TO_OFFSET(id.b, pool->chunk_size_shift);
	total_len = pool->chunk_size;

	pr_debug("pool %s sync id (%llu, %llu), total_len %u, max_io_size %u\n",
		 pool->poolname, id.a, id.b, total_len, max_io_size);

	/*
	 * Coordinator parent: owns the scratch buffer and the chunk's sync
	 * completion, but does not itself read from the peer. Children read
	 * each dirty run from the peer and store into parent_req->data at
	 * their in-chunk offset. We hold one issuing ref on the entry for
	 * the parent and drop it after all children are issued, so a fast
	 * child completion cannot tear the chunk down mid-issue.
	 */
	parent_req = rmr_req_create_sync_req(srv_pool, id, offset, total_len, from_sync, NULL);
	if (IS_ERR(parent_req)) {
		pr_err("pool %s failed to create main sync req to sync id (%llu, %llu)\n",
		       pool->poolname, id.a, id.b);
		return -ENOMEM;
	}
	parent_req->priv = entry;

	/*
	 * Walk the dirty sub-block runs and sync only those, so a sub-block the
	 * client already wrote is not re-pulled.
	 *
	 * The loop below calls rmr_srv_get_sync_permit() which may sleep.
	 * We cannot hold RCU across a sleep, so snapshot the dirty bitmap
	 * into a local buffer up front under RCU and walk the snapshot.
	 */
	bitmap_zero(bits_snap, RMR_MAX_SUBBLK);

	/*
	 * rmr_pool_find_map() returns the pointer unprotected and the map
	 * page is freed once synchronize_srcu(&pool->map_srcu) has passed,
	 * without waiting on pool->ids_inflight_ref. Hold map_srcu across
	 * the lookup and the snapshot it feeds.
	 */
	lock_idx = srcu_read_lock(&pool->map_srcu);
	map = rmr_pool_find_map(pool, srv_pool->member_id);
	if (!map) {
		pr_err("no map found for pool_id %u\n", srv_pool->member_id);
		srcu_read_unlock(&pool->map_srcu, lock_idx);
		/* TODO: Handle this? How? */
		kfree(parent_req->data);
		kmem_cache_free(rmr_req_cachep, parent_req);
		return -EINVAL;
	}

	rcu_read_lock();
	had_slot = rmr_subblk_snapshot_bits(map, id.b, bits_snap, &nr_subblk);
	rcu_read_unlock();
	srcu_read_unlock(&pool->map_srcu, lock_idx);

	if (from_sync)
		atomic_inc(&srv_pool->sync_parents);

	/* inc ref cnt for parent_req (issuing ref) */
	map_entry_get_sync(entry);

	/*
	 * A transfer that walked a slot moves only its dirty runs, so its
	 * completion cannot conclude the chunk converged once the slot is gone.
	 */
	parent_req->had_slot = had_slot;

	/* Without a slot the whole chunk is dirty, and synced as one run. */
	if (!had_slot) {
		nr_subblk = total_len >> RMR_SUBBLK_SHIFT;
		bitmap_set(bits_snap, 0, nr_subblk);
	}

	for_each_set_bitrange(run_first, run_end, bits_snap, nr_subblk) {
		u64 run_off = offset + ((u64)run_first << RMR_SUBBLK_SHIFT);
		u32 run_len = (run_end - run_first) << RMR_SUBBLK_SHIFT;
		u32 done;

		/* tile the run into <= max_io_size child reqs */
		for (done = 0; done < run_len; done += max_io_size) {
			struct rmr_srv_req *req;
			u64 slice_off = run_off + done;
			u32 slice_len = min_t(u32, max_io_size, run_len - done);
			u32 in_chunk = (u32)(slice_off - offset);

			if (from_sync && rmr_srv_get_sync_permit(srv_pool)) {
				pr_err("%s failed to acquire permit for child\n",
				       __func__);
				parent_req->err = -EBUSY;
				goto abandon;
			}

			req = rmr_req_create_sync_req(srv_pool, id, in_chunk, slice_len,
						      from_sync, parent_req);
			if (IS_ERR(req)) {
				pr_err("%s: Pool %s, id (%llu, %llu), in_chunk %u, len %u, err %ld\n",
				       __func__, pool->poolname, id.a, id.b,
				       in_chunk, slice_len, PTR_ERR(req));
				parent_req->err = PTR_ERR(req);
				goto put_permit;
			}

			req->offset = slice_off;
			req->priv = entry;

			/* inc ref cnt for the child req just created */
			map_entry_get_sync(entry);
			rmr_req_remote_read(req);
		}
	}

	/*
	 * Drop the coordinator's issuing ref. If every child has already finished,
	 * trigger the chunk's sync completion now; otherwise the last child does.
	 */
	if (map_entry_put_sync(entry)) {
		INIT_WORK(&parent_req->work, rmr_sync_req_sched);
		schedule_work(&parent_req->work);
	}

	return 0;

put_permit:
	if (from_sync)
		rmr_srv_put_sync_permit(srv_pool);
abandon:
	/*
	 * The issuing ref is taken, so the parent's completion owns the claim
	 * and reports the error. Returning it to the caller as well would have
	 * the caller release a claim a child still in flight is holding,
	 * driving sync_cnt below -1 where no cmpxchg can ever reclaim the
	 * chunk.
	 */
	rmr_req_sync_failed(parent_req);
	return 0;
}

static void __release_parent_req(struct rcu_head *head)
{
	struct rmr_srv_req *req = container_of(head, struct rmr_srv_req, rcu);
	struct rmr_srv_pool *srv_pool = req->srv_pool;
	struct rmr_map_entry *entry = req->priv;
	bool from_sync = req->from_sync;

	pr_debug("is called for req=%p id=(%llu,%llu) err=%d, entry=%p\n",
		 req, req->id.a, req->id.b, req->err, entry);

	if (req->keep_entry) {
		/*
		 * The sync failed or did not converge, which includes a
		 * re-dirty or reset of this node's own map that dropped the
		 * slot it walked. Reset sync_cnt so a later req or the sync
		 * thread can claim the chunk again, and leave the entry for it
		 * to find, unless a reset already dropped it from the index.
		 */
		rmr_req_release_claim(entry, req->err);
	} else {
		pr_debug("req %p, completed all sync req, lets clean map\n", req);
		rmr_process_wait_list(entry, 0);
		pr_debug("free entry %p for req %p\n", entry, req);
		kmem_cache_free(rmr_map_entry_cachep, entry);
	}

	kmem_cache_free(rmr_req_cachep, req);

	/* Last: once the sync thread sees the count drop, the pool can go. */
	if (from_sync)
		rmr_srv_put_sync_parent(srv_pool);
}

/**
 * rmr_req_sync_release() - hand a finished parent sync req to its RCU release.
 *
 * @req:	the parent sync request.
 * @free_entry:	true once the chunk is clean and the entry can be reclaimed.
 */
static void rmr_req_sync_release(struct rmr_srv_req *req, bool free_entry)
{
	req->keep_entry = !free_entry;
	call_rcu(&req->rcu, __release_parent_req);
}

/**
 * rmr_req_sync_converged() - decide whether the chunk is now fully in sync and
 * commit that decision to the local map.
 *
 * @req:	the parent sync request.
 *
 * Description:
 *	Only reaches the verdict and records it in this node's map. On the
 *	clean verdict the coarse dirty bit is cleared and the entry erased;
 *	otherwise the map is left as it is. Kicking the wait_list and
 *	releasing the sync claim are left to rmr_req_sync_release().
 *
 * Return:
 *	true if the chunk converged. false if it did not, or if this node's
 *	map is gone, which also sets @req->err.
 */
static bool rmr_req_sync_converged(struct rmr_srv_req *req)
{
	struct rmr_srv_pool *srv_pool = req->srv_pool;
	struct rmr_dirty_id_map *map;
	bool converged = true;
	int lock_idx;

	/*
	 * rmr_pool_find_map() returns the pointer unprotected, and the map page
	 * is freed once synchronize_srcu(&pool->map_srcu) has passed, without
	 * waiting on pool->ids_inflight_ref. Hold map_srcu from the lookup
	 * until the last dereference below.
	 */
	lock_idx = srcu_read_lock(&srv_pool->pool->map_srcu);
	map = rmr_pool_find_map(srv_pool->pool, srv_pool->member_id);
	if (WARN(!map, "%s: no map for pool_id %u\n", __func__,
		 srv_pool->member_id)) {
		req->err = -EINVAL;
		converged = false;
		goto unlock;
	}

	/*
	 * A chunk that did not converge has sub-blocks still dirty, or a
	 * re-dirty or reset of this node's own map dropped the slot. The
	 * release only resets the claim and kicks parked reqs; it does not
	 * queue another sync.
	 */
	switch (rmr_subblk_take_if_clean(map, req->id.b)) {
	case RMR_SUBBLK_CLEAN_TAKEN:
		/* The dirty bit went with the slot; only the entry is left. */
		rmr_map_erase_entry(map, req->id);
		break;
	case RMR_SUBBLK_PARTIAL:
		converged = false;
		break;
	case RMR_SUBBLK_ABSENT:
		/*
		 * No slot at completion means the chunk converged only if none
		 * existed when the transfer started. A slot dropped mid-sync
		 * means this node's own map was marked dirty again or reset,
		 * and the verdict leaves the map as that left it.
		 */
		if (req->had_slot)
			converged = false;
		else
			rmr_map_unset_dirty(map, req->id, MAP_NO_FILTER);
		break;
	}

unlock:
	srcu_read_unlock(&srv_pool->pool->map_srcu, lock_idx);

	return converged;
}

/**
 * rmr_req_sync_complete() - finalize a sync parent or child request.
 *
 * @req:	the sync request; req->parent is NULL for the parent.
 *
 * Description:
 *	Child reqs only put the permit and free. A parent reaches the chunk
 *	verdict first and only then, on a clean verdict, sends MAP_CLEAR:
 *	MAP_CLEAR asserts to every peer that this node holds the whole chunk
 *	again, so it must not go out while sub-blocks are still dirty. The req
 *	and the entry are released once that message completes.
 */
static void rmr_req_sync_complete(struct rmr_srv_req *req)
{
	pr_debug("sync_req %p completed for id (%llu, %llu), offset %llu, len %u, err %d, from sync %d\n",
		 req, req->id.a, req->id.b, req->offset, req->length,
		 req->err, req->from_sync);

	if (req->err)
		rmr_srv_sync_req_failed(req->srv_pool);

	pr_debug("release sync req %p, flags %u\n", req, req->flags);

	if (req->parent) {
		if (req->from_sync)
			rmr_srv_put_sync_permit(req->srv_pool);
		kmem_cache_free(rmr_req_cachep, req);
		return;
	}

	/*
	 * Every child is done with the buffer, and neither the verdict nor
	 * MAP_CLEAR reads it. Parents take no permit, so freeing it here
	 * limits chunk buffers to the syncs still transferring.
	 */
	kfree(req->data);
	req->data = NULL;

	/*
	 * A failed or unconverged sync leaves the entry in the xarray, unless a
	 * map reset dropped it, so a later req or the sync thread can claim the
	 * chunk; the kick reports the error so parked reqs do not deadlock.
	 */
	if (req->err || !rmr_req_sync_converged(req)) {
		rmr_req_sync_release(req, false);
		return;
	}

	rmr_req_send_map_clear(req);
}

static void rmr_req_sync_failed(struct rmr_srv_req *req)
{
	rmr_srv_sync_req_failed(req->srv_pool);

	pr_err("pool %s sync req %p failed for id (%llu, %llu), offset %llu, len %u, err %d\n",
	       req->srv_pool->pool->poolname, req, req->id.a, req->id.b,
	       req->offset, req->length, req->err);

	rmr_req_store_done(req);
}

// this is actually very like rmr_req_remote_io_done but without rmr_clt_put_iu
// do we want to have one function for both cases?
static void rmr_req_map_clear_done(void *priv, int err)
{
	struct rmr_srv_req *req = priv;

	rmr_clt_put_iu(req->srv_pool->clt, req->iu);

	pr_debug("called for req %p, err code %d\n", req, err);
	if (err)
		pr_err("pool %s, sync req  with id (%llu, %llu) failed to send map clear\n",
		       req->srv_pool->pool->poolname, req->id.a, req->id.b);

	/*
	 * The chunk was declared clean locally before this went out. A send
	 * error only leaves a zombie entry on the peer, which
	 * rmr_srv_check_map_clear() reaps.
	 */
	rmr_req_sync_release(req, true);
}

/*
 * A sync child stored its slice of the chunk. Mark the sub-blocks the slice
 * fully covers in sync.
 */
static void rmr_req_mark_slice_in_sync(struct rmr_srv_req *req)
{
	struct rmr_srv_pool *srv_pool = req->srv_pool;
	struct rmr_dirty_id_map *map;
	struct rmr_subblk_slot *slot;
	u32 first, count;
	int lock_idx;

	count = rmr_subblk_covered(srv_pool->pool->chunk_size_shift,
				   req->offset, req->length, &first);
	if (!count)
		return;

	/*
	 * rmr_pool_find_map() drops its own read section before returning,
	 * and rmr_pool_remove_map() frees the map page once
	 * synchronize_srcu(&pool->map_srcu) has passed. Hold map_srcu across
	 * the lookup and every dereference of the map, or the page can go
	 * away under us.
	 */
	lock_idx = srcu_read_lock(&srv_pool->pool->map_srcu);
	map = rmr_pool_find_map(srv_pool->pool, srv_pool->member_id);
	if (WARN_ON_ONCE(!map))
		goto unlock;

	/*
	 * Refine an existing record only. Creating one here would turn a
	 * chunk the cache describes as fully dirty into a partial one while
	 * its own whole-chunk sync is still running, and a GFP_NOWAIT failure
	 * for one child would leave that child's run dirty on a slot a sibling
	 * created, costing a second transfer of a chunk that was in fact
	 * fully written.
	 */
	rcu_read_lock();
	slot = rmr_subblk_lookup(map, req->id.b);
	if (slot)
		rmr_subblk_slot_clear(slot, first, count);
	rcu_read_unlock();
unlock:
	srcu_read_unlock(&srv_pool->pool->map_srcu, lock_idx);
}

static void rmr_req_store_done(struct rmr_srv_req *req)
{
	struct rmr_map_entry *entry = req->priv;
	struct rmr_srv_req *parent_req = NULL;

	pr_debug("called for req %p id (%llu, %llu ) offset %llu len %u with parent req %p\n",
		 req, req->id.a, req->id.b, req->offset, req->length, req->parent);

	if (req->parent)
		parent_req = req->parent;
	else
		parent_req = req;

	if (req->err)
		parent_req->err = req->err;
	else if (req->parent)
		rmr_req_mark_slice_in_sync(req);

	if (map_entry_put_sync(entry)) {
		pr_debug("%s: for entry %p id (%llu, %llu) all sync req done.\n", __func__,
			 entry, req->id.a, req->id.b);

		/* We have to schedule the work of parent req from here since we are in the
		 * interrupt context of either parent req or child req
		 */
		pr_debug("%s: process parent_req %p\n", __func__, parent_req);
		INIT_WORK(&parent_req->work, rmr_sync_req_sched);
		schedule_work(&parent_req->work);
	}

	if (req != parent_req) {
		pr_debug("completing req %p with err %d\n", req, req->err);
		rmr_req_sync_complete(req);
	}
}

static void rmr_req_send_map_clear(struct rmr_srv_req *req)
{
	struct rmr_srv_pool *srv_pool = req->srv_pool;
	struct rmr_pool *pool = srv_pool->clt;
	struct rmr_iu *iu;
	int err;

	if (!pool) {
		pr_err("Cannot send map clear. No pool client assigend for srv pool %s\n",
		       req->srv_pool->pool->poolname);
		goto err;
	}

	/*
	 * We try to clear map, but if we fail to, we simply ignore the error.
	 * Such zombie entries will be clear by rmr_srv_check_map_clear.
	 */
	iu = rmr_clt_get_iu(pool, RMR_OP_WRITE, WAIT);
	if (IS_ERR_OR_NULL(iu)) {
		pr_err("Failed to get rmr_iu for req id (%llu, %llu)\n",
		       req->id.a, req->id.b);
		goto err;
	}

	pr_debug("send map clear req id (%llu, %llu), member_id %u\n",
		 req->id.a, req->id.b, srv_pool->member_id);

	/*
	 * For MAP_CLEAR, we only need rmr_id_t for chunk number,
	 * and our member_id to say to clear the above chunk number for ths storage node.
	 *
	 * We also update the minimum members needed for map update.
	 */
	rmr_srv_fill_map_clear(iu, pool, srv_pool->member_id, req->id);

	iu->conf = rmr_req_map_clear_done;
	iu->priv = req;

	req->iu = iu;

	err = rmr_clt_send_map_update(pool, req->iu);
	if (err) {
		pr_err("%s error %d\n", __func__, err);
		rmr_clt_put_iu(pool, req->iu);
		goto err;
	}

	pr_debug("send map clear submitted\n");
	return;

err:
	rmr_req_sync_release(req, true);
}
