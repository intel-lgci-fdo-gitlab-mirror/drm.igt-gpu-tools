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

	igt_fixture()
	{
		drm_close_driver(fd);
	}
}
