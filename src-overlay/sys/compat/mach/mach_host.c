

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/types.h>
#include <sys/malloc.h>
#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/systm.h>
#include <sys/signal.h>
#include <sys/proc.h>
#include <sys/smp.h>		/* mp_ncpus */
#include <sys/counter.h>
#include <sys/vmmeter.h>	/* vm_cnt, vm_free_count(), vm_wire_count() */

#include <vm/vm.h>
#include <vm/vm_param.h>
#include <vm/vm_extern.h>	/* vm_active_count(), vm_inactive_count() */

#include <sys/mach/mach_types.h>


#include <sys/mach/host_special_ports.h>
#include <sys/mach/host.h>
#include <sys/mach/mach_host_server.h>

host_data_t	realhost;

int
host_create_mach_voucher(
	host_t host,
	mach_voucher_attr_raw_recipe_array_t recipes,
	mach_msg_type_number_t recipesCnt,
	ipc_voucher_t *voucher
)
UNSUPPORTED;

int
host_get_clock_service(
	host_t host,
	clock_id_t clock_id,
	clock_serv_t *clock_serv
)
UNSUPPORTED;

/*
 * host_info / host_statistics (nextbsd-userland#93, group 6).
 *
 * Flavor codes and layouts are Apple's (userland include/mach/host_info.h
 * for HOST_VM_INFO; sys/mach/host_info.h for the rest).  As in XNU, a
 * count too short for a known flavor is KERN_FAILURE; an unknown flavor
 * is KERN_INVALID_ARGUMENT.
 */
CTASSERT(sizeof(struct host_basic_info) == 48);
CTASSERT(HOST_BASIC_INFO_COUNT == 12);
CTASSERT(HOST_SCHED_INFO_COUNT == 2);
CTASSERT(HOST_PRIORITY_INFO_COUNT == 8);
CTASSERT(HOST_VM_INFO_COUNT == 15);

int
host_info(
	host_t host,
	host_flavor_t flavor,
	host_info_t host_info_out,
	mach_msg_type_number_t *host_info_outCnt
)
{
	if (host == HOST_NULL || host_info_out == NULL ||
	    host_info_outCnt == NULL)
		return (KERN_INVALID_ARGUMENT);

	switch (flavor) {
	case HOST_BASIC_INFO: {
		host_basic_info_t bi = (host_basic_info_t)host_info_out;
		uint64_t mem = (uint64_t)ptoa((uint64_t)physmem);

		if (*host_info_outCnt < HOST_BASIC_INFO_OLD_COUNT)
			return (KERN_FAILURE);

		bi->max_cpus = mp_ncpus;
		bi->avail_cpus = mp_ncpus;
		bi->memory_size = (natural_t)MIN(mem, (uint64_t)0x80000000);
#if defined(__amd64__)
		bi->cpu_type = CPU_TYPE_X86_64;
		bi->cpu_subtype = CPU_SUBTYPE_X86_64_ALL;
#elif defined(__aarch64__)
		bi->cpu_type = CPU_TYPE_ARM64;
		bi->cpu_subtype = CPU_SUBTYPE_ARM64_ALL;
#else
		bi->cpu_type = 0;
		bi->cpu_subtype = 0;
#endif
		if (*host_info_outCnt < HOST_BASIC_INFO_COUNT) {
			*host_info_outCnt = HOST_BASIC_INFO_OLD_COUNT;
			break;
		}
		bi->cpu_threadtype = 0;		/* CPU_THREADTYPE_NONE */
		bi->physical_cpu = mp_ncpus;
		bi->physical_cpu_max = mp_ncpus;
		bi->logical_cpu = mp_ncpus;
		bi->logical_cpu_max = mp_ncpus;
		bi->max_mem = mem;
		*host_info_outCnt = HOST_BASIC_INFO_COUNT;
		break;
	}

	case HOST_SCHED_INFO: {
		host_sched_info_t si = (host_sched_info_t)host_info_out;
		integer_t tick_ms = (hz > 0 && hz <= 1000) ? 1000 / hz : 1;

		if (*host_info_outCnt < HOST_SCHED_INFO_COUNT)
			return (KERN_FAILURE);
		/* Both in milliseconds; FreeBSD's granularity is one tick. */
		si->min_timeout = tick_ms;
		si->min_quantum = tick_ms;
		*host_info_outCnt = HOST_SCHED_INFO_COUNT;
		break;
	}

	case HOST_PRIORITY_INFO: {
		host_priority_info_t pi = (host_priority_info_t)host_info_out;

		if (*host_info_outCnt < HOST_PRIORITY_INFO_COUNT)
			return (KERN_FAILURE);
		/*
		 * Mach priority space (0..127, higher is more urgent), the
		 * values XNU reports.  Callers interpret these as Mach
		 * priorities, not FreeBSD ones.
		 */
		pi->kernel_priority = 80;	/* MINPRI_KERNEL */
		pi->system_priority = 80;	/* MINPRI_KERNEL */
		pi->server_priority = 64;	/* MINPRI_RESERVED */
		pi->user_priority = 31;		/* BASEPRI_DEFAULT */
		pi->depress_priority = 0;	/* DEPRESSPRI */
		pi->idle_priority = 0;		/* IDLEPRI */
		pi->minimum_priority = 0;	/* MINPRI_USER */
		pi->maximum_priority = 79;	/* MAXPRI_RESERVED */
		*host_info_outCnt = HOST_PRIORITY_INFO_COUNT;
		break;
	}

	default:
		return (KERN_INVALID_ARGUMENT);
	}

	return (KERN_SUCCESS);
}

