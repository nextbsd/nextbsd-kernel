/*
 * NextBSD in-kernel IOPMrootDomain (nextbsd-kernel#243, E17 K4).
 *
 * As on xnu, the power-management root domain lives in the kernel
 * (sys/dev/iokit/iokit_pmroot.c). It is the IOPMrootDomain entry in the
 * in-kernel IORegistry, under IOResources, so IOServiceMatching
 * ("IOPMrootDomain") finds it. It sends the system sleep/wake messages to
 * every app that registered with IORegisterForSystemPower, waits for their
 * acknowledgements, and then sleeps. pmconfigd is one of its clients and owns
 * policy (idle sleep, assertions, clamshell and power-button rules).
 *
 * This header is the userland ABI: the /dev/iopmroot ioctls, which stand in
 * for RootDomainUserClient (NextBSD has no IOServiceOpen), and the message
 * values. It is additive and self-contained, so userland (libIOKit,
 * pmconfigd) may carry a copy until the SDK publishes it; keep these structs
 * frozen once shipped.
 *
 * App fan-out. A client registers an IOREGIOCINTEREST watch with interest
 * IOREG_INTEREST_APP_POWER (kIOAppPowerStateInterest) on the root domain's
 * registry id. Each message arrives as an ioreg_interest_msg
 * (<sys/mach/iokit_notify.h>) whose `message` is an IOPMROOT_MSG_* value and
 * whose argument[0] is that client's acknowledgement id, Darwin's
 * notificationID (always non-zero). argument[1] is 0.
 *
 *   idle sleep:   CAN_SYSTEM_SLEEP -> every client answers IOPMROOT_ALLOW or
 *                 IOPMROOT_CANCEL; one CANCEL vetoes -> SYSTEM_WILL_NOT_SLEEP
 *   any sleep:    SYSTEM_WILL_SLEEP -> every client answers IOPMROOT_ALLOW
 *                 (a CANCEL counts as an ALLOW; the sleep can no longer be
 *                 vetoed) -> S3 -> SYSTEM_WILL_POWER_ON ->
 *                 SYSTEM_HAS_POWERED_ON
 *   aborted:      a sleep that is called off before the system suspends
 *                 (acpiconf -k 1, a failed suspend) sends
 *                 SYSTEM_WILL_NOT_SLEEP after SYSTEM_WILL_SLEEP
 *
 * The root domain waits at most kern.pmtimeout seconds (0, the default,
 * means 30) for the answers to each message that needs one; clients that
 * have not answered by then are logged and the sleep goes ahead. WILL_POWER_ON,
 * HAS_POWERED_ON and WILL_NOT_SLEEP need no answer. An answer with a stale or
 * unknown id is ignored and still returns 0.
 *
 * General interest (IOREG_INTEREST_GENERAL) on the same entry hears
 * kIOMessageServicePropertyChange whenever a property below changes, and
 * IOPMROOT_MSG_CLAMSHELL_STATE_CHANGE when the lid opens or closes.
 *
 * Properties (IORegistryEntryCreateCFProperty / ioreg -l), Apple's keys:
 *   "IOSleepSupported"          bool    S3 can be requested on this machine
 *   "IOPMSystemSleepType"       number  2 (kIOPMSleepTypeNormalSleep)
 *   "System Capabilities"       number  0xf, CPU|Graphics|Audio|Network
 *   "Last Sleep Reason"         string  "Clamshell Sleep", "Power Button
 *                                       Sleep", "Software Sleep", "Idle
 *                                       Sleep", "Low Power Sleep", "Thermal
 *                                       Emergency Sleep", ... (first sleep on)
 *   "Wake Reason"               string  "EC.LidOpen (User)", "PWRB (User)",
 *                                       "" if unknown (first wake on)
 *   "Wake Type"                 string  "User", "" if unknown
 *   "SleepWakeUUID"             string  set for each sleep (first sleep on)
 *   "AppleClamshellState"       bool    lid closed (only with a lid)
 *   "AppleClamshellCausesSleep" bool    closing the lid sleeps (with a lid)
 *   "SleepDisabled", "DisablePowerButtonSleep"  bool, once set
 *
 * Timestamps: kern.sleeptime and kern.waketime (struct timeval, calendar
 * time) as on xnu, for IOPMGetLastWakeTime.
 */
