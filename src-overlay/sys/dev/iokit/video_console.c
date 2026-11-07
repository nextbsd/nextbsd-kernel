/*
 * NextBSD video console (E19): initialize_screen(), the vc_progress boot
 * spinner and the boot-graphics painter (nextbsd-kernel#407, #408).
 *
 * xnu's osfmk/console/video_console.c owns the console screen: who may draw
 * on it, when the boot spinner runs, and how the framebuffer record is
 * swapped. This is NextBSD's implementation of the same state machine, with
 * xnu's names and op semantics (<sys/pexpert.h>); no xnu code or Apple art
 * is copied. vt(4) stays the text console underneath: it asks
 * PE_graphics_console_active() before it clears or draws, the analogue of
 * xnu's gc_enable(FALSE).
 *
 * State (xnu's names):
 *   gc_graphics_boot   kPEGraphicsMode was set: the booter left graphics
 *   gc_acquired        the console has an owner (kPEAcquireScreen)
 *   gc_desire_text     kPETextScreen arrived before the acquire
 *   vc_progress_enable the spinner runs
 *   vc_text_disabled   graphics own the screen; vt(4) must not draw text
 *
 * Ops:
 *   kPEGraphicsMode/TextMode  boot mode (PE_init_console(), at cninit)
 *   kPEAcquireScreen          graphics boot: paint the whole splash from the
 *                             kernel's own copy, start the spinner with no
 *                             delay, text off. vt_fb_init() sends it for the
 *                             boot framebuffer (PE_boot_console_fb()), in
 *                             place of its blank: the first kernel paint is
 *                             the splash, never a black screen.
 *   kPEDisableScreen/Enable   pause/resume the spinner around a framebuffer
 *                             change; the screen stays owned. They nest.
 *                             Disable with a framebuffer also forgets it
 *                             (and its flush) if it is the console's, so
 *                             its driver may free it (vt_deallocate()).
 *   kPETextScreen             stop the spinner, text on; vt(4) redraws the
 *                             whole screen (PE_console_text_generation())
 *   kPEReleaseScreen          a display server owns the screen: stop the
 *                             spinner and paint its square with the
 *                             background (xnu's save-under), keep the rest,
 *                             text stays off. Ignored once text owns the
 *                             screen.
 *   kPEBaseAddressChange      swap the framebuffer under the spinner's lock;
 *                             never clear. Unlike xnu, the geometry is taken
 *                             from the new record too: a NextBSD fbdev
 *                             buffer need not match the firmware mode.
 *   kPERefreshBootGraphics    repaint background, logo and spinner frame
 *
 * Splash. Background, the NextBSD cube and the spinner, laid out as
 * <sys/nbsd_boot_video.h> specifies, so it is pixel-identical to what the
 * loader painted and the kernel's first paint is invisible. The logo is the
 * loader's own decoded copy (MODINFOMD_SPLASH) when the loader's boot video
 * record says it sent one of the right size, else the compiled-in cube
 * (bootsplash_data.h; the Pi and other loader-less boots). Both come from
 * assets/bootsplash/gen-bootsplash.py.
 *
 * Spinner (vc_progress). 12 steps a second, one revolution a second: 3
 * alpha frames, each blitted at 4 quarter turns (xnu's trick), 40 px at 1x
 * and 80 px at 2x. A callout with its own spin lock draws only the spinner
 * square; like xnu's save-under buffer, which is pre-filled with the
 * background colour, it composites over the solid background, here through
 * a 256-entry table of packed pixels. Every tick checks panic, release,
 * pause and FB_FLAG_NOWRITE (DRM's freeze), and then calls the backend's
 * flush, if it registered one (PE_set_console_flush()), for
 * shadow-buffered framebuffers.
 *
 * Locking: vc_progress_lock is a spin mutex, xnu's vc_progress_lock, and the
 * only lock here. It is never held across a call into vt(4), and vt(4)'s
 * locks are never taken under it. The panic path takes no lock.
 *
 * In text mode (-v, -s, serial console, no framebuffer) every op only
 * updates these flags and nothing is drawn; vt(4) sees
 * PE_graphics_console_active() false, so its output is byte-identical to a
 * kernel without this file.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/callout.h>
#include <sys/fbio.h>
#include <sys/linker.h>
#include <sys/splash.h>
#include <sys/sysctl.h>
#include <sys/nbsd_boot_video.h>
#include <sys/pexpert.h>

#include <machine/atomic.h>
#include <machine/cpu.h>

#include <dev/iokit/video_console.h>
#include <dev/iokit/bootsplash_data.h>

static struct mtx vc_progress_lock;
static bool vc_lock_inited;

/* A framebuffer's pixel format. */
struct vc_fmt {
	u_int	bytespp;		/* 2 or 4 */
	u_int	ro, go, bo;		/* channel offsets */
	u_int	rb, gb, bb;		/* channel widths */
};

