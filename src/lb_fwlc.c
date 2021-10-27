/*
 * Fast Weighted Least Connection load balancing algorithm.
 *
 * Copyright 2000-2009 Willy Tarreau <w@1wt.eu>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <import/eb32tree.h>
#include <haproxy/api.h>
#include <haproxy/backend.h>
#include <haproxy/queue.h>
#include <haproxy/server-t.h>

/* Update fwlc_bins after inflight[bin] and/or srvcount[bin] changed
 */
static inline void fwlc_bins_update_load(struct fwlc_bins *bins, unsigned bin) {
	/* Find the position of the modified bin in the by_load array */
	uint8_t pos;
	for (pos = 0; bins->by_load[pos] != bin; pos++);

	/* Compute the updated load of the bin
	 *
	 * inflight[bin] is assumed to fit in the right-most 48 bits so that it
	 * can be shifted by 16 bits to do the division with some precision
	 */
	bins->load[bin] = bins->srvcount[bin] > 0 ? (bins->inflight[bin] << 16) / bins->srvcount[bin] : UINT64_MAX;

	/* Load decreased - bubble bin left in by_load array (also below any tied bin) */
	while (pos > 0 && bins->load[bins->by_load[pos - 1]] >= bins->load[bins->by_load[pos]]) {
		uint8_t temp = bins->by_load[pos];
		bins->by_load[pos] = bins->by_load[pos-1];
		bins->by_load[pos-1] = temp;
		pos--;
	}

	/* Load increased - bubble bin right in by_load array */
	while (pos < bins->used - 1 && bins->load[bins->by_load[pos + 1]] < bins->load[bins->by_load[pos]]) {
		uint8_t temp = bins->by_load[pos];
		bins->by_load[pos] = bins->by_load[pos+1];
		bins->by_load[pos+1] = temp;
		pos++;
	}
}

/* Increase the weighted sum of inflight requests on servers in the given bin
 * and/or the number of servers in the bin
 */
static inline void fwlc_bins_add(struct fwlc_bins *bins, uint8_t bin, uint64_t inflight, uint32_t srvcount) {
	int bin_added = bins->srvcount[bin] == 0 && srvcount > 0;

	bins->inflight[bin] += inflight;
	bins->srvcount[bin] += srvcount;

	/* If new bin comes into use, it needs to be put at the end of the
	 * by_load array and then fwlc_bins_update_load will move it to its
	 * proper position when sorting.
	 */
	if (bin_added) {
		bins->by_load[bins->used] = bin;
		bins->used++;
	}

	fwlc_bins_update_load(bins, bin);
}

/* Decrease the weighted sum of inflight requests on servers in the given bin
 * and/or the number of servers in the bin
 */
static inline void fwlc_bins_sub(struct fwlc_bins *bins, uint8_t bin, uint64_t inflight, uint32_t srvcount) {
	bins->inflight[bin] -= inflight;
	bins->srvcount[bin] -= srvcount;

	fwlc_bins_update_load(bins, bin);

	/* If bin comes out of use, fwlc_bins_update_load will put it
	 * at the end of the by_load array when sorting (zero srvcount implies
	 * maximum possible load) and then the corresponding by_load entry can
	 * be cleared.
	 */
	if (bins->srvcount[bin] == 0) {
		bins->by_load[bins->used-1] = 0;
		bins->used--;
	}
}

/* Remove the server from the LB tree. Update statistics of the servers bin.
 *
 * The lbprm's lock must be held.
 */
static inline void fwlc_dequeue_srv(struct server *srv) {
	fwlc_bins_sub(srv->bins, FWLC_EWEIGHT_TO_BIN(srv->cur_eweight), srv->lb_node.key, 1);
	srv->bins = NULL;

	eb32_delete(&srv->lb_node);
	srv->lb_tree = NULL;
}

/* Insert the server into the server tree for the servers bin.
 *
 * Servers eweight is used to weigh the number of inflight requests on the
 * server, both when computing the servers key in the server tree and also when
 * adding the servers contribution to the bin load.
 *
 * srv->next_eweight is used as the servers eweight since at this stage the
 * server state is not yet committed.
 *
 * The lbprm's lock must be held.
 */
