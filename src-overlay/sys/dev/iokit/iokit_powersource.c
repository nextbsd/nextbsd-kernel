/*
 * NextBSD IORegistry power-source bridge (nextbsd-kernel#244, E17 K7).
 *
 * Publishes each ACPI battery as an IOPMPowerSource entry carrying Apple's
 * power-source keys (kIOPMPS*Key in IOKit/pwr_mgt/IOPM.h), so pmconfigd finds
 * batteries the way it does on a Mac: IOServiceMatching("IOPMPowerSource"),
 * then IOServiceAddInterestNotification(kIOGeneralInterest) on each, and a
 * re-read of every property on kIOPMMessageBatteryStatusHasChanged. On Darwin
 * AppleSmartBattery is that entry; NextBSD has no Smart Battery Manager, so
 * the values come from the ACPI battery drivers' _BST and _BIX/_BIF data
 * (ACPI_BATT_GET_STATUS / ACPI_BATT_GET_INFO) and the AC adapter's _PSR
 * (acpi_acad_get_acline()).
 *
 * Entry. One per "battery" device under acpi (acpi_cmbat, or acpi_smbat),
 * published under that device's newbus node while a battery is in the slot,
 * and retired when it is taken out or the device detaches. Name: the ACPI
 * node's name ("BAT0"); class: IOPMPowerSource. No battery, no entry: a
 * machine without one (QEMU, desktops, the Pi) publishes nothing, and an
 * empty slot publishes nothing until a battery is inserted. pmconfigd
 * reports AC power on its own then, as on a desktop Mac.
 *
 * Units are Apple's: capacities in mAh, current in mA (negative while
 * discharging), voltage in mV, times in minutes. A battery that reports in
 * mWh/mW is converted with its design voltage (present voltage for the rate),
 * so CurrentCapacity / Amperage gives hours as pmconfigd expects.
 *
 * Push signals, no polling. acpi_cmbat_event (patches/0088: 0x80 status,
 * 0x81 information, 0x00/0x01 insert/remove), acpi_acad_event and
 * power_resume schedule a refresh. A refresh that finds new values merges
 * them with ioreg_update_virtual(), which sends
 * kIOMessageServicePropertyChange, then sends
 * kIOPMMessageBatteryStatusHasChanged on the entry; one that finds nothing
 * changed sends nothing. While the driver has no valid data yet (the
 * embedded controller is not always ready right after boot or resume) the
 * refresh retries a bounded number of times, as acpi_cmbat's own
 * initialisation does.
 *
 * Context. The ACPI eventhandlers run in the ACPI task thread, the device
 * attach/detach ones under the bus topology lock, power_resume in the resume
 * path. Each only takes this file's mutex and schedules the entry's
 * taskqueue_thread timeout task. The task reads the battery and talks to the
 * registry (both may sleep) holding no lock of ours: the driver calls can
 * invoke acpi_acad_event re-entrantly. The task is the only writer of an
 * entry's registry state; detach unlinks the entry, drains its task, then
 * retires and frees it, so the device_t is never used after detach begins.
 *
 * Nothing here prints: a normal boot stays silent on the console.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/queue.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/bus.h>
#include <sys/eventhandler.h>
#include <sys/taskqueue.h>
#include <sys/nv.h>
#include <sys/ioregistry.h>

#include <contrib/dev/acpica/include/acpi.h>

#include <dev/acpica/acpivar.h>
#include <dev/acpica/acpiio.h>

#define	IOKIT_PS_CLASS		"IOPMPowerSource"

/* kIOPMMessageBatteryStatusHasChanged: iokit_family_msg(sub_iokit_pmu, 0x100). */
#define	IOKIT_PS_MSG_BATTERY_STATUS	0xe0024100

/* AvgTimeToEmpty / AvgTimeToFull when not applicable (Smart Battery 0xffff). */
#define	IOKIT_PS_TIME_NONE	65535

/* Waiting for the driver's first valid data: 30 x 2 s, like acpi_cmbat's 60 s. */
#define	IOKIT_PS_RETRY_MAX	30
#define	IOKIT_PS_RETRY_DELAY	(2 * hz)

/* Let acpi_cmbat re-read _BIX (queued from its notify) or re-initialise. */
#define	IOKIT_PS_INFO_DELAY	(hz)
#define	IOKIT_PS_RESUME_DELAY	(2 * hz)

static MALLOC_DEFINE(M_IOKITPS, "iokit_ps", "IORegistry power-source bridge");

