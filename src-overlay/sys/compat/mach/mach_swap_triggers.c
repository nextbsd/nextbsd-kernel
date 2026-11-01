/*
 * mach_swap_triggers.c — macx_triggers() and default_pager_space_alert
 * (nextbsd-kernel#218).
 *
 * Darwin's dynamic_pager learns that backing store is running low (or has
 * recovered) without polling: it registers a receive right with the
 * macx_triggers() Mach trap, and the default pager sends that port a
 * default_pager_space_alert when free backing store crosses a threshold
 * (xnu-2422: bsd/vm/dp_backing_file.c macx_triggers(), osfmk/vm/bsd_vm.c
 * mach_macx_triggers(), osfmk/default_pager/dp_backing_store.c
 * default_pager_triggers() and the HI_WAT_ALERT / LO_WAT_ALERT sends).
 * NextBSD's swapd (nextbsd/nextbsd-userland#178) is built on the same
 * interface.
 *
 * NextBSD's thresholds are the swap pager's almost-full hysteresis, and the
 * crossing is the swap_lowspace eventhandler (patches/0080):
 *
 *   HI_WAT_ALERT  free swap fell below vm.nswap_lowat  (swap almost full;
 *                 grow it). The hi_water argument, in bytes, sets that
 *                 threshold, as it sets minimum_pages_remaining in xnu.
 *   LO_WAT_ALERT  free swap rose back above vm.nswap_hiwat (swap may be
 *                 shrunk). The low_water argument, in bytes, sets that
 *                 threshold, as it sets maximum_pages_free in xnu.
 *
 * A zero hi_water or low_water leaves that threshold alone. Both are swap
 * pager state, so a macx_triggers() also changes the vm.nswap_* sysctls.
 *
 * As in xnu each registration is one-shot: the port is consumed by the
 * alert it receives and the caller re-arms with another macx_triggers().
 * A new registration for a flag replaces (and releases) the previous port.
 * A HI_WAT_ALERT registration made while swap is already almost full
 * (including when there is no swap at all) is alerted at once, as xnu's
 * re-armed trigger fires on the next allocation below the threshold.
 *
 * The eventhandler runs with the swap device mutex held, so it only records
 * the transition and queues a task; the task, on taskqueue_thread, sends the
 * message (the same kernel-originated send as host_notify_calendar_change()
 * in mach_host.c and iokit_kextd_send()).
 *
 * Only HI_WAT_ALERT and LO_WAT_ALERT carry a port. SWAP_FILE_CREATION_ERROR
 * is accepted and its port released, as in xnu; NextBSD has no emergency
 * swap segment, so it has nothing more to do. The encryption, compaction,
 * PROC_RESUME and emergency-swap-file flags belong to xnu's default pager
 * and are accepted as no-ops; contradictory pairs are EINVAL, as in xnu.
 *
 * Returns are xnu's: the value itself (0, EINVAL, EPERM), not -1/errno,
 * since macx_triggers is a Mach trap there.
 */

#include <sys/cdefs.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/priv.h>
#include <sys/eventhandler.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/taskqueue.h>
#include <machine/atomic.h>

#include <vm/vm.h>
#include <vm/vm_param.h>

#include <sys/mach/mach_types.h>
#include <sys/mach/default_pager_types.h>
#include <sys/mach/ipc/ipc_port.h>
#include <sys/mach/ipc/ipc_space.h>
#include <sys/mach/ipc/ipc_object.h>
#include <sys/mach/ipc/ipc_kmsg.h>
#include <sys/mach/ipc/ipc_mqueue.h>
#include <sys/mach/thread.h>		/* current_task() */

/* From <vm/swap_pager.h> (patches/0080); declared here to keep that
 * header's swap-device types out of the Mach namespace. */
extern bool swap_pager_almost_full;
int swap_pager_set_watermarks(int lowat, int hiwat);

static struct mtx dp_trigger_mtx;
MTX_SYSINIT(dp_trigger, &dp_trigger_mtx, "mach swap triggers", MTX_DEF);

static ipc_port_t dp_hi_port = IP_NULL;	/* HI_WAT_ALERT registration */
static ipc_port_t dp_lo_port = IP_NULL;	/* LO_WAT_ALERT registration */
static volatile u_int dp_pending;		/* *_WAT_ALERT bits to deliver */
static struct task dp_alert_task;
static eventhandler_tag dp_lowspace_tag;

