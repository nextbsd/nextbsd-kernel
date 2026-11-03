/*
 * NextBSD in-kernel IOPMrootDomain (nextbsd-kernel#243, E17 K4).
 *
 * As on xnu, the power-management root domain lives in the kernel. It
 * publishes the IOPMrootDomain entry Apple's code looks for, sends the
 * system sleep/wake messages to every app that registered with
 * IORegisterForSystemPower, waits for their acknowledgements, and then
 * sleeps. pmconfigd is a client of it and owns policy (idle sleep,
 * assertions, clamshell rules), not the app fan-out. The userland ABI, the
 * message sequence and the property keys are in sys/sys/iopmroot.h.
 *
 * Entry. A virtual registry entry (#333) named and classed IOPMrootDomain,
 * published under IOResources from a SYSINIT at SI_SUB_CONFIGURE, after
 * acpi0 and its lid and buttons have attached. Its properties are kept
 * current with ioreg_update_virtual() (#241), which also sends
 * kIOMessageServicePropertyChange to its general-interest clients.
 *
 * App fan-out. IORegisterForSystemPower registers an IOREGIOCINTEREST watch
 * of type IOREG_INTEREST_APP_POWER on the entry. For each phase the root
 * domain takes the list of those watches (ioreg_interest_clients()) and
 * messages each one on its own (ioreg_message_client()), with its
 * acknowledgement id, Darwin's notificationID, in argument[0]: the phase's
 * serial number in the upper bits and the client's 1-based index in the low
 * 16. IOPMROOT_ALLOW / IOPMROOT_CANCEL on /dev/iopmroot answer with that id.
 * A phase that needs answers waits for all of them, kern.pmtimeout seconds
 * at most (30 by default, xnu's budget); a client still silent then is
 * logged and the sleep goes ahead. A client whose port has died counts as
 * answered.
 *
 * Sleep. Every S1-S4 request goes through acpi_ReqSleepState(): this file's
 * own (IOPMROOT_SLEEP, clamshell, power button), and the kernel's
 * (hw.acpi.sleep_state, acpiconf -s, zzz, the legacy lid and button
 * actions). The root domain is ACPI's one sleep listener (K1, patch 0090),
 * so each request reaches iopm_acpi_listener() as ACPI_SLEEP_REQUEST and
 * waits for it. The worker thread then sends kIOMessageSystemWillSleep,
 * collects the answers and acknowledges with acpi_sleep_listener_ack(0),
 * which suspends in the worker's own thread and returns after the wake;
 * the worker sends kIOMessageSystemWillPowerOn and
 * kIOMessageSystemHasPoweredOn. An idle sleep (IOPMROOT_REASON_IDLE) first
 * asks kIOMessageCanSystemSleep, before the ACPI request, so the ACPI force
 * timeout (hw.acpi.suspend_ack_timeout, 45 s) covers only the WillSleep
 * phase; one veto ends it with kIOMessageSystemWillNotSleep. So does an ACPI
 * request that is called off (acpiconf -k 1, an apm client) or a suspend
 * that fails before it starts. If ACPI forces the sleep before the answers
 * are in, the worker follows it; a sleep the root domain never heard
 * requested still gets the two wake messages.
 *
 * Policy. Lid and button presses arrive through the acpi_lid_event and
 * acpi_button_event eventhandlers (K3, patches 0088/0089); each lid change
 * updates AppleClamshellState / AppleClamshellCausesSleep and sends
 * kIOPMMessageClamshellStateChange to general-interest clients. Who acts on
 * them depends on whether a process has set clamshell or power-button
 * policy (see "Policy owner" in sys/sys/iopmroot.h). With none, the
 * kernel's hw.acpi.lid_switch_state / power_button_state /
 * sleep_button_state actions apply as before. The first descriptor that sets
 * it (pmconfigd) owns it: those sysctls go to NONE and the root domain
 * itself requests "Clamshell Sleep" and "Power Button Sleep" as xnu does,
 * until that descriptor is closed and the sysctls come back. acpi_acad_event
 * keeps the AC state for IOPMROOT_GETSTATE; power sources are K7's.
 *
 * Wake. "Wake Reason" is "PWRB (User)" if acpi_woke_by_power_button(),
 * "RTC (Alarm)" if the wake alarm armed for this sleep went off, or
 * "EC.LidOpen (User)" if the lid opened during the sleep, else "" (looked at
 * once more two seconds later, since a control-method button's wake notify
 * may arrive late). kern.sleeptime / kern.waketime are set from the
 * power_suspend_early / power_resume eventhandlers, xnu's semantics.
 *
 * Wake alarms (nextbsd-kernel#245). As on xnu, pmconfigd hands scheduled
 * wake and power-on times to the root domain as PM settings:
 * "WakeByCalendarDate" / "wake" and "PowerByCalendarDate" / "poweron"
 * through IOPMROOT_SETPROP (what IOKitUser's tellClockController() writes),
 * and the maintenance and SleepService wakes through IOPMROOT_SETMAINTWAKE
 * (kPMSetMaintenanceWakeCalendar). On xnu they go on to the platform RTC
 * driver (AppleRTC); here that is atrtc's CMOS alarm. The earliest wake
 * time still ahead is armed from power_suspend_early, as the system goes to
 * sleep, and disarmed again from power_resume; the wake times are good for
 * one sleep, since pmconfigd sets the next one at each WillSleep. The
 * earliest power-on time is armed from shutdown_final on a power-off and
 * stays until it is cleared.
 *
 * Context. The ACPI listener and the eventhandlers run in ACPI's task
 * thread, in the thread that requested sleep, or during DEVICE_RESUME with
 * processes stopped. They only record state under iopm_mtx and wake the
 * worker thread, which does everything that sleeps: registry updates, Mach
 * sends, waiting for answers, and the suspend itself. iopm_mtx is never held
 * across any of those. Lock order: ACPI's locks, then iopm_mtx; registry
 * calls are made with iopm_mtx dropped.
 *
 * Without DEV_ACPI, or on arm64 (where acpi_ReqSleepState() cannot sleep),
 * the entry is published with IOSleepSupported false and IOPMROOT_SLEEP
 * returns EOPNOTSUPP. Nothing here prints on a normal boot.
 */

#include "opt_acpi.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/ctype.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/conf.h>
#include <sys/proc.h>
#include <sys/priv.h>
#include <sys/kthread.h>
#include <sys/eventhandler.h>
#include <sys/sysctl.h>
#include <sys/syslog.h>
#include <sys/time.h>
#include <sys/clock.h>
#include <sys/uuid.h>
#include <sys/bus.h>
#include <sys/nv.h>
#include <sys/ioregistry.h>
#include <sys/iopmroot.h>

#ifdef DEV_ACPI
#include <contrib/dev/acpica/include/acpi.h>
#include <dev/acpica/acpivar.h>
#endif

#if defined(DEV_ACPI) && (defined(__amd64__) || defined(__i386__))
#define	IOPM_CAN_SLEEP	1	/* acpi_ReqSleepState() can suspend */
#include <sys/reboot.h>
#include <isa/rtc.h>		/* atrtc_wake_alarm_*() */
#endif

static MALLOC_DEFINE(M_IOPMROOT, "iopmroot", "IOPMrootDomain");

#define	IOPM_BUDGET_DEFAULT	30	/* s, xnu's app answer budget */
#define	IOPM_BUDGET_MAX		300
#define	IOPM_CLIENTS_MAX	0xffff	/* low 16 bits of a notificationID */
#define	IOPM_EVENT_WINDOW	(5 * SBT_1S) /* a lid/button event names the
					   sleep it caused within this */
#define	IOPM_WAKE_GRACE		(3 * SBT_1S) /* power button after a wake */
#define	IOPM_WAKE_RECHECK	(2 * SBT_1S) /* second look at the wake reason */
#define	IOPM_SLEEP_WAIT		(180 * SBT_1S) /* bound on an acked suspend */
#define	IOPM_ACK_GRACE		(2 * SBT_1S)	/* after a failed ack */

#define	IOPM_NOTIFICATION_ID(txn, i)	(((txn) << 16) | ((uint64_t)(i) + 1))

/* Darwin values the entry publishes. */
#define	IOPM_SLEEP_TYPE_NORMAL	2	/* kIOPMSleepTypeNormalSleep */
#define	IOPM_CAPABILITIES_FULL	0xf	/* kIOPMSystemCapabilityCPU|Graphics|
					   Audio|Network */
#define	IOPM_WAKE_REASON_PWRB	"PWRB (User)"
#define	IOPM_WAKE_REASON_LID	"EC.LidOpen (User)"
#define	IOPM_WAKE_REASON_RTC	"RTC (Alarm)"
#define	IOPM_WAKE_TYPE_USER	"User"

