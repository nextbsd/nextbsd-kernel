/*
 * NextBSD in-kernel IORegistry (K1, nextbsd#214).
 *
 * A read-only, on-demand view of the live newbus device_t tree, presented as
 * IOKit-style nodes + property bags and queryable from userland via ioctl on
 * /dev/ioregistry (and dumped as text via hw.iokit.registry). This is the
 * kernel-served replacement for hwregd's tree: where hwregd caches hw.bus
 * sysctls + /dev/devctl in userland, this walks the real device_t topology.
 *
 * There is NO shadow tree. Every query walks the live newbus tree (safe in the
 * ioctl/sysctl process context). The only persistent state is a small,
 * sx-guarded device_t -> uint64 id map so userland gets stable node ids: ids
 * are minted lazily during a walk, maintained by the device_attach/device_detach
 * eventhandlers, and an id LINGERS (marked detached) after its device goes away
 * so an in-flight userland iterator never dangles (mirrors hwregd's
 * linger-until-unreferenced semantics). At detach the departing device's name,
 * class, driver, path and description are snapshotted (#334), so a lingering
 * id still answers IOREGIOCNODE / IOREGIOCPROPS with what it was, as a
 * terminated IOService stays readable on Darwin.
 *
 * Virtual entries (#333): an in-kernel producer can also publish entries that
 * have no device_t (ioreg_publish_virtual / ioreg_retire_virtual, declared in
 * sys/ioregistry.h). These are the one exception to "no shadow tree": the
 * registry keeps a snapshot for each (struct ioreg_virt) and lists it next to
 * the newbus children of its parent. Entries with no driver parent hang off a
 * synthetic IOResources node under the root, as on Darwin.
 *
 * See sys/sys/ioregistry.h and the design in
 * pkgdemon.github.io/nextbsd-inkernel-iokit-feasibility.html.
 */

#include "opt_compat_mach.h"

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
#include <sys/nv.h>
#include <sys/ioregistry.h>
#include <sys/iocatalogue.h>		/* SYSCTL_DECL(_hw_iokit) */

#include <dev/pci/pcivar.h>

#ifdef COMPAT_MACH
/*
 * K-PR2 (#225): the Mach half of the device-event notification channel lives in
 * compat/mach/iokit_notify.c. Declared here (not via <sys/mach/iokit_notify.h>)
 * so this standard file pulls in no Mach headers — exactly how iokit_catalogue.c
 * reaches iokit_kextd_send. The kernel-side port is opaque (void *) here; values
 * mirror the IOREG_EVENT_* / IOKIT_NOTIFY_EVENT_MSGID in those headers. */
extern int iokit_notify_copyin_port(uint32_t port_name, void **out);
extern int iokit_notify_port_dead(void *port);
extern void *iokit_notify_copy_port(void *port);
extern void iokit_notify_release_port(void *port);
extern int iokit_notify_send(void *port_ref, uint32_t kind, uint64_t id,
    const char *name, const char *classname, uint32_t pci_vendor,
    uint32_t pci_device);
extern int iokit_notify_send_interest(void *port_ref, uint32_t message,
    uint64_t id, uint64_t arg0, uint64_t arg1, const char *devnode);

/* Darwin's kIOMessageServiceIsTerminated; == IOKIT_NOTIFY_MSG_* there. */
#define	IOREG_MSG_SERVICE_IS_TERMINATED	0xe0000010
#endif

static MALLOC_DEFINE(M_IOREG, "ioregistry", "in-kernel IOKit registry id map");

/* Bound recursion depth so a pathological tree can't blow the kernel stack.
 * Real newbus trees are <10 deep; ioreg_build_path() caps at 32 likewise. */
#define	IOREG_MAX_DEPTH	64

/*
 * The stable id map. A NEWBUS entry lives as long as its device, then lingers
 * (state == IOREG_STATE_DETACHED) so userland ids don't dangle. At detach its
 * device_t is dropped and `virt` takes a snapshot of the departed node (#334),
 * which IOREGIOCNODE / IOREGIOCPROPS then serve.
 *
 * A VIRTUAL entry (#333) has no device_t; `virt` holds its snapshot. Live
 * virtual entries are on ioreg_virt_live (so a children/lookup/dump walk scans
 * only virtual entries, not the whole map).
 *
 * Every lingering entry, newbus or virtual, is on ioreg_linger, oldest first.
 * Producers such as uinput can churn (one entry per Bluetooth reconnect), and
 * so can hotplug, so lingerers are freed oldest-first past IOREG_LINGER_MAX; a
 * freed id then returns ENOENT. Ids are never reused.
 *
 * Everything here, including each snapshot and its property bag, is guarded
 * by ioreg_lock.
 */
#define	IOREG_KIND_NEWBUS	0
#define	IOREG_KIND_VIRTUAL	1

#define	IOREG_LINGER_MAX	256

/* Darwin's IOResources: the default parent of driverless entries. Its path is
 * fixed because the root bus is always root0. */
#define	IOREG_RESOURCES_NAME	"IOResources"
#define	IOREG_RESOURCES_PATH	"/root0/IOResources"

struct ioreg_virt {
	struct ioreg_node node;		/* snapshot: parent_id, name, class,
					   driver (newbus), path, pci_* */
	nvlist_t	*props;		/* extra keys, owned; may be NULL */
	bool		permanent;	/* IOResources: never retired */
};

struct ioreg_ent {
	LIST_ENTRY(ioreg_ent) link;
	TAILQ_ENTRY(ioreg_ent) vlink;	/* live virtual list or linger list */
	device_t	dev;		/* NEWBUS: NULL once detached */
	struct ioreg_virt *virt;	/* VIRTUAL, or departed NEWBUS: snapshot */
	uint64_t	id;		/* stable, never reused */
	int		state;		/* IOREG_STATE_* */
	int		kind;		/* IOREG_KIND_* */
};
static LIST_HEAD(, ioreg_ent) ioreg_map = LIST_HEAD_INITIALIZER(ioreg_map);
static TAILQ_HEAD(, ioreg_ent) ioreg_virt_live =
    TAILQ_HEAD_INITIALIZER(ioreg_virt_live);
static TAILQ_HEAD(, ioreg_ent) ioreg_linger =
    TAILQ_HEAD_INITIALIZER(ioreg_linger);
static u_int ioreg_nlinger;
static struct ioreg_ent *ioreg_resources;	/* created on first use */
static struct sx ioreg_lock;
SX_SYSINIT(ioreg_lock, &ioreg_lock, "ioregistry");
static uint64_t ioreg_next_id = 1;	/* 0 is reserved as "invalid" */

/*
 * K-PR2 (#225): the device-event watch registry. A userland client registers a
 * Mach notify port (IOREGIOCWATCH) with a flat match criteria struct and an
 * event mask; the kernel pushes an ioreg_event_msg to that port from the
 * device_attach / device_detach eventhandlers for each matching event.
 *
 * `port` is an opaque retained send right (an ipc_port_t under COMPAT_MACH; the
 * Mach details live in compat/mach/iokit_notify.c). `criteria` is the flat
 * by-value match struct (#218; zero-as-wildcard, all-zero == match all). The
 * list is guarded by its own sx (ioreg_watch_lock) — separate from ioreg_lock so
 * emission never contends the id map — but the Mach send is NEVER done while
 * holding it (the send is performed on a snapshot after the lock is dropped;
 * see ioreg_emit_event).
 *
 * An interest watch (#338, IOREGIOCINTEREST) shares the list: `target` is the
 * one entry it follows and `mask` is 0, so the match/terminate pass never
 * selects it. It receives ioreg_message_device() messages for `target`, and
 * kIOMessageServiceIsTerminated when `target` departs, after which it is
 * dropped. Match watches have target == 0.
 */
struct ioreg_watch {
	TAILQ_ENTRY(ioreg_watch) link;
	struct ioreg_criteria criteria;	/* flat match criteria (by value) */
	uint32_t	mask;		/* OR of IOREG_EVENT_* to deliver */
	void		*port;		/* opaque retained send right */
	uint64_t	target;		/* interest watch: entry id; else 0 */
};
static TAILQ_HEAD(, ioreg_watch) ioreg_watches =
    TAILQ_HEAD_INITIALIZER(ioreg_watches);
static struct sx ioreg_watch_lock;
SX_SYSINIT(ioreg_watch_lock, &ioreg_watch_lock, "ioregwatch");
static u_int ioreg_watch_count;		/* # registered watches (debug) */
static u_int ioreg_interest_count;	/* # of those that are interest */

/* ---- id map helpers (all callers hold ioreg_lock) ---- */

static struct ioreg_ent *
ioreg_find_dev_locked(device_t dev)
{
	struct ioreg_ent *e;

	sx_assert(&ioreg_lock, SX_LOCKED);
	LIST_FOREACH(e, &ioreg_map, link)
		if (e->kind == IOREG_KIND_NEWBUS && e->dev == dev)
			return (e);
	return (NULL);
}

