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
 * Darwin network kernel events (KEV_NETWORK_CLASS) from the existing
 * interface and address EVENTHANDLERs, nextbsd-kernel#293:
 *
 *   ifnet_arrival_event	KEV_DL_IF_ATTACHED, then PROTO_ATTACHED per family
 *   ifnet_departure_event	KEV_DL_IF_DETACHING, PROTO_DETACHED per family,
 *				then IF_DETACHED
 *   ifnet_link_event		KEV_DL_LINK_ON / LINK_OFF (UNKNOWN is skipped)
 *   iflladdr_event		KEV_DL_LINK_ADDRESS_CHANGED
 *   rt_ifmsg_event		KEV_DL_SIFFLAGS / SIFMTU (patched into rt_ifmsg)
 *   ifaddr_event_ext		KEV_INET_NEW_ADDR / ADDR_DELETED
 *   rt_addrmsg (AF_INET6)	KEV_INET6_NEW_{LL,RTADV,USER}_ADDR / ADDR_DELETED
 *   arp_collision_event	KEV_INET_ARPCOLLISION (base patch)
 *   inet6 ::/0 rib_subscribe	KEV_INET6_DEFROUTER
 *
 * Darwin has no PROTO_* source here, so they are synthesized at arrival and
 * departure. A rename fires departure then arrival, so it is reported as a
 * detach then an attach. IPv4 uses ifaddr_event_ext, not rt_addrmsg, which
 * fires again when the interface comes up. IPv6 addresses are reported when
 * added, while still tentative.
 *
 * Every field is copied out of the ifnet before posting; nothing keeps ifp.
 */

#include "opt_inet.h"
#include "opt_inet6.h"
#include "opt_route.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/limits.h>
#include <sys/ctype.h>
#include <sys/eventhandler.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/kern_event.h>

#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_var.h>
#include <net/if_private.h>
#include <net/if_types.h>
#include <net/route.h>
#include <net/route/nhop.h>
#include <net/route/route_ctl.h>
#include <net/vnet.h>
#include <net/net_kev.h>

#include <netinet/in.h>
#ifdef INET
#include <netinet/in_var.h>
#include <netinet/if_ether.h>
#endif
#ifdef INET6
#include <netinet6/in6_var.h>
#endif

/* Darwin's IFNET_FAMILY_* values for net_event_data.if_family. */
#define	KEV_IFNET_FAMILY_ANY		0
#define	KEV_IFNET_FAMILY_LOOPBACK	1
#define	KEV_IFNET_FAMILY_ETHERNET	2
#define	KEV_IFNET_FAMILY_SLIP		3
#define	KEV_IFNET_FAMILY_VLAN		5
#define	KEV_IFNET_FAMILY_PPP		6
#define	KEV_IFNET_FAMILY_GIF		10
#define	KEV_IFNET_FAMILY_STF		12
#define	KEV_IFNET_FAMILY_FIREWIRE	13
#define	KEV_IFNET_FAMILY_BOND		14

static u_int32_t
kev_net_family(const struct ifnet *ifp)
{

	switch (ifp->if_type) {
	case IFT_LOOP:
		return (KEV_IFNET_FAMILY_LOOPBACK);
	case IFT_ETHER:
	case IFT_BRIDGE:
	case IFT_IEEE80211:
		return (KEV_IFNET_FAMILY_ETHERNET);
	case IFT_SLIP:
		return (KEV_IFNET_FAMILY_SLIP);
	case IFT_L2VLAN:
		return (KEV_IFNET_FAMILY_VLAN);
	case IFT_PPP:
		return (KEV_IFNET_FAMILY_PPP);
	case IFT_GIF:
		return (KEV_IFNET_FAMILY_GIF);
	case IFT_STF:
		return (KEV_IFNET_FAMILY_STF);
	case IFT_IEEE1394:
		return (KEV_IFNET_FAMILY_FIREWIRE);
	case IFT_IEEE8023ADLAG:
		return (KEV_IFNET_FAMILY_BOND);
	default:
		return (KEV_IFNET_FAMILY_ANY);
	}
}

/*
 * Darwin carries a driver name and a unit; split if_xname at its trailing
 * digits ("em0" is "em" and 0). A name with no trailing digits ("epair0a")
 * is passed whole with unit 0.
 */
