/*-
 * Copyright (c) 2014-2015, Matthew Macy <mmacy@nextbsd.org>
 * Copyright (c) 2021-2022 Zoe Knox <zoe@pixin.net>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 *  2. Neither the name of Matthew Macy nor the names of its
 *     contributors may be used to endorse or promote products derived from
 *     this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */


#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/types.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/mount.h>
#include <sys/proc.h>
#include <sys/mman.h>
#include <sys/malloc.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/exec.h>
#include <sys/sysproto.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#include <sys/ktrace.h>

#include <vm/vm.h>
#include <vm/vm_param.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <vm/vm_extern.h>
#include <vm/vm_kern.h>

#include <sys/mach/mach_types.h>
#include <sys/mach/mach_vm.h>
#include <sys/mach/ipc/ipc_kmsg.h>
#include <sys/mach/thread.h>
#include <sys/mach/mach_vm_server.h>
#include <sys/mach/vm_map_server.h>
#include <sys/mach/host_priv_server.h>


extern vm_size_t	msg_ool_size_small;

int mach_vm_map_page_query(vm_map_t target_map, vm_offset_t offset, integer_t *disposition, integer_t *ref_count);
int mach_vm_mapped_pages_info(vm_map_t task, page_address_array_t *pages, mach_msg_type_number_t *pagesCnt);
int mach_vm_wire_32(
	host_priv_t host_priv,
	vm_map_t task,
	vm_address_t address,
	vm_size_t size,
	vm_prot_t desired_access
	);


#define EXPERIMENTAL_MACH_ENFORCE_PROT 1
int
mach_vm_map(vm_map_t map, mach_vm_address_t *address, mach_vm_size_t _size,
			mach_vm_offset_t _mask, int _flags, mem_entry_name_port_t object __unused,
			memory_object_offset_t offset __unused, boolean_t copy __unused,
			vm_prot_t cur_protection, vm_prot_t max_protection, vm_inherit_t inh)
{
	vm_offset_t addr = 0;
	size_t size;
	int docow, error, find_space;

	/* XXX Darwin fails on mapping a page at address 0 */
	if ((_flags & VM_FLAGS_ANYWHERE) == 0 && *address == 0)
		return (ENOMEM);

	size = round_page(_size);
	docow = error = 0;

	if (!(_mask & (_mask + 1)) && _mask != 0)
		_mask++;

	find_space = _mask ? VMFS_ALIGNED_SPACE(ffs(_mask)) : VMFS_ANY_SPACE;
#if EXPERIMENTAL_MACH_ENFORCE_PROT
	int flags = MAP_ANON;
#endif
	if ((_flags & VM_FLAGS_ANYWHERE) == 0) {
#if EXPERIMENTAL_MACH_ENFORCE_PROT
		flags |= MAP_FIXED;
#endif
		addr = trunc_page(*address);
	} else
		addr = 0;

	switch(inh) {
	case VM_INHERIT_SHARE:
#if EXPERIMENTAL_MACH_ENFORCE_PROT
		flags |= MAP_INHERIT_SHARE;
#endif
		break;
	case VM_INHERIT_COPY:
#if EXPERIMENTAL_MACH_ENFORCE_PROT
		flags |= MAP_COPY_ON_WRITE;
#endif
		docow = MAP_COPY_ON_WRITE;
		break;
	case VM_INHERIT_NONE:
		break;
	case VM_INHERIT_DONATE_COPY:
	default:
		uprintf("mach_vm_map: unsupported inheritance flag %d\n", inh);
		break;
	}

#if EXPERIMENTAL_MACH_ENFORCE_PROT
        vm_map_lock(map);
        map->flags = flags;
        vm_map_unlock(map);
#endif

	if (vm_map_find(map, NULL, 0, &addr, size, 0, find_space,
	    cur_protection, max_protection, docow) != KERN_SUCCESS) {
		error = ENOMEM;
		goto done;
	}

	*address = addr;

#if EXPERIMENTAL_MACH_ENFORCE_PROT
#if 0
        /* The caller may optionally free the allocated range if either of
         * these errors occurs. The memory is valid but may not behave as
         * desired. Caveat emptor.
         */
        if(vm_map_protect(map, addr, addr + size, cur_protection, max_protection,
            VM_MAP_PROTECT_SET_PROT|VM_MAP_PROTECT_SET_MAXPROT) != KERN_SUCCESS) {
                /* Memory was alloc'd but not protected properly */
		error = EACCES;
        }
#endif // 0

        if(vm_map_inherit(map, addr, addr + size, inh) != KERN_SUCCESS) {
            /* Memory was alloc'd but set inheritance failed */
            error = EACCES;
        }
#endif

done:
	return (error);
}

