/*
 * NextBSD in-kernel IOKit catalogue (K2, nextbsd#215).
 *
 * Flat in-kernel store of IOKit driver personalities, pushed from userland
 * (kextd) via ioctl on /dev/iocatalogue — mechanism (a): the kernel never
 * parses XML. The K3 matcher (nextbsd#216) calls iocat_lookup_pci() from the
 * device_nomatch path to find the driver bundle that claims an unmatched device.
 *
 * See sys/sys/iocatalogue.h and the design in
 * pkgdemon.github.io/nextbsd-inkernel-iokit-feasibility.html §9.
 */

#include "opt_compat_mach.h"
#include "opt_platform.h"	/* FDT — device-tree personalities (#185) */
#include "opt_acpi.h"		/* DEV_ACPI — ACPI _HID personalities (#329) */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/malloc.h>
#include <sys/queue.h>
#include <sys/sx.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/conf.h>
#include <sys/sbuf.h>
#include <sys/sysctl.h>
#include <sys/bus.h>
#include <sys/eventhandler.h>
#include <sys/taskqueue.h>
#include <sys/iocatalogue.h>

#include <dev/pci/pcivar.h>
#include <dev/pci/pcireg.h>	/* PCIC_DISPLAY — vgapci look-through (#64) */

#ifdef FDT
#include <dev/ofw/ofw_bus.h>	/* ofw_bus_get_compat() — #185 */
#include <dev/ofw/ofw_bus_subr.h>
#endif

/* USB personalities (#329): the probe-time usb_attach_arg of an interface. */
#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>

#ifdef DEV_ACPI
/* ACPI _HID personalities (#329). */
#include <contrib/dev/acpica/include/acpi.h>
#include <dev/acpica/acpivar.h>
#endif

#ifdef COMPAT_MACH
/* K3b (#216): the kernel->kextd Mach load-request send (compat/mach/iokit_kextd.c).
 * Declared here rather than via <sys/mach/iokit_kextd.h> so this standard file
 * pulls in no Mach headers. */
extern int iokit_kextd_send(const char *bundle, const char *device,
    uint32_t match_word);
#endif

/* push-triggers-match (K3b): re-check devices that went unmatched against a
 * newly added personality. Defined with the matcher below; called from iocat_add. */
static void iocat_rematch_pending(void);

/* Active present-device rescan: walk the live PCI tree and request loads for
 * present, driver-less devices the catalogue now claims. Robust autoload trigger
 * that does not depend on the device_nomatch event having been captured (real
 * hardware misses boot nomatches the eventhandler never saw). Runs on the
 * taskqueue (sleeps/allocates).
 *
 * Both rematches run once per push batch, not once per personality: kextd adds
 * personalities one IOCATIOCADD at a time (one per USB vendor/product pair, so
 * thousands with the full FreeBSD kext set), and a rescan plus load request per
 * add flooded kextd and the console. Like Darwin's IOCatalogue, which matches
 * after the whole set is added, each add (re)arms one timeout task that fires
 * IOCAT_SETTLE_MS after the last add of the batch. */
static void iocat_rematch_present(void);
static struct timeout_task iocat_present_task;
#define	IOCAT_SETTLE_MS	250
#define	iocat_schedule_rematch()					\
	taskqueue_enqueue_timeout(taskqueue_thread, &iocat_present_task,	\
	    MAX(1, IOCAT_SETTLE_MS * hz / 1000))

static MALLOC_DEFINE(M_IOCAT, "iocatalogue", "in-kernel IOKit catalogue");

static TAILQ_HEAD(, iocat_record) iocat_list =
    TAILQ_HEAD_INITIALIZER(iocat_list);
static struct sx iocat_lock;
SX_SYSINIT(iocat_lock, &iocat_lock, "iocatalogue");
static u_int iocat_count;	/* # records; mutated under iocat_lock */

static void
iocat_free_record(struct iocat_record *r)
{
	if (r->match != NULL)
		free(r->match, M_IOCAT);
	if (r->compat != NULL)
		free(r->compat, M_IOCAT);
	free(r, M_IOCAT);
}

static void
iocat_flush(void)
{
	struct iocat_record *r;

	sx_xlock(&iocat_lock);
	while ((r = TAILQ_FIRST(&iocat_list)) != NULL) {
		TAILQ_REMOVE(&iocat_list, r, link);
		iocat_free_record(r);
	}
	iocat_count = 0;
	sx_xunlock(&iocat_lock);
}

static int
iocat_add(struct iocat_add *ua)
{
	struct iocat_record *r;
	uint32_t *match;
	size_t sz;
	int error;

	if (ua->nmatch == 0 || ua->nmatch > IOCAT_MAX_MATCH)
		return (EINVAL);
	/* The class providers carry (value, mask) pairs (#329). */
	if ((ua->provider_class == IOCAT_PROVIDER_IOUSBHOSTINTERFACE ||
	    ua->provider_class == IOCAT_PROVIDER_IOPCICLASS) &&
	    (ua->nmatch % 2) != 0)
		return (EINVAL);
	/* Force a NUL terminator, then reject an empty bundle id. */
	ua->bundle_id[IOCAT_BUNDLE_ID_MAX - 1] = '\0';
	if (ua->bundle_id[0] == '\0')
		return (EINVAL);

	sz = (size_t)ua->nmatch * sizeof(uint32_t);
	match = malloc(sz, M_IOCAT, M_WAITOK);
	error = copyin((const void *)(uintptr_t)ua->match, match, sz);
	if (error != 0) {
		free(match, M_IOCAT);
		return (error);
	}

	r = malloc(sizeof(*r), M_IOCAT, M_WAITOK | M_ZERO);
	strlcpy(r->bundle_id, ua->bundle_id, sizeof(r->bundle_id));
	r->provider_class = ua->provider_class;
	r->probe_score = ua->probe_score;
	r->nmatch = ua->nmatch;
	r->match = match;

	sx_xlock(&iocat_lock);
	TAILQ_INSERT_TAIL(&iocat_list, r, link);
	iocat_count++;
	sx_xunlock(&iocat_lock);

	/* push-triggers-match: this personality may claim a device that went
	 * unmatched earlier (e.g. the 8260 at boot, before kextd pushed): re-run
	 * the matcher over the pending list, and also actively rescan the live
	 * PCI tree — the boot device_nomatch capture is unreliable on real
	 * hardware, so iocat_pending may not contain a device that is in fact
	 * present and unmatched. Both on the taskqueue, once the push batch
	 * settles (the rematch path re-takes iocat_lock via iocat_lookup_pci). */
	iocat_schedule_rematch();
	return (0);
}

