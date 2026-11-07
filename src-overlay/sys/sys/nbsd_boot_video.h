/*
 * NextBSD loader -> kernel boot video record (E19, nextbsd-kernel#407).
 *
 * The loader plays boot.efi's role: it decides once whether this boot is
 * graphical or text, paints the splash when it is graphical, and hands the
 * kernel one record that describes the screen it left behind, the way
 * boot.efi hands xnu its Boot_Video. The kernel builds PE_state.video
 * (<sys/pexpert.h>) from it at cninit.
 *
 * Transport: a module metadata entry of type MODINFOMD_NBSD_BOOT_VIDEO on
 * the kernel's preload record, read with
 *
 *   MD_FETCH(preload_kmdp, MODINFOMD_NBSD_BOOT_VIDEO, struct nbsd_boot_video *)
 *
 * (or preload_search_info(kmdp, MODINFO_METADATA | MODINFOMD_NBSD_BOOT_VIDEO)).
 * 0x2001 is the first number of the NextBSD-reserved MODINFOMD range,
 * 0x2000-0x2fff: FreeBSD uses 0x0001-0x00ff for the MI types, 0x1000-0x1fff
 * for the MD types (<machine/metadata.h>) and 0x4001 | MODINFOMD_NOCOPY for
 * the dependency list, so nothing upstream collides on amd64 or arm64. The
 * MODINFOMD_NOCOPY bit is clear: the record is copied to the kernel.
 *
 * The loader emits the record on every UEFI boot, in TEXT mode too (with
 * v_display = NBSD_BOOT_VIDEO_TEXT). The logo itself travels separately, as
 * the stock MODINFOMD_SPLASH image (struct splash_info + pixels), and only
 * in GRAPHICS mode. A kernel that finds no record falls back to
 * MODINFOMD_EFI_FB and computes the geometry below itself, so both sides
 * must use these formulas and the result is pixel-identical:
 *
 *   scale   = 2 if (v_flags & NBSD_BOOT_VIDEO_HIDPI), else
 *             2 if v_height >= 1440, else 1
 *   logo_w, logo_h = the logo asset at that scale (assets/bootsplash/ in
 *             nextbsd-kernel: nextbsd-logo.png at 1x, nextbsd-logo@2x.png)
 *   logo_x  = (v_width - logo_w) / 2
 *   logo_y  = v_height / 2 - logo_h / 2       (vertical centre at height/2)
 *   spin_d  = 40 * scale
 *   spin_x  = (v_width - spin_d) / 2
 *   spin_y  = logo_y + logo_h + (logo_h * 40) / 100
 *
 * All divisions are unsigned integer divisions. Colours are 0x00RRGGBB
 * with exact 8-bit components: bootsplash_style "black" (and unset) is
 * bg 0x000000 / fg 0xFFFFFF, "grey" is bg 0x5F6770 / fg 0x000000; fg is the
 * spinner colour. Alpha is composited as
 *
 *   out = (src * a + dst * (255 - a) + 127) / 255     per 8-bit channel.
 *
 * Layout is frozen at version 1: every field is naturally aligned, the
 * struct is 72 bytes with no padding, and new fields go at the end with a
 * version bump. This header is self-contained so the loader can carry an
 * identical copy (nextbsd-freebsd-compat); keep the two byte-identical.
 */

#ifndef _SYS_NBSD_BOOT_VIDEO_H_
#define	_SYS_NBSD_BOOT_VIDEO_H_

#include <sys/types.h>

#define	MODINFOMD_NBSD_BOOT_VIDEO	0x2001	/* struct nbsd_boot_video */

#define	NBSD_BOOT_VIDEO_VERSION	1

/* v_display: xnu's GRAPHICS_MODE and FB_TEXT_MODE values. */
#define	NBSD_BOOT_VIDEO_GRAPHICS	1
#define	NBSD_BOOT_VIDEO_TEXT		2

/* v_flags */
#define	NBSD_BOOT_VIDEO_HIDPI		0x00000001	/* kBootArgsFlagHiDPI */

struct nbsd_boot_video {
	uint32_t version;		/* NBSD_BOOT_VIDEO_VERSION */
	uint32_t v_display;		/* 1 = GRAPHICS_MODE, 2 = FB_TEXT_MODE (xnu values) */
	uint64_t v_baseAddr;		/* physical framebuffer base */
	uint32_t v_width, v_height, v_rowBytes, v_depth;
	uint32_t v_flags;		/* bit0 HIDPI */
	uint32_t bg_rgb, fg_rgb;	/* 0x00RRGGBB */
	uint32_t logo_x, logo_y, logo_w, logo_h;
	uint32_t spin_x, spin_y, spin_d;
};

#endif /* !_SYS_NBSD_BOOT_VIDEO_H_ */