#ifndef _SYS_IOPMROOT_H_
#define _SYS_IOPMROOT_H_

#include <sys/types.h>
#include <sys/ioccom.h>

#define	IOPMROOT_DEVICE		"/dev/iopmroot"
#define	IOPMROOT_CLASS		"IOPMrootDomain"	/* entry name and class */

/* The IOREGIOCINTEREST type for kIOAppPowerStateInterest; also in
 * <sys/ioregistry.h>. */
#ifndef IOREG_INTEREST_APP_POWER
#define	IOREG_INTEREST_APP_POWER	2
#endif

/*
 * ioreg_interest_msg.message values. App power interest: Darwin's IOMessage.h
 * (iokit_common_msg(x) == 0xe0000000 | x). General interest: IOPMPrivate.h
 * (iokit_family_msg(sub_iokit_powermanagement, x) == 0xe0034000 | x).
 */
#define	IOPMROOT_MSG_CAN_SYSTEM_SLEEP		0xe0000270 /* kIOMessageCanSystemSleep */
#define	IOPMROOT_MSG_SYSTEM_WILL_SLEEP		0xe0000280 /* kIOMessageSystemWillSleep */
#define	IOPMROOT_MSG_SYSTEM_WILL_NOT_SLEEP	0xe0000290 /* kIOMessageSystemWillNotSleep */
#define	IOPMROOT_MSG_SYSTEM_HAS_POWERED_ON	0xe0000300 /* kIOMessageSystemHasPoweredOn */
#define	IOPMROOT_MSG_SYSTEM_WILL_POWER_ON	0xe0000320 /* kIOMessageSystemWillPowerOn */
#define	IOPMROOT_MSG_CLAMSHELL_STATE_CHANGE	0xe0034100 /* kIOPMMessageClamshellStateChange */

/* IOPMROOT_MSG_CLAMSHELL_STATE_CHANGE argument[0] (kClamshellStateBit,
 * kClamshellSleepBit). */
#define	IOPMROOT_CLAMSHELL_STATE_BIT	0x00000001	/* lid is closed */
#define	IOPMROOT_CLAMSHELL_SLEEP_BIT	0x00000002	/* closing it sleeps */

/*
 * Sleep reasons, xnu's kIOPMSleepReason* (IOPMPrivate.h). The root domain
 * publishes the matching string as "Last Sleep Reason".
 */
#define	IOPMROOT_REASON_SOFTWARE	0	/* == 103, IOPMSleepSystem() */
#define	IOPMROOT_REASON_CLAMSHELL	101	/* "Clamshell Sleep" */
#define	IOPMROOT_REASON_POWER_BUTTON	102	/* "Power Button Sleep" */
#define	IOPMROOT_REASON_SOFTWARE_SLEEP	103	/* "Software Sleep" */
#define	IOPMROOT_REASON_IDLE		105	/* "Idle Sleep" (CanSystemSleep) */
#define	IOPMROOT_REASON_LOW_POWER	106	/* "Low Power Sleep" */
#define	IOPMROOT_REASON_THERMAL		107	/* "Thermal Emergency Sleep" */
#define	IOPMROOT_REASON_MAINTENANCE	108	/* "Maintenance Sleep" */

/*
 * IOPMROOT_SLEEP: IOPMSleepSystem / IOPMSleepSystemWithOptions (selector
 * kPMSleepSystem). Requests sleep with `reason` (an IOPMROOT_REASON_*) and
 * returns at once; the fan-out and the sleep follow asynchronously.
 * IOPMROOT_REASON_IDLE first asks CAN_SYSTEM_SLEEP. Root only. Returns 0
 * (also when a sleep is already under way), EPERM for a non-root caller,
 * EINVAL for an unknown reason, EOPNOTSUPP when the machine cannot sleep
 * ("IOSleepSupported" false), EBUSY while "SleepDisabled" or "System
 * Shutdown" is set (the thermal reason ignores "SleepDisabled").
 */
struct iopmroot_sleep {
	uint32_t	reason;		/* in: IOPMROOT_REASON_* */
	uint32_t	_pad;
};

