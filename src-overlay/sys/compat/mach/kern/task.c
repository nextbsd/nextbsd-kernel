/*-
 * Copyright (c) 2014-2015, Matthew Macy <mmacy@nextbsd.org>
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
/* 
 * Mach Operating System
 * Copyright (c) 1991,1990,1989,1988 Carnegie Mellon University
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
 *	File:	kern/task.c
 *	Author:	Avadis Tevanian, Jr., Michael Wayne Young, David Golub,
 *		David Black
 *
 *	Task management primitives implementation.
 */
/*
 * Copyright (c) 1993 The University of Utah and
 * the Computer Systems Laboratory (CSL).  All rights reserved.
 *
 * Permission to use, copy, modify and distribute this software and its
 * documentation is hereby granted, provided that both the copyright
 * notice and this permission notice appear in all copies of the
 * software, derivative works or modified versions, and any portions
 * thereof, and that both notices appear in supporting documentation.
 *
 * THE UNIVERSITY OF UTAH AND CSL ALLOW FREE USE OF THIS SOFTWARE IN ITS "AS
 * IS" CONDITION.  THE UNIVERSITY OF UTAH AND CSL DISCLAIM ANY LIABILITY OF
 * ANY KIND FOR ANY DAMAGES WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.
 *
 * CSL requests users of this software to return to csl-dist@cs.utah.edu any
 * improvements that they make and grant CSL redistribution rights.
 *
 */

#include <sys/cdefs.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/errno.h>
#include <sys/eventhandler.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/proc.h>
#include <sys/resource.h>
#include <sys/sched.h>
#include <sys/sx.h>
#include <sys/sysctl.h>
#include <sys/sysent.h>
#include <sys/systm.h>
#include <sys/taskqueue.h>
#include <sys/mach/vm_types.h>

#include <sys/mach/task_info.h>
#include <sys/mach/task_special_ports.h>
#include <sys/mach/mach_types.h>
#include <sys/mach/rpc.h>
#include <sys/mach/ipc/ipc_space.h>
#include <sys/mach/ipc/ipc_entry.h>
#include <sys/mach/mach_param.h>

#include <sys/mach/task.h>
#include <sys/mach/ipc/ipc_kmsg.h>
#include <sys/mach/thread.h>

#include <sys/mach/sched_prim.h>	/* for thread_wakeup */
#include <sys/mach/ipc_tt.h>
#if 0
#include <sys/mach/ledger.h>
#endif
#include <sys/mach/host_special_ports.h>
#include <sys/mach/host.h>
#include <vm/vm_kern.h>		/* for kernel_map, ipc_kernel_map */
#include <vm/uma.h>
#if	MACH_KDB
#include <ddb/db_sym.h>
#endif	/* MACH_KDB */

#include <sys/mach/queue.h>
/*
 * Exported interfaces
 */
#include <sys/mach/task_server.h>
#include <sys/mach/mach_host_server.h>
#include <sys/mach/mach_port_server.h>

struct processor_set default_pset;
unsigned	int sched_ticks;
task_t	kernel_task;
uma_zone_t	task_zone;

/* Forwards */


kern_return_t	task_hold_locked(
			task_t		task);
void		task_wait_locked(
			task_t		task);
kern_return_t	task_release(
			task_t		task);
void		task_act_iterate(
			task_t		 task,
			kern_return_t	(*func)(thread_act_t inc));
void		task_free(
			task_t		task );
void		task_synchronizer_destroy_all(
			task_t		task);


kern_return_t
task_create(
	task_t				parent_task,
	__unused ledger_array_t	ledger_ports,
	__unused mach_msg_type_number_t	num_ledger_ports,
	__unused boolean_t		inherit_memory,
	__unused task_t			*child_task)	/* OUT */
{
	if (parent_task == TASK_NULL)
		return(KERN_INVALID_ARGUMENT);

	return(KERN_FAILURE);
}


static kern_return_t
task_create_internal(
	task_t		new_task)
{

	/* one ref for just being alive; one for our caller */
	new_task->ref_count = 2;
	new_task->semaphores_owned = 0;

	ipc_task_create(new_task);

	return (KERN_SUCCESS);
}

static kern_return_t
task_init_internal(
	task_t		parent_task,
	task_t		new_task)
{
#ifdef notyet
	register processor_set_t	pset;
#endif

	/* one ref for just being alive; one for our caller */
	new_task->ref_count = 2;
	new_task->semaphores_owned = 0;

	ipc_task_init(new_task, parent_task);

	if (parent_task != TASK_NULL) {
#ifdef notyet		
		pset = parent_task->processor_set;
		if (!pset->active)
			pset = &default_pset;
#endif
		set_security_token(new_task);
		new_task->policy = parent_task->policy;
	} else {
#ifdef notyet
		pset = &default_pset;
#endif
		new_task->policy = POLICY_TIMESHARE;
		new_task->sec_token = KERNEL_SECURITY_TOKEN;
		new_task->audit_token = KERNEL_AUDIT_TOKEN;
	}
	ipc_task_enable(new_task);
	return(KERN_SUCCESS);
}



/*
 *	task_free:
 *
 *	Called by task_deallocate when the task's reference count drops to zero.
 *	Task is locked.
 */
void
task_free( register task_t	task )
{
	/*
	 * Never freed. The task struct belongs to the proc struct, which is
	 * type-stable and reused by later processes; per-process state is torn
	 * down by mach_task_exit() and rebuilt by mach_task_recycle() instead
	 * (see the lifecycle comment above mach_task_init). The code below is
	 * the original Mach free path and is unreachable.
	 */
	task_unlock(task);
	return;
	/*
	 * Temporarily restore the reference we dropped above, then
	 * freeze the task so that the task->processor_set field
	 * cannot change. In the !MACH_HOST case, the logic can be
	 * simplified, since the default_pset is the only pset.
	 */
	++task->ref_count;
	task_unlock(task);
	task_lock(task);
	if (--task->ref_count > 0) {
		/*
		 * A new reference appeared (probably from the pset).
		 * Back out. Must unfreeze inline since we'already
		 * dropped our reference.
		 */
		task_unlock(task);
		return;
	}
	task_unlock(task);
	is_release(task->itk_space);
	uma_zfree(task_zone, task);
}


void
task_deallocate( task_t task )
{
	if (task != TASK_NULL) {
	    int x;
	    task_lock(task);
	    x = --task->ref_count;
	    if (x == 0)
		task_free(task);	/* unlocks task */
	    else
		task_unlock(task);
	}
}

void
task_reference( register task_t task )
{
	if (task != TASK_NULL) {
	    task_lock(task);
	    task->ref_count++;
	    task_unlock(task);
	}
}

/*
 *	task_terminate:
 *
 *	Terminate the specified task.  See comments on thread_terminate
 *	(kern/thread.c) about problems with terminating the "current task."
 */
kern_return_t
task_terminate(
	register task_t	task)
{

	/*
	 * nextbsd-kernel#300: convert_port_to_task() now returns the port's
	 * own task, which may be TASK_NULL or another process's. Only the
	 * IPC teardown below is implemented: nothing stops the target's
	 * threads, so tearing down another task's space while it runs is
	 * refused rather than attempted.
	 */
	if (task == TASK_NULL)
		return (KERN_INVALID_ARGUMENT);
	if (task != current_task())
		return (KERN_FAILURE);
#ifdef notyet	
	register thread_t	thread, cur_thread;
#if 0
	register queue_head_t	*list;
#endif	
	register task_t		cur_task;
	thread_act_t		thr_act, cur_thr_act;

	if (task == TASK_NULL)
		return(KERN_INVALID_ARGUMENT);

	assert(task != kernel_task);
#if 0
	list = &task->thr_acts;
	cur_task = current_task();
	cur_thr_act = current_thread()->top_act;
#endif

	/*
	 *	Deactivate task so that it can't be terminated again,
	 *	and so lengthy operations in progress will abort.
	 *
	 *	If the current thread is in this task, remove it from
	 *	the task's thread list to keep the thread-termination
	 *	loop simple.
	 */
	if (task == cur_task) {
		task_lock(task);
		if (!task->active) {
			/*
			 *	Task is already being terminated.
			 */
			task_unlock(task);
			thread_block();
			return(KERN_FAILURE);
		}

		task_hold_locked(task);
#if 0
		task->active = FALSE;

		/*
		 *	Make sure current thread is not being terminated.
		 */
		mutex_lock(&task->act_list_lock);
		cur_thread = act_lock_thread(cur_thr_act);
		if (!cur_thr_act->active) {
			act_unlock_thread(cur_thr_act);
			mutex_unlock(&task->act_list_lock);
			task_unlock(task);
			thread_terminate(cur_thr_act);
			return(KERN_FAILURE);
		}

		/*
		 * make sure that this thread is the last one in the list
		 */
		queue_remove(list, cur_thr_act, thread_act_t, thr_acts);
		queue_enter(list, cur_thr_act, thread_act_t, thr_acts);
		act_unlock_thread(cur_thr_act);
		mutex_unlock(&task->act_list_lock);
		/*
		 *	Shut down this thread's ipc now because it must
		 *	be left alone to terminate the task.
		 */
		ipc_thr_act_disable(cur_thr_act);
		ipc_thr_act_terminate(cur_thr_act);
#endif
	}
	else {
		/*
		 *	Lock both current and victim task to check for
		 *	potential deadlock.
		 */
		if (task < cur_task) {
			task_lock(task);
			task_lock(cur_task);
		}
		else {
			task_lock(cur_task);
			task_lock(task);
		}
		/*
		 *	Check if current thread_act or task is being terminated.
		 */
		cur_thread = act_lock_thread(cur_thr_act);
		if ((!cur_task->active) || (!cur_thr_act->active)) {
			/*
			 * Current task or thread is being terminated.
			 */
			act_unlock_thread(cur_thr_act);
			task_unlock(task);
			task_unlock(cur_task);
			return(KERN_FAILURE);
		}
		act_unlock_thread(cur_thr_act);
		task_unlock(cur_task);

		if (!task->active) {
			/*
			 *	Task is already being terminated.
			 */
			task_unlock(task);
			thread_block();
			return(KERN_FAILURE);
		}
		task_hold_locked(task);
		task->active = FALSE;
	}

	/*
	 *	Prevent further execution of the task.  ipc_task_disable
	 *	prevents further task operations via the task port.
	 *	If this is the current task, the current thread will
	 *	be left running.
	 */
	ipc_task_disable(task);
	task_wait_locked(task);

	/*
	 *	Terminate each thread in the task.  Depending on the
	 *	state of the thread, this can mean a number of things.
	 *	However, we just call thread_terminate(), which
	 *	takes care of all cases (see that code for details).
	 *
         *      The task_port is closed down, so no more thread_create
         *      operations can be done.  Thread_terminate closes the
         *      thread port for each thread; when that is done, the
         *      thread will eventually disappear.  Thus the loop will
         *      terminate.
	 *	Need to call thread_block() inside loop because some
         *      other thread (e.g., the reaper) may have to run to get rid
         *      of all references to the thread; it won't vanish from
         *      the task's thread list until the last one is gone.
         */
        while (!queue_empty(list)) {
                thr_act = (thread_act_t) queue_first(list);
                act_reference(thr_act);
                task_unlock(task);
                thread_terminate(thr_act);
                act_deallocate(thr_act);
                task_lock(task);
        }
        task_unlock(task);
#endif
	/*
	 *	Destroy all synchronizers owned by the task.
	 */
	task_synchronizer_destroy_all(task);

	/*
	 *	Shut down IPC.
	 */
	ipc_task_terminate(task);

	/*
	 *	Deallocate the task's reference to itself.
	 */
	task_deallocate(task);

	return(KERN_SUCCESS);
}