/*
 * Wake alarms: the PM settings xnu's root domain passes to the platform RTC
 * driver (IOPM.h / IOPMPrivate.h kIOPMSetting*Key), one slot each. A
 * calendar key carries an IOPMCalendarStruct (struct iopmroot_calendar,
 * UTC), a seconds key the seconds from now; a zero year or 0 seconds clears.
 */
#define	IOPM_ALARM_WAKE		0	/* "WakeByCalendarDate" */
#define	IOPM_ALARM_WAKE_SECS	1	/* "wake" */
#define	IOPM_ALARM_MAINT	2	/* kPMSetMaintenanceWakeCalendar, 1 */
#define	IOPM_ALARM_SLEEPSVC	3	/* kPMSetMaintenanceWakeCalendar, 2 */
#define	IOPM_ALARM_POWERON	4	/* "PowerByCalendarDate" */
#define	IOPM_ALARM_POWERON_SECS	5	/* "poweron" */
#define	IOPM_ALARM_COUNT	6

static const struct {
	const char	*key;		/* IOPMROOT_SETPROP key, NULL if none */
	bool		calendar;	/* else seconds from now */
	const char	*wake_type;	/* "Wake Type" when it wakes us */
} iopm_alarms[IOPM_ALARM_COUNT] = {
	[IOPM_ALARM_WAKE] =	{ "WakeByCalendarDate", true, "Alarm" },
	[IOPM_ALARM_WAKE_SECS] = { "wake", false, "Alarm" },
	[IOPM_ALARM_MAINT] =	{ NULL, true, "Maintenance" },
	[IOPM_ALARM_SLEEPSVC] =	{ NULL, true, "SleepService" },
	[IOPM_ALARM_POWERON] =	{ "PowerByCalendarDate", true, NULL },
	[IOPM_ALARM_POWERON_SECS] = { "poweron", false, NULL },
};

/* Worker work bits (iopm.work). */
#define	IOPM_W_SLEEP		0x0001	/* IOPMROOT_SLEEP: request sleep */
#define	IOPM_W_ACPI_REQ		0x0002	/* ACPI_SLEEP_REQUEST is waiting */
#define	IOPM_W_RESUME		0x0004	/* a suspend/resume cycle ended */
#define	IOPM_W_LID		0x0008	/* acpi_lid_event */
#define	IOPM_W_BUTTON		0x0010	/* acpi_button_event */
#define	IOPM_W_POLICY		0x0020	/* clamshell/button policy changed */
#define	IOPM_W_RECHECK		0x0040	/* second look at the wake reason */

/* iopm_wait_answers() outcomes. */
#define	IOPM_WAIT_DONE		0	/* every client answered */
#define	IOPM_WAIT_TIMEOUT	1	/* budget spent */
#define	IOPM_WAIT_VETO		2	/* CanSystemSleep: a client cancelled */
#define	IOPM_WAIT_ABORT		3	/* revert, ACPI cancel, forced sleep */
#define	IOPM_WAIT_PREEMPT	4	/* CanSystemSleep: an ACPI request */

#define	IOPM_BUTTON_POWER	0x1
#define	IOPM_BUTTON_SLEEP	0x2

/* One app power client in the current phase. */
struct iopm_client {
	struct ioreg_interest_client ic;
	bool		answered;
};

/* One open /dev/iopmroot. */
struct iopm_fd {
	pid_t		pid;
};

/*
 * The root domain's state. Everything below is guarded by iopm_mtx, except
 * that `clients` is replaced only by the worker thread, which therefore
 * reads its immutable `ic` fields without the lock.
 */
static struct {
	u_int		work;		/* IOPM_W_* */
	sbintime_t	recheck;	/* wake reason second look; 0 = none */

	/* The phase under way and its clients. */
	int		phase;		/* IOPMROOT_PHASE_* */
	uint64_t	txn;		/* phase serial, notificationID >> 16 */
	struct iopm_client *clients;
	u_int		nclients;
	u_int		pending;	/* answers still awaited */
	bool		vetoed;		/* CanSystemSleep cancelled */
	bool		revert;		/* kPMRequestIdleSleepRevert */
	bool		reap;		/* a descriptor closed: recheck clients */

	/* Sleep requests. */
	uint32_t	req_reason;	/* IOPMROOT_SLEEP's */
	pid_t		req_pid;
	int		acpi_state;	/* ACPI_SLEEP_REQUEST's state, 0 = none */
	bool		acpi_cancelled;	/* ACPI_SLEEP_CANCEL since */
	bool		own_pending;	/* the ACPI request is the worker's */
	uint32_t	own_reason;
	pid_t		own_pid;
	uint64_t	suspends;	/* power_suspend_early count */
	uint64_t	resumes;	/* power_resume count */
	uint64_t	handled;	/* resumes the wake messages went out for */
	sbintime_t	wake_time;	/* sbinuptime() of the last wake */
	bool		wake_lid;	/* the lid opened during the sleep */
	time_t		alarm[IOPM_ALARM_COUNT]; /* UTC s, 0 = none */
	int		alarm_armed;	/* IOPM_ALARM_* armed, -1 = none */
	int		alarm_woke;	/* IOPM_ALARM_* that woke us, -1 = none */

	/* Lid, buttons, AC. */
	bool		lid_present;
	bool		lid_closed;
	sbintime_t	lid_time;
	u_int		buttons;	/* IOPM_BUTTON_* not yet handled */
	sbintime_t	pb_time;
	bool		ac_online;

	/* Policy, set through /dev/iopmroot. */
	struct iopm_fd	*owner;		/* policy owner, NULL = kernel's */
	pid_t		owner_pid;
	int		saved_lid_sx;	/* hw.acpi.*_state before the owner */
	int		saved_pb_sx;
	int		saved_sb_sx;
	uint32_t	clamshell_disable; /* kPMSetClamshellSleepState */
	bool		pb_disabled;	/* "DisablePowerButtonSleep" */
	bool		pb_published;	/* ... has been published */
	bool		sleep_disabled;	/* "SleepDisabled" */
	bool		uuid_set;	/* "SleepWakeUUID" set by a client */
	char		uuid[IOPMROOT_STRING_MAX];

	/* Stored for pmconfigd; the kernel does not act on these. */
	uint32_t	aggr[IOPMROOT_AGGR_MAX];
	uint64_t	assertion_levels;
	int64_t		tickle;

	uint32_t	last_reason;
	pid_t		last_pid;
	uint64_t	sleep_count;
} iopm;

static struct mtx iopm_mtx;
MTX_SYSINIT(iopm_mtx, &iopm_mtx, "iopmroot", MTX_DEF);
static struct cv iopm_cv;
static uint64_t iopm_regid;		/* set once at SI_SUB_CONFIGURE */
static struct cdev *iopm_cdev;
static struct thread *iopm_td;
#ifdef DEV_ACPI
static struct acpi_softc *iopm_acpi;	/* acpi0, NULL if absent */
#endif

static int iopm_pmtimeout;
SYSCTL_INT(_kern, OID_AUTO, pmtimeout, CTLFLAG_RWTUN, &iopm_pmtimeout, 0,
    "Seconds the IOPMrootDomain waits for apps to answer a sleep message "
    "(0: 30)");
static struct timeval iopm_sleeptime;
SYSCTL_STRUCT(_kern, OID_AUTO, sleeptime, CTLFLAG_RD, &iopm_sleeptime,
    timeval, "Time of the last system sleep");
static struct timeval iopm_waketime;
SYSCTL_STRUCT(_kern, OID_AUTO, waketime, CTLFLAG_RD, &iopm_waketime,
    timeval, "Time of the last system wake");

static void	iopm_sleep_request(uint32_t reason, pid_t pid);

/* ---- helpers ---- */

static const char *
iopm_reason_name(uint32_t reason)
{
	switch (reason) {
	case IOPMROOT_REASON_CLAMSHELL:
		return ("Clamshell Sleep");
	case IOPMROOT_REASON_POWER_BUTTON:
		return ("Power Button Sleep");
	case IOPMROOT_REASON_IDLE:
		return ("Idle Sleep");
	case IOPMROOT_REASON_LOW_POWER:
		return ("Low Power Sleep");
	case IOPMROOT_REASON_THERMAL:
		return ("Thermal Emergency Sleep");
	case IOPMROOT_REASON_MAINTENANCE:
		return ("Maintenance Sleep");
	default:
		return ("Software Sleep");
	}
}

static bool
iopm_reason_valid(uint32_t reason)
{
	switch (reason) {
	case IOPMROOT_REASON_SOFTWARE:
	case IOPMROOT_REASON_CLAMSHELL:
	case IOPMROOT_REASON_POWER_BUTTON:
	case IOPMROOT_REASON_SOFTWARE_SLEEP:
	case IOPMROOT_REASON_IDLE:
	case IOPMROOT_REASON_LOW_POWER:
	case IOPMROOT_REASON_THERMAL:
	case IOPMROOT_REASON_MAINTENANCE:
		return (true);
	default:
		return (false);
	}
}

