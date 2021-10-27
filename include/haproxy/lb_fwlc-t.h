/*
 * include/haproxy/lb_fwlc-t.h
 * Types for Fast Weighted Least Connection load balancing algorithm.
 *
 * Copyright (C) 2000-2009 Willy Tarreau - w@1wt.eu
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation, version 2.1
 * exclusively.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifndef _HAPROXY_LB_FWLC_T_H
#define _HAPROXY_LB_FWLC_T_H

#include <import/ebtree-t.h>
#include <haproxy/thread-t.h>

#include <import/eb64tree.h>

struct lb_fwlc {
	struct eb_root weights;                    /* tree for weight lookup */
	__decl_thread(HA_SPINLOCK_T weights_lock); /* lock serializing access to the weights tree */

	struct eb_root act_leastconn;              /* tree for group lookup by least connected (active servers) */
	struct eb_root bck_leastconn;              /* tree for group lookup by least connected (backup servers) */
};

/* Agroup consists of all servers of equal weight and equal active/backup status. */
struct fwlc_group {
	uint64_t total_inflight;                   /* sum of in-flight requests on all servers in the group (invariant: equal to sum of keys in srv_tree) */
	uint32_t total_eweight;                    /* sum of eweights of servers in the group */
	struct eb_root srv_tree;                   /* server tree for lookup of least connected server in the group */
	struct eb_root *leastconn_tree;            /* &act_leastconn or &bck_leastconn */
	struct eb64_node leastconn_node;           /* node in leastconn_tree */
};

struct fwlc_weight {
	struct eb32_node weights_node;             /* node in weights_tree */
	struct fwlc_group act;                     /* active subgroup */
	struct fwlc_group bck;                     /* backup subgroup */
};

#endif /* _HAPROXY_LB_FWLC_T_H */

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