/*
 *	task_hold_locked:
 *
 *	Suspend execution of the specified task.
 *	This is a recursive-style suspension of the task, a count of
 *	suspends is maintained.
 *
 * 	CONDITIONS: the task is locked.
 */
kern_return_t
task_hold_locked(
	register task_t	task)
{
#if 0
	register queue_head_t	*list;
	register thread_act_t	thr_act, cur_thr_act;

	cur_thr_act = current_act();
#endif	

	if (!task->active) {
		return(KERN_FAILURE);
	}
#if 0

	task->suspend_count++;
	/*
	 *	Iterate through all the thread_act's and hold them.
	 *	Do not hold the current thread_act if it is within the
	 *	task.
	 */
	list = &task->thr_acts;
	thr_act = (thread_act_t) queue_first(list);
	while (!queue_end(list, (queue_entry_t) thr_act)) {
		(void)act_lock_thread(thr_act);
		thread_hold(thr_act);
		act_unlock_thread(thr_act);
		thr_act = (thread_act_t) queue_next(&thr_act->thr_acts);
	}
#endif	
	return(KERN_SUCCESS);
}


kern_return_t
task_release(
	register task_t	task)
{
#if 0
	register queue_head_t	*list;
	register thread_act_t	thr_act, next;

	task_lock(task);
	if (!task->active) {
		task_unlock(task);
		return(KERN_FAILURE);
	}

	task->suspend_count--;

	/*
	 *	Iterate through all the thread_act's and release them.
	 */
	list = &task->thr_acts;
	thr_act = (thread_act_t) queue_first(list);
	while (!queue_end(list, (queue_entry_t) thr_act)) {
		next = (thread_act_t) queue_next(&thr_act->thr_acts);
		(void)act_lock_thread(thr_act);
		thread_release(thr_act);
		act_unlock_thread(thr_act);
		thr_act = next;
	}
	task_unlock(task);
#endif	
	return(KERN_SUCCESS);
}

kern_return_t
task_threads(
	task_t			task,
	thread_act_array_t	*thr_act_list,
	mach_msg_type_number_t	*count)
{
#if 0
	unsigned int		actual;	/* this many thr_acts */
	thread_act_t		thr_act;
	thread_act_t		*thr_acts;
	thread_t		thread;
	int			i, j;
	boolean_t rt = FALSE; /* ### This boolean is FALSE, because there
			       * currently exists no mechanism to determine
			       * whether or not the reply port is an RT port
			       */


	vm_size_t size, size_needed;
	vm_offset_t addr;

	if (task == TASK_NULL)
		return KERN_INVALID_ARGUMENT;

	size = 0; addr = 0;

	for (;;) {
		task_lock(task);
		if (!task->active) {
			task_unlock(task);
			if (size != 0)
				KFREE(addr, size, rt);
			return KERN_FAILURE;
		}

		actual = task->thr_act_count;

		/* do we have the memory we need? */
		size_needed = actual * sizeof(mach_port_t);
		if (size_needed <= size)
			break;

		/* unlock the task and allocate more memory */
		task_unlock(task);

		if (size != 0)
			KFREE(addr, size, rt);

		assert(size_needed > 0);
		size = size_needed;

		addr = KALLOC(size, rt);
		if (addr == 0)
			return KERN_RESOURCE_SHORTAGE;
	}

	/* OK, have memory and the task is locked & active */
	thr_acts = (thread_act_t *) addr;

	for (i = j = 0, thr_act = (thread_act_t) queue_first(&task->thr_acts);
	     i < actual;
	     i++, thr_act = (thread_act_t) queue_next(&thr_act->thr_acts)) {
		act_reference(thr_act);
		thr_acts[j++] = thr_act;
	}
	assert(queue_end(&task->thr_acts, (queue_entry_t) thr_act));
	actual = j;

	/* can unlock task now that we've got the thr_act refs */
	task_unlock(task);

	if (actual == 0) {
		/* no thr_acts, so return null pointer and deallocate memory */

		*thr_act_list = 0;
		*count = 0;

		if (size != 0)
			KFREE(addr, size, rt);
	} else {
		/* if we allocated too much, must copy */

		if (size_needed < size) {
			vm_offset_t newaddr;

			newaddr = KALLOC(size_needed, rt);
			if (newaddr == 0) {
				for (i = 0; i < actual; i++)
					act_deallocate(thr_acts[i]);
				KFREE(addr, size, rt);
				return KERN_RESOURCE_SHORTAGE;
			}

			bcopy((char *) addr, (char *) newaddr, size_needed);
			KFREE(addr, size, rt);
			thr_acts = (thread_act_t *) newaddr;
		}

		*thr_act_list = (mach_port_t *) thr_acts;
		*count = actual;

		/* do the conversion that Mig should handle */

		for (i = 0; i < actual; i++)
			((ipc_port_t *) thr_acts)[i] =
				convert_act_to_port(thr_acts[i]);
	}
#endif
	return KERN_SUCCESS;
}

kern_return_t
task_suspend(
	register task_t		task)
{
#if 0
	if (task == TASK_NULL)
		return (KERN_INVALID_ARGUMENT);

	task_lock(task);
	if (!task->active) {
		task_unlock(task);
		return (KERN_FAILURE);
	}
	if ((task->user_stop_count)++ > 0) {
		/*
		 *	If the stop count was positive, the task is
		 *	already stopped and we can exit.
		 */
		task_unlock(task);
		return (KERN_SUCCESS);
	}

	/*
	 *	Hold all of the threads in the task, and wait for
	 *	them to stop.  If the current thread is within
	 *	this task, hold it separately so that all of the
	 *	other threads can stop first.
	 */
	if (task_hold_locked(task) != KERN_SUCCESS) {
		task_unlock(task);
		return (KERN_FAILURE);
	}

	task_wait_locked(task);
	task_unlock(task);
#endif
	return (KERN_SUCCESS);
}

/*
 * Wait for all threads in task to stop.  Called with task locked.
 */
void
task_wait_locked(
	register task_t		task)
{
	#if 0
	register queue_head_t	*list;
	register thread_act_t	thr_act, refd_thr_act;
	register thread_t	thread, cur_thr;

	cur_thr = current_thread();
	/*
	 *	Iterate through all the thread's and wait for them to
	 *	stop.  Do not wait for the current thread if it is within
	 *	the task.
	 */
	list = &task->thr_acts;
	refd_thr_act = THR_ACT_NULL;
	while (1) {
		thr_act = (thread_act_t) queue_first(list);
		while (!queue_end(list, (queue_entry_t) thr_act)) {
			thread = act_lock_thread(thr_act);
			if (refd_thr_act != THR_ACT_NULL) {
				act_deallocate(refd_thr_act);
				refd_thr_act = THR_ACT_NULL;
			}
			if (thread &&
				thr_act == thread->top_act && thread != cur_thr) {
				refd_thr_act = thr_act;
				act_locked_act_reference(thr_act);
				act_unlock_thread(thr_act);
				task_unlock(task);
				(void)thread_wait(thread);
				task_lock(task);
				thread = act_lock_thread(thr_act);
				if (!thr_act->active) {
					act_unlock_thread(thr_act);
					break;
				}
			}
			act_unlock_thread(thr_act);
			thr_act = (thread_act_t) queue_next(&thr_act->thr_acts);
		}
	    	if (queue_end(list, (queue_entry_t)thr_act))
			break;
	}
	if (refd_thr_act != THR_ACT_NULL) {
		act_deallocate(refd_thr_act);
		refd_thr_act = THR_ACT_NULL;
	}
#endif
}