/*
 * IOPMROOT_SETPROP: IORegistryEntrySetCFProperty on the root domain. Root
 * only. `key` is NUL-terminated; `type` says which value field is used.
 * Keys the root domain acts on, as xnu's IOPMrootDomain::setProperties:
 *   "System Shutdown"          bool    blocks sleep (kern.suspend_blocked)
 *   "SleepDisabled"            bool    refuses every sleep but thermal
 *   "DisablePowerButtonSleep"  bool    the power button does not sleep
 *   "Sleep On Power Button"    bool or number, the inverse of the above
 *   "SleepWakeUUID"            string  the UUID for the next sleep; bool
 *                                      false clears it (the kernel then
 *                                      makes one per sleep)
 * Other keys are accepted and ignored, as on xnu. Returns 0, EPERM, EINVAL
 * for a bad type or a key with the wrong type, EBUSY (see policy owner).
 */
#define	IOPMROOT_PROP_BOOL	1	/* value in `number`, 0 or 1 */
#define	IOPMROOT_PROP_NUMBER	2	/* value in `number` */
#define	IOPMROOT_PROP_STRING	3	/* value in `string` */

#define	IOPMROOT_KEY_MAX	64
#define	IOPMROOT_STRING_MAX	64

struct iopmroot_prop {
	char		key[IOPMROOT_KEY_MAX];		/* in */
	uint32_t	type;				/* in: IOPMROOT_PROP_* */
	uint32_t	_pad;
	int64_t		number;				/* in: bool / number */
	char		string[IOPMROOT_STRING_MAX];	/* in: string */
};

/*
 * IOPMROOT_SETAGGR / IOPMROOT_GETAGGR: kPMSetAggressiveness and
 * kPMGetAggressiveness. The kernel stores the value per type (kPMMinutesToSleep
 * and the rest, < IOPMROOT_AGGR_MAX) for pmconfigd, which stays the policy
 * owner; nothing in the kernel acts on it. SET is root only. EINVAL for a
 * type out of range.
 */
#define	IOPMROOT_AGGR_MAX	32

struct iopmroot_aggr {
	uint32_t	type;		/* in */
	uint32_t	value;		/* in (SET) / out (GET) */
};

/*
 * IOPMROOT_GETSTATE: the root domain's state, for pmconfigd and debugging.
 */
#define	IOPMROOT_PHASE_ON		0	/* awake, nothing under way */
#define	IOPMROOT_PHASE_CAN_SLEEP	1	/* waiting on CAN_SYSTEM_SLEEP */
#define	IOPMROOT_PHASE_WILL_SLEEP	2	/* waiting on SYSTEM_WILL_SLEEP */
#define	IOPMROOT_PHASE_SLEEPING		3	/* suspending or asleep */
#define	IOPMROOT_PHASE_WAKING		4	/* sending the wake messages */

#define	IOPMROOT_F_SLEEP_SUPPORTED	0x00000001
#define	IOPMROOT_F_LID_PRESENT		0x00000002
#define	IOPMROOT_F_LID_CLOSED		0x00000004
#define	IOPMROOT_F_CLAMSHELL_SLEEPS	0x00000008 /* AppleClamshellCausesSleep */
#define	IOPMROOT_F_AC_ONLINE		0x00000010 /* last acpi_acad_event */
#define	IOPMROOT_F_SLEEP_DISABLED	0x00000020 /* "SleepDisabled" */
#define	IOPMROOT_F_PB_SLEEP_DISABLED	0x00000040 /* "DisablePowerButtonSleep" */
#define	IOPMROOT_F_SYSTEM_SHUTDOWN	0x00000080 /* kern.suspend_blocked */
#define	IOPMROOT_F_POLICY_OWNED		0x00000100 /* see policy owner */

struct iopmroot_state {
	uint64_t	registry_id;	/* the IOPMrootDomain entry */
	uint32_t	phase;		/* IOPMROOT_PHASE_* */
	uint32_t	flags;		/* IOPMROOT_F_* */
	uint32_t	clients;	/* IOREG_INTEREST_APP_POWER watches */
	uint32_t	pending;	/* answers still awaited this phase */
	uint32_t	last_sleep_reason; /* IOPMROOT_REASON_*, 0 = none yet */
	int32_t		last_sleep_pid;	/* IOPMROOT_SLEEP caller, 0 = kernel */
	int32_t		owner_pid;	/* policy owner, 0 = none */
	uint32_t	clamshell_sleep_disabled; /* kPMSetClamshellSleepState */
	uint64_t	user_assertion_levels;	/* kPMSetUserAssertionLevels */
	int64_t		activity_tickle;	/* kPMActivityTickle, uptime s */
	uint64_t	sleep_count;	/* sleeps since boot */
	uint64_t	reserved[4];
};

