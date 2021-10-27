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

DECLARE_STATIC_POOL(fwlc_weights_pool, "fwlc_weight", sizeof(struct fwlc_weight));

static inline void fwlc_register_uweight(struct proxy *p, unsigned uweight) {
	struct eb_root init_head = EB_ROOT;
	struct fwlc_weight *weight = pool_alloc(fwlc_weights_pool);
	struct eb32_node *existing_weights_node;

	if (!weight) {
		return;
	}

	weight->weights_node.key = uweight;

	weight->act.total_inflight = 0;
	weight->act.total_eweight = 0;
	weight->act.srv_tree = init_head;
	weight->act.leastconn_tree = &p->lbprm.fwlc.act_leastconn;

	weight->bck.total_inflight = 0;
	weight->bck.total_eweight = 0;
	weight->bck.srv_tree = init_head;
	weight->bck.leastconn_tree = &p->lbprm.fwlc.bck_leastconn;

	HA_RWLOCK_WRLOCK(FWLC_WEIGHTS_LOCK, &p->lbprm.fwlc.weights_lock);
	/* Another thread could have already added the group */
	existing_weights_node = eb32_lookup(&p->lbprm.fwlc.weights, uweight);
	if (existing_weights_node == NULL)
		eb32_insert(&p->lbprm.fwlc.weights, &weight->weights_node);
	HA_RWLOCK_WRUNLOCK(FWLC_WEIGHTS_LOCK, &p->lbprm.fwlc.weights_lock);

	if (existing_weights_node != NULL)
		pool_free(fwlc_weights_pool, weight);
}

/* Remove the server from the group. If group became empty, it will be removed
 * from the tree for lookup by weight and from the tree for leastconn lookup.
 *
 * The lbprm's lock must be held.
 */
static inline void fwlc_dequeue_srv(struct server *srv) {
	unsigned long long eweight = _HA_ATOMIC_LOAD(&srv->cur_eweight);
	struct fwlc_group *group = srv->lb_group;

	group->total_inflight -= srv->lb_node.key;
	group->total_eweight -= eweight;

	srv->lb_group = NULL;
	eb32_delete(&srv->lb_node);
	srv->lb_tree = NULL;

	eb64_delete(&group->leastconn_node);
	if (group->total_eweight > 0) {
		group->leastconn_node.key = (group->total_inflight << 16) / (group->total_eweight);
		eb64_insert(group->leastconn_tree, &group->leastconn_node);
	}
}

/* Find or create the server group for <srv> pending new weight. Reposition or
 * insert the group into two trees: one for lookup by weight and another for
 * looking up the least connected group. Finally, reposition or insert the
 * server into the server tree of the group.
 *
 * srv->next_eweight is used as the servers weight since at this stage the
 * server state is not yet committed.
 *
 * The lbprm's lock must be held.
 */
static inline void fwlc_queue_srv(struct server *srv) {
	unsigned long long inflight = _HA_ATOMIC_LOAD(&srv->served) + _HA_ATOMIC_LOAD(&srv->queue.length);
	unsigned long long eweight = _HA_ATOMIC_LOAD(&srv->next_eweight);

	struct eb32_node *weights_node;
	struct fwlc_weight *weight;
	struct fwlc_group *group;

	/* Find the group for this weight */
	HA_RWLOCK_RDLOCK(FWLC_WEIGHTS_LOCK, &srv->proxy->lbprm.fwlc.weights_lock);
	weights_node = eb32_lookup(&srv->proxy->lbprm.fwlc.weights, srv->uweight);
	weight = eb32_entry(weights_node, struct fwlc_weight, weights_node);
	HA_RWLOCK_RDUNLOCK(FWLC_WEIGHTS_LOCK, &srv->proxy->lbprm.fwlc.weights_lock);

	/* Select the appropiate group */
	if (srv->flags & SRV_F_BACKUP) {
		group = &weight->bck;
	} else {
		group = &weight->act;
	}

	/* Dequeue if necessary */
	if (group->total_eweight > 0)
		eb64_delete(&group->leastconn_node);

	/* Add servers eweight */
	group->total_eweight += eweight;

	/* Reposition server in the server tree */
	eb32_delete(&srv->lb_node);
	srv->lb_node.key = inflight;
	eb32_insert(&group->srv_tree, &srv->lb_node);

	/* Update server pointers */
	srv->lb_group = group;
	srv->lb_tree = &group->srv_tree;

	/* Re-key the group with inflight/(size*weight), extending everything
	 * to uint64_t and multipling nominator by 2^32 to get maximum
	 * precision.
	 */
	group->total_inflight += srv->lb_node.key;
	group->leastconn_node.key = (group->total_inflight << 16) / group->total_eweight;
	eb64_insert(group->leastconn_tree, &group->leastconn_node);
}

