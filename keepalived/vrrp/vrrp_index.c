/*
 * Soft:        Keepalived is a failover program for the LVS project
 *              <www.linuxvirtualserver.org>. It monitor & manipulate
 *              a loadbalanced server pool using multi-layer checks.
 *
 * Part:        VRRP instance index table.
 *
 * Author:      Alexandre Cassen, <acassen@linux-vs.org>
 *
 *              This program is distributed in the hope that it will be useful,
 *              but WITHOUT ANY WARRANTY; without even the implied warranty of
 *              MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *              See the GNU General Public License for more details.
 *
 *              This program is free software; you can redistribute it and/or
 *              modify it under the terms of the GNU General Public License
 *              as published by the Free Software Foundation; either version
 *              2 of the License, or (at your option) any later version.
 *
 * Copyright (C) 2001-2017 Alexandre Cassen, <acassen@gmail.com>
 */

#include "config.h"

/* local include */
#include "vrrp_index.h"
#include "vrrp_data.h"
#include "vrrp.h"
#include "memory.h"
#include "list.h"

/* VRID hash table */
int
get_vrrp_hash(const int vrid, const int fd)
{
	return ( vrid * 31 + ( fd/2 ) * 37 )%1151;
}

void
alloc_vrrp_bucket(vrrp_if *vif)
{
	vrrp_t *vrrp = vif->vrrp;
	list_add(&vrrp_data->vrrp_index[get_vrrp_hash(vrrp->vrid, vif->fd_in)], vif);
}

vrrp_if *
vrrp_index_lookup(const int vrid, const int fd)
{
	vrrp_if *vif;
	element e;
	list l = &vrrp_data->vrrp_index[get_vrrp_hash(vrid, fd)];

	/* return if list is empty */
	if (LIST_ISEMPTY(l))
		return NULL;

	/*
	 * If list size's is 1 then no collisions. So
	 * Test and return the singleton.
	 */
	if (LIST_SIZE(l) == 1) {
		vif = ELEMENT_DATA(LIST_HEAD(l));
		return ((vif->fd_in == fd) && (vif->vrrp->vrid == vrid)) ? vif : NULL;
	}

	/*
	 * List collision on the vrid bucket. The same
	 * vrid is used on a different interface. We perform
	 * a fd lookup as collisions solver.
	 */
	for (e = LIST_HEAD(l); e; ELEMENT_NEXT(e)) {
		vif = ELEMENT_DATA(e);
		if ((vif->fd_in == fd) && (vif->vrrp->vrid == vrid))
			return vif;
	}

	/* No match */
	return NULL;
}

/* FD hash table */
void
alloc_vrrp_fd_bucket(vrrp_if *vif)
{
	/* We use a mod key plus 1 */
	list_add(&vrrp_data->vrrp_index_fd[vif->fd_in%1024 + 1], vif);
}

void
remove_vrrp_fd_bucket(vrrp_if *vif)
{
	list l = &vrrp_data->vrrp_index_fd[vif->fd_in%1024 + 1];
	list_del(l, vif);
}

void set_vrrp_fd_bucket(int old_fd, vrrp_if *vif)
{
	vrrp_t *vrrp_ptr;
	vrrp_if *vif_ptr;
	element e;
	element next;
	element e_vif;
	list l = &vrrp_data->vrrp_index_fd[old_fd%1024 + 1];

	/* Release old stalled entries */
	for (e = LIST_HEAD(l); e; e = next) {
		next = e->next;
		vif_ptr =  ELEMENT_DATA(e);
		if (vif_ptr->fd_in == old_fd) {
			if (e->prev)
				e->prev->next = e->next;
			else
				 l->head = e->next;

			if (e->next)
				e->next->prev = e->prev;
			else
				l->tail = e->prev;
			l->count--;
			FREE(e);
		}
	}
	if (LIST_ISEMPTY(l))
		l->head = l->tail = NULL;

	/* Hash refreshed entries */
	l = vrrp_data->vrrp;
	for (e = LIST_HEAD(l); e; ELEMENT_NEXT(e)) {
		vrrp_ptr = ELEMENT_DATA(e);
		for (e_vif = LIST_HEAD(vrrp_ptr->vrrp_if); e_vif; ELEMENT_NEXT(e_vif)) {
			vif_ptr =  ELEMENT_DATA(e);
			if (vif_ptr->fd_in == old_fd) {
				/* Update new hash */
				vif_ptr->fd_in = vif->fd_in;
				vif_ptr->fd_out = vif->fd_out;
				alloc_vrrp_fd_bucket(vif_ptr);
			}
		}
	}
}
