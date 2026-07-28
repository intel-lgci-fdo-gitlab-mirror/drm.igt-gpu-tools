// SPDX-License-Identifier: GPL-2.0 or MIT
/* Copyright (c) 2026 Imagination Technologies Ltd. All Rights Reserved */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include "igt.h"
#include "igt_pvr.h"

#include "pvr_drm.h"

enum pvr_page_size {
	PVR_PAGE_SIZE_4K = 12,
	PVR_PAGE_SIZE_16K = 14,
	PVR_PAGE_SIZE_64K = 16,
	PVR_PAGE_SIZE_256K = 18,
	PVR_PAGE_SIZE_1M = 20,
	PVR_PAGE_SIZE_2M = 21
};

int igt_main()
{
	int fd;

	igt_fixture()
	{
		fd = drm_open_driver(DRIVER_POWERVR);
	}

	igt_describe("Test getting the size of an array from the kernel");
	igt_subtest("heap-info-get-size")
	{
		/* Check we can get the size of the array kernel side. */
		struct drm_pvr_dev_query_heap_info heap_info_get = {0};

		igt_pvr_ioctl_dev_query(fd, DRM_PVR_DEV_QUERY_HEAP_INFO_GET,
					sizeof(heap_info_get), &heap_info_get, 0);
		igt_assert_neq(heap_info_get.heaps.count, 0);
		igt_assert_neq(heap_info_get.heaps.stride, 0);
	}

	igt_describe("Test copying a single heap info from the kernel");
	igt_subtest("heap-info-copy-single")
	{
		struct drm_pvr_heap *heaps =
			calloc(DRM_PVR_HEAP_COUNT, sizeof(*heaps));

		/* Try copying a single heap. */
		struct drm_pvr_dev_query_heap_info heap_info_get = {
			.heaps = DRM_PVR_OBJ_ARRAY(1, heaps),
		};

		igt_pvr_ioctl_dev_query(fd, DRM_PVR_DEV_QUERY_HEAP_INFO_GET,
					sizeof(heap_info_get), &heap_info_get, 0);

		/* Kernel should stop copying after a single heap. */
		igt_assert_eq(heap_info_get.heaps.count, 1);
		igt_assert_eq(heaps[1].size, 0);

		/* Kernel should respect the user side array stride. */
		igt_assert_eq(heap_info_get.heaps.stride, sizeof(*heaps));
	}

	igt_describe("Test copying all heaps info from the kernel");
	igt_subtest("heap-info-copy-all")
	{
		struct drm_pvr_dev_query_heap_info heap_info_get = {0};
		struct drm_pvr_heap *heaps =
			calloc(DRM_PVR_HEAP_COUNT, sizeof(*heaps));
		uint32_t kernel_array_size = 0;

		igt_pvr_ioctl_dev_query(fd, DRM_PVR_DEV_QUERY_HEAP_INFO_GET,
					sizeof(heap_info_get), &heap_info_get, 0);

		kernel_array_size = heap_info_get.heaps.count;

		heap_info_get.heaps =
			(struct drm_pvr_obj_array)DRM_PVR_OBJ_ARRAY(DRM_PVR_HEAP_COUNT,
								    heaps);

		igt_pvr_ioctl_dev_query(fd, DRM_PVR_DEV_QUERY_HEAP_INFO_GET,
					sizeof(heap_info_get), &heap_info_get, 0);

		igt_assert(heap_info_get.heaps.count == DRM_PVR_HEAP_COUNT ||
			   heap_info_get.heaps.count == kernel_array_size);

		/* Kernel should respect the user side array stride. */
		igt_assert_eq(heap_info_get.heaps.stride, sizeof(*heaps));
	}

	igt_describe("Test validity of all heaps");
	igt_subtest("heap-info-heaps-valid")
	{
		uint32_t heaps_count = 0;
		struct drm_pvr_heap *heaps =
			igt_pvr_get_heap_info(fd, &heaps_count);

		igt_assert(heaps);

		for (int heap_id = 0; heap_id < heaps_count; heap_id++) {
			const uint64_t page_mask =
				(1ull << heaps[heap_id].page_size_log2) - 1;

			/* This heap isn't present for all devices. */
			if (heap_id == (int)DRM_PVR_HEAP_RGNHDR)
				continue;

			igt_assert(heaps[heap_id].page_size_log2 == PVR_PAGE_SIZE_4K ||
				   heaps[heap_id].page_size_log2 == PVR_PAGE_SIZE_16K ||
				   heaps[heap_id].page_size_log2 == PVR_PAGE_SIZE_64K ||
				   heaps[heap_id].page_size_log2 == PVR_PAGE_SIZE_256K ||
				   heaps[heap_id].page_size_log2 == PVR_PAGE_SIZE_1M ||
				   heaps[heap_id].page_size_log2 == PVR_PAGE_SIZE_2M);

			igt_assert_neq_u64(heaps[heap_id].size, 0);
			igt_assert(!(heaps[heap_id].size & page_mask));
			igt_assert(!(heaps[heap_id].base & page_mask));

			igt_assert(heaps[heap_id].flags == 0);
		}

		free(heaps);
	}

	igt_describe("Test validity of static data areas");
	igt_subtest("heap-info-static-data-areas-valid")
	{
		uint32_t heaps_count = 0;
		struct drm_pvr_heap *heaps =
			igt_pvr_get_heap_info(fd, &heaps_count);

		uint32_t sda_count = 0;
		struct drm_pvr_static_data_area *static_data_areas =
			igt_pvr_get_static_data_areas(fd, &sda_count);

		igt_assert(heaps);
		igt_assert(static_data_areas);

		for (uint32_t i = 0; i < sda_count; i++) {
			const uint64_t start_offset = static_data_areas[i].offset;
			const uint64_t end_offset =
				start_offset + static_data_areas[i].size;
			const struct drm_pvr_heap *const heap =
				&heaps[static_data_areas[i].location_heap_id];

			igt_assert_lte(static_data_areas[i].area_usage,
				       DRM_PVR_STATIC_DATA_AREA_YUV_CSC);
			igt_assert_lt(static_data_areas[i].location_heap_id,
				      DRM_PVR_HEAP_COUNT);

			if (!static_data_areas[i].size)
				/* Not present for device. */
				continue;

			/* Ensure reported range is inside the heap. */
			igt_assert_lte_u64(start_offset, heap->size);
			igt_assert_lte_u64(end_offset, heap->size);
		}

		free(heaps);
		free(static_data_areas);
	}

	igt_fixture()
	{
		drm_close_driver(fd);
	}
}
