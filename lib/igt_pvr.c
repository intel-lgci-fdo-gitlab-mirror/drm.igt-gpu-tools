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
