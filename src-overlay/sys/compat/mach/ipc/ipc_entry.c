/*-
 * Copyright (c) 2014-2015, Matthew Macy <mmacy@nextbsd.org>
 * Copyright (c) 2022, Zoe Knox <zoe@pixin.net>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 *  2. Neither the name of Matthew Macy nor the names of its
 *     contributors may be used to endorse or promote products derived from
 *     this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

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
/* CMU_HIST */
/*
 * Revision 2.7  91/10/09  16:08:15  af
 * 	 Revision 2.6.2.1  91/09/16  10:15:30  rpd
 * 	 	Added <ipc/ipc_hash.h>.
 * 	 	[91/09/02            rpd]
 * 
 * Revision 2.6.2.1  91/09/16  10:15:30  rpd
 * 	Added <ipc/ipc_hash.h>.
 * 	[91/09/02            rpd]
 * 
 * Revision 2.6  91/05/14  16:31:38  mrt
 * 	Correcting copyright
 * 
 * Revision 2.5  91/03/16  14:47:45  rpd
 * 	Fixed ipc_entry_grow_table to use it_entries_realloc.
 * 	[91/03/05            rpd]
 * 
 * Revision 2.4  91/02/05  17:21:17  mrt
 * 	Changed to new Mach copyright
 * 	[91/02/01  15:44:19  mrt]
 * 
 * Revision 2.3  91/01/08  15:12:58  rpd
 * 	Removed MACH_IPC_GENNOS.
 * 	[90/11/08            rpd]
 * 
 * Revision 2.2  90/06/02  14:49:36  rpd
 * 	Created for new IPC.
 * 	[90/03/26  20:54:27  rpd]
 * 
 */
/* CMU_ENDHIST */
/* 
 * Mach Operating System
 * Copyright (c) 1991,1990,1989 Carnegie Mellon University
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
 *	File:	ipc/ipc_entry.c
 *	Author:	Rich Draves
 *	Date:	1989
 *
 *	Primitive functions to manipulate translation entries.
 */

/*
 * Lock order (nextbsd-kernel#149, #151):
 *
 *  R1. The space rwlock (is_lock_data) comes before io_lock (the port or
 *      port-set mutex). ipc_object_translate() and ipc_right_reverse() take
 *      them in that order.
 *
 *  R2. ips_note_lock (the port set's knlist sx) comes before the space
 *      rwlock. kqueue_scan() calls filt_machport() with the knlist lock
 *      held, and filt_machport() translates the name, which takes the space
 *      lock. So never take ips_note_lock while holding a space lock: every
 *      KNOTE() site unlocks the space first (ipc_right_clean,
 *      ipc_right_destroy, ipc_right_delta), and so does ipc_pset_move()
 *      before ipc_pset_signal().
 *
 *  R3. Never hold a space lock across an M_WAITOK allocation. Allocate
 *      first, then lock (ipc_entry_get, ipc_entry_alloc_name), or drop
 *      the lock around it (ipc_entry_grow_table).
 *
 * ipc_entry_lookup() asserts the space lock and never takes it; the lock is
 * not recursive and callers already hold it, read or write.
 */


#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/types.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/eventhandler.h>
#include <sys/counter.h>
#include <sys/imgact.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/proc.h>
#include <sys/rwlock.h>
#include <sys/sysctl.h>
#include <sys/sysent.h>
#include <sys/time.h>
#include <sys/syslog.h>
#include <sys/limits.h>

#include <sys/mach/mach_types.h>
#include <sys/mach/kern_return.h>
#include <sys/mach/port.h>
#include <sys/mach/message.h>
#include <sys/mach/mach_port_server.h>
#include <vm/uma.h>
#include <sys/mach/ipc/port.h>
#include <sys/mach/ipc/ipc_entry.h>
#include <sys/mach/ipc/ipc_space.h>
#include <sys/mach/ipc/ipc_object.h>
#include <sys/mach/ipc/ipc_hash.h>
#include <sys/mach/ipc/ipc_table.h>
#include <sys/mach/ipc/ipc_port.h>
#include <sys/mach/ipc/ipc_pset.h>
#include <sys/mach/ipc/ipc_right.h>
#include <sys/mach/task.h>
#include <sys/mach/thread.h>