static struct ioreg_ent *
ioreg_find_id_locked(uint64_t id)
{
	struct ioreg_ent *e;

	sx_assert(&ioreg_lock, SX_LOCKED);
	if (id == 0)
		return (NULL);
	LIST_FOREACH(e, &ioreg_map, link)
		if (e->id == id)
			return (e);
	return (NULL);
}

/*
 * Return the stable id for dev, minting one (and recording it live) on first
 * sighting. Caller holds the xlock. dev must be non-NULL.
 */
static uint64_t
ioreg_id_for_locked(device_t dev)
{
	struct ioreg_ent *e;

	sx_assert(&ioreg_lock, SX_XLOCKED);
	e = ioreg_find_dev_locked(dev);
	if (e != NULL) {
		/* A reused device_t (re-attach into the same slot) is live again. */
		e->state = IOREG_STATE_LIVE;
		return (e->id);
	}
	e = malloc(sizeof(*e), M_IOREG, M_WAITOK | M_ZERO);
	e->dev = dev;
	e->id = ioreg_next_id++;
	e->state = IOREG_STATE_LIVE;
	e->kind = IOREG_KIND_NEWBUS;
	LIST_INSERT_HEAD(&ioreg_map, e, link);
	return (e->id);
}

/*
 * The root device_t: root0, or (should the devclass be missing) the top of
 * any live newbus entry's ancestry. Virtual and detached entries have no
 * device_t, so the fallback skips them.
 */
static device_t
ioreg_root_dev(void)
{
	struct ioreg_ent *e;
	device_t d;

	d = devclass_get_device(devclass_find("root"), 0);
	if (d != NULL)
		return (d);
	sx_xlock(&ioreg_lock);
	LIST_FOREACH(e, &ioreg_map, link) {
		if (e->kind == IOREG_KIND_NEWBUS && e->dev != NULL) {
			d = e->dev;
			break;
		}
	}
	sx_xunlock(&ioreg_lock);
	if (d == NULL)
		return (NULL);
	for (; device_get_parent(d) != NULL; d = device_get_parent(d))
		;
	return (d);
}

/* ---- virtual entries (#333; all helpers run under the ioreg_lock xlock) ---- */

/*
 * IOResources may be created before root0 exists (a producer's early SYSINIT
 * can race the root bus module, both at SI_SUB_DRIVERS/SI_ORDER_FIRST). It is
 * then parented on first query, once root0 is there.
 */
static void
ioreg_resources_link_locked(void)
{
	device_t root;

	sx_assert(&ioreg_lock, SX_XLOCKED);
	if (ioreg_resources == NULL ||
	    ioreg_resources->virt->node.parent_id != 0)
		return;
	root = devclass_get_device(devclass_find("root"), 0);
	if (root != NULL)
		ioreg_resources->virt->node.parent_id =
		    ioreg_id_for_locked(root);
}

/* Fill a node descriptor from an entry's snapshot (virtual, or departed
 * newbus). */
static void
ioreg_virt_fill_node_locked(struct ioreg_ent *e, struct ioreg_node *n)
{
	sx_assert(&ioreg_lock, SX_XLOCKED);
	if (e == ioreg_resources)
		ioreg_resources_link_locked();
	*n = e->virt->node;
	n->id = e->id;
	n->state = e->state;
	n->devstate = (e->state == IOREG_STATE_LIVE) ? DS_ATTACHED :
	    DS_NOTPRESENT;
}

/*
 * Return the ids of the live virtual children of `pid` as an M_IOREG array
 * (NULL when there are none; the caller frees it), count in *np.
 */
static uint64_t *
ioreg_virt_children_locked(uint64_t pid, int *np)
{
	struct ioreg_ent *e;
	uint64_t *ids;
	int n, i;

	sx_assert(&ioreg_lock, SX_XLOCKED);
	*np = 0;
	if (TAILQ_EMPTY(&ioreg_virt_live))
		return (NULL);
	ioreg_resources_link_locked();
	n = 0;
	TAILQ_FOREACH(e, &ioreg_virt_live, vlink)
		if (e->virt->node.parent_id == pid)
			n++;
	if (n == 0)
		return (NULL);
	ids = malloc((size_t)n * sizeof(uint64_t), M_IOREG, M_WAITOK);
	i = 0;
	TAILQ_FOREACH(e, &ioreg_virt_live, vlink)
		if (e->virt->node.parent_id == pid && i < n)
			ids[i++] = e->id;
	*np = i;
	return (ids);
}

/*
 * Insert a live virtual entry. `props` (may be NULL) is owned by the entry
 * from here on. The path is the parent's path plus "/name".
 */
static struct ioreg_ent *
ioreg_virt_insert_locked(uint64_t pid, const char *ppath, const char *name,
    const char *classname, nvlist_t *props, bool permanent)
{
	struct ioreg_ent *e;
	struct ioreg_node *n;

	sx_assert(&ioreg_lock, SX_XLOCKED);
	e = malloc(sizeof(*e), M_IOREG, M_WAITOK | M_ZERO);
	e->virt = malloc(sizeof(*e->virt), M_IOREG, M_WAITOK | M_ZERO);
	e->id = ioreg_next_id++;
	e->state = IOREG_STATE_LIVE;
	e->kind = IOREG_KIND_VIRTUAL;
	e->virt->props = props;
	e->virt->permanent = permanent;
	n = &e->virt->node;
	n->parent_id = pid;
	strlcpy(n->name, name, sizeof(n->name));
	strlcpy(n->classname, classname, sizeof(n->classname));
	if (strcmp(ppath, "/") == 0)
		ppath = "";
	snprintf(n->path, sizeof(n->path), "%s/%s", ppath, name);
	LIST_INSERT_HEAD(&ioreg_map, e, link);
	TAILQ_INSERT_TAIL(&ioreg_virt_live, e, vlink);
	return (e);
}

/* Free an entry (and its snapshot, if any) already off the map and lists. */
static void
ioreg_ent_free(struct ioreg_ent *e)
{
	if (e->virt != NULL) {
		if (e->virt->props != NULL)
			nvlist_destroy(e->virt->props);
		free(e->virt, M_IOREG);
	}
	free(e, M_IOREG);
}

/*
 * Put a just-departed entry (already marked detached, snapshot in place) on
 * the linger list, then age out the oldest lingerers so churn can't grow the
 * map.
 */
static void
ioreg_linger_locked(struct ioreg_ent *e)
{
	struct ioreg_ent *old;

	sx_assert(&ioreg_lock, SX_XLOCKED);
	TAILQ_INSERT_TAIL(&ioreg_linger, e, vlink);
	ioreg_nlinger++;
	while (ioreg_nlinger > IOREG_LINGER_MAX) {
		old = TAILQ_FIRST(&ioreg_linger);
		TAILQ_REMOVE(&ioreg_linger, old, vlink);
		LIST_REMOVE(old, link);
		ioreg_nlinger--;
		ioreg_ent_free(old);
	}
}

/*
 * Copy a producer's extra keys into a property bag. The registry's own keys
 * win; only the types libIOKit converts (string, number, bool) are copied.
 */
static void
ioreg_bag_merge(nvlist_t *bag, const nvlist_t *extra)
{
	const char *key;
	void *cookie;
	int type;

	cookie = NULL;
	while ((key = nvlist_next(extra, &type, &cookie)) != NULL) {
		if (nvlist_exists(bag, key))
			continue;
		switch (type) {
		case NV_TYPE_STRING:
			nvlist_add_string(bag, key,
			    nvlist_get_string(extra, key));
			break;
		case NV_TYPE_NUMBER:
			nvlist_add_number(bag, key,
			    nvlist_get_number(extra, key));
			break;
		case NV_TYPE_BOOL:
			nvlist_add_bool(bag, key, nvlist_get_bool(extra, key));
			break;
		default:
			break;
		}
	}
}

/* ---- newbus accessors -> ioreg_node ---- */

/* True if dev is a PCI nub (parent bus is "pci"); pci_get_* is then valid. */
static bool
ioreg_is_pci(device_t dev)
{
	device_t parent = device_get_parent(dev);

	return (parent != NULL && strcmp(device_get_name(parent), "pci") == 0);
}

/*
 * Build dev's full nameunit path from the root, e.g. "/nexus0/pcib0/pci0/em0".
 * Bounded by IOREG_PATH_MAX; deep trees are simply truncated (NUL-terminated).
 */