/* The published values; compared whole to skip a refresh that changed nothing. */
struct iokit_ps_state {
	bool		external;	/* ExternalConnected */
	bool		charging;	/* IsCharging */
	bool		full;		/* FullyCharged */
	bool		warn;		/* AtWarnLevel */
	bool		critical;	/* AtCriticalLevel */
	uint32_t	cur;		/* CurrentCapacity, mAh */
	uint32_t	max;		/* MaxCapacity, mAh */
	uint32_t	design;		/* DesignCapacity, mAh */
	int32_t		amperage;	/* Amperage, mA */
	uint32_t	voltage;	/* Voltage, mV */
	uint32_t	cycles;		/* CycleCount */
	uint32_t	tte;		/* AvgTimeToEmpty, min */
	uint32_t	ttf;		/* AvgTimeToFull, min */
	uint32_t	remaining;	/* TimeRemaining, min */
	char		manufacturer[ACPI_CMBAT_MAXSTRLEN];
	char		model[ACPI_CMBAT_MAXSTRLEN];
	char		serial[ACPI_CMBAT_MAXSTRLEN];
	char		type[ACPI_CMBAT_MAXSTRLEN];
};

/*
 * One battery device. `dev`, `task` and `dying` are set under iokit_ps_mtx;
 * `id`, `last` and the names belong to the task (and to detach once the
 * task is drained).
 */
struct iokit_ps_ent {
	LIST_ENTRY(iokit_ps_ent) link;
	device_t		dev;
	struct timeout_task	task;
	int			retries;
	bool			dying;
	uint64_t		id;		/* registry id, 0 = not published */
	struct iokit_ps_state	last;
};

static LIST_HEAD(, iokit_ps_ent) iokit_ps_list =
    LIST_HEAD_INITIALIZER(iokit_ps_list);
static struct mtx iokit_ps_mtx;
MTX_SYSINIT(iokit_ps_mtx, &iokit_ps_mtx, "iokit power source", MTX_DEF);

static eventhandler_tag iokit_ps_attach_tag;
static eventhandler_tag iokit_ps_detach_tag;
static eventhandler_tag iokit_ps_cmbat_tag;
static eventhandler_tag iokit_ps_acad_tag;
static eventhandler_tag iokit_ps_resume_tag;

static void	iokit_ps_task(void *arg, int pending);

/* A "battery" device on the acpi bus (acpi_cmbat or acpi_smbat). */
static bool
iokit_ps_is_battery(device_t dev)
{
	device_t parent;

	if (strcmp(device_get_name(dev), "battery") != 0)
		return (false);
	parent = device_get_parent(dev);
	return (parent != NULL && strcmp(device_get_name(parent), "acpi") == 0);
}

static struct iokit_ps_ent *
iokit_ps_find_locked(device_t dev)
{
	struct iokit_ps_ent *pe;

	mtx_assert(&iokit_ps_mtx, MA_OWNED);
	LIST_FOREACH(pe, &iokit_ps_list, link)
		if (pe->dev == dev)
			return (pe);
	return (NULL);
}

/* Schedule a refresh of `pe` in `delay` ticks, with a fresh retry budget. */
static void
iokit_ps_kick_locked(struct iokit_ps_ent *pe, int delay)
{

	mtx_assert(&iokit_ps_mtx, MA_OWNED);
	if (pe->dying)
		return;
	pe->retries = 0;
	taskqueue_enqueue_timeout(taskqueue_thread, &pe->task, delay);
}

static void
iokit_ps_kick_all(int delay)
{
	struct iokit_ps_ent *pe;

	mtx_lock(&iokit_ps_mtx);
	LIST_FOREACH(pe, &iokit_ps_list, link)
		iokit_ps_kick_locked(pe, delay);
	mtx_unlock(&iokit_ps_mtx);
}

/* The driver has no valid data yet: try again later, a bounded number of times. */
static void
iokit_ps_retry(struct iokit_ps_ent *pe)
{

	mtx_lock(&iokit_ps_mtx);
	if (!pe->dying && pe->retries < IOKIT_PS_RETRY_MAX) {
		pe->retries++;
		taskqueue_enqueue_timeout(taskqueue_thread, &pe->task,
		    IOKIT_PS_RETRY_DELAY);
	}
	mtx_unlock(&iokit_ps_mtx);
}

/* An ACPI _BIX string, printable ASCII only, trailing blanks cut. */
static void
iokit_ps_str(char *dst, const char *src)
{
	size_t i, n;

	for (i = 0, n = 0; i < ACPI_CMBAT_MAXSTRLEN - 1 && src[i] != '\0';
	    i++) {
		dst[i] = (src[i] >= 0x20 && src[i] < 0x7f) ? src[i] : '?';
		if (dst[i] != ' ')
			n = i + 1;
	}
	dst[n] = '\0';
}