static void
kev_net_fill(const struct ifnet *ifp, struct net_event_data *ed)
{
	const char *name = ifp->if_xname;
	size_t end, len;

	memset(ed, 0, sizeof(*ed));
	ed->if_family = kev_net_family(ifp);
	len = strnlen(name, sizeof(ifp->if_xname));
	for (end = len; end > 1 && isdigit((u_char)name[end - 1]); end--)
		;
	if (end < len)
		ed->if_unit = (u_int32_t)strtoul(name + end, NULL, 10);
	memcpy(ed->if_name, name, MIN(end, sizeof(ed->if_name) - 1));
}

static void
kev_net_post(struct ifnet *ifp, u_int32_t subclass, u_int32_t code,
    void *data, u_int32_t len)
{
	struct kev_msg ev;

	memset(&ev, 0, sizeof(ev));
	ev.vendor_code = KEV_VENDOR_APPLE;
	ev.kev_class = KEV_NETWORK_CLASS;
	ev.kev_subclass = subclass;
	ev.event_code = code;
	ev.dv[0].data_length = len;
	ev.dv[0].data_ptr = data;
	CURVNET_SET_QUIET(ifp->if_vnet);
	(void)kev_post_msg(&ev);
	CURVNET_RESTORE();
}

static void
kev_net_dl(struct ifnet *ifp, u_int32_t code)
{
	struct net_event_data ed;

	kev_net_fill(ifp, &ed);
	kev_net_post(ifp, KEV_DL_SUBCLASS, code, &ed, sizeof(ed));
}

static void
kev_net_proto(struct ifnet *ifp, u_int32_t code, u_int32_t family,
    u_int32_t remaining)
{
	struct kev_dl_proto_data pd;

	kev_net_fill(ifp, &pd.link_data);
	pd.proto_family = family;
	pd.proto_remaining_count = remaining;
	kev_net_post(ifp, KEV_DL_SUBCLASS, code, &pd, sizeof(pd));
}

static void
kev_net_arrival(void *arg __unused, struct ifnet *ifp)
{
	u_int32_t n = 0;

	kev_net_dl(ifp, KEV_DL_IF_ATTACHED);
#ifdef INET
	kev_net_proto(ifp, KEV_DL_PROTO_ATTACHED, AF_INET, ++n);
#endif
#ifdef INET6
	kev_net_proto(ifp, KEV_DL_PROTO_ATTACHED, AF_INET6, ++n);
#endif
}
EVENTHANDLER_DEFINE(ifnet_arrival_event, kev_net_arrival, NULL,
    EVENTHANDLER_PRI_ANY);

static void
kev_net_departure(void *arg __unused, struct ifnet *ifp)
{
	u_int32_t n = 0;

#ifdef INET
	n++;
#endif
#ifdef INET6
	n++;
#endif
	kev_net_dl(ifp, KEV_DL_IF_DETACHING);
#ifdef INET6
	kev_net_proto(ifp, KEV_DL_PROTO_DETACHED, AF_INET6, --n);
#endif
#ifdef INET
	kev_net_proto(ifp, KEV_DL_PROTO_DETACHED, AF_INET, --n);
#endif
	kev_net_dl(ifp, KEV_DL_IF_DETACHED);
}
EVENTHANDLER_DEFINE(ifnet_departure_event, kev_net_departure, NULL,
    EVENTHANDLER_PRI_ANY);

static void
kev_net_link(void *arg __unused, struct ifnet *ifp, int state)
{

	if (state == LINK_STATE_UP)
		kev_net_dl(ifp, KEV_DL_LINK_ON);
	else if (state == LINK_STATE_DOWN)
		kev_net_dl(ifp, KEV_DL_LINK_OFF);
}
EVENTHANDLER_DEFINE(ifnet_link_event, kev_net_link, NULL,
    EVENTHANDLER_PRI_ANY);

static void
kev_net_lladdr(void *arg __unused, struct ifnet *ifp)
{

	kev_net_dl(ifp, KEV_DL_LINK_ADDRESS_CHANGED);
}
EVENTHANDLER_DEFINE(iflladdr_event, kev_net_lladdr, NULL,
    EVENTHANDLER_PRI_ANY);

/*
 * KEV_DL_SIFFLAGS / KEV_DL_SIFMTU, nextbsd-kernel#294. rt_ifmsg() fires on
 * a flag change (if_up/if_down, if_setflag), on an MTU change and on a link
 * state change; the last two both pass mask 0. Keep the last-seen flags and
 * MTU per if_index and post only what actually moved, so a link change
 * alone posts nothing here (ifnet_link_event covers it). The cache is
 * seeded at arrival and dropped at departure, so a reused index starts
 * fresh. An index with no entry (allocation failed) falls back to the mask.
 */
