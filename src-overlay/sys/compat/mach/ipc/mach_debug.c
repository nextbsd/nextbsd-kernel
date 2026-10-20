/*
 * Copyright 1991-1998 by Open Software Foundation, Inc. 
 *              All Rights Reserved 
 *  
 * Permission to use, copy, modify, and distribute this software and 
 * its documentation for any purpose and without fee is hereby granted, 
 * provided that the above copyright notice appears in all copies and 
 * that both the copyright notice and this permission notice appear in 
 * supporting documentation. 
 *  
 * OSF DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE 
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS 
 * FOR A PARTICULAR PURPOSE. 
 *  
 * IN NO EVENT SHALL OSF BE LIABLE FOR ANY SPECIAL, INDIRECT, OR 
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM 
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN ACTION OF CONTRACT, 
 * NEGLIGENCE, OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION 
 * WITH THE USE OR PERFORMANCE OF THIS SOFTWARE. 
 */
/*
 * MkLinux
 */
/* CMU_HIST */
/*
 * Revision 2.5.5.1  92/03/03  16:18:59  jeffreyh
 * 	Eliminate keep_wired argument from vm_map_copyin().
 * 	[92/02/21  10:13:00  dlb]
 * 	Changes from TRUNK
 * 	[92/02/26  11:52:22  jeffreyh]
 * 
 * Revision 2.6  92/01/14  16:44:48  rpd
 * 	Changed host_ipc_hash_info, host_ipc_marequest_info,
 * 	and mach_port_space_info for CountInOut.
 * 	[92/01/14            rpd]
 * 	Added mach_port_kernel_object.
 * 	[91/12/14            rpd]
 * 
 * Revision 2.5  91/05/14  16:38:28  mrt
 * 	Correcting copyright
 * 
 * Revision 2.4  91/02/05  17:24:30  mrt
 * 	Changed to new Mach copyright
 * 	[91/02/01  15:52:50  mrt]
 * 
 * Revision 2.3  91/01/08  15:14:55  rpd
 * 	Changed ipc_info_bucket_t to hash_info_bucket_t.
 * 	[91/01/02            rpd]
 * 	Removed MACH_IPC_GENNOS.
 * 	[90/11/08            rpd]
 * 
 * Revision 2.2  90/06/02  14:52:15  rpd
 * 	Created for new IPC.
 * 	[90/03/26  21:05:22  rpd]
 * 
 */
/* CMU_ENDHIST */
/* 
 * Mach Operating System
 * Copyright (c) 1991,1990 Carnegie Mellon University
 * All Rights Reserved.
 * 
 * Permission to use, copy, modify and distribute this software and its
 * documentation is hereby granted, provided that both the copyright
 * notice and this permission notice appear in all copies of the
 * software, derivative works or modified versions, and any portions
 * thereof, and that both notices appear in supporting documentation.
 * 
 * CARNEGIE MELLON ALLOWS FREE USE OF THIS SOFTWARE IN ITS "AS IS"
 * CONDITION.  CARNEGIE MELLON DISCLAIMS ANY LIABILITY OF ANY KIND FOR
 * ANY DAMAGES WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.
 * 
 * Carnegie Mellon requests users of this software to return to
 * 
 *  Software Distribution Coordinator  or  Software.Distribution@CS.CMU.EDU
 *  School of Computer Science
 *  Carnegie Mellon University
 *  Pittsburgh PA 15213-3890
 * 
 * any improvements or extensions that they make and grant Carnegie Mellon
 * the rights to redistribute these changes.
 */
/*
 */
/*
 *	File:	ipc/mach_debug.c
 *	Author:	Rich Draves
 *	Date:	1989
 *
 *	Exported kernel calls.  See mach_debug/mach_debug.defs.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/rwlock.h>
#include <sys/mach/kern_return.h>
#include <sys/mach/port.h>
#include <sys/mach/vm_types.h>
#include <sys/mach_debug/ipc_info.h>
#include <sys/mach_debug/hash_info.h>
#if 0
#include <sys/mach/host.h>
#include <kern/misc_protos.h>
#include <vm/vm_map.h>
#include <vm/vm_kern.h>
#endif
#include <sys/mach/ipc/ipc_space.h>
#include <sys/mach/ipc/ipc_port.h>
#include <sys/mach/ipc/ipc_hash.h>
#include <sys/mach/ipc/ipc_table.h>
#include <sys/mach/ipc/ipc_right.h>
#include <sys/mach/mach_port_server.h>

#include <vm/vm.h>
#include <vm/vm_kern.h>
#include <vm/vm_extern.h>

#include <crypto/siphash/siphash.h>

/*
 * nextbsd-kernel#155: the wire layout of the ipc_info types is all
 * 32-bit fields on every arch, and must match mach_debug_types.defs
 * (struct[6]/[6]/[7]/[9] of natural_t) and userland's ipc_info.h.
 */
