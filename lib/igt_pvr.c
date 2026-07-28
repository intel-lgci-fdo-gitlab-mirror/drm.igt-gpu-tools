// SPDX-License-Identifier: GPL-2.0 or MIT
/* Copyright (c) 2026 Imagination Technologies Ltd. All Rights Reserved */

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "igt_pvr.h"

#include "drmtest.h"
#include "ioctl_wrappers.h"

#include "pvr_drm.h"

/**
 * SECTION:igt_pvr
 * @short_description: PowerVR support library
 * @title: pvr
 * @include: igt.h
 *
 * This library provides various auxiliary helper functions for writing PowerVR
 * tests.
 */

/**
 * igt_pvr_ioctl_create_bo_ex:
 * @fd: The file descriptor of the DRM device.
 * @size: On entry, the requested size of the buffer object. On return, the
 *        actual size of the buffer object.
 * @flags: Flags to control the behaviour of the buffer object.
 *
 * Function to create a buffer object.
 *
 * Returns: The handle of the created buffer object.
 */
uint32_t igt_pvr_ioctl_create_bo_ex(int fd, size_t *size, uint64_t flags)
{
	struct drm_pvr_ioctl_create_bo_args arg = {
		.size = *size,
		.flags = flags,
	};

	do_ioctl(fd, DRM_IOCTL_PVR_CREATE_BO, &arg);

	igt_assert(arg.size >= *size && arg.size <= SIZE_MAX);
	*size = (size_t)arg.size;

	return arg.handle;
}

/**
 * igt_pvr_ioctl_create_bo:
 * @fd: The file descriptor of the DRM device.
 * @size: On entry, the requested size of the buffer object. On return, the
 *        actual size of the buffer object.
 *
 * Function to create a buffer object with default flags.
 *
 * Returns: The handle of the created buffer object.
 */
uint32_t igt_pvr_ioctl_create_bo(int fd, size_t *size)
{
	return igt_pvr_ioctl_create_bo_ex(fd, size,
					  DRM_PVR_BO_ALLOW_CPU_USERSPACE_ACCESS);
}

/**
 * igt_pvr_ioctl_get_bo_mmap_offset:
 * @fd: The file descriptor of the DRM device.
 * @handle: The handle of the buffer object.
 *
 * Function to get the mmap offset of a buffer object.
 *
 * Returns: The mmap offset of the buffer object.
 */
off_t igt_pvr_ioctl_get_bo_mmap_offset(int fd, uint32_t handle)
{
	struct drm_pvr_ioctl_get_bo_mmap_offset_args arg = {
		.handle = handle,
	};

	do_ioctl(fd, DRM_IOCTL_PVR_GET_BO_MMAP_OFFSET, &arg);

	/*
	 * There is no OFF_MAX equivalent to SIZE_MAX; use the identical (by
	 * definition) PTRDIFF_MAX instead.
	 */
	igt_assert(arg.offset <= PTRDIFF_MAX);

	return (off_t)arg.offset;
}

/**
 * igt_pvr_ioctl_dev_query:
 * @fd: The file descriptor of the DRM device.
 * @type: The type of the device query.
 * @size: The size of the query structure.
 * @pointer: Pointer to the query structure.
 * @expect_err: Expected error code, or 0 if no error is expected.
 *
 * Function to perform a device query.
 *
 * Returns: The filled-in query arguments structure.
 */
struct drm_pvr_ioctl_dev_query_args
igt_pvr_ioctl_dev_query(int fd, enum drm_pvr_dev_query type, uint64_t size,
			void *pointer, int expect_err)
{
	struct drm_pvr_ioctl_dev_query_args args = {
		.type = type,
		.size = size,
		.pointer = to_user_pointer(pointer),
	};

	if (expect_err)
		do_ioctl_err(fd, DRM_IOCTL_PVR_DEV_QUERY, &args, expect_err);
	else
		do_ioctl(fd, DRM_IOCTL_PVR_DEV_QUERY, &args);

	return args;
}