/* Splash geometry on one framebuffer. */
struct vc_geom {
	u_int	scale;			/* 1 or 2 */
	u_int	lx, ly, lw, lh;		/* logo */
	u_int	sx, sy, sd;		/* spinner */
};

static PE_Video vinfo;			/* the console record */

/* The console framebuffer and what is derived from it, under the lock. */
static struct fb_info *vc_fb;
static struct vc_fmt vc_fmt_cur;
static struct vc_geom vc_geom_cur;
static uint32_t vc_lut[256];		/* spinner alpha -> packed pixel */
static pe_fb_flush_t *vc_flush;
static struct fb_info *vc_flush_fb;

static bool gc_graphics_boot;
static bool gc_acquired;
static bool gc_desire_text;
static bool gc_paused_progress;
static u_int gc_pause_depth;		/* nested kPEDisableScreen, under the lock */
static volatile u_int vc_flush_busy;	/* backend flushes in flight */
static bool vc_progress_enable;
static bool vc_released;
static u_int vc_progress_step;
static volatile int vc_text_disabled;
static volatile int vc_panicked;
static volatile u_int vc_text_gen;

static struct callout vc_progress_call;
static bool vc_callout_ready;

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
	return (vc_text_disabled != 0 && vc_panicked == 0);
}

u_int
PE_console_text_generation(void)
{
	return (vc_text_gen);
}

/* ------------------------------------------------------------ pixels ---- */

static int
vc_fmt_from_fb(const struct fb_info *fb, struct vc_fmt *f)
{
	const struct fb_rgboffs *o = &fb->fb_rgboffs;
	bool dflt = o->red == 0 && o->green == 0 && o->blue == 0;

	if (fb->fb_vbase == 0 || fb->fb_stride <= 0 || fb->fb_width <= 0 ||
	    fb->fb_height <= 0)
		return (EINVAL);
	switch (fb->fb_bpp) {
	case 32:
		f->bytespp = 4;
		f->rb = f->gb = f->bb = 8;
		f->ro = dflt ? 16 : o->red;
		f->go = dflt ? 8 : o->green;
		f->bo = dflt ? 0 : o->blue;
		break;
	case 16:
		f->bytespp = 2;
		f->rb = f->bb = 5;
		f->gb = fb->fb_depth == 15 ? 5 : 6;
		f->ro = dflt ? f->gb + 5 : o->red;
		f->go = dflt ? 5 : o->green;
		f->bo = dflt ? 0 : o->blue;
		break;
	default:
		return (EOPNOTSUPP);
	}
	if (f->ro + f->rb > f->bytespp * 8 || f->go + f->gb > f->bytespp * 8 ||
	    f->bo + f->bb > f->bytespp * 8)
		return (EINVAL);
	return (0);
}

static __inline uint32_t
vc_pack(const struct vc_fmt *f, u_int r, u_int g, u_int b)
{
	return (((r >> (8 - f->rb)) << f->ro) | ((g >> (8 - f->gb)) << f->go) |
	    ((b >> (8 - f->bb)) << f->bo));
}

/* <sys/nbsd_boot_video.h>: out = (src * a + dst * (255 - a) + 127) / 255 */
static __inline u_int
vc_blend(u_int s, u_int d, u_int a)
{
	return ((s * a + d * (255 - a) + 127) / 255);
}

static __inline uint32_t
vc_pack_blend(const struct vc_fmt *f, uint32_t fg, uint32_t bg, u_int a)
{
	return (vc_pack(f, vc_blend((fg >> 16) & 0xff, (bg >> 16) & 0xff, a),
	    vc_blend((fg >> 8) & 0xff, (bg >> 8) & 0xff, a),
	    vc_blend(fg & 0xff, bg & 0xff, a)));
}

static __inline void
vc_put(const struct fb_info *fb, const struct vc_fmt *f, u_int x, u_int y,
    uint32_t px)
{
	vm_offset_t a;

	a = fb->fb_vbase + (vm_offset_t)y * fb->fb_stride + x * f->bytespp;
	if (f->bytespp == 4)
		*(volatile uint32_t *)a = px;
	else
		*(volatile uint16_t *)a = (uint16_t)px;
}