static sbintime_t
iopm_budget(void)
{
	int s;

	s = iopm_pmtimeout;
	if (s <= 0)
		s = IOPM_BUDGET_DEFAULT;
	else if (s > IOPM_BUDGET_MAX)
		s = IOPM_BUDGET_MAX;
	return (s * SBT_1S);
}

/* True if acpi_ReqSleepState() can suspend to hw.acpi.suspend_state. */
static bool
iopm_sleep_supported(void)
{
#ifdef IOPM_CAN_SLEEP
	int sx;

	if (iopm_acpi == NULL)
		return (false);
	sx = iopm_acpi->acpi_suspend_sx;
	return (sx >= ACPI_STATE_S1 && sx <= ACPI_STATE_S4);
#else
	return (false);
#endif
}

/* xnu's shouldSleepOnClamshellClosed(), or the kernel's own lid action
 * while there is no policy owner. */
static bool
iopm_clamshell_sleeps_locked(void)
{
	mtx_assert(&iopm_mtx, MA_OWNED);
	if (!iopm.lid_present)
		return (false);
	if (iopm.owner != NULL)
		return (iopm.clamshell_disable == 0 && iopm_sleep_supported());
#ifdef IOPM_CAN_SLEEP
	if (iopm_acpi != NULL)
		return (iopm_acpi->acpi_lid_switch_sx >= ACPI_STATE_S1 &&
		    iopm_acpi->acpi_lid_switch_sx <= ACPI_STATE_S4);
#endif
	return (false);
}

/* The reason for an ACPI request the root domain did not make: the lid or
 * button event that just preceded it, else a software request. */
static uint32_t
iopm_event_reason_locked(void)
{
	sbintime_t now;

	mtx_assert(&iopm_mtx, MA_OWNED);
	now = sbinuptime();
	if (iopm.pb_time != 0 && now - iopm.pb_time < IOPM_EVENT_WINDOW)
		return (IOPMROOT_REASON_POWER_BUTTON);
	if (iopm.lid_closed && iopm.lid_time != 0 &&
	    now - iopm.lid_time < IOPM_EVENT_WINDOW)
		return (IOPMROOT_REASON_CLAMSHELL);
	return (IOPMROOT_REASON_SOFTWARE_SLEEP);
}

/* acpi_ReqSleepState() to hw.acpi.suspend_state; only called once
 * iopm_sleep_supported() said yes. */
static int
iopm_acpi_request(void)
{
#ifdef DEV_ACPI
	return (acpi_ReqSleepState(iopm_acpi, iopm_acpi->acpi_suspend_sx));
#else
	return (EOPNOTSUPP);
#endif
}

/* acpi_sleep_listener_ack(): 0 acknowledges (and suspends), else cancels. */
static int
iopm_acpi_ack(int error)
{
#ifdef DEV_ACPI
	return (acpi_sleep_listener_ack(error));
#else
	(void)error;
	return (EOPNOTSUPP);
#endif
}

/* ---- wake alarms ---- */

/* True if this machine has a wake alarm (atrtc's, with RTC_EN). */
static bool
iopm_alarm_supported(void)
{
#ifdef IOPM_CAN_SLEEP
	return (atrtc_wake_alarm_reach() != 0);
#else
	return (false);
#endif
}

/* The earliest alarm in [first, last] still ahead, or -1; its time in
 * *when. */
static int
iopm_alarm_next(int first, int last, time_t *when)
{
	time_t now;
	int i, k;

	now = time_second;
	k = -1;
	mtx_lock(&iopm_mtx);
	for (i = first; i <= last; i++)
		if (iopm.alarm[i] > now &&
		    (k < 0 || iopm.alarm[i] < iopm.alarm[k]))
			k = i;
	if (k >= 0)
		*when = iopm.alarm[k];
	mtx_unlock(&iopm_mtx);
	return (k);
}

/* Arm alarm `k` in the RTC. */
static int
iopm_alarm_arm(int k, time_t when)
{
#ifdef IOPM_CAN_SLEEP
	int error;

	error = atrtc_wake_alarm_arm(when);
	if (error != 0)
		log(LOG_WARNING, "IOPMrootDomain: RTC alarm for %jd not set "
		    "(error %d)\n", (intmax_t)when, error);
	else if (bootverbose)
		printf("IOPMrootDomain: RTC alarm set for %jd (%s)\n",
		    (intmax_t)when, iopm_alarms[k].key != NULL ?
		    iopm_alarms[k].key : iopm_alarms[k].wake_type);
	return (error);
#else
	(void)k;
	(void)when;
	return (EOPNOTSUPP);
#endif
}

static void
iopm_alarm_disarm(void)
{
#ifdef IOPM_CAN_SLEEP
	atrtc_wake_alarm_disarm();
#endif
}

/* An IOPMCalendarStruct as UTC seconds; a zero year is 0 (clear). */
static int
iopm_calendar_time(const struct iopmroot_calendar *cal, time_t *out)
{
	struct clocktime ct;
	struct timespec ts;

	if (cal->year == 0) {
		*out = 0;
		return (0);
	}
	bzero(&ct, sizeof(ct));
	ct.year = cal->year;
	ct.mon = cal->month;
	ct.day = cal->day;
	ct.hour = cal->hour;
	ct.min = cal->minute;
	ct.sec = cal->second;
	ct.dow = -1;
	if (clock_ct_to_ts(&ct, &ts) != 0)
		return (EINVAL);
	*out = ts.tv_sec;
	return (0);
}

/* Set or clear (when == 0) alarm `k`. EOPNOTSUPP without a wake alarm. */
static int
iopm_alarm_set(int k, time_t when)
{
	if (!iopm_alarm_supported())
		return (EOPNOTSUPP);
	mtx_lock(&iopm_mtx);
	iopm.alarm[k] = when;
	mtx_unlock(&iopm_mtx);
	return (0);
}

/* IOPMROOT_SETPROP for alarm `k`'s key. */
static int
iopm_alarm_setprop(int k, const struct iopmroot_prop *pr)
{
	struct iopmroot_calendar cal;
	time_t when;
	int error;

	if (iopm_alarms[k].calendar) {
		if (pr->type != IOPMROOT_PROP_DATA ||
		    pr->number != sizeof(cal))
			return (EINVAL);
		memcpy(&cal, pr->string, sizeof(cal));
		if ((error = iopm_calendar_time(&cal, &when)) != 0)
			return (error);
	} else {
		if (pr->type != IOPMROOT_PROP_NUMBER || pr->number < 0)
			return (EINVAL);
		when = pr->number == 0 ? 0 : time_second + pr->number;
	}
	return (iopm_alarm_set(k, when));
}

/* Merge `nvl` into the entry's properties; consumes it. */
static void
iopm_set_props(nvlist_t *nvl)
{
	if (iopm_regid == 0) {
		nvlist_destroy(nvl);
		return;
	}
	(void)ioreg_update_virtual(iopm_regid, nvl);
}

/* ---- policy owner ---- */

/*
 * The first descriptor that sets clamshell or power-button policy owns it.
 * The kernel's own lid and button actions are switched off meanwhile, so
 * only the root domain requests sleep for them.
 */
static int
iopm_claim_policy(struct iopm_fd *fd)
{
	mtx_lock(&iopm_mtx);
	if (iopm.owner != NULL && iopm.owner != fd) {
		mtx_unlock(&iopm_mtx);
		return (EBUSY);
	}
	if (iopm.owner == NULL) {
		iopm.owner = fd;
		iopm.owner_pid = fd->pid;
#ifdef DEV_ACPI
		if (iopm_acpi != NULL) {
			iopm.saved_lid_sx = iopm_acpi->acpi_lid_switch_sx;
			iopm.saved_pb_sx = iopm_acpi->acpi_power_button_sx;
			iopm.saved_sb_sx = iopm_acpi->acpi_sleep_button_sx;
			iopm_acpi->acpi_lid_switch_sx = ACPI_STATE_UNKNOWN;
			iopm_acpi->acpi_power_button_sx = ACPI_STATE_UNKNOWN;
			iopm_acpi->acpi_sleep_button_sx = ACPI_STATE_UNKNOWN;
		}
#endif
	}
	mtx_unlock(&iopm_mtx);
	return (0);
}

/* The owner's descriptor closed: give the lid and buttons back to the
 * kernel's own actions, unless someone changed the sysctls meanwhile. */
static void
iopm_release_policy_locked(struct iopm_fd *fd)
{
	mtx_assert(&iopm_mtx, MA_OWNED);
	if (iopm.owner != fd)
		return;
	iopm.owner = NULL;
	iopm.owner_pid = 0;
	iopm.clamshell_disable = 0;
	iopm.pb_disabled = false;
#ifdef DEV_ACPI
	if (iopm_acpi != NULL) {
		if (iopm_acpi->acpi_lid_switch_sx == ACPI_STATE_UNKNOWN)
			iopm_acpi->acpi_lid_switch_sx = iopm.saved_lid_sx;
		if (iopm_acpi->acpi_power_button_sx == ACPI_STATE_UNKNOWN)
			iopm_acpi->acpi_power_button_sx = iopm.saved_pb_sx;
		if (iopm_acpi->acpi_sleep_button_sx == ACPI_STATE_UNKNOWN)
			iopm_acpi->acpi_sleep_button_sx = iopm.saved_sb_sx;
	}
#endif
	iopm.work |= IOPM_W_POLICY;
	cv_broadcast(&iopm_cv);
}

