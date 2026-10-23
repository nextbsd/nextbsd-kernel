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
 * The Darwin system socket domain (PF_SYSTEM) and its kernel event protocol
 * (SYSPROTO_EVENT), nextbsd-kernel#293.
 *
 * Clean-room: only the interface facts (constants, struct layouts, ioctl
 * numbers and the filter rule) are taken from Darwin; the messages and ioctls
 * are in sys/kern_event.h.
 *
 * kev_post_msg() may be called from any context that can take a default
 * mutex, including inside the network epoch, with curvnet set to the vnet
 * the event belongs to. It copies the message at once, stamps its id and
 * queues it; one taskqueue thread then delivers the queue in order to every
 * socket whose vnet and filter match. A socket whose
 * receive buffer is full loses the event, which is counted, as on Darwin.
 *
 * Statistics are under net.systm.kevt. The file must not be called
 * kern/kern_event*.c: the kernel build appends to the base kern_event.c.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/mutex.h>
#include <sys/protosw.h>
#include <sys/domain.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/sysctl.h>
#include <sys/taskqueue.h>
#include <sys/sys_domain.h>
#include <sys/kern_event.h>

#include <net/vnet.h>

/*
 * sys_domain.h only takes 39 when socket.h has no AF_SYSTEM of its own;
 * make sure that is still the unused vendor slot.
 */
CTASSERT(AF_SYSTEM == AF_VENDOR00);
CTASSERT(AF_SYSTEM <= AF_MAX);

static MALLOC_DEFINE(M_KEV, "kev", "PF_SYSTEM kernel event messages");

/* Filter vendor of a new socket: matches no event until SIOCSKEVFILT. */
#define	KEV_FILTER_NONE		0xffffffffU

/* Dynamic vendor codes (SIOCGKEVVENDOR) start here, as on Darwin. */
#define	KEV_VENDOR_FIRST_DYNAMIC	1000
#define	KEV_VENDOR_MAX			64

/* Messages queued for delivery beyond this are dropped and counted. */
#define	KEV_MAXPENDING		1024

/* Same fields as Darwin's struct kevtstat (net.systm.kevt.stats). */
struct kevtstat {
	uint64_t	kes_pcbcount;	/* sockets open */
	uint64_t	kes_gencnt;	/* socket list generation */
	uint64_t	kes_badvendor;	/* posts with no vendor */
	uint64_t	kes_toobig;	/* posts larger than MLEN */
	uint64_t	kes_nomem;	/* posts or copies without memory */
	uint64_t	kes_fullsock;	/* copies dropped on a full socket */
	uint64_t	kes_detached;	/* not used */
};

struct kev_pcb {
	LIST_ENTRY(kev_pcb)	kp_link;
	struct socket		*kp_socket;
	struct kev_request	kp_filter;
};

struct kev_pending {
	STAILQ_ENTRY(kev_pending) kq_link;
	struct vnet		*kq_vnet;	/* compared only, never used */
	u_int32_t		kq_len;
	struct kern_event_msg	kq_msg;		/* kq_len bytes */
};

/*
 * kev_mtx protects the socket list, every filter and the vendor table; it
 * is taken before a socket's receive buffer lock. kev_qmtx protects the
 * pending queue and is a leaf.
 */
static struct mtx kev_mtx;
MTX_SYSINIT(kev_mtx, &kev_mtx, "kev sockets", MTX_DEF);
static struct mtx kev_qmtx;
MTX_SYSINIT(kev_qmtx, &kev_qmtx, "kev queue", MTX_DEF);

static LIST_HEAD(, kev_pcb) kev_pcbs = LIST_HEAD_INITIALIZER(kev_pcbs);
static STAILQ_HEAD(, kev_pending) kev_queue =
    STAILQ_HEAD_INITIALIZER(kev_queue);
static u_int kev_npending;
static u_int kev_pcbcount;
static u_int32_t kev_id;

static struct {
	u_int32_t	code;
	char		string[KEV_VENDOR_CODE_MAX_STR_LEN];
} kev_vendors[KEV_VENDOR_MAX];
static u_int kev_nvendors;

static struct kevtstat kevtstat;

static SYSCTL_NODE(_net, OID_AUTO, systm, CTLFLAG_RW | CTLFLAG_MPSAFE, 0,
    "PF_SYSTEM");
static SYSCTL_NODE(_net_systm, OID_AUTO, kevt, CTLFLAG_RW | CTLFLAG_MPSAFE, 0,
    "Kernel event sockets");
SYSCTL_OPAQUE(_net_systm_kevt, OID_AUTO, stats, CTLFLAG_RD, &kevtstat,
    sizeof(kevtstat), "S,kevtstat", "Kernel event socket statistics");
SYSCTL_UINT(_net_systm_kevt, OID_AUTO, pcbcount, CTLFLAG_RD, &kev_pcbcount,
    0, "Kernel event sockets open");

