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

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/types.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/eventhandler.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/queue.h>
#include <sys/resource.h>
#include <sys/resourcevar.h>
#include <sys/rwlock.h>
#include <sys/sched.h>
#include <sys/sleepqueue.h>
#include <sys/signal.h>
#include <sys/syslog.h>
#include <sys/sysent.h>

#include <sys/mach/mach_types.h>
#include <sys/mach/mach_traps.h>

#include <sys/mach/ipc/ipc_kmsg.h>
#include <sys/mach/thread.h>
#include <sys/mach/ipc_tt.h>
#include <sys/mach/thread_switch.h>

#define MT_SETRUNNABLE 0x1

#ifdef notyet
/*
 * Am assuming that Mach lacks the concept of uninterruptible
 * sleep - this may need to be changed back to what is in pci_pass.c
 */
static int
_intr_tdsigwakeup(struct thread *td, int intrval)
{
	struct proc *p = td->td_proc;
	int rc = 0;

	PROC_SLOCK(p);
	thread_lock(td);
	if (TD_ON_SLEEPQ(td)) {
		/*
		 * Give low priority threads a better chance to run.
		 */
		if (td->td_priority > PUSER)
			sched_prio(td, PUSER);

		sleepq_abort(td, intrval);
		rc = 1;
	}
	PROC_SUNLOCK(p);
	thread_unlock(td);
	return (rc);
}
#endif


int
mach_thread_switch(mach_port_name_t thread_name, int option, mach_msg_timeout_t option_time)
{
	struct thread *td = curthread;
#ifdef notyet		   
	int timeout;
        struct mach_emuldata *med;

       med = (struct mach_emuldata *)td->td_proc->p_emuldata;
       timeout = option_time * hz / 1000;
#endif

       /*
        * The day we will be able to find out the struct proc from
        * the port number, try to use preempt() to call the right thread.
        * [- but preempt() is for _involuntary_ context switches.]
        */
       switch(option) {
       case SWITCH_OPTION_NONE:
               sched_relinquish(curthread);
               break;

       case SWITCH_OPTION_WAIT:
#ifdef notyet		   
               med->med_thpri = 1;
               while (med->med_thpri != 0) {
                       rw_wlock(&med->med_rightlock);
                       (void)msleep(&med->med_thpri, &med->med_rightlock, PZERO|PCATCH,
                                                "thread_switch", timeout);
                       rw_wunlock(&med->med_rightlock);
              }
               break;
#endif
       case SWITCH_OPTION_DEPRESS:
               /* Use a callout to restore the priority after depression? */
               td->td_priority = PRI_MAX_TIMESHARE;
               break;

       default:
              uprintf("sys_mach_syscall_thread_switch(): unknown option %d\n", option);
               break;
       }
       return (0);
}

void
thread_go(thread_t thread)
{
	int needunlock = 0;
	struct mtx *block_lock = thread->ith_block_lock_data;

	MPASS(thread->ith_state != MACH_SEND_IN_PROGRESS &&
		  thread->ith_state != MACH_RCV_IN_PROGRESS &&
		  thread->ith_state != MACH_RCV_IN_PROGRESS_TIMED);

	LAUNCHD_TRACE("thread_go thread=%p block_lock=%p state=%d",
	    thread, block_lock, thread->ith_state);

	if (block_lock != NULL && !mtx_owned(block_lock)) {
		needunlock = 1;
		mtx_lock(block_lock);
	}
	wakeup(thread);
	if (needunlock)
		mtx_unlock(block_lock);

	LAUNCHD_TRACE("thread_go thread=%p wakeup posted", thread);
}

void
thread_block(void)
{
	thread_t thread = current_thread();
	int rc;

	MPASS(curthread == thread->ith_td);

	rc = msleep(thread, thread->ith_block_lock_data, PCATCH|PSOCK, "thread_block", thread->timeout);
	switch (rc) {
	case EINTR:
	case ERESTART:
		thread->wait_result = THREAD_INTERRUPTED;
		break;
	case EWOULDBLOCK:
		thread->wait_result = THREAD_TIMED_OUT;
		break;
	case 0:
		thread->wait_result = THREAD_AWAKENED;
		break;
	default:
		panic("unexpected return from msleep: %d\n", rc);
	}
#ifdef INVARIANTS
	if (thread->timeout == 0) {
		if(rc == 0)
			MPASS(thread->ith_state == MACH_MSG_SUCCESS);
		else
			MPASS(rc == EINTR || rc == ERESTART);
	}
#endif
}

void
thread_will_wait_with_timeout(thread_t thread, int timeout)
{

	thread->sleep_stamp = ticks;
	thread->timeout = timeout;
}


void
thread_will_wait(thread_t thread)
{

	thread->sleep_stamp = ticks;
	thread->timeout = 0;
}

static void
mach_thread_create(struct thread *td, thread_t thread)
{

	thread->ref_count = 1;
	ipc_thread_init(thread);
}


static uma_zone_t thread_shuttle_zone;

static int
uma_thread_init(void *_thread, int a, int b)
{
	/* allocate thread substructures */
	return (0);
}