int
vm_allocate(vm_map_t map, vm_offset_t *addr, size_t _size, int flags)
{

	return (mach_vm_allocate(map, addr, _size, flags));
}

int
mach_vm_allocate(vm_map_t map, vm_offset_t *addr, size_t _size, int flags)
{
	size_t size = round_page(_size);
	vm_offset_t start, daddr;
	vm_prot_t prot, protmax;
	int err;

	prot = VM_PROT_READ|VM_PROT_WRITE;
	protmax = VM_PROT_ALL;
	KASSERT(addr != NULL, ("invalid addr passed"));
#if defined(INVARIANTS) && defined(__LP64__)
	start = 1UL<<32;
#else
	start = 0;
#endif

	daddr = trunc_page(*addr);
	vm_map_lock(map);
	if ((flags & VM_FLAGS_ANYWHERE) &&
		((daddr = vm_map_findspace(map, start, size)) == (vm_map_max(map)-size+1) )) {
	  err = ENOMEM;
	  goto error;
	} else {
		err = EINVAL;
		/* Address range must be all in user VM space. */
		if (daddr < vm_map_min(map) ||
		    daddr + size > vm_map_max(map))
		  goto error;
		if (daddr + size < daddr)
		  goto error;
	}
	if (vm_map_insert(map, NULL, 0, daddr, daddr + size, prot, protmax, 0)) {
	  err = EFAULT;
	  goto error;
	}
	vm_map_unlock(map);
	*addr = daddr;
	return (0);
 error:
	vm_map_unlock(map);
	return (err);
}

int
vm_deallocate(vm_map_t map, vm_offset_t addr, size_t len)
{

	return (mach_vm_deallocate(map, addr, len));
}

int
mach_vm_deallocate(vm_map_t target __unused, mach_vm_address_t addr, mach_vm_size_t len)
{
	struct munmap_args cup;

	cup.addr = (void *)addr;
	cup.len = len;
	return (sys_munmap(curthread, &cup));
}

int
mach_vm_protect(vm_map_t target_task __unused, vm_offset_t addr, size_t len,
				boolean_t setmax __unused, vm_prot_t prot)
{
	struct mprotect_args cup;

	cup.addr = (void *)addr;
	cup.len = len;
	cup.prot = prot;

	return (sys_mprotect(curthread, &cup));
}


int
mach_vm_inherit(vm_map_t target_task, mach_vm_address_t address, mach_vm_size_t size,
				vm_inherit_t new_inheritance)
{
	struct minherit_args cup;
	int error;

	cup.addr = (void *)address;
	cup.len = size;
	cup.inherit = new_inheritance;

	if ((error = sys_minherit(curthread, &cup)) != 0)
		return (KERN_FAILURE);

	return (0);
}

int
mach_vm_map_page_query(
	vm_map_t target_map,
	vm_offset_t offset,
	integer_t *disposition,
	integer_t *ref_count
)
{
	return (KERN_NOT_SUPPORTED);
}

int
_mach_make_memory_entry(
	vm_map_t target_task,
	memory_object_size_t *size,
	memory_object_offset_t offset,
	vm_prot_t permission,
	mem_entry_name_port_t *object_handle,
	mem_entry_name_port_t parent_handle
)
{	

	return (KERN_NOT_SUPPORTED);
}

int
mach_make_memory_entry(vm_map_t target_task, vm_size_t *size,
					   vm_offset_t offset, vm_prot_t permission,
					   mem_entry_name_port_t *object_handle,
					   mem_entry_name_port_t parent_handle)
{	

	return (KERN_NOT_SUPPORTED);
}