/* ---- app fan-out ---- */

/*
 * Start a phase: take the current app power clients, number them under a
 * new serial, and send each `message` with its notificationID. With
 * `answer`, the phase waits for every client (iopm_wait_answers()); a client
 * that cannot be reached counts as answered.
 */
static void
iopm_fanout(uint32_t message, int phase, bool answer)
{
	struct ioreg_interest_client *ic;
	struct iopm_client *cl, *old;
	u_int n, max, i;
	uint64_t txn;

	ic = NULL;
	n = 0;
	for (max = 16; iopm_regid != 0;) {
		ic = malloc(max * sizeof(*ic), M_IOPMROOT, M_WAITOK);
		n = ioreg_interest_clients(iopm_regid,
		    IOREG_INTEREST_APP_POWER, ic, max);
		if (n <= max)
			break;
		free(ic, M_IOPMROOT);
		ic = NULL;
		max = n + 8;
	}
	if (n > IOPM_CLIENTS_MAX)
		n = IOPM_CLIENTS_MAX;
	cl = NULL;
	if (n > 0) {
		cl = malloc(n * sizeof(*cl), M_IOPMROOT, M_WAITOK | M_ZERO);
		for (i = 0; i < n; i++) {
			cl[i].ic = ic[i];
			cl[i].answered = !answer;
		}
	}
	free(ic, M_IOPMROOT);

	mtx_lock(&iopm_mtx);
	old = iopm.clients;
	txn = ++iopm.txn;
	iopm.clients = cl;
	iopm.nclients = n;
	iopm.pending = answer ? n : 0;
	iopm.vetoed = false;
	iopm.revert = false;
	iopm.phase = phase;
	mtx_unlock(&iopm_mtx);
	free(old, M_IOPMROOT);

	for (i = 0; i < n; i++) {
		if (ioreg_message_client(cl[i].ic.client, message,
		    IOPM_NOTIFICATION_ID(txn, i), 0) == 0)
			continue;
		mtx_lock(&iopm_mtx);
		if (!cl[i].answered) {
			cl[i].answered = true;
			iopm.pending--;
		}
		mtx_unlock(&iopm_mtx);
	}
}

/* Count clients whose port has died (their process exited) as answered. */
static void
iopm_reap(void)
{
	u_int i, n;

	mtx_lock(&iopm_mtx);
	n = iopm.nclients;
	mtx_unlock(&iopm_mtx);
	for (i = 0; i < n; i++) {
		if (iopm.clients[i].answered ||
		    ioreg_interest_client_live(iopm.clients[i].ic.client))
			continue;
		mtx_lock(&iopm_mtx);
		if (!iopm.clients[i].answered) {
			iopm.clients[i].answered = true;
			iopm.pending--;
		}
		mtx_unlock(&iopm_mtx);
	}
}

/* xnu's "PM notification timeout (pid N, name)". */
static void
iopm_log_tardy(uint32_t message)
{
	struct iopm_client *cl;
	u_int i;

	mtx_lock(&iopm_mtx);
	for (i = 0; i < iopm.nclients; i++) {
		cl = &iopm.clients[i];
		if (!cl->answered)
			log(LOG_NOTICE, "IOPMrootDomain: PM notification "
			    "timeout (pid %d, %s) for message 0x%x\n",
			    (int)cl->ic.pid, cl->ic.comm, message);
	}
	mtx_unlock(&iopm_mtx);
}

/*
 * Wait for the answers to the current phase, iopm_budget() at most. A
 * CanSystemSleep phase also ends on a veto, a revert, or an ACPI request
 * (which supersedes the idle sleep); a WillSleep phase on an ACPI cancel or
 * a sleep ACPI forced (`suspends` moved on).
 */
static int
iopm_wait_answers(uint64_t suspends)
{
	sbintime_t deadline;
	int r;

	deadline = sbinuptime() + iopm_budget();
	mtx_lock(&iopm_mtx);
	for (;;) {
		if (iopm.phase == IOPMROOT_PHASE_CAN_SLEEP) {
			if (iopm.vetoed) {
				r = IOPM_WAIT_VETO;
				break;
			}
			if (iopm.revert) {
				r = IOPM_WAIT_ABORT;
				break;
			}
			if ((iopm.work & IOPM_W_ACPI_REQ) != 0) {
				r = IOPM_WAIT_PREEMPT;
				break;
			}
		} else if (iopm.acpi_cancelled || iopm.suspends != suspends) {
			r = IOPM_WAIT_ABORT;
			break;
		}
		if (iopm.pending == 0) {
			r = IOPM_WAIT_DONE;
			break;
		}
		if (iopm.reap) {
			iopm.reap = false;
			mtx_unlock(&iopm_mtx);
			iopm_reap();
			mtx_lock(&iopm_mtx);
			continue;
		}
		if (cv_timedwait_sbt(&iopm_cv, &iopm_mtx, deadline, 0,
		    C_ABSOLUTE) == EWOULDBLOCK && iopm.pending != 0) {
			r = IOPM_WAIT_TIMEOUT;
			break;
		}
	}
	mtx_unlock(&iopm_mtx);
	return (r);
}

/* ---- wake ---- */

/*
 * Publish "Wake Reason" / "Wake Type". `first` is the look taken right
 * after the wake: it always publishes (an unknown reason as "") and, if the
 * reason is unknown, arms the second look.
 */
static void
iopm_wake_reason_update(bool first)
{
	const char *reason, *type;
	nvlist_t *nvl;
	bool lid;
	int alarm;

	mtx_lock(&iopm_mtx);
	lid = iopm.wake_lid;
	alarm = iopm.alarm_woke;
	mtx_unlock(&iopm_mtx);

	reason = NULL;
	type = IOPM_WAKE_TYPE_USER;
#ifdef DEV_ACPI
	if (acpi_woke_by_power_button())
		reason = IOPM_WAKE_REASON_PWRB;
#endif
	if (reason == NULL && alarm >= 0) {
		reason = IOPM_WAKE_REASON_RTC;
		type = iopm_alarms[alarm].wake_type;
	}
	if (reason == NULL && lid)
		reason = IOPM_WAKE_REASON_LID;
	if (reason != NULL || first) {
		nvl = nvlist_create(0);
		nvlist_add_string(nvl, "Wake Reason",
		    reason != NULL ? reason : "");
		nvlist_add_string(nvl, "Wake Type",
		    reason != NULL ? type : "");
		iopm_set_props(nvl);
	}

	mtx_lock(&iopm_mtx);
	iopm.recheck = (first && reason == NULL) ?
	    sbinuptime() + IOPM_WAKE_RECHECK : 0;
	mtx_unlock(&iopm_mtx);
}

/* The system is back in S0: the wake properties, then the two wake
 * messages, which need no answer. */
static void
iopm_did_wake(void)
{
	mtx_lock(&iopm_mtx);
	iopm.handled = iopm.resumes;
	iopm.wake_time = sbinuptime();
	iopm.phase = IOPMROOT_PHASE_WAKING;
	mtx_unlock(&iopm_mtx);

	if (bootverbose)
		printf("IOPMrootDomain: wake\n");
	iopm_wake_reason_update(true);
	iopm_fanout(IOPMROOT_MSG_SYSTEM_WILL_POWER_ON, IOPMROOT_PHASE_WAKING,
	    false);
	iopm_fanout(IOPMROOT_MSG_SYSTEM_HAS_POWERED_ON, IOPMROOT_PHASE_ON,
	    false);
}

/* A suspend/resume cycle ended that no WillSleep phase is following, e.g.
 * one entered while the root domain was not yet the sleep listener. */
static void
iopm_check_wake(void)
{
	bool woke;

	mtx_lock(&iopm_mtx);
	woke = iopm.resumes != iopm.handled && iopm.resumes == iopm.suspends;
	mtx_unlock(&iopm_mtx);
	if (woke)
		iopm_did_wake();
}

/* ---- sleep ---- */

