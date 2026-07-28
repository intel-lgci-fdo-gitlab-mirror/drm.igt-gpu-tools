// SPDX-License-Identifier: GPL-2.0 or MIT
/* Copyright (c) 2026 Imagination Technologies Ltd. All Rights Reserved */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

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

	igt_describe("Test valid VM context creation and destruction");
	igt_subtest("create-and-destroy-vm-ctx")
	{
		const uint32_t handle = igt_pvr_ioctl_create_vm_context(fd, 0);

		igt_pvr_ioctl_destroy_vm_context(fd, handle, 0);
	}

	igt_describe("Test VM context creation with bad padding");
	igt_subtest("create-vm-ctx-bad-padding")
	{
		struct drm_pvr_ioctl_create_vm_context_args args = {
			._padding_4 = 1,
		};

		do_ioctl_err(fd, DRM_IOCTL_PVR_CREATE_VM_CONTEXT, &args, EINVAL);
	}

	igt_describe("Test VM context destruction with bad padding");
	igt_subtest("destroy-vm-ctx-bad-padding")
	{
		struct drm_pvr_ioctl_destroy_vm_context_args args = {
			._padding_4 = 1,
		};

		do_ioctl_err(fd, DRM_IOCTL_PVR_DESTROY_VM_CONTEXT, &args, EINVAL);
	}

	igt_describe("Test VM context destruction with bad handle");
	igt_subtest("destroy-vm-ctx-bad-handle")
	{
		struct drm_pvr_ioctl_destroy_vm_context_args args = {
			.handle = 0xbad6bad6,
		};

		do_ioctl_err(fd, DRM_IOCTL_PVR_DESTROY_VM_CONTEXT, &args, EINVAL);
	}

	igt_describe("Test VM context destruction with handle that is not a VM context");
	igt_subtest("destroy-vm-ctx-handle-is-not-vm-ctx")
	{
		size_t size = 4096;
		const uint32_t bo_handle = igt_pvr_ioctl_create_bo(fd, &size);
		struct drm_pvr_ioctl_destroy_vm_context_args args = {
			.handle = bo_handle,
		};

		do_ioctl_err(fd, DRM_IOCTL_PVR_DESTROY_VM_CONTEXT, &args, EINVAL);
		gem_close(fd, bo_handle);
	}

	igt_fixture()
	{
		drm_close_driver(fd);
	}
}
