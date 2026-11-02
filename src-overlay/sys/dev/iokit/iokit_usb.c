/*
 * NextBSD IORegistry USB bridge (nextbsd-kernel#391).
 *
 * Publishes every USB device as an IOUSBHostDevice entry in the in-kernel
 * IORegistry, whether or not a driver claims it, so userland can find a
 * device with IOServiceGetMatchingServices and follow hotplug with
 * IOServiceAddMatchingNotification (first-match / terminated) instead of a
 * devd pipe. The motivating case is a Bluetooth controller in bootloader
 * mode (Intel ibt, Realtek): no driver attaches to it, so it never becomes a
 * newbus device, and bluetoothd must see it to download its firmware. On
 * Darwin every USB device has its IOUSBHostDevice nub for the same reason.
 *
 * Hook. usb_alloc_device() and usb_free_device() invoke the usb_dev_attached
 * and usb_dev_detaching eventhandlers (patches/0087) at the same points as
 * the devctl ATTACH / DETACH events: after the device is enumerated and
 * configured, before its interfaces are probed, and before it is
 * unconfigured. This file is compiled in with the USB stack and subscribes
 * from a SYSINIT at SI_SUB_DRIVERS, before any USB controller attaches
 * (SI_SUB_CONFIGURE), so no device can arrive unseen and no catch-up walk is
 * needed.
 *
 * Entry. Name: the ugen name ("ugen0.2"), unique while the device is present
 * and what libusb and the firmware loaders take. Class: IOUSBHostDevice.
 * Parent: the hub's newbus node (uhubN), or usbusN for a root hub. The
 * properties use Darwin's IOUSBHostDevice keys, all numbers as numbers so a
 * matching dictionary's idVendor compares as an integer, plus the class
 * triple of the first interface and org.nextbsd.* extras. Everything is
 * copied at publish time; the usb_device pointer is only a lookup key.
 *
 * ARRIVE goes out before the interfaces are probed, so a client cannot tell
 * from the notification alone whether a driver will claim the device; it can
 * look for the driver's entry under the same hub. Host mode only: a device
 * side (gadget) port is not a device this host enumerated.
 *
 * Locking. The handlers run in the USB explore thread (or the bus attach and
 * detach paths) holding the device's enumeration sx and usually Giant, which
 * may be held across a sleep. They take only this file's sx and the
 * registry's sx/mutex and the Mach port locks (via ioreg_publish_virtual /
 * ioreg_retire_virtual), and allocate M_WAITOK. They never configure or free
 * a USB device. The parent device_t is read only at attach, when the hub is
 * attached and exploring. Lock order: USB enum sx / Giant -> iokit_usb_lock
 * -> registry.
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
#include <sys/condvar.h>
#include <sys/callout.h>
#include <sys/bus.h>
#include <sys/eventhandler.h>
#include <sys/nv.h>
#include <sys/ioregistry.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usb_core.h>
#include <dev/usb/usb_process.h>
#include <dev/usb/usb_device.h>
#include <dev/usb/usb_busdma.h>
#include <dev/usb/usb_controller.h>
#include <dev/usb/usb_bus.h>

#define	IOKIT_USB_CLASS		"IOUSBHostDevice"

/* locationID: bus in the top byte, then one nibble per hub port. */
#define	IOKIT_USB_LOC_LEVELS	6

static MALLOC_DEFINE(M_IOKITUSB, "iokit_usb", "IORegistry USB bridge");

/* One published USB device. `udev` is a lookup key only. */
struct iokit_usb_ent {
	LIST_ENTRY(iokit_usb_ent) link;
	struct usb_device	*udev;
	uint64_t		id;		/* registry id */
};

static LIST_HEAD(, iokit_usb_ent) iokit_usb_list =
    LIST_HEAD_INITIALIZER(iokit_usb_list);
static struct sx iokit_usb_lock;
SX_SYSINIT(iokit_usb_lock, &iokit_usb_lock, "iokit usb");

static eventhandler_tag iokit_usb_attach_tag;
static eventhandler_tag iokit_usb_detach_tag;

static struct iokit_usb_ent *
iokit_usb_find_locked(struct usb_device *udev)
{
	struct iokit_usb_ent *ue;

	sx_assert(&iokit_usb_lock, SA_XLOCKED);
	LIST_FOREACH(ue, &iokit_usb_list, link)
		if (ue->udev == udev)
			return (ue);
	return (NULL);
}

/*
 * Darwin's locationID: the bus number in bits 31-24, then the port on each
 * hub from the root down, one nibble each from bits 23-20. A root hub has
 * only its bus number. Ports above 15 and hubs deeper than six levels do not
 * fit and are cut.
 */
static uint32_t
iokit_usb_location(struct usb_device *udev)
{
	struct usb_device *d;
	uint8_t ports[USB_HUB_MAX_DEPTH + 1];
	uint32_t loc;
	int n, i;

	n = 0;
	for (d = udev; d != NULL && d->parent_hub != NULL &&
	    n < (int)nitems(ports); d = d->parent_hub)
		ports[n++] = d->port_no;
	loc = (uint32_t)(device_get_unit(udev->bus->bdev) & 0xff) << 24;
	for (i = 0; i < n && i < IOKIT_USB_LOC_LEVELS; i++)
		loc |= (uint32_t)(ports[n - 1 - i] & 0xf) << (20 - 4 * i);
	return (loc);
}