static void
ipc_entry_hash_delete(
	ipc_space_t space,
	ipc_entry_t entry)
{
	mach_port_index_t idx;
	ipc_entry_t entryp;

	if ((idx = entry->ie_index) == UINT_MAX)
		return;
	if ((entry->ie_bits & (MACH_PORT_TYPE_DEAD_NAME | MACH_PORT_TYPE_SEND)) !=
		MACH_PORT_TYPE_SEND)
		return;
	if (space->is_reverse_hash == NULL || space->is_reverse_size == 0)
		return;
	if (idx >= space->is_reverse_size)
		return;

	entryp = space->is_reverse_hash[idx];
	assert(entryp);

	if (entryp == entry) {
		space->is_reverse_hash[idx] = entry->ie_link;
		entry->ie_link = NULL;
		entry->ie_index = UINT_MAX;
	} else {
		while (entryp->ie_link != NULL) {
			if (entryp->ie_link == entry) {
				entryp->ie_link = entry->ie_link;
				entry->ie_link = NULL;
				entry->ie_index = UINT_MAX;
				break;
			}
			entryp = entryp->ie_link;
		}
	}
	/* assert that it was found */
	MPASS(entry->ie_index == UINT_MAX);
}

/*
 * Per-space entry cap (#151). It bounds the name INDEX, and with it both
 * the number of names a space can hold and the size of its slot array
 * (16 bytes a slot, so 1 MiB at the default). Port names are not fds
 * (#152), so the cap is the only bound on them: they no longer count
 * against RLIMIT_NOFILE or kern.maxfilesperproc. Lowering it never shrinks
 * a table; it only stops further growth.
 */
#define	IPC_SPACE_MAX_ENTRIES_DEFAULT	65536
#define	IPC_SPACE_MAX_ENTRIES_MIN	1024
#define	IPC_SPACE_MAX_ENTRIES_MAX	(MACH_PORT_INDEX_MASK + 1)

static u_int ipc_space_max_entries = IPC_SPACE_MAX_ENTRIES_DEFAULT;
static COUNTER_U64_DEFINE_EARLY(ipc_space_cap_hits);

SYSCTL_DECL(_mach);

