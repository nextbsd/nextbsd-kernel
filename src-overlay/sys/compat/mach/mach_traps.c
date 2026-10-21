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

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/namei.h>
#include <sys/dirent.h>
#include <sys/eventhandler.h>
#include <sys/proc.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/filedesc.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/resourcevar.h>
#include <sys/socket.h>
#include <sys/vnode.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <sys/unistd.h>
#include <sys/times.h>
#include <sys/sem.h>
#include <sys/msg.h>
#include <sys/ptrace.h>
#include <sys/signalvar.h>

#include <netinet/in.h>
#include <sys/sysproto.h>

#include <vm/vm.h>
#include <vm/vm_param.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <vm/vm_extern.h>

#include <sys/mach/mach_types.h>

#include <sys/mach/ipc/ipc_types.h>
#include <sys/mach/ipc/ipc_kmsg.h>
#include <sys/mach/ipc/mach_msg.h>
#include <sys/mach/thread.h>
#include <sys/mach/mach_port_server.h>
#include <sys/mach/mach_vm_server.h>
#include <sys/mach/task_server.h>
#include <sys/mach/thread_switch.h>

#include <sys/mach/mach_init.h>
#include <sys/mach/ipc_kobject.h>	/* IKOT_TASK */

#include <sys/mach/mach_vm.h>
#include <sys/mach/mach.h>
#include <sys/mach/host.h>		/* realhost, host_lock */
#include <sys/mach/host_special_ports.h>	/* HOST_BOOTSTRAP_PORT */


#pragma clang diagnostic ignored "-Wunused-parameter"

int
sys_clock_sleep_trap(struct thread *td __unused, struct clock_sleep_trap_args *uap)
{

	td->td_retval[0] = clock_sleep(uap->clock_name, uap->sleep_type, uap->sleep_sec, uap->sleep_nsec, uap->wakeup_time);
	return (0);
}

int
sys_mach_timebase_info(struct thread *td __unused, struct mach_timebase_info_args *uap)
{

	return (mach_timebase_info(uap->info));
}

int
sys_mach_msg_overwrite_trap(struct thread *td __unused, struct mach_msg_overwrite_trap_args *uap)
{
	td->td_retval[0] = mach_msg_overwrite_trap(
		uap->msg, uap->option, uap->send_size, uap->rcv_size,
		uap->rcv_name, uap->timeout, uap->notify, uap->rcv_msg,
		uap->scatter_list_size);
	return (0);
}

int
sys_mach_msg_trap(struct thread *td __unused, struct mach_msg_trap_args *uap)
{
	struct mach_msg_overwrite_trap_args uap0;

	bcopy(uap, &uap0, sizeof(*uap));
	uap0.rcv_msg = NULL;
	return (sys_mach_msg_overwrite_trap(td, &uap0));
}
	
int
sys_semaphore_wait_trap(struct thread *td, struct semaphore_wait_trap_args *uap)
	UNSUPPORTED;

int
sys_semaphore_signal_trap(struct thread *td, struct semaphore_signal_trap_args *uap)
	UNSUPPORTED;
int
sys_semaphore_wait_signal_trap(struct thread *td, struct semaphore_wait_signal_trap_args *uap)
	UNSUPPORTED;

int
sys_semaphore_signal_thread_trap(struct thread *td, struct semaphore_signal_thread_trap_args *uap)
	UNSUPPORTED;

int
sys_semaphore_signal_all_trap(struct thread *td, struct semaphore_signal_all_trap_args *uap)
	UNSUPPORTED;

/*
 * task_for_pid (nextbsd-kernel#298): put a send right to pid's task port in
 * the caller's IPC space, as XNU's task_for_pid() in bsd/kern/kern_proc.c.
 *
 * Privilege is the ptrace(2) rule, with no taskgated: pget() runs
 * p_cansee() and p_candebug() on the locked proc, as kern_ptrace() does for
 * PT_ATTACH, and system processes are refused as there. So root passes
 * unless a jail, securelevel (for init) or the MAC policy says otherwise;
 * anyone else needs security.bsd.unprivileged_proc_debug, a target whose
 * credentials are a subset of its own, and a target that is not set-id.
 *
 * The send right is made before the proc is unlocked. p_candebug() refuses
 * a target in exec, so a set-id exec can only start afterwards, and that
 * exec resets the task port (ipc_task_reset, #267): this right then names
 * a dead kobject, not the new image.
 *
 * Errors are XNU's: KERN_FAILURE for pid 0, a target_tport that is not the
 * caller's own task, no such process, a denied caller, or a target with no
 * live Mach task. *t is always written, MACH_PORT_NULL on failure.
 */