kern_return_t 
task_resume(register task_t task)
{
	register boolean_t	release;

	if (task == TASK_NULL)
		return(KERN_INVALID_ARGUMENT);

	release = FALSE;
#if 0	
	task_lock(task);
	if (!task->active) {
		task_unlock(task);
		return(KERN_FAILURE);
	}
	if (task->user_stop_count > 0) {
		if (--(task->user_stop_count) == 0)
	    		release = TRUE;
	}
	else {
		task_unlock(task);
		return(KERN_FAILURE);
	}
	task_unlock(task);
#endif
	/*
	 *	Release the task if necessary.
	 */
	if (release)
		return(task_release(task));

	return(KERN_SUCCESS);
}

kern_return_t
task_set_info(
	task_t		task,
	task_flavor_t	flavor,
	task_info_t	task_info_in __unused,		/* pointer to IN array */
	mach_msg_type_number_t	task_info_count __unused)
{

	if (task == TASK_NULL)
		return(KERN_INVALID_ARGUMENT);

	switch (flavor) {
	    default:
			return (KERN_INVALID_ARGUMENT);
	}
	return (KERN_SUCCESS);
}

/*
 *	task_info:
 *
 *	The flavours libc, launchd and ps-style tools use, as in XNU
 *	(xnu-2422), from the task's NextBSD process (nextbsd-kernel#303):
 *	TASK_BASIC_INFO_32, TASK_BASIC2_INFO_32, TASK_BASIC_INFO_64
 *	(TASK_BASIC_INFO on LP64), MACH_TASK_BASIC_INFO,
 *	TASK_THREAD_TIMES_INFO and TASK_AUDIT_TOKEN.
 *	mach_task_get_usage() (mach_task.c) reads the process and refuses a
 *	task whose process is exiting, in exec or gone. The obsolete
 *	TASK_SCHED_* and TASK_SECURITY_TOKEN flavours are unchanged; every
 *	other flavour is KERN_INVALID_ARGUMENT.
 */
kern_return_t
task_info(
	task_t			task,
	task_flavor_t		flavor,
	task_info_t		task_info_out,
	mach_msg_type_number_t	*task_info_count)
{
	struct mach_task_usage	tu;
	kern_return_t		kr;

	if (task == TASK_NULL)
		return(KERN_INVALID_ARGUMENT);

	switch (flavor) {
	    case TASK_BASIC_INFO_32:
	    case TASK_BASIC2_INFO_32:
	    {
		task_basic_info_32_t	basic_info;

		if (*task_info_count < TASK_BASIC_INFO_32_COUNT)
			return (KERN_INVALID_ARGUMENT);
		if ((kr = mach_task_get_usage(task, &tu)) != KERN_SUCCESS)
			return (kr);

		basic_info = (task_basic_info_32_t)task_info_out;
		basic_info->virtual_size = (natural_t)tu.virtual_size;
		/* The "BASIC2" flavour reports the peak resident size. */
		basic_info->resident_size = (natural_t)
		    (flavor == TASK_BASIC2_INFO_32 ?
		    tu.resident_size_max : tu.resident_size);
		basic_info->policy = POLICY_TIMESHARE;
		basic_info->suspend_count = tu.suspend_count;
		basic_info->user_time = tu.user_time;
		basic_info->system_time = tu.system_time;

		*task_info_count = TASK_BASIC_INFO_32_COUNT;
		break;
	    }

	    case TASK_BASIC_INFO_64:
	    {
		task_basic_info_64_t	basic_info;

		if (*task_info_count < TASK_BASIC_INFO_64_COUNT)
			return (KERN_INVALID_ARGUMENT);
		if ((kr = mach_task_get_usage(task, &tu)) != KERN_SUCCESS)
			return (kr);

		basic_info = (task_basic_info_64_t)task_info_out;
		basic_info->virtual_size = tu.virtual_size;
		basic_info->resident_size = tu.resident_size;
		basic_info->policy = POLICY_TIMESHARE;
		basic_info->suspend_count = tu.suspend_count;
		basic_info->user_time = tu.user_time;
		basic_info->system_time = tu.system_time;

		*task_info_count = TASK_BASIC_INFO_64_COUNT;
		break;
	    }

	    case MACH_TASK_BASIC_INFO:
	    {
		mach_task_basic_info_t	basic_info;

		if (*task_info_count < MACH_TASK_BASIC_INFO_COUNT)
			return (KERN_INVALID_ARGUMENT);
		if ((kr = mach_task_get_usage(task, &tu)) != KERN_SUCCESS)
			return (kr);

		basic_info = (mach_task_basic_info_t)task_info_out;
		basic_info->virtual_size = tu.virtual_size;
		basic_info->resident_size = tu.resident_size;
		basic_info->resident_size_max = tu.resident_size_max;
		basic_info->policy = POLICY_TIMESHARE;
		basic_info->suspend_count = tu.suspend_count;
		basic_info->user_time = tu.user_time;
		basic_info->system_time = tu.system_time;

		*task_info_count = MACH_TASK_BASIC_INFO_COUNT;
		break;
	    }

	    case TASK_THREAD_TIMES_INFO:
	    {
		task_thread_times_info_t times_info;

		if (*task_info_count < TASK_THREAD_TIMES_INFO_COUNT)
			return (KERN_INVALID_ARGUMENT);
		if ((kr = mach_task_get_usage(task, &tu)) != KERN_SUCCESS)
			return (kr);

		times_info = (task_thread_times_info_t)task_info_out;
		times_info->user_time = tu.live_user_time;
		times_info->system_time = tu.live_system_time;

		*task_info_count = TASK_THREAD_TIMES_INFO_COUNT;
		break;
	    }

	    case TASK_AUDIT_TOKEN:
	    {
		if (*task_info_count < TASK_AUDIT_TOKEN_COUNT)
			return (KERN_INVALID_ARGUMENT);
		if ((kr = mach_task_get_usage(task, &tu)) != KERN_SUCCESS)
			return (kr);

		*(audit_token_t *)task_info_out = tu.audit_token;

		*task_info_count = TASK_AUDIT_TOKEN_COUNT;
		break;
	    }

	    case TASK_SCHED_FIFO_INFO:
	    {
		register policy_fifo_base_t	fifo_base;

		if (*task_info_count < POLICY_FIFO_BASE_COUNT)
			return(KERN_INVALID_ARGUMENT);

		fifo_base = (policy_fifo_base_t) task_info_out;

		task_lock(task);
		if (task->policy != POLICY_FIFO) {
			task_unlock(task);
			return(KERN_INVALID_POLICY);
		}
		fifo_base->base_priority = task->priority;
		task_unlock(task);

		*task_info_count = POLICY_FIFO_BASE_COUNT;
		break;
	    }

	    case TASK_SCHED_RR_INFO:
	    {
		register policy_rr_base_t	rr_base;

		if (*task_info_count < POLICY_RR_BASE_COUNT)
			return(KERN_INVALID_ARGUMENT);

		rr_base = (policy_rr_base_t) task_info_out;

		task_lock(task);
		if (task->policy != POLICY_RR) {
			task_unlock(task);
			return(KERN_INVALID_POLICY);
		}
		rr_base->base_priority = task->priority;
		rr_base->quantum = (task->sched_data * ticks)/1000;
		task_unlock(task);

		*task_info_count = POLICY_RR_BASE_COUNT;
		break;
	    }

	    case TASK_SCHED_TIMESHARE_INFO:
	    {
		register policy_timeshare_base_t	ts_base;

		if (*task_info_count < POLICY_TIMESHARE_BASE_COUNT)
			return(KERN_INVALID_ARGUMENT);

		ts_base = (policy_timeshare_base_t) task_info_out;

		task_lock(task);
		if (task->policy != POLICY_TIMESHARE) {
			task_unlock(task);
			return(KERN_INVALID_POLICY);
		}
		ts_base->base_priority = task->priority;
		task_unlock(task);

		*task_info_count = POLICY_TIMESHARE_BASE_COUNT;
		break;
	    }

            case TASK_SECURITY_TOKEN:
	    {
                register security_token_t	*sec_token_p;

		if (*task_info_count < TASK_SECURITY_TOKEN_COUNT) {
		    return(KERN_INVALID_ARGUMENT);
		}

		sec_token_p = (security_token_t *) task_info_out;

		task_lock(task);
		*sec_token_p = task->sec_token;
		task_unlock(task);

		*task_info_count = TASK_SECURITY_TOKEN_COUNT;
                break;
            }
            
	    default:
		return (KERN_INVALID_ARGUMENT);
	}

	return(KERN_SUCCESS);
}

/*
 *	task_assign:
 *
 *	Change the assigned processor set for the task
 */
kern_return_t
task_assign(
	task_t				task __unused,
	processor_set_t			new_pset __unused,
	boolean_t			assign_threads __unused)
{

	return (KERN_FAILURE);
}

/*
 *	task_assign_default:
 *
 *	Version of task_assign to assign to default processor set.
 */