/* "Last Sleep Reason" and "SleepWakeUUID" for the sleep about to happen. */
static void
iopm_sleep_props(uint32_t reason)
{
	struct uuid u;
	nvlist_t *nvl;
	char buf[IOPMROOT_STRING_MAX];
	char *p;

	mtx_lock(&iopm_mtx);
	if (iopm.uuid_set) {
		strlcpy(buf, iopm.uuid, sizeof(buf));
		iopm.uuid_set = false;	/* one sleep only */
		mtx_unlock(&iopm_mtx);
	} else {
		mtx_unlock(&iopm_mtx);
		kern_uuidgen(&u, 1);
		snprintf_uuid(buf, sizeof(buf), &u);
		for (p = buf; *p != '\0'; p++)
			*p = toupper(*p);	/* as Darwin's CFUUID */
	}
	nvl = nvlist_create(0);
	nvlist_add_string(nvl, "Last Sleep Reason", iopm_reason_name(reason));
	nvlist_add_string(nvl, "SleepWakeUUID", buf);
	iopm_set_props(nvl);
}

/*
 * ACPI_SLEEP_REQUEST is waiting: the WillSleep phase, then the
 * acknowledgement that suspends, then the wake (or WillNotSleep if the
 * sleep was called off before the system suspended).
 */
static void
iopm_system_sleep(void)
{
	sbintime_t deadline;
	uint64_t suspends;
	uint32_t reason;
	bool asked, refuse, slept, cancelled;
	int error, r, state;

	mtx_lock(&iopm_mtx);
	iopm.work &= ~(IOPM_W_ACPI_REQ | IOPM_W_SLEEP);
	state = iopm.acpi_state;
	iopm.acpi_state = 0;
	/* An idle sleep's clients were asked CanSystemSleep already. */
	asked = iopm.phase == IOPMROOT_PHASE_CAN_SLEEP;
	if (state == 0) {	/* cancelled before the worker got to it */
		iopm.own_pending = false;
		iopm.phase = IOPMROOT_PHASE_ON;
		mtx_unlock(&iopm_mtx);
		if (asked)
			iopm_fanout(IOPMROOT_MSG_SYSTEM_WILL_NOT_SLEEP,
			    IOPMROOT_PHASE_ON, false);
		return;
	}
	if (iopm.own_pending) {
		reason = iopm.own_reason;
		iopm.last_pid = iopm.own_pid;
	} else {
		reason = iopm_event_reason_locked();
		iopm.last_pid = 0;
	}
	iopm.own_pending = false;
	refuse = iopm.sleep_disabled && reason != IOPMROOT_REASON_THERMAL;
	if (!refuse) {
		iopm.last_reason = reason;
		iopm.sleep_count++;
	}
	suspends = iopm.suspends;
	mtx_unlock(&iopm_mtx);

	if (refuse) {
		(void)iopm_acpi_ack(EPERM);
		mtx_lock(&iopm_mtx);
		iopm.phase = IOPMROOT_PHASE_ON;
		mtx_unlock(&iopm_mtx);
		if (asked)
			iopm_fanout(IOPMROOT_MSG_SYSTEM_WILL_NOT_SLEEP,
			    IOPMROOT_PHASE_ON, false);
		return;
	}
	if (bootverbose)
		printf("IOPMrootDomain: %s (S%d)\n", iopm_reason_name(reason),
		    state);

	iopm_sleep_props(reason);
	iopm_fanout(IOPMROOT_MSG_SYSTEM_WILL_SLEEP, IOPMROOT_PHASE_WILL_SLEEP,
	    true);
	r = iopm_wait_answers(suspends);
	if (r == IOPM_WAIT_TIMEOUT)
		iopm_log_tardy(IOPMROOT_MSG_SYSTEM_WILL_SLEEP);

	mtx_lock(&iopm_mtx);
	slept = iopm.suspends != suspends;
	cancelled = iopm.acpi_cancelled;
	iopm.phase = IOPMROOT_PHASE_SLEEPING;
	mtx_unlock(&iopm_mtx);

	/*
	 * Acknowledge: unless the request was called off or ACPI already
	 * forced the sleep, this suspends right here and returns after the
	 * wake. It returns early (0) while an apm client has yet to
	 * acknowledge, or with an error if the suspend could not start.
	 */
	error = 0;
	if (!slept && !cancelled)
		error = iopm_acpi_ack(0);

	/* A failed ack may have lost a race with ACPI's own forced sleep,
	 * which is then already suspending: give it a moment to show. */
	deadline = sbinuptime() + (error != 0 ? IOPM_ACK_GRACE :
	    IOPM_SLEEP_WAIT);
	mtx_lock(&iopm_mtx);
	for (;;) {
		slept = iopm.suspends != suspends;
		if (slept && iopm.resumes == iopm.suspends)
			break;		/* slept and woke */
		if (!slept && iopm.acpi_cancelled)
			break;		/* never suspended */
		if (cv_timedwait_sbt(&iopm_cv, &iopm_mtx, deadline, 0,
		    C_ABSOLUTE) == EWOULDBLOCK)
			break;
	}
	slept = iopm.suspends != suspends && iopm.resumes == iopm.suspends;
	mtx_unlock(&iopm_mtx);

	if (slept)
		iopm_did_wake();
	else
		iopm_fanout(IOPMROOT_MSG_SYSTEM_WILL_NOT_SLEEP,
		    IOPMROOT_PHASE_ON, false);
}

/*
 * Sleep for `reason`: an idle sleep asks CanSystemSleep first; then the ACPI
 * request, which comes straight back through iopm_acpi_listener() as
 * ACPI_SLEEP_REQUEST and runs the WillSleep phase.
 */
static void
iopm_sleep_request(uint32_t reason, pid_t pid)
{
	bool got;
	int r;

	if (!iopm_sleep_supported())
		return;
	mtx_lock(&iopm_mtx);
	if (iopm.phase != IOPMROOT_PHASE_ON || suspend_blocked ||
	    (iopm.sleep_disabled && reason != IOPMROOT_REASON_THERMAL)) {
		mtx_unlock(&iopm_mtx);
		return;
	}
	mtx_unlock(&iopm_mtx);

	if (reason == IOPMROOT_REASON_IDLE) {
		iopm_fanout(IOPMROOT_MSG_CAN_SYSTEM_SLEEP,
		    IOPMROOT_PHASE_CAN_SLEEP, true);
		r = iopm_wait_answers(0);
		if (r == IOPM_WAIT_TIMEOUT)
			iopm_log_tardy(IOPMROOT_MSG_CAN_SYSTEM_SLEEP);
		if (r == IOPM_WAIT_VETO || r == IOPM_WAIT_ABORT) {
			iopm_fanout(IOPMROOT_MSG_SYSTEM_WILL_NOT_SLEEP,
			    IOPMROOT_PHASE_ON, false);
			return;
		}
		if (r == IOPM_WAIT_PREEMPT) {
			/* Someone else's sleep; the worker runs it next. */
			mtx_lock(&iopm_mtx);
			iopm.phase = IOPMROOT_PHASE_ON;
			mtx_unlock(&iopm_mtx);
			return;
		}
	}

	mtx_lock(&iopm_mtx);
	iopm.own_pending = true;
	iopm.own_reason = reason;
	iopm.own_pid = pid;
	mtx_unlock(&iopm_mtx);

	(void)iopm_acpi_request();

	mtx_lock(&iopm_mtx);
	got = (iopm.work & IOPM_W_ACPI_REQ) != 0;
	if (!got) {
		/* Refused (shutting down, another request pending). */
		iopm.own_pending = false;
		iopm.phase = IOPMROOT_PHASE_ON;
	}
	mtx_unlock(&iopm_mtx);
	if (got)
		iopm_system_sleep();
	else if (reason == IOPMROOT_REASON_IDLE)
		iopm_fanout(IOPMROOT_MSG_SYSTEM_WILL_NOT_SLEEP,
		    IOPMROOT_PHASE_ON, false);
}

/* ---- lid, buttons, policy ---- */

/* AppleClamshellState / AppleClamshellCausesSleep, and with `message`
 * kIOPMMessageClamshellStateChange to general-interest clients. */
static void
iopm_clamshell_publish(bool message)
{
	nvlist_t *nvl;
	bool present, closed, sleeps;

	mtx_lock(&iopm_mtx);
	present = iopm.lid_present;
	closed = iopm.lid_closed;
	sleeps = iopm_clamshell_sleeps_locked();
	mtx_unlock(&iopm_mtx);
	if (!present)
		return;
	nvl = nvlist_create(0);
	nvlist_add_bool(nvl, "AppleClamshellState", closed);
	nvlist_add_bool(nvl, "AppleClamshellCausesSleep", sleeps);
	iopm_set_props(nvl);
	if (message)
		ioreg_message_entry(iopm_regid,
		    IOPMROOT_MSG_CLAMSHELL_STATE_CHANGE,
		    (closed ? IOPMROOT_CLAMSHELL_STATE_BIT : 0) |
		    (sleeps ? IOPMROOT_CLAMSHELL_SLEEP_BIT : 0), 0, NULL);
}

/* xnu: with the lid closed and clamshell sleep allowed, sleep. Only the
 * policy owner's rules act here; without one the kernel's lid action has
 * already requested the sleep. */
