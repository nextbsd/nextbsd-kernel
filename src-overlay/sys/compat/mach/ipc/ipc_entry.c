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
 *  R3. Never hold a space lock across FILEDESC_XLOCK, fdrop() or an
 *      M_WAITOK allocation. fdrop() can be the last reference and run
 *      mach_port_close(), which takes the space lock itself.
 *
 * ipc_entry_lookup() asserts the space lock and never takes it; the lock is
 * not recursive and callers already hold it, read or write.
 */


#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include "opt_capsicum.h"

#include <sys/types.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/eventhandler.h>
#include <sys/capsicum.h>
#include <sys/counter.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/imgact.h>
#include <sys/fcntl.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/rwlock.h>
#include <sys/syscallsubr.h>
#include <sys/sysctl.h>
#include <sys/sysent.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/syslog.h>
#include <sys/user.h>
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
#include <sys/mach/task.h>
#include <sys/mach/thread.h>

#include <security/audit/audit.h>

static void fdunused(struct filedesc *fdp, int fd);
static int kern_fdalloc(struct thread *td, int minfd, int *result);
static void kern_fddealloc(struct thread *td, int fd);
static inline void kern_fdfree(struct filedesc *fdp, int fd);
static int kern_finstall(struct thread *td, struct file *fp, int *fd, int flags,
			 struct filecaps *fcaps);

