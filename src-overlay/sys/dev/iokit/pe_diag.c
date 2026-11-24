/*
 * TEMPORARY DIAGNOSTIC (nextbsd/nextbsd#542). NOT FOR MERGE.
 *
 * debug.pe_diag.phys: copy out debug.pe_diag.len bytes of physical memory
 * at debug.pe_diag.addr through a fresh kernel mapping (device memory, or
 * write-combining with debug.pe_diag.wc=1). Root only. Used to read the
 * BCM2712 HVS display list and the framebuffer it scans out on a Raspberry
 * Pi 500+ booted through U-Boot, where the boot splash does not show.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/priv.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <sys/fbio.h>

#include <vm/vm.h>
#include <vm/pmap.h>

static uint64_t pd_addr;
static u_int pd_len = 4096;
static int pd_wc;

static SYSCTL_NODE(_debug, OID_AUTO, pe_diag, CTLFLAG_RD | CTLFLAG_MPSAFE,
    0, "NextBSD #542 console diagnostics (temporary)");
SYSCTL_U64(_debug_pe_diag, OID_AUTO, addr, CTLFLAG_RW, &pd_addr, 0,
    "physical address to read");
SYSCTL_UINT(_debug_pe_diag, OID_AUTO, len, CTLFLAG_RW, &pd_len, 0,
    "bytes to read (multiple of 4, at most 16 MB)");
SYSCTL_INT(_debug_pe_diag, OID_AUTO, wc, CTLFLAG_RW, &pd_wc, 0,
    "map write-combining instead of device memory");

static int
pd_sysctl_phys(SYSCTL_HANDLER_ARGS)
{
	volatile uint32_t *src;
	uint32_t *buf;
	size_t len, i;
	void *va;
	int error;

	error = priv_check(req->td, PRIV_KMEM_READ);
	if (error != 0)
		return (error);
	len = pd_len;
	if (len == 0 || len > 16 * 1024 * 1024 || (len & 3) != 0 ||
	    (pd_addr & 3) != 0)
		return (EINVAL);
	if (req->oldptr == NULL)
		return (SYSCTL_OUT(req, NULL, len));
	buf = malloc(len, M_TEMP, M_WAITOK);
	if (pd_wc)
		va = pmap_mapdev_attr(pd_addr, len, VM_MEMATTR_WRITE_COMBINING);
	else
		va = pmap_mapdev(pd_addr, len);
	src = va;
	for (i = 0; i < len / 4; i++)
		buf[i] = src[i];
	pmap_unmapdev(va, len);
	error = SYSCTL_OUT(req, buf, len);
	free(buf, M_TEMP);
	return (error);
}
SYSCTL_PROC(_debug_pe_diag, OID_AUTO, phys,
    CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_MPSAFE, NULL, 0, pd_sysctl_phys,
    "", "physical memory at addr, len bytes");

/*
 * The loader's splash, as the kernel first finds it: PE_boot_console_fb()
 * copies the middle of the booter's framebuffer here before the kernel
 * paints over it. debug.pe_diag.loader_snap: a 16-byte header (width,
 * height, bytes per pixel, row bytes) then the rows.
 */
#define	PD_SNAP_W	400
#define	PD_SNAP_H	280
static uint32_t pd_snap_hdr[4];
static uint8_t pd_snap[PD_SNAP_W * PD_SNAP_H * 4];

void	pe_diag_snap(const struct fb_info *fb);

void
pe_diag_snap(const struct fb_info *fb)
{
	const uint8_t *src;
	u_int bpp, x0, y0, y, w, h;

	if (fb == NULL || fb->fb_vbase == 0 || pd_snap_hdr[0] != 0)
		return;
	bpp = fb->fb_bpp / 8;
	if (bpp != 2 && bpp != 4)
		return;
	w = MIN(PD_SNAP_W, (u_int)fb->fb_width);
	h = MIN(PD_SNAP_H, (u_int)fb->fb_height);
	x0 = (fb->fb_width - w) / 2;
	y0 = (fb->fb_height - h) / 2 + 60;
	if (y0 + h > (u_int)fb->fb_height)
		y0 = fb->fb_height - h;
	for (y = 0; y < h; y++) {
		src = (const uint8_t *)fb->fb_vbase +
		    (size_t)(y0 + y) * fb->fb_stride + (size_t)x0 * bpp;
		memcpy(&pd_snap[(size_t)y * w * bpp], src, (size_t)w * bpp);
	}
	pd_snap_hdr[0] = w;
	pd_snap_hdr[1] = h;
	pd_snap_hdr[2] = bpp;
	pd_snap_hdr[3] = y0;
}

static int
pd_sysctl_snap(SYSCTL_HANDLER_ARGS)
{
	int error;

	error = SYSCTL_OUT(req, pd_snap_hdr, sizeof(pd_snap_hdr));
	if (error == 0 && pd_snap_hdr[0] != 0)
		error = SYSCTL_OUT(req, pd_snap,
		    pd_snap_hdr[0] * pd_snap_hdr[1] * pd_snap_hdr[2]);
	return (error);
}
SYSCTL_PROC(_debug_pe_diag, OID_AUTO, loader_snap,
    CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_MPSAFE, NULL, 0, pd_sysctl_snap,
    "", "the middle of the booter's framebuffer before the kernel paints");