int
mach_make_memory_entry_64(vm_map_t target_task, memory_object_size_t *size,
						  memory_object_offset_t offset, vm_prot_t permission,
						  mach_port_t *object_handle,
						  mem_entry_name_port_t parent_handle)
{	

	return (KERN_NOT_SUPPORTED);
}


int
mach_vm_msync(vm_map_t target_task, mach_vm_address_t addr, mach_vm_size_t size,
		vm_sync_t flags)
{
	struct msync_args cup;
	int error;

	/* convert_port_to_map() gives VM_MAP_NULL for a bad port (#300). */
	if (target_task == VM_MAP_NULL)
		return (KERN_INVALID_ARGUMENT);
	cup.addr = (void *)addr;
	cup.len = size;
	cup.flags = 0;
	if (flags & VM_SYNC_ASYNCHRONOUS)
		cup.flags |= MS_ASYNC;
	if (flags & VM_SYNC_SYNCHRONOUS)
		cup.flags |= MS_SYNC;
	if (flags & VM_SYNC_INVALIDATE)
		cup.flags |= MS_INVALIDATE;
	error = sys_msync(curthread, &cup);
	
	if (error)
		return (KERN_FAILURE);
	return (0);
}

/* XXX Do it for remote task */
int
mach_vm_copy(vm_map_t target_task, mach_vm_address_t src, mach_vm_size_t size,
		mach_vm_address_t dst)
		
{
	char *tmpbuf;
	int error;

	/* convert_port_to_map() gives VM_MAP_NULL for a bad port (#300). */
	if (target_task == VM_MAP_NULL)
		return (KERN_INVALID_ARGUMENT);
#ifdef DEBUG_MACH_VM
	printf("mach_vm_copy: src = 0x%08lx, size = 0x%08lx, addr = 0x%08lx\n",
	    (long)req->req_src, (long)req->req_size, (long)req->req_addr);
#endif
	if ((src & (PAGE_SIZE - 1)) ||
	    (dst & (PAGE_SIZE - 1)) ||
	    (size & (PAGE_SIZE - 1)))
		return (KERN_INVALID_ARGUMENT);


	tmpbuf = malloc(PAGE_SIZE, M_TEMP, M_WAITOK);

	/* Is there an easy way of dealing with that efficiently? */
	do {
		if ((error = copyin((void*)src, tmpbuf, PAGE_SIZE)) != 0)
			goto out;

		if ((error = copyout(tmpbuf, (void *)dst, PAGE_SIZE)) != 0)
			goto out;

		src += PAGE_SIZE;
		dst += PAGE_SIZE;
		size -= PAGE_SIZE;
	} while (size > 0);

	free(tmpbuf, M_TEMP);
	return (0);

out:
	free(tmpbuf, M_TEMP);
	return (KERN_FAILURE);
}

int
mach_vm_read(vm_map_t map, mach_vm_address_t addr, mach_vm_size_t size,
			 vm_offset_t *data, mach_msg_type_number_t *dataCnt)
{
	caddr_t tbuf;
	vm_offset_t dstaddr;
	int error;

	/* convert_port_to_map() gives VM_MAP_NULL for a bad port (#300). */
	if (map == VM_MAP_NULL)
		return (KERN_INVALID_ARGUMENT);
	size = round_page(size);

	if ((error = mach_vm_allocate(map, &dstaddr, size, 0)))
		return (KERN_NO_SPACE);
	/*
	 * Copy the data from the target process to the current process
	 * This is reasonable for small chunk of data, but we should
	 * remap COW for areas bigger than a page.
	 */
	tbuf = malloc(size, M_MACH_TMP, M_WAITOK);
#ifdef notyet
	if ((error = copyin_vm_map(map, (caddr_t)addr, tbuf, size)) != 0) {
		printf("copyin_proc error = %d, addr = %lx, size = %zx\n", error, addr, size);
		free(tbuf, M_MACH_TMP);
		return (KERN_PROTECTION_FAILURE);
	}
#endif
	if ((error = copyout(tbuf, (void *)dstaddr, size)) != 0) {
		if (mach_debug_enable)
			printf("copyout error = %d\n", error);
		free(tbuf, M_MACH_TMP);
		return (KERN_PROTECTION_FAILURE);
	}

	free(tbuf, M_MACH_TMP);
	return (0);
}


