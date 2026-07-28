// SPDX-License-Identifier: GPL-2.0 or MIT
/* Copyright (c) 2026 Imagination Technologies Ltd. All Rights Reserved */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include "igt.h"
#include "igt_pvr.h"

#include "pvr_drm.h"

IGT_TEST_DESCRIPTION("Check that igt/imagination knows about this GPU ID");

static bool
has_known_gpu_id(int fd, const struct drm_pvr_dev_query_gpu_info *gpu_info)
{
#define PVR_BVNC_ID(b, v, n, c) \
	(((uint64_t)(b) << 48) | \
	((uint64_t)(v) << 32) | \
	((uint64_t)(n) << 16) | \
	((uint64_t)(c)))

	switch (gpu_info->gpu_id) {
	case PVR_BVNC_ID(4, 40, 2, 51):
	case PVR_BVNC_ID(33, 15, 11, 3):
	case PVR_BVNC_ID(36, 53, 104, 796):
		break;
	default:
		igt_warn("Unknown GPU ID: 0x%016llx\n", gpu_info->gpu_id);
		return false;
	}
#undef PVR_BVNC_ID

	igt_info("GPU BVNC: %u.%u.%u.%u\n",
		 (unsigned int)(gpu_info->gpu_id >> 48 & 0xffff),
		 (unsigned int)(gpu_info->gpu_id >> 32 & 0xffff),
		 (unsigned int)(gpu_info->gpu_id >> 16 & 0xffff),
		 (unsigned int)(gpu_info->gpu_id & 0xffff));

	return true;
}

int igt_simple_main()
{
	int fd = drm_open_driver(DRIVER_POWERVR);
	struct drm_pvr_dev_query_gpu_info gpu_info = {0};

	igt_pvr_ioctl_dev_query(fd, DRM_PVR_DEV_QUERY_GPU_INFO_GET,
				sizeof(gpu_info), &gpu_info, 0);

	igt_assert(has_known_gpu_id(fd, &gpu_info));
	igt_assert_neq(gpu_info.num_phantoms, 0);
}