kern_return_t
task_assign_default(
	task_t		task,
	boolean_t	assign_threads)
{
    return (task_assign(task, &default_pset, assign_threads));
}

/*
 *	task_get_assignment
 *
 *	Return name of processor set that task is assigned to.
 */
kern_return_t
task_get_assignment(
	task_t		task,
	processor_set_t	*pset)
{
	if (task == TASK_NULL)
		return (KERN_INVALID_ARGUMENT);
	if (!task->active)
		return(KERN_FAILURE);

	*pset = task->processor_set;
	pset_reference(*pset);
	return(KERN_SUCCESS);
}

/*
 * 	task_policy
 *
 *	Set scheduling policy and parameters, both base and limit, for
 *	the given task. Policy must be a policy which is enabled for the
 *	processor set. Change contained threads if requested. 
 */
kern_return_t
task_policy(
	task_t			task,
        policy_t		policy,
        policy_base_t		base,
	mach_msg_type_number_t	count,
        boolean_t		set_limit,
        boolean_t		change)
{

	return (KERN_FAILURE);
}

kern_return_t
task_set_policy(
	task_t			task __unused,
	processor_set_t		pset __unused,
	policy_t		policy __unused,
	policy_base_t		base __unused,
	mach_msg_type_number_t	base_count __unused,
	policy_limit_t		limit __unused,
	mach_msg_type_number_t	limit_count __unused,
	boolean_t		change __unused)
{

	return (KERN_FAILURE);
}


kern_return_t
task_set_ras_pc(
 	task_t		task __unused,
 	vm_offset_t	pc __unused,
 	vm_offset_t	endpc __unused)
{

	return (KERN_FAILURE);
}

void
task_synchronizer_destroy_all(task_t task)
{
	semaphore_t	semaphore;

	/*
	 *  Destroy owned semaphores
	 */

	while (!queue_empty(&task->semaphore_list)) {
		semaphore = (semaphore_t) queue_first(&task->semaphore_list);
		(void) semaphore_destroy(task, semaphore);
	}
}

/*
 * Mach task lifecycle (nextbsd-kernel#266).
 *
 * A task is allocated once per proc *struct*, not once per process. It hangs
 * off p_machdata, an alias of p_emuldata (compat_shim.h), which lies outside
 * the parts of struct proc that fork1() zeroes or copies, and struct proc is
 * type-stable UMA memory. A reused proc struct therefore still carries the
 * previous owner's task. The struct is kept; its per-process state is not:
 *
 *  - mach_task_alloc() creates it the first time a process needs one (the
 *    fork hook, or mach_task_init_lazy() on the first Mach call);
 *  - mach_task_exit() tears it down on process_exit: the task port loses its
 *    kobject, every special-port right is released and the space destroyed.
 *    itk_self == IP_NULL marks the task terminated;
 *  - mach_task_recycle() rebuilds it for the next owner: always at fork,
 *    before anything is inherited, and in mach_task_init_lazy() if a
 *    terminated task is found.
 *
 * A child never gets anything from its parent but itk_bootstrap.
 *
 * Linux-ABI processes are skipped throughout: their p_emuldata is a struct
 * linux_pemuldata, not a task. When the Linux layer takes over a slot that
 * still holds a dead task, that struct is leaked rather than freed (a
 * reference to it may still be in flight) and counted in mach.task_orphaned.
 *
 * mach_task_init() below is the original process_init handler. It was never
 * registered (see task_sysinit) and is compiled out.
 */
static long task_uniqueid;

SYSCTL_DECL(_mach);
static u_long mach_task_orphaned;
SYSCTL_ULONG(_mach, OID_AUTO, task_orphaned, CTLFLAG_RD,
    &mach_task_orphaned, 0,
    "dead task structs leaked when the Linux ABI took over p_emuldata");

/*
 * mach_proc_task — p's Mach task, or TASK_NULL if it has none.
 *
 * p_machdata is #defined to p_emuldata (compat_shim.h), and for a Linux-ABI
 * process the Linuxulator keeps its struct linux_pemuldata there (60 bytes,
 * a malloc(64) allocation). Read as a task, that struct puts itk_space (at
 * offset 48 on LP64) on top of pem->oom_score_adj and pem->so_timestamp
 * (offsets 48 and 52), and itk_lock_data and itk_bootstrap past the end of
 * the allocation. That was the real cause of nextbsd-kernel#63 and #66: the
 * "itk_space == NULL" guards only held while both Linux fields were zero,
 * and writing /proc/self/oom_score_adj or setting SO_TIMESTAMP defeated
 * them. So nothing may dereference another process's (or the exiting
 * process's) p_machdata without checking the ABI first; this does both.
 *
 * Mach traps need no such check: only the FreeBSD sysent reaches them, so
 * current_task() is left alone.
 */
task_t
mach_proc_task(struct proc *p)
{

	if (p->p_machdata == NULL || SV_PROC_ABI(p) != SV_ABI_FREEBSD)
		return (TASK_NULL);
	return ((task_t)p->p_machdata);
}

#ifdef __LP64__
/* The overlay described above; keep it in view if the layout changes. */
CTASSERT(offsetof(struct mach_task, itk_space) == 48);
#endif

/* Serialises the (rare) lazy recycle of a terminated task by a live process. */
static struct sx mach_task_recycle_sx;
SX_SYSINIT(mach_task_recycle, &mach_task_recycle_sx, "mach task recycle");

/*
 * Give a new or just-cleared task the per-process state of p: a new empty
 * space and task port, the TASK_NULL-parent special ports, a new unique id,
 * and p's audit token.
 */
static void
mach_task_setup(task_t task, struct proc *p)
{

	task->itk_p = p;
	queue_init(&task->semaphore_list);
	task_create_internal(task);
	task_init_internal(TASK_NULL, task);
	/* Task #39: task_init_internal(TASK_NULL, ...) leaves
	 * KERNEL_AUDIT_TOKEN, breaking any MIG call whose handler keys on
	 * audit_token.val[5]. Stamp p's PID and creds. */
	set_security_token(task);
	task->itk_uniqueid = atomic_fetchadd_long(&task_uniqueid, 1) + 1;
}

static task_t
mach_task_alloc(struct proc *p, int flags)
{
	task_t task;

	task = uma_zalloc(task_zone, flags | M_ZERO);
	if (task == NULL)
		return (NULL);
	mach_mutex_init(&task->lock, "ETAP_THREAD_TASK_NEW");
	mach_mutex_init(&task->itk_lock_data, "ETAP_THREAD_TASK_ITK");
	mach_task_setup(task, p);
	return (task);
}

/*
 * mach_task_exit — tear down a task's per-process state.
 *
 * Runs on process_exit after the names have been closed (ipc_entry.c), and
 * defensively before any reuse. The struct stays attached to its proc
 * struct. Idempotent: a terminated task (itk_self == IP_NULL) is left alone.
 */
void
mach_task_exit(task_t task)
{

	if (task == TASK_NULL || task->itk_self == IP_NULL)
		return;
	/*
	 * Spawn attributes never applied (an exec that failed, nextbsd#539
	 * part E) go now. A dead task takes no more boosts: a watch port
	 * may still name it (ip_imp_task) until launchd binds the port to
	 * the job's next process.
	 */
	task_importance_drop_spawn_attrs(task);
	task_lock(task);
	task->imp_receiver = 0;
	task->task_imp_assertcnt = 0;
	task->task_imp_externcnt = 0;
	task_unlock(task);
	task_synchronizer_destroy_all(task);
	ipc_task_terminate(task);
}

/*
 * mach_task_recycle — make a task left behind by a previous owner of the
 * proc struct into a fresh task for p.
 */
static void
mach_task_recycle(task_t task, struct proc *p)
{
	ipc_space_t space;

	mach_task_exit(task);

	/* Drop the reference ipc_space_destroy() leaves for the task. */
	space = task->itk_space;
	task->itk_space = IS_NULL;
	if (space != IS_NULL)
		is_release(space);

	/*
	 * Clear what belonged to the previous owner. The two mutexes are left
	 * alone: they were initialised when the struct was allocated and are
	 * not held here.
	 */
	task->ref_count = 0;
	task->active = FALSE;
	task->kernel_loaded = FALSE;
	task->itk_uniqueid = 0;
	task->itk_puniqueid = 0;
	/* exc_actions[] through itk_registered[]: the IPC port slots. */
	bzero(&task->exc_actions[0], offsetof(struct mach_task, itk_p) -
	    offsetof(struct mach_task, exc_actions));
	task->thr_act_count = 0;
	task->res_act_count = 0;
	task->priority = 0;
	task->max_priority = 0;
	task->sched_data = 0;
	task->policy = 0;
	task->processor_set = NULL;
	bzero(&task->sec_token, sizeof(task->sec_token));
	bzero(&task->audit_token, sizeof(task->audit_token));
	task->semaphores_owned = 0;
	task->messages_received = 0;
	/* Importance state (nextbsd#539 part D) belongs to the old owner. */
	task->imp_receiver = 0;
	task->imp_donor = 0;
	task->imp_reserved = 0;
	task->task_imp_assertcnt = 0;
	task->task_imp_externcnt = 0;
	task->task_imp_apptype = TASK_APPTYPE_NONE;
	task->task_imp_role = TASK_UNSPECIFIED;
	task->task_imp_watchports = NULL;	/* released by mach_task_exit */
	task->task_imp_watchport_cnt = 0;
	task->task_imp_spawn_apptype = TASK_APPTYPE_NONE;
	task->task_imp_spawn_pending = FALSE;
	/* p_nice was not touched for p (part F); see task_importance_sched_fork */
	task->task_imp_sched_bg = 0;
	task->task_imp_sched_nice = 0;

	mach_task_setup(task, p);
}