/*
 * mach_vm_write -
 * Overwrite the specified address range with the data provided
 * (from the current map).
 */
kern_return_t
mach_vm_write(
	vm_map_t			map,
	mach_vm_address_t		address,
	vm_offset_t			data,
	mach_msg_type_number_t	size __unused)
{
	if (map == VM_MAP_NULL)
		return KERN_INVALID_ARGUMENT;

	return vm_map_copy_overwrite(map, (vm_map_address_t)address,
		(vm_map_copy_t) data, FALSE /* interruptible XXX */);
}

int
mach_vm_machine_attribute(vm_map_t target_task, mach_vm_address_t addr, mach_vm_size_t size,
						  vm_machine_attribute_t attribute, vm_machine_attribute_val_t *valuep)
{
	int error = 0;
	vm_machine_attribute_val_t value;

	/* convert_port_to_map() gives VM_MAP_NULL for a bad port (#300). */
	if (target_task == VM_MAP_NULL)
		return (KERN_INVALID_ARGUMENT);
	if ((error = copyin(valuep, &value, sizeof(value))))
		return (KERN_PROTECTION_FAILURE);

	switch (attribute) {
	case MATTR_CACHE:
		switch(value) {
		case MATTR_VAL_CACHE_FLUSH:
		case MATTR_VAL_DCACHE_FLUSH:
		case MATTR_VAL_ICACHE_FLUSH:
		case MATTR_VAL_CACHE_SYNC:
#ifdef notyet
			error = cpu_mach_vm_machine_attribute(target_task, addr, size, &value);
#endif
			break;
		default:
#ifdef DEBUG_MACH
			printf("unimplemented value %d\n", value);
#endif
			error = EINVAL;
			break;
		}
		break;

	case MATTR_MIGRATE:
	case MATTR_REPLICATE:
	default:
#ifdef DEBUG_MACH
		printf("unimplemented attribute %d\n", attribute);
#endif
		error = EINVAL;
		break;
	}


	if (error)
		return (KERN_FAILURE);

	return (copyout(&value, valuep, sizeof(value)));
}

/*
 *	vm_map_entry_dispose:	[ internal use only ]
 *
 *	Inverse of vm_map_entry_create.
 */
#define	vm_map_entry_dispose(map, entry)		\
MACRO_BEGIN						\
	assert((entry) != (map)->first_free &&		\
	       (entry) != (map)->hint);			\
	_vm_map_entry_dispose(&(map)->hdr, (entry));	\
MACRO_END

#define	vm_map_copy_entry_dispose(map, entry) \
	_vm_map_entry_dispose(&(copy)->cpy_hdr, (entry))

#ifdef notused
static uma_zone_t vm_map_entry_zone;
static uma_zone_t vm_map_kentry_zone;

static void
_vm_map_entry_dispose(
	register struct vm_map_header	*map_header,
	register vm_map_entry_t		entry)
{
	uma_zone_t		zone;

	if (map_header->entries_pageable)
	    zone = vm_map_entry_zone;
	else
	    zone = vm_map_kentry_zone;

	uma_zfree(zone, entry);
}
#endif

/*
 *	Routine: vm_map_copyin_internal [internal use only]
 *
 *	Description:
 *		Copy in data to a kernel buffer from space in the
 *		source map. The original space may be optionally
 *		deallocated.
 *
 *		If successful, returns a new copy object.
 */
