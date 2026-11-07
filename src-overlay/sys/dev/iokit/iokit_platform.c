/*
 * NextBSD platform expert, console role (E19, nextbsd-kernel#407).
 *
 * xnu's IOPlatformExpert owns the console record: the booter fills
 * PE_state.video, PE_create_console() puts the console in graphics or text
 * mode from it, and every framebuffer change goes through
 * PE_initialize_console(), which IOFramebuffer reaches through
 * setConsoleInfo(). This file is that role for NextBSD's in-kernel IOKit.
 * The names and op values are xnu's (<sys/pexpert.h>); the code is
 * NextBSD's. initialize_screen(), the state machine behind it, is in
 * video_console.c.
 *
 * Boot. cninit() calls PE_init_console() right after patch 0062's mute
 * decision and before any console driver probes, with the boothowto/kenv
 * half of the graphics-or-text decision already made. This file adds the
 * other half: no framebuffer, or a loader record with
 * v_display == FB_TEXT_MODE, means text. The record comes from the loader's
 * struct nbsd_boot_video (MODINFOMD_NBSD_BOOT_VIDEO), else from the stock
 * MODINFOMD_EFI_FB record vt_efifb uses (amd64 and arm64). BIOS/VBE boots
 * have neither and stay text. Nothing here prints, so a -v boot is
 * byte-identical to a kernel without this file.
 *
 * Style. bootsplash_style = "black" | "grey" from the kenv (the loader's
 * loader.conf.d, or a compiled-in env); unset or anything else is black.
 *
 * kexts. drm.ko reports framebuffer changes through the pe_console_op and
 * pe_drm_master eventhandlers, by name (see <sys/pexpert.h>), so it links
 * nothing from here and loads on a kernel without them.
 *
 * Observability: hw.iokit.console.* (read-only).
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/bus.h>
#include <sys/cons.h>
#include <sys/eventhandler.h>
#include <sys/fbio.h>
#include <sys/linker.h>
#include <sys/msgbuf.h>
#include <sys/reboot.h>
#include <sys/sysctl.h>
#include <sys/iocatalogue.h>
#include <sys/nbsd_boot_video.h>
#include <sys/pexpert.h>

#include <machine/metadata.h>

#include <dev/iokit/video_console.h>

PE_state_t PE_state;

static int pe_boot_display = FB_TEXT_MODE;
/* What cninit's boothowto/kenv half asked for, before the framebuffer half. */
static int pe_requested_display = FB_TEXT_MODE;

/*
 * Serial-console requests the loader leaves in the kenv. boothowto already
 * carries RB_SERIAL / RB_MULTIPLE when the loader translated them; this
 * also catches a console list that names comconsole without either flag.
 */
int
PE_serial_console_requested(void)
{
	char buf[64];

	if (testenv("boot_serial") || testenv("boot_multicons"))
		return (1);
	if (getenv_string("console", buf, sizeof(buf)) &&
	    strstr(buf, "comconsole") != NULL)
		return (1);
	return (0);
}

static void
pe_read_style(void)
{
	char buf[16];

	PE_state.style = PE_STYLE_BLACK;
	if (getenv_string("bootsplash_style", buf, sizeof(buf)) &&
	    strcmp(buf, "grey") == 0)
		PE_state.style = PE_STYLE_GREY;
	if (PE_state.style == PE_STYLE_GREY) {
		PE_state.bg_rgb = PE_STYLE_GREY_BG;
		PE_state.fg_rgb = PE_STYLE_GREY_FG;
	} else {
		PE_state.bg_rgb = PE_STYLE_BLACK_BG;
		PE_state.fg_rgb = PE_STYLE_BLACK_FG;
	}
}

static int
pe_scale_for(unsigned long height, uint32_t flags)
{
	if ((flags & NBSD_BOOT_VIDEO_HIDPI) != 0 || height >= 1440)
		return (kPEScaleFactor2x);
	return (kPEScaleFactor1x);
}

/*
 * Fill `v' from the loader's records. Returns the loader's v_display
 * (GRAPHICS_MODE / FB_TEXT_MODE), or 0 when there is no framebuffer.
 */