/* Darwin's "Device Speed" (kUSBDeviceSpeed*) for a USB stack speed. */
static uint32_t
iokit_usb_speed(enum usb_dev_speed speed)
{
	switch (speed) {
	case USB_SPEED_LOW:	return (0);
	case USB_SPEED_FULL:	return (1);
	case USB_SPEED_HIGH:	return (2);
	case USB_SPEED_SUPER:	return (3);
	default:		return (1);
	}
}

/* Publish `udev` unless it already is. */
static void
iokit_usb_attached(void *arg __unused, struct usb_device *udev)
{
	struct iokit_usb_ent *ue;
	struct usb_interface *iface;
	struct usb_device_descriptor *dd;
	device_t parent;
	nvlist_t *props;
	char name[IOREG_NAME_MAX], devnode[IOREG_PATH_MAX];

	if (udev == NULL || udev->flags.usb_mode != USB_MODE_HOST)
		return;

	sx_xlock(&iokit_usb_lock);
	if (iokit_usb_find_locked(udev) != NULL) {
		sx_xunlock(&iokit_usb_lock);
		return;
	}

#if USB_HAVE_UGEN
	strlcpy(name, udev->ugen_name, sizeof(name));
#else
	snprintf(name, sizeof(name), "ugen%u.%u",
	    device_get_unit(udev->bus->bdev), udev->address);
#endif
	snprintf(devnode, sizeof(devnode), "/dev/%s", name);
	parent = udev->parent_dev;
	dd = &udev->ddesc;

	props = nvlist_create(0);
	nvlist_add_number(props, "idVendor", UGETW(dd->idVendor));
	nvlist_add_number(props, "idProduct", UGETW(dd->idProduct));
	nvlist_add_number(props, "bcdDevice", UGETW(dd->bcdDevice));
	nvlist_add_number(props, "bcdUSB", UGETW(dd->bcdUSB));
	nvlist_add_number(props, "bDeviceClass", dd->bDeviceClass);
	nvlist_add_number(props, "bDeviceSubClass", dd->bDeviceSubClass);
	nvlist_add_number(props, "bDeviceProtocol", dd->bDeviceProtocol);
	nvlist_add_number(props, "bNumConfigurations",
	    dd->bNumConfigurations);
	/* The device's class is often 0 (per interface): give the first's. */
	iface = usbd_get_iface(udev, 0);
	if (iface != NULL && iface->idesc != NULL) {
		nvlist_add_number(props, "bInterfaceClass",
		    iface->idesc->bInterfaceClass);
		nvlist_add_number(props, "bInterfaceSubClass",
		    iface->idesc->bInterfaceSubClass);
		nvlist_add_number(props, "bInterfaceProtocol",
		    iface->idesc->bInterfaceProtocol);
	}
	nvlist_add_number(props, "USB Address", udev->address);
	nvlist_add_number(props, "locationID", iokit_usb_location(udev));
	nvlist_add_number(props, "PortNum", udev->port_no);
	nvlist_add_number(props, "Device Speed", iokit_usb_speed(udev->speed));
	if (udev->product != NULL && udev->product[0] != '\0')
		nvlist_add_string(props, "USB Product Name", udev->product);
	if (udev->manufacturer != NULL && udev->manufacturer[0] != '\0')
		nvlist_add_string(props, "USB Vendor Name",
		    udev->manufacturer);
	if (udev->serial != NULL && udev->serial[0] != '\0')
		nvlist_add_string(props, "USB Serial Number", udev->serial);
	nvlist_add_string(props, "org.nextbsd.ugen", name);
	nvlist_add_string(props, "org.nextbsd.DevNode", devnode);

	ue = malloc(sizeof(*ue), M_IOKITUSB, M_WAITOK | M_ZERO);
	ue->udev = udev;
	/* The registry takes ownership of props, even on failure. */
	ue->id = ioreg_publish_virtual(parent, name, IOKIT_USB_CLASS, props);
	if (ue->id == 0) {
		free(ue, M_IOKITUSB);
		sx_xunlock(&iokit_usb_lock);
		return;
	}
	LIST_INSERT_HEAD(&iokit_usb_list, ue, link);
	sx_xunlock(&iokit_usb_lock);
}

/* Retire the entry (DEPART to watchers); it lingers readable for a while. */
static void
iokit_usb_detaching(void *arg __unused, struct usb_device *udev)
{
	struct iokit_usb_ent *ue;

	sx_xlock(&iokit_usb_lock);
	ue = iokit_usb_find_locked(udev);
	if (ue != NULL)
		LIST_REMOVE(ue, link);
	sx_xunlock(&iokit_usb_lock);
	if (ue == NULL)
		return;
	(void)ioreg_retire_virtual(ue->id);
	free(ue, M_IOKITUSB);
}

static void
iokit_usb_init(void *arg __unused)
{
	iokit_usb_attach_tag = EVENTHANDLER_REGISTER(usb_dev_attached,
	    iokit_usb_attached, NULL, EVENTHANDLER_PRI_ANY);
	iokit_usb_detach_tag = EVENTHANDLER_REGISTER(usb_dev_detaching,
	    iokit_usb_detaching, NULL, EVENTHANDLER_PRI_ANY);
}
SYSINIT(iokit_usb, SI_SUB_DRIVERS, SI_ORDER_FIRST, iokit_usb_init, NULL);
