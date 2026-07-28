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
 * igt_pvr_ioctl_create_bo:
 * @fd: The file descriptor of the DRM device.
 * @size: On entry, the requested size of the buffer object. On return, the
 *        actual size of the buffer object.
 *
 * Function to create a buffer object.
 *
 * Returns: The handle of the created buffer object.
 */
uint32_t igt_pvr_ioctl_create_bo(int fd, size_t *size)
{
	struct drm_pvr_ioctl_create_bo_args arg = {
		.size = *size,
	};

	do_ioctl(fd, DRM_IOCTL_PVR_CREATE_BO, &arg);

	igt_assert(arg.size >= *size && arg.size <= SIZE_MAX);
	*size = (size_t)arg.size;

	return arg.handle;
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