/* Send one default_pager_space_alert to port, consuming its send right. */
static void
dp_space_alert_send(ipc_port_t port, int flags)
{
	ipc_kmsg_t kmsg;
	default_pager_space_alert_msg_t *m;

	if (!ip_active(port)) {
		ipc_port_release_send(port);
		return;
	}
	kmsg = ikm_alloc(sizeof *m);
	if (kmsg == IKM_NULL) {
		ipc_port_release_send(port);
		return;
	}
	ikm_init(kmsg, sizeof *m);
	m = (default_pager_space_alert_msg_t *)kmsg->ikm_header;

	m->Head.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_PORT_SEND, 0);
	m->Head.msgh_local_port = MACH_PORT_NULL;
	m->Head.msgh_remote_port = (mach_port_t)port;	/* consumed by send */
	m->Head.msgh_voucher_port = 0;
	m->Head.msgh_id = DEFAULT_PAGER_SPACE_ALERT_MSGID;
	m->Head.msgh_size = (mach_msg_size_t)
	    (sizeof(*m) - sizeof(mach_msg_format_0_trailer_t));
	m->NDR = NDR_record;
	m->flags = flags;

	/* A sane minimum trailer, as iokit_kextd.c / ipc_notify.c do. */
	m->trailer.msgh_seqno = 0;
	m->trailer.msgh_sender = KERNEL_SECURITY_TOKEN;
	m->trailer.msgh_trailer_type = MACH_MSG_TRAILER_FORMAT_0;
	m->trailer.msgh_trailer_size = MACH_MSG_TRAILER_MINIMUM_SIZE;

	ipc_mqueue_send_always(kmsg);
}

/* Take the one-shot registration for flag (HI_ or LO_WAT_ALERT) and alert it. */
static void
dp_alert_fire(int flag)
{
	ipc_port_t port;

	mtx_lock(&dp_trigger_mtx);
	if (flag == HI_WAT_ALERT) {
		port = dp_hi_port;
		dp_hi_port = IP_NULL;
	} else {
		port = dp_lo_port;
		dp_lo_port = IP_NULL;
	}
	mtx_unlock(&dp_trigger_mtx);

	if (IP_VALID(port))
		dp_space_alert_send(port, flag);
}

static void
dp_alert_task_fn(void *arg __unused, int count __unused)
{
	u_int pending;

	pending = atomic_readandclear_int(&dp_pending);
	/* Both edges queued: deliver the one matching the current state last. */
	if (swap_pager_almost_full) {
		if (pending & LO_WAT_ALERT)
			dp_alert_fire(LO_WAT_ALERT);
		if (pending & HI_WAT_ALERT)
			dp_alert_fire(HI_WAT_ALERT);
	} else {
		if (pending & HI_WAT_ALERT)
			dp_alert_fire(HI_WAT_ALERT);
		if (pending & LO_WAT_ALERT)
			dp_alert_fire(LO_WAT_ALERT);
	}
}

static void
dp_queue_alert(u_int flag)
{

	atomic_set_int(&dp_pending, flag);
	taskqueue_enqueue(taskqueue_thread, &dp_alert_task);
}

/* swap_lowspace handler: swap device mutex held, so never sleeps. */
static void
dp_swap_lowspace(void *arg __unused, int almost_full, int avail __unused)
{

	dp_queue_alert(almost_full ? HI_WAT_ALERT : LO_WAT_ALERT);
}

/*
 * Make a send right from the caller's receive right `name`. xnu's
 * trigger_name_to_port() likewise requires the receive right.
 */
static int
dp_copyin(mach_port_name_t name, ipc_port_t *portp)
{
	task_t task = current_task();
	ipc_port_t port = IP_NULL;
	kern_return_t kr;

	if (task == TASK_NULL || !MACH_PORT_NAME_VALID(name))
		return (EINVAL);
	kr = ipc_object_copyin(task->itk_space, name, MACH_MSG_TYPE_MAKE_SEND,
	    (ipc_object_t *)&port);
	if (kr != KERN_SUCCESS || !IP_VALID(port))
		return (EINVAL);
	*portp = port;
	return (0);
}

