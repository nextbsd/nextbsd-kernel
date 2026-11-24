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
