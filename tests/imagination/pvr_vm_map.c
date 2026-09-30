// SPDX-License-Identifier: GPL-2.0 or MIT
/* Copyright (c) 2026 Gyeyoung Baek */

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#include "igt.h"
#include "igt_os.h"
#include "igt_pvr.h"
#include "igt_sizes.h"

IGT_TEST_DESCRIPTION("Test that VM map, unmap and remap operations work");

/*
 * vm_map/unmap/remap() are each run on a range of RANGE_PAGES pages (p0..p3),
 * placed in one of three ways. The drawings are not to scale.
 *
 * in-2m       Inside one 2MiB region. No boundary is crossed.
 *
 *             |<------------ 2MiB region ----------->|
 *             |           [p0|p1|p2|p3]              |
 *
 * cross-2m    Across a 2MiB boundary.
 *
 *             |<--- 2MiB region A --->|<--- 2MiB region B --->|
 *                            [p0|p1]|[p2|p3]
 *                                    ^ 2MiB boundary
 *
 * cross-1g    Across a 1GiB boundary, which is also a 2MiB boundary.
 *
 *             |<--- 1GiB region A --->|<--- 1GiB region B --->|
 *                            [p0|p1]|[p2|p3]
 *                                    ^ 1GiB boundary
 */

#define RANGE_PAGES 4
#define LEAK_BO_SIZE SZ_32M
#define LEAK_THRESHOLD SZ_8M

static uint64_t align_up(uint64_t value, uint64_t align)
{
	return (value + align - 1) & ~(align - 1);
}

static void assert_shmem_released(uint64_t baseline)
{
	uint64_t now = 0;

	for (int i = 0; i < 20; i++) {
		now = igt_get_meminfo("Shmem");
		if (now < baseline + LEAK_THRESHOLD)
			return;
		usleep(100 * 1000);
	}

	igt_assert_f(false, "%" PRIu64 " bytes of shmem still held after close\n", now - baseline);
}

static void map_range(int fd, uint64_t addr, uint64_t size)
{
	const uint64_t baseline = igt_get_meminfo("Shmem");
	const uint32_t vm_ctx = igt_pvr_ioctl_create_vm_context(fd, 0);
	size_t bo_size = LEAK_BO_SIZE;
	const uint32_t handle = igt_pvr_ioctl_create_bo(fd, &bo_size);

	igt_pvr_ioctl_vm_map(fd, vm_ctx, handle, addr, 0, size);
	igt_pvr_ioctl_vm_unmap(fd, vm_ctx, addr, size);

	gem_close(fd, handle);
	igt_pvr_ioctl_destroy_vm_context(fd, vm_ctx, 0);

	assert_shmem_released(baseline);
}

/*
 * A new mapping is mapped over an old one. The overlaps follow "DOC: Split and Merge" in
 * drivers/gpu/drm/drm_gpuvm.c: replace is case 14, remap-next 5, remap-prev 7, remap-both 11.
 */
static void remap_range(int fd, uint64_t old_addr, uint64_t old_size,
			uint64_t new_addr, uint64_t new_size)
{
	const uint64_t baseline = igt_get_meminfo("Shmem");
	const uint32_t vm_ctx = igt_pvr_ioctl_create_vm_context(fd, 0);
	const uint64_t old_end = old_addr + old_size;
	const uint64_t new_end = new_addr + new_size;
	size_t size_a = LEAK_BO_SIZE;
	size_t size_b = new_size;
	const uint32_t handle_a = igt_pvr_ioctl_create_bo(fd, &size_a);
	const uint32_t handle_b = igt_pvr_ioctl_create_bo(fd, &size_b);

	igt_pvr_ioctl_vm_map(fd, vm_ctx, handle_a, old_addr, 0, old_size);
	igt_pvr_ioctl_vm_map(fd, vm_ctx, handle_b, new_addr, 0, new_size);

	if (new_addr > old_addr)
		igt_pvr_ioctl_vm_unmap(fd, vm_ctx, old_addr, new_addr - old_addr);
	igt_pvr_ioctl_vm_unmap(fd, vm_ctx, new_addr, new_size);
	if (new_end < old_end)
		igt_pvr_ioctl_vm_unmap(fd, vm_ctx, new_end, old_end - new_end);

	gem_close(fd, handle_a);
	gem_close(fd, handle_b);
	igt_pvr_ioctl_destroy_vm_context(fd, vm_ctx, 0);

	assert_shmem_released(baseline);
}

struct place {
	const char *name;
	uint64_t addr;
};

int igt_main()
{
	struct place places[] = { { "in-2m", 0 }, { "cross-2m", 0 }, { "cross-1g", 0 } };
	struct drm_pvr_heap heap;
	uint64_t size;
	int fd;

	igt_fixture()
	{
		fd = drm_open_driver(DRIVER_POWERVR);
		heap = igt_pvr_find_general_heap(fd);

		size = RANGE_PAGES * sysconf(_SC_PAGESIZE);
		places[0].addr = align_up(heap.base, SZ_2M) + SZ_1M;
		places[1].addr = align_up(heap.base + 1, SZ_2M) - size / 2;
		places[2].addr = align_up(heap.base + 1, SZ_1G) - size / 2;

		igt_require(places[2].addr + size <= heap.base + heap.size);
	}

	for (size_t i = 0; i < ARRAY_SIZE(places); i++) {
		const char *name = places[i].name;
		const uint64_t addr = places[i].addr;

		igt_describe_f("A range can be mapped and unmapped, and the page tables it needs are available (%s)",
			       name);
		igt_subtest_f("map-%s", name)
		{
			/*
			 * map         |nnnnnnnn|    a range is mapped, then unmapped
			 */
			map_range(fd, addr, size);
		}

		igt_describe_f("A mapping that covers an old one replaces it and leaves nothing of it behind (%s)",
			       name);
		igt_subtest_f("replace-%s", name)
		{
			/*
			 * replace     |  oooo  |    o = old mapping, n = new mapping
			 *             |nnnnnnnn|    nothing is left of the old one
			 */
			remap_range(fd, addr + size / 4, size / 2, addr, size);
		}

		igt_describe_f("Mapping over the left part of a mapping leaves its right part, and the original "
			       "buffer object is released once everything is closed (%s)", name);
		igt_subtest_f("remap-next-%s", name)
		{
			/*
			 * remap-next  |oooooooo|    the right part of the old mapping is left
			 *             |nnnn    |
			 */
			remap_range(fd, addr, size, addr, size / 2);
		}

		igt_describe_f("Mapping over the right part of a mapping leaves its left part, and the original "
			       "buffer object is released once everything is closed (%s)", name);
		igt_subtest_f("remap-prev-%s", name)
		{
			/*
			 * remap-prev  |oooooooo|    the left part of the old mapping is left
			 *             |    nnnn|
			 */
			remap_range(fd, addr, size, addr + size / 2, size / 2);
		}

		igt_describe_f("Mapping over the middle of a mapping leaves its left and right parts, and the "
			       "original buffer object is released once everything is closed (%s)",
			       name);
		igt_subtest_f("remap-both-%s", name)
		{
			/*
			 * remap-both  |oooooooo|    left and right parts of the old one are left
			 *             |   nn   |
			 */
			remap_range(fd, addr, size, addr + size / 2, size / 4);
		}
	}
}