/*
 * Policy owner. Lid and button events reach the root domain from ACPI. While
 * no process has set clamshell or power-button policy, the kernel's own
 * hw.acpi.lid_switch_state, power_button_state and sleep_button_state apply
 * as before (a sleep they request still goes through the app fan-out). The
 * first /dev/iopmroot descriptor to set that policy, with IOPMROOT_SETCLAMSHELL
 * or the "DisablePowerButtonSleep" / "Sleep On Power Button" property,
 * becomes the policy owner (pmconfigd): those three sysctls are set to NONE
 * and the root domain itself requests "Clamshell Sleep" when the lid closes
 * and clamshell sleep is not disabled, "Power Button Sleep" on the power
 * button unless "DisablePowerButtonSleep", and "Software Sleep" on the sleep
 * button. As on xnu, a power-button press that does not sleep (disabled, or
 * sleep impossible) is not acted on by the root domain at all, and no
 * root-domain message carries it: userland sees the press as a HID power
 * key (evdev KEY_POWER on the ACPI button's IOHIDDevice entry), which is
 * what loginwindow uses on Darwin. A press within 3 s of a wake is ignored.
 * Without an owner the kernel's own action (soft-off by default) applies.
 * When the owner closes its descriptor (or exits) the sysctls get
 * their earlier values back, if still NONE. Another descriptor that tries to
 * set that policy meanwhile gets EBUSY.
 */

/*
 * The ioctls. ALLOW, CANCEL, GETAGGR and GETSTATE are open to every user (an
 * app acknowledges with them); the rest need root and return EPERM otherwise.
 *
 *   IOPMROOT_ALLOW      IOAllowPowerChange(notificationID), kPMAllowPowerChange
 *   IOPMROOT_CANCEL     IOCancelPowerChange(notificationID), kPMCancelPowerChange
 *   IOPMROOT_SLEEP      IOPMSleepSystem, kPMSleepSystem (struct above)
 *   IOPMROOT_SETPROP    IORegistryEntrySetCFProperty (struct above)
 *   IOPMROOT_SETAGGR    kPMSetAggressiveness
 *   IOPMROOT_GETAGGR    kPMGetAggressiveness
 *   IOPMROOT_SETASSERTIONS  kPMSetUserAssertionLevels (levels bitmask), stored
 *   IOPMROOT_TICKLE     kPMActivityTickle, records the time
 *   IOPMROOT_SETCLAMSHELL   kPMSetClamshellSleepState: non-zero disables
 *                       clamshell sleep, 0 allows it (and sleeps at once if
 *                       the lid is closed); claims the policy owner
 *   IOPMROOT_IDLEREVERT kPMRequestIdleSleepRevert: abandons an idle sleep
 *                       still in CAN_SYSTEM_SLEEP (-> WILL_NOT_SLEEP)
 *   IOPMROOT_GETSTATE   struct iopmroot_state
 */
#define	IOPMROOT_ALLOW		_IOW('O', 1, int64_t)
#define	IOPMROOT_CANCEL		_IOW('O', 2, int64_t)
#define	IOPMROOT_SLEEP		_IOW('O', 3, struct iopmroot_sleep)
#define	IOPMROOT_SETPROP	_IOW('O', 4, struct iopmroot_prop)
#define	IOPMROOT_SETAGGR	_IOW('O', 5, struct iopmroot_aggr)
#define	IOPMROOT_GETAGGR	_IOWR('O', 6, struct iopmroot_aggr)
#define	IOPMROOT_SETASSERTIONS	_IOW('O', 7, uint64_t)
#define	IOPMROOT_TICKLE		_IO('O', 8)
#define	IOPMROOT_SETCLAMSHELL	_IOW('O', 9, uint32_t)
#define	IOPMROOT_IDLEREVERT	_IO('O', 10)
#define	IOPMROOT_GETSTATE	_IOR('O', 11, struct iopmroot_state)

#endif /* !_SYS_IOPMROOT_H_ */