static void	kev_deliver(void *, int);

TASKQUEUE_DEFINE_THREAD(kev);
static struct task kev_task = TASK_INITIALIZER(0, kev_deliver, NULL);

static bool
kev_match(const struct kev_request *f, const struct kern_event_msg *ev)
{

	if (f->vendor_code == KEV_FILTER_NONE)
		return (false);
	if (f->vendor_code == KEV_ANY_VENDOR)
		return (true);
	if (f->vendor_code != ev->vendor_code)
		return (false);
	if (f->kev_class == KEV_ANY_CLASS)
		return (true);
	if (f->kev_class != ev->kev_class)
		return (false);
	return (f->kev_subclass == KEV_ANY_SUBCLASS ||
	    f->kev_subclass == ev->kev_subclass);
}

int
kev_post_msg(struct kev_msg *event)
{
	struct kev_pending *kq;
	char *p;
	u_int32_t len;
	int i;

	if (event->vendor_code == KEV_ANY_VENDOR) {
		atomic_add_64(&kevtstat.kes_badvendor, 1);
		return (EINVAL);
	}
	len = KEV_MSG_HEADER_SIZE;
	for (i = 0; i < N_KEV_VECTORS && event->dv[i].data_length != 0; i++)
		len += event->dv[i].data_length;
	if (len > (u_int32_t)MLEN) {
		atomic_add_64(&kevtstat.kes_toobig, 1);
		return (EMSGSIZE);
	}

	/* Nobody listening: only the id moves on. */
	if (atomic_load_int(&kev_pcbcount) == 0) {
		atomic_add_32(&kev_id, 1);
		return (0);
	}

	kq = malloc(__offsetof(struct kev_pending, kq_msg) + len, M_KEV,
	    M_NOWAIT);
	if (kq == NULL) {
		atomic_add_64(&kevtstat.kes_nomem, 1);
		return (ENOMEM);
	}
	kq->kq_vnet = curvnet;
	kq->kq_len = len;
	kq->kq_msg.total_size = len;
	kq->kq_msg.vendor_code = event->vendor_code;
	kq->kq_msg.kev_class = event->kev_class;
	kq->kq_msg.kev_subclass = event->kev_subclass;
	kq->kq_msg.event_code = event->event_code;
	p = (char *)&kq->kq_msg + KEV_MSG_HEADER_SIZE;
	for (i = 0; i < N_KEV_VECTORS && event->dv[i].data_length != 0; i++) {
		memcpy(p, event->dv[i].data_ptr, event->dv[i].data_length);
		p += event->dv[i].data_length;
	}

	/* Ids are taken under the queue lock so they rise in delivery order. */
	mtx_lock(&kev_qmtx);
	if (kev_npending >= KEV_MAXPENDING) {
		mtx_unlock(&kev_qmtx);
		free(kq, M_KEV);
		atomic_add_64(&kevtstat.kes_nomem, 1);
		return (ENOBUFS);
	}
	kq->kq_msg.id = atomic_fetchadd_32(&kev_id, 1) + 1;
	STAILQ_INSERT_TAIL(&kev_queue, kq, kq_link);
	kev_npending++;
	mtx_unlock(&kev_qmtx);
	taskqueue_enqueue(taskqueue_kev, &kev_task);
	return (0);
}

static void
kev_deliver_one(struct kev_pending *kq)
{
	struct kev_pcb *kp;
	struct socket *so;
	struct mbuf *m, *n;

	m = m_get2(kq->kq_len, M_WAITOK, MT_DATA, M_PKTHDR);
	memcpy(mtod(m, void *), &kq->kq_msg, kq->kq_len);
	m->m_len = m->m_pkthdr.len = kq->kq_len;

	mtx_lock(&kev_mtx);
	LIST_FOREACH(kp, &kev_pcbs, kp_link) {
		so = kp->kp_socket;
		if (so->so_vnet != kq->kq_vnet ||
		    !kev_match(&kp->kp_filter, &kq->kq_msg))
			continue;
		SOCK_RECVBUF_LOCK(so);
		if ((so->so_rcv.sb_state & SBS_CANTRCVMORE) != 0) {
			SOCK_RECVBUF_UNLOCK(so);
			continue;
		}
		if (sbspace(&so->so_rcv) < (long)kq->kq_len) {
			SOCK_RECVBUF_UNLOCK(so);
			atomic_add_64(&kevtstat.kes_fullsock, 1);
			continue;
		}
		n = m_copym(m, 0, M_COPYALL, M_NOWAIT);
		if (n == NULL) {
			SOCK_RECVBUF_UNLOCK(so);
			atomic_add_64(&kevtstat.kes_nomem, 1);
			continue;
		}
		sbappendrecord_locked(&so->so_rcv, n);
		sorwakeup_locked(so);
	}
	mtx_unlock(&kev_mtx);
	m_freem(m);
}