static kern_return_t
vm_map_copyin_internal(
	vm_map_t	src_map,
	vm_map_offset_t	src_addr,
	vm_map_size_t	len,
	boolean_t	src_destroy,
	vm_map_copy_t	*copy_result)
{
	kern_return_t kr;
	vm_map_copy_t copy;
	vm_size_t size;

	if ((vm_size_t) len != len) {
		/* "len" is too big and doesn't fit in a "vm_size_t" */
		return KERN_RESOURCE_SHORTAGE;
	}
	size =  (vm_size_t) (sizeof(struct vm_map_copy) + len);
	/*
	 * A user-map source is copied in with copyinmap(), which may
	 * fault and sleep, so no non-sleepable lock can be held here and
	 * the allocation may sleep too. A kernel_map source comes from a
	 * kernel MIG reply built in server context; keep M_NOWAIT there
	 * and report KERN_RESOURCE_SHORTAGE on failure.
	 */
	copy = (vm_map_copy_t) malloc(size, M_MACH_VM,
	    (src_map == kernel_map ? M_NOWAIT : M_WAITOK) | M_ZERO);
	if (copy == VM_MAP_COPY_NULL) {
		kr = KERN_RESOURCE_SHORTAGE;
		goto fail;
	}
	copy->type = VM_MAP_COPY_KERNEL_BUFFER;
	copy->size = len;
	copy->offset = 0;
	copy->cpy_kalloc_size = size;
	copy->cpy_kdata = (vm_offset_t) (copy + 1);

	if (src_map == kernel_map) {
		memcpy((void *)copy->cpy_kdata, (void *)src_addr, len);
		free((void *)src_addr, M_MACH_VM);
	} else {

		kr = copyinmap(src_map, src_addr, copy->cpy_kdata, (vm_size_t) len);
		if (kr != KERN_SUCCESS) {
			free(copy, M_MACH_VM);
			return kr;
		}
		if (src_destroy) {
			(void) vm_map_remove(
				src_map,
				trunc_page(src_addr),
				round_page(src_addr + len));
		}
	}
	*copy_result = copy;
	return KERN_SUCCESS;
fail:
	if (src_map == kernel_map)
		free((void *)src_addr, M_MACH_VM);
	return (kr);
}

/*
 *	Routine:	vm_map_copyout_kernel_buffer
 *
 *	Description:
 *		Copy out data from a kernel buffer into space in the
 *		destination map. The space may be otpionally dynamically
 *		allocated.
 *
 *		If successful, consumes the copy object.
 *		Otherwise, the caller is responsible for it.
 */
static kern_return_t
vm_map_copyout_kernel_buffer(
	vm_map_t	map,
	vm_offset_t	*addr,	/* IN/OUT */
	vm_map_copy_t	copy,
	boolean_t	overwrite)
{
	kern_return_t kr = KERN_SUCCESS;

	if (!overwrite) {

		/*
		 * Allocate space in the target map for the data
		 */
		*addr = 0;
		kr = mach_vm_allocate(map, addr, round_page(copy->size), VM_FLAGS_ANYWHERE);
		if (kr != KERN_SUCCESS)
			return(kr);
	}

	/*
	 * Copyout the data from the kernel buffer to the target map.
	 */
	if (current_map() == map) {
		/*
		 * If the target map is the current map, just do
		 * the copy.
		 */
		if (copyout((char *)copy->cpy_kdata, (char *)*addr,
				copy->size)) {
			/*
			 * Not consumed on failure: the caller discards the
			 * copy, so release only the mapping made above.
			 */
			if (!overwrite) {
				(void) mach_vm_deallocate(map, *addr,
				    round_page(copy->size));
				*addr = 0;
			}
			return (KERN_INVALID_ADDRESS);
		}
	}
	else {
		panic("unexpected map %p", map);
#if 0
		vm_map_t oldmap;

		/*
		 * If the target map is another map, assume the
		 * target's address space identity for the duration
		 * of the copy.
		 */
		vm_map_reference(map);
		oldmap = vm_map_switch(map);

		if (copyout((char *)copy->cpy_kdata, (char *)*addr,
				copy->size)) {
			kr = KERN_INVALID_ADDRESS;
		}

		(void) vm_map_switch(oldmap);
		vm_map_deallocate(map);
#endif
		return (KERN_NOT_SUPPORTED);
	}

	free(copy, M_MACH_VM);

	return(kr);
}