/* Re-position the server and the group it belongs to after the server has been
 * assigned one connection or after it has released one. Note that it is
 * possible that the server has been moved out of the tree due to failed
 * health-checks.
 *
 * The lbprm's lock will be used.
 */
static void fwlc_reposition_srv_and_group(struct server *srv)
{
	unsigned long long inflight = _HA_ATOMIC_LOAD(&srv->served) + _HA_ATOMIC_LOAD(&srv->queue.length);
	unsigned long long eweight = _HA_ATOMIC_LOAD(&srv->cur_eweight);

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
			struct fwlc_group *group = srv->lb_group;
			uint32_t old_srv_key = srv->lb_node.key;
			uint32_t new_srv_key = inflight;

			/* Reposition server */
			eb32_delete(&srv->lb_node);
			srv->lb_node.key = new_srv_key;
			eb32_insert(srv->lb_tree, &srv->lb_node);

			/* Reposition group */
			eb64_delete(&group->leastconn_node);
			group->total_inflight += (new_srv_key - old_srv_key);
			/* Extend to uint64_t and multiply nominator by 2^32 to get maximum precision */
			group->leastconn_node.key = (((uint64_t) group->total_inflight) << 32) / group->total_eweight;
			eb64_insert(group->leastconn_tree, &group->leastconn_node);
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

	p->lbprm.fwlc.weights = init_head;
	HA_RWLOCK_INIT(&p->lbprm.fwlc.weights_lock);

	p->lbprm.fwlc.act_leastconn = init_head;
	p->lbprm.fwlc.bck_leastconn = init_head;

	p->lbprm.set_server_status_up   = fwlc_set_server_status_up;
	p->lbprm.set_server_status_down = fwlc_set_server_status_down;
	p->lbprm.update_server_eweight  = fwlc_update_server_weight;
	p->lbprm.server_take_conn = fwlc_reposition_srv_and_group;
	p->lbprm.server_drop_conn = fwlc_reposition_srv_and_group;
	p->lbprm.register_uweight = fwlc_register_uweight;

	p->lbprm.wdiv = BE_WEIGHT_SCALE;
	for (srv = p->srv; srv; srv = srv->next) {
		fwlc_register_uweight(p, srv->uweight);
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

/* Return next server for backend <p>. First pick the least connected group,
 * where load of the group is proportional to its total number of inflight
 * requests, but inversely proportional to the size of the group and its
 * weight: load = (inflight/(size*weight)). If the tree is empty, return NULL.
 * Saturated servers are skipped. If all servers in a group are saturated,
 * other groups than the least loaded one will also be considered.
 *
 * The lbprm's lock will be used in R/O mode. The server's lock is not used.
 */
struct server *fwlc_get_next_server(struct proxy *p, struct server *srvtoavoid)
{
	struct server *sel_srv, *avoided;
	struct eb64_node *group_node;

	sel_srv = avoided = NULL;

	HA_RWLOCK_RDLOCK(LBPRM_LOCK, &p->lbprm.lock);
	if (p->srv_act) {
		group_node = eb64_first(&p->lbprm.fwlc.act_leastconn);
		if (!group_node)
			goto out;
	} else if (p->lbprm.fbck) {
		sel_srv = p->lbprm.fbck;
		goto out;
	} else if (p->srv_bck) {
		group_node = eb64_first(&p->lbprm.fwlc.bck_leastconn);
		if (!group_node)
			goto out;
	} else {
		sel_srv = NULL;
		goto out;
	}

	while (group_node) {
		struct fwlc_group *group = eb64_entry(group_node, struct fwlc_group, leastconn_node);

		struct eb32_node *srv_node = eb32_first(&group->srv_tree);
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

		group_node = eb64_next(group_node);
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
