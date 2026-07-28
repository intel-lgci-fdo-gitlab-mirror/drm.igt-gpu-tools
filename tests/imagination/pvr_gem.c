// SPDX-License-Identifier: GPL-2.0 or MIT
/* Copyright (c) 2026 Imagination Technologies Ltd. All Rights Reserved */

#include <linux/errno.h>
#include <stddef.h>
#include <stdint.h>

#include "igt.h"
#include "igt_pvr.h"

#include "pvr_drm.h"

int igt_main()
{
	int fd;

	igt_fixture()
	{
		fd = drm_open_driver(DRIVER_POWERVR);
	}

	igt_describe("Test valid buffer object creation");
	igt_subtest("create-bo-4096")
	{
		size_t size = 4096;
		uint32_t handle = igt_pvr_ioctl_create_bo(fd, &size);

		gem_close(fd, handle);
	}

	igt_describe("Test buffer object creation with 0 size");
	igt_subtest("create-bo-0")
	{
		struct drm_pvr_ioctl_create_bo_args arg = {
			.size = 0,
		};

		do_ioctl_err(fd, DRM_IOCTL_PVR_CREATE_BO, &arg, EINVAL);
	}

	igt_describe("Test buffer object creation with bad padding");
	igt_subtest("create-bo-bad-padding")
	{
		struct drm_pvr_ioctl_create_bo_args arg = {
			.size = 4096,
			._padding_c = 0xbad6bad6,
		};

		do_ioctl_err(fd, DRM_IOCTL_PVR_CREATE_BO, &arg, EINVAL);
	}

	igt_describe("Test buffer object creation with unaligned size");
	igt_subtest("create-bo-unaligned-fail")
	{
		struct drm_pvr_ioctl_create_bo_args arg = {
			.size = 4000,
		};

		do_ioctl_err(fd, DRM_IOCTL_PVR_CREATE_BO, &arg, EINVAL);
	}

	igt_describe("Test valid buffer object mapping");
	igt_subtest("mmap-bo-4096")
	{
		size_t size = 4096;
		uint32_t handle = igt_pvr_ioctl_create_bo(fd, &size);

		off_t offset = igt_pvr_ioctl_get_bo_mmap_offset(fd, handle);

		char *mapped = mmap(NULL, size, PROT_WRITE, MAP_SHARED,
				    fd, offset);
		igt_assert(mapped != MAP_FAILED);

		/* Test writing to beginning and end of mapped range. */
		mapped[0] = 0xff;
		mapped[size - 1] = 0xff;

		munmap(mapped, size);
		gem_close(fd, handle);
	}

	igt_describe("Test buffer object mapping with bad handle");
	igt_subtest("mmap-bo-bad-handle")
	{
		struct drm_pvr_ioctl_get_bo_mmap_offset_args arg = {
			.handle = 0xbad6bad6,
		};

		do_ioctl_err(fd, DRM_IOCTL_PVR_GET_BO_MMAP_OFFSET, &arg, ENOENT);
	}

	igt_describe("Test buffer object mapping with bad padding");
	igt_subtest("mmap-bo-bad-padding")
	{
		size_t size = 4096;
		uint32_t handle = igt_pvr_ioctl_create_bo(fd, &size);

		struct drm_pvr_ioctl_get_bo_mmap_offset_args arg = {
			.handle = handle,
			._padding_4 = 0xbad6bad6,
		};

		do_ioctl_err(fd, DRM_IOCTL_PVR_GET_BO_MMAP_OFFSET, &arg, EINVAL);
	}

	igt_fixture()
	{
		drm_close_driver(fd);
	}
}