static void
ioreg_build_path(device_t dev, char *buf, size_t buflen)
{
	device_t chain[32];
	int n = 0;
	size_t off = 0;
	device_t d;

	for (d = dev; d != NULL && n < (int)nitems(chain); d = device_get_parent(d))
		chain[n++] = d;
	buf[0] = '\0';
	/* Walk from root (last collected) down to dev (first collected). */
	for (int i = n - 1; i >= 0; i--) {
		const char *nu = device_get_nameunit(chain[i]);
		int r;

		if (nu == NULL || nu[0] == '\0')
			continue;
		r = snprintf(buf + off, buflen - off, "/%s", nu);
		if (r < 0 || (size_t)r >= buflen - off) {
			buf[buflen - 1] = '\0';
			return;
		}
		off += r;
	}
	if (off == 0)
		strlcpy(buf, "/", buflen);
}

/* Fill out a node descriptor for dev with stable id `id`. Caller is in process
 * context (no locks held that forbid the newbus reads here). */
static void
ioreg_fill_node(device_t dev, uint64_t id, uint64_t parent_id, int state,
    struct ioreg_node *n)
{
	driver_t *drv;
	devclass_t dc;
	const char *s;

	bzero(n, sizeof(*n));
	n->id = id;
	n->parent_id = parent_id;
	n->state = state;
	n->devstate = (int32_t)device_get_state(dev);

	s = device_get_name(dev);
	if (s != NULL)
		strlcpy(n->name, s, sizeof(n->name));

	dc = device_get_devclass(dev);
	if (dc != NULL && (s = devclass_get_name(dc)) != NULL)
		strlcpy(n->classname, s, sizeof(n->classname));

	drv = device_get_driver(dev);
	if (drv != NULL && drv->name != NULL)
		strlcpy(n->driver, drv->name, sizeof(n->driver));

	ioreg_build_path(dev, n->path, sizeof(n->path));

	if (ioreg_is_pci(dev)) {
		n->pci_vendor = pci_get_vendor(dev);
		n->pci_device = pci_get_device(dev);
		n->pci_subvendor = pci_get_subvendor(dev);
		n->pci_class = pci_get_class(dev);
	}
}

/* ---- property bag (packed nvlist) ---- */

/*
 * Build a node's property bag. Synthesised from the device_get_* / pci_get_*
 * accessors (releng/15.0 exposes device_get_property()/device_has_property() but
 * no key enumeration, so we cannot iterate arbitrary device properties; we
 * publish the well-known scalars). Returns a freshly created nvlist the caller
 * must nvlist_destroy(); NULL on allocation failure.
 */
/*
 * Build a property bag from an already-populated ioreg_node. The device_t-free
 * core of ioreg_build_bag(); used by the IOREGIOCPROPS output path (the only
 * remaining nvlist consumer — the criteria *input* paths are now flat structs,
 * #218). `desc` is an optional description string (NULL when unavailable).
 */
static nvlist_t *
ioreg_build_bag_node(const struct ioreg_node *n, const char *desc)
{
	nvlist_t *nvl;

	nvl = nvlist_create(0);
	if (nvl == NULL)
		return (NULL);

	nvlist_add_number(nvl, "id", n->id);
	nvlist_add_number(nvl, "parent_id", n->parent_id);
	nvlist_add_number(nvl, "state", (uint64_t)(uint32_t)n->state);
	nvlist_add_number(nvl, "devstate", (uint64_t)(uint32_t)n->devstate);
	nvlist_add_string(nvl, "name", n->name);
	nvlist_add_string(nvl, "class", n->classname);
	nvlist_add_string(nvl, "driver", n->driver);
	nvlist_add_string(nvl, "path", n->path);
	if (n->pci_vendor != 0 || n->pci_device != 0) {
		nvlist_add_number(nvl, "pci_vendor", n->pci_vendor);
		nvlist_add_number(nvl, "pci_device", n->pci_device);
		nvlist_add_number(nvl, "pci_subvendor", n->pci_subvendor);
		nvlist_add_number(nvl, "pci_class", n->pci_class);
	}
	if (desc != NULL && desc[0] != '\0')
		nvlist_add_string(nvl, "description", desc);
	return (nvl);
}

static nvlist_t *
ioreg_build_bag(device_t dev, uint64_t id, uint64_t parent_id, int state)
{
	struct ioreg_node n;

	ioreg_fill_node(dev, id, parent_id, state, &n);
	return (ioreg_build_bag_node(&n, device_get_desc(dev)));
}

/* ---- ioctl handlers ---- */

/* IOREGIOCROOT: the root is the top of dev0's ancestry (the "root0" nexus). */
static int
ioreg_get_root(uint64_t *out)
{
	device_t root;

	root = ioreg_root_dev();
	if (root == NULL) {
		*out = 0;
		return (0);
	}
	sx_xlock(&ioreg_lock);
	*out = ioreg_id_for_locked(root);
	sx_xunlock(&ioreg_lock);
	return (0);
}

/*
 * IOREGIOCCHILDREN: enumerate the immediate children of node `c->id`. Walks the
 * live tree, mints ids for any newly seen children, copies up to c->max ids out
 * to the user array, and reports the true child count. The id xlock is dropped
 * before copyout (copyout may fault/sleep); the child id list is captured into a
 * small kernel array first. Live virtual children (#333) follow the newbus ones.
 */
static int
ioreg_children(struct ioreg_children *c)
{
	device_t parent;
	device_t *kids;
	uint64_t *ids, *vkids;
	int nkids, nvkids, i, error;
	uint32_t ncopy, total;

	/* Resolve the parent under the lock; reject lingering/unknown. A virtual
	 * parent has no newbus children, only virtual ones. */
	sx_xlock(&ioreg_lock);
	{
		struct ioreg_ent *e = ioreg_find_id_locked(c->id);

		if (e == NULL || e->state != IOREG_STATE_LIVE ||
		    (e->kind == IOREG_KIND_NEWBUS && e->dev == NULL)) {
			sx_xunlock(&ioreg_lock);
			return (ENOENT);
		}
		parent = e->dev;	/* NULL for a virtual entry */
	}
	sx_xunlock(&ioreg_lock);

	kids = NULL;
	nkids = 0;
	if (parent != NULL) {
		error = device_get_children(parent, &kids, &nkids);
		if (error != 0)
			return (error);
		if (nkids < 0)
			nkids = 0;
	}

	sx_xlock(&ioreg_lock);
	vkids = ioreg_virt_children_locked(c->id, &nvkids);
	total = (uint32_t)nkids + (uint32_t)nvkids;
	ids = malloc((size_t)total * sizeof(uint64_t) + 1, M_IOREG, M_WAITOK);
	for (i = 0; i < nkids; i++)
		ids[i] = ioreg_id_for_locked(kids[i]);
	sx_xunlock(&ioreg_lock);
	if (kids != NULL)
		free(kids, M_TEMP);
	if (vkids != NULL) {
		memcpy(ids + nkids, vkids, (size_t)nvkids * sizeof(uint64_t));
		free(vkids, M_IOREG);
	}

	c->count = total;
	ncopy = (c->max < total) ? c->max : total;
	error = 0;
	if (ncopy > 0 && c->children != 0)
		error = copyout(ids, (void *)(uintptr_t)c->children,
		    (size_t)ncopy * sizeof(uint64_t));
	free(ids, M_IOREG);
	return (error);
}

/* IOREGIOCNODE: fill the node descriptor for n->id (live or lingering). */
static int
ioreg_node(struct ioreg_node *n)
{
	device_t dev;
	uint64_t id, parent_id;
	int state;

	id = n->id;
	sx_xlock(&ioreg_lock);
	{
		struct ioreg_ent *e = ioreg_find_id_locked(id);
		struct ioreg_ent *pe;

		if (e == NULL) {
			sx_xunlock(&ioreg_lock);
			return (ENOENT);
		}
		if (e->virt != NULL) {
			/* Virtual (live or lingering) or departed newbus:
			 * the snapshot answers. */
			ioreg_virt_fill_node_locked(e, n);
			sx_xunlock(&ioreg_lock);
			return (0);
		}
		state = e->state;
		dev = e->dev;
		parent_id = 0;
		if (dev != NULL) {
			device_t p = device_get_parent(dev);

			if (p != NULL) {
				pe = ioreg_find_dev_locked(p);
				parent_id = (pe != NULL) ? pe->id :
				    ioreg_id_for_locked(p);
			}
		}
	}
	sx_xunlock(&ioreg_lock);

	if (dev == NULL) {
		/* Lingering: device gone, report what the id remembers. */
		bzero(n, sizeof(*n));
		n->id = id;
		n->state = IOREG_STATE_DETACHED;
		return (0);
	}
	ioreg_fill_node(dev, id, parent_id, state, n);
	return (0);
}

/*
 * IOREGIOCPROPS: pack the node's property bag (nvlist) and copy it out. Sizes
 * the bag, copies up to p->len bytes, and always reports the full packed size so
 * userland can size a retry. The nvlist is built and packed in process context
 * with the id lock dropped (nvlist allocs are M_WAITOK).
 */
