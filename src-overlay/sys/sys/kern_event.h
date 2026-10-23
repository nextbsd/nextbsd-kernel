/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The NextBSD Project
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Darwin kernel event messages and the SYSPROTO_EVENT socket ioctls,
 * nextbsd-kernel#292.
 *
 * Clean-room: only the interface facts (constants, struct layouts, ioctl
 * numbers and the filter rule) are taken from Darwin.
 *
 * A socket(PF_SYSTEM, SOCK_RAW, SYSPROTO_EVENT) receives nothing until a
 * filter is set with SIOCSKEVFILT. An event is delivered when the filter's
 * vendor_code is KEV_ANY_VENDOR, or it matches and then kev_class is
 * KEV_ANY_CLASS or matches, and then kev_subclass is KEV_ANY_SUBCLASS or
 * matches. Each read returns one struct kern_event_msg.
 */
#ifndef _SYS_KERN_EVENT_H_
#define	_SYS_KERN_EVENT_H_

#include <sys/cdefs.h>
#include <sys/types.h>
#include <sys/ioccom.h>
#include <sys/sys_domain.h>

#define	KEV_SNDSPACE		(4 * 1024)
#define	KEV_RECVSPACE		(32 * 1024)

/* Filter wildcards. */
#define	KEV_ANY_VENDOR		0
#define	KEV_ANY_CLASS		0
#define	KEV_ANY_SUBCLASS	0

/* Vendor codes. Others are allocated dynamically; see SIOCGKEVVENDOR. */
#define	KEV_VENDOR_APPLE	1

/* Classes for KEV_VENDOR_APPLE. */
#define	KEV_NETWORK_CLASS	1	/* subclasses and codes: net/net_kev.h */
#define	KEV_IOKIT_CLASS		2
#define	KEV_SYSTEM_CLASS	3
#define	KEV_APPLESHARE_CLASS	4
#define	KEV_FIREWALL_CLASS	5
#define	KEV_IEEE80211_CLASS	6

/*
 * Header of every message read from an event socket. total_size covers the
 * header and event_data; the payload length is
 * total_size - KEV_MSG_HEADER_SIZE.
 */
struct kern_event_msg {
	u_int32_t	total_size;	/* size of the whole message */
	u_int32_t	vendor_code;	/* KEV_VENDOR_* */
	u_int32_t	kev_class;	/* layer of the event source */
	u_int32_t	kev_subclass;	/* component within the layer */
	u_int32_t	id;		/* monotonically increasing */
	u_int32_t	event_code;	/* event within the subclass */
	u_int32_t	event_data[1];	/* payload, one or more words */
};

#define	KEV_MSG_HEADER_SIZE	(__offsetof(struct kern_event_msg, event_data[0]))

/* SIOCSKEVFILT / SIOCGKEVFILT argument. */
struct kev_request {
	u_int32_t	vendor_code;
	u_int32_t	kev_class;
	u_int32_t	kev_subclass;
};

#define	KEV_VENDOR_CODE_MAX_STR_LEN	200

/* SIOCGKEVVENDOR argument: bundle-style vendor string in, code out. */
struct kev_vendor_code {
	u_int32_t	vendor_code;
	char		vendor_string[KEV_VENDOR_CODE_MAX_STR_LEN];
};

#define	SIOCGKEVID	_IOR('e', 1, u_int32_t)		/* current event id */
#define	SIOCSKEVFILT	_IOW('e', 2, struct kev_request)
#define	SIOCGKEVFILT	_IOR('e', 3, struct kev_request)
#define	SIOCGKEVVENDOR	_IOWR('e', 4, struct kev_vendor_code)

#ifdef _KERNEL
/*
 * Posting interface. The kernel joins the data vectors, in order, behind a
 * kern_event_msg header and stamps the id; a vector with data_length 0 ends
 * the list.
 */
#define	N_KEV_VECTORS	5

struct kev_d_vectors {
	u_int32_t	data_length;	/* length of the data */
	void		*data_ptr;	/* the data */
};

struct kev_msg {
	u_int32_t		vendor_code;
	u_int32_t		kev_class;
	u_int32_t		kev_subclass;
	u_int32_t		event_code;
	struct kev_d_vectors	dv[N_KEV_VECTORS];
};

int	kev_post_msg(struct kev_msg *event);
#endif /* _KERNEL */

#endif /* !_SYS_KERN_EVENT_H_ */