struct kev_net_ifstate {
	int		valid;
	int		flags;
	u_long		mtu;
};

static MALLOC_DEFINE(M_KEVNET, "kev_net", "kernel event interface state");
static struct mtx kev_net_ifs_mtx;
MTX_SYSINIT(kev_net_ifs, &kev_net_ifs_mtx, "kev_net ifstate", MTX_DEF);
static struct kev_net_ifstate *kev_net_ifs;
static u_int kev_net_nifs;

/* Return the entry for idx, growing the table if needed; NULL on failure. */
static struct kev_net_ifstate *
kev_net_ifstate(u_int idx)
{
	struct kev_net_ifstate *n;
	u_int cnt;

	mtx_assert(&kev_net_ifs_mtx, MA_OWNED);
	if (idx < kev_net_nifs)
		return (&kev_net_ifs[idx]);
	for (cnt = MAX(kev_net_nifs, 16); cnt <= idx; cnt *= 2)
		;
	n = malloc(cnt * sizeof(*n), M_KEVNET, M_NOWAIT | M_ZERO);
	if (n == NULL)
		return (NULL);
	if (kev_net_ifs != NULL) {
		memcpy(n, kev_net_ifs, kev_net_nifs * sizeof(*n));
		free(kev_net_ifs, M_KEVNET);
	}
	kev_net_ifs = n;
	kev_net_nifs = cnt;
	return (&kev_net_ifs[idx]);
}

static void
kev_net_ifs_seed(void *arg __unused, struct ifnet *ifp)
{
	struct kev_net_ifstate *st;

	mtx_lock(&kev_net_ifs_mtx);
	st = kev_net_ifstate(ifp->if_index);
	if (st != NULL) {
		st->valid = 1;
		st->flags = ifp->if_flags;
		st->mtu = ifp->if_mtu;
	}
	mtx_unlock(&kev_net_ifs_mtx);
}

static void
kev_net_ifs_drop(void *arg __unused, struct ifnet *ifp)
{

	mtx_lock(&kev_net_ifs_mtx);
	if (ifp->if_index < kev_net_nifs)
		kev_net_ifs[ifp->if_index].valid = 0;
	mtx_unlock(&kev_net_ifs_mtx);
}

static void
kev_net_ifmsg(void *arg __unused, struct ifnet *ifp, int mask)
{
	struct kev_net_ifstate *st;
	int flags, sflags, smtu;
	u_long mtu;

	flags = ifp->if_flags;
	mtu = ifp->if_mtu;
	mtx_lock(&kev_net_ifs_mtx);
	st = kev_net_ifstate(ifp->if_index);
	if (st != NULL && st->valid) {
		sflags = (st->flags != flags);
		smtu = (st->mtu != mtu);
	} else {
		sflags = (mask != 0);
		smtu = 0;
	}
	if (st != NULL) {
		st->valid = 1;
		st->flags = flags;
		st->mtu = mtu;
	}
	mtx_unlock(&kev_net_ifs_mtx);

	if (sflags)
		kev_net_dl(ifp, KEV_DL_SIFFLAGS);
	if (smtu)
		kev_net_dl(ifp, KEV_DL_SIFMTU);
}

/*
 * Registered by hand: EVENTHANDLER_DEFINE names its tag and SYSINIT after
 * the event, so a second arrival/departure handler here cannot use it.
 */
static void
kev_net_ifmsg_init(void *arg __unused)
{

	EVENTHANDLER_REGISTER(ifnet_arrival_event, kev_net_ifs_seed, NULL,
	    EVENTHANDLER_PRI_ANY);
	EVENTHANDLER_REGISTER(ifnet_departure_event, kev_net_ifs_drop, NULL,
	    EVENTHANDLER_PRI_ANY);
	EVENTHANDLER_REGISTER(rt_ifmsg_event, kev_net_ifmsg, NULL,
	    EVENTHANDLER_PRI_ANY);
}
SYSINIT(kev_net_ifmsg, SI_SUB_CONFIGURE, SI_ORDER_ANY, kev_net_ifmsg_init,
    NULL);

