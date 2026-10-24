/*
 * NextBSD IORegistry HID bridge (#335, part of #331).
 *
 * Publishes every evdev input node (/dev/input/eventN), uinput nodes
 * included, as an IOHIDDevice entry in the in-kernel IORegistry, so userland
 * (the IOKit libudev, a WindowServer) can enumerate input devices with
 * IOServiceGetMatchingServices and follow hotplug with
 * IOServiceAddMatchingNotification (first-match / terminated) instead of a
 * devd pipe.
 *
 * Hook. evdev invokes the evdev_registered / evdev_unregistering
 * eventhandlers (patches/0068) with its device list sx held exclusive. An
 * early SYSINIT subscribes to both and then walks the devices that registered
 * before it ran (sysmouse, kbdmux) with evdev_foreach(). A device that
 * registers between the subscription and the walk is seen twice, so
 * publishing is idempotent: entries are keyed by the evdev_dev pointer, and a
 * second sighting of a published device is a no-op.
 *
 * Entry. Name "eventN", class IOHIDDevice. The parent is the driver's newbus
 * node when the evdev "phys" is that node's nameunit (hms0, hkbd0, ukbd0,
 * psm0, atkbd0, ...); anything else (kbdmux, sysmouse, uinput) goes under the
 * synthetic IOResources node, as IOHIDUserDevice does on Darwin. Properties
 * use the Apple IOHIDDevice / IOBSD keys plus org.nextbsd.* extras; the
 * registry adds its own keys (id, name, class, path, ...) on top. Everything
 * is copied at publish time: uinput bzero()s its evdev_dev after unregister,
 * so nothing here keeps a pointer into one except as an opaque lookup key.
 *
 * Locking. The handlers run with the evdev list sx held, under whatever the
 * registering driver holds: Giant / the newbus topology lock (kbdmux, atkbd,
 * psm, and every newbus attach/detach) or a uinput sx. They take only this
 * file's sx, the registry's sx/mutex and the Mach port locks (via
 * ioreg_publish_virtual / ioreg_retire_virtual), and allocate M_WAITOK, all
 * of which is legal there because evdev registration already sleeps
 * (make_dev_s(MAKEDEV_WAITOK), destroy_dev()). They never take Giant or a
 * driver lock and never (un)register an evdev device, per the rules in
 * patches/0068, so no taskqueue deferral is needed. Lock order:
 * Giant/driver -> evdev list -> iokit_hid_lock -> registry.
 *
 * The newbus parent is looked up only when the caller holds the topology lock
 * (Giant), so the device_t cannot go away under the lookup; otherwise (a
 * uinput client, whose phys is not a newbus nameunit anyway) the entry goes
 * under IOResources.
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
#include <sys/sx.h>
#include <sys/conf.h>
#include <sys/bus.h>
#include <sys/bitstring.h>
#include <sys/eventhandler.h>
#include <sys/nv.h>
#include <sys/ioregistry.h>

#include <dev/evdev/input.h>
#include <dev/evdev/evdev.h>
#include <dev/evdev/evdev_private.h>
#include <dev/hid/hid.h>
#include <dev/hid/hidbus.h>

#define	IOKIT_HID_CLASS		"IOHIDDevice"

static MALLOC_DEFINE(M_IOKITHID, "iokit_hid", "IORegistry evdev bridge");

/* One published evdev node. `evdev` is a lookup key only, never dereferenced
 * after the device unregisters. */
struct iokit_hid_ent {
	LIST_ENTRY(iokit_hid_ent) link;
	struct evdev_dev	*evdev;
	uint64_t		id;		/* registry id */
};

static LIST_HEAD(, iokit_hid_ent) iokit_hid_list =
    LIST_HEAD_INITIALIZER(iokit_hid_list);
static struct sx iokit_hid_lock;
SX_SYSINIT(iokit_hid_lock, &iokit_hid_lock, "iokit hid");

static eventhandler_tag iokit_hid_reg_tag;
static eventhandler_tag iokit_hid_unreg_tag;

