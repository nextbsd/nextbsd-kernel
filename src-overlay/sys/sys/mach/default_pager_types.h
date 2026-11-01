/*
 * default_pager_types.h — macx_triggers() flags and the
 * default_pager_space_alert message (nextbsd-kernel#218).
 *
 * Darwin's dynamic_pager registers a receive right with the kernel through
 * the macx_triggers() Mach trap (xnu-2422 bsd/vm/dp_backing_file.c,
 * osfmk/vm/bsd_vm.c mach_macx_triggers()). When free backing store crosses
 * a threshold the kernel sends that port a default_pager_space_alert, the
 * one simpleroutine of the default_pager_alerts subsystem (2295) in
 * xnu-2422 osfmk/default_pager/default_pager_alerts.defs:
 *
 *	simpleroutine default_pager_space_alert(
 *		alert_port	: mach_port_t;
 *	in	flags		: int);
 *
 * NextBSD sends the same message for the swap pager's almost-full
 * transitions (the swap_lowspace eventhandler, patches/0080): HI_WAT_ALERT
 * when free swap falls below vm.nswap_lowat, LO_WAT_ALERT when it rises
 * back above vm.nswap_hiwat. A MIG server generated from Darwin's
 * default_pager_alerts.defs receives it unchanged.
 *
 * The flag values are Darwin's (xnu-2422
 * osfmk/default_pager/default_pager_types.h).
 */
#ifndef _SYS_MACH_DEFAULT_PAGER_TYPES_H_
#define _SYS_MACH_DEFAULT_PAGER_TYPES_H_

#include <sys/mach/message.h>
#include <sys/mach/ndr.h>

#define	HI_WAT_ALERT			0x01	/* free swap below the low-water mark */
#define	LO_WAT_ALERT			0x02	/* free swap back above the high-water mark */
#define	SWAP_ENCRYPT_ON			0x04
#define	SWAP_ENCRYPT_OFF		0x08
#define	SWAP_COMPACT_DISABLE		0x10
#define	SWAP_COMPACT_ENABLE		0x20
#define	PROC_RESUME			0x40
#define	SWAP_FILE_CREATION_ERROR	0x80
#define	USE_EMERGENCY_SWAP_FILE_FIRST	0x100

/* default_pager_alerts subsystem base; default_pager_space_alert is +0. */
#define	DEFAULT_PAGER_SPACE_ALERT_MSGID	2295

/*
 * Wire format: MIG's __Request__default_pager_space_alert_t (header, NDR,
 * flags; msgh_size 36), plus the trailer the receiver gets appended.
 */
typedef struct {
	mach_msg_header_t		Head;
	NDR_record_t			NDR;
	int				flags;		/* HI_WAT_ALERT or LO_WAT_ALERT */
	mach_msg_format_0_trailer_t	trailer;	/* appended on receive */
} default_pager_space_alert_msg_t;

#endif /* _SYS_MACH_DEFAULT_PAGER_TYPES_H_ */