/**
 * igt_pvr_get_heap_info:
 * @fd: The file descriptor of the DRM device.
 * @array_len_out: Pointer to store the number of heaps.
 *
 * Function to get information about the device heaps.
 *
 * Returns: An array of drm_pvr_heap structures. The caller is responsible
 * for freeing the array.
 */
struct drm_pvr_heap *
igt_pvr_get_heap_info(int fd, uint32_t *array_len_out)
{
	struct drm_pvr_heap *heaps =
		calloc(DRM_PVR_HEAP_COUNT, sizeof(*heaps));
	struct drm_pvr_dev_query_heap_info heap_info_get = {
		.heaps = DRM_PVR_OBJ_ARRAY(DRM_PVR_HEAP_COUNT, heaps),
	};

	if (!heaps)
		return NULL;

	igt_pvr_ioctl_dev_query(fd, DRM_PVR_DEV_QUERY_HEAP_INFO_GET,
				sizeof(heap_info_get), &heap_info_get, 0);

	if (array_len_out)
		*array_len_out = heap_info_get.heaps.count;

	return heaps;
}

/**
 * igt_pvr_find_general_heap:
 * @fd: The file descriptor of the DRM device.
 *
 * Function to find the general heap of the device.
 *
 * Returns: The drm_pvr_heap structure representing the general heap.
 */
struct drm_pvr_heap igt_pvr_find_general_heap(int fd)
{
	struct drm_pvr_heap *heaps = igt_pvr_get_heap_info(fd, NULL);
	struct drm_pvr_heap general_heap;

	igt_assert(heaps);

	general_heap = heaps[DRM_PVR_HEAP_GENERAL];
	free(heaps);

	return general_heap;
}

/**
 * igt_pvr_get_static_data_areas:
 * @fd: The file descriptor of the DRM device.
 * @array_len_out: Pointer to store the number of static data areas.
 *
 * Function to get information about the device static data areas.
 *
 * Returns: An array of drm_pvr_static_data_area structures. The caller is responsible
 * for freeing the array.
 */
struct drm_pvr_static_data_area *
igt_pvr_get_static_data_areas(int fd, uint32_t *array_len_out)
{
	struct drm_pvr_static_data_area *sdas =
		calloc(DRM_PVR_STATIC_DATA_AREA_YUV_CSC + 1, sizeof(*sdas));
	struct drm_pvr_dev_query_static_data_areas sdas_get = {
		.static_data_areas =
			DRM_PVR_OBJ_ARRAY(DRM_PVR_STATIC_DATA_AREA_YUV_CSC + 1,
					  sdas),
	};

	igt_pvr_ioctl_dev_query(fd, DRM_PVR_DEV_QUERY_STATIC_DATA_AREAS_GET,
				sizeof(sdas_get), &sdas_get, 0);

	if (array_len_out)
		*array_len_out = sdas_get.static_data_areas.count;

	return sdas;
}

/**
 * igt_pvr_ioctl_create_vm_context:
 * @fd: The file descriptor of the DRM device.
 * @expect_err: Expected error code, or 0 if no error is expected.
 *
 * Function to create a VM context.
 *
 * Returns: The handle of the created VM context.
 */
uint32_t igt_pvr_ioctl_create_vm_context(int fd, int expect_err)
{
	struct drm_pvr_ioctl_create_vm_context_args args = {0};

	if (expect_err)
		do_ioctl_err(fd, DRM_IOCTL_PVR_CREATE_VM_CONTEXT, &args,
			     expect_err);
	else
		do_ioctl(fd, DRM_IOCTL_PVR_CREATE_VM_CONTEXT, &args);

	return args.handle;
}