static int
pe_video_from_loader(PE_Video *v)
{
	const struct nbsd_boot_video *bv;
#if defined(__amd64__) || defined(__aarch64__)
	const struct efi_fb *efifb;
	int depth;
#endif

	bzero(v, sizeof(*v));
	if (preload_kmdp == NULL)
		return (0);

	bv = (const struct nbsd_boot_video *)preload_search_info(preload_kmdp,
	    MODINFO_METADATA | MODINFOMD_NBSD_BOOT_VIDEO);
	if (bv != NULL && bv->version >= 1) {
		PE_state.boot_video = bv;
		if (bv->v_baseAddr == 0 || bv->v_width == 0 ||
		    bv->v_height == 0 || bv->v_rowBytes == 0 ||
		    bv->v_depth == 0)
			return (0);
		v->v_baseAddr = bv->v_baseAddr;
		v->v_rowBytes = bv->v_rowBytes;
		v->v_width = bv->v_width;
		v->v_height = bv->v_height;
		v->v_depth = bv->v_depth;
		v->v_scale = pe_scale_for(bv->v_height, bv->v_flags);
		return (bv->v_display == NBSD_BOOT_VIDEO_GRAPHICS ?
		    GRAPHICS_MODE : FB_TEXT_MODE);
	}

#if defined(__amd64__) || defined(__aarch64__)
	efifb = (const struct efi_fb *)preload_search_info(preload_kmdp,
	    MODINFO_METADATA | MODINFOMD_EFI_FB);
	if (efifb == NULL || efifb->fb_addr == 0 || efifb->fb_width == 0 ||
	    efifb->fb_height == 0 || efifb->fb_stride == 0)
		return (0);
	/* The same arithmetic as vt_efifb_init(). */
	depth = fls(efifb->fb_mask_red | efifb->fb_mask_green |
	    efifb->fb_mask_blue | efifb->fb_mask_reserved);
	depth = roundup2(depth, NBBY);
	if (depth == 0)
		return (0);
	v->v_baseAddr = efifb->fb_addr;
	v->v_width = efifb->fb_width;
	v->v_height = efifb->fb_height;
	v->v_depth = depth;
	v->v_rowBytes = efifb->fb_stride * (depth / NBBY);
	v->v_scale = pe_scale_for(efifb->fb_height, 0);
	/* No NextBSD record: an older loader; it painted nothing. */
	return (GRAPHICS_MODE);
#else
	return (0);
#endif
}

/*
 * xnu's PE_init_platform() video part plus PE_create_console(), at cninit.
 */
void
PE_init_console(int mode)
{
	int loader;

	if (PE_state.initialized)
		return;

	pe_read_style();
	pe_requested_display = mode == GRAPHICS_MODE ? GRAPHICS_MODE :
	    FB_TEXT_MODE;
	loader = pe_video_from_loader(&PE_state.video);
	if (loader != GRAPHICS_MODE)
		mode = FB_TEXT_MODE;
	if (mode != GRAPHICS_MODE)
		mode = FB_TEXT_MODE;
	PE_state.video.v_display = mode;
	pe_boot_display = mode;
	PE_state.initialized = TRUE;

	initialize_screen(&PE_state.video,
	    mode == GRAPHICS_MODE ? kPEGraphicsMode : kPETextMode);
}

int
PE_boot_display(void)
{
	return (pe_boot_display);
}

/*
 * xnu pe_init.c PE_initialize_console(), without the serial-console switch:
 * NextBSD's console list (cn_devlist) is not changed here.
 */
int
PE_initialize_console(PE_Video *info, int op)
{
	if (info != NULL) {
		info->v_offset = 0;
		info->v_length = 0;
		info->v_display = GRAPHICS_MODE;
	}

	switch (op) {
	case kPEEnableScreen:
		initialize_screen(info, op);
		if (info != NULL)
			PE_state.video = *info;
		break;
	case kPEBaseAddressChange:
		if (info != NULL)
			PE_state.video = *info;
		/* FALLTHROUGH */
	default:
		initialize_screen(info, op);
		break;
	}
	return (0);
}

int
PE_current_console(PE_Video *info)
{
	if (!PE_state.initialized)
		return (-1);
	*info = PE_state.video;
	return (0);
}

/* The PE_Video an fb_info describes. */
static void
pe_video_from_fb(PE_Video *v, const struct fb_info *fb)
{
	bzero(v, sizeof(*v));
	v->v_baseAddr = fb->fb_pbase;
	v->v_rowBytes = fb->fb_stride;
	v->v_width = fb->fb_width;
	v->v_height = fb->fb_height;
	v->v_depth = fb->fb_bpp;
	v->v_scale = pe_scale_for(fb->fb_height,
	    PE_state.boot_video != NULL ? PE_state.boot_video->v_flags : 0);
}