/*
 * A capacity (mWh or mAh) or rate (mW or mA) in Apple's mAh / mA. `mv` is the
 * voltage to divide a power figure by; 0 leaves the value as it is.
 */
static uint32_t
iokit_ps_ma(uint32_t v, const struct acpi_bix *bix, uint32_t mv)
{

	if (v == ACPI_BATT_UNKNOWN)
		return (0);
	if (bix->units == ACPI_BIX_UNITS_MA || mv == 0 ||
	    mv == ACPI_BATT_UNKNOWN)
		return (v);
	return ((uint32_t)((uint64_t)v * 1000 / mv));
}

/* Apple's view of one battery from its _BST, _BIX and the AC line. */
static void
iokit_ps_compute(struct iokit_ps_state *s, const struct acpi_bst *bst,
    const struct acpi_bix *bix, int acline)
{
	uint32_t dvol, pvol, rate;
	bool discharging;

	memset(s, 0, sizeof(*s));
	dvol = (bix->dvol != 0 && bix->dvol != ACPI_BATT_UNKNOWN) ?
	    bix->dvol : bst->volt;
	pvol = (bst->volt != 0 && bst->volt != ACPI_BATT_UNKNOWN) ?
	    bst->volt : dvol;

	/* Both set is invalid; acpi_battery.c trusts discharging then. */
	discharging = (bst->state & ACPI_BATT_STAT_DISCHARG) != 0;
	s->charging = !discharging &&
	    (bst->state & ACPI_BATT_STAT_CHARGING) != 0;
	/* No AC adapter device (or no reading yet): not discharging is AC. */
	s->external = acline >= 0 ? acline != 0 : !discharging;

	s->max = iokit_ps_ma(bix->lfcap, bix, dvol);
	s->cur = iokit_ps_ma(bst->cap, bix, dvol);
	/* Some batteries report design capacity when full: cap at 100 %. */
	if (s->cur > s->max)
		s->cur = s->max;
	s->design = iokit_ps_ma(bix->dcap, bix, dvol);
	if (s->design == 0)
		s->design = s->max;
	s->voltage = pvol != ACPI_BATT_UNKNOWN ? pvol : 0;

	rate = iokit_ps_ma(bst->rate, bix, pvol);
	if (rate > INT32_MAX)
		rate = INT32_MAX;
	if (discharging)
		s->amperage = -(int32_t)rate;
	else if (s->charging)
		s->amperage = (int32_t)rate;

	s->tte = (discharging && rate != 0) ?
	    (uint32_t)((uint64_t)s->cur * 60 / rate) : IOKIT_PS_TIME_NONE;
	s->ttf = (s->charging && rate != 0) ?
	    (uint32_t)((uint64_t)(s->max - s->cur) * 60 / rate) :
	    IOKIT_PS_TIME_NONE;
	if (s->tte > IOKIT_PS_TIME_NONE)
		s->tte = IOKIT_PS_TIME_NONE;
	if (s->ttf > IOKIT_PS_TIME_NONE)
		s->ttf = IOKIT_PS_TIME_NONE;
	/* AppleSmartBattery: time to full on AC, time to empty otherwise. */
	if (s->external)
		s->remaining = s->ttf != IOKIT_PS_TIME_NONE ? s->ttf : 0;
	else
		s->remaining = s->tte != IOKIT_PS_TIME_NONE ? s->tte : 0;

	s->full = s->external && !s->charging && s->max != 0 &&
	    s->cur >= s->max;
	/* The OEM warning and low levels, in the battery's own units. */
	if (!s->external) {
		s->warn = bix->wcap != 0 && bix->wcap != ACPI_BATT_UNKNOWN &&
		    bst->cap <= bix->wcap;
		s->critical = (bst->state & ACPI_BATT_STAT_CRITICAL) != 0 ||
		    (bix->lcap != 0 && bix->lcap != ACPI_BATT_UNKNOWN &&
		    bst->cap <= bix->lcap);
	}

	if (bix->rev != ACPI_BIX_REV_BIF && bix->cycles != ACPI_BATT_UNKNOWN)
		s->cycles = bix->cycles;
	iokit_ps_str(s->manufacturer, bix->oeminfo);
	iokit_ps_str(s->model, bix->model);
	iokit_ps_str(s->serial, bix->serial);
	iokit_ps_str(s->type, bix->type);
}