static struct iokit_hid_ent *
iokit_hid_find_locked(struct evdev_dev *evdev)
{
	struct iokit_hid_ent *he;

	sx_assert(&iokit_hid_lock, SA_XLOCKED);
	LIST_FOREACH(he, &iokit_hid_list, link)
		if (he->evdev == evdev)
			return (he);
	return (NULL);
}

/*
 * The newbus device whose nameunit is `phys`, or NULL. Drivers set phys to
 * device_get_nameunit(dev) (atkbd builds the same "atkbd%d" by hand), so split
 * it into a name and a trailing unit and ask the devclass. Called only with
 * the topology lock held.
 */
static device_t
iokit_hid_phys_device(const char *phys)
{
	char name[NAMELEN];
	const char *p;
	devclass_t dc;
	device_t dev;
	size_t nlen;
	int unit;

	nlen = strnlen(phys, NAMELEN);
	if (nlen == 0 || nlen >= NAMELEN)
		return (NULL);
	p = phys + nlen;
	while (p > phys && p[-1] >= '0' && p[-1] <= '9')
		p--;
	if (p == phys || *p == '\0' || (p[0] == '0' && p[1] != '\0'))
		return (NULL);
	if (phys + nlen - p > 9)
		return (NULL);
	unit = (int)strtoul(p, NULL, 10);
	memcpy(name, phys, p - phys);
	name[p - phys] = '\0';

	dc = devclass_find(name);
	if (dc == NULL)
		return (NULL);
	dev = devclass_get_device(dc, unit);
	if (dev == NULL || device_get_state(dev) < DS_ALIVE ||
	    strcmp(device_get_nameunit(dev), phys) != 0)
		return (NULL);
	return (dev);
}

/* Any bit set in [start, end) of a bitstring sized at least `end` bits. */
static bool
iokit_hid_any(bitstr_t *bits, int start, int end)
{
	int r;

	bit_ffs_at(bits, start, end, &r);
	return (r != -1);
}

/*
 * Primary usage from the capability bits, for nodes that are not hidbus
 * children (PS/2, kbdmux, sysmouse, uinput, ...). This is libudev-devd 0.7.0's
 * classification (create_evdev_handler, itself xf86-input-evdev's EvdevProbe),
 * so libinput keeps seeing the device type it sees today, with each type
 * mapped to the HID usage the IOKit libudev maps back to the same ID_INPUT_*
 * tags. Returns false (no usage keys) for a switch-only or unclassified node.
 */