static inline void fwlc_queue_srv(struct server *srv) {
	unsigned long long inflight = _HA_ATOMIC_LOAD(&srv->served) + _HA_ATOMIC_LOAD(&srv->queue.length);
	unsigned long long eweight = _HA_ATOMIC_LOAD(&srv->next_eweight);
	uint8_t bin = FWLC_EWEIGHT_TO_BIN(eweight);

	if (srv->flags & SRV_F_BACKUP) {
		srv->bins = &srv->proxy->lbprm.fwlc.bck;
	} else {
		srv->bins = &srv->proxy->lbprm.fwlc.act;
	}

	/* Reposition server in the server tree */
	srv->lb_tree = &srv->bins->srv_tree[bin];
	eb32_delete(&srv->lb_node);
	srv->lb_node.key = inflight * SRV_EWGHT_MAX / eweight;
	eb32_insert(srv->lb_tree, &srv->lb_node);

	/* Update bin statistics */
	fwlc_bins_add(srv->bins, bin, srv->lb_node.key, 1);
}

/* Re-position the server in the server tree after the server
 * has been assigned one connection or after it has released one. Note that it
 * is possible that the server has been moved out of the tree due to failed
 * health-checks. Update bin statistics.
 *
 * The lbprm's lock will be used.
 */
static void fwlc_srv_reposition(struct server *srv)
{
	unsigned long long inflight = _HA_ATOMIC_LOAD(&srv->served) + _HA_ATOMIC_LOAD(&srv->queue.length);
	unsigned long long eweight = _HA_ATOMIC_LOAD(&srv->cur_eweight);
	uint8_t bin = FWLC_EWEIGHT_TO_BIN(eweight);

	/* some calls will be made for no change (e.g connect_server() after
	 * assign_server(). Let's check that first.
	 */
	if (srv->lb_node.node.leaf_p && eweight && srv->lb_node.key == inflight)
		return;

	HA_RWLOCK_WRLOCK(LBPRM_LOCK, &srv->proxy->lbprm.lock);
	if (srv->lb_tree) {
		/* we might have been waiting for a while on the lock above
		 * so it's worth testing again because other threads are very
		 * likely to have released a connection or taken one leading
		 * to our target value (50% of the case in measurements).
		 */

		unsigned long long inflight = _HA_ATOMIC_LOAD(&srv->served) + _HA_ATOMIC_LOAD(&srv->queue.length);

		if (!srv->lb_node.node.leaf_p || srv->lb_node.key != inflight) {
			uint32_t old_srv_key = srv->lb_node.key;
			uint32_t new_srv_key = inflight * SRV_EWGHT_MAX / eweight;

			/* Reposition server */
			eb32_delete(&srv->lb_node);
			srv->lb_node.key = new_srv_key;
			eb32_insert(srv->lb_tree, &srv->lb_node);

			/* Update bin statistics */
			if (new_srv_key > old_srv_key) {
				fwlc_bins_add(srv->bins, bin, new_srv_key - old_srv_key, 0);
			} else {
				fwlc_bins_sub(srv->bins, bin, old_srv_key - new_srv_key, 0);
			}

		}
	}
	HA_RWLOCK_WRUNLOCK(LBPRM_LOCK, &srv->proxy->lbprm.lock);
}

/* This function updates the server trees according to server <srv>'s new
 * state. It should be called when server <srv>'s status changes to down.
 * It is not important whether the server was already down or not. It is not
 * important either that the new state is completely down (the caller may not
 * know all the variables of a server's state).
 *
 * The server's lock must be held. The lbprm's lock will be used.
 */
static void fwlc_set_server_status_down(struct server *srv)
{
	struct proxy *p = srv->proxy;

	if (!srv_lb_status_changed(srv))
		return;

	if (srv_willbe_usable(srv))
		goto out_update_state;

	HA_RWLOCK_WRLOCK(LBPRM_LOCK, &p->lbprm.lock);

	if (!srv_currently_usable(srv))
		/* server was already down */
		goto out_update_backend;

	if (srv->flags & SRV_F_BACKUP) {
		p->lbprm.tot_wbck -= srv->cur_eweight;
		p->srv_bck--;

		if (srv == p->lbprm.fbck) {
			/* we lost the first backup server in a single-backup
			 * configuration, we must search another one.
			 */
			struct server *srv2 = p->lbprm.fbck;
			do {
				srv2 = srv2->next;
			} while (srv2 &&
				 !((srv2->flags & SRV_F_BACKUP) &&
				   srv_willbe_usable(srv2)));
			p->lbprm.fbck = srv2;
		}
	} else {
		p->lbprm.tot_wact -= srv->cur_eweight;
		p->srv_act--;
	}

	fwlc_dequeue_srv(srv);

 out_update_backend:
	/* check/update tot_used, tot_weight */
	update_backend_weight(p);
	HA_RWLOCK_WRUNLOCK(LBPRM_LOCK, &p->lbprm.lock);

 out_update_state:
	srv_lb_commit_status(srv);
}