/* The property bag for `s`; every key is always present. */
static nvlist_t *
iokit_ps_props(const struct iokit_ps_state *s)
{
	nvlist_t *props;

	props = nvlist_create(0);
	nvlist_add_bool(props, "ExternalConnected", s->external);
	nvlist_add_bool(props, "ExternalChargeCapable", s->external);
	nvlist_add_bool(props, "BatteryInstalled", true);
	nvlist_add_bool(props, "IsCharging", s->charging);
	nvlist_add_bool(props, "FullyCharged", s->full);
	nvlist_add_bool(props, "AtWarnLevel", s->warn);
	nvlist_add_bool(props, "AtCriticalLevel", s->critical);
	nvlist_add_number(props, "CurrentCapacity", s->cur);
	nvlist_add_number(props, "MaxCapacity", s->max);
	nvlist_add_number(props, "DesignCapacity", s->design);
	/* Signed: two's complement in the 64-bit number, negative = discharging. */
	nvlist_add_number(props, "Amperage", (uint64_t)(int64_t)s->amperage);
	nvlist_add_number(props, "Voltage", s->voltage);
	nvlist_add_number(props, "CycleCount", s->cycles);
	nvlist_add_number(props, "TimeRemaining", s->remaining);
	nvlist_add_number(props, "AvgTimeToEmpty", s->tte);
	nvlist_add_number(props, "AvgTimeToFull", s->ttf);
	nvlist_add_string(props, "Manufacturer", s->manufacturer);
	nvlist_add_string(props, "Model", s->model);
	nvlist_add_string(props, "Serial", s->serial);
	nvlist_add_string(props, "BatteryType", s->type);
	return (props);
}

/* Retire the entry, if published (battery removed or device detaching). */
static void
iokit_ps_unpublish(struct iokit_ps_ent *pe)
{

	if (pe->id == 0)
		return;
	(void)ioreg_retire_virtual(pe->id);
	pe->id = 0;
	memset(&pe->last, 0, sizeof(pe->last));
}

/* Publish the battery as a new entry under its device. */
static void
iokit_ps_publish(struct iokit_ps_ent *pe, const struct iokit_ps_state *s)
{
	ACPI_HANDLE h;
	ACPI_BUFFER buf;
	nvlist_t *props;
	char name[IOREG_NAME_MAX], path[IOREG_PATH_MAX];

	strlcpy(name, device_get_nameunit(pe->dev), sizeof(name));
	path[0] = '\0';
	h = acpi_get_handle(pe->dev);
	if (h != NULL) {
		buf.Length = sizeof(name);
		buf.Pointer = name;
		if (ACPI_FAILURE(AcpiGetName(h, ACPI_SINGLE_NAME, &buf)))
			strlcpy(name, device_get_nameunit(pe->dev),
			    sizeof(name));
		buf.Length = sizeof(path);
		buf.Pointer = path;
		if (ACPI_FAILURE(AcpiGetName(h, ACPI_FULL_PATHNAME, &buf)))
			path[0] = '\0';
	}

	props = iokit_ps_props(s);
	nvlist_add_string(props, "org.nextbsd.Device",
	    device_get_nameunit(pe->dev));
	if (path[0] != '\0')
		nvlist_add_string(props, "org.nextbsd.ACPIPath", path);
	/* The registry takes ownership of props, even on failure. */
	pe->id = ioreg_publish_virtual(pe->dev, name, IOKIT_PS_CLASS, props);
	if (pe->id != 0)
		memcpy(&pe->last, s, sizeof(pe->last));
}

/* Read the battery and bring its entry up to date. */
static void
iokit_ps_task(void *arg, int pending __unused)
{
	struct iokit_ps_ent *pe;
	struct iokit_ps_state s;
	struct acpi_bst bst;
	struct acpi_bix bix;
	int acline, error;

	pe = arg;
	memset(&bst, 0, sizeof(bst));
	memset(&bix, 0, sizeof(bix));
	if (ACPI_BATT_GET_STATUS(pe->dev, &bst) != 0) {
		iokit_ps_retry(pe);
		return;
	}
	if (bst.state == ACPI_BATT_STAT_NOT_PRESENT) {
		iokit_ps_unpublish(pe);
		return;
	}
	if (ACPI_BATT_GET_INFO(pe->dev, &bix, sizeof(bix)) != 0 ||
	    !acpi_battery_bst_valid(&bst) || !acpi_battery_bix_valid(&bix)) {
		iokit_ps_retry(pe);
		return;
	}
	if (acpi_acad_get_acline(&acline) != 0)
		acline = -1;

	iokit_ps_compute(&s, &bst, &bix, acline);
	if (pe->id == 0) {
		iokit_ps_publish(pe, &s);
		return;
	}
	if (memcmp(&s, &pe->last, sizeof(s)) == 0)
		return;
	error = ioreg_update_virtual(pe->id, iokit_ps_props(&s));
	if (error == ENOENT) {
		/* Not live any more: publish afresh on the next refresh. */
		pe->id = 0;
		memset(&pe->last, 0, sizeof(pe->last));
	}
	if (error != 0)
		return;
	memcpy(&pe->last, &s, sizeof(pe->last));
	ioreg_message_entry(pe->id, IOKIT_PS_MSG_BATTERY_STATUS, 0, 0, NULL);
}

