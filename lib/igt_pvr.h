/* SPDX-License-Identifier: GPL-2.0 or MIT */
/* Copyright (c) 2026 Imagination Technologies Ltd. All Rights Reserved */

#ifndef IGT_PVR_H
#define IGT_PVR_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "pvr_drm.h"

uint32_t igt_pvr_ioctl_create_bo(int fd, size_t *size);
uint32_t igt_pvr_ioctl_create_bo_ex(int fd, size_t *size, uint64_t flags);
off_t igt_pvr_ioctl_get_bo_mmap_offset(int fd, uint32_t handle);

struct drm_pvr_ioctl_dev_query_args
igt_pvr_ioctl_dev_query(int fd, enum drm_pvr_dev_query type, uint64_t size,
			void *pointer, int expect_err);

struct drm_pvr_heap *
igt_pvr_get_heap_info(int fd, uint32_t *array_len_out);
struct drm_pvr_heap igt_pvr_find_general_heap(int fd);

struct drm_pvr_static_data_area *
igt_pvr_get_static_data_areas(int fd, uint32_t *array_len_out);

uint32_t igt_pvr_ioctl_create_vm_context(int fd, int expect_err);
void igt_pvr_ioctl_destroy_vm_context(int fd, uint32_t handle, int expect_err);

void igt_pvr_ioctl_vm_map(int fd, uint32_t vm_ctx_handle, uint32_t handle,
			  uint64_t device_addr, uint64_t offset, uint64_t size);
void igt_pvr_ioctl_vm_unmap(int fd, uint32_t vm_ctx_handle,
			    uint64_t device_addr, uint64_t size);

uint32_t igt_pvr_ioctl_create_free_list(int fd, uint32_t vm_ctx_handle,
					uint64_t gpu_addr);
void igt_pvr_ioctl_destroy_free_list(int fd, uint32_t free_list_handle);

#endif /* IGT_PVR_H */
