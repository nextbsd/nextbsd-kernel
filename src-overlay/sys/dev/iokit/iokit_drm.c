/*
 * NextBSD IORegistry DRM bridge (#338).
 *
 * Turns DRM connector hotplug into IORegistry interest notifications, so the
 * WindowServer and the IOKit libudev (for XLibre's RandR hotplug) can follow
 * display changes with IOServiceAddInterestNotification(kIOGeneralInterest)
 * on the GPU's registry entry instead of a devd pipe. NextBSD has no devd, so
 * the devctl_notify("DRM", "CONNECTOR", "HOTPLUG", ...) that drm-kmod also
 * sends reaches nobody.
 *
 * Hook. drm.ko (drm-kmod, out of tree, loaded as a kext) invokes the
 * drm_connector_hotplug eventhandler next to each of those devctl_notify
 * calls in drm_sysfs.c; the patch lives in nextbsd-kernel-extensions-graphics
 * graphics/drm-kmod-patches. EVENTHANDLER_INVOKE finds the list by name at
 * run time, so drm.ko links nothing from here and loads on a kernel without
 * this file. The declaration below is the contract and must stay identical
 * to the one in that patch:
 *
 *   dev           the newbus device the DRM driver attached to (drmn0 for
 *                 the drm-kmod PCI drivers), i.e. drm_device->dev->bsddev;
 *   cdev          the primary minor's name, "card0";
 *   connector_id  DRM connector object id, 0 for a whole-device hotplug;
 *   property_id   DRM property object id, 0 for a connection change.
 *
 * Entry and message. The target is that device's own newbus entry (class
 * drmn): it already exists, and the GPU coming and going is already
 * match/terminate. Each event becomes kIOMessageServicePropertyChange with
 * argument[0] = connector_id, argument[1] = property_id and devnode
 * "dri/cardN" (ioreg_message_device()).
 *
 * Context. drm-kmod sends these from driver context, sometimes with modeset
 * locks held (HDCP, privacy screen), like devctl_notify. The handler takes
 * only a mutex and allocates M_NOWAIT; a taskqueue_thread task does the
 * registry lookup and the Mach send. The queue is bounded and drops on
 * overflow; a client re-probes all connectors on the next message anyway.
 * The device_t is queued as a pointer and the registry only compares it
 * against its id map, so a device that detaches meanwhile matches nothing.
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
#include <sys/ioregistry.h>

/* Darwin's kIOMessageServicePropertyChange; == IOKIT_NOTIFY_MSG_SERVICE_
 * PROPERTY_CHANGE in <sys/mach/iokit_notify.h> (not included: no Mach
 * headers in the standard build). */
#define	IOKIT_DRM_MSG_PROPERTY_CHANGE	0xe0000130

#define	IOKIT_DRM_QUEUE_MAX	64
#define	IOKIT_DRM_NODE_MAX	32	/* == IOKIT_NOTIFY_NAME_MAX */

typedef void (*drm_connector_hotplug_fn)(void *, device_t, const char *,
    uint32_t, uint32_t);
EVENTHANDLER_DECLARE(drm_connector_hotplug, drm_connector_hotplug_fn);

static MALLOC_DEFINE(M_IOKITDRM, "iokit_drm", "IORegistry DRM bridge");

struct iokit_drm_ev {
	STAILQ_ENTRY(iokit_drm_ev) link;
	device_t	dev;		/* compared only, never dereferenced */
	uint32_t	connector_id;
	uint32_t	property_id;
	char		devnode[IOKIT_DRM_NODE_MAX];
};

static STAILQ_HEAD(, iokit_drm_ev) iokit_drm_queue =
    STAILQ_HEAD_INITIALIZER(iokit_drm_queue);
static u_int iokit_drm_queued;
static struct mtx iokit_drm_mtx;
MTX_SYSINIT(iokit_drm_mtx, &iokit_drm_mtx, "iokit drm", MTX_DEF);
static struct task iokit_drm_task;
static eventhandler_tag iokit_drm_tag;

static void
iokit_drm_deliver(void *arg __unused, int pending __unused)
{
	STAILQ_HEAD(, iokit_drm_ev) drain = STAILQ_HEAD_INITIALIZER(drain);
	struct iokit_drm_ev *ev;

	mtx_lock(&iokit_drm_mtx);
	STAILQ_CONCAT(&drain, &iokit_drm_queue);
	iokit_drm_queued = 0;
	mtx_unlock(&iokit_drm_mtx);

	while ((ev = STAILQ_FIRST(&drain)) != NULL) {
		STAILQ_REMOVE_HEAD(&drain, link);
		ioreg_message_device(ev->dev, IOKIT_DRM_MSG_PROPERTY_CHANGE,
		    ev->connector_id, ev->property_id, ev->devnode);
		free(ev, M_IOKITDRM);
	}
}

static void
iokit_drm_hotplug(void *arg __unused, device_t dev, const char *cdev,
    uint32_t connector_id, uint32_t property_id)
{
	struct iokit_drm_ev *ev;

	if (dev == NULL)
		return;
	ev = malloc(sizeof(*ev), M_IOKITDRM, M_NOWAIT | M_ZERO);
	if (ev == NULL)
		return;
	ev->dev = dev;
	ev->connector_id = connector_id;
	ev->property_id = property_id;
	if (cdev != NULL && cdev[0] != '\0')
		snprintf(ev->devnode, sizeof(ev->devnode), "dri/%s", cdev);

	mtx_lock(&iokit_drm_mtx);
	if (iokit_drm_queued >= IOKIT_DRM_QUEUE_MAX) {
		mtx_unlock(&iokit_drm_mtx);
		free(ev, M_IOKITDRM);
		return;
	}
	STAILQ_INSERT_TAIL(&iokit_drm_queue, ev, link);
	iokit_drm_queued++;
	mtx_unlock(&iokit_drm_mtx);
	taskqueue_enqueue(taskqueue_thread, &iokit_drm_task);
}

static void
iokit_drm_init(void *arg __unused)
{
	TASK_INIT(&iokit_drm_task, 0, iokit_drm_deliver, NULL);
	iokit_drm_tag = EVENTHANDLER_REGISTER(drm_connector_hotplug,
	    iokit_drm_hotplug, NULL, EVENTHANDLER_PRI_ANY);
}
SYSINIT(iokit_drm, SI_SUB_DRIVERS, SI_ORDER_ANY, iokit_drm_init, NULL);