/*
 * Exact match of one word against every record of `provider` (#329 widened
 * this from PCI-only; IOUSBHostDevice words have the same shape).
 */
int
iocat_lookup_word(uint32_t provider, uint32_t word, char *buf, size_t buflen,
    int32_t *score_out)
{
	struct iocat_record *r, *best;
	uint32_t i;

	best = NULL;
	sx_slock(&iocat_lock);
	TAILQ_FOREACH(r, &iocat_list, link) {
		if (r->provider_class != provider)
			continue;
		for (i = 0; i < r->nmatch; i++) {
			if (r->match[i] != word)
				continue;
			if (best == NULL || r->probe_score > best->probe_score)
				best = r;
			break;
		}
	}
	if (best != NULL) {
		strlcpy(buf, best->bundle_id, buflen);
		if (score_out != NULL)
			*score_out = best->probe_score;
	}
	sx_sunlock(&iocat_lock);
	return (best != NULL ? 0 : ENOENT);
}

/*
 * (value, mask) pair match for the class providers (#329): IOPCIClassMatch
 * and IOUSBHostInterface. A record matches when any of its pairs satisfies
 * (word & mask) == (value & mask). An all-zero mask would match everything,
 * so it is skipped rather than treated as a wildcard.
 */
int
iocat_lookup_masked(uint32_t provider, uint32_t word, char *buf, size_t buflen,
    int32_t *score_out)
{
	struct iocat_record *r, *best;
	uint32_t i, v, m;

	best = NULL;
	sx_slock(&iocat_lock);
	TAILQ_FOREACH(r, &iocat_list, link) {
		if (r->provider_class != provider)
			continue;
		for (i = 0; i + 1 < r->nmatch; i += 2) {
			v = r->match[i];
			m = r->match[i + 1];
			if (m == 0 || (word & m) != (v & m))
				continue;
			if (best == NULL || r->probe_score > best->probe_score)
				best = r;
			break;
		}
	}
	if (best != NULL) {
		strlcpy(buf, best->bundle_id, buflen);
		if (score_out != NULL)
			*score_out = best->probe_score;
	}
	sx_sunlock(&iocat_lock);
	return (best != NULL ? 0 : ENOENT);
}

int
iocat_lookup_pci(uint32_t match_word, char *buf, size_t buflen,
    int32_t *score_out)
{
	struct iocat_record *r, *best;
	uint32_t i;

	best = NULL;
	sx_slock(&iocat_lock);
	TAILQ_FOREACH(r, &iocat_list, link) {
		if (r->provider_class != IOCAT_PROVIDER_IOPCIDEVICE)
			continue;
		for (i = 0; i < r->nmatch; i++) {
			if (r->match[i] != match_word)
				continue;
			if (best == NULL || r->probe_score > best->probe_score)
				best = r;
			break;
		}
	}
	if (best != NULL) {
		strlcpy(buf, best->bundle_id, buflen);
		if (score_out != NULL)
			*score_out = best->probe_score;
	}
	sx_sunlock(&iocat_lock);
	return (best != NULL ? 0 : ENOENT);
}

/*
 * Add a device-tree personality (nextbsd-kernel-extensions-graphics#185).
 *
 * The PCI twin of this is iocat_add(). Kept separate rather than widening that
 * one, because struct iocat_add is duplicated into kextd and is an ABI -- see
 * the comment on struct iocat_add_compat.
 */
static int
iocat_add_compat(struct iocat_add_compat *ua)
{
	struct iocat_record *r;
	char *compat;
	size_t sz;
	uint32_t i;
	int error;

	if (ua->ncompat == 0 || ua->ncompat > IOCAT_MAX_MATCH)
		return (EINVAL);
	ua->bundle_id[IOCAT_BUNDLE_ID_MAX - 1] = '\0';
	if (ua->bundle_id[0] == '\0')
		return (EINVAL);

	sz = (size_t)ua->ncompat * IOCAT_COMPAT_MAX;
	compat = malloc(sz, M_IOCAT, M_WAITOK);
	error = copyin((const void *)(uintptr_t)ua->compat, compat, sz);
	if (error != 0) {
		free(compat, M_IOCAT);
		return (error);
	}
	/*
	 * Terminate every slot. copyin gives us whatever userland had; a string
	 * without a NUL would run into the next slot on every later strcmp.
	 */
	for (i = 0; i < ua->ncompat; i++)
		compat[(size_t)i * IOCAT_COMPAT_MAX + IOCAT_COMPAT_MAX - 1] = '\0';

	r = malloc(sizeof(*r), M_IOCAT, M_WAITOK | M_ZERO);
	strlcpy(r->bundle_id, ua->bundle_id, sizeof(r->bundle_id));
	r->provider_class = IOCAT_PROVIDER_IOPLATFORMDEVICE;
	r->probe_score = ua->probe_score;
	r->ncompat = ua->ncompat;
	r->compat = compat;

	sx_xlock(&iocat_lock);
	TAILQ_INSERT_TAIL(&iocat_list, r, link);
	iocat_count++;
	sx_xunlock(&iocat_lock);

	/* Same push-triggers-match as the PCI path: a device may already have
	 * gone unmatched before this personality existed. */
	iocat_schedule_rematch();
	return (0);
}