/*
 * A boot with no loader, the Raspberry Pi 5's: the firmware loads
 * kernel8.img directly, so at cninit there is no boot video record and no
 * EFI framebuffer, and PE_init_console() fell back to text. The firmware
 * framebuffer only turns up when bcm2835_fbd(4) attaches; it calls this
 * just before handing the framebuffer to vt(4). If cninit's own half of
 * the decision was graphics (no -v, -s, -a, no serial console requested)
 * and nothing else has claimed the console yet, build PE_state.video from
 * the firmware framebuffer and enter kPEGraphicsMode, as xnu's ARM
 * PE_init_platform() does from the device tree's video record. vt(4)'s
 * first vd_init on it then sends kPEAcquireScreen (PE_boot_console_fb()),
 * which paints the compiled-in splash and starts the spinner. The style
 * is the compiled-in kenv (config/rpi5.env), read at cninit.
 */
void
PE_late_console_fb(struct fb_info *fb)
{
	PE_Video v;

	if (!PE_state.initialized || fb == NULL ||
	    pe_requested_display != GRAPHICS_MODE ||
	    pe_boot_display == GRAPHICS_MODE ||
	    PE_state.boot_video != NULL || PE_state.video.v_baseAddr != 0)
		return;
	if ((boothowto & (RB_SINGLE | RB_VERBOSE | RB_ASKNAME | RB_SERIAL |
	    RB_MULTIPLE)) != 0 || PE_serial_console_requested())
		return;
	if (fb->fb_pbase == 0 || fb->fb_width <= 0 || fb->fb_height <= 0 ||
	    fb->fb_stride <= 0 || fb->fb_bpp == 0)
		return;
	pe_video_from_fb(&v, fb);
	v.v_display = GRAPHICS_MODE;
	PE_state.video = v;
	pe_boot_display = GRAPHICS_MODE;
	initialize_screen(&PE_state.video, kPEGraphicsMode);
}

int
PE_initialize_console_fb(struct fb_info *fb, int op)
{
	PE_Video v;

	if (fb == NULL || op == kPERefreshBootGraphics) {
		vc_console_fb(fb, op);
		return (PE_initialize_console(NULL, op));
	}
	pe_video_from_fb(&v, fb);
	vc_console_fb(fb, op);
	return (PE_initialize_console(&v, op));
}

/*
 * Print the message buffer's unfinished last line (a prompt such as geli's
 * "Enter passphrase for ada0p4: ") on the console only. It was printed
 * while the console was muted, so it reached the message buffer but not
 * the screen. cnputsn() does not add it to the message buffer again.
 */
static void
pe_replay_prompt(void)
{
	struct msgbuf *mbp;
	char buf[160];
	u_int seq, avail, take;
	int n, r, start;

	mbp = msgbufp;
	if (mbp == NULL || mbp->msg_size == 0)
		return;
	avail = (mbp->msg_flags & MSGBUF_WRAP) != 0 ? mbp->msg_size :
	    mbp->msg_wseq;
	take = MIN(avail, sizeof(buf));
	seq = MSGBUF_SEQSUB(mbp, mbp->msg_wseq, take);
	for (n = 0; n < (int)take; n += r) {
		r = msgbuf_peekbytes(mbp, buf + n, take - n, &seq);
		if (r <= 0)
			break;
	}
	for (start = n; start > 0 && buf[start - 1] != '\n'; start--)
		;
	/* No line start in sight: too long to be a prompt; skip it. */
	if (start == 0 && n == (int)sizeof(buf))
		return;
	if (start < n)
		cnputsn(buf + start, n - start);
}

/*
 * A prompt must be seen (#371): the console may be muted (patch 0062,
 * where cngetc() returns -1 and cngets() would spin forever) and, on a
 * graphics boot, owned by the splash. Lift both, xnu's kPETextScreen.
 * Called from cngets() and before the mountroot prompt. A no-op on an
 * unmuted text boot (-v, -s, serial), which stays byte-identical.
 */
void
PE_enter_text_console(void)
{
	int was_muted;

	was_muted = cn_mute;
	cn_mute = 0;
	if (pe_boot_display == GRAPHICS_MODE)
		PE_initialize_console(NULL, kPETextScreen);
	if (was_muted)
		pe_replay_prompt();
}

void
PE_panic_console(void)
{
	/*
	 * A fatal trap prints its register dump before panic() lifts the
	 * mute, so lift it here first (nextbsd-kernel#371).
	 */
	cn_mute = 0;
	vc_panic();
}