int
sys_task_for_pid(struct thread *td, struct task_for_pid_args *uap)
{
	task_t self = current_task();
	ipc_port_t port = IP_NULL, sright = IP_NULL;
	mach_port_name_t tret = MACH_PORT_NAME_NULL;
	kern_return_t kr = KERN_FAILURE;
	struct proc *p;
	task_t task;

	/* pid 0 is the kernel; XNU refuses it whoever asks. */
	if (uap->pid == 0)
		goto out;

	/* target_tport must be a send right to the caller's own task. */
	if (ipc_port_translate_send(self->itk_space, uap->target_tport,
	    &port) != KERN_SUCCESS)
		goto out;
	task = (ip_active(port) && ip_kotype(port) == IKOT_TASK) ?
	    (task_t)port->ip_kobject : TASK_NULL;
	ip_unlock(port);
	if (task != self)
		goto out;

	if (pget(uap->pid, PGET_CANSEE | PGET_CANDEBUG | PGET_NOTWEXIT |
	    PGET_NOTID, &p) != 0)
		goto out;
	if ((p->p_flag & P_SYSTEM) == 0 &&
	    (task = mach_proc_task(p)) != TASK_NULL) {
		task_reference(task);
		sright = convert_task_to_port(task);	/* consumes the ref */
	}
	PROC_UNLOCK(p);
	if (!IP_VALID(sright))
		goto out;

	/* Consumes sright, also on failure. */
	tret = ipc_port_copyout_send(sright, self->itk_space);
	if (MACH_PORT_NAME_VALID(tret))
		kr = KERN_SUCCESS;
	else
		tret = MACH_PORT_NAME_NULL;
out:
	if (copyout(&tret, uap->t, sizeof(tret)) != 0 && kr == KERN_SUCCESS) {
		mach_port_deallocate(self->itk_space, tret);
		kr = KERN_INVALID_ADDRESS;
	}
	td->td_retval[0] = kr;
	return (0);
}

int
sys_thread_switch(struct thread *td __unused, struct thread_switch_args *uap)
{

	return (mach_thread_switch(uap->thread_name, uap->option, uap->option_time));
}	

static int
_swtch_pri(struct thread *td)
{
	thread_lock(td);
	if (td->td_state == TDS_RUNNING)
		td->td_proc->p_stats->p_cru.ru_nivcsw++;        /* XXXSMP */
	mi_switch(SW_VOL);
	thread_unlock(td);
	return (0);
}

int
sys_swtch_pri(struct thread *td, struct swtch_pri_args *uap __unused)
{
	
	return (_swtch_pri(td));
}

int
sys_swtch(struct thread *td, struct swtch_args *v __unused)
{

	return (_swtch_pri(td));
}

int
sys_mach_reply_port(struct thread *td, struct mach_reply_port_args *uap __unused)
{

	td->td_retval[0] = mach_reply_port();
	return (0);
}

int
sys_thread_self_trap(struct thread *td, struct thread_self_trap_args *uap)
{

	td->td_retval[0] = mach_thread_self();
	return (0);
}

int
sys_task_self_trap(struct thread *td, struct task_self_trap_args *uap)
{

	td->td_retval[0] = mach_task_self();
	return (0);
}

int
sys_host_self_trap(struct thread *td, struct host_self_trap_args *uap)
{

	td->td_retval[0] = mach_host_self();
	return (0);
}

int
sys__kernelrpc_mach_port_allocate_trap(struct thread *td __unused, struct _kernelrpc_mach_port_allocate_trap_args *uap)
{
	ipc_space_t space = current_task()->itk_space;
	mach_port_name_t name;
	int error;

	if ((error = mach_port_allocate(space, uap->right, &name)) != 0)
		return (error);
	
	return (copyout(&name, uap->name, sizeof(*uap->name)));
}