static int
sysctl_ipc_space_max_entries(SYSCTL_HANDLER_ARGS)
{
	u_int val;
	int error;

	val = ipc_space_max_entries;
	error = sysctl_handle_int(oidp, &val, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	if (val < IPC_SPACE_MAX_ENTRIES_MIN || val > IPC_SPACE_MAX_ENTRIES_MAX)
		return (EINVAL);
	ipc_space_max_entries = val;
	return (0);
}
SYSCTL_PROC(_mach, OID_AUTO, ipc_space_max_entries,
    CTLTYPE_UINT | CTLFLAG_RWTUN | CTLFLAG_MPSAFE, NULL, 0,
    sysctl_ipc_space_max_entries, "IU",
    "Most port names one IPC space may hold (bounds the name index)");
SYSCTL_COUNTER_U64(_mach, OID_AUTO, ipc_space_cap_hits, CTLFLAG_RD,
    &ipc_space_cap_hits,
    "Port name allocations refused by mach.ipc_space_max_entries");

static void
ipc_space_cap_hit(mach_port_index_t index)
{
	static struct timeval lasttime;
	static int curpps;

	counter_u64_add(ipc_space_cap_hits, 1);
	if (ppsratecheck(&lasttime, &curpps, 1))
		log(LOG_WARNING, "mach: pid %d (%s): port name index %u is past "
		    "mach.ipc_space_max_entries (%u)\n", curproc->p_pid,
		    curproc->p_comm, index, ipc_space_max_entries);
}

/*
 *	Routine:	ipc_entry_grow_table
 *	Purpose:
 *		Grow the space's slot array until it covers `index`.
 *		Doubles from IPC_ENTRY_TABLE_MIN, clamped to the cap.
 *	Conditions:
 *		The space is write-locked. On KERN_SUCCESS it is still
 *		write-locked; on failure it is unlocked. The lock is
 *		dropped around the M_WAITOK allocation, with is_growing
 *		set, as in XNU; ipc_space_destroy() waits for that.
 *	Returns:
 *		KERN_SUCCESS		The table covers index.
 *		KERN_INVALID_TASK	The space is dead.
 *		KERN_NO_SPACE		index is at or past the cap.
 */
static kern_return_t
ipc_entry_grow_table(ipc_space_t space, mach_port_index_t index)
{
	struct ipc_entry_slot *otable, *ntable;
	ipc_entry_num_t nsize;
	u_int cap;

	is_assert_wlocked(space);
	for (;;) {
		if (!space->is_active || space->is_table == NULL) {
			is_write_unlock(space);
			return (KERN_INVALID_TASK);
		}
		if (index < space->is_table_size)
			return (KERN_SUCCESS);
		cap = ipc_space_max_entries;
		if (index >= cap) {
			is_write_unlock(space);
			ipc_space_cap_hit(index);
			return (KERN_NO_SPACE);
		}
		if (space->is_growing) {
			rw_sleep(space, &space->is_lock_data, 0, "ipcgrow", 0);
			continue;
		}

		nsize = MAX(space->is_table_size, IPC_ENTRY_TABLE_MIN);
		while (nsize <= index)
			nsize *= 2;
		if (nsize > cap)
			nsize = cap;

		space->is_growing = TRUE;
		is_write_unlock(space);
		ntable = malloc(nsize * sizeof(*ntable), M_MACH_IPC_TABLE,
		    M_WAITOK | M_ZERO);
		is_write_lock(space);
		space->is_growing = FALSE;
		wakeup(space);

		if (space->is_active && space->is_table != NULL &&
		    space->is_table_size < nsize) {
			otable = space->is_table;
			memcpy(ntable, otable,
			    space->is_table_size * sizeof(*ntable));
			space->is_table = ntable;
			space->is_table_size = nsize;
		} else
			otable = ntable;	/* died, or someone else grew it */

		/* Not with the space lock held (R3); then re-check. */
		is_write_unlock(space);
		free(otable, M_MACH_IPC_TABLE);
		is_write_lock(space);
	}
}

/*
 *	Routine:	ipc_entry_unpublish
 *	Purpose:
 *		Take an entry out of its slot, if it is still there, and
 *		advance the slot's generation so the name just freed is
 *		never reissued unchanged.
 *	Conditions:
 *		The space is write-locked.
 */
static void
ipc_entry_unpublish(ipc_space_t space, ipc_entry_t entry)
{
	struct ipc_entry_slot *slot;
	mach_port_index_t idx;

	is_assert_wlocked(space);
	if (!entry->ie_in_table)
		return;
	entry->ie_in_table = FALSE;

	idx = MACH_PORT_INDEX(entry->ie_name);
	if (space->is_table == NULL || idx >= space->is_table_size)
		return;
	slot = &space->is_table[idx];
	if (slot->ies_entry != entry)
		return;
	slot->ies_entry = IE_NULL;
	slot->ies_gen = IE_BITS_NEW_GEN(slot->ies_gen);
}

/*
 *	Routine:	ipc_entry_teardown
 *	Purpose:
 *		Take an entry out of its space and free it, destroying
 *		what it names on the way: a port set is destroyed, and a
 *		port right is given up as a dying space gives it up
 *		(ipc_right_clean_port): a receive right destroys the
 *		port, a send right drops its share of ip_srights and may
 *		fire no-more-senders, a send-once right is consumed with
 *		a send-once notification, and a dead-name request is
 *		cancelled.
 *	Conditions:
 *		Nothing locked, or only a port lock when the entry is
 *		already out of the table and holds no object (see
 *		ipc_entry_dealloc()). Takes the space lock only while the
 *		entry is still in the table. An entry that holds a port
 *		right sends notifications, so it must come in with
 *		nothing locked; only the exit/exec sweep passes one.
 *
 *	This was the body of mach_port_close(), the fo_close of a port
 *	name's fd, until names stopped being fds (#152). It is now the one
 *	place an entry is freed.
 */
static void
ipc_entry_teardown(ipc_space_t space, ipc_entry_t entry)
{
	static struct timeval lasttime;
	static int curpps;
	ipc_object_t object;
	ipc_pset_t pset;
	ipc_port_t port;
	mach_port_type_t type;
	boolean_t bad_receive;

	MPASS(entry->ie_space == space);

	/*
	 * ipc_entry_dealloc(), ipc_entry_close() and the exit/exec sweep take
	 * the entry out of its slot under the space lock before they get
	 * here -- ipc_entry_dealloc() with a port lock held, so taking the
	 * space lock here then would invert R1 -- and say so in ie_in_table,
	 * which only ever goes TRUE -> FALSE under the space lock.
	 */
	if (entry->ie_in_table) {
		is_write_lock(space);
		ipc_entry_unpublish(space, entry);
		if ((entry->ie_bits & MACH_PORT_TYPE_PORT_SET) == 0)
			ipc_entry_hash_delete(space, entry);
		is_write_unlock(space);
	} else if ((entry->ie_bits & MACH_PORT_TYPE_PORT_SET)  == 0)
		ipc_entry_hash_delete(space, entry);
	MPASS(entry->ie_link == NULL);
	if ((object = entry->ie_object) != NULL) {
		if (entry->ie_bits & MACH_PORT_TYPE_PORT_SET) {
			pset = (ipc_pset_t)object;
			ips_lock(pset);
			ipc_pset_destroy(pset);
		} else {
			port = (ipc_port_t)object;
			type = IE_BITS_TYPE(entry->ie_bits);
			/*
			 * A dying task's rights go the way XNU's
			 * ipc_right_clean takes them (#265). Dropping just
			 * the port reference, as this did, left ip_srights
			 * and ip_sorights counting a right nobody held, so
			 * the receiver never got no-more-senders or the
			 * send-once notification when a client died, and a
			 * dead-name request the task had armed stayed in
			 * the port's ip_dnrequests, holding a slot.
			 *
			 * The entry is out of its slot and the reverse hash,
			 * so nothing else can reach it; that stands in for
			 * the space write lock ipc_right_dncancel wants, as
			 * a dead space does for ipc_right_clean. Nothing is
			 * locked here: ipc_entry_dealloc(), the one caller
			 * that may hold a port lock, has cleared ie_object
			 * already, and ipc_entry_close()'s callers hand over
			 * entries already of type NONE, so in practice this
			 * is the exit/exec sweep, which drops the space lock
			 * around each entry.
			 *
			 * Anything else keeps the old handling below: a type
			 * with no port right that still has an object, or a
			 * receive right the port does not agree `space` holds
			 * under this name (logged; INVARIANTS is off, so the
			 * helper's asserts would not catch it).
			 */
			bad_receive = (type & MACH_PORT_TYPE_RECEIVE) != 0 &&
			    (port->ip_receiver != space ||
			    port->ip_receiver_name != entry->ie_name);
			if (__predict_false(bad_receive) &&
			    ppsratecheck(&lasttime, &curpps, 1))
				log(LOG_WARNING, "mach: %s: name 0x%x holds "
				    "the receive right to port %p, which "
				    "names 0x%x in space %p, not space %p\n",
				    __func__, entry->ie_name, port,
				    port->ip_receiver_name, port->ip_receiver,
				    space);
			if (__predict_true(!bad_receive &&
			    (type == MACH_PORT_TYPE_SEND ||
			    type == MACH_PORT_TYPE_SEND_ONCE ||
			    type == MACH_PORT_TYPE_RECEIVE ||
			    type == MACH_PORT_TYPE_SEND_RECEIVE))) {
				ipc_right_clean_port(space, entry->ie_name,
				    entry);
			} else if (port->ip_receiver == space) {
				ip_lock(port);
				ipc_port_clear_receiver(port);
				ipc_port_destroy(port);
			} else {
				ip_release(port);
			}
		}
		entry->ie_object = NULL;
	}
	free(entry, M_MACH_IPC_ENTRY);
}

/*
 *	Routine:	ipc_entry_lookup
 *	Purpose:
 *		Searches for an entry, given its name.
 *	Conditions:
 *		The space must be locked (read or write) and active.
 *
 *	Resolves through space->is_table only (#149, #151): this used to
 *	fget() the name in curthread's fd table, ignoring `space`, so a
 *	mach_port_* call on another task's space searched the caller's.
 *	XNU semantics: a name past the table, an empty slot, a stale
 *	generation (the slot's entry carries a different name) and an entry
 *	with no rights yet (type NONE) all fail. No fget(), so no Capsicum
 *	CAP_KQUEUE_* check on ports any more.
 */

ipc_entry_t
ipc_entry_lookup(ipc_space_t space, mach_port_name_t name)
{
	mach_port_index_t idx;
	ipc_entry_t entry;

	is_assert_locked(space);
	assert(space->is_active);

	idx = MACH_PORT_INDEX(name);
	if (space->is_table == NULL || idx >= space->is_table_size)
		return (IE_NULL);
	entry = space->is_table[idx].ies_entry;
	if (entry == IE_NULL)
		return (IE_NULL);
	if (entry->ie_name != name) {
		if (mach_debug_enable)
			log(LOG_DEBUG, "%s:%d stale port name 0x%x (slot holds "
			    "0x%x)\n", curproc->p_comm, curproc->p_pid, name,
			    entry->ie_name);
		return (IE_NULL);
	}
	if (IE_BITS_TYPE(entry->ie_bits) == MACH_PORT_TYPE_NONE)
		return (IE_NULL);
	return (entry);
}

/*
 * Lowest index ipc_entry_get() hands out. Names are no longer fds (#152),
 * but they start where the old fd-backed allocator started (the lowest
 * free fd at or above 16), so kernel-assigned names keep their shape.
 * Index 0 is never handed out, so no minted name is MACH_PORT_NULL.
 */
#define	IPC_ENTRY_INDEX_MIN	16

/*
 *	Routine:	ipc_entry_publish
 *	Purpose:
 *		Initialise an ipc_entry and put it in the empty slot at
 *		MACH_PORT_INDEX(*namep).
 *	Conditions:
 *		The space is write-locked, and the table already covers
 *		the index (ipc_entry_grow_table).
 *
 *		With pick_gen the generation half of *namep is chosen here,
 *		from the slot, and written back (ipc_entry_get); without it
 *		*namep is the caller's name and the slot takes its generation
 *		(ipc_entry_alloc_name).
 */
static void
ipc_entry_publish(
	ipc_space_t	space,
	mach_port_name_t	*namep,
	boolean_t	pick_gen,
	ipc_entry_t	entry)
{
	struct ipc_entry_slot *slot;
	mach_port_index_t idx;

	is_assert_wlocked(space);
	idx = MACH_PORT_INDEX(*namep);
	KASSERT(space->is_table != NULL && idx < space->is_table_size,
	    ("ipc_entry_publish: index %u outside table of %u", idx,
	    space->is_table_size));
	slot = &space->is_table[idx];
	KASSERT(slot->ies_entry == IE_NULL,
	    ("ipc_entry_publish: slot %u in use", idx));

	entry->ie_request = 0;
	entry->ie_index = UINT_MAX;
	entry->ie_link = NULL;
	entry->ie_space = space;

	if (pick_gen) {
		/* Kernel-assigned: never gen 0, never past GEN_MAX. */
		if (IE_BITS_GEN(slot->ies_gen) == 0 ||
		    IE_BITS_GEN(slot->ies_gen) > IE_BITS_GEN_MAX)
			slot->ies_gen = IE_BITS_GEN_ONE;
		*namep = MACH_PORT_MAKE(idx, slot->ies_gen);
	} else
		slot->ies_gen = MACH_PORT_GEN(*namep);
	slot->ies_entry = entry;
	entry->ie_in_table = TRUE;

	/*
	 * The same generation in the name and in ie_bits, on both the get
	 * and the alloc_name path. #160 and #163 left ie_bits at gen 0 while
	 * the name carried one. MACH_PORT_GEN() and IE_BITS_GEN_MASK are the
	 * same bits in the same positions (gen-high layout), so the name's
	 * generation half is the ie_bits value.
	 */
	entry->ie_name = *namep;
	entry->ie_bits = IE_BITS_GEN(MACH_PORT_GEN(*namep));
}

/*
 *	Routine:	ipc_entry_get
 *	Purpose:
 *		Tries to allocate an entry out of the space.
 *	Conditions:
 *		The space is active throughout. Nothing locked: this
 *		takes the space lock, so no object lock may be held (R1).
 *		Will try to allocate memory.
 *
 *		Takes the lowest free index at or above IPC_ENTRY_INDEX_MIN,
 *		growing the table when every slot is in use. The slot's
 *		generation has moved on since its last name was freed
 *		(ipc_entry_unpublish), so a reused index never reissues a
 *		name unchanged.
 *	Returns:
 *		KERN_SUCCESS		A free entry was found.
 *		KERN_NO_SPACE		Every index below the entry cap is
 *					in use.
 *		KERN_INVALID_TASK	The space died.
 *		KERN_RESOURCE_SHORTAGE	No memory.
 */
kern_return_t
ipc_entry_get(
	ipc_space_t	space,
	boolean_t	is_send_once,
	mach_port_name_t	*namep,
	ipc_entry_t	*entryp)
{
	ipc_entry_t free_entry;
	mach_port_index_t idx;
	mach_port_name_t name;
	kern_return_t kr;

	assert(space->is_active);

	/* M_WAITOK, so before the space lock (R3). */
	if ((free_entry = malloc(sizeof(*free_entry), M_MACH_IPC_ENTRY, M_WAITOK|M_ZERO)) == NULL)
		return KERN_RESOURCE_SHORTAGE;

	is_write_lock(space);
	for (idx = IPC_ENTRY_INDEX_MIN;; idx++) {
		/*
		 * A no-op while idx is inside the table; at its end, grow it
		 * or fail at the cap. Checks is_active either way, and
		 * unlocks the space on failure.
		 */
		if ((kr = ipc_entry_grow_table(space, idx)) != KERN_SUCCESS) {
			free(free_entry, M_MACH_IPC_ENTRY);
			return (kr);
		}
		if (space->is_table[idx].ies_entry == IE_NULL)
			break;
	}

	name = idx;	/* the index; publish adds the slot's generation */
	ipc_entry_publish(space, &name, TRUE, free_entry);
	is_write_unlock(space);
	*namep = name;
	*entryp = free_entry;

	return KERN_SUCCESS;
}

/*
 *	Routine:	ipc_entry_alloc
 *	Purpose:
 *		Allocate an entry out of the space.
 *	Conditions:
 *		The space is not locked before, but it is write-locked after
 *		if the call is successful.  May allocate memory.
 *	Returns:
 *		KERN_SUCCESS		An entry was allocated.
 *		KERN_INVALID_TASK	The space is dead.
 *		KERN_NO_SPACE		No room for an entry in the space.
 *		KERN_RESOURCE_SHORTAGE	Couldn't allocate memory for an entry.
 */

kern_return_t
ipc_entry_alloc(
	ipc_space_t	space,
	boolean_t	is_send_once,
	mach_port_name_t	*namep,
	ipc_entry_t	*entryp)
{
	kern_return_t kr;

	if (!space->is_active)
		return (KERN_INVALID_TASK);

	*namep = MACH_PORT_NAME_NULL;
	if ((kr = ipc_entry_get(space, is_send_once, namep, entryp)) != KERN_SUCCESS)
		return (kr);

	is_write_lock(space);
	return (0);
}

/*
 *	Routine:	ipc_entry_alloc_name
 *	Purpose:
 *		Allocates/finds an entry with a specific name.
 *		If an existing entry is returned, its type will be nonzero.
 *	Conditions:
 *		The space is not locked before, but it is write-locked after
 *		if the call is successful.  May allocate memory.
 *	Returns:
 *		KERN_SUCCESS		Found existing entry with same name.
 *		KERN_SUCCESS		Allocated a new entry.
 *		KERN_INVALID_TASK	The space is dead.
 *		KERN_NAME_EXISTS	The index is in use under another
 *					generation, or is index 0.
 *		KERN_NO_SPACE		Index at or past the entry cap.
 *		KERN_RESOURCE_SHORTAGE	Couldn't allocate memory.
 */

kern_return_t
ipc_entry_alloc_name(
	ipc_space_t	space,
	mach_port_name_t	name,
	ipc_entry_t	*entryp)
{
	mach_port_index_t idx;
	ipc_entry_t entry, new_entry;
	kern_return_t kr;

	if (!space->is_active) {
		return (KERN_INVALID_TASK);
	}
	assert(MACH_PORT_NAME_VALID(name));

	/* The cap, before any allocation or growth. */
	idx = MACH_PORT_INDEX(name);
	if (idx >= ipc_space_max_entries) {
		ipc_space_cap_hit(idx);
		return (KERN_NO_SPACE);
	}
	/* Never handed out by ipc_entry_get(); reserved, as in XNU. */
	if (idx == 0)
		return (KERN_NAME_EXISTS);

	/* M_WAITOK, so before the space lock (R3); freed if not needed. */
	if ((new_entry = malloc(sizeof(*new_entry), M_MACH_IPC_ENTRY,
	    M_WAITOK | M_ZERO)) == NULL)
		return (KERN_RESOURCE_SHORTAGE);

	is_write_lock(space);
	/* Checks is_active; unlocks the space on failure. */
	if ((kr = ipc_entry_grow_table(space, idx)) != KERN_SUCCESS) {
		free(new_entry, M_MACH_IPC_ENTRY);
		return (kr);
	}
	/*
	 * Inspect the slot itself rather than ipc_entry_lookup(), which
	 * hides an entry still being set up (type NONE). The same name with
	 * a right is that entry; anything else at this index -- another
	 * generation, or an allocation in flight -- owns the index.
	 */
	if ((entry = space->is_table[idx].ies_entry) != IE_NULL) {
		free(new_entry, M_MACH_IPC_ENTRY);
		if (entry->ie_name == name &&
		    IE_BITS_TYPE(entry->ie_bits) != MACH_PORT_TYPE_NONE) {
			*entryp = entry;
			return (KERN_SUCCESS);
		}
		is_write_unlock(space);
		return (KERN_NAME_EXISTS);
	}

	ipc_entry_publish(space, &name, FALSE, new_entry);
	*entryp = new_entry;
	return (KERN_SUCCESS);
}

/*
 *	Routine:	ipc_entry_close
 *	Purpose:
 *		Destroy a name: take it out of the table, then tear the
 *		entry down (ipc_entry_teardown).
 *	Conditions:
 *		Nothing locked; no port lock held (takes the space lock).
 *		Reads the slot directly, not through ipc_entry_lookup():
 *		callers close entries whose type has just gone to NONE.
 */
void
ipc_entry_close(
	ipc_space_t space,
	mach_port_name_t name)
{
	mach_port_index_t idx;
	ipc_entry_t entry;

	idx = MACH_PORT_INDEX(name);
	is_write_lock(space);
	if (space->is_table != NULL && idx < space->is_table_size &&
	    (entry = space->is_table[idx].ies_entry) != IE_NULL &&
	    entry->ie_name == name) {
		ipc_entry_unpublish(space, entry);
		if ((entry->ie_bits & MACH_PORT_TYPE_PORT_SET) == 0)
			ipc_entry_hash_delete(space, entry);
	} else
		entry = IE_NULL;
	is_write_unlock(space);

	if (entry != IE_NULL)
		ipc_entry_teardown(space, entry);
}

/*
 *	Routine:	ipc_entry_dealloc
 *	Purpose:
 *		Deallocates an entry from a space.
 *	Conditions:
 *		The space must be write-locked.
 *		The space is unlocked on return.
 *		The space must be active.
 *		A port lock may be held (ipc_right_delta, ipc_right_destroy),
 *		so the slot is cleared under the lock the caller already
 *		holds and nothing after the unlock takes the space lock again.
 */

void
ipc_entry_dealloc(
	ipc_space_t	space,
	mach_port_name_t	name,
	ipc_entry_t	entry)
{
	assert(space->is_active);
	assert(entry->ie_object == IO_NULL);
	assert(entry->ie_request == 0);
	MPASS(space == entry->ie_space);

	ipc_entry_hash_delete(space, entry);
	ipc_entry_unpublish(space, entry);
	is_write_unlock(space);
	MPASS(entry->ie_link == NULL);

	/* Out of the table and holding no object: only frees it. */
	ipc_entry_teardown(space, entry);
}

/*
 * Destroy all of p's port names. Shared by the exit and exec hooks.
 */
static void
ipc_entry_list_close(struct proc *p)
{
	ipc_entry_t entry;
	ipc_space_t space;
	mach_port_index_t i;
	int pass;

	/*
	 * Out-of-tree fix: this event handler is registered for every
	 * process_exit / process_exec in the kernel, but only processes
	 * that have been initialized with a Mach task have p_machdata
	 * set (via mach_task_init). Without that guard, ipc_entry_list_close
	 * was page-faulting on current_space() -> p_machdata->itk_space
	 * for any non-Mach process, including kldload itself right after
	 * loading mach.ko. Skip processes that never picked up Mach state,
	 * and non-native ABI ones, whose p_machdata is not a task at all
	 * (see mach_proc_task(), nextbsd-kernel#63/#66).
	 */
	if (mach_proc_task(p) == TASK_NULL ||
	    mach_proc_task(curthread->td_proc) == TASK_NULL)
		return;

	space = current_space();

	/*
	 * Out-of-tree fix (nextbsd-kernel#63): p_machdata being set does NOT
	 * imply the task owns an IPC space. A process whose ABI never allocates
	 * one -- a Linux binary under COMPAT_LINUX acquires a mach task lazily
	 * but no itk_space -- reaches here with current_space() == NULL, and the
	 * walk below then dereferences NULL and panics in exit1(). Every Linux
	 * process died on exit(2).
	 */
	if (space == NULL)
		return;
	KASSERT(space == mach_proc_task(p)->itk_space,
	    ("ipc_entry_list_close: %p is not pid %d's space", space,
	    p->p_pid));

	/*
	 * Port sets first, then everything else, as the fd-table walks this
	 * replaces did (#152). Each entry leaves its slot and the reverse
	 * hash before the lock drops, so nothing else can reach it while it
	 * is torn down unlocked (R1, R2). An entry still of type NONE is an
	 * allocation in flight -- only another task can be making one, the
	 * process is single-threaded here -- and its allocator owns it.
	 */
	for (pass = 0; pass < 2; pass++) {
		is_write_lock(space);
		for (i = 0; space->is_table != NULL &&
		    i < space->is_table_size; i++) {
			entry = space->is_table[i].ies_entry;
			if (entry == IE_NULL ||
			    IE_BITS_TYPE(entry->ie_bits) == MACH_PORT_TYPE_NONE)
				continue;
			if (pass == 0 &&
			    (entry->ie_bits & MACH_PORT_TYPE_PORT_SET) == 0)
				continue;
			ipc_entry_unpublish(space, entry);
			if ((entry->ie_bits & MACH_PORT_TYPE_PORT_SET) == 0)
				ipc_entry_hash_delete(space, entry);
			is_write_unlock(space);
			ipc_entry_teardown(space, entry);
			is_write_lock(space);
		}
		is_write_unlock(space);
	}
}

/*
 * process_exit: close the names, then tear the task down
 * (nextbsd-kernel#266). The order matters: mach_task_exit() will not destroy
 * a space that still has entries in it, and those only go away as the names
 * are closed above.
 */
static void
ipc_entry_exit_hook(void *arg __unused, struct proc *p)
{

	ipc_entry_list_close(p);
	mach_task_exit(mach_proc_task(p));
}

/*
 * process_exec: names die with exec; the task, its space and its special
 * ports (bootstrap above all, which launchd sets before fork and expects the
 * child to keep across execve) carry over, as in XNU.
 *
 * This runs before sv_onexec, so p->p_sysent is still the old ABI and
 * imgp->sysent the new one. Leaving a Linux image (Linux -> native): the old
 * p_emuldata is a linux_pemuldata, which mach_proc_task() refuses, and the
 * Linux layer frees it afterwards. Entering one (native -> Linux): the task
 * is torn down and detached now, before linux_on_exec() installs its pem in
 * the same slot.
 */
static void
ipc_entry_exec_hook(void *arg __unused, struct proc *p,
    struct image_params *imgp)
{
	task_t task;

	task = mach_proc_task(p);
	if (task == TASK_NULL)
		return;

	ipc_entry_list_close(p);

	if ((imgp->sysent->sv_flags & SV_ABI_MASK) != SV_ABI_FREEBSD)
		mach_task_orphan(p);
}

static void
ipc_entry_sysinit(void *arg __unused)
{

	/*
	 * Re-enabled (were #if 0 /PhaseB-bisect/, 2026-05-20): without
	 * these, every process that touched Mach IPC leaked its entire
	 * IPC space on exit/exec — leaked ports and stale send rights to
	 * launchd's bootstrap port accumulated until client RPC degraded
	 * (MACH_SEND_INVALID_DEST / hangs). The page-fault that caused
	 * the original Phase B bisect is guarded inside
	 * ipc_entry_list_close (the p_machdata == NULL early return).
	 */
	EVENTHANDLER_REGISTER(process_exit, ipc_entry_exit_hook, NULL, EVENTHANDLER_PRI_ANY);
	EVENTHANDLER_REGISTER(process_exec, ipc_entry_exec_hook, NULL, EVENTHANDLER_PRI_ANY);
}

SYSINIT(ipc_entry, SI_SUB_KLD, SI_ORDER_ANY, ipc_entry_sysinit, NULL);

#if	MACH_KDB
#include <ddb/db_output.h>
#define	printf	kdbprintf

ipc_entry_t	db_ipc_object_by_name(
			task_t		task,
			mach_port_name_t	name);


ipc_entry_t
db_ipc_object_by_name(
	task_t		task,
	mach_port_name_t	name)
{
        ipc_space_t space = task->itk_space;
        ipc_entry_t entry;
 
 
        entry = ipc_entry_lookup(space, name);
        if(entry != IE_NULL) {
                iprintf("(task 0x%x, name 0x%x) ==> object 0x%x\n",
			task, name, entry->ie_object);
                return (ipc_entry_t) entry->ie_object;
        }
        return entry;
}
#endif	/* MACH_KDB */