static void
iopm_clamshell_eval(void)
{
	bool go;

	mtx_lock(&iopm_mtx);
	go = iopm.owner != NULL && iopm.lid_closed &&
	    iopm_clamshell_sleeps_locked() && iopm.phase == IOPMROOT_PHASE_ON;
	mtx_unlock(&iopm_mtx);
	if (go)
		iopm_sleep_request(IOPMROOT_REASON_CLAMSHELL, 0);
}

static void
iopm_lid_changed(void)
{
	bool second_look;

	iopm_clamshell_publish(true);
	mtx_lock(&iopm_mtx);
	second_look = iopm.recheck != 0 && iopm.wake_lid;
	mtx_unlock(&iopm_mtx);
	if (second_look)
		iopm_wake_reason_update(false);
	iopm_clamshell_eval();
}

/*
 * xnu's handlePowerNotification(kIOPMPowerButton): the power button sleeps
 * unless "DisablePowerButtonSleep" (or sleep is impossible), and otherwise
 * the root domain does nothing with it; the sleep key sleeps ("Software
 * Sleep"). Only with a policy owner; without one the kernel's own
 * hw.acpi.power_button_state action (soft-off by default) applies. As on
 * Darwin, a press reaches userland as a HID power key, not as a root-domain
 * message: acpi_button's evdev KEY_POWER, an IOHIDDevice entry (#335), for
 * loginwindow to offer restart/sleep/shut down. A press within
 * IOPM_WAKE_GRACE of a wake is the one that woke the machine.
 */
static void
iopm_buttons(void)
{
	u_int b;
	bool owner, pb, woke;

	mtx_lock(&iopm_mtx);
	b = iopm.buttons;
	iopm.buttons = 0;
	owner = iopm.owner != NULL && iopm.phase == IOPMROOT_PHASE_ON;
	woke = iopm.wake_time != 0 && iopm.pb_time >= iopm.wake_time &&
	    iopm.pb_time - iopm.wake_time < IOPM_WAKE_GRACE;
	pb = !iopm.pb_disabled && !suspend_blocked && !iopm.sleep_disabled &&
	    iopm_sleep_supported();
	mtx_unlock(&iopm_mtx);
	if (!owner)
		return;
	if ((b & IOPM_BUTTON_POWER) != 0) {
		if (pb && !woke)
			iopm_sleep_request(IOPMROOT_REASON_POWER_BUTTON, 0);
	} else if ((b & IOPM_BUTTON_SLEEP) != 0)
		iopm_sleep_request(IOPMROOT_REASON_SOFTWARE_SLEEP, 0);
}

static void
iopm_policy_changed(void)
{
	nvlist_t *nvl;
	bool pbpub, pbdis;

	mtx_lock(&iopm_mtx);
	pbpub = iopm.pb_published;
	pbdis = iopm.pb_disabled;
	mtx_unlock(&iopm_mtx);
	if (pbpub) {
		nvl = nvlist_create(0);
		nvlist_add_bool(nvl, "DisablePowerButtonSleep", pbdis);
		iopm_set_props(nvl);
	}
	iopm_clamshell_publish(false);
	iopm_clamshell_eval();
}

/* ---- the worker thread ---- */

static void
iopm_worker(void *arg __unused)
{
	uint32_t reason;
	pid_t pid;
	u_int work;

	mtx_lock(&iopm_mtx);
	for (;;) {
		while (iopm.work == 0) {
			if (iopm.recheck == 0) {
				cv_wait(&iopm_cv, &iopm_mtx);
				continue;
			}
			if (cv_timedwait_sbt(&iopm_cv, &iopm_mtx, iopm.recheck,
			    0, C_ABSOLUTE) == EWOULDBLOCK && iopm.work == 0 &&
			    iopm.recheck != 0) {
				iopm.recheck = 0;
				iopm.work |= IOPM_W_RECHECK;
			}
		}
		/* ACPI_REQ is left for iopm_system_sleep() to take. */
		work = iopm.work;
		iopm.work &= IOPM_W_ACPI_REQ;
		reason = iopm.req_reason;
		pid = iopm.req_pid;
		mtx_unlock(&iopm_mtx);

		if ((work & IOPM_W_RESUME) != 0)
			iopm_check_wake();
		/* The lid first, so a lid-close sleep is announced before it. */
		if ((work & IOPM_W_LID) != 0)
			iopm_lid_changed();
		if ((work & IOPM_W_ACPI_REQ) != 0)
			iopm_system_sleep();
		else if ((work & IOPM_W_SLEEP) != 0)
			iopm_sleep_request(reason, pid);
		if ((work & IOPM_W_BUTTON) != 0)
			iopm_buttons();
		if ((work & IOPM_W_POLICY) != 0)
			iopm_policy_changed();
		if ((work & IOPM_W_RECHECK) != 0)
			iopm_wake_reason_update(false);

		mtx_lock(&iopm_mtx);
	}
}

/* ---- events in ---- */

#ifdef DEV_ACPI
/*
 * The ACPI sleep listener (K1). Runs in the requester's thread with no lock
 * held (for the worker's own request, inside its acpi_ReqSleepState()), and
 * RESUME inside the suspending thread's acpi_EnterSleepState(); it only
 * records and wakes the worker.
 */
static void
iopm_acpi_listener(void *arg __unused, int event, int state)
{
	mtx_lock(&iopm_mtx);
	switch (event) {
	case ACPI_SLEEP_REQUEST:
		iopm.acpi_state = state;
		iopm.acpi_cancelled = false;
		iopm.work |= IOPM_W_ACPI_REQ;
		break;
	case ACPI_SLEEP_CANCEL:
		iopm.acpi_state = 0;
		iopm.acpi_cancelled = true;
		iopm.own_pending = false;
		break;
	case ACPI_SLEEP_RESUME:
		iopm.work |= IOPM_W_RESUME;
		break;
	}
	cv_broadcast(&iopm_cv);
	mtx_unlock(&iopm_mtx);
}

static void
iopm_lid_event(void *arg __unused, int open)
{
	mtx_lock(&iopm_mtx);
	iopm.lid_present = true;
	iopm.lid_closed = (open == 0);
	iopm.lid_time = sbinuptime();
	/* Opened while asleep: reported during DEVICE_RESUME, or soon after
	 * the wake if the firmware sends a notify instead. */
	if (open != 0 && (iopm.suspends != iopm.handled || iopm.recheck != 0))
		iopm.wake_lid = true;
	iopm.work |= IOPM_W_LID;
	cv_broadcast(&iopm_cv);
	mtx_unlock(&iopm_mtx);
}

static void
iopm_button_event(void *arg __unused, int type)
{
	mtx_lock(&iopm_mtx);
	if (type == ACPI_POWER_BUTTON) {
		iopm.buttons |= IOPM_BUTTON_POWER;
		iopm.pb_time = sbinuptime();
	} else
		iopm.buttons |= IOPM_BUTTON_SLEEP;
	iopm.work |= IOPM_W_BUTTON;
	cv_broadcast(&iopm_cv);
	mtx_unlock(&iopm_mtx);
}

static void
iopm_acad_event(void *arg __unused, int online)
{
	mtx_lock(&iopm_mtx);
	iopm.ac_online = (online != 0);
	mtx_unlock(&iopm_mtx);
}
#endif /* DEV_ACPI */

/*
 * The system is about to sleep: arm the earliest wake alarm still ahead.
 * This runs in the thread that suspends, before processes are stopped.
 */
static void
iopm_suspend_early(void *arg __unused)
{
	time_t when;
	int k;

	k = iopm_alarm_next(IOPM_ALARM_WAKE, IOPM_ALARM_SLEEPSVC, &when);
	if (k >= 0 && iopm_alarm_arm(k, when) != 0)
		k = -1;

	mtx_lock(&iopm_mtx);
	iopm.suspends++;
	iopm.wake_lid = false;
	iopm.recheck = 0;
	iopm.alarm_armed = k;
	iopm.alarm_woke = -1;
	microtime(&iopm_sleeptime);
	cv_broadcast(&iopm_cv);
	mtx_unlock(&iopm_mtx);
}

/*
 * Back in S0 (or the suspend backed out): disarm the wake alarm, note
 * whether it woke us, and drop the wake times, which were for this sleep.
 */
static void
iopm_resume(void *arg __unused)
{
	int i, k;
	bool rtc;

	rtc = false;
#ifdef IOPM_CAN_SLEEP
	rtc = acpi_woke_by_rtc_alarm() != 0;
#endif
	mtx_lock(&iopm_mtx);
	k = iopm.alarm_armed;
	iopm.alarm_armed = -1;
	iopm.alarm_woke = rtc ? k : -1;
	for (i = IOPM_ALARM_WAKE; i <= IOPM_ALARM_SLEEPSVC; i++)
		iopm.alarm[i] = 0;
	mtx_unlock(&iopm_mtx);
	if (k >= 0)
		iopm_alarm_disarm();

	mtx_lock(&iopm_mtx);
	iopm.resumes++;
	microtime(&iopm_waketime);
	iopm.work |= IOPM_W_RESUME;
	cv_broadcast(&iopm_cv);
	mtx_unlock(&iopm_mtx);
}