/* --------------------------------------------------------- geometry ---- */

/* <sys/nbsd_boot_video.h>, or the loader's numbers on the loader's mode. */
static void
vc_geometry(u_int w, u_int h, struct vc_geom *g)
{
	const struct nbsd_boot_video *bv = PE_state.boot_video;

	g->scale = ((bv != NULL && (bv->v_flags & NBSD_BOOT_VIDEO_HIDPI)) ||
	    h >= 1440) ? 2 : 1;
	g->lw = g->scale == 2 ? VC_LOGO_2X_W : VC_LOGO_1X_W;
	g->lh = g->scale == 2 ? VC_LOGO_2X_H : VC_LOGO_1X_H;
	g->sd = g->scale == 2 ? VC_SPIN_2X_D : VC_SPIN_1X_D;
	if (bv != NULL && bv->v_width == w && bv->v_height == h &&
	    bv->logo_w == g->lw && bv->logo_h == g->lh && bv->spin_d == g->sd) {
		g->lx = bv->logo_x;
		g->ly = bv->logo_y;
		g->sx = bv->spin_x;
		g->sy = bv->spin_y;
		return;
	}
	g->lx = w > g->lw ? (w - g->lw) / 2 : 0;
	g->ly = h / 2 > g->lh / 2 ? h / 2 - g->lh / 2 : 0;
	g->sx = w > g->sd ? (w - g->sd) / 2 : 0;
	g->sy = g->ly + g->lh + (g->lh * 40) / 100;
}

/* ------------------------------------------------------------ paint ---- */

/* The loader's decoded logo, when its boot video record vouches for it. */
static const uint8_t *
vc_loader_logo(const struct vc_geom *g)
{
	const struct nbsd_boot_video *bv = PE_state.boot_video;
	struct splash_info *si;
	uintptr_t image;

	if (bv == NULL || bv->v_display != NBSD_BOOT_VIDEO_GRAPHICS ||
	    bv->logo_w != g->lw || bv->logo_h != g->lh || preload_kmdp == NULL)
		return (NULL);
	si = MD_FETCH(preload_kmdp, MODINFOMD_SPLASH, struct splash_info *);
	if (si == NULL || si->si_width != g->lw || si->si_height != g->lh ||
	    si->si_depth != 4)
		return (NULL);
	image = roundup2((uintptr_t)si + sizeof(struct splash_info), 8);
	return ((const uint8_t *)image);
}

static __inline void
vc_logo_px(const struct fb_info *fb, const struct vc_fmt *f,
    const struct vc_geom *g, u_int i, const uint8_t *p, uint32_t bg)
{
	u_int x, y;

	if (p[3] == 0)
		return;
	x = g->lx + i % g->lw;
	y = g->ly + i / g->lw;
	if (x >= (u_int)fb->fb_width || y >= (u_int)fb->fb_height)
		return;
	vc_put(fb, f, x, y, vc_pack(f,
	    vc_blend(p[0], (bg >> 16) & 0xff, p[3]),
	    vc_blend(p[1], (bg >> 8) & 0xff, p[3]),
	    vc_blend(p[2], bg & 0xff, p[3])));
}

static void
vc_paint_logo(const struct fb_info *fb, const struct vc_fmt *f,
    const struct vc_geom *g, uint32_t bg)
{
	const uint8_t *img, *rle;
	size_t len, k;
	u_int i, n, c;

	img = vc_loader_logo(g);
	if (img != NULL) {
		n = g->lw * g->lh;
		for (i = 0; i < n; i++)
			vc_logo_px(fb, f, g, i, img + i * 4, bg);
		return;
	}
	if (g->scale == 2) {
		rle = vc_logo_2x_rle;
		len = sizeof(vc_logo_2x_rle);
	} else {
		rle = vc_logo_1x_rle;
		len = sizeof(vc_logo_1x_rle);
	}
	n = g->lw * g->lh;
	for (k = 0, i = 0; k + 5 <= len && i < n; k += 5)
		for (c = rle[k]; c > 0 && i < n; c--, i++)
			vc_logo_px(fb, f, g, i, rle + k + 1, bg);
}

static void
vc_fill(const struct fb_info *fb, const struct vc_fmt *f, uint32_t px)
{
	u_int x, y;

	for (y = 0; y < (u_int)fb->fb_height; y++)
		for (x = 0; x < (u_int)fb->fb_width; x++)
			vc_put(fb, f, x, y, px);
}