static void
uma_thread_fini(void *_thread, int a)
{
	/* deallocate thread substructures */
}

static void
mach_thread_init(void *arg __unused, struct thread *td)
{
	thread_t thread;

	thread = uma_zalloc(thread_shuttle_zone, M_WAITOK|M_ZERO);
	mtx_init(&thread->ith_lock_data, "mach_thread lock", NULL, MTX_DEF);

	MPASS(td->td_machdata == NULL);
	td->td_machdata = thread;
	thread->ith_td = td;
	ipc_thr_act_init(thread);
}

/*
 * True when td_machdata may hold a Mach thread: td's process is native ABI,
 * or a kernel process (whose threads may send Mach messages from the kernel,
 * e.g. iokit_kextd_send() from a taskqueue, and which the Linuxulator never
 * touches). Otherwise the slot is the Linuxulator's.
 */
static __inline boolean_t
mach_td_native(struct thread *td)
{
	struct proc *p;

	p = td->td_proc;
	return (SV_PROC_ABI(p) == SV_ABI_FREEBSD ||
	    (p->p_flag & P_SYSTEM) != 0);
}

/*
 * mach_td_thread — td's Mach thread, or THREAD_NULL if it has none.
 *
 * td_machdata is #defined to td_emuldata (compat_shim.h), and for every
 * thread of a Linux-ABI process the Linuxulator keeps its 32-byte struct
 * linux_emuldata there (linux_proc_init(), on fork, clone and exec). Read as
 * a struct thread_shuttle, every ith_* field past the first few is out of
 * bounds of that allocation. So nothing may read a thread's td_machdata
 * without checking the ABI first; this does both. It is the thread analog
 * of mach_proc_task() (nextbsd-kernel#153, #269).
 *
 * td's process must be live: the caller is td itself, or holds td's process
 * still (the exec hook). The thread_dtor handler below cannot use this.
 */
thread_t
mach_td_thread(struct thread *td)
{

	if (td->td_machdata == NULL || !mach_td_native(td))
		return (THREAD_NULL);
	return ((thread_t)td->td_machdata);
}

/*
 * Free a thread's Mach state and clear its slot. thread must be td's.
 *
 * ipc_thr_act_terminate() takes the shuttle off its thread kport first, so
 * any send right to the port still held elsewhere is a dead name from then
 * on, and releases the shuttle's own send rights. Idempotent in the sense
 * that a shuttle already terminated (ith_self == IP_NULL) is only freed.
 */
static void
mach_thread_teardown(struct thread *td, thread_t thread)
{

	td->td_machdata = NULL;
	ipc_thr_act_terminate(thread);
	mtx_destroy(&thread->ith_lock_data);
	uma_zfree(thread_shuttle_zone, thread);
}

/*
 * mach_thread_exit — tear down td's Mach thread state, if it has any.
 *
 * For the exec hook (ipc_entry.c): on a native -> Linux exec every thread's
 * Mach state goes before linux_on_exec() takes the slot (nextbsd-kernel#269).
 * It loops because, for td == curthread, the port teardown could reach
 * ipc_kmsg_destroy(), which would give the still-native thread a fresh
 * shuttle on demand; that one must not be left for the Linuxulator either.
 */
void
mach_thread_exit(struct thread *td)
{
	thread_t thread;

	while ((thread = mach_td_thread(td)) != THREAD_NULL)
		mach_thread_teardown(td, thread);
}

static int mach_thread_stale_logged;

/*
 * mach_thread_init_lazy — give a thread Mach state on demand.
 *
 * Called by the wired thread-self / host-self syscall wrappers when
 * current_thread() would return NULL (because the thread eventhandlers
 * never ran — either because the kmod loaded after this thread was
 * created, or because the handlers are intentionally not registered).
 *
 * Combines the work of both mach_thread_init (allocate + ipc_thr_act_init)
 * and mach_thread_create (ref_count + ipc_thread_init) so the resulting
 * td_machdata is in the same shape as if both eventhandlers had fired.
 * Idempotent; concurrent callers race on atomic_cmpset_ptr with the
 * loser leaking a bounded amount.
 *
 * A thread of a Linux-ABI process is refused with ENOMEM, and nothing is
 * allocated into its slot: that is the Linuxulator's (see mach_td_thread).
 *
 * A shuttle left in the slot that is not td's (ith_td != td) or whose
 * thread kport is gone (ith_self == IP_NULL) is stale: the struct thread
 * was recycled with Mach state still attached. mach_thread_dtor() should
 * make that impossible, so it is logged once. The stale shuttle is
 * detached and leaked (something may still point at it), after its thread
 * kport is terminated if the shuttle is td's own, and td gets a fresh one.
 */