int
sys__kernelrpc_mach_port_deallocate_trap(struct thread *td, struct _kernelrpc_mach_port_deallocate_trap_args *uap)
{
	ipc_space_t space = current_task()->itk_space;

	mach_port_deallocate(space, uap->name);
	return (0);
}

int
sys__kernelrpc_mach_port_insert_right_trap(struct thread *td, struct _kernelrpc_mach_port_insert_right_trap_args *uap)
{
	task_t task = current_task(); /* port_name_to_task(uap->target); */
	ipc_port_t port;
	mach_msg_type_name_t disp;
	int rv = MACH_SEND_INVALID_DEST;

	if (task != current_task())
		goto done;
	if (uap->name != uap->poly)
		goto done;
	rv = ipc_object_copyin(task->itk_space, uap->poly, uap->polyPoly, (ipc_object_t *)&port);
	if (rv != KERN_SUCCESS)
		goto done;
	disp = ipc_object_copyin_type(uap->polyPoly);
	rv = mach_port_insert_right(task->itk_space, uap->name, port, disp);
done:
	/* current_task() takes no reference, so none is dropped (#300). */
	td->td_retval[0] = rv;
	return (0);
}

/*
 * nextbsd#347/#353: arm a Mach notification (dead-name / no-senders /
 * port-destroyed) so libdispatch's Mach backend learns when a watched peer
 * dies. Thin wrapper over the existing mach_port_request_notification() in
 * ipc/mach_port.c. libmach passes 6 args (no target — see the 6-arg ABI note in
 * libmach and mach_syscall_wire.c); we operate on current_task()'s space like
 * every trap here.
 *
 * PANIC-CRITICAL (launchd calls this at PID-1 init inside os_assumes_zero):
 *  - `previous` is written by the callee ONLY on KERN_SUCCESS; init it IP_NULL
 *    and only translate/copyout on success.
 *  - the callee returns `previous` as an ipc_port_t with a ref (pd/nsrequest);
 *    ipc_object_copyout(SEND_ONCE) consumes exactly that ref. Guard on
 *    IP_VALID — the common DEAD_NAME path returns IP_NULL, for which we copyout
 *    MACH_PORT_NULL and do NOT call copyout on the port.
 */
int
sys__kernelrpc_mach_port_request_notification_trap(struct thread *td,
    struct _kernelrpc_mach_port_request_notification_trap_args *uap)
{
	ipc_space_t space = current_task()->itk_space;
	ipc_port_t notify_port = IP_NULL, previous = IP_NULL;
	mach_port_name_t prev_name = MACH_PORT_NAME_NULL;
	kern_return_t kr;

	/* Resolve the notify port name -> ipc_port_t with the caller's disposition
	 * (typically MACH_MSG_TYPE_MAKE_SEND_ONCE). A null notify name is allowed. */
	if (uap->notify != 0) {
		kr = ipc_object_copyin(space, uap->notify, uap->notifyPoly,
		    (ipc_object_t *)&notify_port);
		if (kr != KERN_SUCCESS) {
			td->td_retval[0] = kr;
			return (0);
		}
	}

	kr = mach_port_request_notification(space, uap->name, uap->msgid,
	    uap->sync, notify_port, &previous);

	if (kr == KERN_SUCCESS && IP_VALID(previous)) {
		kern_return_t ckr = ipc_object_copyout(space,
		    (ipc_object_t)previous, MACH_MSG_TYPE_PORT_SEND_ONCE, &prev_name);
		if (ckr != KERN_SUCCESS) {
			ipc_port_release_send(previous);
			prev_name = (ckr == KERN_INVALID_CAPABILITY) ?
			    MACH_PORT_NAME_DEAD : MACH_PORT_NAME_NULL;
		}
	}

	if (uap->previous != NULL)
		(void)copyout(&prev_name, uap->previous, sizeof(prev_name));
	td->td_retval[0] = kr;
	return (0);
}

int
sys__kernelrpc_mach_port_mod_refs_trap(struct thread *td, struct _kernelrpc_mach_port_mod_refs_trap_args *uap)
{
	ipc_space_t space = current_task()->itk_space;
	/*
	  mach_port_name_t target = uap->target;
	  mach_port_name_t *name = uap->name;
	  mach_port_right_t right = uap->right;
	  mach_port_delta_t delta = uap->delta;
	*/
	td->td_retval[0] = mach_port_mod_refs(space, uap->name, uap->right, uap->delta);
	return (0);
}