/*
 * One spinner step: base frame step % 3, turned step / 3 quarter turns
 * clockwise, dst(x, y) = src(y, d - 1 - x) per turn. `lut' maps alpha to
 * the packed spinner colour over the background.
 */
static void
vc_draw_spinner(const struct fb_info *fb, const struct vc_fmt *f,
    const struct vc_geom *g, u_int step, const uint32_t *lut)
{
	const uint8_t *a;
	u_int d, x, y, sx, sy;

	d = g->sd;
	if (g->sx + d > (u_int)fb->fb_width || g->sy + d > (u_int)fb->fb_height)
		return;
	a = g->scale == 2 ? vc_spin_2x[step % VC_SPIN_BASE] :
	    vc_spin_1x[step % VC_SPIN_BASE];
	for (y = 0; y < d; y++) {
		for (x = 0; x < d; x++) {
			switch ((step / VC_SPIN_BASE) & 3) {
			case 0: sx = x; sy = y; break;
			case 1: sx = y; sy = d - 1 - x; break;
			case 2: sx = d - 1 - x; sy = d - 1 - y; break;
			default: sx = d - 1 - y; sy = x; break;
			}
			vc_put(fb, f, g->sx + x, g->sy + y, lut[a[sy * d + sx]]);
		}
	}
}

static void
vc_build_lut(const struct vc_fmt *f, uint32_t *lut)
{
	u_int a;

	for (a = 0; a < 256; a++)
		lut[a] = vc_pack_blend(f, PE_state.fg_rgb, PE_state.bg_rgb, a);
}

/*
 * The whole boot picture into `fb': background, logo, the current spinner
 * step. The big fill runs unlocked; a spinner tick racing it only draws
 * the square this function redraws last, under the lock.
 */
static void
vc_paint_boot_graphics(struct fb_info *fb)
{
	struct vc_fmt f;
	struct vc_geom g;
	uint32_t lut[256];
	pe_fb_flush_t *flush;
	u_int step;

	if (fb == NULL || vc_fmt_from_fb(fb, &f) != 0 ||
	    (fb->fb_flags & FB_FLAG_NOWRITE) != 0 || vc_panicked)
		return;
	vc_geometry(fb->fb_width, fb->fb_height, &g);
	vc_build_lut(&f, lut);
	vc_fill(fb, &f, lut[0]);
	vc_paint_logo(fb, &f, &g, PE_state.bg_rgb);
	flush = NULL;
	VC_LOCK();
	step = vc_progress_step;
	if (!vc_panicked && (fb->fb_flags & FB_FLAG_NOWRITE) == 0)
		vc_draw_spinner(fb, &f, &g, step, lut);
	if (vc_flush != NULL && vc_flush_fb == fb) {
		flush = vc_flush;
		atomic_add_int(&vc_flush_busy, 1);
	}
	VC_UNLOCK();
	if (flush != NULL) {
		flush(fb, 0, 0, fb->fb_width, fb->fb_height);
		atomic_subtract_rel_int(&vc_flush_busy, 1);
	}
}

/*
 * kPEDisableScreen for a framebuffer that is going away (vt_deallocate(),
 * a driver detaching): forget it, and its flush, before the op returns, and
 * wait out a flush already in flight, so the driver may free it next. The
 * patch 0058 race, for the spinner. Takes only the spinner's lock; never
 * sleeps.
 */
static void
vc_forget_fb(struct fb_info *fb)
{
	VC_LOCK();
	if (vc_fb == fb)
		vc_fb = NULL;
	if (vc_flush_fb == fb) {
		vc_flush_fb = NULL;
		vc_flush = NULL;
	}
	VC_UNLOCK();
	while (atomic_load_acq_int(&vc_flush_busy) != 0)
		cpu_spinwait();
}

/* ---------------------------------------------------------- spinner ---- */

static void vc_progress_task(void *);

static void
vc_progress_arm(void)
{
	if (vc_callout_ready)
		callout_reset_sbt(&vc_progress_call, SBT_1S / VC_SPIN_FRAMES, 0,
		    vc_progress_task, NULL, 0);
}