/* Store `name` as the one-shot registration for flag, releasing any earlier one. */
static int
dp_register(mach_port_name_t name, int flag)
{
	ipc_port_t port, old;
	int error;

	if ((error = dp_copyin(name, &port)) != 0)
		return (error);

	mtx_lock(&dp_trigger_mtx);
	if (flag == HI_WAT_ALERT) {
		old = dp_hi_port;
		dp_hi_port = port;
	} else {
		old = dp_lo_port;
		dp_lo_port = port;
	}
	mtx_unlock(&dp_trigger_mtx);

	if (IP_VALID(old))
		ipc_port_release_send(old);
	return (0);
}

static int
dp_macx_triggers(struct thread *td, int hi_water, int low_water, int flags,
    mach_port_name_t alert_port)
{
	int error, lowat, hiwat;

	error = priv_check(td, PRIV_SWAPON);
	if (error != 0)
		return (error);

	if (((flags & SWAP_ENCRYPT_ON) && (flags & SWAP_ENCRYPT_OFF)) ||
	    ((flags & SWAP_COMPACT_ENABLE) && (flags & SWAP_COMPACT_DISABLE)))
		return (EINVAL);

	if (flags & SWAP_FILE_CREATION_ERROR) {
		ipc_port_t port;

		/* xnu checks the port, then releases it unused. */
		if ((error = dp_copyin(alert_port, &port)) != 0)
			return (error);
		ipc_port_release_send(port);
	}

	if ((flags & (HI_WAT_ALERT | LO_WAT_ALERT)) == 0)
		return (0);

	/*
	 * Byte thresholds, as xnu's hi_wat / lo_wat (dynamic_pager passes both
	 * on every call); 0 keeps the current one. hi_water must not exceed
	 * low_water once both are applied.
	 */
	lowat = hi_water > 0 ? (int)(hi_water / PAGE_SIZE) : -1;
	hiwat = low_water > 0 ? (int)(low_water / PAGE_SIZE) : -1;
	if ((lowat >= 0 || hiwat >= 0) &&
	    swap_pager_set_watermarks(lowat, hiwat) != 0)
		return (EINVAL);

	if (flags & HI_WAT_ALERT) {
		if ((error = dp_register(alert_port, HI_WAT_ALERT)) != 0)
			return (error);
		if (swap_pager_almost_full)
			dp_queue_alert(HI_WAT_ALERT);
	}
	if (flags & LO_WAT_ALERT) {
		if ((error = dp_register(alert_port, LO_WAT_ALERT)) != 0)
			return (error);
	}
	return (0);
}

/* Wired as mach.syscall.macx_triggers by mach_syscall_wire.c. */
int
sys_macx_triggers(struct thread *td, struct macx_triggers_args *uap)
{

	td->td_retval[0] = dp_macx_triggers(td, uap->hi_water, uap->low_water,
	    uap->flags, uap->alert_port);
	return (0);
}

static void
dp_triggers_sysinit(void *arg __unused)
{

	TASK_INIT(&dp_alert_task, 0, dp_alert_task_fn, NULL);
	dp_lowspace_tag = EVENTHANDLER_REGISTER(swap_lowspace,
	    dp_swap_lowspace, NULL, EVENTHANDLER_PRI_ANY);
}
/* After SI_SUB_EVENTHANDLER; same slot as mach_host.c's time_set handler. */
SYSINIT(dp_triggers, SI_SUB_KLD, SI_ORDER_ANY, dp_triggers_sysinit, NULL);

static void
dp_triggers_sysuninit(void *arg __unused)
{
	ipc_port_t hi, lo;

	EVENTHANDLER_DEREGISTER(swap_lowspace, dp_lowspace_tag);
	taskqueue_drain(taskqueue_thread, &dp_alert_task);
	mtx_lock(&dp_trigger_mtx);
	hi = dp_hi_port;
	lo = dp_lo_port;
	dp_hi_port = dp_lo_port = IP_NULL;
	mtx_unlock(&dp_trigger_mtx);
	if (IP_VALID(hi))
		ipc_port_release_send(hi);
	if (IP_VALID(lo))
		ipc_port_release_send(lo);
}
SYSUNINIT(dp_triggers, SI_SUB_KLD, SI_ORDER_ANY, dp_triggers_sysuninit, NULL);