#ifdef IOPM_CAN_SLEEP
/*
 * Powering off: arm the earliest power-on alarm still ahead, before ACPI
 * enters S5 (acpi_shutdown_final runs at SHUTDOWN_PRI_LAST + 150).
 */
static void
iopm_shutdown_final(void *arg __unused, int howto)
{
	time_t when;
	int k;

	if ((howto & RB_POWEROFF) == 0)
		return;
	k = iopm_alarm_next(IOPM_ALARM_POWERON, IOPM_ALARM_POWERON_SECS,
	    &when);
	if (k >= 0)
		(void)iopm_alarm_arm(k, when);
}
#endif

/* ---- /dev/iopmroot ---- */

static d_open_t iopm_open;
static d_ioctl_t iopm_ioctl;
static struct cdevsw iopm_cdevsw = {
	.d_version = D_VERSION,
	.d_open    = iopm_open,
	.d_ioctl   = iopm_ioctl,
	.d_name    = "iopmroot",
};

static void
iopm_fd_dtor(void *data)
{
	struct iopm_fd *fd = data;

	mtx_lock(&iopm_mtx);
	iopm_release_policy_locked(fd);
	/* Its process may have exited: recheck the clients still awaited. */
	iopm.reap = true;
	cv_broadcast(&iopm_cv);
	mtx_unlock(&iopm_mtx);
	free(fd, M_IOPMROOT);
}

static int
iopm_open(struct cdev *dev __unused, int oflags __unused,
    int devtype __unused, struct thread *td)
{
	struct iopm_fd *fd;
	int error;

	fd = malloc(sizeof(*fd), M_IOPMROOT, M_WAITOK | M_ZERO);
	fd->pid = td->td_proc->p_pid;
	error = devfs_set_cdevpriv(fd, iopm_fd_dtor);
	if (error != 0)
		free(fd, M_IOPMROOT);
	return (error);
}

/* IOPMROOT_ALLOW / IOPMROOT_CANCEL. A stale or unknown id is ignored. */
static int
iopm_answer(int64_t nid, bool cancel)
{
	struct iopm_client *cl;
	uint64_t txn, idx;

	if (nid <= 0)
		return (0);
	txn = (uint64_t)nid >> 16;
	idx = (uint64_t)nid & 0xffff;
	mtx_lock(&iopm_mtx);
	if (txn == iopm.txn && idx >= 1 && idx <= iopm.nclients &&
	    (iopm.phase == IOPMROOT_PHASE_CAN_SLEEP ||
	    iopm.phase == IOPMROOT_PHASE_WILL_SLEEP)) {
		cl = &iopm.clients[idx - 1];
		if (!cl->answered) {
			cl->answered = true;
			iopm.pending--;
		}
		/* Only CanSystemSleep can be vetoed; a cancel of WillSleep
		 * counts as an acknowledgement, as on xnu. */
		if (cancel && iopm.phase == IOPMROOT_PHASE_CAN_SLEEP)
			iopm.vetoed = true;
		cv_broadcast(&iopm_cv);
	}
	mtx_unlock(&iopm_mtx);
	return (0);
}

static int
iopm_ioctl_sleep(struct iopmroot_sleep *rq, struct thread *td)
{
	uint32_t reason;
	int error;

	reason = rq->reason;
	if (!iopm_reason_valid(reason))
		return (EINVAL);
	if (reason == IOPMROOT_REASON_SOFTWARE)
		reason = IOPMROOT_REASON_SOFTWARE_SLEEP;
	if (!iopm_sleep_supported())
		return (EOPNOTSUPP);
	error = 0;
	mtx_lock(&iopm_mtx);
	if (suspend_blocked ||
	    (iopm.sleep_disabled && reason != IOPMROOT_REASON_THERMAL))
		error = EBUSY;
	else if (iopm.phase == IOPMROOT_PHASE_ON &&
	    (iopm.work & (IOPM_W_SLEEP | IOPM_W_ACPI_REQ)) == 0) {
		iopm.req_reason = reason;
		iopm.req_pid = td->td_proc->p_pid;
		iopm.work |= IOPM_W_SLEEP;
		cv_broadcast(&iopm_cv);
	}
	/* else a sleep is already under way */
	mtx_unlock(&iopm_mtx);
	return (error);
}

static int
iopm_prop_bool(const struct iopmroot_prop *pr, bool *out)
{
	if (pr->type != IOPMROOT_PROP_BOOL && pr->type != IOPMROOT_PROP_NUMBER)
		return (EINVAL);
	*out = pr->number != 0;
	return (0);
}

/* IOPMROOT_SETPROP: the keys xnu's IOPMrootDomain::setProperties acts on;
 * others are ignored, as there. */
static int
iopm_ioctl_setprop(struct iopmroot_prop *pr, struct iopm_fd *fd)
{
	nvlist_t *nvl;
	bool v;
	int error, k;

	pr->key[sizeof(pr->key) - 1] = '\0';
	if (pr->type < IOPMROOT_PROP_BOOL || pr->type > IOPMROOT_PROP_DATA)
		return (EINVAL);
	if (pr->type == IOPMROOT_PROP_STRING)
		pr->string[sizeof(pr->string) - 1] = '\0';
	if (pr->type == IOPMROOT_PROP_DATA &&
	    (pr->number < 0 || pr->number > IOPMROOT_STRING_MAX))
		return (EINVAL);

	for (k = 0; k < IOPM_ALARM_COUNT; k++)
		if (iopm_alarms[k].key != NULL &&
		    strcmp(pr->key, iopm_alarms[k].key) == 0)
			return (iopm_alarm_setprop(k, pr));

	if (strcmp(pr->key, "System Shutdown") == 0) {
		if ((error = iopm_prop_bool(pr, &v)) != 0)
			return (error);
		suspend_blocked = v;
		return (0);
	}
	if (strcmp(pr->key, "SleepDisabled") == 0) {
		if ((error = iopm_prop_bool(pr, &v)) != 0)
			return (error);
		mtx_lock(&iopm_mtx);
		iopm.sleep_disabled = v;
		mtx_unlock(&iopm_mtx);
		nvl = nvlist_create(0);
		nvlist_add_bool(nvl, "SleepDisabled", v);
		iopm_set_props(nvl);
		return (0);
	}
	if (strcmp(pr->key, "DisablePowerButtonSleep") == 0 ||
	    strcmp(pr->key, "Sleep On Power Button") == 0) {
		if ((error = iopm_prop_bool(pr, &v)) != 0)
			return (error);
		if (strcmp(pr->key, "Sleep On Power Button") == 0)
			v = !v;
		if ((error = iopm_claim_policy(fd)) != 0)
			return (error);
		mtx_lock(&iopm_mtx);
		iopm.pb_disabled = v;
		iopm.pb_published = true;
		mtx_unlock(&iopm_mtx);
		nvl = nvlist_create(0);
		nvlist_add_bool(nvl, "DisablePowerButtonSleep", v);
		iopm_set_props(nvl);
		return (0);
	}
	if (strcmp(pr->key, "SleepWakeUUID") == 0) {
		if (pr->type == IOPMROOT_PROP_STRING && pr->string[0] != '\0') {
			mtx_lock(&iopm_mtx);
			strlcpy(iopm.uuid, pr->string, sizeof(iopm.uuid));
			iopm.uuid_set = true;
			mtx_unlock(&iopm_mtx);
			nvl = nvlist_create(0);
			nvlist_add_string(nvl, "SleepWakeUUID", pr->string);
			iopm_set_props(nvl);
			return (0);
		}
		if ((error = iopm_prop_bool(pr, &v)) != 0 || v)
			return (EINVAL);
		mtx_lock(&iopm_mtx);
		iopm.uuid_set = false;
		mtx_unlock(&iopm_mtx);
		return (0);
	}
	return (0);
}