#ifdef INET
/*
 * NextBSD has no classful ia_net/ia_netmask; report the subnet for both,
 * in host order as on Darwin.
 */
static void
kev_net_inet(void *arg __unused, struct ifnet *ifp, struct ifaddr *ifa,
    int event)
{
	struct in_ifaddr *ia;
	struct kev_in_data id;

	if (ifa->ifa_addr->sa_family != AF_INET)
		return;
	ia = (struct in_ifaddr *)ifa;

	memset(&id, 0, sizeof(id));
	kev_net_fill(ifp, &id.link_data);
	id.ia_addr = ia->ia_addr.sin_addr;
	id.ia_net = (u_int32_t)ia->ia_subnet;
	id.ia_netmask = (u_int32_t)ia->ia_subnetmask;
	id.ia_subnet = (u_int32_t)ia->ia_subnet;
	id.ia_subnetmask = (u_int32_t)ia->ia_subnetmask;
	id.ia_netbroadcast.s_addr =
	    htonl((u_int32_t)(ia->ia_subnet | ~ia->ia_subnetmask));
	id.ia_dstaddr = ia->ia_dstaddr.sin_addr;
	kev_net_post(ifp, KEV_INET_SUBCLASS, event == IFADDR_EVENT_ADD ?
	    KEV_INET_NEW_ADDR : KEV_INET_ADDR_DELETED, &id, sizeof(id));
}
EVENTHANDLER_DEFINE(ifaddr_event_ext, kev_net_inet, NULL,
    EVENTHANDLER_PRI_ANY);

/*
 * Another host is using one of our addresses. The hardware address follows
 * the struct, and the length is sizeof plus hw_len, as on Darwin. The
 * ARP_LOG beside the hook is untouched.
 */
static void
kev_net_arpcollision(void *arg __unused, struct ifnet *ifp,
    struct in_addr addr, const u_char *hw, int hwlen)
{
	u_char buf[sizeof(struct kev_in_collision) + UCHAR_MAX]
	    __aligned(sizeof(u_int32_t));
	struct kev_in_collision *ic = (struct kev_in_collision *)buf;

	if (hwlen < 0 || hwlen > UCHAR_MAX)
		return;
	memset(ic, 0, sizeof(*ic));
	kev_net_fill(ifp, &ic->link_data);
	ic->ia_ipaddr = addr;
	ic->hw_len = (u_char)hwlen;
	memcpy(ic->hw_addr, hw, hwlen);
	kev_net_post(ifp, KEV_INET_SUBCLASS, KEV_INET_ARPCOLLISION, ic,
	    sizeof(*ic) + hwlen);
}
EVENTHANDLER_DEFINE(arp_collision_event, kev_net_arpcollision, NULL,
    EVENTHANDLER_PRI_ANY);
#endif /* INET */

#ifdef INET6
/* Move a KAME embedded zone into sin6_scope_id, as userland expects. */
static void
kev_net_unembed(struct sockaddr_in6 *sin6)
{

	if (IN6_IS_SCOPE_LINKLOCAL(&sin6->sin6_addr) ||
	    IN6_IS_ADDR_MC_INTFACELOCAL(&sin6->sin6_addr)) {
		if (sin6->sin6_scope_id == 0)
			sin6->sin6_scope_id =
			    ntohs(sin6->sin6_addr.s6_addr16[1]);
		sin6->sin6_addr.s6_addr16[1] = 0;
	}
}

/* Lifetimes are uptime based in the kernel; Darwin reports wall clock. */
static u_int32_t
kev_net_walltime(time_t t)
{

	if (t == 0)
		return (0);
	return ((u_int32_t)(t - time_uptime + time_second));
}