static int
ioreg_props(struct ioreg_props *p)
{
	device_t dev;
	uint64_t id, parent_id;
	int state, error;
	nvlist_t *nvl, *extra;
	void *packed;
	size_t plen;
	uint32_t outlen;
	struct ioreg_node vn;

	id = p->id;
	extra = NULL;
	sx_xlock(&ioreg_lock);
	{
		struct ioreg_ent *e = ioreg_find_id_locked(id);
		struct ioreg_ent *pe;

		if (e != NULL && e->virt != NULL) {
			/* Virtual (live or lingering) or departed newbus:
			 * build from the snapshot. The extra keys are cloned
			 * so the bag is built unlocked (aging may free the
			 * original once we drop it). */
			ioreg_virt_fill_node_locked(e, &vn);
			if (e->virt->props != NULL)
				extra = nvlist_clone(e->virt->props);
			sx_xunlock(&ioreg_lock);
			nvl = ioreg_build_bag_node(&vn, NULL);
			if (nvl != NULL && extra != NULL)
				ioreg_bag_merge(nvl, extra);
			if (extra != NULL)
				nvlist_destroy(extra);
			goto pack;
		}
		if (e == NULL || e->dev == NULL) {
			sx_xunlock(&ioreg_lock);
			return (ENOENT);
		}
		state = e->state;
		dev = e->dev;
		parent_id = 0;
		{
			device_t pp = device_get_parent(dev);

			if (pp != NULL) {
				pe = ioreg_find_dev_locked(pp);
				parent_id = (pe != NULL) ? pe->id :
				    ioreg_id_for_locked(pp);
			}
		}
	}
	sx_xunlock(&ioreg_lock);

	nvl = ioreg_build_bag(dev, id, parent_id, state);
pack:
	if (nvl == NULL)
		return (ENOMEM);
	if (nvlist_error(nvl) != 0) {
		error = nvlist_error(nvl);
		nvlist_destroy(nvl);
		return (error);
	}

	packed = nvlist_pack(nvl, &plen);
	nvlist_destroy(nvl);
	if (packed == NULL)
		return (ENOMEM);

	outlen = (plen > p->len) ? p->len : (uint32_t)plen;
	error = 0;
	if (outlen > 0 && p->buf != 0)
		error = copyout(packed, (void *)(uintptr_t)p->buf, outlen);
	p->len = (uint32_t)plen;	/* always report full size */
	free(packed, M_NVLIST);
	return (error);
}

/*
 * AND-match a node against a flat criteria struct (#218). Every non-empty
 * string field and every non-zero numeric field must equal the node's
 * corresponding value (zero-as-wildcard; an all-zero criteria matches every
 * node). No serialization is involved — the former packed-nvlist criteria bag
 * (which never round-tripped because libxpc's packer and the kernel's libnv
 * reader disagree on the wire format) is gone. Works for a real or an injected
 * event alike since it reads only the already-populated struct ioreg_node.
 */
static bool
ioreg_node_matches_crit(const struct ioreg_node *n,
    const struct ioreg_criteria *c)
{
	if (c == NULL)
		return (true);
	if (c->name[0] != '\0' && strcmp(n->name, c->name) != 0)
		return (false);
	if (c->classname[0] != '\0' && strcmp(n->classname, c->classname) != 0)
		return (false);
	if (c->driver[0] != '\0' && strcmp(n->driver, c->driver) != 0)
		return (false);
	if (c->pci_vendor != 0 && n->pci_vendor != c->pci_vendor)
		return (false);
	if (c->pci_device != 0 && n->pci_device != c->pci_device)
		return (false);
	if (c->pci_subvendor != 0 && n->pci_subvendor != c->pci_subvendor)
		return (false);
	if (c->pci_class != 0 && n->pci_class != c->pci_class)
		return (false);
	return (true);
}

/* Match a real device_t against flat criteria (fills a node, no allocation). */
static bool
ioreg_dev_matches_crit(device_t dev, uint64_t id, uint64_t parent_id, int state,
    const struct ioreg_criteria *c)
{
	struct ioreg_node n;

	if (c == NULL)
		return (true);
	ioreg_fill_node(dev, id, parent_id, state, &n);
	return (ioreg_node_matches_crit(&n, c));
}

/*
 * Lookup walk over a live virtual entry and its live virtual children (#333).
 * Same contract as ioreg_lookup_walk below.
 */
static void
ioreg_lookup_walk_virt(uint64_t id, const struct ioreg_criteria *crit,
    uint64_t *ids, uint32_t cap, uint32_t *count, int depth)
{
	struct ioreg_ent *e;
	struct ioreg_node n;
	uint64_t *vkids;
	int nvkids, i;

	if (depth > IOREG_MAX_DEPTH)
		return;

	sx_xlock(&ioreg_lock);
	e = ioreg_find_id_locked(id);
	if (e == NULL || e->kind != IOREG_KIND_VIRTUAL ||
	    e->state != IOREG_STATE_LIVE) {
		sx_xunlock(&ioreg_lock);
		return;
	}
	ioreg_virt_fill_node_locked(e, &n);
	vkids = ioreg_virt_children_locked(id, &nvkids);
	sx_xunlock(&ioreg_lock);

	if (ioreg_node_matches_crit(&n, crit)) {
		if (*count < cap && ids != NULL)
			ids[*count] = id;
		(*count)++;
	}
	for (i = 0; i < nvkids; i++)
		ioreg_lookup_walk_virt(vkids[i], crit, ids, cap, count,
		    depth + 1);
	if (vkids != NULL)
		free(vkids, M_IOREG);
}

/*
 * Recursively collect matching node ids into `ids` (capacity in *cap entries,
 * which may be 0 to count-only). *count is the running total of matches found
 * (incremented even past capacity, so the caller learns the true count). Walks
 * the live tree from `dev`, newbus children first, then virtual children.
 * Mints ids as needed (own xlock per device).
 */
static void
ioreg_lookup_walk(device_t dev, const struct ioreg_criteria *crit, uint64_t *ids,
    uint32_t cap, uint32_t *count, int depth)
{
	device_t *kids;
	int nkids, nvkids, i;
	uint64_t id, parent_id, *vkids;
	int state;

	if (depth > IOREG_MAX_DEPTH)
		return;

	sx_xlock(&ioreg_lock);
	id = ioreg_id_for_locked(dev);
	{
		device_t p = device_get_parent(dev);
		struct ioreg_ent *pe;

		parent_id = 0;
		if (p != NULL) {
			pe = ioreg_find_dev_locked(p);
			parent_id = (pe != NULL) ? pe->id : ioreg_id_for_locked(p);
		}
	}
	state = IOREG_STATE_LIVE;
	vkids = ioreg_virt_children_locked(id, &nvkids);
	sx_xunlock(&ioreg_lock);

	if (ioreg_dev_matches_crit(dev, id, parent_id, state, crit)) {
		if (*count < cap && ids != NULL)
			ids[*count] = id;
		(*count)++;
	}

	if (device_get_children(dev, &kids, &nkids) == 0) {
		for (i = 0; i < nkids; i++)
			ioreg_lookup_walk(kids[i], crit, ids, cap, count,
			    depth + 1);
		free(kids, M_TEMP);
	}
	for (i = 0; i < nvkids; i++)
		ioreg_lookup_walk_virt(vkids[i], crit, ids, cap, count,
		    depth + 1);
	if (vkids != NULL)
		free(vkids, M_IOREG);
}

/*
 * IOREGIOCLOOKUP: walk the whole tree from the root matching each node against
 * the flat criteria struct (#218; no nvlist unpack — see ioreg_node_matches_crit)
 * and copy out up to lu->max matching ids (reporting the true count). The
 * criteria arrives by value inside lu (copied in/out by the ioctl framework).
 */
static int
ioreg_lookup(struct ioreg_lookup *lu)
{
	const struct ioreg_criteria *crit = &lu->criteria;
	uint64_t *ids = NULL;
	device_t root;
	uint32_t count, ncopy;
	int error;

	/* Caller-supplied strings may arrive unterminated; bound them in place. */
	lu->criteria.name[sizeof(lu->criteria.name) - 1] = '\0';
	lu->criteria.classname[sizeof(lu->criteria.classname) - 1] = '\0';
	lu->criteria.driver[sizeof(lu->criteria.driver) - 1] = '\0';

	/* Find the root device_t (same logic as IOREGIOCROOT). */
	root = ioreg_root_dev();
	if (root == NULL) {
		lu->count = 0;
		return (0);
	}

	ids = (lu->max > 0) ? malloc((size_t)lu->max * sizeof(uint64_t),
	    M_IOREG, M_WAITOK) : NULL;
	count = 0;
	ioreg_lookup_walk(root, crit, ids, lu->max, &count, 0);

	lu->count = count;
	ncopy = (lu->max < count) ? lu->max : count;
	error = 0;
	if (ncopy > 0 && lu->matches != 0)
		error = copyout(ids, (void *)(uintptr_t)lu->matches,
		    (size_t)ncopy * sizeof(uint64_t));
	if (ids != NULL)
		free(ids, M_IOREG);
	return (error);
}