int
sys__kernelrpc_mach_port_move_member_trap(struct thread *td, struct _kernelrpc_mach_port_move_member_trap_args *uap)
{
	ipc_space_t space = current_task()->itk_space;

	td->td_retval[0] = mach_port_move_member(space, uap->member, uap->after);
	return (0);
}

int
sys__kernelrpc_mach_port_insert_member_trap(struct thread *td, struct _kernelrpc_mach_port_insert_member_trap_args *uap)
{
	ipc_space_t space = current_task()->itk_space;

	td->td_retval[0] = mach_port_move_member(space, uap->name, uap->pset);
	return (0);
}

int
sys__kernelrpc_mach_port_extract_member_trap(struct thread *td, struct _kernelrpc_mach_port_extract_member_trap_args *uap)
{
	ipc_space_t space = current_task()->itk_space;

	td->td_retval[0] = mach_port_move_member(space, uap->name, MACH_PORT_NAME_NULL);
	return (0);
}

int
sys__kernelrpc_mach_port_construct_trap(struct thread *td, struct _kernelrpc_mach_port_construct_trap_args *uap)
	UNSUPPORTED;

int
sys__kernelrpc_mach_port_destruct_trap(struct thread *td, struct _kernelrpc_mach_port_destruct_trap_args *uap)
	UNSUPPORTED;

int
sys__kernelrpc_mach_port_destroy_trap(struct thread *td, struct _kernelrpc_mach_port_destroy_trap_args *uap)
{
	ipc_space_t space = current_task()->itk_space;

	return (mach_port_destroy(space, uap->name));
}

int
sys__kernelrpc_mach_port_guard_trap(struct thread *td, struct _kernelrpc_mach_port_guard_trap_args *uap)
	UNSUPPORTED;


int
sys__kernelrpc_mach_port_unguard_trap(struct thread *td, struct _kernelrpc_mach_port_unguard_trap_args *uap)
	UNSUPPORTED;

int
sys__kernelrpc_mach_vm_map_trap(struct thread *td, struct _kernelrpc_mach_vm_map_trap_args *uap)
{
	int error;
	vm_offset_t addr;

	if ((error = copyin(uap->address, &addr, sizeof(addr))) != 0)
		return (error);
	error = mach_vm_map(&curthread->td_proc->p_vmspace->vm_map, &addr, uap->size, uap->mask, uap->flags, NULL, 0, 0, uap->cur_protection,
						VM_PROT_ALL, VM_INHERIT_NONE);
	if (error)
		return (error);
	return (copyout(&addr, uap->address, sizeof(addr)));
}

int
sys__kernelrpc_mach_vm_allocate_trap(struct thread *td, struct _kernelrpc_mach_vm_allocate_trap_args *uap)
{
	/* mach_port_name_t target = uap->target; current task only */
	mach_vm_offset_t *address = uap->address;
	mach_vm_offset_t uaddr;
	mach_vm_size_t size = uap->size;
	int flags = uap->flags;
	int error;

	if ((error = copyin(address, &uaddr, sizeof(mach_vm_offset_t))))
		return (error);

	if ((error = mach_vm_allocate(&td->td_proc->p_vmspace->vm_map,
								  &uaddr, size, flags)))
		return (error);
	if ((error = copyout(&uaddr, address, sizeof(mach_vm_offset_t))))
		return (error);
	return (0);
}

int
sys__kernelrpc_mach_vm_deallocate_trap(struct thread *td, struct _kernelrpc_mach_vm_deallocate_trap_args *uap)
{
	/* mach_port_name_t target = uap->target; current task only */

	return (mach_vm_deallocate(&td->td_proc->p_vmspace->vm_map, uap->address, uap->size));
}

int
sys__kernelrpc_mach_vm_protect_trap(struct thread *td, struct _kernelrpc_mach_vm_protect_trap_args *uap)
{
	/* mach_port_name_t target = uap->target */
	/* int set_maximum = uap->set_maximum */

	return (mach_vm_protect(&td->td_proc->p_vmspace->vm_map, uap->address, uap->size, FALSE, uap->new_protection));
}