static bool
iokit_hid_classify(struct evdev_dev *evdev, uint32_t *pagep, uint32_t *usagep)
{
	bitstr_t *key = evdev->ev_key_flags;
	bitstr_t *abs = evdev->ev_abs_flags;
	bitstr_t *rel = evdev->ev_rel_flags;
	bitstr_t *prop = evdev->ev_prop_flags;
	bool has_keys, has_buttons, has_lmr, has_dpad, has_joy_axes;
	bool has_rel_axes, has_abs_axes, has_mt;

	has_keys = iokit_hid_any(key, 0, BTN_MISC);
	has_buttons = iokit_hid_any(key, BTN_MISC, BTN_JOYSTICK);
	has_lmr = iokit_hid_any(key, BTN_LEFT, BTN_MIDDLE + 1);
	has_dpad = iokit_hid_any(key, BTN_DPAD_UP, BTN_DPAD_RIGHT + 1);
	has_joy_axes = iokit_hid_any(abs, ABS_RX, ABS_HAT3Y + 1);
	has_rel_axes = iokit_hid_any(rel, 0, REL_CNT);
	has_abs_axes = iokit_hid_any(abs, 0, ABS_CNT);
	has_mt = iokit_hid_any(abs, ABS_MT_SLOT, ABS_CNT);

#define	USAGE(pg, u)	do { *pagep = (pg); *usagep = (u); return (true); } \
			while (0)
	if (has_abs_axes) {
		if (has_mt && !has_buttons) {
			if (bit_test(key, BTN_JOYSTICK))
				USAGE(HUP_GENERIC_DESKTOP, HUG_JOYSTICK);
			has_buttons = true;
		}
		if (bit_test(abs, ABS_X) && bit_test(abs, ABS_Y)) {
			if (bit_test(key, BTN_TOOL_PEN) ||
			    bit_test(key, BTN_STYLUS) ||
			    bit_test(key, BTN_STYLUS2))
				USAGE(HUP_DIGITIZERS, HUD_PEN);
			if (has_joy_axes || bit_test(key, BTN_JOYSTICK))
				USAGE(HUP_GENERIC_DESKTOP, HUG_JOYSTICK);
			if (bit_test(key, BTN_SOUTH) || has_dpad ||
			    bit_test(abs, ABS_HAT0X) ||
			    bit_test(abs, ABS_HAT0Y) ||
			    bit_test(key, BTN_THUMBL) ||
			    bit_test(key, BTN_THUMBR))
				USAGE(HUP_GENERIC_DESKTOP, HUG_JOYSTICK);
			if (bit_test(abs, ABS_PRESSURE) ||
			    bit_test(key, BTN_TOUCH)) {
				if (has_lmr || bit_test(key, BTN_TOOL_FINGER))
					USAGE(HUP_DIGITIZERS, HUD_TOUCHPAD);
				USAGE(HUP_DIGITIZERS, HUD_TOUCHSCREEN);
			}
			if (!(bit_test(rel, REL_X) && bit_test(rel, REL_Y)) &&
			    has_lmr)
				USAGE(HUP_GENERIC_DESKTOP, HUG_MOUSE);
		}
	}

	if (bit_test(prop, INPUT_PROP_ACCELEROMETER))
		USAGE(0x20, 0x73);	/* Sensors: Motion: Accelerometer 3D */
	if (has_keys)
		USAGE(HUP_GENERIC_DESKTOP, HUG_KEYBOARD);
	if (bit_test(prop, INPUT_PROP_POINTER) || has_rel_axes ||
	    has_abs_axes || has_buttons)
		USAGE(HUP_GENERIC_DESKTOP, HUG_MOUSE);
#undef USAGE
	return (false);
}

/* kIOHIDTransportKey value for an evdev bus type, or NULL to omit the key. */
static const char *
iokit_hid_transport(uint16_t bustype)
{
	switch (bustype) {
	case BUS_USB:		return ("USB");
	case BUS_BLUETOOTH:	return ("Bluetooth");
	case BUS_I2C:		return ("I2C");
	case BUS_SPI:		return ("SPI");
	case BUS_VIRTUAL:	return ("Virtual");
	case BUS_I8042:		return ("PS2");	/* NextBSD; Apple has none */
	case BUS_RS232:		return ("Serial");
	default:		return (NULL);
	}
}