/* This function updates the server trees according to server <srv>'s new
 * state. It should be called when server <srv>'s status changes to up.
 * It is not important whether the server was already down or not. It is not
 * important either that the new state is completely UP (the caller may not
 * know all the variables of a server's state). This function will not change
 * the weight of a server which was already up.
 *
 * The server's lock must be held. The lbprm's lock will be used.
 */
static void fwlc_set_server_status_up(struct server *srv)
{
	struct proxy *p = srv->proxy;

	if (!srv_lb_status_changed(srv))
		return;

	if (!srv_willbe_usable(srv))
		goto out_update_state;

	HA_RWLOCK_WRLOCK(LBPRM_LOCK, &p->lbprm.lock);

	if (srv_currently_usable(srv))
		/* server was already up */
		goto out_update_backend;

	if (srv->flags & SRV_F_BACKUP) {
		p->lbprm.tot_wbck += srv->next_eweight;
		p->srv_bck++;

		if (!(p->options & PR_O_USE_ALL_BK)) {
			if (!p->lbprm.fbck) {
				/* there was no backup server anymore */
				p->lbprm.fbck = srv;
			} else {
				/* we may have restored a backup server prior to fbck,
				 * in which case it should replace it.
				 */
				struct server *srv2 = srv;
				do {
					srv2 = srv2->next;
				} while (srv2 && (srv2 != p->lbprm.fbck));
				if (srv2)
					p->lbprm.fbck = srv;
			}
		}
	} else {
		p->lbprm.tot_wact += srv->next_eweight;
		p->srv_act++;
	}

	fwlc_queue_srv(srv);

 out_update_backend:
	/* check/update tot_used, tot_weight */
	update_backend_weight(p);
	HA_RWLOCK_WRUNLOCK(LBPRM_LOCK, &p->lbprm.lock);

 out_update_state:
	srv_lb_commit_status(srv);
}

/* This function must be called after an update to server <srv>'s effective
 * weight. It may be called after a state change too.
 *
 * The server's lock must be held. The lbprm's lock will be used.
 */
static void fwlc_update_server_weight(struct server *srv)
{
	int old_state, new_state;
	struct proxy *p = srv->proxy;

	if (!srv_lb_status_changed(srv))
		return;

	/* If changing the server's weight changes its state, we simply apply
	 * the procedures we already have for status change. If the state
	 * remains down, the server is not in any tree, so it's as easy as
	 * updating its values. If the state remains up with different weights,
	 * there are some computations to perform to find a new place and
	 * possibly a new tree for this server.
	 */

	old_state = srv_currently_usable(srv);
	new_state = srv_willbe_usable(srv);

	if (!old_state && !new_state) {
		srv_lb_commit_status(srv);
		return;
	}
	else if (!old_state && new_state) {
		fwlc_set_server_status_up(srv);
		return;
	}
	else if (old_state && !new_state) {
		fwlc_set_server_status_down(srv);
		return;
	}

	HA_RWLOCK_WRLOCK(LBPRM_LOCK, &p->lbprm.lock);

	fwlc_dequeue_srv(srv);

	if (srv->flags & SRV_F_BACKUP) {
		p->lbprm.tot_wbck += srv->next_eweight - srv->cur_eweight;
	} else {
		p->lbprm.tot_wact += srv->next_eweight - srv->cur_eweight;
	}

	fwlc_queue_srv(srv);

	update_backend_weight(p);
	HA_RWLOCK_WRUNLOCK(LBPRM_LOCK, &p->lbprm.lock);

	srv_lb_commit_status(srv);
}