int
iocat_lookup_compat(const char *want, char *buf, size_t buflen,
    int32_t *score_out)
{
	struct iocat_record *r, *best;
	uint32_t i;

	if (want == NULL || *want == '\0')
		return (ENOENT);

	best = NULL;
	sx_slock(&iocat_lock);
	TAILQ_FOREACH(r, &iocat_list, link) {
		if (r->provider_class != IOCAT_PROVIDER_IOPLATFORMDEVICE)
			continue;
		for (i = 0; i < r->ncompat; i++) {
			if (strcmp(&r->compat[(size_t)i * IOCAT_COMPAT_MAX],
			    want) != 0)
				continue;
			if (best == NULL || r->probe_score > best->probe_score)
				best = r;
			break;
		}
	}
	if (best != NULL) {
		strlcpy(buf, best->bundle_id, buflen);
		if (score_out != NULL)
			*score_out = best->probe_score;
	}
	sx_sunlock(&iocat_lock);
	return (best != NULL ? 0 : ENOENT);
}

/* ---- K3 (#216): device_nomatch -> match -> request a load from userland ----
 *
 * When newbus probes a device no built-in driver claims, it fires the
 * device_nomatch event. We capture the device's PCI id (a quick, non-sleeping
 * ivar read) and defer the rest to a taskqueue — the Phase 0 PoC proved that
 * deferring off the bus lock is deadlock-free, and iocat_lookup_pci() takes a
 * sleepable sx, so it must run in thread context. On a catalogue hit the kernel
 * decides the winner and asks userland (kextd) to load it by bundle id via a
 * devctl notify ("system=IOKIT type=load"). kextd's listener (lands with the
 * K3 userland half) loads the named bundle; kldload then re-probes the waiting
 * device automatically. The kernel decides; userland fetches.
 */
/* What kind of device a work item describes (#185, #329). */
enum iocat_kind {
	IOCAT_KIND_PCI = 0,	/* match_word + class_word */
	IOCAT_KIND_FDT,		/* compat */
	IOCAT_KIND_USB,		/* match_word (product<<16|vendor) + class_word (iface triple) */
	IOCAT_KIND_ACPI,	/* acpi_handle, resolved to _HID/_CID in thread context */
};

struct iocat_match_work {
	STAILQ_ENTRY(iocat_match_work) link;
	enum iocat_kind	kind;
	/*
	 * PCI: 0x<device><vendor> (IOPCIPrimaryMatch). USB: 0x<idProduct><idVendor>.
	 * Passed to kextd with the load request for its log.
	 */
	uint32_t	match_word;
	/*
	 * PCI: Darwin's IOPCIClassMatch word 0x<class><subclass><progif><rev>.
	 * USB: the interface triple 0x00<class><subclass><protocol>.
	 * Tried only when no exact personality claims match_word.
	 */
	uint32_t	class_word;
	char		devname[64];
	/*
	 * Device-tree devices have no match word, they have a compatible
	 * string (#185).
	 */
	char		compat[IOCAT_COMPAT_MAX];
	/*
	 * ACPI: the namespace node. Its _HID and first _CID are read once, in
	 * thread context, into compat/compat2 ("acpi:"-prefixed); every later
	 * re-match compares strings and does not touch the namespace again.
	 */
	void	       *acpi_handle;
	char		compat2[IOCAT_COMPAT_MAX];
	bool		acpi_resolved;
};
static STAILQ_HEAD(, iocat_match_work) iocat_work =
    STAILQ_HEAD_INITIALIZER(iocat_work);
/* Devices that went unmatched (no personality yet). push-triggers-match
 * re-checks these when a personality is added. Both lists use iocat_work_mtx. */
static STAILQ_HEAD(, iocat_match_work) iocat_pending =
    STAILQ_HEAD_INITIALIZER(iocat_pending);
static struct mtx iocat_work_mtx;
static struct task iocat_match_task;
static eventhandler_tag iocat_nomatch_tag;

/*
 * Ask userland (kextd) to load `bundle` for a matched device. The faithful
 * path is a Mach message to HOST_KEXTD_PORT (K3b); without COMPAT_MACH there is
 * no kextd channel, so it's a no-op beyond a verbose log.
 */
static void
iocat_request_load(uint32_t match_word, const char *devname,
    const char *bundle, int32_t score __unused)
{
#ifdef COMPAT_MACH
	int e = iokit_kextd_send(bundle, devname, match_word);

	if (bootverbose)
		printf("iokit: %s (0x%08x) -> request load %s (kextd_send=%d)\n",
		    devname, match_word, bundle, e);
#else
	if (bootverbose)
		printf("iokit: %s (0x%08x) matches %s (no kextd channel)\n",
		    devname, match_word, bundle);
#endif
}

/*
 * Look up whichever kind of work item this is (#185): a device-tree item
 * carries a compatible string, a PCI item a match word. One place to make that
 * decision, so the two consumers below cannot drift apart.
 */
#ifdef DEV_ACPI
/*
 * Read an ACPI node's _HID and first _CID as "acpi:"-prefixed strings (#329).
 * Thread context only: AcpiGetObjectInfo() allocates. Either string may come
 * back empty.
 */
static void
iocat_acpi_ids(ACPI_HANDLE h, char *hid, char *cid, size_t len)
{
	ACPI_DEVICE_INFO *info;

	hid[0] = cid[0] = '\0';
	if (h == NULL || ACPI_FAILURE(AcpiGetObjectInfo(h, &info)))
		return;
	if ((info->Valid & ACPI_VALID_HID) != 0 && info->HardwareId.Length > 1)
		snprintf(hid, len, "%s%s", IOCAT_ACPI_PREFIX,
		    info->HardwareId.String);
	if ((info->Valid & ACPI_VALID_CID) != 0 &&
	    info->CompatibleIdList.Count > 0)
		snprintf(cid, len, "%s%s", IOCAT_ACPI_PREFIX,
		    info->CompatibleIdList.Ids[0].String);
	AcpiOsFree(info);
}