static void
kev_net_inet6(void *arg __unused, struct ifaddr *ifa, int cmd)
{
	struct in6_ifaddr *ia;
	struct ifnet *ifp;
	struct kev_in6_data id;
	u_int32_t code;

	if (ifa->ifa_addr->sa_family != AF_INET6)
		return;
	ia = (struct in6_ifaddr *)ifa;
	ifp = ifa->ifa_ifp;

	if (cmd == RTM_DELETE)
		code = KEV_INET6_ADDR_DELETED;
	else if (IN6_IS_ADDR_LINKLOCAL(&ia->ia_addr.sin6_addr))
		code = KEV_INET6_NEW_LL_ADDR;
	else if ((ia->ia6_flags & IN6_IFF_AUTOCONF) != 0)
		code = KEV_INET6_NEW_RTADV_ADDR;
	else
		code = KEV_INET6_NEW_USER_ADDR;

	memset(&id, 0, sizeof(id));
	kev_net_fill(ifp, &id.link_data);
	id.ia_addr = ia->ia_addr;
	id.ia_net = ia->ia_net;
	id.ia_dstaddr = ia->ia_dstaddr;
	id.ia_prefixmask = ia->ia_prefixmask;
	kev_net_unembed(&id.ia_addr);
	kev_net_unembed(&id.ia_dstaddr);
	id.ia_plen = ia->ia_plen;
	id.ia6_flags = ia->ia6_flags;
	id.ia_lifetime.ia6t_expire =
	    kev_net_walltime(ia->ia6_lifetime.ia6t_expire);
	id.ia_lifetime.ia6t_preferred =
	    kev_net_walltime(ia->ia6_lifetime.ia6t_preferred);
	id.ia_lifetime.ia6t_vltime = ia->ia6_lifetime.ia6t_vltime;
	id.ia_lifetime.ia6t_pltime = ia->ia6_lifetime.ia6t_pltime;
	if (ifp->if_addr != NULL && ifp->if_addrlen == ETHER_ADDR_LEN)
		memcpy(id.ia_mac, IF_LLADDR(ifp), ETHER_ADDR_LEN);
	kev_net_post(ifp, KEV_INET6_SUBCLASS, code, &id, sizeof(id));
}
EVENTHANDLER_DEFINE(rt_addrmsg, kev_net_inet6, NULL, EVENTHANDLER_PRI_ANY);

/*
 * The IPv6 default route was added, changed or removed. The payload is a
 * kev_in6_data for the router's interface with the router in ia_addr (left
 * zero for an interface route). Each multipath member is reported on its own.
 */
static void
kev_net_defrouter_nh(const struct rib_cmd_info *rc, void *arg __unused)
{
	struct nhop_object *nh;
	struct ifnet *ifp;
	struct kev_in6_data id;

	nh = rc->rc_cmd == RTM_DELETE ? rc->rc_nh_old : rc->rc_nh_new;
	if (nh == NULL || (ifp = nh->nh_ifp) == NULL)
		return;

	memset(&id, 0, sizeof(id));
	kev_net_fill(ifp, &id.link_data);
	if ((nh->nh_flags & NHF_GATEWAY) != 0 &&
	    nh->gw_sa.sa_family == AF_INET6) {
		id.ia_addr = nh->gw6_sa;
		kev_net_unembed(&id.ia_addr);
	}
	kev_net_post(ifp, KEV_INET6_SUBCLASS, KEV_INET6_DEFROUTER, &id,
	    sizeof(id));
}

static void
kev_net_defrouter(struct rib_head *rnh __unused, struct rib_cmd_info *rc,
    void *arg __unused)
{
	struct in6_addr dst;
	uint32_t scopeid;
	int plen;

	rt_get_inet6_prefix_plen(rc->rc_rt, &dst, &plen, &scopeid);
	if (plen != 0)
		return;
#ifdef ROUTE_MPATH
	if ((rc->rc_cmd != RTM_DELETE && NH_IS_NHGRP(rc->rc_nh_new)) ||
	    (rc->rc_cmd != RTM_ADD && NH_IS_NHGRP(rc->rc_nh_old))) {
		rib_decompose_notification(rc, kev_net_defrouter_nh, NULL);
		return;
	}
#endif
	kev_net_defrouter_nh(rc, NULL);
}

/*
 * Subscribe to every inet6 FIB of each vnet once its tables exist. FIBs
 * added later through net.fibs are not covered. The subscriptions are
 * freed with the tables when the vnet goes away.
 */
static void
kev_net_defrouter_init(const void *unused __unused)
{
	struct rib_head *rnh;

	for (uint32_t fib = 0; fib < V_rt_numfibs; fib++) {
		rnh = rt_tables_get_rnh_safe(fib, AF_INET6);
		if (rnh != NULL)
			(void)rib_subscribe_internal(rnh, kev_net_defrouter,
			    NULL, RIB_NOTIFY_DELAYED, true);
	}
}
VNET_SYSINIT(kev_net_defrouter_init, SI_SUB_PROTO_DOMAIN, SI_ORDER_ANY,
    kev_net_defrouter_init, NULL);
#endif /* INET6 */