CTASSERT(sizeof(ipc_info_space_t) == 6 * sizeof(natural_t));
CTASSERT(sizeof(ipc_info_space_basic_t) == 6 * sizeof(natural_t));
CTASSERT(sizeof(ipc_info_name_t) == 7 * sizeof(natural_t));
CTASSERT(sizeof(ipc_info_tree_name_t) == 9 * sizeof(natural_t));
CTASSERT(sizeof(natural_t) == 4);
/* gen-high name layout (#151): the generation is the top byte */
CTASSERT(MACH_PORT_NGEN(MACH_PORT_NAME_DEAD) == 0xff000000U);

/*
 * iin_object is a natural_t, so it cannot carry a 64-bit pointer, and a
 * kernel address must not leave the kernel anyway. Report a per-boot
 * keyed SipHash of the object pointer instead: stable within one boot,
 * so a tool can match a send right to its receive right across tasks,
 * without revealing the address (the intent of XNU's VM_KERNEL_ADDRPERM).
 * NULL objects report 0; a hash that folds to 0 reports 1.
 */
static uint8_t ipc_info_objid_key[SIPHASH_KEY_LENGTH];

static void
ipc_info_objid_sysinit(void *arg __unused)
{
	arc4random_buf(ipc_info_objid_key, sizeof(ipc_info_objid_key));
}
SYSINIT(ipc_info_objid, SI_SUB_KLD, SI_ORDER_ANY, ipc_info_objid_sysinit,
    NULL);

static natural_t
ipc_info_objid(const void *obj)
{
	SIPHASH_CTX ctx;
	uintptr_t addr;
	uint64_t h;
	natural_t id;

	if (obj == NULL)
		return (0);
	addr = (uintptr_t)obj;
	h = SipHash24(&ctx, ipc_info_objid_key, &addr, sizeof(addr));
	id = (natural_t)(h ^ (h >> 32));
	return (id != 0 ? id : 1);
}

/*
 *	Routine:	mach_port_get_srights [kernel call]
 *	Purpose:
 *		Retrieve the number of extant send rights
 *		that a receive right has.
 *	Conditions:
 *		Nothing locked.
 *	Returns:
 *		KERN_SUCCESS		Retrieved number of send rights.
 *		KERN_INVALID_TASK	The space is null.
 *		KERN_INVALID_TASK	The space is dead.
 *		KERN_INVALID_NAME	The name doesn't denote a right.
 *		KERN_INVALID_RIGHT	Name doesn't denote receive rights.
 */

kern_return_t
mach_port_get_srights(
	ipc_space_t		space,
	mach_port_name_t		name,
	mach_port_rights_t	*srightsp)
{
	ipc_port_t port;
	kern_return_t kr;
	mach_port_rights_t srights;

	if (space == IS_NULL)
		return KERN_INVALID_TASK;

	kr = ipc_port_translate_receive(space, name, &port);
	if (kr != KERN_SUCCESS)
		return kr;
	/* port is locked and active */

	srights = port->ip_srights;
	ip_unlock(port);

	*srightsp = srights;
	return KERN_SUCCESS;
}
#if 0
/*
 *	Routine:	host_ipc_hash_info
 *	Purpose:
 *		Return information about the global reverse hash table.
 *	Conditions:
 *		Nothing locked.  Obeys CountInOut protocol.
 *	Returns:
 *		KERN_SUCCESS		Returned information.
 *		KERN_INVALID_HOST	The host is null.
 *		KERN_RESOURCE_SHORTAGE	Couldn't allocate memory.
 */

kern_return_t
host_ipc_hash_info(
	host_t				host,
	hash_info_bucket_array_t	*infop,
	mach_msg_type_number_t 		*countp)
{
	vm_offset_t addr;
	vm_size_t size;
	hash_info_bucket_t *info;
	unsigned int potential, actual;
	kern_return_t kr;

	if (host == HOST_NULL)
		return KERN_INVALID_HOST;

	/* start with in-line data */

	info = *infop;
	potential = *countp;

	for (;;) {
		actual = ipc_hash_info(info, potential);
		if (actual <= potential)
			break;

		/* allocate more memory */

		if (info != *infop)
			kmem_free(ipc_kernel_map, addr, size);

		size = round_page(actual * sizeof *info);
		kr = kmem_alloc_pageable(ipc_kernel_map, &addr, size);
		if (kr != KERN_SUCCESS)
			return KERN_RESOURCE_SHORTAGE;

		info = (hash_info_bucket_t *) addr;
		potential = size/sizeof *info;
	}

	if (info == *infop) {
		/* data fit in-line; nothing to deallocate */

		*countp = actual;
	} else if (actual == 0) {
		kmem_free(ipc_kernel_map, addr, size);

		*countp = 0;
	} else {
		vm_map_copy_t copy;
		vm_size_t used;

		used = round_page(actual * sizeof *info);

		if (used != size)
			kmem_free(ipc_kernel_map, addr + used, size - used);

		kr = vm_map_copyin(kernel_map, addr, used,
				   TRUE, &copy);
		assert(kr == KERN_SUCCESS);

		*infop = (hash_info_bucket_t *) copy;
		*countp = actual;
	}

	return KERN_SUCCESS;
}
#endif