/* Match _HID first, then _CID, against IONameMatch "acpi:" strings. */
static int
iocat_lookup_acpi_ids(const char *hid, const char *cid, char *buf,
    size_t buflen, int32_t *score_out)
{

	if (hid[0] != '\0' &&
	    iocat_lookup_compat(hid, buf, buflen, score_out) == 0)
		return (0);
	if (cid[0] != '\0')
		return (iocat_lookup_compat(cid, buf, buflen, score_out));
	return (ENOENT);
}
#endif

static int
iocat_lookup_work(const struct iocat_match_work *w, char *buf, size_t buflen,
    int32_t *score_out)
{

	switch (w->kind) {
	case IOCAT_KIND_FDT:
		return (iocat_lookup_compat(w->compat, buf, buflen, score_out));
	case IOCAT_KIND_USB:
		/* Darwin precedence: a vendor/product personality, then class. */
		if (iocat_lookup_word(IOCAT_PROVIDER_IOUSBHOSTDEVICE,
		    w->match_word, buf, buflen, score_out) == 0)
			return (0);
		return (iocat_lookup_masked(IOCAT_PROVIDER_IOUSBHOSTINTERFACE,
		    w->class_word, buf, buflen, score_out));
	case IOCAT_KIND_ACPI:
#ifdef DEV_ACPI
		/* Resolved by iocat_acpi_resolve() before any lookup. */
		return (iocat_lookup_acpi_ids(w->compat, w->compat2, buf,
		    buflen, score_out));
#else
		return (ENOENT);
#endif
	case IOCAT_KIND_PCI:
	default:
		if (iocat_lookup_pci(w->match_word, buf, buflen,
		    score_out) == 0)
			return (0);
		return (iocat_lookup_masked(IOCAT_PROVIDER_IOPCICLASS,
		    w->class_word, buf, buflen, score_out));
	}
}

/* Fill in an ACPI item's ids, once, in thread context (#329). */
static void
iocat_acpi_resolve(struct iocat_match_work *w)
{

#ifdef DEV_ACPI
	if (w->kind == IOCAT_KIND_ACPI && !w->acpi_resolved) {
		iocat_acpi_ids(w->acpi_handle, w->compat, w->compat2,
		    sizeof(w->compat));
		w->acpi_resolved = true;
	}
#endif
}

/* Darwin's IOPCIClassMatch word for a PCI device (#329). */
static uint32_t
iocat_pci_class_word(device_t dev)
{

	return (((uint32_t)pci_get_class(dev) << 24) |
	    ((uint32_t)pci_get_subclass(dev) << 16) |
	    ((uint32_t)pci_get_progif(dev) << 8) |
	    (uint32_t)pci_get_revid(dev));
}

/* Remember an unmatched device for push-triggers-match. Takes ownership of w
 * (links it, or frees it if a same-named entry is already pending). */
static void
iocat_remember_pending(struct iocat_match_work *w)
{
	struct iocat_match_work *p;

	mtx_lock(&iocat_work_mtx);
	STAILQ_FOREACH(p, &iocat_pending, link) {
		if (strcmp(p->devname, w->devname) == 0) {
			mtx_unlock(&iocat_work_mtx);
			free(w, M_IOCAT);
			return;
		}
	}
	STAILQ_INSERT_TAIL(&iocat_pending, w, link);
	mtx_unlock(&iocat_work_mtx);
}

/* Re-run the matcher over every pending (unmatched) device — called after a
 * personality is added (push-triggers-match). Splices the list out under the
 * mutex, then does lookups/sends with no lock held (iocat_lookup_pci takes the
 * sleepable iocat_lock; iokit_kextd_send may block). */
static void
iocat_rematch_pending(void)
{
	STAILQ_HEAD(, iocat_match_work) todo = STAILQ_HEAD_INITIALIZER(todo);
	struct iocat_match_work *w;
	char bundle[IOCAT_BUNDLE_ID_MAX];
	int32_t score;

	mtx_lock(&iocat_work_mtx);
	STAILQ_CONCAT(&todo, &iocat_pending);	/* iocat_pending now empty */
	mtx_unlock(&iocat_work_mtx);

	while ((w = STAILQ_FIRST(&todo)) != NULL) {
		STAILQ_REMOVE_HEAD(&todo, link);
		iocat_acpi_resolve(w);
		if (iocat_lookup_work(w, bundle, sizeof(bundle),
		    &score) == 0) {
			iocat_request_load(w->match_word, w->devname, bundle, score);
			free(w, M_IOCAT);
		} else {
			mtx_lock(&iocat_work_mtx);
			STAILQ_INSERT_TAIL(&iocat_pending, w, link);
			mtx_unlock(&iocat_work_mtx);
		}
	}
}

#ifdef FDT
/*
 * Recursively walk an OF subtree looking for present-but-unmatched nodes whose
 * compatible string the catalogue now claims (#185).
 *
 * Recursive because the device tree nests -- ofwbus0 -> simplebus0 -> ... --
 * and the node we care about can be at any depth. Depth is bounded by the
 * device tree itself (a handful of levels on a Pi), and each frame is small.
 */
