/*
 * include/haproxy/lb_fwlcgr-t.h
 * Types for Fast Weighted Grouped Least Connection load balancing algorithm.
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

#ifndef _HAPROXY_LB_FWLCGR_T_H
#define _HAPROXY_LB_FWLCGR_T_H

#include <import/ebtree.h>
#include <import/eb64tree.h>

#include <haproxy/backend-t.h>
#include <haproxy/server-t.h>

/* A group consists of all servers of equal weight and of the same status (active/backup).
 *
 * The least connected group is the one with the lowest inflight/(size*weight).
 */
struct fwlcgr_group {
	uint32_t weight;         /* weight of the group */
	uint32_t size;           /* number of servers in the group */
	uint32_t inflight;       /* sum of in-flight requests on all servers in the group */

	struct eb_root *tree;    /* root of the least connected group tree (&lb_fwlcgr->act or &lb_fwlcgr->bck) */
	struct eb64_node node;   /* node in the least connected group tree */

	struct eb_root srv_tree; /* least connected server tree (among servers within the group) */
};

struct lb_fwlcgr {
	struct eb_root act;      /* least connected group tree (only active servers) */
	struct eb_root bck;      /* least connected group tree (only backup servers) */

	struct fwlcgr_group act_groups[SRV_EWGHT_MAX+1];  /* preallocated groups for each possible weight (only active servers) */
	struct fwlcgr_group bck_groups[SRV_EWGHT_MAX+1];  /* preallocated groups for each possible weight (only backup servers) */
};

#endif /* _HAPROXY_LB_FWLCGR_T_H */

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