/*
 * The Linux layer is about to overwrite p's p_emuldata: at fork, where the
 * slot can only hold a dead task left by the previous owner of the proc
 * struct, and at a native -> Linux exec, where it holds p's own task.
 * Terminate the task (a no-op if already terminated), detach it and leak it.
 */
void
mach_task_orphan(struct proc *p)
{
	task_t task;

	task = p->p_machdata;
	if (task == TASK_NULL)
		return;
	mach_task_exit(task);
	p->p_machdata = NULL;
	atomic_add_long(&mach_task_orphaned, 1);
}

#if 0
/*
 * Never registered (see task_sysinit); kept for reference only. The live
 * paths are mach_task_alloc/mach_task_recycle above and the fork hook below
 * (nextbsd-kernel#153).
 */
static void
mach_task_init(void *arg __unused, struct proc *p)
{
	task_t task;

	p->p_machdata = task = uma_zalloc(task_zone, M_WAITOK|M_ZERO);
	task->itk_p = p;

	mach_mutex_init(&task->lock, "ETAP_THREAD_TASK_NEW");
	mach_mutex_init(&task->itk_lock_data, "ETAP_THREAD_TASK_ITK");
	queue_init(&task->semaphore_list);

	if (p == &proc0) {
		kernel_task = task;
		task_create_internal(task);
		task_init_internal(TASK_NULL, task);
	} else {
		task_create_internal(task);
	}
}
#endif

/*
 * mach_task_init_lazy — give a process a Mach task on demand.
 *
 * Called by the wired Mach syscall wrappers when p_machdata == NULL —
 * i.e. for any process that pre-existed mach.ko's load (the eventhandler
 * couldn't have run for them) or any process whose Mach state was torn
 * down. If p_machdata is already set it returns 0, after rebuilding the
 * task first if it is terminated (nextbsd-kernel#266): a terminated task
 * carries nobody's space, and must not hand out a stale bootstrap port or
 * audit token.
 *
 * Concurrency: two threads in the same process racing through the
 * wrappers will both reach here with p_machdata == NULL. Both allocate
 * a task; the loser's task gets discarded via the atomic_cmpset_ptr
 * check below. The loser-task leak is small (one struct mach_task) and
 * bounded (at most one per losing race), and the alternative — taking
 * a sleepable lock around uma_zalloc — is heavier.
 *
 * Returns 0 on success, non-zero on allocation failure.
 */
int
mach_task_init_lazy(struct proc *p)
{
	task_t task;

	if (p->p_machdata != NULL) {
		task = mach_proc_task(p);
		if (task != TASK_NULL && task->itk_self == IP_NULL) {
			sx_xlock(&mach_task_recycle_sx);
			if (task->itk_self == IP_NULL)
				mach_task_recycle(task, p);
			sx_xunlock(&mach_task_recycle_sx);
		}
		return (0);
	}

	task = mach_task_alloc(p, M_NOWAIT);
	if (task == NULL)
		return (ENOMEM);

	if (atomic_cmpset_ptr((volatile uintptr_t *)&p->p_machdata,
	    (uintptr_t)NULL, (uintptr_t)task))
		return (0);

	/*
	 * Lost the race — another thread installed a task first. Leak
	 * our task; teardown of an unreachable task_create_internal-style
	 * object is non-trivial and the leak is bounded.
	 */
	return (0);
}

#if 0
/* Never registered either; superseded by mach_task_fork_bsport (#153). */
static void
mach_task_fork(void *arg __unused, struct proc *p1, struct proc *p2, int flags __unused)
{
	task_t task;
	task_t parent_task;

	/*
	 * Out-of-tree fix: we still need to initialize the child task
	 * even when the parent has no Mach state (the parent existed
	 * before mach.ko loaded — common for any process descended from
	 * pre-load init/sshd). Without this initialization, the child's
	 * p_machdata points at a zero-filled task struct (allocated by
	 * mach_task_init's process_init handler) and any subsequent
	 * deref of task->itk_space NULL-faults.
	 *
	 * - If the child has no task (shouldn't happen post-mach.ko, but
	 *   defensive): early-return.
	 * - If parent has no Mach state: initialize child with TASK_NULL
	 *   parent. task_init_internal handles NULL parent_task by
	 *   skipping the inheritance step.
	 * - Otherwise: full parent-derived init.
	 */
	task = p2->p_machdata;
	if (task == NULL)
		return;

	parent_task = p1->p_machdata;
	if (parent_task == NULL) {
		task_init_internal(TASK_NULL, task);
		return;
	}

	atomic_add_long(&task_uniqueid, 1);
	task->itk_uniqueid = task_uniqueid;
	task->itk_puniqueid = parent_task->itk_uniqueid;
	task_init_internal(parent_task, task);
}
#endif

static int
uma_task_init(void *_thread, int a, int b)
{
	/* allocate task substructures */
	return (0);
}

static void
uma_task_fini(void *_thread, int a)
{
	/* deallocate task substructures */
}


/*
 * Task #39 fix: minimal fork-time bootstrap port inheritance.
 *
 * Apple's model: fork() copies parent's task port state (including
 * itk_bootstrap) into child. Our Phase C2 disabled the full
 * task-init / task-fork eventhandlers because they panicked in
 * thread_self_trap when only task (not thread) eventhandlers were
 * wired — full restoration needs both layers safe.
 *
 * But the daemons launchd spawns *must* see launchd's bootstrap_port
 * inherited at fork time. By the time syslogd's lazy mach_task_init
 * runs (via task_self_trap), launchd has already reset its own
 * bootstrap port back to NULL after the fork. So lazy-side "look at
 * parent" inheritance won't work — the snapshot must happen at fork.
 *
 * This handler does the bare minimum: at fork, if the parent has a
 * non-NULL itk_bootstrap, give the child a fresh task with that
 * bootstrap port pre-set. Does NOT call task_init_internal (which is
 * what panicked in 2026-05-11). Threads are still lazy.
 *
 * It also resets any task the child's proc struct still carries from a
 * previous owner, whatever the parent has (nextbsd-kernel#266).
 */
static eventhandler_tag mach_fork_eh_tag;

static void
mach_task_fork_bsport(void *arg __unused, struct proc *p1, struct proc *p2,
    int flags __unused)
{
	task_t parent_task;
	task_t child_task;

	/*
	 * nextbsd-kernel#266: p2 is PRS_NEW, so anything already in its
	 * p_machdata was left there by the previous owner of the proc struct:
	 * that process's space, task port and special ports. Never reuse it.
	 *
	 * A Linux parent's child gets its p_emuldata from linux_proc_init()
	 * after this hook returns; the dead task is orphaned.
	 */
	if (SV_PROC_ABI(p1) != SV_ABI_FREEBSD) {
		mach_task_orphan(p2);
		return;
	}
	/* From here on both p1 and p2 are native (p2 has p1's sysent). */

	/*
	 * Otherwise reset it now, before the parent checks below, so the child
	 * starts with a fresh space and task port even when there is nothing
	 * to inherit.
	 */
	if ((child_task = mach_proc_task(p2)) != TASK_NULL)
		mach_task_recycle(child_task, p2);

	parent_task = mach_proc_task(p1);
	if (mach_debug_enable)
		printf("[T39-fork] p1=%s(%d) -> p2=%s(%d) parent_task=%p "
		    "itk_bootstrap=%p\n",
		    p1->p_comm, p1->p_pid, p2->p_comm, p2->p_pid,
		    parent_task,
		    parent_task ? (void *)parent_task->itk_bootstrap : NULL);

	if (parent_task == NULL)
		return; /* parent has no Mach state — nothing to inherit */

	/*
	 * Out-of-tree fix (nextbsd-kernel#66): p_machdata being set does NOT mean
	 * the task is a real Mach task. A Linux-ABI process acquires a task
	 * lazily but never an itk_space — the same asymmetry #64 fixed in
	 * ipc_entry_list_close, where current_space() is NULL for such a process.
	 * Its itk_* fields are therefore not valid to read OR to lock, and this
	 * hook died two different ways on them:
	 *
	 *   +0xf2  -> ipc_port_copy_send: read 0x490, page not present (stale
	 *             itk_bootstrap port)
	 *   +0x293 -> itk_lock -> __mtx_lock_sleep -> turnstile_wait ->
	 *             propagate_priority: WRITE to kernel text, protection
	 *             violation (itk_lock_data is not an initialised struct mtx,
	 *             so the lock looks contended and the turnstile chain is
	 *             garbage)
	 *
	 * Two distinct crash sites means guarding individual dereferences is the
	 * wrong shape: the whole hook is operating on a structure that is not
	 * valid for this process. Bail out instead.
	 *
	 * Native processes own a space, so bootstrap-port inheritance for
	 * launchd-spawned daemons — the reason this hook exists (Task #39) — is
	 * unaffected. A Linux process has no space and cannot use Mach IPC at
	 * all, so there is genuinely nothing to inherit.
	 *
	 * (The true cause: a Linux process's p_machdata is its linux_pemuldata,
	 * see mach_proc_task(). The ABI check above now catches it first.)
	 */
	if (parent_task->itk_space == NULL)
		return;

	/* nextbsd#539 part F: a demoted parent's child gets its nice back. */
	task_importance_sched_fork(p1, parent_task, p2);

	if (parent_task->itk_bootstrap == IP_NULL) {
		/* No bootstrap port to inherit; importance flags still are. */
		task_importance_inherit(parent_task, mach_proc_task(p2));
		return;
	}

	/* Child should have p_machdata set up the same way mach_task_init_lazy
	 * does on first syscall. We pre-create it here so the bootstrap port
	 * is available immediately. It may already be there, freshly recycled
	 * above. */
	if ((child_task = mach_proc_task(p2)) == TASK_NULL) {
		/* TASK_NULL parent — full inheritance is what tripped the
		 * earlier panic. Just init the empty task, then patch in
		 * the bootstrap port below. */
		child_task = mach_task_alloc(p2, M_NOWAIT);
		if (child_task == NULL)
			return; /* OOM; fall back to no inheritance */
		p2->p_machdata = child_task;
	}
	child_task->itk_puniqueid = parent_task->itk_uniqueid;

	/* nextbsd#539 part E: donor/receiver flags, as xnu-2422 fork. */
	task_importance_inherit(parent_task, child_task);

	/* Task #39: task_init_internal(TASK_NULL, ...) above leaves
	 * audit_token = KERNEL_AUDIT_TOKEN (val[5]=0). Without this,
	 * launchd's job_mig_intran looks up the caller by PID=0, finds
	 * nothing, falls through to job_new_anonymous(jm, 0) which
	 * short-circuits and returns NULL — so job_mig_check_in2 sees
	 * j=NULL and returns BOOTSTRAP_NO_MEMORY. Stamp the real PID
	 * and ucred into the child's audit_token now. */
	set_security_token(child_task);

	itk_lock(parent_task);
	itk_lock(child_task);

	/* Replace whatever the child's itk_bootstrap was (probably IP_NULL
	 * from task_init_internal's TASK_NULL-parent branch) with a send
	 * right to the parent's bootstrap port. */
	if (IP_VALID(child_task->itk_bootstrap))
		ipc_port_release_send(child_task->itk_bootstrap);
	child_task->itk_bootstrap = ipc_port_copy_send(parent_task->itk_bootstrap);

	if (mach_debug_enable)
		printf("[T39-fork] %s(%d) inherited bsport %p -> child_task=%p\n",
		    p2->p_comm, p2->p_pid,
		    (void *)child_task->itk_bootstrap, child_task);

	itk_unlock(child_task);
	itk_unlock(parent_task);
}