static void
vc_progress_task(void *arg __unused)
{
	struct fb_info *fb;
	pe_fb_flush_t *flush;
	struct vc_geom g;
	bool again;

	fb = NULL;
	flush = NULL;
	bzero(&g, sizeof(g));
	VC_LOCK();
	again = vc_progress_enable && !vc_panicked;
	if (again && vc_text_disabled && vc_fb != NULL &&
	    (vc_fb->fb_flags & FB_FLAG_NOWRITE) == 0) {
		vc_progress_step = (vc_progress_step + 1) % VC_SPIN_FRAMES;
		vc_draw_spinner(vc_fb, &vc_fmt_cur, &vc_geom_cur,
		    vc_progress_step, vc_lut);
		fb = vc_fb;
		g = vc_geom_cur;
		if (vc_flush_fb == fb && vc_flush != NULL) {
			flush = vc_flush;
			atomic_add_int(&vc_flush_busy, 1);
		}
	}
	VC_UNLOCK();
	if (flush != NULL) {
		flush(fb, g.sx, g.sy, g.sd, g.sd);
		atomic_subtract_rel_int(&vc_flush_busy, 1);
	}
	if (again)
		vc_progress_arm();
}

/*
 * Paint the spinner square with the background, xnu restoring its
 * save-under when the spinner stops for kPEReleaseScreen: the display
 * server gets the background and the cube, never a frozen spinner frame.
 */
static void
vc_progress_erase(void)
{
	struct fb_info *fb;
	pe_fb_flush_t *flush;
	struct vc_geom g;
	u_int x, y;

	fb = NULL;
	flush = NULL;
	bzero(&g, sizeof(g));
	VC_LOCK();
	if (vc_fb != NULL && !vc_panicked &&
	    (vc_fb->fb_flags & FB_FLAG_NOWRITE) == 0 &&
	    vc_geom_cur.sx + vc_geom_cur.sd <= (u_int)vc_fb->fb_width &&
	    vc_geom_cur.sy + vc_geom_cur.sd <= (u_int)vc_fb->fb_height) {
		fb = vc_fb;
		g = vc_geom_cur;
		for (y = 0; y < g.sd; y++)
			for (x = 0; x < g.sd; x++)
				vc_put(fb, &vc_fmt_cur, g.sx + x, g.sy + y,
				    vc_lut[0]);
		if (vc_flush_fb == fb && vc_flush != NULL) {
			flush = vc_flush;
			atomic_add_int(&vc_flush_busy, 1);
		}
	}
	VC_UNLOCK();
	if (flush != NULL) {
		flush(fb, g.sx, g.sy, g.sd, g.sd);
		atomic_subtract_rel_int(&vc_flush_busy, 1);
	}
}

/* Start or stop the spinner (xnu's vc_progress_set()); no delay here. */
static void
vc_progress_set(bool enable, uint32_t delay __unused)
{
	bool start;

	VC_LOCK();
	start = enable && !vc_progress_enable && vc_panicked == 0;
	vc_progress_enable = enable && vc_panicked == 0;
	VC_UNLOCK();
	if (start)
		vc_progress_arm();
	else if (!enable && vc_callout_ready)
		callout_stop(&vc_progress_call);
}

static void
vc_progress_init(void *arg __unused)
{
	bool run;

	callout_init(&vc_progress_call, 1);
	VC_LOCK();
	vc_callout_ready = true;
	run = vc_progress_enable;
	VC_UNLOCK();
	if (run)
		vc_progress_arm();
}
SYSINIT(vc_progress, SI_SUB_SOFTINTR, SI_ORDER_ANY, vc_progress_init, NULL);

/* ----------------------------------------------------- state machine ---- */

/* xnu's gc_enable(): text console on or off. */
static void
gc_enable(bool text)
{
	if (text) {
		if (vc_text_disabled) {
			vc_text_disabled = 0;
			atomic_add_int(&vc_text_gen, 1);
		}
	} else
		vc_text_disabled = 1;
}

/*
 * xnu's gc_pause(): the spinner around a framebuffer change. NextBSD nests
 * it: vt(4) brackets every backend switch and a DRM driver may bracket its
 * own freeze around that, so only the outermost Disable saves the spinner's
 * state and only the matching Enable restores it. An Enable with no Disable
 * outstanding does nothing.
 */
static void
gc_pause(bool pause)
{
	bool start;

	start = false;
	VC_LOCK();
	if (pause) {
		if (gc_pause_depth++ == 0) {
			gc_paused_progress = vc_progress_enable;
			vc_progress_enable = false;
		}
	} else if (gc_pause_depth > 0 && --gc_pause_depth == 0) {
		start = gc_paused_progress && !vc_progress_enable &&
		    vc_panicked == 0;
		vc_progress_enable = gc_paused_progress && vc_panicked == 0;
	}
	VC_UNLOCK();
	if (start)
		vc_progress_arm();
}

