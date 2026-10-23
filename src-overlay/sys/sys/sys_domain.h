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
 * The Darwin system socket domain (PF_SYSTEM), nextbsd-kernel#292.
 *
 * Clean-room: only the interface facts (numbers and layouts) are taken from
 * Darwin. The kernel event socket (SYSPROTO_EVENT) lives in
 * net/kern_event_sock.c; see sys/kern_event.h for its messages and ioctls.
 */
#ifndef _SYS_SYS_DOMAIN_H_
#define	_SYS_SYS_DOMAIN_H_

#include <sys/types.h>

/*
 * Darwin uses 32, which on NextBSD is AF_NETGRAPH; taking it would rule out
 * netgraph and the netgraph-based Bluetooth stack. Use the first vendor
 * family instead (AF_VENDOR00, sys/socket.h).
 */
#ifndef AF_SYSTEM
#define	AF_SYSTEM		39	/* == AF_VENDOR00 */
#endif
#ifndef PF_SYSTEM
#define	PF_SYSTEM		AF_SYSTEM
#endif

/* Protocols within PF_SYSTEM. */
#define	SYSPROTO_EVENT		1	/* kernel events */
#define	SYSPROTO_CONTROL	2	/* kernel control (reserved) */

#define	AF_SYS_CONTROL		2	/* sub address type for SYSPROTO_CONTROL */

/* System family socket address. */
struct sockaddr_sys {
	u_char		ss_len;		/* sizeof(struct sockaddr_sys) */
	u_char		ss_family;	/* AF_SYSTEM */
	u_int16_t	ss_sysaddr;	/* protocol address in AF_SYSTEM */
	u_int32_t	ss_reserved[7];	/* reserved to the protocol */
};

#endif /* !_SYS_SYS_DOMAIN_H_ */