static void
task_sysinit(void *arg __unused)
{
	task_zone = uma_zcreate("mach_task_zone",
							sizeof(struct mach_task),
							NULL, NULL, uma_task_init,
							uma_task_fini, 1, 0);

	/*
	 * Phase C2 (still in effect): do NOT re-enable the full
	 * process_init or process_fork eventhandlers — they panicked in
	 * thread_self_trap when only task (not thread) state was prepped
	 * at fork time. Lazy init is the baseline.
	 *
	 * Task #39 (2026-05-18): register a MINIMAL fork eventhandler that
	 * does ONLY bootstrap port inheritance. This is the smallest patch
	 * that lets launchd-spawned daemons see launchd's bootstrap_port
	 * via task_get_special_port at startup — which they need to call
	 * bootstrap_check_in for their MachServices.
	 */
	mach_fork_eh_tag = EVENTHANDLER_REGISTER(process_fork,
	    mach_task_fork_bsport, NULL, EVENTHANDLER_PRI_ANY);
}

/* before SI_SUB_INTRINSIC and after SI_SUB_EVENTHANDLER */
SYSINIT(mach_thread, SI_SUB_KLD, SI_ORDER_ANY, task_sysinit, NULL);

/*
 * Importance donation accounting (nextbsd#539 part D).
 *
 * The task half of xnu-2422's IMPORTANCE_INHERITANCE (kern/task_policy.c):
 * the donor and receiver flags, the app type that sets them, and the
 * internal (kernel-managed) and external (userspace-managed) assertion
 * counts.  The port half is in ipc_port.c; RAISEIMP messages and receive
 * rights in transit move counts between ports and into the task that
 * receives on them.
 *
 * This is accounting only.  xnu-2422 calls task_update_boost_locked() when
 * task_imp_assertcnt leaves or reaches zero, to apply or remove the
 * boost; nothing here does, so a count never changes how a task is
 * scheduled.
 *
 * xnu-2422 panics (or asserts) on an over-release of an internal
 * assertion and on clearing imp_receiver with counts pending.  NextBSD
 * clamps at zero instead, and logs only when mach.debug_enable is set.
 *
 * Every routine takes the task lock.  Callers hold no port lock (the
 * xnu-2422 order: ports are unlocked before a task is touched).
 */

boolean_t
task_is_importance_donor(task_t task)
{

	if (task == TASK_NULL)
		return (FALSE);
	return ((task->imp_donor == 1 || task->task_imp_assertcnt > 0) ?
	    TRUE : FALSE);
}

boolean_t
task_is_importance_receiver(task_t task)
{

	if (task == TASK_NULL)
		return (FALSE);
	return (task->imp_receiver ? TRUE : FALSE);
}

void
task_importance_mark_donor(task_t task, boolean_t donating)
{

	if (task == TASK_NULL)
		return;
	task_lock(task);
	task->imp_donor = (donating ? 1 : 0);
	task_unlock(task);
}

static void
task_importance_mark_receiver_locked(task_t task, boolean_t receiving)
{

	/* Already a receiver: keep the counts it holds. */
	if (receiving && task->imp_receiver != 0)
		return;
	if (!receiving && mach_debug_enable &&
	    (task->task_imp_assertcnt != 0 || task->task_imp_externcnt != 0))
		printf("mach importance: task %p stops receiving with %u "
		    "assertions (%u external) pending; clearing\n", task,
		    task->task_imp_assertcnt, task->task_imp_externcnt);
	task->imp_receiver = (receiving ? 1 : 0);
	task->task_imp_assertcnt = 0;
	task->task_imp_externcnt = 0;
}

void
task_importance_mark_receiver(task_t task, boolean_t receiving)
{

	if (task == TASK_NULL)
		return;
	task_lock(task);
	task_importance_mark_receiver_locked(task, receiving);
	task_unlock(task);
	task_importance_sched_kick(task);
}

/*
 * Record a task's app type and set its donor and receiver flags from it,
 * as xnu-2422's proc_set_task_apptype() does.  An unknown type is ignored
 * (xnu-2422 panics).
 */
void
task_importance_set_apptype(task_t task, int apptype)
{
	int donor, receiver;

	if (task == TASK_NULL)
		return;

	switch (apptype) {
	case TASK_APPTYPE_APP_TAL:
	case TASK_APPTYPE_APP_DEFAULT:
	case TASK_APPTYPE_DAEMON_INTERACTIVE:
		donor = 1;
		receiver = 1;
		break;
	case TASK_APPTYPE_DAEMON_STANDARD:
		donor = 1;
		receiver = 0;
		break;
	case TASK_APPTYPE_DAEMON_ADAPTIVE:
		donor = 0;
		receiver = 1;
		break;
	case TASK_APPTYPE_DAEMON_BACKGROUND:
		donor = 0;
		receiver = 0;
		break;
	default:
		return;
	}

	task_lock(task);
	task->task_imp_apptype = apptype;
	task->imp_donor = donor;
	task_importance_mark_receiver_locked(task, receiver);
	task_unlock(task);
	task_importance_sched_kick(task);
}

/*
 * Add to an assertion count, saturating instead of wrapping.  Nothing in
 * NextBSD's userland gives back an external assertion yet (xnu-2422's
 * libdispatch does, through proc_importance_assertion_complete()), so a
 * receiver's counts only grow while it runs; a wrap to zero would make a
 * busy daemon look unboosted.
 */
static uint32_t
task_importance_count_add(uint32_t cnt, uint32_t count)
{

	return (count > UINT32_MAX - cnt ? UINT32_MAX : cnt + count);
}

static int
task_importance_hold_assertion_locked(task_t target_task, boolean_t external,
    uint32_t count)
{

	if (target_task->imp_receiver == 0)
		return (0);

	if (external) {
		if (target_task->task_imp_externcnt == 0) {
			/*
			 * Only allowed to take a new boost assertion when
			 * holding an external boost.
			 */
			if (mach_debug_enable)
				printf("mach importance: task %p took a boost "
				    "assertion without holding an external one "
				    "(%u total, %u external)\n", target_task,
				    target_task->task_imp_assertcnt,
				    target_task->task_imp_externcnt);
			return (EOVERFLOW);
		}
		target_task->task_imp_assertcnt = task_importance_count_add(
		    target_task->task_imp_assertcnt, count);
		target_task->task_imp_externcnt = task_importance_count_add(
		    target_task->task_imp_externcnt, count);
	} else
		target_task->task_imp_assertcnt = task_importance_count_add(
		    target_task->task_imp_assertcnt, count);

	return (0);
}