/* kPETextScreen / kPEReleaseScreen end any pause: nothing to resume. */
static void
gc_pause_reset(void)
{
	VC_LOCK();
	gc_pause_depth = 0;
	gc_paused_progress = false;
	VC_UNLOCK();
}

/* Make `fb' the console framebuffer (NULL if it can't be drawn on). */
static void
vc_set_fb(struct fb_info *fb)
{
	struct vc_fmt f;
	struct vc_geom g;
	uint32_t lut[256];

	if (fb != NULL && vc_fmt_from_fb(fb, &f) != 0)
		fb = NULL;
	if (fb != NULL) {
		vc_geometry(fb->fb_width, fb->fb_height, &g);
		vc_build_lut(&f, lut);
	}
	VC_LOCK();
	vc_fb = fb;
	if (fb != NULL) {
		vc_fmt_cur = f;
		vc_geom_cur = g;
		memcpy(vc_lut, lut, sizeof(vc_lut));
	}
	VC_UNLOCK();
}

void
vc_console_fb(struct fb_info *fb, int op)
{
	switch (op) {
	case kPEBaseAddressChange:
	case kPEEnableScreen:
		if (fb != NULL)
			vc_set_fb(fb);
		break;
	case kPEDisableScreen:
		/* The framebuffer may be freed once this returns. */
		if (fb != NULL)
			vc_forget_fb(fb);
		break;
	case kPERefreshBootGraphics:
		if (vc_text_disabled && gc_graphics_boot && gc_acquired)
			vc_paint_boot_graphics(fb != NULL ? fb : vc_fb);
		break;
	default:
		break;
	}
}

bool
PE_boot_console_fb(struct fb_info *fb)
{
	if (PE_boot_display() != GRAPHICS_MODE || !gc_graphics_boot ||
	    gc_desire_text || gc_acquired || vc_released || vc_fb != NULL ||
	    vc_panicked || fb == NULL)
		return (false);
	/* Only the framebuffer the booter left, as the booter left it. */
	if (fb->fb_pbase != PE_state.video.v_baseAddr ||
	    (u_long)fb->fb_width != PE_state.video.v_width ||
	    (u_long)fb->fb_height != PE_state.video.v_height)
		return (false);
	vc_set_fb(fb);
	if (vc_fb == NULL)
		return (false);
	initialize_screen(NULL, kPEAcquireScreen);
	return (PE_graphics_console_active());
}

void
PE_set_console_flush(struct fb_info *fb, pe_fb_flush_t *flush)
{
	VC_LOCK();
	vc_flush_fb = flush != NULL ? fb : NULL;
	vc_flush = flush;
	VC_UNLOCK();
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
		if (graphics_now) {
			/* Text off before the paint: vt(4) must not race it. */
			gc_enable(false);
			vc_paint_boot_graphics(vc_fb);
		}
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
		/*
		 * NextBSD: after kPEReleaseScreen the screen can still come
		 * back to text (getty after loginwindow, a display server
		 * exiting); xnu re-acquires instead, from IOFramebuffer's
		 * close().
		 */
		if (!gc_acquired && !vc_released) {
			gc_desire_text = true;
			break;
		}
		if (!gc_graphics_boot)
			break;
		vc_progress_set(false, 0);
		gc_pause_reset();
		gc_enable(true);
		break;

	case kPEReleaseScreen:
		/*
		 * NextBSD: a text console already owns the screen (getty
		 * took it with kPETextScreen, or graphics never did):
		 * there is no splash to release, and turning text off now
		 * would leave a dead console when the display server
		 * exits.
		 */
		if (!vc_text_disabled)
			break;
		if (gc_acquired)
			vc_released = true;
		gc_acquired = false;
		gc_desire_text = false;
		if (!gc_graphics_boot)
			break;
		/* xnu: the display server owns the pixels; text stays off. */
		gc_enable(false);
		vc_progress_set(false, 0);
		gc_pause_reset();
		/* xnu restores the save-under: the spinner goes, the rest stays. */
		vc_progress_erase();
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
	if (vc_panicked)
		return;
	vc_panicked = 1;
	vc_progress_enable = false;
	if (vc_text_disabled) {
		vc_text_disabled = 0;
		atomic_add_int(&vc_text_gen, 1);
	}
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
	else if (!vc_text_disabled)
		s = "text";
	else
		s = vc_progress_enable ? "graphics" : "paused";
	return (sysctl_handle_string(oidp, __DECONST(char *, s), 0, req));
}
