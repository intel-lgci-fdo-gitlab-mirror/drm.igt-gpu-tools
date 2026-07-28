// SPDX-License-Identifier: GPL-2.0 or MIT
/* Copyright (c) 2026 Imagination Technologies Ltd. All Rights Reserved */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include "igt.h"
#include "igt_pvr.h"

#include "pvr_drm.h"

#define INVALID_ADDRESS 0xdeadbee0
#define INVALID_HANDLE 0

static void build_create_args(struct drm_pvr_ioctl_create_free_list_args *create_free_list_args,
			      uint32_t vm_ctx_handle,
			      uint64_t gpu_addr)
{
	memset(create_free_list_args, 0, sizeof(*create_free_list_args));
	create_free_list_args->free_list_gpu_addr = gpu_addr;
	create_free_list_args->initial_num_pages = 32;
	create_free_list_args->max_num_pages = 256;
	create_free_list_args->grow_num_pages = 16;
	create_free_list_args->grow_threshold = 50;
	create_free_list_args->vm_context_handle = vm_ctx_handle;
}

static void build_destroy_args(struct drm_pvr_ioctl_destroy_free_list_args *destroy_free_list_args,
			       uint32_t handle)
{
	memset(destroy_free_list_args, 0, sizeof(*destroy_free_list_args));
	destroy_free_list_args->handle = handle;
}

