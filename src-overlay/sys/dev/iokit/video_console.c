/*
 * NextBSD video console (E19, nextbsd-kernel#407): initialize_screen().
 *
 * xnu's osfmk/console/video_console.c owns the console screen: who may draw
 * on it, when the boot spinner runs, and how the framebuffer record is
 * swapped. This is NextBSD's implementation of the same state machine, with
 * xnu's names and op semantics (<sys/pexpert.h>); no xnu code is copied.
 * vt(4) stays the text console underneath: it asks
 * PE_graphics_console_active() before it clears or draws, the analogue of
 * xnu's gc_enable(FALSE).
 *
 * State (xnu's names):
 *   gc_graphics_boot   kPEGraphicsMode was set: the booter left graphics
 *   gc_acquired        the console has an owner (kPEAcquireScreen)
 *   gc_desire_text     kPETextScreen arrived before the acquire
 *   vc_progress_enable the spinner runs
 *   vc_graphics_owner  graphics own the screen; vt(4) must not draw text
 *
 * Ops:
 *   kPEGraphicsMode/TextMode  boot mode (PE_init_console(), at cninit)
 *   kPEAcquireScreen          graphics boot: the splash owns the screen and
 *                             the spinner starts with no delay
 *   kPEDisableScreen/Enable   pause/resume the spinner around a framebuffer
 *                             change; the screen stays owned
 *   kPETextScreen             stop the spinner, hand the screen to text
 *   kPEReleaseScreen          a display server owns the screen: stop the
 *                             spinner and keep the pixels
 *   kPEBaseAddressChange      swap the record under the spinner's lock;
 *                             never clear. Unlike xnu, the geometry is taken
 *                             from the new record too: a NextBSD fbdev
 *                             buffer need not match the firmware mode.
 *   kPERefreshBootGraphics    repaint the boot graphics
 *
 * Locking: vc_progress_lock is a spin mutex, xnu's vc_progress_lock, and the
 * only lock here. It is never held across a call into vt(4), and vt(4)'s
 * locks are never taken under it. The panic path takes no lock.
 *
 * In text mode (-v, -s, serial console, no framebuffer) every op only
 * updates these flags, and vt(4) sees PE_graphics_console_active() false,
 * so its output is byte-identical to a kernel without this file.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/fbio.h>
#include <sys/sysctl.h>
#include <sys/pexpert.h>

#include <dev/iokit/video_console.h>

static struct mtx vc_progress_lock;
static bool vc_lock_inited;

static PE_Video vinfo;			/* the console record */
static struct fb_info *vc_fb;		/* its mapping, when known */

static bool gc_graphics_boot;
static bool gc_acquired;
static bool gc_desire_text;
static bool gc_paused_progress;
static bool vc_progress_enable;
static volatile int vc_graphics_owner;
static volatile int vc_panicked;
static bool vc_released;

#define	VC_LOCK()	do {					\
	if (vc_lock_inited)					\
		mtx_lock_spin(&vc_progress_lock);		\
} while (0)
#define	VC_UNLOCK()	do {					\
	if (vc_lock_inited)					\
		mtx_unlock_spin(&vc_progress_lock);		\
} while (0)

static void
vc_lock_init(void)
{
	if (vc_lock_inited)
		return;
	mtx_init(&vc_progress_lock, "vc_progress", NULL, MTX_SPIN);
	vc_lock_inited = true;
}

bool
PE_graphics_console_active(void)
{
	return (vc_graphics_owner != 0 && vc_panicked == 0);
}

/*
 * Start or stop the spinner (xnu's vc_progress_set()). The drawing lives in
 * the spinner ticket (nextbsd-kernel#408); here only the state changes.
 */
static void
vc_progress_set(bool enable, uint32_t delay __unused)
{
	VC_LOCK();
	vc_progress_enable = enable && vc_panicked == 0;
	VC_UNLOCK();
}

/* xnu's gc_enable(): text console on or off. */
static void
gc_enable(bool text)
{
	vc_graphics_owner = text ? 0 : 1;
}

/* xnu's gc_pause(): the spinner around a framebuffer change. */
static void
gc_pause(bool pause)
{
	VC_LOCK();
	if (pause) {
		gc_paused_progress = vc_progress_enable;
		vc_progress_enable = false;
	} else {
		vc_progress_enable = gc_paused_progress && vc_panicked == 0;
	}
	VC_UNLOCK();
}

void
vc_console_fb(struct fb_info *fb, int op)
{
	switch (op) {
	case kPEBaseAddressChange:
	case kPEEnableScreen:
		if (fb == NULL)
			break;
		VC_LOCK();
		vc_fb = fb;
		VC_UNLOCK();
		break;
	default:
		break;
	}
}

void
initialize_screen(PE_Video *info, unsigned int op)
{
	bool graphics_now;

	vc_lock_init();

	if (info != NULL) {
		/* Atomically with respect to the spinner, as in xnu. */
		VC_LOCK();
		vinfo = *info;
		if (vinfo.v_scale != kPEScaleFactor2x)
			vinfo.v_scale = kPEScaleFactor1x;
		VC_UNLOCK();
	}

	graphics_now = gc_graphics_boot && !gc_desire_text;
	switch (op) {
	case kPEGraphicsMode:
		gc_graphics_boot = true;
		gc_desire_text = false;
		break;

	case kPETextMode:
		gc_graphics_boot = false;
		break;

	case kPEAcquireScreen:
		if (gc_acquired)
			break;
		vc_progress_set(graphics_now, 0);
		gc_enable(!graphics_now);
		gc_acquired = true;
		gc_desire_text = false;
		break;

	case kPEDisableScreen:
		if (gc_acquired)
			gc_pause(true);
		break;

	case kPEEnableScreen:
		if (gc_acquired)
			gc_pause(false);
		break;

	case kPETextScreen:
		if (!gc_acquired) {
			gc_desire_text = true;
			break;
		}
		if (!gc_graphics_boot)
			break;
		vc_progress_set(false, 0);
		gc_enable(true);
		break;

	case kPEReleaseScreen:
		if (gc_acquired)
			vc_released = true;
		gc_acquired = false;
		gc_desire_text = false;
		vc_graphics_owner = 0;
		if (!gc_graphics_boot)
			break;
		vc_progress_set(false, 0);
		break;

	case kPERefreshBootGraphics:
	case kPEBaseAddressChange:
	default:
		break;
	}
}

/* No locks: the panic path may run with any lock held, on any CPU. */
void
vc_panic(void)
{
	vc_panicked = 1;
	vc_graphics_owner = 0;
	vc_progress_enable = false;
}

int
vc_sysctl_state(SYSCTL_HANDLER_ARGS)
{
	const char *s;

	if (vc_panicked)
		s = "panic";
	else if (!gc_graphics_boot)
		s = "text";
	else if (!gc_acquired)
		s = vc_released ? "released" : "boot";
	else if (!vc_graphics_owner)
		s = "text";
	else
		s = vc_progress_enable ? "graphics" : "paused";
	return (sysctl_handle_string(oidp, __DECONST(char *, s), 0, req));
}
