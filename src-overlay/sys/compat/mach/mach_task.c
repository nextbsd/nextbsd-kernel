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
#include <sys/exec.h>
#include <sys/systm.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/mutex.h>
#include <sys/priv.h>
#include <sys/proc.h>
#include <sys/resourcevar.h>
#include <sys/sysproto.h>
#include <sys/uio.h>
#include <sys/ktrace.h>


#include <vm/vm.h>
#include <vm/vm_param.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <vm/vm_extern.h>

#include <sys/mach/mach_types.h>

#include <sys/mach/task.h>
#include <sys/mach/task_server.h>

int
task_get_mach_voucher(
	task_t task,
	mach_voucher_selector_t which,
	ipc_voucher_t *voucher
)
UNSUPPORTED;

int
task_get_state(
	task_t task,
	thread_state_flavor_t flavor,
	thread_state_t old_state,
	mach_msg_type_number_t *old_stateCnt
)
UNSUPPORTED;

/*
 * task_policy_get / task_policy_set (nextbsd-userland#93, group 3).
 *
 * NextBSD has no Mach task-policy engine: there is nothing for a
 * category or QoS tier to drive.  So set validates the flavor and the
 * count the way XNU does and accepts known flavors as a no-op, and get
 * reports the defaults (*get_default = TRUE) with the flavor's count.
 * Unknown flavors are KERN_INVALID_ARGUMENT in both directions.
 *
 * TASK_SUPPRESSION_POLICY is accepted by set, as XNU accepts it, but its
 * layout is not in our headers, so get has no defaults to report for it;
 * TASK_POLICY_STATE is get-only and privileged in XNU.  Both are
 * KERN_INVALID_ARGUMENT from get.
 */
int
task_policy_get(
	task_t task,
	task_policy_flavor_t flavor,
	task_policy_t policy_info,
	mach_msg_type_number_t *policy_infoCnt,
	boolean_t *get_default
)
{
	task_category_policy_t cat;
	task_qos_policy_t qos;

	if (task == NULL || policy_info == NULL ||
	    policy_infoCnt == NULL || get_default == NULL)
		return (KERN_INVALID_ARGUMENT);

	switch (flavor) {
	case TASK_CATEGORY_POLICY:
		if (*policy_infoCnt < TASK_CATEGORY_POLICY_COUNT)
			return (KERN_INVALID_ARGUMENT);
		cat = (task_category_policy_t)policy_info;
		cat->role = TASK_UNSPECIFIED;
		*policy_infoCnt = TASK_CATEGORY_POLICY_COUNT;
		break;

	case TASK_BASE_QOS_POLICY:
	case TASK_OVERRIDE_QOS_POLICY:
	case TASK_BASE_LATENCY_QOS_POLICY:
	case TASK_BASE_THROUGHPUT_QOS_POLICY:
		if (*policy_infoCnt < TASK_QOS_POLICY_COUNT)
			return (KERN_INVALID_ARGUMENT);
		qos = (task_qos_policy_t)policy_info;
		qos->task_latency_qos_tier = LATENCY_QOS_TIER_UNSPECIFIED;
		qos->task_throughput_qos_tier = THROUGHPUT_QOS_TIER_UNSPECIFIED;
		*policy_infoCnt = TASK_QOS_POLICY_COUNT;
		break;

	default:
		return (KERN_INVALID_ARGUMENT);
	}

	*get_default = TRUE;
	return (KERN_SUCCESS);
}

int
task_policy_set(
	task_t task,
	task_policy_flavor_t flavor,
	task_policy_t policy_info,
	mach_msg_type_number_t policy_infoCnt
)
{

	if (task == NULL)
		return (KERN_INVALID_ARGUMENT);

	switch (flavor) {
	case TASK_CATEGORY_POLICY:
		if (policy_infoCnt < TASK_CATEGORY_POLICY_COUNT)
			return (KERN_INVALID_ARGUMENT);
		break;

	case TASK_BASE_QOS_POLICY:
	case TASK_OVERRIDE_QOS_POLICY:
	case TASK_BASE_LATENCY_QOS_POLICY:
	case TASK_BASE_THROUGHPUT_QOS_POLICY:
		if (policy_infoCnt < TASK_QOS_POLICY_COUNT)
			return (KERN_INVALID_ARGUMENT);
		break;

	case TASK_SUPPRESSION_POLICY:
		break;

	default:
		return (KERN_INVALID_ARGUMENT);
	}

	/* Accepted; no task-policy engine to apply it to. */
	(void)policy_info;
	return (KERN_SUCCESS);
}

int
task_purgable_info(
	task_t task,
	task_purgable_info_t *stats
)
UNSUPPORTED;

int
task_resume2(
	task_suspension_token_t suspend_token
)
UNSUPPORTED;

int
task_set_mach_voucher(
	task_t task,
	ipc_voucher_t voucher
	)
UNSUPPORTED;

int
task_set_phys_footprint_limit(
	task_t task,
	int new_limit,
	int *old_limit
)
UNSUPPORTED;

int
task_set_state(
	task_t task,
	thread_state_flavor_t flavor,
	thread_state_t new_state,
	mach_msg_type_number_t new_stateCnt
)
UNSUPPORTED;

int
task_suspend2(
	task_t target_task,
	task_suspension_token_t *suspend_token
)
UNSUPPORTED;

