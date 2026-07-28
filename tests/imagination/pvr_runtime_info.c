// SPDX-License-Identifier: GPL-2.0 or MIT
/* Copyright (c) 2026 Imagination Technologies Ltd. All Rights Reserved */

#include <stdbool.h>

#include "igt.h"
#include "igt_pvr.h"

#include "pvr_drm.h"

IGT_TEST_DESCRIPTION("Test that the runtime info query returns valid data");

int igt_simple_main()
{
	int fd = drm_open_driver(DRIVER_POWERVR);
	struct drm_pvr_dev_query_runtime_info runtime_info = {0};

	igt_pvr_ioctl_dev_query(fd, DRM_PVR_DEV_QUERY_RUNTIME_INFO_GET,
				sizeof(runtime_info), &runtime_info, 0);

	igt_assert_neq_u64(runtime_info.free_list_min_pages, 0);
	igt_assert_lt_u64(runtime_info.free_list_min_pages,
			  runtime_info.free_list_max_pages);

	igt_assert_neq(runtime_info.common_store_alloc_region_size, 0);
	igt_assert_neq(runtime_info.common_store_partition_space_size, 0);

	igt_assert_neq(runtime_info.max_coeffs, 0);

	igt_assert_neq(runtime_info.cdm_max_local_mem_size_regs, 0);
}