static void
iocat_scan_of_subtree(device_t bus, int *checked, int *unmatched, int *requested)
{
	device_t *kids;
	const char *compat;
	char bundle[IOCAT_BUNDLE_ID_MAX];
	int32_t score;
	int nkids, i;

	if (device_get_children(bus, &kids, &nkids) != 0)
		return;
	for (i = 0; i < nkids; i++) {
		/* Recurse first: a bus with a driver still has children. */
		iocat_scan_of_subtree(kids[i], checked, unmatched, requested);

		compat = ofw_bus_get_compat(kids[i]);
		if (compat == NULL || *compat == '\0')
			continue;	/* not an OF node we can match */
		(*checked)++;
		if (device_get_driver(kids[i]) != NULL)
			continue;	/* already has a real owner */
		(*unmatched)++;
		if (iocat_lookup_compat(compat, bundle, sizeof(bundle),
		    &score) != 0)
			continue;	/* no personality claims it */
		if (bootverbose)
			printf("iokit: present-scan: %s (%s) present + "
			    "unmatched -> request load %s\n",
			    device_get_nameunit(kids[i]), compat, bundle);
		/* match_word 0: this is an FDT match, not a PCI one. */
		iocat_request_load(0, device_get_nameunit(kids[i]), bundle,
		    score);
		(*requested)++;
	}
	free(kids, M_TEMP);
}
#endif /* FDT */

/*
 * Walk every PCI device currently on the bus; for any with no driver attached
 * whose id the catalogue now claims, ask kextd to load its bundle.
 *
 * iocat_rematch_pending only re-checks devices the device_nomatch eventhandler
 * captured into iocat_pending. On real hardware that boot capture is unreliable
 * — a T460s comes up with the I219-LM (0x156f) and 8260 Wi-Fi (0x24f3) present
 * and driver-less, yet neither was ever queued, so push-triggers-match had
 * nothing to match and they never autoloaded (while injecting a load request by
 * hand loads them fine — the load path works, only the trigger was missed).
 * This active scan of the live device tree is the authoritative trigger: it does
 * not depend on the event firing, so it autoloads whatever is actually present.
 * Idempotent — devices already bound to a real driver are skipped; a display
 * device claimed by vgapci(4) is looked THROUGH to its (possibly still-unbound)
 * drmn child, so the GPU's DRM kext is requested even though vgapci already owns
 * the PCI function (#64).
 */

/*
 * True if a vgapci device already has an *attached* drmn (DRM/KMS) child.
 * vga_pci_attach ALWAYS pre-creates a drmn child in DS_NOTPRESENT, so mere
 * presence is not enough — only an *attached* drmn means KMS is actually bound,
 * and only then is there nothing for the present-scan to do.
 */
static bool
iocat_drmn_bound(device_t vga)
{
	device_t *kids = NULL;
	int nkids = 0, k;
	bool bound = false;

	if (device_get_children(vga, &kids, &nkids) != 0)
		return (false);
	for (k = 0; k < nkids; k++) {
		devclass_t dc = device_get_devclass(kids[k]);
		const char *n = dc != NULL ? devclass_get_name(dc) : NULL;

		if (n != NULL && strcmp(n, "drmn") == 0 &&
		    device_is_attached(kids[k])) {
			bound = true;
			break;
		}
	}
	free(kids, M_TEMP);
	return (bound);
}

/*
 * True if this virtio transport still has base virtio_gpu(4) (devclass "vtgpu")
 * attached to it.
 *
 * The virtio-gpu case is the mirror image of the vgapci one above, and it needs
 * its own test because the shadowing driver is not a bus — it is a complete,
 * working driver. Base virtio_gpu(4) is compiled into GENERIC, probes long
 * before any kext exists, and becomes the vt(4) console backend; on arm64 it is
 * the ONLY console, because neither qemu `virt` nor Apple Virtualization.framework
 * provides an EFI GOP for vt_efifb. So it cannot simply be removed, and a stock
 * boot legitimately reaches userland with the device already bound and no
 * device_nomatch ever fired for it. Nothing would ask kextd for the DRM bundle.
 *
 * An *attached* vtgpu child therefore means "console yes, KMS no, kext wanted".
 * Once VirtIOGraphics has taken the device the child's devclass is
 * virtio_gpu_drm instead, so this returns false and the scan skips it — which is
 * what makes the scan idempotent across repeated runs.
 */
static bool
iocat_vtgpu_shadowed(device_t transport)
{
	device_t *kids = NULL;
	int nkids = 0, k;
	bool shadowed = false;

	if (device_get_children(transport, &kids, &nkids) != 0)
		return (false);
	for (k = 0; k < nkids; k++) {
		devclass_t dc = device_get_devclass(kids[k]);
		const char *n = dc != NULL ? devclass_get_name(dc) : NULL;

		if (n != NULL && strcmp(n, "vtgpu") == 0 &&
		    device_is_attached(kids[k])) {
			shadowed = true;
			break;
		}
	}
	free(kids, M_TEMP);
	return (shadowed);
}