/* ------------------------------------------------ kext eventhandlers ---- */

static void
pe_console_op_handler(void *arg __unused, struct fb_info *fb, int op)
{
	switch (op) {
	case kPEReleaseScreen:
	case kPEEnableScreen:
	case kPEDisableScreen:
	case kPEBaseAddressChange:
	case kPERefreshBootGraphics:
		break;
	default:
		return;
	}
	if (pe_boot_display != GRAPHICS_MODE)
		return;
	if (op == kPEBaseAddressChange && fb == NULL)
		return;
	PE_initialize_console_fb(fb, op);
}

static void
pe_drm_master_handler(void *arg __unused, device_t dev __unused,
    int acquired)
{
	if (pe_boot_display != GRAPHICS_MODE || !acquired)
		return;
	PE_initialize_console(NULL, kPEReleaseScreen);
}

static void
pe_platform_init(void *arg __unused)
{
	EVENTHANDLER_REGISTER(pe_console_op, pe_console_op_handler, NULL,
	    EVENTHANDLER_PRI_ANY);
	EVENTHANDLER_REGISTER(pe_drm_master, pe_drm_master_handler, NULL,
	    EVENTHANDLER_PRI_ANY);
}
SYSINIT(pe_platform, SI_SUB_EVENTHANDLER, SI_ORDER_ANY, pe_platform_init,
    NULL);

/* --------------------------------------------------------- sysctls ---- */

static SYSCTL_NODE(_hw_iokit, OID_AUTO, console, CTLFLAG_RD | CTLFLAG_MPSAFE,
    0, "Platform expert console record (PE_state.video)");

static int
pe_sysctl_video(SYSCTL_HANDLER_ARGS)
{
	PE_Video v;
	long val;

	v = PE_state.video;
	switch (arg2) {
	case 0: val = v.v_display; break;
	case 1: val = v.v_width; break;
	case 2: val = v.v_height; break;
	case 3: val = v.v_depth; break;
	case 4: val = v.v_rowBytes; break;
	case 5: val = v.v_scale; break;
	default: return (EINVAL);
	}
	return (sysctl_handle_long(oidp, &val, 0, req));
}

SYSCTL_PROC(_hw_iokit_console, OID_AUTO, display,
    CTLTYPE_ULONG | CTLFLAG_RD | CTLFLAG_MPSAFE, NULL, 0, pe_sysctl_video,
    "LU", "v_display: 1 GRAPHICS_MODE, 2 FB_TEXT_MODE");
SYSCTL_PROC(_hw_iokit_console, OID_AUTO, width,
    CTLTYPE_ULONG | CTLFLAG_RD | CTLFLAG_MPSAFE, NULL, 1, pe_sysctl_video,
    "LU", "v_width");
SYSCTL_PROC(_hw_iokit_console, OID_AUTO, height,
    CTLTYPE_ULONG | CTLFLAG_RD | CTLFLAG_MPSAFE, NULL, 2, pe_sysctl_video,
    "LU", "v_height");
SYSCTL_PROC(_hw_iokit_console, OID_AUTO, depth,
    CTLTYPE_ULONG | CTLFLAG_RD | CTLFLAG_MPSAFE, NULL, 3, pe_sysctl_video,
    "LU", "v_depth");
SYSCTL_PROC(_hw_iokit_console, OID_AUTO, rowbytes,
    CTLTYPE_ULONG | CTLFLAG_RD | CTLFLAG_MPSAFE, NULL, 4, pe_sysctl_video,
    "LU", "v_rowBytes");
SYSCTL_PROC(_hw_iokit_console, OID_AUTO, scale,
    CTLTYPE_ULONG | CTLFLAG_RD | CTLFLAG_MPSAFE, NULL, 5, pe_sysctl_video,
    "LU", "v_scale");
SYSCTL_INT(_hw_iokit_console, OID_AUTO, boot_display, CTLFLAG_RD,
    &pe_boot_display, 0, "Boot mode chosen at cninit: 1 graphics, 2 text");
SYSCTL_INT(_hw_iokit_console, OID_AUTO, style, CTLFLAG_RD,
    &PE_state.style, 0, "bootsplash_style: 0 black, 1 grey");
SYSCTL_PROC(_hw_iokit_console, OID_AUTO, state,
    CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_MPSAFE, NULL, 0, vc_sysctl_state,
    "A", "Console owner: text, graphics, paused, released");
