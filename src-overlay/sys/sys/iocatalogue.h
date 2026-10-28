/*
 * NextBSD in-kernel IOKit catalogue (K2, nextbsd#215).
 *
 * A flat, in-kernel database of IOKit driver personalities (device-id match
 * records) — the Apple-faithful IOCatalogue. Userland (kextd) parses each kext
 * bundle's Info.plist IOKitPersonalities and PUSHES a flat match-record per
 * personality here via ioctl on /dev/iocatalogue. This is mechanism (a): the
 * kernel never parses XML — userland owns plist parsing, the kernel owns the
 * store + matching. The in-kernel matcher (K3, nextbsd#216) reads these records
 * to bind an unmatched device to its driver bundle.
 *
 * See pkgdemon.github.io/nextbsd-inkernel-iokit-feasibility.html §9.
 */
#ifndef _SYS_IOCATALOGUE_H_
#define _SYS_IOCATALOGUE_H_

#include <sys/types.h>
#include <sys/ioccom.h>

#define	IOCAT_BUNDLE_ID_MAX	128	/* max CFBundleIdentifier incl. NUL */
#define	IOCAT_MAX_MATCH		512	/* max device-id entries per personality */

/* IOProviderClass — stored opaque by K2; interpreted by the K3 matcher. */
#define	IOCAT_PROVIDER_UNKNOWN		0
#define	IOCAT_PROVIDER_IOPCIDEVICE	1
/*
 * Device-tree nodes. Apple's name for this provider, and IONameMatch is the
 * personality key that carries the strings -- so a NextBSD personality reads
 * the same as a Darwin one rather than inventing a NextBSD-only spelling.
 */
#define	IOCAT_PROVIDER_IOPLATFORMDEVICE	2
/*
 * nextbsd-kernel#329: USB and PCI-class personalities. All three ride the
 * existing struct iocat_add (IOCATIOCADD) -- only provider_class and the
 * meaning of match[] differ -- so the kextd ABI does not change size.
 *
 * IOUSBHostDevice: match[] holds 0x<idProduct><idVendor> words (product in the
 *   high 16 bits, vendor low), the USB twin of IOPCIPrimaryMatch.
 * IOUSBHostInterface: match[] holds (value, mask) PAIRS over the interface
 *   triple 0x00<bInterfaceClass><bInterfaceSubClass><bInterfaceProtocol>; a
 *   device matches when (triple & mask) == (value & mask). nmatch is even.
 * IOPCIClassMatch (provider IOPCIDevice in a Darwin plist; its own class here):
 *   match[] holds (value, mask) PAIRS over Darwin's IOPCIClassMatch word
 *   0x<class><subclass><progif><revision>, e.g. 0x01080200&0xffffff00 for NVMe.
 *
 * ACPI _HID personalities (IOACPIPlatformDevice + IONameMatch) use the
 * device-tree string path (IOCATIOCADDCOMPAT) with each name prefixed "acpi:"
 * (e.g. "acpi:PNP0C14"), so an ACPI id can never collide with an FDT
 * compatible string.
 */
#define	IOCAT_PROVIDER_IOUSBHOSTDEVICE	3
#define	IOCAT_PROVIDER_IOUSBHOSTINTERFACE 4
#define	IOCAT_PROVIDER_IOPCICLASS	5
#define	IOCAT_ACPI_PREFIX		"acpi:"

/* Longest FDT compatible string we will store, including NUL. */
#define	IOCAT_COMPAT_MAX		128

/*
 * One personality, as pushed from userland. `match` points at an array of
 * `nmatch` uint32_t PCI match words, each encoded 0x<device><vendor> (device in
 * the high 16 bits, vendor in the low 16) — the IOPCIPrimaryMatch form, e.g.
 * 0x24f38086 for the Intel 8260. `match` is a uint64_t so the ABI is identical
 * for 32- and 64-bit userland; `_pad` keeps it 8-byte aligned deterministically.
 */
struct iocat_add {
	char		bundle_id[IOCAT_BUNDLE_ID_MAX];
	uint32_t	provider_class;		/* IOCAT_PROVIDER_* */
	int32_t		probe_score;		/* IOProbeScore; higher wins */
	uint32_t	nmatch;			/* number of entries in match[] */
	uint32_t	_pad;
	uint64_t	match;			/* user ptr to uint32_t[nmatch] */
};

/*
 * Look up the best driver bundle for a PCI match word (0x<device><vendor>).
 * Userland sets `match`; the kernel fills `bundle_id` + `score` and returns 0,
 * or ENOENT if nothing matches. This is the same lookup the in-kernel
 * device_nomatch matcher (K3) uses — exposed so userland can verify it
 * deterministically (e.g. is the 8260 bound to IntelWiFi?).
 */
struct iocat_lookup {
	uint32_t	match;				/* in: 0x<device><vendor> */
	int32_t		score;				/* out: winning IOProbeScore */
	char		bundle_id[IOCAT_BUNDLE_ID_MAX];	/* out: winning bundle */
};

/*
 * A device-tree personality (nextbsd-kernel-extensions-graphics#185).
 *
 * PCI personalities match a 32-bit 0x<device><vendor> word; a device-tree node
 * has no such id, it has a "compatible" string list. So this is a SEPARATE
 * record type rather than a wider `match` in struct iocat_add -- that struct is
 * duplicated verbatim into kextd (nextbsd-userland
 * src/kext_tools/kextd/iocatalogue.h) and is a kernel<->userland ABI. Widening
 * it would silently break a kextd built against the old header; adding a record
 * type leaves the old path byte-identical.
 *
 * `compat` points at `ncompat` fixed-width IOCAT_COMPAT_MAX strings laid end to
 * end -- fixed width rather than packed NUL-separated so the kernel can bound
 * each copyin without walking user memory looking for terminators. A uint64_t
 * for the same 32/64-bit ABI reason as struct iocat_add::match.
 */