/* This function is responsible for building the trees in case of fast
 * weighted least-conns. It also sets p->lbprm.wdiv to the eweight to
 * uweight ratio. Both active and backup groups are initialized.
 */
void fwlc_init_server_tree(struct proxy *p)
{
	struct eb_root init_head = EB_ROOT;

	struct server *srv;

	for (int i = 0; i < FWLC_BINS; i++) {
		p->lbprm.fwlc.act.inflight[i] = 0;
		p->lbprm.fwlc.act.srvcount[i] = 0;
		p->lbprm.fwlc.act.load[i] = UINT64_MAX;
		p->lbprm.fwlc.act.srv_tree[i] = init_head;

		p->lbprm.fwlc.act.by_load[i] = 0;

		p->lbprm.fwlc.bck.inflight[i] = 0;
		p->lbprm.fwlc.bck.srvcount[i] = 0;
		p->lbprm.fwlc.bck.load[i] = UINT64_MAX;
		p->lbprm.fwlc.bck.srv_tree[i] = init_head;

		p->lbprm.fwlc.bck.by_load[i] = 0;
	}

	p->lbprm.set_server_status_up   = fwlc_set_server_status_up;
	p->lbprm.set_server_status_down = fwlc_set_server_status_down;
	p->lbprm.update_server_eweight  = fwlc_update_server_weight;
	p->lbprm.server_take_conn = fwlc_srv_reposition;
	p->lbprm.server_drop_conn = fwlc_srv_reposition;

	p->lbprm.wdiv = BE_WEIGHT_SCALE;
	for (srv = p->srv; srv; srv = srv->next) {
		srv->next_eweight = (srv->uweight * p->lbprm.wdiv + p->lbprm.wmult - 1) / p->lbprm.wmult;
		srv_lb_commit_status(srv);
	}

	recount_servers(p);
	update_backend_weight(p);

	for (srv = p->srv; srv; srv = srv->next) {
		if (!srv_currently_usable(srv))
			continue;
		fwlc_queue_srv(srv);
	}
}

/* Return next server for backend <p>. First pick the least connected bin,
 * where load of the bin is the sum of inflight requests from all servers in
 * the bin (weighted by each servers eweight) divided by the number of servers
 * in the bin. If the tree is empty, return NULL. Saturated servers are
 * skipped.  If all servers in the bin are saturated, other bin will be
 * considered in the order of least load.
 *
 * The lbprm's lock will be used in R/O mode. The server's lock is not used.
 */
struct server *fwlc_get_next_server(struct proxy *p, struct server *srvtoavoid)
{
	struct server *sel_srv, *avoided;
	struct fwlc_bins *bins;

	sel_srv = avoided = NULL;

	HA_RWLOCK_RDLOCK(LBPRM_LOCK, &p->lbprm.lock);
	if (p->srv_act) {
		bins = &p->lbprm.fwlc.act;
	} else if (p->lbprm.fbck) {
		sel_srv = p->lbprm.fbck;
		goto out;
	} else if (p->srv_bck) {
		bins = &p->lbprm.fwlc.bck;
	} else {
		sel_srv = NULL;
		goto out;
	}

	for (int bin_rank = 0; bin_rank < bins->used; bin_rank++) {
		unsigned bin = bins->by_load[bin_rank];
		struct eb_root *srv_tree;
		struct eb32_node *srv_node;

		srv_tree = &bins->srv_tree[bin];
		srv_node = eb32_first(srv_tree);

		while (srv_node) {
			/* OK, we have a server. However, it may be saturated, in which
			 * case we don't want to reconsider it for now, so we'll simply
			 * skip it. Same if it's the server we try to avoid, in which
			 * case we simply remember it for later use if needed.
			 */
			struct server *srv = eb32_entry(srv_node, struct server, lb_node);

			if (!srv->maxconn || srv->served + srv->queue.length < srv_dynamic_maxconn(srv) + srv->maxqueue) {
				if (srv != srvtoavoid) {
					sel_srv = srv;
					goto out;
				}
				avoided = srv;
			}

			srv_node = eb32_next(srv_node);
		}
	}

	if (!sel_srv)
		sel_srv = avoided;

 out:
	HA_RWLOCK_RDUNLOCK(LBPRM_LOCK, &p->lbprm.lock);
	return sel_srv;
}

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