static void
iocat_rematch_present(void)
{
	devclass_t pci_dc;
	device_t *buses = NULL;
	int nbuses = 0, i;
	char bundle[IOCAT_BUNDLE_ID_MAX];
	int32_t score;
	int checked = 0, unmatched = 0, requested = 0;

	pci_dc = devclass_find("pci");
	if (pci_dc == NULL)
		return;
	if (devclass_get_devices(pci_dc, &buses, &nbuses) != 0)
		return;
	for (i = 0; i < nbuses; i++) {
		device_t *kids = NULL;
		int nkids = 0, j;

		if (device_get_children(buses[i], &kids, &nkids) != 0)
			continue;
		for (j = 0; j < nkids; j++) {
			device_t child = kids[j];
			uint32_t mw;

			checked++;
			/*
			 * Skip devices already bound to a real driver — EXCEPT
			 * two shapes of GPU where the bound driver is standing
			 * in for the KMS driver we actually want:
			 *
			 * vgapci(4): the real GPU driver (i915kms/amdgpu/
			 * radeonkms) attaches as vgapci's CHILD
			 * (DRIVER_MODULE(.., vgapci)), so a vgapci-claimed GPU
			 * is not yet KMS-driven. Look THROUGH vgapci to its drmn
			 * child; if no *attached* drmn yet, request the GPU
			 * kext. kextd kldloads it and newbus BUS_DRIVER_ADDED
			 * then attaches drmn automatically — no manual reprobe
			 * (that would race the built-in attach). (#64)
			 *
			 * virtio_pci: base virtio_gpu(4) has the device and is
			 * the vt(4) console (the only one arm64 has — no EFI GOP
			 * under qemu `virt` or Virtualization.framework). It is a
			 * real driver, so device_nomatch never fires and nothing
			 * else would ever ask for the DRM bundle. Request it;
			 * VirtIOGraphics does its own atomic detach/re-probe on
			 * load, because unlike the vgapci case there is no
			 * DS_NOTPRESENT child for BUS_DRIVER_ADDED to pick up.
			 */
			if (device_get_driver(child) != NULL) {
				devclass_t dc = device_get_devclass(child);
				const char *dn = dc != NULL ?
				    devclass_get_name(dc) : NULL;

				if (dn == NULL)
					continue;	/* real owner — skip */
				if (strcmp(dn, "vgapci") == 0 &&
				    pci_get_class(child) == PCIC_DISPLAY) {
					if (iocat_drmn_bound(child))
						continue; /* DRM already bound */
					/* vgapci-shadowed GPU — fall through. */
				} else if (strcmp(dn, "virtio_pci") == 0) {
					if (!iocat_vtgpu_shadowed(child))
						continue; /* not a shadowed gpu */
					/* base-shadowed virtio-gpu — fall through. */
				} else
					continue;	/* real owner — skip */
			}
			unmatched++;
			/* IOPCIPrimaryMatch form: device<<16 | vendor. */
			mw = ((uint32_t)pci_get_device(child) << 16) |
			    pci_get_vendor(child);
			if (iocat_lookup_pci(mw, bundle, sizeof(bundle),
			    &score) != 0 &&
			    (device_get_driver(child) != NULL ||
			    iocat_lookup_masked(IOCAT_PROVIDER_IOPCICLASS,
			    iocat_pci_class_word(child), bundle, sizeof(bundle),
			    &score) != 0))
				continue;	/* no personality claims it */
			/*
			 * The class fallback (#329) is only for a device with no
			 * driver at all: a vgapci- or virtio-shadowed GPU is
			 * handed to its DRM kext by id, never by class.
			 */
			if (bootverbose)
				printf("iokit: present-scan: %s (0x%08x) present + "
				    "unmatched -> request load %s\n",
				    device_get_nameunit(child), mw, bundle);
			iocat_request_load(mw, device_get_nameunit(child),
			    bundle, score);
			requested++;
		}
		free(kids, M_TEMP);
	}
	free(buses, M_TEMP);

#ifdef FDT
	/*
	 * The same scan for device-tree devices (#185).
	 *
	 * Everything the comment above says about the PCI capture being
	 * unreliable applies here too, and an OF device cannot be found by the
	 * PCI walk at all -- it is on ofwbus/simplebus, not under a pci bus. A
	 * Pi 5 shows the case this exists for:
	 *
	 *	ofwbus0: <firmwarekms> irq 12 compat raspberrypi,rpi-firmware-kms-2712 (no driver attached)
	 *
	 * present, unmatched, and invisible to the loop above.
	 *
	 * Walks every devclass rather than a named bus, because OF devices hang
	 * off ofwbus, simplebus, and any number of nested simplebuses; asking
	 * for a compatible string and skipping devices that do not have one is
	 * both simpler and more complete than enumerating bus names.
	 */
	{
		device_t *devs;
		int ndevs, i;

		if (devclass_get_devices(devclass_find("ofwbus"), &devs,
		    &ndevs) == 0) {
			for (i = 0; i < ndevs; i++)
				iocat_scan_of_subtree(devs[i], &checked,
				    &unmatched, &requested);
			free(devs, M_TEMP);
		}
	}
#endif
#ifdef DEV_ACPI
	/*
	 * ACPI devices (#329): acpi(4)'s children that no driver claimed, matched
	 * by _HID/_CID. Like the device-tree walk, this is the authoritative
	 * trigger for nodes the boot device_nomatch capture missed.
	 */
	{
		device_t *acpis, *kids;
		int nacpi, nkids, i, j;

		if (devclass_get_devices(devclass_find("acpi"), &acpis,
		    &nacpi) == 0) {
			for (i = 0; i < nacpi; i++) {
				if (device_get_children(acpis[i], &kids,
				    &nkids) != 0)
					continue;
				for (j = 0; j < nkids; j++) {
					ACPI_HANDLE h;

					if (device_get_driver(kids[j]) != NULL)
						continue;
					h = acpi_get_handle(kids[j]);
					if (h == NULL)
						continue;
					checked++;
					unmatched++;
					{
						char hid[IOCAT_COMPAT_MAX];
						char cid[IOCAT_COMPAT_MAX];

						iocat_acpi_ids(h, hid, cid,
						    sizeof(hid));
						if (iocat_lookup_acpi_ids(hid, cid,
						    bundle, sizeof(bundle),
						    &score) != 0)
							continue;
					}
					if (bootverbose)
						printf("iokit: present-scan: %s "
						    "(acpi) present + unmatched "
						    "-> request load %s\n",
						    device_get_nameunit(kids[j]),
						    bundle);
					iocat_request_load(0,
					    device_get_nameunit(kids[j]),
					    bundle, score);
					requested++;
				}
				free(kids, M_TEMP);
			}
			free(acpis, M_TEMP);
		}
	}
#endif
	/* One summary line, under bootverbose, so a verbose boot can confirm the
	 * scan ran even when every present device is already attached (qemu) and
	 * no per-device line is printed. Quiet on a normal boot so the autoload
	 * scan doesn't paper over the console / getty login prompt; the data is
	 * still in dmesg on a verbose boot. (CI verifies autoload via the
	 * EM-AUTOLOAD / IOCATALOGUE / kextstat markers, not this line.) */
	if (bootverbose)
		printf("iokit: present-scan: checked %d pci devices, %d unmatched, "
		    "%d load(s) requested\n", checked, unmatched, requested);
}