/*
 *	Routine:	mach_port_space_info
 *	Purpose:
 *		Returns information about an IPC space: one
 *		ipc_info_name_t per name-table slot, and an empty
 *		tree array (the splay tree is gone).
 *	Conditions:
 *		Nothing locked.
 *	Returns:
 *		KERN_SUCCESS		Returned information.
 *		KERN_INVALID_TASK	The space is null.
 *		KERN_INVALID_TASK	The space is dead.
 *		KERN_RESOURCE_SHORTAGE	Couldn't allocate memory.
 *
 *	nextbsd-kernel#155: rewritten for the #151 slot table. The old
 *	body assumed old-MIG in-line CountInOut buffers and ipc_kernel_map
 *	wired memory, and copied the raw ie_object pointer out. This one
 *	allocates with the space unlocked (it is an rwlock), retries if the
 *	table grew, and returns the array the way mach_port_get_set_status
 *	does: vm_map_copyin(kernel_map, ...), which frees the M_MACH_VM
 *	source.
 */

kern_return_t
mach_port_space_info(
	ipc_space_t			space,
	ipc_info_space_t		*infop,
	ipc_info_name_array_t		*tablep,
	mach_msg_type_number_t 		*tableCntp,
	ipc_info_tree_name_array_t	*treep,
	mach_msg_type_number_t 		*treeCntp)
{
	ipc_info_name_t *table_info;
	ipc_entry_num_t bound;		/* table_info holds this many */
	struct ipc_entry_slot *table;
	ipc_entry_num_t tsize;
	mach_port_index_t index;
	vm_map_copy_t copy;
	kern_return_t kr;

	if (space == IS_NULL)
		return KERN_INVALID_TASK;

	table_info = NULL;
	bound = 0;
	for (;;) {
		is_read_lock(space);
		if (!space->is_active) {
			is_read_unlock(space);
			free(table_info, M_MACH_VM);
			return KERN_INVALID_TASK;
		}
		tsize = space->is_table_size;
		if (table_info != NULL && tsize <= bound)
			break;
		is_read_unlock(space);

		free(table_info, M_MACH_VM);
		bound = tsize;
		table_info = malloc(MAX(bound, 1) * sizeof(*table_info),
		    M_MACH_VM, M_WAITOK | M_ZERO);
	}
	/* space is read-locked and active; we have enough memory */

	bzero(infop, sizeof(*infop));
	infop->iis_genno_mask = MACH_PORT_NGEN(MACH_PORT_NAME_DEAD);
	infop->iis_table_size = tsize;
	/*
	 * The name table doubles up to mach.ipc_space_max_entries (#151);
	 * is_table_next now sizes only the reverse hash, so there is no
	 * meaningful "next size" to report.
	 */
	infop->iis_table_next = 0;
	/* iis_tree_size, iis_tree_small, iis_tree_hash: no tree, 0 */

	table = space->is_table;
	for (index = 0; index < tsize; index++) {
		ipc_info_name_t *iin = &table_info[index];
		ipc_entry_t entry = table[index].ies_entry;
		ipc_entry_bits_t bits;

		if (entry == IE_NULL) {
			/* empty: the name this slot would issue next */
			iin->iin_name = MACH_PORT_MAKE(index,
			    table[index].ies_gen);
			continue;
		}

		bits = entry->ie_bits;
		iin->iin_name = entry->ie_name;
		iin->iin_collision = 0;
		iin->iin_type = IE_BITS_TYPE(bits);
		if ((bits & MACH_PORT_TYPE_PORT_RIGHTS) != MACH_PORT_TYPE_NONE &&
		    entry->ie_request != 0)
			iin->iin_type |= MACH_PORT_TYPE_DNREQUEST;
		iin->iin_urefs = IE_BITS_UREFS(bits);
		iin->iin_object = ipc_info_objid(entry->ie_object);
		iin->iin_next = entry->ie_next;
		iin->iin_hash = entry->ie_index;
	}
	is_read_unlock(space);

	if (tsize == 0) {
		free(table_info, M_MACH_VM);
		copy = VM_MAP_COPY_NULL;
	} else {
		/* frees table_info, on failure too */
		kr = vm_map_copyin(kernel_map, (vm_offset_t)table_info,
		    tsize * sizeof(*table_info), FALSE, &copy);
		if (kr != KERN_SUCCESS)
			return KERN_RESOURCE_SHORTAGE;
	}

	*tablep = (ipc_info_name_t *) copy;
	*tableCntp = tsize;
	*treep = (ipc_info_tree_name_t *) VM_MAP_COPY_NULL;
	*treeCntp = 0;
	return KERN_SUCCESS;
}

