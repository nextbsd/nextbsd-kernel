
#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/types.h>
#include <sys/malloc.h>
#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/systm.h>
#include <sys/signal.h>
#include <sys/proc.h>

#include <sys/mach/mach_types.h>
#include <sys/mach/host_priv_server.h>
#include <sys/mach/host.h>		/* realhost, host_lock */
#include <sys/mach/host_special_ports.h>
#include <sys/mach/ipc/ipc_port.h>


int
get_dp_control_port(
	host_priv_t host,
	mach_port_t *contorl_port
)
UNSUPPORTED;

int
host_default_memory_manager(
	host_priv_t host_priv,
	memory_object_default_t *default_manager,
	memory_object_cluster_size_t cluster_size
)
UNSUPPORTED;

int
host_get_UNDServer(
	host_priv_t host,
	UNDServerRef *server
)
UNSUPPORTED;

int
host_get_boot_info(
	host_priv_t host_priv,
	kernel_boot_info_t boot_info
)
UNSUPPORTED;

int
host_get_clock_control(
	host_priv_t host_priv,
	clock_id_t clock_id,
	clock_ctrl_t *clock_ctrl
)
UNSUPPORTED;

/*
 *	Routine:	host_get_special_port [kernel call]
 *	Purpose:
 *		Return a send right for one of the host's special
 *		ports (nextbsd-userland#93 group 5; Apple's
 *		host_get_host_port() / host_get_host_priv_port()
 *		macros are this with HOST_PORT / HOST_PRIV_PORT).
 *		This tree does not keep the kernel ports in
 *		realhost.special[] as XNU does, so they are taken
 *		from realhost directly:
 *
 *		HOST_PORT		the unprivileged host port
 *		HOST_PRIV_PORT		the host_priv port (the caller
 *					already holds it)
 *		HOST_BOOTSTRAP_PORT,	whatever userland registered
 *		HOST_KEXTD_PORT		with host_set_special_port();
 *					MACH_PORT_NULL if unset
 *
 *		Any other slot is KERN_INVALID_ARGUMENT. `node` is
 *		ignored, as in XNU: there is only the local node.
 *	Conditions:
 *		Nothing locked.
 *	Returns:
 *		KERN_SUCCESS		*port holds a naked send right
 *					(possibly IP_NULL).
 *		KERN_INVALID_HOST	host_priv is not the host_priv port.
 *		KERN_INVALID_ARGUMENT	Unsupported slot.
 */
kern_return_t
host_get_special_port(
	host_priv_t host_priv,
	int node __unused,
	int which,
	mach_port_t *port
)
{

	if (host_priv == HOST_NULL)
		return (KERN_INVALID_HOST);

	switch (which) {
	case HOST_PORT:
		*port = ipc_port_make_send(realhost.host_self);
		break;
	case HOST_PRIV_PORT:
		*port = ipc_port_make_send(realhost.host_priv_self);
		break;
	default:
		if (!host_special_port_settable(which))
			return (KERN_INVALID_ARGUMENT);
		host_lock(&realhost);
		*port = ipc_port_copy_send(realhost.special[which]);
		host_unlock(&realhost);
		break;
	}
	return (KERN_SUCCESS);
}

int
host_priv_statistics(
	host_priv_t host_priv,
	host_flavor_t flavor,
	host_info_t host_info_out,
	mach_msg_type_number_t *host_info_outCnt
)
UNSUPPORTED;

int
host_processor_set_priv(
	host_priv_t host_priv,
	processor_set_name_t set_name,
	processor_set_t *set
)
UNSUPPORTED;

int
host_processor_sets(
	host_priv_t host_priv,
	processor_set_name_array_t *processor_sets,
	mach_msg_type_number_t *processor_setsCnt
)
UNSUPPORTED;

int
host_processors(
	host_priv_t host_priv,
	processor_array_t *out_processor_list,
	mach_msg_type_number_t *out_processor_listCnt
)
UNSUPPORTED;

int
host_reboot(
	host_priv_t host_priv,
	int options
)
UNSUPPORTED;

int
host_set_UNDServer(
	host_priv_t host,
	UNDServerRef server
)

UNSUPPORTED;

/*
 * host_special_port_settable — the host special-port slots userland may
 * set (nextbsd-userland#84). HOST_BOOTSTRAP_PORT is the bootstrap server
 * and HOST_KEXTD_PORT is kextd's load-request receive right (K3b #216).
 * The kernel-provided slots (HOST_PORT, HOST_PRIV_PORT, ...) and the
 * other Apple-defined slots are refused. Shared by host_get_special_port
 * above and the host_set_special_port MIG handler below.
 */
boolean_t
host_special_port_settable(int which)
{

	return (which == HOST_BOOTSTRAP_PORT || which == HOST_KEXTD_PORT);
}

/*
 *	Routine:	host_set_special_port [kernel call]
 *	Purpose:
 *		Set one of the host's userland-settable special ports
 *		(nextbsd-userland#84, Phase B2). Reached over MIG
 *		(host_priv 413), which needs a host_priv request port:
 *		mach_host_self() hands that to unjailed euid-0 tasks
 *		(kern/ipc_host.c). The old host_set_special_port
 *		trap is gone (#84 Phase C); MIG is the only path.
 *	Conditions:
 *		Nothing locked. If successful, consumes the supplied
 *		send right (which may be IP_NULL to clear the slot);
 *		on failure the caller still owns it (MIG destroys the
 *		request message).
 *	Returns:
 *		KERN_SUCCESS		Changed the special port.
 *		KERN_INVALID_HOST	host_priv is not the host_priv port.
 *		KERN_INVALID_ARGUMENT	Not a userland-settable slot.
 */
kern_return_t
host_set_special_port(
	host_priv_t host_priv,
	int which,
	mach_port_t port
)
{
	ipc_port_t old;

	if (host_priv == HOST_NULL)
		return (KERN_INVALID_HOST);
	if (!host_special_port_settable(which))
		return (KERN_INVALID_ARGUMENT);

	host_lock(&realhost);
	old = realhost.special[which];
	realhost.special[which] = port;
	host_unlock(&realhost);

	if (IP_VALID(old))
		ipc_port_release_send(old);
	return (KERN_SUCCESS);
}

int
kext_request(
	host_priv_t host_priv,
	uint32_t user_log_flags,
	vm_offset_t request_data,
	mach_msg_type_number_t request_dataCnt,
	vm_offset_t *response_data,
	mach_msg_type_number_t *response_dataCnt,
	vm_offset_t *log_data,
	mach_msg_type_number_t *log_dataCnt,
	kern_return_t *op_result
)
UNSUPPORTED;

int
set_dp_control_port(
	host_priv_t host,
	mach_port_t control_port
)
UNSUPPORTED;

int
thread_wire(
	host_priv_t host_priv,
	thread_act_t thread,
	boolean_t wired
)
UNSUPPORTED;
