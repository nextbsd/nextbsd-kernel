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
 * Darwin network kernel events (KEV_NETWORK_CLASS), nextbsd-kernel#292.
 *
 * Clean-room: only the interface facts (event codes and payload layouts) are
 * taken from Darwin. Darwin spreads the payload structs over if_var.h and
 * in{,6}_var.h; they are folded in here so the NextBSD headers of those names
 * are not shadowed. Layouts match Darwin field for field. Address-family
 * values inside payloads are NextBSD's (AF_INET6 is 28).
 */
#ifndef _NET_NET_KEV_H_
#define	_NET_NET_KEV_H_

#include <sys/types.h>
#include <netinet/in.h>

/* Subclasses of KEV_NETWORK_CLASS. */
#define	KEV_INET_SUBCLASS	1
#define	KEV_DL_SUBCLASS		2
#define	KEV_INET6_SUBCLASS	6

/* KEV_INET_SUBCLASS codes. */
#define	KEV_INET_NEW_ADDR		1	/* kev_in_data */
#define	KEV_INET_CHANGED_ADDR		2	/* kev_in_data */
#define	KEV_INET_ADDR_DELETED		3	/* kev_in_data */
#define	KEV_INET_SIFDSTADDR		4	/* kev_in_data */
#define	KEV_INET_SIFBRDADDR		5	/* kev_in_data */
#define	KEV_INET_SIFNETMASK		6	/* kev_in_data */
#define	KEV_INET_ARPCOLLISION		7	/* kev_in_collision */
#define	KEV_INET_PORTINUSE		8
#define	KEV_INET_ARPRTRFAILURE		9
#define	KEV_INET_ARPRTRALIVE		10

/* KEV_DL_SUBCLASS codes. */
#define	KEV_DL_SIFFLAGS			1
#define	KEV_DL_SIFMETRICS		2
#define	KEV_DL_SIFMTU			3
#define	KEV_DL_SIFPHYS			4
#define	KEV_DL_SIFMEDIA			5
#define	KEV_DL_SIFGENERIC		6
#define	KEV_DL_ADDMULTI			7
#define	KEV_DL_DELMULTI			8
#define	KEV_DL_IF_ATTACHED		9
#define	KEV_DL_IF_DETACHING		10
#define	KEV_DL_IF_DETACHED		11
#define	KEV_DL_LINK_OFF			12
#define	KEV_DL_LINK_ON			13
#define	KEV_DL_PROTO_ATTACHED		14	/* kev_dl_proto_data */
#define	KEV_DL_PROTO_DETACHED		15	/* kev_dl_proto_data */
#define	KEV_DL_LINK_ADDRESS_CHANGED	16

/* KEV_INET6_SUBCLASS codes. */
#define	KEV_INET6_NEW_USER_ADDR		1	/* kev_in6_data; DAD done */
#define	KEV_INET6_CHANGED_ADDR		2	/* kev_in6_data */
#define	KEV_INET6_ADDR_DELETED		3	/* kev_in6_data */
#define	KEV_INET6_NEW_LL_ADDR		4	/* kev_in6_data */
#define	KEV_INET6_NEW_RTADV_ADDR	5	/* kev_in6_data */
#define	KEV_INET6_DEFROUTER		6

/*
 * Leads every network payload; KEV_DL_* events other than the PROTO ones
 * carry only this. if_name is the driver name without the unit ("em" for
 * em0), and if_family is Darwin's IFNET_FAMILY_* value.
 */
struct net_event_data {
	u_int32_t	if_family;
	u_int32_t	if_unit;
	char		if_name[16];	/* IFNAMSIZ */
};

struct kev_dl_proto_data {
	struct net_event_data	link_data;
	u_int32_t		proto_family;
	u_int32_t		proto_remaining_count;
};

struct kev_in_data {
	struct net_event_data	link_data;
	struct in_addr		ia_addr;	/* interface address */
	u_int32_t		ia_net;		/* network number */
	u_int32_t		ia_netmask;	/* mask of the net part */
	u_int32_t		ia_subnet;	/* subnet number, incl. net */
	u_int32_t		ia_subnetmask;	/* mask of the subnet part */
	struct in_addr		ia_netbroadcast;
	struct in_addr		ia_dstaddr;
};

struct kev_in_collision {
	struct net_event_data	link_data;	/* link the ARP came in on */
	struct in_addr		ia_ipaddr;	/* conflicting address */
	u_char			hw_len;		/* length of hw_addr */
	u_char			hw_addr[];	/* conflicting hardware address */
};

struct kev_in6_addrlifetime {
	u_int32_t	ia6t_expire;	/* valid lifetime expiration time */
	u_int32_t	ia6t_preferred;	/* preferred lifetime expiration time */
	u_int32_t	ia6t_vltime;	/* valid lifetime */
	u_int32_t	ia6t_pltime;	/* preferred lifetime */
};

struct kev_in6_data {
	struct net_event_data		link_data;
	struct sockaddr_in6		ia_addr;	/* interface address */
	struct sockaddr_in6		ia_net;		/* network number */
	struct sockaddr_in6		ia_dstaddr;	/* destination address */
	struct sockaddr_in6		ia_prefixmask;
	u_int32_t			ia_plen;	/* prefix length */
	u_int32_t			ia6_flags;	/* IN6_IFF_* */
	struct kev_in6_addrlifetime	ia_lifetime;
	u_int8_t			ia_mac[6];	/* ETHER_ADDR_LEN */
};

#endif /* !_NET_NET_KEV_H_ */