int
mach_thread_init_lazy(struct thread *td)
{
	thread_t thread;

	if (!mach_td_native(td))
		return (ENOMEM);

	thread = mach_td_thread(td);
	if (thread != THREAD_NULL) {
		if (thread->ith_td == td && thread->ith_self != IP_NULL)
			return (0);
		if (!mach_thread_stale_logged) {
			mach_thread_stale_logged = 1;
			log(LOG_WARNING, "mach: tid %d: stale Mach thread %p "
			    "in a recycled thread; replacing it\n",
			    td->td_tid, thread);
		}
		if (thread->ith_td == td)
			ipc_thr_act_terminate(thread);
		td->td_machdata = NULL;
	}

	thread = uma_zalloc(thread_shuttle_zone, M_NOWAIT | M_ZERO);
	if (thread == NULL)
		return (ENOMEM);
	mtx_init(&thread->ith_lock_data, "mach_thread lock", NULL, MTX_DEF);
	thread->ith_td = td;
	ipc_thr_act_init(thread);
	thread->ref_count = 1;
	ipc_thread_init(thread);

	if (atomic_cmpset_ptr((volatile uintptr_t *)&td->td_machdata,
	    (uintptr_t)NULL, (uintptr_t)thread))
		return (0);

	/* Lost the race; leak this thread (see mach_task_init_lazy comment). */
	return (0);
}

static void
mach_thread_fini(void *arg __unused, struct thread *td)
{
	thread_t thread;

	/*
	 * Out-of-tree fix: this thread_fini handler fires for every
	 * thread in the kernel, including threads that existed before
	 * mach.ko loaded (so mach_thread_init never ran on them) and
	 * threads of processes that never picked up Mach state.
	 * mach_td_thread() is THREAD_NULL for those, and for Linux-ABI
	 * threads, whose slot (td_emuldata, compat_shim.h) holds a
	 * linux_emuldata. Without this guard the very next thread to
	 * exit page-faults inside this handler, panicking the kernel.
	 */
	thread = mach_td_thread(td);
	if (thread == THREAD_NULL)
		return;

	MPASS(thread->ith_kmsg == NULL);
	MPASS(thread->ith_td == td);
	mach_thread_teardown(td, thread);
}

/*
 * thread_dtor handler: free a dead thread's Mach state (nextbsd-kernel#269).
 *
 * Hook point. thread_dtor runs once per dead thread, after it is off the
 * CPU and before its struct thread goes back to the type-stable UMA zone:
 * from thread_free() and the zombie reaper (thread_reap_domain()) for every
 * thread but a process's last, and from proc_dtor() for the last one, which
 * stays with the proc struct. So no IPC can still be running on the thread,
 * and every recycled struct thread starts with no Mach state. It never runs
 * on the dying thread itself. thread_fini (UMA release) would be too late:
 * type-stable items are rarely released, and a reused struct would carry
 * the old shuttle and thread port. process_exit covers only the last
 * thread. The PhaseB-bisect registrations below stay disabled.
 *
 * Why the slot is read directly, not through mach_td_thread(): for a
 * zombie, td_proc may already have been reaped and reused, by a Linux
 * process even (thread_stash() drops p_exitthreads before the zombie is
 * freed), so its ABI says nothing about td. The slot needs no ABI check
 * here anyway: the Linuxulator frees and NULLs its td_emuldata on every
 * thread exit (linux_thread_dtor() as sv_ontdexit, from kern_thr_exit()
 * and exit1()), so a dead thread's non-NULL slot can only be ours.
 *
 * Threads that never had Mach state, the common case, return at once.
 */
static void
mach_thread_dtor(void *arg __unused, struct thread *td)
{
	thread_t thread;

	thread = td->td_machdata;
	if (thread == NULL)
		return;
	mach_thread_teardown(td, thread);
}

static void
mach_thread_ctor(void *arg __unused, struct thread *td)
{
	thread_t thread;

	/* Same NULL-guard rationale as mach_thread_fini above. */
	thread = mach_td_thread(td);
	if (thread == THREAD_NULL)
		return;

	MPASS(thread->ith_td == td);
	mach_thread_create(td, thread);
	thread->ith_block_lock_data = NULL;
}

static void
thread_sysinit(void *arg __unused)
{
	thread_shuttle_zone = uma_zcreate("thread_shuttle_zone",
									  sizeof(struct thread_shuttle),
									  NULL, NULL, uma_thread_init,
									  uma_thread_fini, 1, 0);

	#if 0 /*PhaseB-bisect*/
	EVENTHANDLER_REGISTER(thread_ctor, mach_thread_ctor, NULL, EVENTHANDLER_PRI_ANY);
#endif
	#if 0 /*PhaseB-bisect*/
	EVENTHANDLER_REGISTER(thread_init, mach_thread_init, NULL, EVENTHANDLER_PRI_ANY);
#endif
	#if 0 /*PhaseB-bisect*/
	EVENTHANDLER_REGISTER(thread_fini, mach_thread_fini, NULL, EVENTHANDLER_PRI_ANY);
#endif
	/* See mach_thread_dtor for why thread_dtor (nextbsd-kernel#269). */
	EVENTHANDLER_REGISTER(thread_dtor, mach_thread_dtor, NULL, EVENTHANDLER_PRI_ANY);
}

/* before SI_SUB_INTRINSIC and after SI_SUB_EVENTHANDLER */
SYSINIT(mach_thread, SI_SUB_KLD, SI_ORDER_ANY, thread_sysinit, NULL);