int
host_kernel_version(
	host_t host,
	kernel_version_t kernel_version
)
UNSUPPORTED;

int
host_page_size(
	host_t host,
	vm_size_t *out_page_size
)
UNSUPPORTED;

int
host_processor_info(
	host_t host,
	processor_flavor_t flavor,
	natural_t *out_processor_count,
	processor_info_array_t *out_processor_info,
	mach_msg_type_number_t *out_processor_infoCnt
)
UNSUPPORTED;

int
host_register_mach_voucher_attr_manager(
	host_t host,
	mach_voucher_attr_manager_t attr_manager,
	mach_voucher_attr_value_handle_t default_value,
	mach_voucher_attr_key_t *new_key,
	ipc_voucher_attr_control_t *new_attr_control
)
UNSUPPORTED;

int
host_register_well_known_mach_voucher_attr_manager(
	host_t host,
	mach_voucher_attr_manager_t attr_manager,
	mach_voucher_attr_value_handle_t default_value,
	mach_voucher_attr_key_t key,
	ipc_voucher_attr_control_t *new_attr_control
)
UNSUPPORTED;

int
host_request_notification(
	host_t host,
	host_flavor_t notify_type,
	mach_port_t notify_port
)
UNSUPPORTED;

int
host_statistics(
	host_t host_priv,
	host_flavor_t flavor,
	host_info_t host_info_out,
	mach_msg_type_number_t *host_info_outCnt
)
{
	if (host_priv == HOST_NULL || host_info_out == NULL ||
	    host_info_outCnt == NULL)
		return (KERN_INVALID_ARGUMENT);

	switch (flavor) {
	case HOST_VM_INFO: {
		vm_statistics_t vs = (vm_statistics_t)host_info_out;
		mach_msg_type_number_t cnt = *host_info_outCnt;

		if (cnt < HOST_VM_INFO_REV0_COUNT)
			return (KERN_FAILURE);

		/* Page counts and event counters, truncated to 32 bits. */
		vs->free_count = (integer_t)vm_free_count();
		vs->active_count = (integer_t)vm_active_count();
		vs->inactive_count = (integer_t)vm_inactive_count();
		vs->wire_count = (integer_t)vm_wire_count();
		vs->zero_fill_count = (integer_t)VM_CNT_FETCH(v_zfod);
		vs->reactivations = (integer_t)VM_CNT_FETCH(v_reactivated);
		vs->pageins = (integer_t)(VM_CNT_FETCH(v_vnodepgsin) +
		    VM_CNT_FETCH(v_swappgsin));
		vs->pageouts = (integer_t)(VM_CNT_FETCH(v_vnodepgsout) +
		    VM_CNT_FETCH(v_swappgsout));
		vs->faults = (integer_t)VM_CNT_FETCH(v_vm_faults);
		vs->cow_faults = (integer_t)VM_CNT_FETCH(v_cow_faults);
		vs->lookups = 0;	/* no Mach object cache */
		vs->hits = 0;
		*host_info_outCnt = HOST_VM_INFO_REV0_COUNT;

		if (cnt < HOST_VM_INFO_REV1_COUNT)
			break;
		vs->purgeable_count = 0;	/* no purgeable memory */
		vs->purges = 0;
		*host_info_outCnt = HOST_VM_INFO_REV1_COUNT;

		if (cnt < HOST_VM_INFO_REV2_COUNT)
			break;
		vs->speculative_count = 0;
		*host_info_outCnt = HOST_VM_INFO_REV2_COUNT;
		break;
	}

	default:
		return (KERN_INVALID_ARGUMENT);
	}

	return (KERN_SUCCESS);
}

int
host_statistics64(
	host_t host_priv,
	host_flavor_t flavor,
	host_info64_t host_info64_out,
	mach_msg_type_number_t *host_info64_outCnt
)
UNSUPPORTED;

int
host_virtual_physical_table_info(
	host_t host,
	hash_info_bucket_array_t *info,
	mach_msg_type_number_t *infoCnt
)
UNSUPPORTED;

int
mach_memory_object_memory_entry(
	host_t host,
	boolean_t internal,
	vm_size_t size,
	vm_prot_t permission,
	memory_object_t pager,
	mach_port_t *entry_handle
)
UNSUPPORTED;

int
mach_memory_object_memory_entry_64(
	host_t host,
	boolean_t internal,
	memory_object_size_t size,
	vm_prot_t permission,
	memory_object_t pager,
	mach_port_t *entry_handle
)
UNSUPPORTED;

int
mach_zone_info(
	host_priv_t host,
	mach_zone_name_array_t *names,
	mach_msg_type_number_t *namesCnt,
	mach_zone_info_array_t *info,
	mach_msg_type_number_t *infoCnt
)
UNSUPPORTED;

int
processor_set_create(
	host_t host,
	processor_set_t *new_set,
	processor_set_name_t *new_name
)
UNSUPPORTED;