/* ---- K-PR2 (#225): device-event notification watches ---- */

/* Free a watch's resources (the retained send right; the flat criteria is by
 * value). The watch must already be unlinked. Safe to call with no lock held. */
static void
ioreg_watch_free(struct ioreg_watch *w)
{
#ifdef COMPAT_MACH
	if (w->port != NULL)
		iokit_notify_release_port(w->port);
#endif
	free(w, M_IOREG);
}

/*
 * IOREGIOCWATCH: register a notify port + flat match criteria + event mask
 * (#218; the criteria arrives by value in wr — no nvlist unpack). Resolves the
 * caller-supplied port name to a retained send right and appends a watch.
 * Without COMPAT_MACH there is no Mach channel, so the call is rejected (ENOSYS)
 * — same shape as the catalogue's test-send path.
 */
static int
ioreg_watch_register(struct ioreg_watch_reg *wr, struct thread *td __unused)
{
#ifdef COMPAT_MACH
	struct ioreg_watch *w;
	void *port = NULL;
	int error;

	if (wr->notify_port == 0)
		return (EINVAL);
	/* event_mask == 0 would be a no-op watch; reject as a likely caller bug. */
	if ((wr->event_mask & (IOREG_EVENT_ARRIVE | IOREG_EVENT_DEPART |
	    IOREG_EVENT_MATCHED)) == 0)
		return (EINVAL);

	/* Caller-supplied strings may arrive unterminated; bound them in place. */
	wr->criteria.name[sizeof(wr->criteria.name) - 1] = '\0';
	wr->criteria.classname[sizeof(wr->criteria.classname) - 1] = '\0';
	wr->criteria.driver[sizeof(wr->criteria.driver) - 1] = '\0';

	/* Resolve the caller's port name -> retained send right (in the calling
	 * thread's task IPC space; the ioctl runs in that context). */
	error = iokit_notify_copyin_port(wr->notify_port, &port);
	if (error != 0)
		return (error);

	w = malloc(sizeof(*w), M_IOREG, M_WAITOK | M_ZERO);
	w->criteria = wr->criteria;
	w->mask = wr->event_mask;
	w->port = port;

	sx_xlock(&ioreg_watch_lock);
	TAILQ_INSERT_TAIL(&ioreg_watches, w, link);
	ioreg_watch_count++;
	sx_xunlock(&ioreg_watch_lock);
	return (0);
#else
	(void)wr;
	return (ENOSYS);
#endif
}

/*
 * Emit a device event of `kind` (one IOREG_EVENT_* bit) for `dev` to every watch
 * whose criteria match and whose mask includes the kind.
 *
 * Lock discipline: the Mach send is NEVER done under ioreg_watch_lock. Pass 1
 * walks the list under the xlock, prunes any watch whose port has gone dead, and
 * snapshots a fresh per-watch send ref (which keeps the port alive past the
 * unlock) for each matching watch into a small heap array. Pass 2 drops the lock
 * and sends; a send failure (dead port) marks that watch's port for pruning on
 * the next pass. This mirrors iocat_rematch_pending's splice-then-act-lockless.
 *
 * Runs from the device_attach / device_detach eventhandlers, which fire under
 * the sleepable newbus config context (Giant), so the M_WAITOK alloc and sx are
 * fine; the K1 id-map maintenance already runs here.
 */
static void
ioreg_emit_event_node(const struct ioreg_node *n, const char *desc __unused,
    uint32_t kind)
{
#ifdef COMPAT_MACH
	struct ioreg_watch *w, *tw;
	struct { void *ref; bool term; } *snap;
	u_int nsnap, cap, i;

	/* Cheap, racy early-out: skip the alloc+lock when no watch is registered
	 * (re-checked authoritatively under the xlock below). */
	if (ioreg_watch_count == 0)
		return;

	sx_xlock(&ioreg_watch_lock);
	cap = ioreg_watch_count;
	if (cap == 0) {
		sx_xunlock(&ioreg_watch_lock);
		return;
	}
	snap = malloc((size_t)cap * sizeof(*snap), M_IOREG, M_WAITOK);
	nsnap = 0;
	TAILQ_FOREACH_SAFE(w, &ioreg_watches, link, tw) {
		/* Prune a watch whose receive right the client already dropped. */
		if (iokit_notify_port_dead(w->port)) {
			TAILQ_REMOVE(&ioreg_watches, w, link);
			ioreg_watch_count--;
			if (w->target != 0)
				ioreg_interest_count--;
			ioreg_watch_free(w);
			continue;
		}
		/* An interest watch (#338) only hears its target's departure:
		 * hand its stored send right to pass 2 for a final
		 * kIOMessageServiceIsTerminated and drop the watch. */
		if (w->target != 0) {
			if (kind != IOREG_EVENT_DEPART || w->target != n->id ||
			    nsnap >= cap)
				continue;
			TAILQ_REMOVE(&ioreg_watches, w, link);
			ioreg_watch_count--;
			ioreg_interest_count--;
			snap[nsnap].ref = w->port;
			snap[nsnap].term = true;
			nsnap++;
			w->port = NULL;
			ioreg_watch_free(w);
			continue;
		}
		if ((w->mask & kind) == 0)
			continue;
		/* Flat-criteria AND-match against this event's node (#218);
		 * works for a real or an injected event alike. */
		if (!ioreg_node_matches_crit(n, &w->criteria))
			continue;
		if (nsnap >= cap)	/* list can't grow under the xlock */
			break;
		snap[nsnap].ref = iokit_notify_copy_port(w->port);
		snap[nsnap].term = false;
		if (snap[nsnap].ref != NULL)
			nsnap++;
	}
	sx_xunlock(&ioreg_watch_lock);

	/* Pass 2: send with no lock held. Both senders consume each ref. */
	for (i = 0; i < nsnap; i++) {
		if (snap[i].term)
			(void)iokit_notify_send_interest(snap[i].ref,
			    IOREG_MSG_SERVICE_IS_TERMINATED, n->id, 0, 0, NULL);
		else
			(void)iokit_notify_send(snap[i].ref, kind, n->id,
			    n->name, n->classname, n->pci_vendor,
			    n->pci_device);
	}

	free(snap, M_IOREG);
#else
	(void)n; (void)kind;
#endif
}

/*
 * Emit an event for a real device_t: fill its node from the live newbus
 * accessors then run the shared node-based emission path above.
 */
static void
ioreg_emit_event(device_t dev, uint64_t id, uint64_t parent_id, int state,
    uint32_t kind)
{
#ifdef COMPAT_MACH
	struct ioreg_node n;

	if (ioreg_watch_count == 0)
		return;
	ioreg_fill_node(dev, id, parent_id, state, &n);
	ioreg_emit_event_node(&n, device_get_desc(dev), kind);
#else
	(void)dev; (void)id; (void)parent_id; (void)state; (void)kind;
#endif
}

/*
 * IOREGIOCTESTEVENT (PR4): inject a synthetic device event through the exact
 * same watch match + Mach emission path a real device_attach takes, so CI can
 * verify the notify round-trip deterministically without a physical device. The
 * caller supplies the match-relevant node fields (kind/id/name/class/pci_*); we
 * build a node, bound-copy the strings, and run it through
 * ioreg_emit_event_node. `kind` must be exactly one IOREG_EVENT_* bit. Without
 * COMPAT_MACH there is no Mach channel, so this is ENOSYS (same as IOREGIOCWATCH).
 */
static int
ioreg_test_event(struct ioreg_test_event *te)
{
#ifdef COMPAT_MACH
	struct ioreg_node n;

	/* Accept exactly one known event bit (the thing being injected). */
	if (te->kind != IOREG_EVENT_ARRIVE && te->kind != IOREG_EVENT_DEPART &&
	    te->kind != IOREG_EVENT_MATCHED)
		return (EINVAL);

	bzero(&n, sizeof(n));
	n.id = te->id;
	n.parent_id = 0;
	n.state = (te->kind == IOREG_EVENT_DEPART) ?
	    IOREG_STATE_DETACHED : IOREG_STATE_LIVE;
	n.devstate = 0;
	/* Caller-supplied strings: force-terminate the source in place (it may
	 * arrive unterminated from userland) then bounded-copy into the node. */
	te->name[sizeof(te->name) - 1] = '\0';
	te->classname[sizeof(te->classname) - 1] = '\0';
	strlcpy(n.name, te->name, sizeof(n.name));
	strlcpy(n.classname, te->classname, sizeof(n.classname));
	n.driver[0] = '\0';
	n.path[0] = '\0';
	n.pci_vendor = te->pci_vendor;
	n.pci_device = te->pci_device;
	n.pci_subvendor = 0;
	n.pci_class = 0;

	ioreg_emit_event_node(&n, NULL, te->kind);
	return (0);
#else
	(void)te;
	return (ENOSYS);
#endif
}

