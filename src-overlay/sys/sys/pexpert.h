/*
 * NextBSD platform expert: the console record (E19, nextbsd-kernel#407).
 *
 * As in xnu (pexpert/pexpert/pexpert.h), one record describes the console
 * framebuffer, PE_state.video, and PE_initialize_console(video, op) is the
 * only way anything changes it or the console's state. The names, the op
 * values and their meaning are xnu's; the code is NextBSD's own
 * (sys/dev/iokit/iokit_platform.c plays IOPlatformExpert's console role,
 * sys/dev/iokit/video_console.c holds initialize_screen()).
 *
 * Boot. cninit() calls PE_init_console() right after patch 0062's mute
 * decision, before any console driver probes. It builds PE_state.video from
 * the loader's struct nbsd_boot_video (<sys/nbsd_boot_video.h>), else from
 * the stock MODINFOMD_EFI_FB record, and puts the console in
 * kPEGraphicsMode or kPETextMode, xnu's PE_create_console(). Text is chosen
 * for -v, -s, -a, any serial console (RB_SERIAL, RB_MULTIPLE, boot_serial,
 * boot_multicons, "comconsole" in console), no framebuffer, or a loader
 * record that says FB_TEXT_MODE. Text mode is inert: vt(4) behaves exactly
 * as it does without this layer, byte for byte.
 *
 * NextBSD differences from xnu, all additive:
 *   - PE_state.video.v_baseAddr is always the PHYSICAL base. The kernel
 *     mapping belongs to the framebuffer's struct fb_info (<sys/fbio.h>),
 *     which every NextBSD framebuffer driver already has, so the console
 *     ops that carry a new framebuffer pass that fb_info
 *     (PE_initialize_console_fb() and the pe_console_op eventhandler).
 *   - The splash style comes from the kenv bootsplash_style
 *     ("black" | "grey"; unset means black), PE_state.style.
 *
 * GPL kexts (drm.ko) never link against this code: they invoke the
 * eventhandlers below BY NAME, the same contract as
 * sys/dev/iokit/iokit_drm.c and drm-kmod-patch 0007 in
 * nextbsd-kernel-extensions-graphics. EVENTHANDLER_INVOKE finds a list by
 * its name at run time, so drm.ko still loads, and the invoke is a no-op, on
 * a kernel without these handlers. A kext repeats the typedef and the
 * EVENTHANDLER_DECLARE lines exactly as written here.
 */

#ifndef _SYS_PEXPERT_H_
#define	_SYS_PEXPERT_H_

/* v_display: set by the booter (xnu pexpert/i386/boot.h). */
#define	GRAPHICS_MODE		1
#define	FB_TEXT_MODE		2

/* Scale factor values for PE_Video.v_scale. */
enum {
	kPEScaleFactorUnknown	= 0,
	kPEScaleFactor1x	= 1,
	kPEScaleFactor2x	= 2
};

/* xnu's struct PE_Video, field for field (LP64 layout). */
struct PE_Video {
	unsigned long	v_baseAddr;	/* physical base of video memory */
	unsigned long	v_rowBytes;	/* bytes per pixel row */
	unsigned long	v_width;	/* width in pixels */
	unsigned long	v_height;	/* height in pixels */
	unsigned long	v_depth;	/* bits per pixel */
	unsigned long	v_display;	/* GRAPHICS_MODE or FB_TEXT_MODE */
	char		v_pixelFormat[64];
	unsigned long	v_offset;	/* offset into video memory to start at */
	unsigned long	v_length;	/* 0 for v_rowBytes * v_height */
	unsigned char	v_rotate;	/* 0 normal, 1 right 90, 2 180, 3 left 90 */
	unsigned char	v_scale;	/* kPEScaleFactor* */
	char		reserved1[2];
	long		reserved2;
};
typedef struct PE_Video PE_Video;

/* PE_initialize_console() / initialize_screen() ops: xnu's values. */
#define	kPEGraphicsMode		1
#define	kPETextMode		2
#define	kPETextScreen		3
#define	kPEAcquireScreen	4
#define	kPEReleaseScreen	5
#define	kPEEnableScreen		6
#define	kPEDisableScreen	7
#define	kPEBaseAddressChange	8
#define	kPERefreshBootGraphics	9

