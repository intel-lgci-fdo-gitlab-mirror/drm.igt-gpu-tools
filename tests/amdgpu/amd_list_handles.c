// SPDX-License-Identifier: MIT
/*
 * Copyright (c) 2026 Valve Corporation
 */
#include <sys/ioctl.h>

#include "igt.h"
#include "drmtest.h"

#include <amdgpu.h>
#include <amdgpu_drm.h>

#include "lib/amdgpu/amd_memory.h"

#define TEST_PADDING (1 << 0)
#define TEST_FAULT   (1 << 1)
#define TEST_TWOPASS (1 << 2)
#define TEST_PROBE   (1 << 3)

#define BUFFER_SIZE    4096
#define MAX_BOS        1024

static void
basic(amdgpu_device_handle dev, unsigned int num_bos, unsigned int flags)
{
	struct drm_amdgpu_gem_list_handles_entry entry[MAX_BOS];
	struct drm_amdgpu_gem_list_handles list = {
		.entries = (uintptr_t)entry,
		.num_entries = num_bos,
	};
	int fd = amdgpu_device_get_fd(dev);
	amdgpu_bo_handle bo[MAX_BOS];
	int i, ret;

	igt_assert(num_bos <= MAX_BOS);
	igt_assert_fd(fd);

	for (i = 0; i < num_bos; i++) {
		struct amdgpu_bo_alloc_request alloc = {
			.alloc_size = BUFFER_SIZE,
			.preferred_heap = AMDGPU_GEM_DOMAIN_GTT,
		};

		ret = amdgpu_bo_alloc(dev, &alloc, &bo[i]);
		igt_assert_eq(ret, 0);
	}

	if (flags & TEST_PADDING) {
		/*
		 * Apparently kernel never checked for mbz so now we have to
		 * keep accepting garbage.
		 */
		list.padding = 0x13ac5;
	} else if (flags & TEST_FAULT) {
		list.entries = 0;
	}

	if (flags & TEST_TWOPASS) {
		if (flags & TEST_PROBE) {
			list.num_entries = 0;
		} else {
			igt_assert(num_bos > 1);
			list.num_entries = num_bos - 1;
		}
		ret = ioctl(fd, DRM_IOCTL_AMDGPU_GEM_LIST_HANDLES, &list);
		igt_assert_eq(ret, 0);
		igt_assert_eq(list.num_entries, num_bos);
	}

	ret = ioctl(fd, DRM_IOCTL_AMDGPU_GEM_LIST_HANDLES, &list);
	if (flags & TEST_FAULT) {
		i = errno;
		igt_assert_eq(ret, -1);
		igt_assert_eq(i, EFAULT);
		goto out;
	}
	igt_assert_eq(ret, 0);
	igt_assert_eq(list.num_entries, num_bos);

	for (i = 0; i < num_bos; i++) {
		struct drm_amdgpu_gem_list_handles_entry *e = &entry[i];
		uint32_t handle;

		ret = amdgpu_bo_export(bo[i],
			       amdgpu_bo_handle_type_kms_noimport,
			       &handle);
		igt_assert_eq(ret, 0);
		igt_assert_eq(e->gem_handle, handle);

		igt_assert_eq(e->flags, 0);
		igt_assert_eq(e->size, BUFFER_SIZE);
		igt_assert_eq(e->preferred_domains, AMDGPU_GEM_DOMAIN_GTT);
		igt_assert_eq(e->alloc_flags,
			      AMDGPU_GEM_CREATE_VRAM_CLEARED |
			      AMDGPU_GEM_CREATE_VRAM_WIPE_ON_RELEASE);
		igt_assert_eq(e->alignment, 0);
	}

out:
	for (i = 0; i < num_bos; i++)
		amdgpu_bo_free(bo[i]);
}

int igt_main()
{
	amdgpu_device_handle dev;
	int fd = -1;

	igt_fixture() {
		uint32_t major, minor;
		int err;

		fd = drm_open_driver_render(DRIVER_AMDGPU);

		err = amdgpu_device_initialize(fd, &major, &minor, &dev);
		igt_require(err == 0);
	}

	igt_describe("Check padding backward compatibility");
	igt_subtest("padding")
		basic(dev, 1, TEST_PADDING);

	igt_describe("Check fault detection");
	igt_subtest("fault")
		basic(dev, 1, TEST_FAULT);

	igt_describe("List no buffer objects");
	igt_subtest("no-bos")
		basic(dev, 0, 0);

	igt_describe("Create and list one buffer object");
	igt_subtest("one-bo")
		basic(dev, 1, 0);

	igt_describe("Create and list many buffer objects");
	igt_subtest("many-bo")
		basic(dev, 1024, 0);

	igt_describe("Create and list many buffer objects in two-pass mode");
	igt_subtest("many-bo-two-pass")
		basic(dev, 16, TEST_TWOPASS);

	igt_describe("Create and list many buffer objects in two-pass probe mode");
	igt_subtest("many-bo-two-pass-probe")
		basic(dev, 16, TEST_TWOPASS | TEST_PROBE);

	igt_fixture() {
		amdgpu_device_deinitialize(dev);
		drm_close_driver(fd);
	}
}