/*
 * IOREGIOCINTEREST (#338): register an interest watch on one live entry. The
 * port is resolved first (no registry lock held). The liveness check and the
 * insertion then happen under the watch lock, which every departure takes
 * after marking its entry detached, so a departure either fails the check
 * (ENOENT) or finds the watch and terminates it. Lock order: watch lock, then
 * ioreg_lock; nothing takes them the other way round (emission always drops
 * ioreg_lock first).
 */
static int
ioreg_interest_register(struct ioreg_interest_reg *ir)
{
#ifdef COMPAT_MACH
	struct ioreg_watch *w;
	struct ioreg_ent *e;
	void *port = NULL;
	bool live;
	int error;

	if (ir->notify_port == 0 || ir->interest != IOREG_INTEREST_GENERAL)
		return (EINVAL);
	if (ir->id == 0)
		return (ENOENT);

	error = iokit_notify_copyin_port(ir->notify_port, &port);
	if (error != 0)
		return (error);

	w = malloc(sizeof(*w), M_IOREG, M_WAITOK | M_ZERO);
	w->port = port;
	w->target = ir->id;

	sx_xlock(&ioreg_watch_lock);
	sx_slock(&ioreg_lock);
	e = ioreg_find_id_locked(ir->id);
	live = (e != NULL && e->state == IOREG_STATE_LIVE);
	sx_sunlock(&ioreg_lock);
	if (live) {
		TAILQ_INSERT_TAIL(&ioreg_watches, w, link);
		ioreg_watch_count++;
		ioreg_interest_count++;
		w = NULL;
	}
	sx_xunlock(&ioreg_watch_lock);
	if (w != NULL) {
		ioreg_watch_free(w);	/* releases the send right */
		return (ENOENT);
	}
	return (0);
#else
	(void)ir;
	return (ENOSYS);
#endif
}

#ifdef COMPAT_MACH
/*
 * Send `message` to every interest watch on entry `id`, pruning dead ones on
 * the way. Same two-pass discipline as ioreg_emit_event_node: snapshot send
 * refs under the watch lock, send with no lock held.
 */
static void
ioreg_message_id(uint64_t id, uint32_t message, uint64_t arg0, uint64_t arg1,
    const char *devnode)
{
	struct ioreg_watch *w, *tw;
	void **snap;
	u_int nsnap, cap, i;

	sx_xlock(&ioreg_watch_lock);
	cap = ioreg_interest_count;
	if (cap == 0) {
		sx_xunlock(&ioreg_watch_lock);
		return;
	}
	snap = malloc((size_t)cap * sizeof(*snap), M_IOREG, M_WAITOK);
	nsnap = 0;
	TAILQ_FOREACH_SAFE(w, &ioreg_watches, link, tw) {
		if (w->target == 0)
			continue;
		if (iokit_notify_port_dead(w->port)) {
			TAILQ_REMOVE(&ioreg_watches, w, link);
			ioreg_watch_count--;
			ioreg_interest_count--;
			ioreg_watch_free(w);
			continue;
		}
		if (w->target != id || nsnap >= cap)
			continue;
		snap[nsnap] = iokit_notify_copy_port(w->port);
		if (snap[nsnap] != NULL)
			nsnap++;
	}
	sx_xunlock(&ioreg_watch_lock);

	for (i = 0; i < nsnap; i++)
		(void)iokit_notify_send_interest(snap[i], message, id, arg0,
		    arg1, devnode);
	free(snap, M_IOREG);
}
#endif

/*
 * ioreg_message_device: see sys/ioregistry.h. `dev` is only compared against
 * the map, so a stale pointer from a queued producer is harmless.
 */
void
ioreg_message_device(device_t dev, uint32_t message, uint64_t arg0,
    uint64_t arg1, const char *devnode)
{
#ifdef COMPAT_MACH
	struct ioreg_ent *e;
	uint64_t id;

	if (dev == NULL || ioreg_interest_count == 0)
		return;
	sx_slock(&ioreg_lock);
	e = ioreg_find_dev_locked(dev);
	id = (e != NULL && e->state == IOREG_STATE_LIVE) ? e->id : 0;
	sx_sunlock(&ioreg_lock);
	if (id != 0)
		ioreg_message_id(id, message, arg0, arg1, devnode);
#else
	(void)dev; (void)message; (void)arg0; (void)arg1; (void)devnode;
#endif
}

/*
 * Drop every registered watch (releasing its send right + criteria). Called at
 * MOD_UNLOAD so the module can be unloaded without leaking ipc_port_t refs.
 */
static void
ioreg_watch_flush(void)
{
	struct ioreg_watch *w;

	sx_xlock(&ioreg_watch_lock);
	while ((w = TAILQ_FIRST(&ioreg_watches)) != NULL) {
		TAILQ_REMOVE(&ioreg_watches, w, link);
		ioreg_watch_free(w);
	}
	ioreg_watch_count = 0;
	ioreg_interest_count = 0;
	sx_xunlock(&ioreg_watch_lock);
}

/* ---- /dev/ioregistry ---- */

static d_ioctl_t ioreg_dev_ioctl;
static struct cdev *ioreg_dev;
static struct cdevsw ioreg_cdevsw = {
	.d_version = D_VERSION,
	.d_ioctl   = ioreg_dev_ioctl,
	.d_name    = "ioregistry",
};

static int
ioreg_dev_ioctl(struct cdev *dev __unused, u_long cmd, caddr_t data,
    int fflag __unused, struct thread *td)
{
	switch (cmd) {
	case IOREGIOCROOT:
		return (ioreg_get_root((uint64_t *)data));
	case IOREGIOCCHILDREN:
		return (ioreg_children((struct ioreg_children *)data));
	case IOREGIOCNODE:
		return (ioreg_node((struct ioreg_node *)data));
	case IOREGIOCPROPS:
		return (ioreg_props((struct ioreg_props *)data));
	case IOREGIOCLOOKUP:
		return (ioreg_lookup((struct ioreg_lookup *)data));
	case IOREGIOCWATCH:
		return (ioreg_watch_register((struct ioreg_watch_reg *)data, td));
	case IOREGIOCTESTEVENT:
		return (ioreg_test_event((struct ioreg_test_event *)data));
	case IOREGIOCINTEREST:
		return (ioreg_interest_register(
		    (struct ioreg_interest_reg *)data));
	default:
		return (ENOTTY);
	}
}

/* ---- hw.iokit.registry: read-only text dump (debug + hwregd transition) ---- */

static void
ioreg_sysctl_line(struct sbuf *sb, uint64_t id, const struct ioreg_node *n,
    int depth)
{
	int i;

	for (i = 0; i < depth; i++)
		sbuf_cat(sb, "  ");
	sbuf_printf(sb, "[%ju] %s class=%s driver=%s",
	    (uintmax_t)id, n->name[0] ? n->name : "?",
	    n->classname[0] ? n->classname : "-",
	    n->driver[0] ? n->driver : "-");
	if (n->pci_vendor != 0 || n->pci_device != 0)
		sbuf_printf(sb, " pci=%04x:%04x", n->pci_vendor, n->pci_device);
	sbuf_cat(sb, "\n");
}

/* Dump a live virtual entry and its live virtual children (#333). */
static void
ioreg_sysctl_walk_virt(struct sbuf *sb, uint64_t id, int depth)
{
	struct ioreg_ent *e;
	struct ioreg_node n;
	uint64_t *vkids;
	int nvkids, i;

	if (depth > IOREG_MAX_DEPTH)
		return;

	sx_xlock(&ioreg_lock);
	e = ioreg_find_id_locked(id);
	if (e == NULL || e->kind != IOREG_KIND_VIRTUAL ||
	    e->state != IOREG_STATE_LIVE) {
		sx_xunlock(&ioreg_lock);
		return;
	}
	ioreg_virt_fill_node_locked(e, &n);
	vkids = ioreg_virt_children_locked(id, &nvkids);
	sx_xunlock(&ioreg_lock);

	ioreg_sysctl_line(sb, id, &n, depth);
	for (i = 0; i < nvkids; i++)
		ioreg_sysctl_walk_virt(sb, vkids[i], depth + 1);
	if (vkids != NULL)
		free(vkids, M_IOREG);
}

static void
ioreg_sysctl_walk(struct sbuf *sb, device_t dev, int depth)
{
	device_t *kids;
	int nkids, nvkids, i;
	uint64_t id, *vkids;
	struct ioreg_node n;

	if (depth > IOREG_MAX_DEPTH)
		return;

	sx_xlock(&ioreg_lock);
	id = ioreg_id_for_locked(dev);
	vkids = ioreg_virt_children_locked(id, &nvkids);
	sx_xunlock(&ioreg_lock);

	ioreg_fill_node(dev, id, 0, IOREG_STATE_LIVE, &n);
	ioreg_sysctl_line(sb, id, &n, depth);

	if (device_get_children(dev, &kids, &nkids) == 0) {
		for (i = 0; i < nkids; i++)
			ioreg_sysctl_walk(sb, kids[i], depth + 1);
		free(kids, M_TEMP);
	}
	for (i = 0; i < nvkids; i++)
		ioreg_sysctl_walk_virt(sb, vkids[i], depth + 1);
	if (vkids != NULL)
		free(vkids, M_IOREG);
}