#define MODERN (__FreeBSD_version >= 1100000)
#define FNOFDALLOC    0x80000000
#define        fde_change_size (offsetof(struct filedescent, fde_seqc))

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
 * (16 bytes a slot, so 1 MiB at the default). While the index is also an
 * fd (until #152) the cap shares the fd number space: a process whose fds
 * already run past it cannot get more port names. Lowering it never
 * shrinks a table; it only stops further growth.
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
 * Until #152 every entry is backed by an fd in the CALLING process, so a
 * name can only be allocated into the caller's own space. Lookups honour
 * whatever space they are given; allocation into another task's space is
 * refused with KERN_INVALID_TASK (#151 D8). #152 lifts this.
 */
static boolean_t
ipc_entry_space_is_curproc(ipc_space_t space)
{
	task_t task;

	task = mach_proc_task(curproc);
	return (task != TASK_NULL && task->itk_space == space);
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

static fo_close_t mach_port_close;
static fo_stat_t mach_port_stat;
#if MODERN
static fo_fill_kinfo_t mach_port_fill_kinfo;
#endif

struct fileops mach_fileops  = {
	.fo_close = mach_port_close,
	.fo_stat = mach_port_stat,
#if MODERN
	.fo_fill_kinfo = mach_port_fill_kinfo,
#endif
	.fo_flags = 0,
};

static int
mach_port_close(struct file *fp, struct thread *td)
{
	ipc_entry_t entry;
	ipc_object_t object;
	ipc_pset_t pset;
	ipc_port_t port;
	ipc_space_t space;

	MACH_VERIFY(fp->f_data != NULL, ("expected fp->f_data != NULL - got NULL\n"));
	if ((entry = fp->f_data) == NULL)
		return (0);

	/*
	 * close(2), dup2(2) and the exit/exec sweep drop an fd whose name is
	 * still in the table; the slot must not outlive the entry freed
	 * below. ipc_entry_dealloc() and ipc_entry_close() already cleared it
	 * on their paths -- some with a port lock held, so taking the space
	 * lock here then would invert R1 -- and say so in ie_in_table, which
	 * only ever goes TRUE -> FALSE under the space lock. The space itself
	 * outlives its linked entries: mach_task_exit() leaks a non-empty
	 * space rather than destroy it.
	 */
	space = entry->ie_space;
	if (entry->ie_in_table && space != IS_NULL) {
		is_write_lock(space);
		ipc_entry_unpublish(space, entry);
		if ((entry->ie_bits & MACH_PORT_TYPE_PORT_SET) == 0)
			ipc_entry_hash_delete(space, entry);
		is_write_unlock(space);
	} else if ((entry->ie_bits & MACH_PORT_TYPE_PORT_SET)  == 0)
		ipc_entry_hash_delete(entry->ie_space, entry);
	MPASS(entry->ie_link == NULL);
	PROC_LOCK(td->td_proc);
	LIST_REMOVE(entry, ie_space_link);
	PROC_UNLOCK(td->td_proc);
	if ((object = entry->ie_object) != NULL) {
		if (entry->ie_bits & MACH_PORT_TYPE_PORT_SET) {
			pset = (ipc_pset_t)object;
			ips_lock(pset);
			ipc_pset_destroy(pset);
		} else {
			port = (ipc_port_t)object;
			if (port->ip_receiver == current_space()) {
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
	fp->f_data = NULL;

	return (0);
}

static int
mach_port_stat(struct file *fp __unused, struct stat *sb,
			   struct ucred *active_cred __unused)
{
	ipc_entry_t entry;

	bzero((caddr_t)sb, sizeof(*sb));

	entry = fp->f_data;
	if (entry->ie_bits & MACH_PORT_TYPE_PORT_SET) {
		sb->st_mode = S_IFPSET;
	} else {
		sb->st_mode = S_IFPORT;
	}
	return (0);
}

#if MODERN
static int
mach_port_fill_kinfo(struct file *fp, struct kinfo_file *kif,
					 struct filedesc *fdp __unused)
{
	ipc_entry_t entry;

	/* assume it's a port first */
	kif->kf_type = KF_TYPE_PORT;

	if ((entry = fp->f_data) == NULL)
		return (0);
	/* What else do we want from it? */
	if (entry->ie_bits & MACH_PORT_TYPE_PORT_SET) {
		kif->kf_type = KF_TYPE_PORTSET;
	}

	return (0);
}
#endif

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

kern_return_t
ipc_entry_file_to_port(ipc_space_t space, mach_port_name_t name, ipc_object_t *objectp)
{
	struct file *fp;
	ipc_port_t port;
	cap_rights_t rights;

	assert(space->is_active);

	/*
	 * A Mach name carries a generation; a plain fd never does. Refuse
	 * anything with one before touching the fd table, so a Mach name can
	 * never be wrapped as an implicit fileport (#161, folded into #151).
	 * Deleting this path is #152's.
	 */
	if (MACH_PORT_GEN(name) != 0)
		return (KERN_INVALID_ARGUMENT);

	if (curthread->td_proc->p_fd == NULL)
		return (KERN_INVALID_ARGUMENT);

	if (fget(curthread, name, cap_rights_init(&rights, CAP_ALL1), &fp) != 0) {
		log(LOG_DEBUG, "%s:%d entry for port name: %d not found\n", curproc->p_comm, curproc->p_pid, name);
		return (KERN_INVALID_ARGUMENT);
	}
	if (fp->f_type == DTYPE_MACH_IPC) {
		fdrop(fp, curthread);
		return (KERN_INVALID_ARGUMENT);
	}
	if ((port = ipc_port_alloc_special(space)) == NULL)
		return (KERN_RESOURCE_SHORTAGE);

	port->ip_context = (mach_vm_address_t) fp;
	port->ip_flags = IP_CONTEXT_FILE;
	port->ip_receiver = space;
	port->ip_receiver_name = name;

	*objectp = (ipc_object_t)port;
	return (KERN_SUCCESS);
}

void
ipc_entry_file_destroy(ipc_object_t objectp)
{
	ipc_port_t port;
	struct file *fp;

	if (curthread->td_proc->p_fd == NULL)
		return;

	port = (ipc_port_t)objectp;
	fp = (void *)port->ip_context;
	ipc_port_dealloc_special(port, current_space());
	fdrop(fp, curthread);
}

kern_return_t
ipc_entry_port_to_file(ipc_space_t space, mach_port_name_t *namep, ipc_object_t object)
{
	ipc_port_t port;
	struct file *fp;

	port = (ipc_port_t)object;
	MPASS(object != NULL);
	MPASS(port->ip_flags & IP_CONTEXT_FILE);
	fp = (void *)port->ip_context;
	/* the receiver will have been set by the sender of the port */
	port->ip_receiver = space;
	ipc_port_dealloc_special(port, space);

	/* Are sent file O_CLOEXEC? */
	if (kern_finstall(curthread, fp, namep, 0, NULL) != 0) {
		fdrop(fp, curthread);
		if (mach_debug_enable)
			printf("finstall failed\n");
		return (KERN_RESOURCE_SHORTAGE);
	}
	if (mach_debug_enable)
		printf(" installing received file *fp=%p at %d\n", fp, *namep);
	return (KERN_SUCCESS);
}

/*
 *	Routine:	ipc_entry_get
 *	Purpose:
 *		Tries to allocate an entry out of the space.
 *	Conditions:
 *		The space is active throughout. Nothing locked: this
 *		takes the space lock, so no object lock may be held (R1).
 *		Will try to allocate memory.
 *	Returns:
 *		KERN_SUCCESS		A free entry was found.
 *		KERN_NO_SPACE		Index at or past the entry cap.
 *		KERN_INVALID_TASK	Not the caller's own space (D8),
 *					or the space died.
 *		KERN_RESOURCE_SHORTAGE	No fd or memory.
 */

/*
 *	Routine:	ipc_entry_publish
 *	Purpose:
 *		Initialise an ipc_entry for a struct file already installed at
 *		the fd MACH_PORT_INDEX(*namep), wire the two together, and put
 *		the entry in the space's table and entry list.
 *	Conditions:
 *		Nothing locked. The fd is allocated, `fp` is installed at it,
 *		and the table already covers the index (ipc_entry_grow_table).
 *		Consumes the caller's reference on fp.
 *
 *		With pick_gen the generation half of *namep is chosen here,
 *		from the slot, and written back (ipc_entry_get); without it
 *		*namep is the caller's name and the slot takes its generation
 *		(ipc_entry_alloc_name).
 *
 *	Shared by ipc_entry_get(), which allocates the fd itself, and
 *	ipc_entry_alloc_name(), which is handed a specific one. Factored out
 *	because alloc_name used to call ipc_entry_get() to do this -- and
 *	ipc_entry_get() allocates its OWN fd, which is the bug.
 */
static void
ipc_entry_publish(
	ipc_space_t	space,
	struct file	*fp,
	mach_port_name_t	*namep,
	boolean_t	pick_gen,
	ipc_entry_t	entry)
{
	struct thread *td = curthread;
	struct ipc_entry_slot *slot;
	mach_port_index_t idx;
	ipc_entry_t stale;

	idx = MACH_PORT_INDEX(*namep);
	entry->ie_request = 0;
	entry->ie_fp = fp;
	entry->ie_index = UINT_MAX;
	entry->ie_link = NULL;
	entry->ie_space = space;
	entry->ie_in_table = FALSE;

	is_write_lock(space);
	KASSERT(space->is_table != NULL && idx < space->is_table_size,
	    ("ipc_entry_publish: index %u outside table of %u", idx,
	    space->is_table_size));
	if (space->is_table != NULL && idx < space->is_table_size) {
		slot = &space->is_table[idx];
		/*
		 * We hold the fd at idx, so anything still in the slot lost
		 * its fd to close(2)/dup2(2) and has not reached
		 * mach_port_close() yet. Retire it as that would have: its
		 * name must not be reissued, and its reverse-hash link must
		 * not dangle.
		 */
		if ((stale = slot->ies_entry) != IE_NULL) {
			stale->ie_in_table = FALSE;
			ipc_entry_hash_delete(space, stale);
			slot->ies_gen = IE_BITS_NEW_GEN(slot->ies_gen);
		}
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
	}
	/*
	 * The same generation in the name and in ie_bits, on both the get
	 * and the alloc_name path. #160 and #163 left ie_bits at gen 0 while
	 * the name carried one. MACH_PORT_GEN() and IE_BITS_GEN_MASK are the
	 * same bits in the same positions (gen-high layout), so the name's
	 * generation half is the ie_bits value.
	 */
	entry->ie_name = *namep;
	entry->ie_bits = IE_BITS_GEN(MACH_PORT_GEN(*namep));
	is_write_unlock(space);

	PROC_LOCK(curproc);
	LIST_INSERT_HEAD(&space->is_entry_list, entry, ie_space_link);
	PROC_UNLOCK(curproc);
	finit(fp, 0, DTYPE_MACH_IPC, entry, &mach_fileops);
	fdrop(fp, td);
}

kern_return_t
ipc_entry_get(
	ipc_space_t	space,
	boolean_t	is_send_once,
	mach_port_name_t	*namep,
	ipc_entry_t	*entryp)
{	
	ipc_entry_t free_entry;
	mach_port_name_t name;
	kern_return_t kr;
	int fd;
	struct file *fp;
	struct thread *td;

	assert(space->is_active);

	if (!ipc_entry_space_is_curproc(space))
		return (KERN_INVALID_TASK);	/* D8, until #152 */

	td  = curthread;
	if ((free_entry = malloc(sizeof(*free_entry), M_MACH_IPC_ENTRY, M_WAITOK|M_ZERO)) == NULL)
		return KERN_RESOURCE_SHORTAGE;

	/*
	 * fd work happens unlocked (R3). Every failure frees free_entry,
	 * which used to leak on each of these returns.
	 */
	if (kern_fdalloc(td, 16, &fd)) {
		log(LOG_WARNING, "%s:%d failed to allocate fd\n", __FILE__, __LINE__);
		free(free_entry, M_MACH_IPC_ENTRY);
		return (KERN_RESOURCE_SHORTAGE);
	}
	if ((u_int)fd >= ipc_space_max_entries) {
		kern_fddealloc(td, fd);
		free(free_entry, M_MACH_IPC_ENTRY);
		ipc_space_cap_hit(fd);
		return (KERN_NO_SPACE);
	}
	is_write_lock(space);
	if ((kr = ipc_entry_grow_table(space, fd)) != KERN_SUCCESS) {
		/* space is unlocked */
		kern_fddealloc(td, fd);
		free(free_entry, M_MACH_IPC_ENTRY);
		return (kr);
	}
	is_write_unlock(space);

	if (falloc_noinstall(td, &fp)) {
		kern_fddealloc(td, fd);
		free(free_entry, M_MACH_IPC_ENTRY);
		log(LOG_WARNING, "%s:%d failed to allocate fp\n", __FILE__, __LINE__);
		return (KERN_RESOURCE_SHORTAGE);
	}
	if (kern_finstall(td, fp, &fd, FNOFDALLOC, NULL)) {
		log(LOG_WARNING, "%s:%d failed to allocate fp:%p at fd:%d \n", __FILE__, __LINE__, fp, fd);
		kern_fddealloc(td, fd);
		fdrop(fp, td);
		free(free_entry, M_MACH_IPC_ENTRY);
		return (KERN_RESOURCE_SHORTAGE);
	}

	name = fd;	/* the index; publish adds the slot's generation */
	ipc_entry_publish(space, fp, &name, TRUE, free_entry);
	/* The fd table's reference; urefs live in ie_bits (#150). */
	assert(fp->f_count == 1);
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
 *		KERN_INVALID_TASK	The space is dead, or is not the
 *					caller's own space (D8, until #152).
 *		KERN_NAME_EXISTS	The index is in use under another
 *					generation, or by a non-Mach fd.
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
	int fd, newfd;
	struct file *fp;
	ipc_entry_t entry;
	kern_return_t kr;
	struct thread *td = curthread;

	if (!space->is_active) {
		return (KERN_INVALID_TASK);
	}
	assert(MACH_PORT_NAME_VALID(name));

	/* The cap, before any fd reservation or growth. */
	idx = MACH_PORT_INDEX(name);
	if (idx >= ipc_space_max_entries) {
		ipc_space_cap_hit(idx);
		return (KERN_NO_SPACE);
	}
	if (!ipc_entry_space_is_curproc(space))
		return (KERN_INVALID_TASK);	/* D8, until #152 */

	is_write_lock(space);
	if (!space->is_active) {
		is_write_unlock(space);
		return (KERN_INVALID_TASK);
	}
	/*
	 * Inspect the slot itself rather than ipc_entry_lookup(), which
	 * hides an entry still being set up (type NONE). The same name with
	 * a right is that entry; anything else at this index -- another
	 * generation, or an allocation in flight -- owns the index.
	 */
	if (idx < space->is_table_size &&
	    (entry = space->is_table[idx].ies_entry) != IE_NULL) {
		if (entry->ie_name == name &&
		    IE_BITS_TYPE(entry->ie_bits) != MACH_PORT_TYPE_NONE) {
			*entryp = entry;
			return (KERN_SUCCESS);
		}
		is_write_unlock(space);
		return (KERN_NAME_EXISTS);
	}
	if ((kr = ipc_entry_grow_table(space, idx)) != KERN_SUCCESS)
		return (kr);	/* space is unlocked */
	is_write_unlock(space);
	fd = idx;

	/*
	 * Allocate the entry before touching the fd table, exactly as
	 * ipc_entry_get() does. Once kern_finstall() has installed the fp
	 * there is no clean undo with the primitives available here --
	 * kern_fddealloc() does not unwind an installed file -- so every
	 * failure that can happen must happen before that point.
	 */
	if ((entry = malloc(sizeof(*entry), M_MACH_IPC_ENTRY,
	    M_WAITOK | M_ZERO)) == NULL)
		return (KERN_RESOURCE_SHORTAGE);

	/* The fd is the index half of the name, never the whole name. */
	if (kern_fdalloc(td, fd, &newfd)) {
		log(LOG_WARNING, "%s:%d failed to allocate %d\n", __FILE__, __LINE__, fd);
		free(entry, M_MACH_IPC_ENTRY);
		return (KERN_RESOURCE_SHORTAGE);
	}
	if (newfd != fd) {
		kern_fddealloc(td, newfd);
		free(entry, M_MACH_IPC_ENTRY);
		return (KERN_NAME_EXISTS);
	}
	if (falloc_noinstall(td, &fp)) {
		kern_fddealloc(td, newfd);
		free(entry, M_MACH_IPC_ENTRY);
		return (KERN_RESOURCE_SHORTAGE);
	}
	if (kern_finstall(td, fp, &fd, FNOFDALLOC, NULL)) {
		kern_fddealloc(td, newfd);
		fdrop(fp, td);
		free(entry, M_MACH_IPC_ENTRY);
		return (KERN_RESOURCE_SHORTAGE);
	}
	/*
	 * Build the entry AT the fd we just reserved.
	 *
	 * This used to call ipc_entry_get(), which allocates its own fd from
	 * kern_fdalloc(td, 16, ...) and overwrites `name` with it. The result
	 * was that the caller asked for a specific name and got an entry
	 * carrying a different one, while the fd actually reserved here kept
	 * a bare falloc_noinstall() file -- never finit()'d, so badfileops,
	 * so ipc_entry_lookup() rejected it (f_type != DTYPE_MACH_IPC) and it
	 * was never reclaimed until process exit.
	 *
	 * Every caller was affected. ipc_object_alloc_name() set ie_object on
	 * the wrong-named entry; ipc_object_copyout_name() -- which is on the
	 * live mach_msg path -- passed the REQUESTED name to
	 * ipc_right_copyout(), which stamped it into port->ip_receiver_name,
	 * a name that then never resolved; and ipc_object_rename() could not
	 * work at all.
	 */
	ipc_entry_publish(space, fp, &name, FALSE, entry);
	*entryp = entry;

	is_write_lock(space);
	return (KERN_SUCCESS);
}

/*
 * Close the fd backing `name` in the calling process. Refuses an fd that
 * does not hold this name's entry, so a stale name never closes whatever
 * reused its index. Nothing locked (R3).
 */
static void
ipc_entry_close_fd(mach_port_name_t name)
{
	struct filedesc *fdp;
	struct file *fp;
	struct thread *td;
	int fd;

	td = curthread;
	fdp = td->td_proc->p_fd;
	fd = MACH_PORT_INDEX(name);

	FILEDESC_XLOCK(fdp);
	if ((fp = fget_noref(fdp, fd)) == NULL) {
		FILEDESC_XUNLOCK(fdp);
		audit_sysclose(td, fd, NULL);
		return;
	}
	if (fp->f_type != DTYPE_MACH_IPC || fp->f_data == NULL ||
	    ((ipc_entry_t)fp->f_data)->ie_name != name) {
		FILEDESC_XUNLOCK(fdp);
		return;
	}

	audit_sysclose(td, fd, fp);

	/* No knote_fdclose(): EVFILT_MACHPORT is not an fd filter (#147),
	 * its knote holds a port-set reference, not one on this fp. It is
	 * reaped by kq scan or close, and a recycled name is re-homed by
	 * filt_machporttouch() (#169).
	 */
	kern_fdfree(fdp, fd);
	FILEDESC_XUNLOCK(fdp);
	fdrop(fp, td);
}

/*
 *	Routine:	ipc_entry_close
 *	Purpose:
 *		Destroy a name: take it out of the table, then close its fd.
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
	    entry->ie_name == name)
		ipc_entry_unpublish(space, entry);
	is_write_unlock(space);

	ipc_entry_close_fd(name);
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
	/*
	 * Entries only ever live in the caller's own space until #152 (D8);
	 * this used to swap to entry->ie_space's lock when they differed.
	 */
	MPASS(space == entry->ie_space);

	ipc_entry_hash_delete(space, entry);
	ipc_entry_unpublish(space, entry);
	is_write_unlock(space);
	MPASS(entry->ie_link == NULL);

	ipc_entry_close_fd(name);
}

static void
kern_last_close(struct thread *td, struct file *fp, struct filedesc *fdp, int fd)
{

	FILEDESC_XLOCK(fdp);
	knote_fdclose(td, fd);
	kern_fdfree(fdp, fd);
	FILEDESC_XUNLOCK(fdp);
	fdrop(fp, td);
}

/*
 * Close every Mach name p holds through its fd table: port sets first, then
 * ports. Kept apart so that #152 (names stop being fds) can delete it in one
 * hunk.
 */
static void
ipc_entry_close_fd_names(struct proc *p)
{
	struct filedesc *fdp;
	struct filedescent *fde;
	struct file *fp;
	struct thread *td;
	ipc_entry_t entry;
	int i, lastfile;

	fdp = p->p_fd;
	td = curthread;

	FILEDESC_SLOCK(fdp);
	lastfile = fdlastfile(fdp);
	FILEDESC_SUNLOCK(fdp);
	for (i = 0; i <= lastfile; i++) {
		fde = &fdp->fd_ofiles[i];
		fp = fde->fde_file;
		if (fp == NULL || (fp->f_type != DTYPE_MACH_IPC))
			continue;
		MPASS(fp->f_count > 0);

		if (fp->f_data == NULL) {
			log(LOG_WARNING, "%s:%d fd: %d has NULL f_data\n", p->p_comm, p->p_pid, i);
			kern_last_close(td, fp, fdp, i);
			continue;
		}
		entry = fp->f_data;
		MPASS(entry->ie_bits != 0xdeadc0de);
		if ((entry->ie_bits & MACH_PORT_TYPE_PORT_SET) == 0)
			continue;
		kern_last_close(td, fp, fdp, i);

		FILEDESC_SLOCK(fdp);
		lastfile = fdlastfile(fdp);
		FILEDESC_SUNLOCK(fdp);
	}

	FILEDESC_SLOCK(fdp);
	lastfile = fdlastfile(fdp);
	FILEDESC_SUNLOCK(fdp);
	for (i = 0; i <= lastfile; i++) {

		fde = &fdp->fd_ofiles[i];
		fp = fde->fde_file;
		if (fp == NULL || (fp->f_type != DTYPE_MACH_IPC))
			continue;
		MPASS(fp->f_count > 0);

		entry = fp->f_data;
#if 0
		if (fp->f_count > 1) {
			int ispset = (entry->ie_bits & MACH_PORT_TYPE_PORT_SET);
			log(LOG_WARNING, "%s:%d fd: %d %s refcount: %d\n", p->p_comm, p->p_pid, i,
				ispset ? "pset" : "port", fp->f_count);
		}
#endif
		kern_last_close(td, fp, fdp, i);

		FILEDESC_SLOCK(fdp);
		lastfile = fdlastfile(fdp);
		FILEDESC_SUNLOCK(fdp);
	}

#ifdef INVARIANTS
	FILEDESC_SLOCK(fdp);
	for (i = 0; i <= fdlastfile(fdp); i++) {
		fde = &fdp->fd_ofiles[i];
		fp = fde->fde_file;
		if (fp != NULL)
			MPASS(fp->f_type != DTYPE_MACH_IPC);
	}
	FILEDESC_SUNLOCK(fdp);
#endif
}

/*
 * Destroy all of p's port names. Shared by the exit and exec hooks.
 */
static void
ipc_entry_list_close(struct proc *p)
{
	struct filedesc *fdp;
	struct file *fp;
	struct thread *td;
	ipc_entry_t entry;
	ipc_space_t space;
	int i;
	static int shared_logged;

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

	fdp = p->p_fd;
	td = curthread;
	space = current_space();

	/*
	 * A shared fd table (rfork(2) without RFFDG) also holds the names of
	 * every other process sharing it; closing all of its Mach fds would
	 * destroy their ports. This used to be KASSERT(fd_refcnt == 1), which
	 * compiles out without INVARIANTS. Leave the names alone instead: the
	 * entries still belong to files the table holds, so the drain below
	 * must not run either, and mach_task_exit() leaks the non-empty space
	 * rather than destroying it.
	 */
	if (fdp->fd_refcnt > 1) {
		if (!shared_logged) {
			shared_logged = 1;
			log(LOG_WARNING, "%s:%d: shared fd table, Mach port "
			    "names not closed\n", p->p_comm, p->p_pid);
		}
		return;
	}

	ipc_entry_close_fd_names(p);

	/*
	 * Out-of-tree fix (nextbsd-kernel#63): p_machdata being set does NOT
	 * imply the task owns an IPC space. A process whose ABI never allocates
	 * one -- a Linux binary under COMPAT_LINUX acquires a mach task lazily
	 * but no itk_space -- reaches here with current_space() == NULL, and the
	 * walk below then reads NULL + offsetof(struct ipc_space, is_entry_list)
	 * == 0x28 and panics in exit1(). Every Linux process died on exit(2).
	 *
	 * The descriptor loops above stay: such a process can still have
	 * inherited DTYPE_MACH_IPC descriptors that must be closed. It is only
	 * the space-owned entry list that has nothing to free.
	 */
	if (space == NULL)
		return;

	/* free unreferenced ipc_entrys */
	i = 0;
	while(!LIST_EMPTY(&space->is_entry_list)) {
		entry = LIST_FIRST(&space->is_entry_list);
		/* mach_port_close removes the entry */
		fp = entry->ie_fp;
		MPASS(fp->f_count > 0);
		fp->f_count = 1;
		fdrop(fp, td);
		/* ensure no infinite loop */
		MPASS(i++ < 10000);
	}
}

/*
 * process_exit: close the names, then tear the task down
 * (nextbsd-kernel#266). The order matters: mach_task_exit() will not destroy
 * a space that still has entries linked into it, and those only go away as
 * their fds are closed above.
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


#define NDFILE		20
#define NDSLOTSIZE	sizeof(NDSLOTTYPE)
#define	NDENTRIES	(NDSLOTSIZE * __CHAR_BIT)
#define NDSLOT(x)	((x) / NDENTRIES)
#define NDBIT(x)	((NDSLOTTYPE)1 << ((x) % NDENTRIES))
#define	NDSLOTS(x)	(((x) + NDENTRIES - 1) / NDENTRIES)


/*
 * Find the highest non-zero bit in the given bitmap, starting at 0 and
 * not exceeding size - 1. Return -1 if not found.
 */
static int
fd_last_used(struct filedesc *fdp, int size)
{
	NDSLOTTYPE *map = fdp->fd_map;
	NDSLOTTYPE mask;
	int off, minoff;

	off = NDSLOT(size);
	if (size % NDENTRIES) {
		mask = ~(~(NDSLOTTYPE)0 << (size % NDENTRIES));
		if ((mask &= map[off]) != 0)
			return (off * NDENTRIES + flsl(mask) - 1);
		--off;
	}
	for (minoff = NDSLOT(0); off >= minoff; --off)
		if (map[off] != 0)
			return (off * NDENTRIES + flsl(map[off]) - 1);
	return (-1);
}

static int
fdisused(struct filedesc *fdp, int fd)
{

	FILEDESC_LOCK_ASSERT(fdp);

	KASSERT(fd >= 0 && fd < fdp->fd_nfiles,
	    ("file descriptor %d out of range (0, %d)", fd, fdp->fd_nfiles));

	return ((fdp->fd_map[NDSLOT(fd)] & NDBIT(fd)) != 0);
}

/*
 * Mark a file descriptor as unused.
 */
static void
fdunused(struct filedesc *fdp, int fd)
{

	FILEDESC_XLOCK_ASSERT(fdp);

	KASSERT(fdisused(fdp, fd), ("fd=%d is already unused", fd));
	KASSERT(fdp->fd_ofiles[fd].fde_file == NULL,
	    ("fd=%d is still in use", fd));

	fdp->fd_map[NDSLOT(fd)] &= ~NDBIT(fd);
	if (fd < fdp->fd_freefile)
		fdp->fd_freefile = fd;
}

static int
kern_fdalloc(struct thread *td, int minfd, int *result)
{
	struct proc *p = td->td_proc;
	struct filedesc *fdp = p->p_fd;
	int rc;
	FILEDESC_XLOCK(fdp);
	rc = fdalloc(td, minfd, result);
	FILEDESC_XUNLOCK(fdp);
	return (rc);
}

static void
kern_fddealloc(struct thread *td, int fd)
{
	struct proc *p = td->td_proc;
	struct filedesc *fdp = p->p_fd;
	FILEDESC_XLOCK(fdp);
	fdunused(fdp, fd);
	FILEDESC_XUNLOCK(fdp);
}

static inline void
kern_fdfree(struct filedesc *fdp, int fd)
{
	struct filedescent *fde;

	fde = &fdp->fd_ofiles[fd];
#ifdef CAPABILITIES
	seqc_write_begin(&fde->fde_seqc);
#endif
	bzero(fde, fde_change_size);
	fdunused(fdp, fd);
#ifdef CAPABILITIES
	seqc_write_end(&fde->fde_seqc);
#endif
}

static void
filecaps_fill(struct filecaps *fcaps)
{

	CAP_ALL(&fcaps->fc_rights);
	fcaps->fc_ioctls = NULL;
	fcaps->fc_nioctls = -1;
	fcaps->fc_fcntls = CAP_FCNTL_ALL;
}

/*
 * Install a file in a file descriptor table.
 */
static int
kern_finstall(struct thread *td, struct file *fp, int *fd, int flags,
    struct filecaps *fcaps)
{
	struct filedesc *fdp = td->td_proc->p_fd;
	struct filedescent *fde;
	int error, min;

	KASSERT(fd != NULL, ("%s: fd == NULL", __func__));
	KASSERT(fp != NULL, ("%s: fp == NULL", __func__));

	min = 16;

	FILEDESC_XLOCK(fdp);
	if (!(flags & FNOFDALLOC)) {
		if ((error = fdalloc(td, min, fd))) {
			FILEDESC_XUNLOCK(fdp);
			return (error);
		}
	}
	if (!fhold(fp)) {
		FILEDESC_XUNLOCK(fdp);
		return (1); // Caller will fddealloc and fdrop
	}
	fde = &fdp->fd_ofiles[*fd];
#ifdef CAPABILITIES
	seqc_write_begin(&fde->fde_seqc);
#endif
	fde->fde_file = fp;
	if ((flags & O_CLOEXEC) != 0)
		fde->fde_flags |= UF_EXCLOSE;
	filecaps_fill(&fde->fde_caps);
#ifdef CAPABILITIES
	seqc_write_end(&fde->fde_seqc);
#endif
	FILEDESC_XUNLOCK(fdp);
	return (0);
}
 
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
