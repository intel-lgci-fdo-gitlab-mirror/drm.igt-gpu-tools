// SPDX-License-Identifier: GPL-2.0 or MIT
/* Copyright (c) 2026 Imagination Technologies Ltd. All Rights Reserved */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include "igt.h"
#include "igt_pvr.h"

#include "pvr_drm.h"

#define INVALID_ADDRESS 0xdeadbee0

static void build_create_args(struct drm_pvr_ioctl_create_hwrt_dataset_args *create_hwrt_args,
			      const struct drm_pvr_create_hwrt_geom_data_args *geom_data_args,
			      const struct drm_pvr_create_hwrt_rt_data_args *rt_data_args,
			      uint32_t *free_list_handles,
			      uint32_t num_rt_data,
			      uint32_t num_free_lists)
{
	memset(create_hwrt_args, 0, sizeof(*create_hwrt_args));
	memcpy(&create_hwrt_args->geom_data_args, geom_data_args,
	       sizeof(*geom_data_args));

	for (uint32_t i = 0; i < num_rt_data; i++) {
		memcpy(&create_hwrt_args->rt_data_args[i], rt_data_args,
		       sizeof(*rt_data_args) * num_rt_data);
	}

	for (uint32_t i = 0; i < num_free_lists; i++)
		create_hwrt_args->free_list_handles[i] = free_list_handles[i];

	create_hwrt_args->width = 256;
	create_hwrt_args->height = 256;
	create_hwrt_args->samples = 1;
	create_hwrt_args->layers = 1;
	create_hwrt_args->region_header_size = 64;
}

static void build_destroy_args(struct drm_pvr_ioctl_destroy_hwrt_dataset_args *destroy_hwrt_args,
			       uint32_t handle)
{
	memset(destroy_hwrt_args, 0, sizeof(*destroy_hwrt_args));
	destroy_hwrt_args->handle = handle;
}

int igt_main()
{
	struct drm_pvr_ioctl_create_hwrt_dataset_args create_hwrt_args;
	struct drm_pvr_create_hwrt_geom_data_args geom_data_args = {0};
	struct drm_pvr_create_hwrt_rt_data_args rt_data_args[2] = {0};
	struct drm_pvr_ioctl_destroy_hwrt_dataset_args destroy_hwrt_args;

	const uint32_t num_free_lists =
		ARRAY_SIZE(create_hwrt_args.free_list_handles);
	uint32_t free_list_handles[num_free_lists];
	uint32_t gem_obj_handle;
	uint32_t gem_obj_handle_ml;
	uint32_t vm_ctx_handle;
	const uint32_t num_rt_data = ARRAY_SIZE(create_hwrt_args.rt_data_args);
	uint64_t gem_obj_gpu_addr;
	uint64_t gem_obj_gpu_addr_ml;
	size_t size = 2 << 20;
	int fd;

	igt_fixture()
	{
		struct drm_pvr_heap general_heap_info;

		fd = drm_open_driver(DRIVER_POWERVR);
		general_heap_info = igt_pvr_find_general_heap(fd);
		vm_ctx_handle = igt_pvr_ioctl_create_vm_context(fd, 0);

		gem_obj_gpu_addr = general_heap_info.base;
		gem_obj_handle =
			igt_pvr_ioctl_create_bo_ex(fd, &size,
						   DRM_PVR_BO_PM_FW_PROTECT);
		igt_pvr_ioctl_vm_map(fd, vm_ctx_handle, gem_obj_handle,
				     gem_obj_gpu_addr, 0, size);

		for (int i = 0; i < num_free_lists; i++)
			free_list_handles[i] =
				igt_pvr_ioctl_create_free_list(fd,
							       vm_ctx_handle,
							       gem_obj_gpu_addr);

		gem_obj_gpu_addr_ml = general_heap_info.base + size;
		gem_obj_handle_ml =
			igt_pvr_ioctl_create_bo_ex(fd, &size,
						   DRM_PVR_BO_PM_FW_PROTECT);
		igt_pvr_ioctl_vm_map(fd, vm_ctx_handle, gem_obj_handle_ml,
				     gem_obj_gpu_addr_ml, 0, size);

		for (int i = 0; i < num_rt_data; i++)
			rt_data_args[i].pm_mlist_dev_addr = gem_obj_gpu_addr_ml;
	}

	igt_describe("Test valid HWRT creation and destruction");
	igt_subtest("create-hwrt")
	{
		build_create_args(&create_hwrt_args, &geom_data_args,
				  rt_data_args, free_list_handles,
				       num_rt_data, num_free_lists);
		do_ioctl(fd, DRM_IOCTL_PVR_CREATE_HWRT_DATASET, &create_hwrt_args);

		igt_assert_neq(create_hwrt_args.handle, 0);

		build_destroy_args(&destroy_hwrt_args, create_hwrt_args.handle);
		do_ioctl(fd, DRM_IOCTL_PVR_DESTROY_HWRT_DATASET, &destroy_hwrt_args);
	}

	igt_describe("Test HWRT creation with bad padding");
	igt_subtest("destroy-hwrt-bad-padding")
	{
		build_create_args(&create_hwrt_args, &geom_data_args,
				  rt_data_args, free_list_handles,
				       num_rt_data, num_free_lists);
		do_ioctl(fd, DRM_IOCTL_PVR_CREATE_HWRT_DATASET, &create_hwrt_args);

		igt_assert_neq(create_hwrt_args.handle, 0);

		build_destroy_args(&destroy_hwrt_args, create_hwrt_args.handle);
		destroy_hwrt_args._padding_4 = 1;
		do_ioctl_err(fd, DRM_IOCTL_PVR_DESTROY_HWRT_DATASET, &destroy_hwrt_args,
			     EINVAL);

		destroy_hwrt_args._padding_4 = 0;
		do_ioctl(fd, DRM_IOCTL_PVR_DESTROY_HWRT_DATASET, &destroy_hwrt_args);
	}

	igt_describe("Test HWRT creation with invalid free list handle");
	igt_subtest("invalid-free-list-handle")
	{
		build_create_args(&create_hwrt_args, &geom_data_args,
				  rt_data_args, free_list_handles,
				       num_rt_data, num_free_lists);
		create_hwrt_args.free_list_handles[0] = INVALID_ADDRESS;
		do_ioctl_err(fd, DRM_IOCTL_PVR_CREATE_HWRT_DATASET,
			     &create_hwrt_args, EINVAL);
	}

	igt_describe("Test HWRT creation with missing free list");
	igt_subtest("missing-free-list")
	{
		build_create_args(&create_hwrt_args, &geom_data_args,
				  rt_data_args, free_list_handles,
				       num_rt_data, num_free_lists);
		create_hwrt_args.free_list_handles[0] = 0;
		do_ioctl_err(fd, DRM_IOCTL_PVR_CREATE_HWRT_DATASET,
			     &create_hwrt_args, EINVAL);
	}

	igt_fixture()
	{
		for (int i = 0; i < num_free_lists; i++)
			igt_pvr_ioctl_destroy_free_list(fd, free_list_handles[i]);

		igt_pvr_ioctl_vm_unmap(fd, vm_ctx_handle, gem_obj_gpu_addr,
				       size);
		gem_close(fd, gem_obj_handle);
		igt_pvr_ioctl_destroy_vm_context(fd, vm_ctx_handle, 0);

		drm_close_driver(fd);
	}
}