static int
ioreg_sysctl_dump(SYSCTL_HANDLER_ARGS)
{
	struct sbuf sb;
	device_t root;
	int error;

	root = ioreg_root_dev();

	sbuf_new(&sb, NULL, 1024, SBUF_AUTOEXTEND);
	if (root != NULL)
		ioreg_sysctl_walk(&sb, root, 0);
	else
		sbuf_cat(&sb, "(no root device)\n");
	error = sbuf_finish(&sb);
	if (error == 0)
		error = SYSCTL_OUT(req, sbuf_data(&sb), sbuf_len(&sb) + 1);
	sbuf_delete(&sb);
	return (error);
}

SYSCTL_PROC(_hw_iokit, OID_AUTO, registry,
    CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_MPSAFE, NULL, 0,
    ioreg_sysctl_dump, "A",
    "IOKit registry (live newbus device tree plus virtual entries)");

/* ---- id-map maintenance + event emission via newbus eventhandlers ---- */

static eventhandler_tag ioreg_attach_tag;
static eventhandler_tag ioreg_detach_tag;
static eventhandler_tag ioreg_match_start_tag;
static eventhandler_tag ioreg_match_end_tag;

/*
 * Coalesce ARRIVE notifications until the device tree settles. device_attach
 * fires mid-recursion (a bridge's attach probes its children), so emitting an
 * ARRIVE there could notify before the subtree is built. Instead we record each
 * freshly-attached dev on a pending list and flush it only when the outermost
 * device_probe_and_attach() completes (the device_match_end depth returns to 0),
 * mirroring mach_busystate's quiesce counter. The list + depth are touched only
 * under the bus topology lock (these handlers are all serialized by it), but we
 * guard with a mutex too and splice-then-emit-lockless so the Mach send never
 * runs under a lock. K-PR2 (#225).
 *
 * A virtual entry published during a probe/attach (#333) is queued here too,
 * by id (dev == NULL), so its ARRIVE follows its newbus parent's. The mutex is
 * initialized by MTX_SYSINIT, not MOD_LOAD, so a producer may publish before
 * this module's MOD_LOAD has run.
 */
struct ioreg_pend {
	STAILQ_ENTRY(ioreg_pend) link;
	device_t	dev;		/* NULL: virtual entry `vid` */
	uint64_t	vid;
};
static STAILQ_HEAD(, ioreg_pend) ioreg_pending =
    STAILQ_HEAD_INITIALIZER(ioreg_pending);
static struct mtx ioreg_pend_mtx;
MTX_SYSINIT(ioreg_pend_mtx, &ioreg_pend_mtx, "ioregpend", MTX_DEF);
static int ioreg_match_depth;		/* in-flight probe->attach nesting */

/*
 * Send ARRIVE+MATCHED for virtual entry `id` if it is still live (it may have
 * been retired, or even aged out, while its notification was queued).
 */
static void
ioreg_virt_announce(uint64_t id)
{
	struct ioreg_ent *e;
	struct ioreg_node n;

	if (ioreg_watch_count == 0)
		return;
	sx_xlock(&ioreg_lock);
	e = ioreg_find_id_locked(id);
	if (e == NULL || e->kind != IOREG_KIND_VIRTUAL ||
	    e->state != IOREG_STATE_LIVE) {
		sx_xunlock(&ioreg_lock);
		return;
	}
	ioreg_virt_fill_node_locked(e, &n);
	sx_xunlock(&ioreg_lock);
	ioreg_emit_event_node(&n, NULL, IOREG_EVENT_ARRIVE);
	ioreg_emit_event_node(&n, NULL, IOREG_EVENT_MATCHED);
}

/*
 * Announce a freshly published virtual entry: queue it behind an in-flight
 * newbus probe/attach (sent at quiesce, after its parent's ARRIVE), or send
 * now when the tree is settled.
 */
static void
ioreg_virt_arrive(uint64_t id)
{
	struct ioreg_pend *p;

	if (ioreg_watch_count == 0)
		return;
	p = malloc(sizeof(*p), M_IOREG, M_WAITOK | M_ZERO);
	p->vid = id;
	mtx_lock(&ioreg_pend_mtx);
	if (ioreg_match_depth > 0) {
		STAILQ_INSERT_TAIL(&ioreg_pending, p, link);
		p = NULL;
	}
	mtx_unlock(&ioreg_pend_mtx);
	if (p != NULL) {
		free(p, M_IOREG);
		ioreg_virt_announce(id);
	}
}

/*
 * ioreg_publish_virtual: see sys/ioregistry.h. The parent is `parent`'s newbus
 * node (its id is minted here if this is its first sighting) or, for NULL, the
 * synthetic IOResources node, which is created on first use.
 */
uint64_t
ioreg_publish_virtual(device_t parent, const char *name, const char *classname,
    nvlist_t *props)
{
	struct ioreg_ent *e;
	char *ppath;
	uint64_t pid, id, resid;

	if (name == NULL || name[0] == '\0' || classname == NULL ||
	    classname[0] == '\0') {
		if (props != NULL)
			nvlist_destroy(props);
		return (0);
	}
	if (props != NULL && nvlist_error(props) != 0) {
		nvlist_destroy(props);
		props = NULL;
	}

	ppath = malloc(IOREG_PATH_MAX, M_IOREG, M_WAITOK | M_ZERO);
	if (parent != NULL)
		ioreg_build_path(parent, ppath, IOREG_PATH_MAX);

	resid = 0;
	sx_xlock(&ioreg_lock);
	if (parent != NULL) {
		pid = ioreg_id_for_locked(parent);
	} else {
		if (ioreg_resources == NULL) {
			ioreg_resources = ioreg_virt_insert_locked(0, "",
			    IOREG_RESOURCES_NAME, IOREG_RESOURCES_NAME, NULL,
			    true);
			strlcpy(ioreg_resources->virt->node.path,
			    IOREG_RESOURCES_PATH,
			    sizeof(ioreg_resources->virt->node.path));
			ioreg_resources_link_locked();
			resid = ioreg_resources->id;
		}
		pid = ioreg_resources->id;
		strlcpy(ppath, ioreg_resources->virt->node.path,
		    IOREG_PATH_MAX);
	}
	e = ioreg_virt_insert_locked(pid, ppath, name, classname, props, false);
	id = e->id;
	sx_xunlock(&ioreg_lock);
	free(ppath, M_IOREG);

	if (resid != 0)
		ioreg_virt_arrive(resid);
	ioreg_virt_arrive(id);
	return (id);
}

/*
 * ioreg_retire_virtual: see sys/ioregistry.h. DEPART is sent directly, as for
 * a newbus detach; a still-queued ARRIVE for the id is dropped at flush time
 * because the entry is no longer live.
 */
int
ioreg_retire_virtual(uint64_t id)
{
	struct ioreg_ent *e;
	struct ioreg_node n;

	sx_xlock(&ioreg_lock);
	e = ioreg_find_id_locked(id);
	if (e == NULL || e->kind != IOREG_KIND_VIRTUAL ||
	    e->state != IOREG_STATE_LIVE || e->virt->permanent) {
		sx_xunlock(&ioreg_lock);
		return (ENOENT);
	}
	e->state = IOREG_STATE_DETACHED;
	ioreg_virt_fill_node_locked(e, &n);
	TAILQ_REMOVE(&ioreg_virt_live, e, vlink);
	ioreg_linger_locked(e);
	sx_xunlock(&ioreg_lock);

	ioreg_emit_event_node(&n, NULL, IOREG_EVENT_DEPART);
	return (0);
}

/*
 * device_attach: mint (or revive) the id eagerly so the id space tracks the live
 * tree even before the first walk (K1). Then queue the dev for a coalesced
 * ARRIVE|MATCHED notification at the next quiesce (K-PR2). The device_attach
 * event fires only after a fully successful attach (DS_ATTACHED, driver bound),
 * so it is the authoritative "device published + matched" signal. Runs in the
 * (sleepable) newbus config context, so the sx xlock / M_WAITOK are fine.
 */
static void
ioreg_on_attach(void *arg __unused, device_t dev)
{
	struct ioreg_pend *p;

	sx_xlock(&ioreg_lock);
	(void)ioreg_id_for_locked(dev);
	sx_xunlock(&ioreg_lock);

	/* Skip the bookkeeping entirely when nobody is watching. */
	if (ioreg_watch_count == 0)
		return;
	p = malloc(sizeof(*p), M_IOREG, M_NOWAIT | M_ZERO);
	if (p == NULL)
		return;		/* drop a notification rather than fail attach */
	p->dev = dev;
	mtx_lock(&ioreg_pend_mtx);
	STAILQ_INSERT_TAIL(&ioreg_pending, p, link);
	mtx_unlock(&ioreg_pend_mtx);
}