static void
iokit_ps_attached(void *arg __unused, device_t dev)
{
	struct iokit_ps_ent *pe;

	if (!iokit_ps_is_battery(dev))
		return;
	pe = malloc(sizeof(*pe), M_IOKITPS, M_WAITOK | M_ZERO);
	pe->dev = dev;
	TIMEOUT_TASK_INIT(taskqueue_thread, &pe->task, 0, iokit_ps_task, pe);

	mtx_lock(&iokit_ps_mtx);
	if (iokit_ps_find_locked(dev) != NULL) {
		mtx_unlock(&iokit_ps_mtx);
		free(pe, M_IOKITPS);
		return;
	}
	LIST_INSERT_HEAD(&iokit_ps_list, pe, link);
	iokit_ps_kick_locked(pe, 0);
	mtx_unlock(&iokit_ps_mtx);
}

/*
 * Before the driver detaches: stop the task (it calls into the driver) and
 * retire the entry. A detach that then fails starts over as an attach.
 */
static void
iokit_ps_detach(void *arg __unused, device_t dev, enum evhdev_detach state)
{
	struct iokit_ps_ent *pe;

	if (state == EVHDEV_DETACH_FAILED) {
		iokit_ps_attached(NULL, dev);
		return;
	}
	if (state != EVHDEV_DETACH_BEGIN || !iokit_ps_is_battery(dev))
		return;

	mtx_lock(&iokit_ps_mtx);
	pe = iokit_ps_find_locked(dev);
	if (pe != NULL) {
		LIST_REMOVE(pe, link);
		pe->dying = true;
	}
	mtx_unlock(&iokit_ps_mtx);
	if (pe == NULL)
		return;
	taskqueue_drain_timeout(taskqueue_thread, &pe->task);
	iokit_ps_unpublish(pe);
	free(pe, M_IOKITPS);
}

/* acpi_cmbat_event: a battery's notify (status, information, insert/remove). */
static void
iokit_ps_cmbat(void *arg __unused, device_t dev, int notify)
{
	struct iokit_ps_ent *pe;

	mtx_lock(&iokit_ps_mtx);
	pe = iokit_ps_find_locked(dev);
	if (pe != NULL)
		iokit_ps_kick_locked(pe, notify == ACPI_CMBAT_NOTIFY_BST ?
		    0 : IOKIT_PS_INFO_DELAY);
	mtx_unlock(&iokit_ps_mtx);
}

/* acpi_acad_event: AC plugged in or pulled; every battery's ExternalConnected. */
static void
iokit_ps_acad(void *arg __unused, int online __unused)
{

	iokit_ps_kick_all(0);
}

/* power_resume: the batteries were re-initialised; their state moved on. */
static void
iokit_ps_resume(void *arg __unused)
{

	iokit_ps_kick_all(IOKIT_PS_RESUME_DELAY);
}

static void
iokit_ps_init(void *arg __unused)
{

	iokit_ps_attach_tag = EVENTHANDLER_REGISTER(device_attach,
	    iokit_ps_attached, NULL, EVENTHANDLER_PRI_ANY);
	iokit_ps_detach_tag = EVENTHANDLER_REGISTER(device_detach,
	    iokit_ps_detach, NULL, EVENTHANDLER_PRI_ANY);
	iokit_ps_cmbat_tag = EVENTHANDLER_REGISTER(acpi_cmbat_event,
	    iokit_ps_cmbat, NULL, EVENTHANDLER_PRI_ANY);
	iokit_ps_acad_tag = EVENTHANDLER_REGISTER(acpi_acad_event,
	    iokit_ps_acad, NULL, EVENTHANDLER_PRI_ANY);
	iokit_ps_resume_tag = EVENTHANDLER_REGISTER(power_resume,
	    iokit_ps_resume, NULL, EVENTHANDLER_PRI_ANY);
}
/* Before acpi0 attaches its batteries (SI_SUB_CONFIGURE): no catch-up walk. */
SYSINIT(iokit_ps, SI_SUB_DRIVERS, SI_ORDER_FIRST, iokit_ps_init, NULL);