static int
task_importance_drop_assertion_locked(task_t target_task, boolean_t external,
    uint32_t count)
{

	if (target_task->imp_receiver == 0)
		return (0);

	if (external) {
		if (count > target_task->task_imp_externcnt) {
			/* The process over-released its boost count. */
			if (mach_debug_enable)
				printf("mach importance: task %p over-released "
				    "external boost assertions (%u total, %u "
				    "external, dropping %u)\n", target_task,
				    target_task->task_imp_assertcnt,
				    target_task->task_imp_externcnt, count);
			return (EOVERFLOW);
		}
		target_task->task_imp_externcnt -= count;
	}

	if (count > target_task->task_imp_assertcnt) {
		if (mach_debug_enable)
			printf("mach importance: over-release of internal "
			    "assertions for task %p (dropping %u, %u held, %u "
			    "external); clamping at zero\n", target_task, count,
			    target_task->task_imp_assertcnt,
			    target_task->task_imp_externcnt);
		count = target_task->task_imp_assertcnt;
	}
	target_task->task_imp_assertcnt -= count;
	if (target_task->task_imp_externcnt > target_task->task_imp_assertcnt)
		target_task->task_imp_externcnt =
		    target_task->task_imp_assertcnt;

	return (0);
}

int
task_importance_hold_internal_assertion(task_t target_task, uint32_t count)
{
	boolean_t was, now;
	int rval;

	if (target_task == TASK_NULL || count == 0)
		return (0);
	task_lock(target_task);
	was = (target_task->task_imp_assertcnt != 0);
	rval = task_importance_hold_assertion_locked(target_task, FALSE, count);
	now = (target_task->task_imp_assertcnt != 0);
	task_unlock(target_task);
	if (was != now)
		task_importance_sched_kick(target_task);
	return (rval);
}

int
task_importance_drop_internal_assertion(task_t target_task, uint32_t count)
{
	boolean_t was, now;
	int rval;

	if (target_task == TASK_NULL || count == 0)
		return (0);
	task_lock(target_task);
	was = (target_task->task_imp_assertcnt != 0);
	rval = task_importance_drop_assertion_locked(target_task, FALSE, count);
	now = (target_task->task_imp_assertcnt != 0);
	task_unlock(target_task);
	if (was != now)
		task_importance_sched_kick(target_task);
	return (rval);
}

int
task_importance_hold_external_assertion(task_t target_task, uint32_t count)
{
	boolean_t was, now;
	int rval;

	if (target_task == TASK_NULL || count == 0)
		return (0);
	task_lock(target_task);
	was = (target_task->task_imp_assertcnt != 0);
	rval = task_importance_hold_assertion_locked(target_task, TRUE, count);
	now = (target_task->task_imp_assertcnt != 0);
	task_unlock(target_task);
	if (was != now)
		task_importance_sched_kick(target_task);
	return (rval);
}

int
task_importance_drop_external_assertion(task_t target_task, uint32_t count)
{
	boolean_t was, now;
	int rval;

	if (target_task == TASK_NULL || count == 0)
		return (0);
	task_lock(target_task);
	was = (target_task->task_imp_assertcnt != 0);
	rval = task_importance_drop_assertion_locked(target_task, TRUE, count);
	now = (target_task->task_imp_assertcnt != 0);
	task_unlock(target_task);
	if (was != now)
		task_importance_sched_kick(target_task);
	return (rval);
}

/*
 * Transfer an internal assertion to userspace responsibility: a RAISEIMP
 * message was delivered, and the receiver now owns its boost until it
 * drops it (proc_importance_assertion_complete() in libsyscall).
 */
int
task_importance_externalize_assertion(task_t target_task, uint32_t count,
    int sender_pid __unused)
{

	if (target_task == TASK_NULL || count == 0)
		return (0);
	task_lock(target_task);
	if (target_task->imp_receiver != 0) {
		target_task->task_imp_externcnt = task_importance_count_add(
		    target_task->task_imp_externcnt, count);
		if (target_task->task_imp_externcnt >
		    target_task->task_imp_assertcnt)
			target_task->task_imp_externcnt =
			    target_task->task_imp_assertcnt;
	}
	task_unlock(target_task);
	return (0);
}

/*
 * Spawn-time importance attributes (nextbsd#539 part E).
 *
 * xnu-2422's posix_spawn takes the new process's type
 * (posix_spawnattr_setprocesstype_np) and its importance watch ports
 * (posix_spawnattr_set_importancewatch_port_np, PSPA_IMP_WATCHPORTS).
 * It copies the watch ports in, loads the image, sets the app type
 * (exec_handle_spawnattr_apptype), and only then binds the ports to the
 * new task (task_add_importance_watchport), because whether a task can
 * receive a boost depends on its app type (bsd/kern/kern_exec.c).
 *
 * launchd's POSIX_SPAWN_SETEXEC spawn is a plain execve(2) on NextBSD, so
 * the child records the same attributes on its own task just before it
 * execs (task_set_importance_watchports_np, mach_traps.c), and the
 * process_exec hook (ipc_entry.c) applies them once the new image is in
 * place.  The attributes are dropped unused at exit, at a later
 * attribute call, and on an exec into a Linux image.
 */

/*
 * A watch port charges the task it is bound to while its receive right
 * waits in launchd.  Bind port to task, as xnu-2422's
 * task_add_importance_watchport(), and return the boosts already queued
 * on it in *boostp.  The port must be tempowner (launchd sets
 * MACH_PORT_TEMPOWNER on every listener port), which also rules out a
 * receive right already in a message.  If the port was bound to another
 * task, that task's share of the boosts is dropped.
 *
 * xnu-2422 holds a task reference for ip_imp_task; NextBSD task structs
 * are type-stable and never freed (task_free), so none is taken, as for
 * the other ip_imp_task users (ipc_port.c).  A binding to a task struct
 * that has since passed to another process is ignored
 * (ipc_port_importance_owner): the boosts on the port then belong to
 * nobody, and the new task takes them all.  That is also the common case
 * of a job respawned into the proc struct its last instance used.
 */
static void
task_add_importance_watchport(task_t task, ipc_port_t port, int *boostp)
{
	task_t owner, release_imp_task = TASK_NULL;
	natural_t boost = 0;

	*boostp = 0;
	if (task->imp_receiver == 0 || !IP_VALID(port))
		return;

	ip_lock(port);
	if (ip_active(port) && port->ip_tempowner != 0 &&
	    port->ip_impdonation != 0) {
		boost = port->ip_impcount;
		owner = ipc_port_importance_owner(port);
		if (owner == task)
			boost = 0;	/* already bound here: nothing moves */
		else
			release_imp_task = owner;

		/* mark the port as watching another task */
		ipc_port_importance_bind(port, task);
	}
	ip_unlock(port);

	if (release_imp_task != TASK_NULL && boost > 0)
		task_importance_drop_internal_assertion(release_imp_task, boost);

	*boostp = (int)boost;
}

static void
task_importance_release_watchports(ipc_port_t *ports, int count)
{
	int i;

	if (ports == NULL)
		return;
	for (i = 0; i < count; i++)
		if (IP_VALID(ports[i]))
			ipc_port_release_send(ports[i]);
	free(ports, M_MACH_TMP);
}

void
task_importance_set_spawn_attrs(task_t task, int apptype, ipc_port_t *ports,
    int count)
{
	ipc_port_t *old_ports;
	int old_count;

	task_lock(task);
	old_ports = task->task_imp_watchports;
	old_count = task->task_imp_watchport_cnt;
	task->task_imp_watchports = ports;
	task->task_imp_watchport_cnt = (ports != NULL) ? count : 0;
	task->task_imp_spawn_apptype = apptype;
	task->task_imp_spawn_pending = TRUE;
	task_unlock(task);

	task_importance_release_watchports(old_ports, old_count);
}

void
task_importance_drop_spawn_attrs(task_t task)
{
	ipc_port_t *ports;
	int count;

	if (task == TASK_NULL)
		return;
	task_lock(task);
	ports = task->task_imp_watchports;
	count = task->task_imp_watchport_cnt;
	task->task_imp_watchports = NULL;
	task->task_imp_watchport_cnt = 0;
	task->task_imp_spawn_apptype = TASK_APPTYPE_NONE;
	task->task_imp_spawn_pending = FALSE;
	task_unlock(task);

	task_importance_release_watchports(ports, count);
}

/*
 * Apply the pending attributes after a successful exec, in xnu-2422's
 * order: the app type first, then the watch ports, and only for an
 * adaptive daemon (xnu-2422 does not even collect the ports for any
 * other type).  The boosts already queued on the ports are held by the
 * task, so that it starts boosted until it handles those messages
 * (xnu-2422 task_hold_multiple_assertion).  That is done whatever the
 * mach.importance sysctl says now: a port only carries a count made while
 * it was 1, and binding has already taken that count off the port's
 * previous owner, so skipping the hold here would leave the count charged
 * to nobody until the messages drain.  Every port's send right is
 * released.
 *
 * Nothing here can fail an exec: a port that does not qualify is
 * skipped, as xnu-2422 skips it.
 */
void
task_importance_apply_spawn_attrs(task_t task)
{
	ipc_port_t *ports;
	int apptype, boost, count, i, needboost;
	boolean_t pending;

	if (task == TASK_NULL)
		return;
	task_lock(task);
	pending = task->task_imp_spawn_pending;
	ports = task->task_imp_watchports;
	count = task->task_imp_watchport_cnt;
	apptype = task->task_imp_spawn_apptype;
	task->task_imp_watchports = NULL;
	task->task_imp_watchport_cnt = 0;
	task->task_imp_spawn_apptype = TASK_APPTYPE_NONE;
	task->task_imp_spawn_pending = FALSE;
	task_unlock(task);

	if (!pending)
		return;

	if (apptype != TASK_APPTYPE_NONE)
		task_importance_set_apptype(task, apptype);

	needboost = 0;
	if (ports != NULL && apptype == TASK_APPTYPE_DAEMON_ADAPTIVE) {
		for (i = 0; i < count; i++) {
			task_add_importance_watchport(task, ports[i], &boost);
			needboost += boost;
		}
	}

	if (needboost > 0)
		task_importance_hold_internal_assertion(task, needboost);

	task_importance_release_watchports(ports, count);
}