struct iocat_add_compat {
	char		bundle_id[IOCAT_BUNDLE_ID_MAX];
	int32_t		probe_score;		/* IOProbeScore; higher wins */
	uint32_t	ncompat;		/* number of strings */
	uint64_t	compat;			/* user ptr to char[ncompat][IOCAT_COMPAT_MAX] */
};

/*
 * Look up the best driver bundle for an FDT compatible string. Userland sets
 * `compat`; the kernel fills `bundle_id` + `score`, or returns ENOENT.
 * The userland-visible half of iocat_lookup_compat(), for the same reason
 * IOCATIOCLOOKUP exists: so the match can be verified deterministically
 * without waiting for a device to go unmatched.
 */
struct iocat_lookup_compat {
	char		compat[IOCAT_COMPAT_MAX];	/* in */
	int32_t		score;				/* out */
	char		bundle_id[IOCAT_BUNDLE_ID_MAX];	/* out */
};

#define	IOCATIOCADD	_IOW('K', 1, struct iocat_add)		/* add a personality */
#define	IOCATIOCFLUSH	_IO('K', 2)				/* drop all (re-push) */
#define	IOCATIOCLOOKUP	_IOWR('K', 3, struct iocat_lookup)	/* match a PCI word */
/*
 * K3b PoC (#216): look up a PCI match word and, on a hit, fire the kernel->kextd
 * Mach load request (HOST_KEXTD_PORT) for the winning bundle — the de-risk test
 * for the matcher's real send. Throwaway: the production matcher sends from its
 * device_nomatch taskqueue, not via an ioctl. Returns 0 / ENOENT (no match) /
 * ENXIO (no kextd registered) / ENOSYS (kernel built without COMPAT_MACH).
 */
#define	IOCATIOCTESTSEND	_IOW('K', 4, uint32_t)
/* Device-tree personalities (#185). New numbers; 1-4 keep their meaning. */
#define	IOCATIOCADDCOMPAT	_IOW('K', 5, struct iocat_add_compat)
#define	IOCATIOCLOOKUPCOMPAT	_IOWR('K', 6, struct iocat_lookup_compat)

/*
 * Kernel-requested loads by module name (nextbsd-kernel#52). kern_kldload()
 * passes a module name it finds nowhere to the catalogue, which sends kextd an
 * IOKIT_KEXTD_KLD_MSGID request and sleeps until kextd reports the outcome.
 *
 * IOCATIOCKLDSERVE: the calling process serves these requests until it closes
 *   the descriptor it was issued on (kextd -w). Without one, a miss fails at
 *   once with ENOENT, as on stock FreeBSD.
 * IOCATIOCKLDDONE: the request `seq` has finished; `error` is 0 when the
 *   module is now loaded, else an errno (ENOENT: no kext provides it).
 */
struct iocat_kld_done {
	uint32_t	seq;
	int32_t		error;
};
#define	IOCATIOCKLDSERVE	_IO('K', 7)
#define	IOCATIOCKLDDONE		_IOW('K', 8, struct iocat_kld_done)

#ifdef _KERNEL
#include <sys/queue.h>
#include <sys/sysctl.h>

/*
 * The hw.iokit sysctl node is defined (non-static) in iokit_catalogue.c. K1's
 * iokit_registry.c declares it here so it can add hw.iokit.registry to the same
 * node — the canonical FreeBSD cross-file SYSCTL_NODE pattern.
 */
SYSCTL_DECL(_hw_iokit);

/*
 * In-kernel record (one personality).
 *
 * A PCI record uses match[]/nmatch (0x<device><vendor> words); an FDT record
 * uses compat[]/ncompat (IOCAT_COMPAT_MAX-wide strings). provider_class says
 * which, and only one of the two is ever non-NULL.
 */
struct iocat_record {
	TAILQ_ENTRY(iocat_record) link;
	char		bundle_id[IOCAT_BUNDLE_ID_MAX];
	uint32_t	provider_class;
	int32_t		probe_score;
	uint32_t	nmatch;
	uint32_t       *match;
	uint32_t	ncompat;
	char	       *compat;		/* ncompat * IOCAT_COMPAT_MAX */
};

/*
 * K3 matcher entry point: find the best (highest probe_score) IOPCIDevice
 * personality whose match table contains `match_word` (0x<device><vendor>).
 * On a hit, copies the winning bundle id into buf and returns 0 (and the score
 * via score_out if non-NULL); returns ENOENT if nothing matches. Takes its own
 * lock; caller holds none.
 */
int	iocat_lookup_pci(uint32_t match_word, char *buf, size_t buflen,
	    int32_t *score_out);

/*
 * The device-tree twin of iocat_lookup_pci(): find the best IOFDTDevice
 * personality listing `compat` among its compatible strings. Same contract --
 * takes its own lock, caller holds none, returns 0 on a hit or ENOENT.
 */
int	iocat_lookup_compat(const char *compat, char *buf, size_t buflen,
	    int32_t *score_out);

/*
 * nextbsd-kernel#329: exact word lookup for any word-matched provider
 * (IOPCIDevice, IOUSBHostDevice), and masked (value, mask) pair lookup for the
 * class providers (IOUSBHostInterface, IOPCIClass). Same contract as above.
 */
int	iocat_lookup_word(uint32_t provider, uint32_t word, char *buf,
	    size_t buflen, int32_t *score_out);
int	iocat_lookup_masked(uint32_t provider, uint32_t word, char *buf,
	    size_t buflen, int32_t *score_out);
#endif /* _KERNEL */

#endif /* _SYS_IOCATALOGUE_H_ */
