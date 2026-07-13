// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include "igt.h"
#include "intel_mocs.h"

struct drm_intel_mocs_index {
	uint8_t uc_index;
	uint8_t wb_index;
	uint8_t displayable_index;
	uint8_t defer_to_pat_index;
};

/**
 * SECTION:intel_mocs
 * @short_description: Intel Memory Object Control State (MOCS) helpers
 * @title: Intel MOCS helpers
 * @include: igt.h
 *
 * Memory Object Control State (MOCS) defines the behaviour of memory accesses
 * beyond the graphics core, including encryption, graphics data types that
 * allow selective flushing of data from outer caches, and controlling
 * cacheability in the outer caches.
 */

static void get_mocs_index(int fd, struct drm_intel_mocs_index *mocs)
{
	uint16_t devid = intel_get_drm_devid(fd);
	unsigned int ip_ver = intel_graphics_ver(devid);

	/*
	 * Gen >= 12 onwards don't have a setting for PTE,
	 * so using I915_MOCS_PTE as mocs index may leads to
	 * some undefined MOCS behavior.
	 * This helper function is providing current UC as well
	 * as WB MOCS index based on platform.
	 */
	if (ip_ver >= IP_VER(20, 0)) {
		mocs->uc_index = 3;
		mocs->wb_index = 4;
		mocs->displayable_index = 1;
		mocs->defer_to_pat_index = 0;
	} else if (IS_METEORLAKE(devid)) {
		mocs->uc_index = 5;
		mocs->wb_index = 1;
		mocs->displayable_index = 14;
	} else if (IS_DG2(devid)) {
		mocs->uc_index = 1;
		mocs->wb_index = 3;
		mocs->displayable_index = 3;
	} else if (IS_DG1(devid)) {
		mocs->uc_index = 1;
		mocs->wb_index = 5;
		mocs->displayable_index = 5;
	} else if (ip_ver >= IP_VER(12, 0)) {
		mocs->uc_index = 3;
		mocs->wb_index = 2;
		mocs->displayable_index = 61;
	} else {
		mocs->uc_index = I915_MOCS_PTE;
		mocs->wb_index = I915_MOCS_CACHED;
		mocs->displayable_index = I915_MOCS_PTE;
	}
}

/**
 * intel_get_wb_mocs_index:
 * @fd: the DRM file descriptor
 *
 * Returns: The MOCS index corresponding to the basic write-back behaviour for
 * the open Intel DRM device.
 */
uint8_t intel_get_wb_mocs_index(int fd)
{
	struct drm_intel_mocs_index mocs;

	get_mocs_index(fd, &mocs);

	return mocs.wb_index;
}

/**
 * intel_get_wb_mocs:
 * @fd: the DRM file descriptor
 *
 * Returns: The full 7-bit MOCS field value for write-back caching, with the
 * 6-bit table index shifted to bits 6:1 and the PXP bit (bit 0) cleared.
 * Use this when assigning to a genxml-generated MOCS field, which encodes
 * both the index and PXP bit in a single 7-bit value.
 */
uint8_t intel_get_wb_mocs(int fd)
{
	return intel_get_wb_mocs_index(fd) << 1;
}

/**
 * intel_get_uc_mocs:
 * @fd: the DRM file descriptor
 *
 * Returns: The full 7-bit MOCS field value for uncached accesses, with the
 * 6-bit table index shifted to bits 6:1 and the PXP bit (bit 0) cleared.
 * Use this when assigning to a genxml-generated MOCS field, which encodes
 * both the index and PXP bit in a single 7-bit value.
 */
uint8_t intel_get_uc_mocs(int fd)
{
	return intel_get_uc_mocs_index(fd) << 1;
}

/**
 * intel_get_uc_mocs_index:
 * @fd: the DRM file descriptor
 *
 * Returns: The MOCS index corresponding to the uncached behaviour for the open
 * Intel DRM device.
 */
uint8_t intel_get_uc_mocs_index(int fd)
{
	struct drm_intel_mocs_index mocs;

	get_mocs_index(fd, &mocs);

	return mocs.uc_index;
}

/**
 * intel_get_displayable_mocs_index:
 * @fd: the DRM file descriptor
 *
 * Returns: The MOCS index corresponding to the uncached displayable behaviour
 * for the open Intel DRM device. This should be used for buffers which may be
 * simultaneously displayed and rendered to.
 */
uint8_t intel_get_displayable_mocs_index(int fd)
{
	struct drm_intel_mocs_index mocs;

	get_mocs_index(fd, &mocs);

	return mocs.displayable_index;
}

/**
 * intel_get_defer_to_pat_mocs_index:
 * @fd: the DRM file descriptor
 *
 * Returns: The MOCS index corresponding to the "defer to PAT" caching
 * behaviour, or asserts if the platform does not support this mode.
 */
uint8_t intel_get_defer_to_pat_mocs_index(int fd)
{
	struct drm_intel_mocs_index mocs;
	uint16_t dev_id = intel_get_drm_devid(fd);

	igt_assert(intel_gen(dev_id) >= 20);

	get_mocs_index(fd, &mocs);

	return mocs.defer_to_pat_index;
}