/**
 * igt_pvr_ioctl_destroy_vm_context:
 * @fd: The file descriptor of the DRM device.
 * @handle: The handle of the VM context to destroy.
 * @expect_err: Expected error code, or 0 if no error is expected.
 *
 * Function to destroy a VM context.
 */
void igt_pvr_ioctl_destroy_vm_context(int fd, uint32_t handle, int expect_err)
{
	struct drm_pvr_ioctl_destroy_vm_context_args args = {
		.handle = handle,
	};

	if (expect_err)
		do_ioctl_err(fd, DRM_IOCTL_PVR_DESTROY_VM_CONTEXT, &args,
			     expect_err);
	else
		do_ioctl(fd, DRM_IOCTL_PVR_DESTROY_VM_CONTEXT, &args);
}

/**
 * igt_pvr_ioctl_vm_map:
 * @fd: The file descriptor of the DRM device.
 * @vm_ctx_handle: The handle of the VM context.
 * @handle: The handle of the target buffer object to map.
 * @device_addr: Requested virtual address in the device's address space.
 * @offset: The offset within the buffer object.
 * @size: The size of the mapping.
 *
 * Function to map a buffer object into a VM context.
 */
void igt_pvr_ioctl_vm_map(int fd, uint32_t vm_ctx_handle, uint32_t handle,
			  uint64_t device_addr, uint64_t offset, uint64_t size)
{
	struct drm_pvr_ioctl_vm_map_args arg = {
		.vm_context_handle = vm_ctx_handle,
		.flags = 0,
		.device_addr = device_addr,
		.handle = handle,
		.offset = offset,
		.size = size,
	};

	do_ioctl(fd, DRM_IOCTL_PVR_VM_MAP, &arg);
}

/**
 * igt_pvr_ioctl_vm_unmap:
 * @fd: The file descriptor of the DRM device.
 * @vm_ctx_handle: The handle of the VM context.
 * @device_addr: The device virtual address of the mapping to unmap.
 * @size: The size of the mapping.
 *
 * Function to unmap a buffer object from a VM context.
 */
void igt_pvr_ioctl_vm_unmap(int fd, uint32_t vm_ctx_handle,
			    uint64_t device_addr, uint64_t size)
{
	struct drm_pvr_ioctl_vm_unmap_args arg = {
		.vm_context_handle = vm_ctx_handle,
		.device_addr = device_addr,
		.size = size,
	};

	do_ioctl(fd, DRM_IOCTL_PVR_VM_UNMAP, &arg);
}

/**
 * igt_pvr_ioctl_create_free_list:
 * @fd: The file descriptor of the DRM device.
 * @vm_ctx_handle: The handle of the VM context.
 * @gpu_addr: The GPU address for the free list.
 *
 * Function to create a free list.
 */
uint32_t igt_pvr_ioctl_create_free_list(int fd, uint32_t vm_ctx_handle,
					uint64_t gpu_addr)
{
	struct drm_pvr_ioctl_create_free_list_args create_free_list_args = {
		.free_list_gpu_addr = gpu_addr,
		.initial_num_pages = 64,
		.max_num_pages = 256,
		.grow_num_pages = 16,
		.grow_threshold = 50,
		.vm_context_handle = vm_ctx_handle,
	};

	do_ioctl(fd, DRM_IOCTL_PVR_CREATE_FREE_LIST, &create_free_list_args);

	return create_free_list_args.handle;
}

/**
 * igt_pvr_ioctl_destroy_free_list:
 * @fd: The file descriptor of the DRM device.
 * @free_list_handle: The handle of the free list to destroy.
 *
 * Function to destroy a free list.
 */
void igt_pvr_ioctl_destroy_free_list(int fd, uint32_t free_list_handle)
{
	struct drm_pvr_ioctl_destroy_free_list_args destroy_free_list_args = {
		.handle = free_list_handle,
	};

	do_ioctl(fd, DRM_IOCTL_PVR_DESTROY_FREE_LIST, &destroy_free_list_args);
}