static void
kev_deliver(void *arg __unused, int pending __unused)
{
	struct kev_pending *kq;

	for (;;) {
		mtx_lock(&kev_qmtx);
		kq = STAILQ_FIRST(&kev_queue);
		if (kq != NULL) {
			STAILQ_REMOVE_HEAD(&kev_queue, kq_link);
			kev_npending--;
		}
		mtx_unlock(&kev_qmtx);
		if (kq == NULL)
			break;
		kev_deliver_one(kq);
		free(kq, M_KEV);
	}
}

/*
 * SIOCGKEVVENDOR: "com.apple" is KEV_VENDOR_APPLE; any other string gets
 * the next dynamic code the first time it is seen.
 */
static int
kev_vendor_code_find(const char *string, u_int32_t *code)
{
	u_int i;

	if (string[0] == '\0')
		return (EINVAL);
	if (strcmp(string, "com.apple") == 0) {
		*code = KEV_VENDOR_APPLE;
		return (0);
	}
	mtx_lock(&kev_mtx);
	for (i = 0; i < kev_nvendors; i++) {
		if (strcmp(kev_vendors[i].string, string) == 0) {
			*code = kev_vendors[i].code;
			mtx_unlock(&kev_mtx);
			return (0);
		}
	}
	if (kev_nvendors == KEV_VENDOR_MAX) {
		mtx_unlock(&kev_mtx);
		return (ENOMEM);
	}
	kev_vendors[i].code = KEV_VENDOR_FIRST_DYNAMIC + i;
	strlcpy(kev_vendors[i].string, string,
	    sizeof(kev_vendors[i].string));
	kev_nvendors++;
	*code = kev_vendors[i].code;
	mtx_unlock(&kev_mtx);
	return (0);
}

static int
kev_control(struct socket *so, u_long cmd, void *data, struct ifnet *ifp,
    struct thread *td)
{
	struct kev_pcb *kp = so->so_pcb;
	struct kev_vendor_code *vc;

	switch (cmd) {
	case SIOCGKEVID:
		*(u_int32_t *)data = atomic_load_32(&kev_id);
		return (0);
	case SIOCSKEVFILT:
		mtx_lock(&kev_mtx);
		kp->kp_filter = *(struct kev_request *)data;
		mtx_unlock(&kev_mtx);
		return (0);
	case SIOCGKEVFILT:
		mtx_lock(&kev_mtx);
		*(struct kev_request *)data = kp->kp_filter;
		mtx_unlock(&kev_mtx);
		return (0);
	case SIOCGKEVVENDOR:
		vc = data;
		vc->vendor_string[sizeof(vc->vendor_string) - 1] = '\0';
		return (kev_vendor_code_find(vc->vendor_string,
		    &vc->vendor_code));
	default:
		return (ENOTSUP);
	}
}

static int
kev_attach(struct socket *so, int proto, struct thread *td)
{
	struct kev_pcb *kp;
	int error;

	error = soreserve(so, KEV_SNDSPACE, KEV_RECVSPACE);
	if (error != 0)
		return (error);

	kp = malloc(sizeof(*kp), M_PCB, M_WAITOK | M_ZERO);
	kp->kp_socket = so;
	kp->kp_filter.vendor_code = KEV_FILTER_NONE;
	so->so_pcb = kp;

	mtx_lock(&kev_mtx);
	LIST_INSERT_HEAD(&kev_pcbs, kp, kp_link);
	atomic_add_int(&kev_pcbcount, 1);
	kevtstat.kes_pcbcount = kev_pcbcount;
	kevtstat.kes_gencnt++;
	mtx_unlock(&kev_mtx);
	return (0);
}

static void
kev_detach(struct socket *so)
{
	struct kev_pcb *kp = so->so_pcb;

	mtx_lock(&kev_mtx);
	LIST_REMOVE(kp, kp_link);
	atomic_subtract_int(&kev_pcbcount, 1);
	kevtstat.kes_pcbcount = kev_pcbcount;
	kevtstat.kes_gencnt++;
	mtx_unlock(&kev_mtx);
	so->so_pcb = NULL;
	free(kp, M_PCB);
}

static struct protosw kev_protosw = {
	.pr_type =		SOCK_RAW,
	.pr_protocol =		SYSPROTO_EVENT,
	.pr_flags =		PR_ATOMIC,
	.pr_attach =		kev_attach,
	.pr_detach =		kev_detach,
	.pr_control =		kev_control,
};

static struct domain systemdomain = {
	.dom_family =		PF_SYSTEM,
	.dom_name =		"system",
	.dom_nprotosw =		1,
	.dom_protosw =		{ &kev_protosw },
};

DOMAIN_SET(system);