static void
iocat_present_taskfn(void *ctx __unused, int pending __unused)
{
	iocat_rematch_pending();
	iocat_rematch_present();
}

static void
iocat_match_taskfn(void *ctx __unused, int pending __unused)
{
	struct iocat_match_work *w;
	char bundle[IOCAT_BUNDLE_ID_MAX];
	int32_t score;

	for (;;) {
		mtx_lock(&iocat_work_mtx);
		w = STAILQ_FIRST(&iocat_work);
		if (w != NULL)
			STAILQ_REMOVE_HEAD(&iocat_work, link);
		mtx_unlock(&iocat_work_mtx);
		if (w == NULL)
			break;

		iocat_acpi_resolve(w);
		if (iocat_lookup_work(w, bundle, sizeof(bundle),
		    &score) == 0) {
			/* Driver known now — ask kextd to load it. */
			iocat_request_load(w->match_word, w->devname, bundle, score);
			free(w, M_IOCAT);
		} else {
			/* No personality yet; a later kextd push will re-match it. */
			iocat_remember_pending(w);
		}
	}
}

static void
iocat_device_nomatch(void *arg __unused, device_t dev)
{
	struct iocat_match_work *w;
	device_t parent;
	const char *nu;

	parent = device_get_parent(dev);
	if (parent == NULL || device_get_name(parent) == NULL)
		return;

	if (strcmp(device_get_name(parent), "pci") == 0) {
		/* pci_get_* reads ivars valid on a pci child. */
		w = malloc(sizeof(*w), M_IOCAT, M_NOWAIT | M_ZERO);
		if (w == NULL)
			return;
		w->kind = IOCAT_KIND_PCI;
		/* IOPCIPrimaryMatch: device in the high 16 bits, vendor low. */
		w->match_word = ((uint32_t)pci_get_device(dev) << 16) |
		    pci_get_vendor(dev);
		w->class_word = iocat_pci_class_word(dev);
	} else if (strcmp(device_get_name(parent), "uhub") == 0) {
		struct usb_attach_arg *uaa;

		/*
		 * A USB interface no driver claimed (#329). During its probe
		 * the child's ivars are the usb_attach_arg (usb_device.c's
		 * usb_probe_and_attach_sub), and device_nomatch fires inside
		 * that probe, so reading them here is as safe as the pci_get_*
		 * reads above. USB reuses one unnamed temporary child per
		 * device, so the item gets a stable name of its own.
		 */
		uaa = device_get_ivars(dev);
		if (uaa == NULL || uaa->usb_mode != USB_MODE_HOST)
			return;
		w = malloc(sizeof(*w), M_IOCAT, M_NOWAIT | M_ZERO);
		if (w == NULL)
			return;
		w->kind = IOCAT_KIND_USB;
		w->match_word = ((uint32_t)uaa->info.idProduct << 16) |
		    uaa->info.idVendor;
		w->class_word = ((uint32_t)uaa->info.bInterfaceClass << 16) |
		    ((uint32_t)uaa->info.bInterfaceSubClass << 8) |
		    uaa->info.bInterfaceProtocol;
		snprintf(w->devname, sizeof(w->devname), "%s.p%u.i%u",
		    device_get_nameunit(parent), uaa->port,
		    uaa->info.bIfaceIndex);
		goto queue;
#ifdef DEV_ACPI
	} else if (strcmp(device_get_name(parent), "acpi") == 0) {
		ACPI_HANDLE h;

		/*
		 * An ACPI node no driver claimed (#329). Reading _HID/_CID
		 * allocates, so only the handle is captured here; the strings
		 * are read in the taskqueue.
		 */
		h = acpi_get_handle(dev);
		if (h == NULL)
			return;
		w = malloc(sizeof(*w), M_IOCAT, M_NOWAIT | M_ZERO);
		if (w == NULL)
			return;
		w->kind = IOCAT_KIND_ACPI;
		w->acpi_handle = h;
		/* Unattached ACPI children are often unnamed; key on the node. */
		snprintf(w->devname, sizeof(w->devname), "acpi:%p", h);
		goto queue;
#endif
	} else {
#ifdef FDT
		const char *compat;

		/*
		 * A device-tree node (#185). ofw_bus_get_compat() reads an
		 * ivar and neither sleeps nor allocates, so it is safe in this
		 * handler for the same reason the pci_get_* calls above are.
		 *
		 * It returns only the FIRST compatible string. A node may list
		 * several, most specific first, and a personality naming a
		 * later (more generic) one will not match here. The specific
		 * string is what a driver personality should name, so this is
		 * the right default -- but it is a real limit, not an
		 * oversight.
		 */
		compat = ofw_bus_get_compat(dev);
		if (compat == NULL || *compat == '\0')
			return;
		w = malloc(sizeof(*w), M_IOCAT, M_NOWAIT | M_ZERO);
		if (w == NULL)
			return;
		w->kind = IOCAT_KIND_FDT;
		strlcpy(w->compat, compat, sizeof(w->compat));
#else
		return;
#endif
	}
	nu = device_get_nameunit(dev);
	strlcpy(w->devname, nu != NULL ? nu : "?", sizeof(w->devname));

queue:
	mtx_lock(&iocat_work_mtx);
	STAILQ_INSERT_TAIL(&iocat_work, w, link);
	mtx_unlock(&iocat_work_mtx);
	taskqueue_enqueue(taskqueue_thread, &iocat_match_task);
}

/* ---- /dev/iocatalogue: ioctl ingestion from userland (kextd) ---- */

static d_ioctl_t iocat_ioctl;
static struct cdev *iocat_dev;
static struct cdevsw iocat_cdevsw = {
	.d_version = D_VERSION,
	.d_ioctl   = iocat_ioctl,
	.d_name    = "iocatalogue",
};