int igt_main()
{
	struct drm_pvr_ioctl_create_free_list_args create_free_list_args;
	struct drm_pvr_ioctl_destroy_free_list_args destroy_free_list_args;
	uint64_t gem_obj_gpu_addr;
	uint32_t gem_obj_handle;
	uint32_t vm_ctx_handle;
	size_t size = 4096;
	int fd;

	igt_fixture()
	{
		struct drm_pvr_heap general_heap_info;

		fd = drm_open_driver(DRIVER_POWERVR);
		vm_ctx_handle = igt_pvr_ioctl_create_vm_context(fd, 0);
		gem_obj_handle =
			igt_pvr_ioctl_create_bo_ex(fd, &size,
						   DRM_PVR_BO_PM_FW_PROTECT);
		general_heap_info = igt_pvr_find_general_heap(fd);
		gem_obj_gpu_addr = general_heap_info.base;

		igt_pvr_ioctl_vm_map(fd, vm_ctx_handle, gem_obj_handle,
				     gem_obj_gpu_addr, 0, size);
	}

	igt_describe("Test valid free list creation and destruction");
	igt_subtest("create-free-list")
	{
		build_create_args(&create_free_list_args,
				  vm_ctx_handle, gem_obj_gpu_addr);
		do_ioctl(fd, DRM_IOCTL_PVR_CREATE_FREE_LIST,
			 &create_free_list_args);

		igt_assert_neq(create_free_list_args.handle, 0);

		build_destroy_args(&destroy_free_list_args,
				   create_free_list_args.handle);
		do_ioctl(fd, DRM_IOCTL_PVR_DESTROY_FREE_LIST,
			 &destroy_free_list_args);
	}

	igt_describe("Test free list destruction with bad padding");
	igt_subtest("destroy-free-list-bad-padding")
	{
		build_create_args(&create_free_list_args,
				  vm_ctx_handle, gem_obj_gpu_addr);
		do_ioctl(fd, DRM_IOCTL_PVR_CREATE_FREE_LIST,
			 &create_free_list_args);

		igt_assert_neq(create_free_list_args.handle, 0);

		build_destroy_args(&destroy_free_list_args,
				   create_free_list_args.handle);
		destroy_free_list_args._padding_4 = 1;
		do_ioctl_err(fd, DRM_IOCTL_PVR_DESTROY_FREE_LIST,
			     &destroy_free_list_args, EINVAL);

		destroy_free_list_args._padding_4 = 0;
		do_ioctl(fd, DRM_IOCTL_PVR_DESTROY_FREE_LIST,
			 &destroy_free_list_args);
	}

	igt_describe("Test creation and destruction of multiple free lists");
	igt_subtest("create-multiple-free-lists")
	{
		uint32_t free_list_handles[2];

		build_create_args(&create_free_list_args,
				  vm_ctx_handle, gem_obj_gpu_addr);

		do_ioctl(fd, DRM_IOCTL_PVR_CREATE_FREE_LIST,
			 &create_free_list_args);
		free_list_handles[0] = create_free_list_args.handle;
		do_ioctl(fd, DRM_IOCTL_PVR_CREATE_FREE_LIST,
			 &create_free_list_args);
		free_list_handles[1] = create_free_list_args.handle;

		igt_assert_neq(free_list_handles[0], 0);
		igt_assert_neq(free_list_handles[1], 0);
		igt_assert_neq(free_list_handles[0], free_list_handles[1]);

		build_destroy_args(&destroy_free_list_args,
				   free_list_handles[1]);
		do_ioctl(fd, DRM_IOCTL_PVR_DESTROY_FREE_LIST,
			 &destroy_free_list_args);
		build_destroy_args(&destroy_free_list_args,
				   free_list_handles[0]);
		do_ioctl(fd, DRM_IOCTL_PVR_DESTROY_FREE_LIST,
			 &destroy_free_list_args);
	}

	igt_describe("Test free list creation with invalid object address");
	igt_subtest("invalid-free-list-object-addr")
	{
		build_create_args(&create_free_list_args,
				  vm_ctx_handle, 0);
		do_ioctl_err(fd, DRM_IOCTL_PVR_CREATE_FREE_LIST,
			     &create_free_list_args, EINVAL);
	}

	igt_describe("Test free list creation with initial pages greater than max pages");
	igt_subtest("initial-pages-greater-than-max")
	{
		build_create_args(&create_free_list_args,
				  vm_ctx_handle, gem_obj_gpu_addr);
		create_free_list_args.initial_num_pages =
			create_free_list_args.max_num_pages + 1;
		do_ioctl_err(fd, DRM_IOCTL_PVR_CREATE_FREE_LIST,
			     &create_free_list_args, EINVAL);
	}

	igt_describe("Test free list creation with grow pages greater than max pages");
	igt_subtest("grow-pages-greater-than-max")
	{
		build_create_args(&create_free_list_args,
				  vm_ctx_handle, gem_obj_gpu_addr);
		create_free_list_args.grow_num_pages =
			create_free_list_args.max_num_pages + 1;
		do_ioctl_err(fd, DRM_IOCTL_PVR_CREATE_FREE_LIST,
			     &create_free_list_args, EINVAL);
	}

	igt_describe("Test free list creation with invalid grow threshold");
	igt_subtest("invalid-grow-threshold")
	{
		build_create_args(&create_free_list_args,
				  vm_ctx_handle, gem_obj_gpu_addr);
		create_free_list_args.grow_threshold = 101;
		do_ioctl_err(fd, DRM_IOCTL_PVR_CREATE_FREE_LIST,
			     &create_free_list_args, EINVAL);
	}

	igt_describe("Test free list creation with zero max pages");
	igt_subtest("zero-max-pages")
	{
		build_create_args(&create_free_list_args,
				  vm_ctx_handle, gem_obj_gpu_addr);
		create_free_list_args.initial_num_pages = 0;
		create_free_list_args.max_num_pages = 0;
		create_free_list_args.grow_num_pages = 0;
		do_ioctl_err(fd, DRM_IOCTL_PVR_CREATE_FREE_LIST,
			     &create_free_list_args, EINVAL);
	}

	igt_describe("Test free list creation with zero grow pages and initial pages less than max pages");
	igt_subtest("zero-grow-pages-initial-less-than-max")
	{
		build_create_args(&create_free_list_args,
				  vm_ctx_handle, gem_obj_gpu_addr);
		create_free_list_args.grow_num_pages = 0;
		do_ioctl_err(fd, DRM_IOCTL_PVR_CREATE_FREE_LIST,
			     &create_free_list_args, EINVAL);
	}

	igt_describe("Test free list creation with zero grow pages and initial pages equal to max pages");
	igt_subtest("zero-grow-pages-initial-equals-max")
	{
		build_create_args(&create_free_list_args,
				  vm_ctx_handle, gem_obj_gpu_addr);
		create_free_list_args.initial_num_pages =
			create_free_list_args.max_num_pages;
		create_free_list_args.grow_num_pages = 0;
		do_ioctl(fd, DRM_IOCTL_PVR_CREATE_FREE_LIST,
			 &create_free_list_args);

		igt_assert_neq(create_free_list_args.handle, 0);

		destroy_free_list_args.handle = create_free_list_args.handle;
		do_ioctl(fd, DRM_IOCTL_PVR_DESTROY_FREE_LIST,
			 &destroy_free_list_args);
	}

	igt_fixture()
	{
		igt_pvr_ioctl_vm_unmap(fd, vm_ctx_handle, gem_obj_gpu_addr, size);
		gem_close(fd, gem_obj_handle);
		igt_pvr_ioctl_destroy_vm_context(fd, vm_ctx_handle, 0);
		drm_close_driver(fd);
	}
}