/*
 * vm_map_copyin — capture a user-space range into a vm_map_copy_t
 * the MIG send path then transports as OOL data.
 *
 * The historical Apple implementation had THREE branches:
 *   1. len < MSG_OOL_SIZE_SMALL → vm_map_copyin_internal (KERNEL_BUFFER
 *      copy, kalloc + copyinmap).
 *   2. src_map == kernel_map → same kernel-buffer path.
 *   3. len ≥ MSG_OOL_SIZE_SMALL on a user map → object-sharing path
 *      (lookup entry, clip, vm_object_reference, COW the source,
 *      vm_map_copyin_object). Cheap on Apple where vm_map_copyout
 *      maps the shared object into the destination process with the
 *      source's pages visible through it.
 *
 * Branch 3 is silently broken on FreeBSD: anon vm_objects are
 * private/COW per process, so when vm_map_copyout calls vm_map_find
 * to map the source's anon object into the destination, the
 * destination gets a fresh demand-zero view of the object, not the
 * source pages. launchctl list (20 MiB GETJOBS OOL response) hit
 * this and got 20 MiB of zeros; launch_data_unpack rejected it
 * with EINVAL at offset 24 (decoded type=0, default case).
 *
 * Policy (nextbsd/nextbsd#66): every caller goes through the
 * kernel-buffer copy, and no VM_MAP_COPY_OBJECT copy is produced or
 * consumed. A shared-page or refcounted-COW transport was considered
 * and rejected: the userland leak that motivated it was fixed in
 * libmach (nextbsd-userland PR #88), and what remains is a transient
 * kernel allocation the size of the message body on rare large OOL
 * messages. User-map sources sleep for that allocation; kernel_map
 * sources fail KERN_RESOURCE_SHORTAGE. See bsd01 debug session
 * 2026-05-23 in the commit log for the original zero-fill bug.
 */
kern_return_t
vm_map_copyin(
	vm_map_t		src_map,
	vm_map_address_t	src_addr,
	vm_map_size_t		len,
	boolean_t		src_destroy,
	vm_map_copy_t		*copy_result)
{
	if (len == 0) {
		*copy_result = VM_MAP_COPY_NULL;
		return (KERN_SUCCESS);
	}

	if (round_page(src_addr + len) < src_addr)
		return (KERN_INVALID_ADDRESS);

	return (vm_map_copyin_internal(src_map, src_addr, len,
	    src_destroy, copy_result));
}



/*
 *	Routine:	vm_map_copyout
 *
 *	Description:
 *		Copy out a copy chain ("copy") into newly-allocated
 *		space in the destination map.
 *
 *		If successful, consumes the copy object.
 *		Otherwise, the caller is responsible for it.
 */


kern_return_t
vm_map_copyout(
	vm_map_t		dst_map,
	vm_map_address_t	*dst_addr,	/* OUT */
	vm_map_copy_t		copy)
{

	*dst_addr = 0;
	/*
	 * vm_map_copyin() only ever produces VM_MAP_COPY_KERNEL_BUFFER
	 * copies (see the comment above it); nothing else is supported.
	 */
	if (copy->type == VM_MAP_COPY_KERNEL_BUFFER) {
		return (vm_map_copyout_kernel_buffer(dst_map, dst_addr,
						    copy, FALSE));
	}
	return KERN_NOT_SUPPORTED;
}

/*
 *	Routine:	vm_map_copy_discard
 *
 *	Description:
 *		Dispose of a map copy object (returned by
 *		vm_map_copyin).
 */
void
vm_map_copy_discard(
	vm_map_copy_t	copy)
{

	if (copy == VM_MAP_COPY_NULL)
		return;

	free(copy, M_MACH_VM);
}

kern_return_t
vm_map_copy_overwrite(vm_map_t                dst_map,
					  vm_map_address_t        dst_addr,
					  vm_map_copy_t           copy,
					  boolean_t               interruptible)
{
	return (KERN_NOT_SUPPORTED);
}

int
mach_vm_behavior_set(vm_map_t target_task, mach_vm_address_t address, mach_vm_size_t size, vm_behavior_t new_behavior)
{

	return (KERN_NOT_SUPPORTED);
}