/*
 * fork: the child starts with its parent's donor and receiver flags and
 * app type, and no assertions, as xnu-2422's task_create_internal().
 *
 * An adaptive daemon's child is not adaptive (xnu-2422
 * task_policy_create(), nextbsd#539 part F): it becomes an interactive
 * daemon that donates if the parent is boosted, and a background daemon
 * that takes no boosts otherwise.  Only exec gives a task the adaptive
 * type, so only a job launchd started as adaptive is ever demoted by
 * mach.importance_sched.
 */
void
task_importance_inherit(task_t parent_task, task_t child_task)
{
	uint32_t donor, receiver;
	integer_t apptype;
	boolean_t boosted;

	if (parent_task == TASK_NULL || child_task == TASK_NULL ||
	    parent_task == child_task)
		return;
	task_lock(parent_task);
	donor = parent_task->imp_donor;
	receiver = parent_task->imp_receiver;
	apptype = parent_task->task_imp_apptype;
	boosted = (parent_task->task_imp_assertcnt != 0);
	task_unlock(parent_task);

	if (apptype == TASK_APPTYPE_DAEMON_ADAPTIVE) {
		if (boosted) {
			apptype = TASK_APPTYPE_DAEMON_INTERACTIVE;
			donor = 1;
		} else {
			apptype = TASK_APPTYPE_DAEMON_BACKGROUND;
			receiver = 0;
		}
	}

	task_lock(child_task);
	child_task->imp_donor = donor;
	child_task->imp_receiver = receiver;
	child_task->task_imp_assertcnt = 0;
	child_task->task_imp_externcnt = 0;
	child_task->task_imp_apptype = apptype;
	task_unlock(child_task);
}

/*
 * Scheduling effect of importance (nextbsd#539 part F).
 *
 * xnu-2422 runs an adaptive daemon (TASK_APPTYPE_DAEMON_ADAPTIVE) as
 * DARWIN_BG while it holds no importance, and returns it to normal
 * scheduling while it is boosted (kern/task_policy.c,
 * task_policy_update_internal_locked(): "Adaptive daemons are DARWIN_BG
 * unless boosted"; task_update_boost_locked() runs on every 0 <-> nonzero
 * change of task_imp_assertcnt).  This is the minimal port of that: an
 * adaptive daemon whose task_imp_assertcnt (which includes
 * task_imp_externcnt) is 0 runs at nice PRIO_MAX, and gets its own nice
 * back while it holds a boost.  Every other app type, PID 1, system
 * processes and any process with no Mach task keep their priority.
 *
 * Why nice and not the idle class: FreeBSD's idle class (rtp_to_pri with
 * RTP_PRIO_IDLE, as idprio(1) does) only runs when no other thread wants
 * the CPU, and ULE gives an idle-class thread none of the priority boost a
 * timeshare thread gets when it sleeps in the kernel (sched_sleep).
 * Turnstiles lend priority to the owner of a mutex or rwlock, but sx and
 * lockmgr locks (vnode locks among them) lend none.  A daemon such as
 * syslogd holds a vnode lock on /var/log/* while it writes; demoted to
 * idle, it could sit on that lock forever under load while normal
 * processes wait behind it.  Nice PRIO_MAX keeps it timeshare: ULE still
 * gives it a share of the CPU and the in-kernel sleep boost.  It is also
 * closer to DARWIN_BG, which is a low timeshare band (MAXPRI_THROTTLE),
 * not an idle class.
 *
 * Nothing is changed under a port or task lock.  A boost transition only
 * enqueues task_imp_sched_task (task_importance_sched_kick).  The
 * taskqueue handler walks the process list under allproc_lock and
 * applies or undoes the demotion with sched_nice() under each process's
 * PROC_LOCK, which also protects task_imp_sched_bg and
 * task_imp_sched_nice.  The task lock is only taken inside the PROC_LOCK,
 * and only to read the counts; nothing takes a PROC_LOCK under a task
 * lock.  The handler recomputes every demotion from the current state,
 * so transitions that cancel out before it runs cost nothing.
 *
 * A nice the administrator changes while a process is demoted is kept:
 * the old nice is only restored if p_nice is still PRIO_MAX.  A demoted
 * process's fork child inherits p_nice; task_importance_sched_fork gives
 * it back (the child is not adaptive, see task_importance_inherit).
 *
 * mach.importance_sched is 0 by default.  While it is 0 nothing is
 * demoted; setting it back to 0 at runtime restores every demoted
 * process.  It also needs mach.importance: with that at 0 no boost is
 * ever counted, so an adaptive daemon could never come back, and nothing
 * is demoted.
 */
static int mach_importance_sched;

static void task_importance_sched_walk(void *arg, int pending);
static struct task task_imp_sched_task =
    TASK_INITIALIZER(0, task_importance_sched_walk, NULL);

static int
sysctl_mach_importance_sched(SYSCTL_HANDLER_ARGS)
{
	int error, val;

	val = mach_importance_sched;
	error = sysctl_handle_int(oidp, &val, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	mach_importance_sched = (val != 0) ? 1 : 0;
	/* A boot-time tunable needs no walk: no process runs yet. */
	if (!cold)
		taskqueue_enqueue(taskqueue_thread, &task_imp_sched_task);
	return (0);
}
SYSCTL_PROC(_mach, OID_AUTO, importance_sched,
    CTLTYPE_INT | CTLFLAG_RWTUN | CTLFLAG_MPSAFE, NULL, 0,
    sysctl_mach_importance_sched, "I",
    "run unboosted adaptive daemons at nice 20 (xnu DARWIN_BG); "
    "needs mach.importance");

/*
 * Should this task's process be demoted now?  Called with the task lock
 * held (and its process's PROC_LOCK).
 */
static boolean_t
task_importance_sched_wants_bg_locked(task_t task)
{

	if (mach_importance_sched == 0 || mach_importance == 0)
		return (FALSE);
	if (task->task_imp_apptype != TASK_APPTYPE_DAEMON_ADAPTIVE ||
	    task->imp_receiver == 0)
		return (FALSE);
	/* task_imp_assertcnt includes task_imp_externcnt */
	return (task->task_imp_assertcnt == 0 ? TRUE : FALSE);
}

void
task_importance_sched_kick(task_t task)
{

	if (task == TASK_NULL)
		return;
	/*
	 * Unlocked reads: a hint only.  A demotion that is missed here is
	 * picked up by the walk the sysctl change queued, or by the next
	 * transition.
	 */
	if (mach_importance_sched == 0 && task->task_imp_sched_bg == 0)
		return;
	if (task->task_imp_apptype != TASK_APPTYPE_DAEMON_ADAPTIVE &&
	    task->task_imp_sched_bg == 0)
		return;
	taskqueue_enqueue(taskqueue_thread, &task_imp_sched_task);
}

static void
task_importance_sched_walk(void *arg __unused, int pending __unused)
{
	struct proc *p;
	task_t task;
	boolean_t want;

	sx_slock(&allproc_lock);
	FOREACH_PROC_IN_SYSTEM(p) {
		PROC_LOCK(p);
		if (p->p_state != PRS_NORMAL ||
		    (p->p_flag & (P_SYSTEM | P_WEXIT)) != 0 ||
		    p->p_pid <= 1 ||
		    (task = mach_proc_task(p)) == TASK_NULL) {
			PROC_UNLOCK(p);
			continue;
		}
		task_lock(task);
		want = task_importance_sched_wants_bg_locked(task);
		task_unlock(task);

		if (want && task->task_imp_sched_bg == 0) {
			task->task_imp_sched_nice = p->p_nice;
			task->task_imp_sched_bg = 1;
			if (p->p_nice < PRIO_MAX)
				sched_nice(p, PRIO_MAX);
		} else if (!want && task->task_imp_sched_bg != 0) {
			task->task_imp_sched_bg = 0;
			/* Keep a nice someone else set meanwhile. */
			if (p->p_nice == PRIO_MAX &&
			    task->task_imp_sched_nice != PRIO_MAX)
				sched_nice(p, task->task_imp_sched_nice);
		}
		PROC_UNLOCK(p);
	}
	sx_sunlock(&allproc_lock);
}

/*
 * fork hook: p2 inherited p1's p_nice.  If that is a demotion, give p2 the
 * nice p1 had before it; p2 is never adaptive (task_importance_inherit),
 * so it would never be restored otherwise.  p2 is PRS_NEW and not yet
 * runnable; nothing is locked on entry.
 */
void
task_importance_sched_fork(struct proc *p1, task_t parent_task,
    struct proc *p2)
{
	int bg, nice;

	if (parent_task == TASK_NULL)
		return;
	PROC_LOCK(p1);
	bg = parent_task->task_imp_sched_bg;
	nice = parent_task->task_imp_sched_nice;
	PROC_UNLOCK(p1);
	if (bg == 0)
		return;
	PROC_LOCK(p2);
	if (p2->p_nice == PRIO_MAX && nice != PRIO_MAX)
		sched_nice(p2, nice);
	PROC_UNLOCK(p2);
}
