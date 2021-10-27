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
#include <haproxy/server-t.h>

#define FWLC_BINS 64
#define FWLC_EWEIGHT_TO_BIN(w) ((w < 37*16) ? w/16 : 32+w/(8*16))

/* To increase accuracy of the server weights, servers are grouped into a
 * number of bins. The bin for a server is determined by its current eweight
 * using FWLC_EWEIGHT_TO_BIN. For each bin, the number of servers belonging to
 * the bin is tracked and the sum of inflight requests on each server weighted
 * by the servers eweight. The weighted sum inflight divided by the number of
 * servers is the bin load. The LB algorithm picks the least loaded bin and
 * then the least loaded server in the bin.
 */
struct fwlc_bins {
	struct eb_root srv_tree[FWLC_BINS];	/* server tree for lookup of least connected server in the given bin */
	uint64_t inflight[FWLC_BINS];		/* sum of inflight requests on servers in given bin, weighted by each servers eweight */
	uint32_t srvcount[FWLC_BINS];		/* number of servers in given bin */
	uint64_t load[FWLC_BINS];		/* load on given bin: (inflight<<16)/srvcount */

	unsigned by_load[FWLC_BINS];		/* array of bin numbers in order of increasing bin load */
	uint8_t used;				/* how many bins are in active use */
};

struct lb_fwlc {
	struct fwlc_bins act;	/* weighted least conns on the active servers */
	struct fwlc_bins bck;	/* weighted least conns on the backup servers */
};

#endif /* _HAPROXY_LB_FWLC_T_H */

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