int
mach_vm_page_info(vm_map_t target_task, mach_vm_address_t address,
				  vm_page_info_flavor_t flavor, vm_page_info_t info,
				  mach_msg_type_number_t *infoCnt)
{

		return (KERN_NOT_SUPPORTED);
}

int
mach_vm_page_query(
	vm_map_t target_map,
	mach_vm_offset_t offset,
	integer_t *disposition,
	integer_t *ref_count
)
{

		return (KERN_NOT_SUPPORTED);
}

int
mach_vm_purgable_control(
	vm_map_t target_task,
	mach_vm_address_t address,
	vm_purgable_t control,
	int *state
	)
{

	return (KERN_NOT_SUPPORTED);
}
	
int
mach_vm_read_list(
	vm_map_t target_task,
	mach_vm_read_entry_t data_list,
	natural_t count
	)
{
	return (KERN_NOT_SUPPORTED);
}


int
vm_read_list(
	vm_map_t target_task,
	vm_read_entry_t data_list,
	natural_t count
	)
{
	return (KERN_NOT_SUPPORTED);
}

int
mach_vm_read_overwrite(
	vm_map_t target_task,
	mach_vm_address_t address,
	mach_vm_size_t size,
	mach_vm_address_t data,
	mach_vm_size_t *outsize
)
{
	return (KERN_NOT_SUPPORTED);
}

int
mach_vm_region(
	vm_map_t target_task,
	mach_vm_address_t *address,
	mach_vm_size_t *size,
	vm_region_flavor_t flavor,
	vm_region_info_t info,
	mach_msg_type_number_t *infoCnt,
	mach_port_t *object_name
	)
{
	/*
	 * MACH_VM_REGION_BASIC_INFO is the only
	 * supported flavor in Darwin.
	 */

	return (KERN_NOT_SUPPORTED);
}

int
mach_vm_region_info(
	vm_map_t task,
	vm_address_t address,
	vm_info_region_t *region,
	vm_info_object_array_t *objects,
	mach_msg_type_number_t *objectsCnt
)
{
	return (KERN_NOT_SUPPORTED);
}

int
mach_vm_region_info_64(
	vm_map_t task,
	vm_address_t address,
	vm_info_region_64_t *region,
	vm_info_object_array_t *objects,
	mach_msg_type_number_t *objectsCnt
)
{
	return (KERN_NOT_SUPPORTED);
}

int
mach_vm_region_recurse(
	vm_map_t target_task,
	mach_vm_address_t *address,
	mach_vm_size_t *size,
	natural_t *nesting_depth,
	vm_region_recurse_info_t info,
	mach_msg_type_number_t *infoCnt
	)
{
	return (KERN_NOT_SUPPORTED);
}

int
mach_vm_mapped_pages_info(
	vm_map_t task,
	page_address_array_t *pages,
	mach_msg_type_number_t *pagesCnt
)
{
	return (KERN_NOT_SUPPORTED);
}

	
int
mach_vm_remap(
	vm_map_t target_task,
	mach_vm_address_t *target_address,
	mach_vm_size_t size,
	mach_vm_offset_t mask,
	int flags,
	vm_map_t src_task,
	mach_vm_address_t src_address,
	boolean_t copy,
	vm_prot_t *cur_protection,
	vm_prot_t *max_protection,
	vm_inherit_t inheritance
	)
{
	return (KERN_NOT_SUPPORTED);
}

int
mach_vm_wire(
	host_priv_t host_priv,
	vm_map_t task,
	mach_vm_address_t address,
	mach_vm_size_t size,
	vm_prot_t desired_access
)
{
	return (KERN_NOT_SUPPORTED);
}

int
mach_vm_wire_32(
	host_priv_t host_priv,
	vm_map_t task,
	vm_address_t address,
	vm_size_t size,
	vm_prot_t desired_access
)
{
	return (KERN_NOT_SUPPORTED);
}

int
task_wire(vm_map_t target_task, boolean_t must_wire)
{

	return (KERN_NOT_SUPPORTED);
}

int
vm_allocate_cpm
(
	host_priv_t host_priv,
	vm_map_t task,
	vm_address_t *address,
	vm_size_t size,
	int flags
)
{

	return (KERN_NOT_SUPPORTED);
}
