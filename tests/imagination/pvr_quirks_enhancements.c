// SPDX-License-Identifier: GPL-2.0 or MIT
/* Copyright (c) 2026 Imagination Technologies Ltd. All Rights Reserved */

#include <errno.h>
#include <stdbool.h>
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

	igt_describe("Test copying enhancements from the kernel");
	igt_subtest("enhancements-array-copy")
	{
		struct drm_pvr_dev_query_enhancements enhancements_get = {0};
		uint32_t *enhancements;

		/* Enhancements. */
		igt_pvr_ioctl_dev_query(fd, DRM_PVR_DEV_QUERY_ENHANCEMENTS_GET,
					sizeof(enhancements_get),
					&enhancements_get, 0);
		/*
		 * Supplementary check - alloc one extra space as a watermark
		 * to check the copy does not overflow.  We only need to check
		 * this once for the whole UAPI, as a macro is used to do this.
		 */
		enhancements = calloc(enhancements_get.count + 1,
				      sizeof(*enhancements));
		igt_assert(enhancements);

		/* Set watermark. */
		enhancements[enhancements_get.count] = 0xABCDEFAB;

		enhancements_get.enhancements = to_user_pointer(enhancements);
		igt_pvr_ioctl_dev_query(fd, DRM_PVR_DEV_QUERY_ENHANCEMENTS_GET,
					sizeof(enhancements_get),
					&enhancements_get, 0);

		for (int i = 0; i < enhancements_get.count; i++)
			igt_assert_neq(enhancements[i], 0);

		/* Check the watermark is intact. */
		igt_assert_eq(enhancements[enhancements_get.count], 0xABCDEFAB);
	}

	igt_describe("Test copying quirks from the kernel");
	igt_subtest("quirks-array-copy")
	{
		struct drm_pvr_dev_query_quirks quirks_get = {0};
		uint32_t *quirks;

		igt_pvr_ioctl_dev_query(fd, DRM_PVR_DEV_QUERY_QUIRKS_GET,
					sizeof(quirks_get), &quirks_get, 0);
		quirks = calloc(quirks_get.count, sizeof(*quirks));
		igt_assert(quirks);

		quirks_get.quirks = to_user_pointer(quirks);
		igt_pvr_ioctl_dev_query(fd, DRM_PVR_DEV_QUERY_QUIRKS_GET,
					sizeof(quirks_get),
					&quirks_get, 0);

		for (int i = 0; i < quirks_get.count; i++)
			igt_assert_neq(quirks[i], 0);

		igt_assert_lte(quirks_get.musthave_count, quirks_get.count);
	}

	igt_describe("Test enhancements query with invalid padding");
	igt_subtest("enhancements-bad-padding")
	{
		struct drm_pvr_dev_query_enhancements enhancements_get = {
			._padding_a = 1,
		};

		igt_pvr_ioctl_dev_query(fd, DRM_PVR_DEV_QUERY_ENHANCEMENTS_GET,
					sizeof(enhancements_get),
					&enhancements_get, EINVAL);

		enhancements_get._padding_a = 0;
		enhancements_get._padding_c = 1;

		igt_pvr_ioctl_dev_query(fd, DRM_PVR_DEV_QUERY_ENHANCEMENTS_GET,
					sizeof(enhancements_get),
					&enhancements_get, EINVAL);
	}

	igt_describe("Test quirks query with invalid padding");
	igt_subtest("quirks-bad-padding")
	{
		struct drm_pvr_dev_query_quirks quirks_get = {
			._padding_c = 1,
		};

		igt_pvr_ioctl_dev_query(fd, DRM_PVR_DEV_QUERY_QUIRKS_GET,
					sizeof(quirks_get), &quirks_get, EINVAL);
	}

	igt_fixture()
	{
		drm_close_driver(fd);
	}
}