/*
 *	Routine:	mach_port_dnrequest_info
 *	Purpose:
 *		Returns information about the dead-name requests
 *		registered with the named receive right.
 *	Conditions:
 *		Nothing locked.
 *	Returns:
 *		KERN_SUCCESS		Retrieved information.
 *		KERN_INVALID_TASK	The space is null.
 *		KERN_INVALID_TASK	The space is dead.
 *		KERN_INVALID_NAME	The name doesn't denote a right.
 *		KERN_INVALID_RIGHT	Name doesn't denote receive rights.
 */

kern_return_t
mach_port_dnrequest_info(
	ipc_space_t	space,
	mach_port_name_t	name,
	unsigned int	*totalp,
	unsigned int	*usedp)
{
	unsigned int total, used;
	ipc_port_t port;
	kern_return_t kr;

	if (space == IS_NULL)
		return KERN_INVALID_TASK;

	kr = ipc_port_translate_receive(space, name, &port);
	if (kr != KERN_SUCCESS)
		return kr;
	/* port is locked and active */

	if (port->ip_dnrequests == IPR_NULL) {
		total = 0;
		used = 0;
	} else {
		ipc_port_request_t dnrequests = port->ip_dnrequests;
		ipc_port_request_index_t index;

		total = dnrequests->ipr_size->its_size;

		for (index = 1, used = 0;
		     index < total; index++) {
			ipc_port_request_t ipr = &dnrequests[index];

			if (ipr->ipr_name != MACH_PORT_NAME_NULL)
				used++;
		}
	}
	ip_unlock(port);

	*totalp = total;
	*usedp = used;
	return KERN_SUCCESS;
}

#if 0
/*
 *	Routine:	mach_port_kernel_object [kernel call]
 *	Purpose:
 *		Retrieve the type and address of the kernel object
 *		represented by a send or receive right.
 *	Conditions:
 *		Nothing locked.
 *	Returns:
 *		KERN_SUCCESS		Retrieved kernel object info.
 *		KERN_INVALID_TASK	The space is null.
 *		KERN_INVALID_TASK	The space is dead.
 *		KERN_INVALID_NAME	The name doesn't denote a right.
 *		KERN_INVALID_RIGHT	Name doesn't denote
 *					send or receive rights.
 */

kern_return_t
mach_port_kernel_object(
	ipc_space_t	space,
	mach_port_name_t	name,
	unsigned int	*typep,
	/* vm_offset_t */	unsigned int *addrp)
{
	ipc_entry_t entry;
	ipc_port_t port;
	kern_return_t kr;

	kr = ipc_right_lookup_read(space, name, &entry);
	if (kr != KERN_SUCCESS)
		return kr;
	/* space is read-locked and active */

	if ((entry->ie_bits & MACH_PORT_TYPE_SEND_RECEIVE) == 0) {
		is_read_unlock(space);
		return KERN_INVALID_RIGHT;
	}

	port = (ipc_port_t) entry->ie_object;
	assert(port != IP_NULL);

	ip_lock(port);
	is_read_unlock(space);

	if (!ip_active(port)) {
		ip_unlock(port);
		return KERN_INVALID_RIGHT;
	}

	*typep = (unsigned int) ip_kotype(port);
	*addrp = (vm_offset_t) port->ip_kobject;
	ip_unlock(port);
	return KERN_SUCCESS;
}
#endif


/*
 *	Routine:	mach_port_kobject [kernel call]
 *	Purpose:
 *		Retrieve the type and address of the kernel object
 *		represented by a send or receive right. Returns
 *		the kernel address in a mach_vm_address_t to
 *		mask potential differences in kernel address space
 *		size.
 *	Conditions:
 *		Nothing locked.
 *	Returns:
 *		KERN_SUCCESS		Retrieved kernel object info.
 *		KERN_INVALID_TASK	The space is null.
 *		KERN_INVALID_TASK	The space is dead.
 *		KERN_INVALID_NAME	The name doesn't denote a right.
 *		KERN_INVALID_RIGHT	Name doesn't denote
 *					send or receive rights.
 */

#if !MACH_IPC_DEBUG
kern_return_t
mach_port_kobject(
	__unused ipc_space_t		space,
	__unused mach_port_name_t	name,
	__unused natural_t		*typep,
	__unused mach_vm_address_t	*addrp)
{
        return KERN_FAILURE;
}
#endif