/* PE_state.style: the kenv bootsplash_style. */
#define	PE_STYLE_BLACK		0	/* "black" or unset: bg #000000, white spinner */
#define	PE_STYLE_GREY		1	/* "grey": bg #5F6770, dark spinner */

#define	PE_STYLE_BLACK_BG	0x000000
#define	PE_STYLE_BLACK_FG	0xFFFFFF
#define	PE_STYLE_GREY_BG	0x5F6770
#define	PE_STYLE_GREY_FG	0x000000

#ifdef _KERNEL

#include <sys/types.h>
#include <sys/bus.h>
#include <sys/eventhandler.h>

struct fb_info;
struct nbsd_boot_video;

typedef struct PE_state {
	boolean_t	initialized;
	PE_Video	video;
	/* NextBSD additions. */
	int		style;		/* PE_STYLE_* */
	uint32_t	bg_rgb;		/* 0x00RRGGBB */
	uint32_t	fg_rgb;		/* spinner colour, 0x00RRGGBB */
	/* The loader's record, or NULL (preload memory, never freed). */
	const struct nbsd_boot_video *boot_video;
} PE_state_t;

extern PE_state_t PE_state;

/*
 * cninit(): build PE_state.video and enter the boot mode. `mode' is
 * GRAPHICS_MODE or FB_TEXT_MODE as decided from boothowto and the
 * serial-console kenv (PE_serial_console_requested()); it is downgraded to
 * FB_TEXT_MODE with no framebuffer or a loader record that says text.
 */
void	PE_init_console(int mode);
int	PE_serial_console_requested(void);

/* xnu's entry points. */
int	PE_initialize_console(PE_Video *info, int op);
int	PE_current_console(PE_Video *info);
void	initialize_screen(PE_Video *info, unsigned int op);

/*
 * NextBSD: PE_initialize_console() for an op that carries a framebuffer.
 * Fills a PE_Video from `fb' (NULL: no record change) and remembers `fb' as
 * the console's mapping for kPEBaseAddressChange, kPEEnableScreen and
 * kPERefreshBootGraphics. This is what the pe_console_op eventhandler
 * calls.
 */
int	PE_initialize_console_fb(struct fb_info *fb, int op);

/*
 * True while graphics own the screen and vt(4) must neither clear it nor
 * draw text (xnu's gc_enable(FALSE)): from kPEAcquireScreen on a graphics
 * boot until kPETextScreen or a panic. It stays true after
 * kPEReleaseScreen, as in xnu, since the display server owns the pixels.
 * Always false in text mode. Lock-free; safe from any context.
 */
bool	PE_graphics_console_active(void);

/* The boot mode PE_init_console() chose: GRAPHICS_MODE or FB_TEXT_MODE. */
int	PE_boot_display(void);

/*
 * vt(4) glue (NextBSD).
 *
 * PE_boot_console_fb(): vt_fb_init() offers each framebuffer it brings up
 * instead of blanking it. When it is the booter's framebuffer on a graphics
 * boot, the platform expert takes it as the console framebuffer, sends
 * kPEAcquireScreen (paint the whole splash, start the spinner) and returns
 * true: vt(4) must not blank it. Otherwise false, and vt(4) blanks as
 * always.
 *
 * PE_console_text_generation(): bumped each time graphics hand the screen
 * back to text (kPETextScreen, panic). vt(4) redraws the whole screen
 * (VDF_INVALID) when it sees a new value, so no splash pixels remain.
 *
 * PE_set_console_flush(): a shadow-buffered framebuffer (virtio_gpu, a
 * damage-tracked fbdev) registers how to push a rectangle to the screen;
 * the spinner calls it after every frame and the painter after a full
 * paint, from a callout or the op's caller, without the spinner's lock.
 * It must not sleep. NULL unregisters.
 */
bool	PE_boot_console_fb(struct fb_info *fb);
u_int	PE_console_text_generation(void);
typedef void pe_fb_flush_t(struct fb_info *fb, u_int x, u_int y, u_int w,
	    u_int h);
void	PE_set_console_flush(struct fb_info *fb, pe_fb_flush_t *flush);