static int
iocat_ioctl(struct cdev *dev __unused, u_long cmd, caddr_t data,
    int fflag __unused, struct thread *td __unused)
{
	switch (cmd) {
	case IOCATIOCADD:
		return (iocat_add((struct iocat_add *)data));
	case IOCATIOCFLUSH:
		iocat_flush();
		return (0);
	case IOCATIOCLOOKUP: {
		struct iocat_lookup *lu = (struct iocat_lookup *)data;

		lu->bundle_id[0] = '\0';
		lu->score = 0;
		return (iocat_lookup_pci(lu->match, lu->bundle_id,
		    sizeof(lu->bundle_id), &lu->score));
	}
	case IOCATIOCADDCOMPAT:		/* device-tree personality (#185) */
		return (iocat_add_compat((struct iocat_add_compat *)data));
	case IOCATIOCLOOKUPCOMPAT: {
		struct iocat_lookup_compat *lu =
		    (struct iocat_lookup_compat *)data;

		lu->compat[IOCAT_COMPAT_MAX - 1] = '\0';
		lu->bundle_id[0] = '\0';
		lu->score = 0;
		return (iocat_lookup_compat(lu->compat, lu->bundle_id,
		    sizeof(lu->bundle_id), &lu->score));
	}
	case IOCATIOCTESTSEND: {	/* K3b PoC: lookup + kernel->kextd Mach send */
#ifdef COMPAT_MACH
		uint32_t mw = *(uint32_t *)data;
		char bundle[IOCAT_BUNDLE_ID_MAX];
		int32_t score;

		if (iocat_lookup_pci(mw, bundle, sizeof(bundle), &score) != 0)
			return (ENOENT);
		return (iokit_kextd_send(bundle, "iocat-test", mw));
#else
		return (ENOSYS);
#endif
	}
	default:
		return (ENOTTY);
	}
}

/* ---- hw.iokit.catalogue: read-only text dump (debug + hwregd transition) ---- */

static int
iocat_sysctl_dump(SYSCTL_HANDLER_ARGS)
{
	struct iocat_record *r;
	struct sbuf sb;
	uint32_t i;
	int error;

	/* Build into auto-extending memory under the lock, copy out after. */
	sbuf_new(&sb, NULL, 256, SBUF_AUTOEXTEND);
	sx_slock(&iocat_lock);
	TAILQ_FOREACH(r, &iocat_list, link) {
		sbuf_printf(&sb, "%s provider=%u score=%d match=",
		    r->bundle_id, r->provider_class, r->probe_score);
		for (i = 0; i < r->nmatch; i++)
			sbuf_printf(&sb, "%s0x%08x", i ? "," : "", r->match[i]);
		sbuf_cat(&sb, "\n");
	}
	sx_sunlock(&iocat_lock);
	error = sbuf_finish(&sb);
	if (error == 0)
		error = SYSCTL_OUT(req, sbuf_data(&sb), sbuf_len(&sb) + 1);
	sbuf_delete(&sb);
	return (error);
}

/*
 * The hw.iokit node is shared: K1's iokit_registry.c hangs hw.iokit.registry
 * off it (via SYSCTL_DECL(_hw_iokit) in <sys/iocatalogue.h>). Hence non-static.
 */
SYSCTL_NODE(_hw, OID_AUTO, iokit, CTLFLAG_RD | CTLFLAG_MPSAFE, 0,
    "NextBSD in-kernel IOKit");
SYSCTL_PROC(_hw_iokit, OID_AUTO, catalogue,
    CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_MPSAFE, NULL, 0,
    iocat_sysctl_dump, "A", "IOKit catalogue (registered driver personalities)");
SYSCTL_UINT(_hw_iokit, OID_AUTO, catalogue_count, CTLFLAG_RD,
    &iocat_count, 0, "number of registered personalities");

static int
iocat_modevent(module_t mod __unused, int type, void *data __unused)
{
	struct iocat_match_work *w;

	switch (type) {
	case MOD_LOAD:
		mtx_init(&iocat_work_mtx, "iocat_work", NULL, MTX_DEF);
		TASK_INIT(&iocat_match_task, 0, iocat_match_taskfn, NULL);
		TIMEOUT_TASK_INIT(taskqueue_thread, &iocat_present_task, 0,
		    iocat_present_taskfn, NULL);
		iocat_dev = make_dev(&iocat_cdevsw, 0, UID_ROOT, GID_WHEEL,
		    0600, "iocatalogue");
		if (iocat_dev == NULL) {
			mtx_destroy(&iocat_work_mtx);
			return (ENXIO);
		}
		/* Registered before SI_SUB_CONFIGURE, so it sees boot nomatches. */
		iocat_nomatch_tag = EVENTHANDLER_REGISTER(device_nomatch,
		    iocat_device_nomatch, NULL, EVENTHANDLER_PRI_ANY);
		return (0);
	case MOD_UNLOAD:
		if (iocat_nomatch_tag != NULL)
			EVENTHANDLER_DEREGISTER(device_nomatch, iocat_nomatch_tag);
		taskqueue_drain(taskqueue_thread, &iocat_match_task);
		taskqueue_drain_timeout(taskqueue_thread, &iocat_present_task);
		if (iocat_dev != NULL)
			destroy_dev(iocat_dev);
		while ((w = STAILQ_FIRST(&iocat_work)) != NULL) {
			STAILQ_REMOVE_HEAD(&iocat_work, link);
			free(w, M_IOCAT);
		}
		while ((w = STAILQ_FIRST(&iocat_pending)) != NULL) {
			STAILQ_REMOVE_HEAD(&iocat_pending, link);
			free(w, M_IOCAT);
		}
		mtx_destroy(&iocat_work_mtx);
		iocat_flush();
		return (0);
	default:
		return (EOPNOTSUPP);
	}
}

static moduledata_t iocat_mod = { "iocatalogue", iocat_modevent, NULL };
DECLARE_MODULE(iocatalogue, iocat_mod, SI_SUB_DRIVERS, SI_ORDER_ANY);
MODULE_VERSION(iocatalogue, 1);