static void
iopm_getstate(struct iopmroot_state *st)
{
	u_int clients;

	clients = ioreg_interest_clients(iopm_regid, IOREG_INTEREST_APP_POWER,
	    NULL, 0);
	bzero(st, sizeof(*st));
	st->registry_id = iopm_regid;
	st->clients = clients;
	if (iopm_sleep_supported())
		st->flags |= IOPMROOT_F_SLEEP_SUPPORTED;
	mtx_lock(&iopm_mtx);
	st->phase = iopm.phase;
	st->pending = iopm.pending;
	st->last_sleep_reason = iopm.last_reason;
	st->last_sleep_pid = iopm.last_pid;
	st->owner_pid = iopm.owner_pid;
	st->clamshell_sleep_disabled = iopm.clamshell_disable;
	st->user_assertion_levels = iopm.assertion_levels;
	st->activity_tickle = iopm.tickle;
	st->sleep_count = iopm.sleep_count;
	if (iopm.lid_present)
		st->flags |= IOPMROOT_F_LID_PRESENT;
	if (iopm.lid_closed)
		st->flags |= IOPMROOT_F_LID_CLOSED;
	if (iopm_clamshell_sleeps_locked())
		st->flags |= IOPMROOT_F_CLAMSHELL_SLEEPS;
	if (iopm.ac_online)
		st->flags |= IOPMROOT_F_AC_ONLINE;
	if (iopm.sleep_disabled)
		st->flags |= IOPMROOT_F_SLEEP_DISABLED;
	if (iopm.pb_disabled)
		st->flags |= IOPMROOT_F_PB_SLEEP_DISABLED;
	if (suspend_blocked)
		st->flags |= IOPMROOT_F_SYSTEM_SHUTDOWN;
	if (iopm.owner != NULL)
		st->flags |= IOPMROOT_F_POLICY_OWNED;
	mtx_unlock(&iopm_mtx);
	if (iopm_alarm_supported())
		st->flags |= IOPMROOT_F_WAKE_ALARM;
}

/* IOPMROOT_SETMAINTWAKE: kPMSetMaintenanceWakeCalendar. */
static int
iopm_ioctl_maintwake(struct iopmroot_calendar *cal)
{
	time_t when;
	int error, k;

	switch (cal->selector) {
	case IOPMROOT_CALENDAR_MAINTENANCE:
		k = IOPM_ALARM_MAINT;
		break;
	case IOPMROOT_CALENDAR_SLEEP_SERVICE:
		k = IOPM_ALARM_SLEEPSVC;
		break;
	default:
		return (EINVAL);
	}
	if ((error = iopm_calendar_time(cal, &when)) != 0)
		return (error);
	return (iopm_alarm_set(k, when));
}

static int
iopm_ioctl(struct cdev *dev __unused, u_long cmd, caddr_t data,
    int fflag __unused, struct thread *td)
{
	struct iopmroot_aggr *ag;
	struct iopm_fd *fd;
	int error;

	error = devfs_get_cdevpriv((void **)&fd);
	if (error != 0)
		return (error);

	/* What an app needs to answer is open to everyone; the rest is
	 * IOPMSleepSystem and pmconfigd's, root only. */
	switch (cmd) {
	case IOPMROOT_ALLOW:
		return (iopm_answer(*(int64_t *)data, false));
	case IOPMROOT_CANCEL:
		return (iopm_answer(*(int64_t *)data, true));
	case IOPMROOT_GETAGGR:
		ag = (struct iopmroot_aggr *)data;
		if (ag->type >= IOPMROOT_AGGR_MAX)
			return (EINVAL);
		mtx_lock(&iopm_mtx);
		ag->value = iopm.aggr[ag->type];
		mtx_unlock(&iopm_mtx);
		return (0);
	case IOPMROOT_GETSTATE:
		iopm_getstate((struct iopmroot_state *)data);
		return (0);
	case IOPMROOT_SLEEP:
	case IOPMROOT_SETPROP:
	case IOPMROOT_SETAGGR:
	case IOPMROOT_SETASSERTIONS:
	case IOPMROOT_TICKLE:
	case IOPMROOT_SETCLAMSHELL:
	case IOPMROOT_IDLEREVERT:
	case IOPMROOT_SETMAINTWAKE:
		break;
	default:
		return (ENOTTY);
	}

	if (priv_check(td, PRIV_DRIVER) != 0)
		return (EPERM);
	switch (cmd) {
	case IOPMROOT_SLEEP:
		return (iopm_ioctl_sleep((struct iopmroot_sleep *)data, td));
	case IOPMROOT_SETPROP:
		return (iopm_ioctl_setprop((struct iopmroot_prop *)data, fd));
	case IOPMROOT_SETAGGR:
		ag = (struct iopmroot_aggr *)data;
		if (ag->type >= IOPMROOT_AGGR_MAX)
			return (EINVAL);
		mtx_lock(&iopm_mtx);
		iopm.aggr[ag->type] = ag->value;
		mtx_unlock(&iopm_mtx);
		return (0);
	case IOPMROOT_SETASSERTIONS:
		mtx_lock(&iopm_mtx);
		iopm.assertion_levels = *(uint64_t *)data;
		mtx_unlock(&iopm_mtx);
		return (0);
	case IOPMROOT_TICKLE:
		mtx_lock(&iopm_mtx);
		iopm.tickle = time_uptime;
		mtx_unlock(&iopm_mtx);
		return (0);
	case IOPMROOT_SETCLAMSHELL:
		if ((error = iopm_claim_policy(fd)) != 0)
			return (error);
		mtx_lock(&iopm_mtx);
		iopm.clamshell_disable = *(uint32_t *)data;
		iopm.work |= IOPM_W_POLICY;
		cv_broadcast(&iopm_cv);
		mtx_unlock(&iopm_mtx);
		return (0);
	case IOPMROOT_IDLEREVERT:
		mtx_lock(&iopm_mtx);
		if (iopm.phase == IOPMROOT_PHASE_CAN_SLEEP) {
			iopm.revert = true;
			cv_broadcast(&iopm_cv);
		}
		mtx_unlock(&iopm_mtx);
		return (0);
	case IOPMROOT_SETMAINTWAKE:
		return (iopm_ioctl_maintwake((struct iopmroot_calendar *)data));
	}
	return (ENOTTY);
}

/* ---- boot ---- */

/*
 * Subscribe before acpi0 and its lid attach (SI_SUB_CONFIGURE), so the lid's
 * initial state is seen. The handlers only record until the worker runs.
 */
static void
iopm_hooks(void *arg __unused)
{
	cv_init(&iopm_cv, "iopmroot");
	iopm.alarm_armed = -1;
	iopm.alarm_woke = -1;
#ifdef DEV_ACPI
	EVENTHANDLER_REGISTER(acpi_lid_event, iopm_lid_event, NULL,
	    EVENTHANDLER_PRI_ANY);
	EVENTHANDLER_REGISTER(acpi_button_event, iopm_button_event, NULL,
	    EVENTHANDLER_PRI_ANY);
	EVENTHANDLER_REGISTER(acpi_acad_event, iopm_acad_event, NULL,
	    EVENTHANDLER_PRI_ANY);
#endif
	EVENTHANDLER_REGISTER(power_suspend_early, iopm_suspend_early, NULL,
	    EVENTHANDLER_PRI_ANY);
	EVENTHANDLER_REGISTER(power_resume, iopm_resume, NULL,
	    EVENTHANDLER_PRI_ANY);
#ifdef IOPM_CAN_SLEEP
	EVENTHANDLER_REGISTER(shutdown_final, iopm_shutdown_final, NULL,
	    SHUTDOWN_PRI_LAST + 100);
#endif
}
SYSINIT(iopmroot_hooks, SI_SUB_DRIVERS, SI_ORDER_ANY, iopm_hooks, NULL);

/*
 * Publish the entry, create /dev/iopmroot, start the worker, and become
 * ACPI's sleep listener, once acpi0 has attached.
 */
static void
iopm_init(void *arg __unused)
{
	nvlist_t *nvl;
	bool present, closed, sleeps;

#ifdef DEV_ACPI
	iopm_acpi = devclass_get_softc(devclass_find("acpi"), 0);
#endif
	mtx_lock(&iopm_mtx);
	present = iopm.lid_present;
	closed = iopm.lid_closed;
	sleeps = iopm_clamshell_sleeps_locked();
	iopm.work &= ~IOPM_W_LID;	/* the initial state goes in below */
	mtx_unlock(&iopm_mtx);

	nvl = nvlist_create(0);
	nvlist_add_bool(nvl, "IOSleepSupported", iopm_sleep_supported());
	nvlist_add_number(nvl, "IOPMSystemSleepType", IOPM_SLEEP_TYPE_NORMAL);
	nvlist_add_number(nvl, "System Capabilities", IOPM_CAPABILITIES_FULL);
	if (present) {
		nvlist_add_bool(nvl, "AppleClamshellState", closed);
		nvlist_add_bool(nvl, "AppleClamshellCausesSleep", sleeps);
	}
	iopm_regid = ioreg_publish_virtual(NULL, IOPMROOT_CLASS,
	    IOPMROOT_CLASS, nvl);

	iopm_cdev = make_dev(&iopm_cdevsw, 0, UID_ROOT, GID_WHEEL, 0666,
	    "iopmroot");
	if (kthread_add(iopm_worker, NULL, NULL, &iopm_td, 0, 0,
	    "iopmroot") != 0)
		return;		/* no worker: leave ACPI sleeping at once */
#ifdef DEV_ACPI
	(void)acpi_register_sleep_listener(iopm_acpi_listener, NULL);
#endif
}
SYSINIT(iopmroot, SI_SUB_CONFIGURE, SI_ORDER_ANY, iopm_init, NULL);