int
task_swap_mach_voucher(
	task_t task,
	ipc_voucher_t new_voucher,
	ipc_voucher_t *old_voucher
)
UNSUPPORTED;

int
task_zone_info(
	task_t target_task,
	mach_zone_name_array_t *names,
	mach_msg_type_number_t *namesCnt,
	task_zone_info_array_t *info,
	mach_msg_type_number_t *infoCnt
)
UNSUPPORTED;

int
thread_create_from_user(
	task_t parent_task,
	thread_act_t *child_act
)
UNSUPPORTED;

int
thread_create_running_from_user(
	task_t parent_task,
	thread_state_flavor_t flavor,
	thread_state_t new_state,
	mach_msg_type_number_t new_stateCnt,
	thread_act_t *child_act
)
UNSUPPORTED;

static void
timeval_to_time_value(const struct timeval *tv, time_value_t *tvp)
{

	tvp->seconds = (integer_t)tv->tv_sec;
	tvp->microseconds = (integer_t)tv->tv_usec;
}

/*
 * mach_task_get_usage — what task_info() reports about task's process
 * (nextbsd-kernel#303).
 *
 * The task struct is attached to its proc struct for good (see the
 * lifecycle comment in kern/task.c), and proc structs are type-stable, so
 * task->itk_p can always be locked; no pid lookup is made, which could
 * find a later process that reused the pid. With the proc locked, the
 * task must still be that process's task, and the process must be fully
 * alive: not being created (PRS_NEW), not exiting (P_WEXIT, set before
 * mach_task_exit() tears the task down) and not in exec (P_INEXEC, while
 * the vmspace and credentials are being replaced). Anything else is
 * KERN_INVALID_ARGUMENT, as XNU answers for an inactive task. Neither
 * flag can be set while the proc lock is held, so p_vmspace and p_ucred
 * are stable until it is dropped, as in fill_kinfo_proc_only().
 *
 * Sizes are what ps(1) reports: the vm_map size and the vmspace resident
 * count. The peak resident size is ru_maxrss when it is larger.
 *
 * Times come from calcru() and rufetchtd() under the proc stat lock, as
 * getrusage(2) does. XNU splits them in two: TASK_BASIC_INFO carries the
 * time of threads that have exited and TASK_THREAD_TIMES_INFO that of the
 * live ones, and callers add the two. The live time is the sum over the
 * live threads; the exited time is the process total less that, floored
 * at zero (calcru() keeps each total monotonic, so the difference can be
 * a tick off).
 *
 * suspend_count is 1 for a stopped process (SIGSTOP or ptrace), as XNU
 * suspends the task for those, and 0 otherwise: NextBSD's task_suspend()
 * does not stop anything yet.
 *
 * The audit token is built with mach_audit_token_from_cred(), the same
 * mapping ipc_kmsg_get() uses for the message trailer, from the process
 * credentials.
 */
kern_return_t
mach_task_get_usage(task_t task, struct mach_task_usage *tu)
{
	struct rusage ru, tru;
	struct timeval ut, st, lut, lst;
	struct vmspace *vm;
	struct thread *td;
	struct proc *p;
	uint64_t maxrss;

	if (task == TASK_NULL || (p = task->itk_p) == NULL)
		return (KERN_INVALID_ARGUMENT);

	PROC_LOCK(p);
	if (mach_proc_task(p) != task || p->p_state != PRS_NORMAL ||
	    (p->p_flag & (P_WEXIT | P_INEXEC)) != 0) {
		PROC_UNLOCK(p);
		return (KERN_INVALID_ARGUMENT);
	}

	PROC_STATLOCK(p);
	rufetch(p, &ru);
	calcru(p, &ut, &st);
	timevalclear(&lut);
	timevalclear(&lst);
	FOREACH_THREAD_IN_PROC(p, td) {
		thread_lock(td);
		rufetchtd(td, &tru);
		thread_unlock(td);
		timevaladd(&lut, &tru.ru_utime);
		timevaladd(&lst, &tru.ru_stime);
	}
	PROC_STATUNLOCK(p);

	if (timevalcmp(&ut, &lut, >))
		timevalsub(&ut, &lut);
	else
		timevalclear(&ut);
	if (timevalcmp(&st, &lst, >))
		timevalsub(&st, &lst);
	else
		timevalclear(&st);
	timeval_to_time_value(&ut, &tu->user_time);
	timeval_to_time_value(&st, &tu->system_time);
	timeval_to_time_value(&lut, &tu->live_user_time);
	timeval_to_time_value(&lst, &tu->live_system_time);

	vm = p->p_vmspace;
	tu->virtual_size = vm->vm_map.size;
	tu->resident_size = ptoa((uint64_t)vmspace_resident_count(vm));
	maxrss = (uint64_t)ru.ru_maxrss * 1024;	/* ru_maxrss is in KB */
	tu->resident_size_max = MAX(tu->resident_size, maxrss);

	tu->suspend_count =
	    (p->p_flag & (P_STOPPED_SIG | P_STOPPED_TRACE)) != 0 ? 1 : 0;

	mach_audit_token_from_cred(p->p_ucred, p->p_pid, &tu->audit_token);
	PROC_UNLOCK(p);

	return (KERN_SUCCESS);
}
