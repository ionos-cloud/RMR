/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Reliable multicast over RTRS (RMR)
 *
 * Copyright (c) 2026 IONOS SE
 */

#ifndef RMR_REQ_H
#define RMR_REQ_H

#include "rmr-pool.h"

struct rmr_srv_req {
	struct rmr_srv_pool *srv_pool;
	rmr_id_t id;

	u64 offset;
	u32 length;
	u32 flags;
	u16 prio;

	u32 mem_id;
	struct rtrs_srv_op *rtrs_op;
	struct rmr_srv_io_store *store;
	void *data;
	u32 datalen; //TODO: what is the difference between lenghth?
	void (*endreq)(struct rmr_srv_req *, int err);
	struct work_struct work;
	int err;
	u8 failed_cnt;
	u8 failed_srv_id[RMR_POOL_MAX_SESS];
	u64 map_ver;
	void *priv;
	struct llist_node node;
	bool from_sync;
	struct scatterlist sg;
	struct rmr_iu *iu;
	struct rmr_srv_req *parent;
	bool sync;
	/*
	 * Set on a parent sync req that found a slot when the transfer started.
	 * A slot missing at completion then means this node's own map was
	 * marked dirty again or reset while the sync ran, which is a different
	 * situation from a chunk that never had one.
	 */
	bool had_slot;
	/*
	 * Set when the sync did not take the chunk clean. The release callback
	 * then lets go of the claim and does not free the entry, which stays in
	 * the index for the next sync unless a map reset dropped it.
	 */
	bool keep_entry;
	struct rcu_head rcu;
};

struct rmr_srv_req *rmr_srv_req_create(const struct rmr_msg_io *msg,
				       struct rmr_srv_pool *srv_pool,
				       struct rtrs_srv_op *rtrs_op,
				       void *data, u32 datalen,
				       void (*endreq)(struct rmr_srv_req *, int));
struct rmr_srv_req *rmr_srv_md_req_create(struct rmr_srv_pool *srv_pool,
					  struct rtrs_srv_op *rtrs_op, void *data,
					  u32 offset, u32 len, unsigned long flags,
					  void (*endreq)(struct rmr_srv_req *, int));
void rmr_req_submit(struct rmr_srv_req *req);
void rmr_md_req_submit(struct rmr_srv_req *req);
void rmr_srv_req_resp(struct rmr_srv_req *req, int err);
void rmr_srv_md_req_resp(struct rmr_srv_req *req, int err);
int rmr_srv_sync_chunk_id(struct rmr_srv_pool *srv_pool, struct rmr_map_entry *entry,
			  rmr_id_t id, bool from_sync);

void rmr_process_wait_list(struct rmr_map_entry *entry, int err);

struct rmr_map_entry_info {
	rmr_id_t id;
	u8 srv_id;
};
#endif /* RMR_REQ_H */