/*
 * Leave graphics for good so a prompt can be seen: clear the console mute
 * (cn_mute), send kPETextScreen on a graphics boot, and when the console
 * was muted, show the prompt line it swallowed. cngets() calls it (the
 * geli and askname prompts) and so does the mountroot prompt. A no-op on
 * an unmuted text boot. Thread context.
 */
void	PE_enter_text_console(void);

/*
 * Panic and fatal-trap path: stop drawing graphics and let text through.
 * Takes no locks and never sleeps; safe to call more than once.
 */
void	PE_panic_console(void);

/*
 * ---------------------------------------------------------------------------
 * Eventhandlers invoked BY NAME from kexts (drm.ko). Contract: keep these two
 * typedefs, the names and the argument meaning identical on both sides.
 *
 * pe_console_op(arg, fb, op)
 *   The framebuffer driver's console ops, xnu IOFramebuffer's
 *   setConsoleInfo(): one call per op, op being one of the kPE* values.
 *     fb  the FreeBSD struct fb_info (<sys/fbio.h>) of the framebuffer the op
 *         is about. In drm-kmod that is &linux_fb_info->fbio, the same
 *         fb_info register_framebuffer() hands to vt(4). NULL where noted.
 *   Ops a driver sends:
 *     kPEDisableScreen (7)        fb about to stop being safe to write (a
 *                                 freeze, FB_FLAG_NOWRITE, a mode change):
 *                                 pause the spinner, stop touching the old
 *                                 mapping before this returns. fb may be NULL.
 *     kPEEnableScreen (6)         writes are safe again; resume. fb, when not
 *                                 NULL, becomes the console framebuffer.
 *     kPEBaseAddressChange (8)    the console moved to fb (a new fbdev
 *                                 buffer, same or new geometry): swap the
 *                                 record under the spinner's lock, never clear.
 *     kPERefreshBootGraphics (9)  paint the boot graphics (background, logo,
 *                                 current spinner frame) into fb, or into the
 *                                 console framebuffer when fb is NULL, e.g. to
 *                                 pre-fill a new fbdev buffer before its first
 *                                 scanout. Does not change the record.
 *     kPEReleaseScreen (5)        a client owns the screen now: stop the
 *                                 spinner, keep the pixels. fb may be NULL.
 *   Any other op is ignored. The handler takes only a spin mutex and never
 *   sleeps, so it may be invoked with sleep mutexes or sx locks held (DRM
 *   modeset locks, the console lock), but not with a spin lock held or from
 *   an interrupt filter (EVENTHANDLER_INVOKE itself takes a default mutex).
 *   With the console in text mode every op is a no-op.
 *
 * pe_drm_master(arg, dev, acquired)
 *   A DRM device gained (acquired = 1) or lost (acquired = 0) its master,
 *   i.e. a display server took or released the screen.
 *     dev  the newbus device the DRM driver attached to
 *          (drm_device->dev->bsddev), as for drm_connector_hotplug.
 *   The kernel turns the first acquire into kPEReleaseScreen (keep the
 *   pixels, stop the spinner), as IOFramebuffer does when the WindowServer
 *   opens it. Same context rules as pe_console_op.
 * ---------------------------------------------------------------------------
 */
typedef void (*pe_console_op_fn)(void *, struct fb_info *, int);
EVENTHANDLER_DECLARE(pe_console_op, pe_console_op_fn);

typedef void (*pe_drm_master_fn)(void *, device_t, int);
EVENTHANDLER_DECLARE(pe_drm_master, pe_drm_master_fn);

/*
 * pe_tty_ctty(arg, tp)
 *   Kernel-internal (not for kexts): a session leader made tp its
 *   controlling terminal (TIOCSCTTY; getty's login_tty()). Invoked from
 *   ttydev_ioctl() after a successful TIOCSCTTY, with no tty lock held.
 *   vt(4) turns it into kPETextScreen when tp is the splash VT, the text
 *   console taking the screen. Nothing else drops the splash for getty: a
 *   plain open of the tty or /dev/console never does.
 */
struct tty;
typedef void (*pe_tty_ctty_fn)(void *, struct tty *);
EVENTHANDLER_DECLARE(pe_tty_ctty, pe_tty_ctty_fn);

#endif /* _KERNEL */

#endif /* !_SYS_PEXPERT_H_ */