/*
 * device_detach: once the device is actually gone (EVHDEV_DETACH_COMPLETE),
 * snapshot the departing node, drop the device_t pointer and mark the id
 * detached, so the id lingers for in-flight userland iterators rather than
 * dangling (K1) and still reads back as the device it was (#334): name, class,
 * driver, path, PCI ids and description. At DETACH_COMPLETE newbus has not yet
 * cleared the devclass, driver or description, so the accessors still return
 * them. The entry then ages out with the other lingerers. DEPART is emitted
 * after the snapshot is in place, so a client that reads the id from its
 * notification callback gets the snapshot. There is no match bracketing around
 * a detach, so DEPART is emitted directly. BEGIN/FAILED are ignored (still
 * present).
 */
static void
ioreg_on_detach(void *arg __unused, device_t dev, enum evhdev_detach state)
{
	struct ioreg_ent *e;
	struct ioreg_virt *snap;
	struct ioreg_node n;
	const char *desc;
	uint64_t id, parent_id;

	if (state != EVHDEV_DETACH_COMPLETE)
		return;

	sx_xlock(&ioreg_lock);
	e = ioreg_find_dev_locked(dev);
	id = (e != NULL) ? e->id : 0;
	parent_id = 0;
	if (e != NULL) {
		device_t pp = device_get_parent(dev);
		struct ioreg_ent *pe;

		if (pp != NULL) {
			pe = ioreg_find_dev_locked(pp);
			parent_id = (pe != NULL) ? pe->id : 0;
		}
	}
	sx_xunlock(&ioreg_lock);
	if (id == 0)
		return;

	/* Build the snapshot unlocked (newbus reads, M_WAITOK allocations). */
	ioreg_fill_node(dev, id, parent_id, IOREG_STATE_DETACHED, &n);
	snap = malloc(sizeof(*snap), M_IOREG, M_WAITOK | M_ZERO);
	snap->node = n;
	desc = device_get_desc(dev);
	if (desc != NULL && desc[0] != '\0') {
		snap->props = nvlist_create(0);
		if (snap->props != NULL) {
			nvlist_add_string(snap->props, "description", desc);
			if (nvlist_error(snap->props) != 0) {
				nvlist_destroy(snap->props);
				snap->props = NULL;
			}
		}
	}

	sx_xlock(&ioreg_lock);
	e = ioreg_find_dev_locked(dev);
	if (e != NULL && e->id == id && e->virt == NULL) {
		e->dev = NULL;
		e->state = IOREG_STATE_DETACHED;
		e->virt = snap;
		snap = NULL;
		ioreg_linger_locked(e);
	}
	sx_xunlock(&ioreg_lock);
	if (snap != NULL) {
		/* Not ours to keep (the entry went away under us). */
		if (snap->props != NULL)
			nvlist_destroy(snap->props);
		free(snap, M_IOREG);
	}

	ioreg_emit_event_node(&n, desc, IOREG_EVENT_DEPART);
}

/* device_match_start: track probe->attach nesting depth (K-PR2). */
static void
ioreg_on_match_start(void *arg __unused, device_t dev __unused)
{
	mtx_lock(&ioreg_pend_mtx);
	ioreg_match_depth++;
	mtx_unlock(&ioreg_pend_mtx);
}

/*
 * device_match_end: on the outermost completion (depth 1->0) the device tree has
 * settled, so flush every pending ARRIVE. Splice the list out under the mutex,
 * then emit with no lock held. Each pending dev is re-resolved in the id map (it
 * is still live) for its id/parent_id. K-PR2 (#225).
 */
static void
ioreg_on_match_end(void *arg __unused, device_t dev __unused)
{
	STAILQ_HEAD(, ioreg_pend) drain = STAILQ_HEAD_INITIALIZER(drain);
	struct ioreg_pend *p;

	mtx_lock(&ioreg_pend_mtx);
	if (ioreg_match_depth > 0)
		ioreg_match_depth--;
	if (ioreg_match_depth == 0)
		STAILQ_CONCAT(&drain, &ioreg_pending);	/* take all pending */
	mtx_unlock(&ioreg_pend_mtx);

	while ((p = STAILQ_FIRST(&drain)) != NULL) {
		struct ioreg_ent *e;
		uint64_t id, parent_id;
		device_t pdev;

		STAILQ_REMOVE_HEAD(&drain, link);
		pdev = p->dev;

		if (pdev == NULL) {
			ioreg_virt_announce(p->vid);
			free(p, M_IOREG);
			continue;
		}

		sx_xlock(&ioreg_lock);
		e = ioreg_find_dev_locked(pdev);
		id = (e != NULL && e->dev == pdev) ? e->id : 0;
		parent_id = 0;
		if (id != 0) {
			device_t pp = device_get_parent(pdev);
			struct ioreg_ent *pe;

			if (pp != NULL) {
				pe = ioreg_find_dev_locked(pp);
				parent_id = (pe != NULL) ? pe->id :
				    ioreg_id_for_locked(pp);
			}
		}
		sx_xunlock(&ioreg_lock);

		/* Emit ARRIVE and MATCHED (attach implies a bound driver); a watch
		 * receives whichever kinds its mask selected. */
		if (id != 0) {
			ioreg_emit_event(pdev, id, parent_id, IOREG_STATE_LIVE,
			    IOREG_EVENT_ARRIVE);
			ioreg_emit_event(pdev, id, parent_id, IOREG_STATE_LIVE,
			    IOREG_EVENT_MATCHED);
		}
		free(p, M_IOREG);
	}
}

static int
ioreg_modevent(module_t mod __unused, int type, void *data __unused)
{
	struct ioreg_ent *e;
	struct ioreg_pend *p;

	switch (type) {
	case MOD_LOAD:
		ioreg_dev = make_dev(&ioreg_cdevsw, 0, UID_ROOT, GID_WHEEL,
		    0644, "ioregistry");
		if (ioreg_dev == NULL)
			return (ENXIO);
		ioreg_attach_tag = EVENTHANDLER_REGISTER(device_attach,
		    ioreg_on_attach, NULL, EVENTHANDLER_PRI_ANY);
		ioreg_detach_tag = EVENTHANDLER_REGISTER(device_detach,
		    ioreg_on_detach, NULL, EVENTHANDLER_PRI_ANY);
		ioreg_match_start_tag = EVENTHANDLER_REGISTER(device_match_start,
		    ioreg_on_match_start, NULL, EVENTHANDLER_PRI_ANY);
		ioreg_match_end_tag = EVENTHANDLER_REGISTER(device_match_end,
		    ioreg_on_match_end, NULL, EVENTHANDLER_PRI_ANY);
		return (0);
	case MOD_UNLOAD:
		if (ioreg_attach_tag != NULL)
			EVENTHANDLER_DEREGISTER(device_attach, ioreg_attach_tag);
		if (ioreg_detach_tag != NULL)
			EVENTHANDLER_DEREGISTER(device_detach, ioreg_detach_tag);
		if (ioreg_match_start_tag != NULL)
			EVENTHANDLER_DEREGISTER(device_match_start,
			    ioreg_match_start_tag);
		if (ioreg_match_end_tag != NULL)
			EVENTHANDLER_DEREGISTER(device_match_end,
			    ioreg_match_end_tag);
		if (ioreg_dev != NULL)
			destroy_dev(ioreg_dev);
		ioreg_watch_flush();
		mtx_lock(&ioreg_pend_mtx);
		while ((p = STAILQ_FIRST(&ioreg_pending)) != NULL) {
			STAILQ_REMOVE_HEAD(&ioreg_pending, link);
			free(p, M_IOREG);
		}
		mtx_unlock(&ioreg_pend_mtx);
		sx_xlock(&ioreg_lock);
		while ((e = LIST_FIRST(&ioreg_map)) != NULL) {
			LIST_REMOVE(e, link);
			ioreg_ent_free(e);
		}
		TAILQ_INIT(&ioreg_virt_live);
		TAILQ_INIT(&ioreg_linger);
		ioreg_nlinger = 0;
		ioreg_resources = NULL;
		sx_xunlock(&ioreg_lock);
		return (0);
	default:
		return (EOPNOTSUPP);
	}
}

static moduledata_t ioreg_mod = { "ioregistry", ioreg_modevent, NULL };
DECLARE_MODULE(ioregistry, ioreg_mod, SI_SUB_DRIVERS, SI_ORDER_ANY);
MODULE_VERSION(ioregistry, 1);
MODULE_DEPEND(ioregistry, iocatalogue, 1, 1, 1);	/* shares hw.iokit node */