/* Publish `evdev` unless it already is. Called with the evdev list held. */
static void
iokit_hid_publish(struct evdev_dev *evdev)
{
	struct iokit_hid_ent *he;
	const char *bsdname, *transport;
	device_t parent;
	nvlist_t *props;
	char name[IOREG_NAME_MAX], devnode[IOREG_PATH_MAX];
	uint32_t page, usage;
	int32_t tlc;
	dev_t udev;
	bool have_usage;

	if (evdev->ev_cdev == NULL)
		return;

	sx_xlock(&iokit_hid_lock);
	if (iokit_hid_find_locked(evdev) != NULL) {
		/* Seen by both the handler and the catch-up walk. */
		sx_xunlock(&iokit_hid_lock);
		return;
	}

	/* Parent: the driver's newbus node, looked up only under the
	 * topology lock; else NULL, i.e. IOResources. */
	parent = NULL;
	if (mtx_owned(bus_topo_mtx()))
		parent = iokit_hid_phys_device(evdev->ev_shortname);

	/* Primary usage: the HID top-level collection on hidbus, else the
	 * libudev-devd classification of the capability bits. */
	have_usage = false;
	if (parent != NULL && device_get_parent(parent) != NULL &&
	    strcmp(device_get_name(device_get_parent(parent)),
	    "hidbus") == 0) {
		tlc = hidbus_get_usage(parent);
		page = HID_GET_USAGE_PAGE(tlc);
		usage = HID_GET_USAGE(tlc);
		have_usage = (page != 0);
	}
	if (!have_usage)
		have_usage = iokit_hid_classify(evdev, &page, &usage);

	bsdname = devtoname(evdev->ev_cdev);		/* "input/eventN" */
	udev = dev2udev(evdev->ev_cdev);
	snprintf(name, sizeof(name), "event%d", evdev->ev_unit);
	snprintf(devnode, sizeof(devnode), "/dev/%s", bsdname);
	transport = iokit_hid_transport(evdev->ev_id.bustype);

	props = nvlist_create(0);
	nvlist_add_string(props, "Product", evdev->ev_name);
	nvlist_add_number(props, "VendorID", evdev->ev_id.vendor);
	nvlist_add_number(props, "ProductID", evdev->ev_id.product);
	nvlist_add_number(props, "VersionNumber", evdev->ev_id.version);
	if (transport != NULL)
		nvlist_add_string(props, "Transport", transport);
	if (evdev->ev_serial[0] != '\0')
		nvlist_add_string(props, "SerialNumber", evdev->ev_serial);
	if (have_usage) {
		nvlist_add_number(props, "PrimaryUsagePage", page);
		nvlist_add_number(props, "PrimaryUsage", usage);
	}
	nvlist_add_string(props, "BSD Name", bsdname);
	nvlist_add_number(props, "BSD Major", major(udev));
	nvlist_add_number(props, "BSD Minor", minor(udev));
	nvlist_add_number(props, "BSD Unit", evdev->ev_unit);
	nvlist_add_string(props, "org.nextbsd.DevNode", devnode);
	nvlist_add_number(props, "org.nextbsd.evdev.BusType",
	    evdev->ev_id.bustype);
	nvlist_add_string(props, "org.nextbsd.evdev.Phys",
	    evdev->ev_shortname);

	he = malloc(sizeof(*he), M_IOKITHID, M_WAITOK | M_ZERO);
	he->evdev = evdev;
	/* The registry takes ownership of props, even on failure. */
	he->id = ioreg_publish_virtual(parent, name, IOKIT_HID_CLASS, props);
	if (he->id == 0) {
		free(he, M_IOKITHID);
		sx_xunlock(&iokit_hid_lock);
		return;
	}
	LIST_INSERT_HEAD(&iokit_hid_list, he, link);
	sx_xunlock(&iokit_hid_lock);
}

static void
iokit_hid_registered(void *arg __unused, struct evdev_dev *evdev)
{
	iokit_hid_publish(evdev);
}

/* Retire the entry (DEPART to watchers); it lingers readable for a while. */
static void
iokit_hid_unregistering(void *arg __unused, struct evdev_dev *evdev)
{
	struct iokit_hid_ent *he;

	sx_xlock(&iokit_hid_lock);
	he = iokit_hid_find_locked(evdev);
	if (he != NULL)
		LIST_REMOVE(he, link);
	sx_xunlock(&iokit_hid_lock);
	if (he == NULL)
		return;
	(void)ioreg_retire_virtual(he->id);
	free(he, M_IOKITHID);
}

static void
iokit_hid_catchup(struct evdev_dev *evdev, void *arg __unused)
{
	iokit_hid_publish(evdev);
}

/*
 * Subscribe first, then walk: a device registering in between is seen by
 * both, which iokit_hid_publish() tolerates. Runs before sysmouse's
 * SYSINIT and before newbus attaches atkbd/psm/hidbus children; kbdmux and
 * anything else already registered are caught up by the walk. The topology
 * lock (Giant, already held by mi_startup, so this recurses) is taken around
 * the walk so the parent lookup applies to it as to a newbus attach.
 */
static void
iokit_hid_init(void *arg __unused)
{
	iokit_hid_reg_tag = EVENTHANDLER_REGISTER(evdev_registered,
	    iokit_hid_registered, NULL, EVENTHANDLER_PRI_ANY);
	iokit_hid_unreg_tag = EVENTHANDLER_REGISTER(evdev_unregistering,
	    iokit_hid_unregistering, NULL, EVENTHANDLER_PRI_ANY);
	bus_topo_lock();
	evdev_foreach(iokit_hid_catchup, NULL);
	bus_topo_unlock();
}
SYSINIT(iokit_hid, SI_SUB_DRIVERS, SI_ORDER_FIRST, iokit_hid_init, NULL);
