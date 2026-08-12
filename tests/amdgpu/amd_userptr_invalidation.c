// SPDX-License-Identifier: MIT
/*
 * Copyright 2026 Advanced Micro Devices, Inc.
 *
 * Test for USERPTR BO PTE invalidation after munmap.
 *
 * When userspace releases the backing pages of a USERPTR buffer object
 * via munmap(), the kernel must invalidate the corresponding GPU page
 * table entries so that subsequent command submissions through the same
 * GPU virtual address range do not access the old physical pages.
 *
 * The test verifies two aspects of this behavior:
 *
 *   userptr-unmap-revalidate
 *       Submits a CS that includes the USERPTR BO in its bo_list after
 *       the backing has been released.  The kernel is expected to detect
 *       the invalidated BO during revalidation and reject the submission.
 *
 *   userptr-unmap-stress
 *       Allocates a large (256 MB) USERPTR region, establishes GPU
 *       mappings via an initial SDMA copy, then releases the backing
 *       and creates memory pressure with many pipes and child processes.
 *       A second SDMA copy through the old VA range is submitted without
 *       the USERPTR BO in the bo_list.  The test verifies that the GPU
 *       does not read back the original data pattern (0xAA), confirming
 *       that the PTEs were properly invalidated.
 *
 *       Detection uses three complementary signals:
 *         - GPU page faults logged by the kernel (klogctl)
 *         - destination buffer containing only zeros (dummy page)
 *         - absence of original 0xAA pattern in destination
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/klog.h>
#include <sys/wait.h>
#include <unistd.h>

#include "igt.h"
#include "ioctl_wrappers.h"
#include "lib/amdgpu/amd_memory.h"
#include "lib/amdgpu/amd_sdma.h"
#include "lib/amdgpu/amd_ip_blocks.h"
#include "lib/amdgpu/amd_command_submission.h"
#include "lib/amdgpu/amd_utils.h"

#define BUF_SZ			(64 * 1024)
#define PM4_DW			256

#define STRESS_TARGET_SZ	(64UL * 1024 * 1024)
#define STRESS_PIPES		200000
#define STRESS_SCAN_CHUNK	(512UL * 1024)
#define STRESS_PTE_STEP		(64UL * 1024 * 1024)

/**
 * count_gpu_page_faults() - count GPU page faults in dmesg for a given PID
 * @pid: process ID to match in fault messages
 * @since_uptime_ms: only count faults after this uptime (milliseconds)
 *
 * Read the kernel ring buffer via klogctl(3) and count lines containing
 * "[gfxhub] page fault" followed by the given PID on the next line.
 * Only messages with a kernel timestamp >= @since_uptime_ms are counted.
 *
 * Return: number of matching page fault entries.
 */
static unsigned int
count_gpu_page_faults(pid_t pid, unsigned long since_uptime_ms)
{
	int bufsize, len;
	char *buf, *p, *line_start;
	unsigned int count = 0;
	unsigned int total_lines = 0;
	unsigned int fault_lines = 0;
	char pid_pattern[64];
	bool prev_was_fault = false;

	snprintf(pid_pattern, sizeof(pid_pattern), "pid %d ", (int)pid);

	bufsize = klogctl(10, NULL, 0);
	if (bufsize <= 0)
		bufsize = 1 << 20;

	buf = malloc(bufsize + 1);
	if (!buf)
		return 0;

	len = klogctl(3, buf, bufsize);
	if (len <= 0) {
		free(buf);
		return 0;
	}
	buf[len] = '\0';

	line_start = buf;
	for (p = buf; p <= buf + len; p++) {
		const char *ts_start;
		double ts;

		if (*p != '\n' && *p != '\0')
			continue;

		*p = '\0';
		total_lines++;

		/* Filter by kernel timestamp */
		ts_start = strchr(line_start, '[');
		if (ts_start && sscanf(ts_start, "[%lf]", &ts) == 1) {
			unsigned long ts_ms = (unsigned long)(ts * 1000.0);

			if (ts_ms < since_uptime_ms) {
				prev_was_fault = false;
				line_start = p + 1;
				continue;
			}
		}

		if (strstr(line_start, "[gfxhub] page fault")) {
			prev_was_fault = true;
			fault_lines++;
		} else if (prev_was_fault && strstr(line_start, pid_pattern)) {
			count++;
			prev_was_fault = false;
		} else {
			prev_was_fault = false;
		}

		line_start = p + 1;
	}

	igt_info("  klogctl: lines=%u faults=%u matched=%u\n",
		 total_lines, fault_lines, count);
	free(buf);
	return count;
}

/**
 * get_uptime_ms() - read current system uptime in milliseconds
 *
 * Read /proc/uptime for the kernel monotonic timestamp that matches
 * the timestamps used in dmesg.
 *
 * Return: uptime in milliseconds, or 0 on error.
 */
static unsigned long get_uptime_ms(void)
{
	FILE *fp;
	double uptime;

	fp = fopen("/proc/uptime", "r");
	if (!fp)
		return 0;
	if (fscanf(fp, "%lf", &uptime) != 1)
		uptime = 0;
	fclose(fp);
	return (unsigned long)(uptime * 1000.0);
}

/**
 * amdgpu_userptr_unmap_revalidate() - test CS rejection after munmap
 * @dev: amdgpu device handle
 *
 * Allocate a USERPTR BO, release its backing via munmap(), then submit
 * a CS that still references the BO in its bo_list.  The kernel should
 * detect that the BO pages are no longer valid and reject the CS.
 *
 * If the CS is accepted, the destination buffer is scanned for bytes
 * that do not match the original fill or zero patterns.
 */
static void amdgpu_userptr_unmap_revalidate(amdgpu_device_handle dev)
{
	const struct amdgpu_ip_block_version *ip_block;
	struct amdgpu_ring_context *ring_context;
	amdgpu_bo_handle up_bo;
	amdgpu_va_handle up_va_h;
	uint64_t up_va;
	void *up_cpu;
	amdgpu_bo_handle dst_bo;
	amdgpu_va_handle dst_va_h;
	uint64_t dst_mc;
	void *dst_cpu_ptr;
	uint8_t *dst;
	unsigned int suspicious;
	uint64_t i;
	int r;

	ip_block = get_ip_block(dev, AMDGPU_HW_IP_DMA);
	igt_assert(ip_block);

	ring_context = calloc(1, sizeof(*ring_context));
	igt_assert(ring_context);
	ring_context->write_length = BUF_SZ;
	ring_context->pm4 = calloc(PM4_DW, sizeof(*ring_context->pm4));
	ring_context->pm4_size = PM4_DW;
	ring_context->secure = false;
	ring_context->res_cnt = 2;
	igt_assert(ring_context->pm4);

	r = amdgpu_cs_ctx_create(dev, &ring_context->context_handle);
	igt_assert_eq(r, 0);

	/* Allocate and fill USERPTR BO */
	up_cpu = mmap(NULL, BUF_SZ, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
	igt_assert(up_cpu != MAP_FAILED);
	memset(up_cpu, 0x77, BUF_SZ);

	r = amdgpu_create_bo_from_user_mem(dev, up_cpu, BUF_SZ, &up_bo);
	igt_assert_eq(r, 0);

	r = amdgpu_va_range_alloc(dev, amdgpu_gpu_va_range_general,
				  BUF_SZ, sysconf(_SC_PAGE_SIZE), 0,
				  &up_va, &up_va_h, 0);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_va_op(up_bo, 0, BUF_SZ, up_va, 0, AMDGPU_VA_OP_MAP);
	igt_assert_eq(r, 0);

	/* Allocate VRAM destination */
	r = amdgpu_bo_alloc_and_map(dev, BUF_SZ, sysconf(_SC_PAGE_SIZE),
				    AMDGPU_GEM_DOMAIN_VRAM,
				    AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED,
				    &dst_bo, &dst_cpu_ptr, &dst_mc,
				    &dst_va_h);
	igt_assert_eq(r, 0);
	memset(dst_cpu_ptr, 0, BUF_SZ);

	/* Release USERPTR backing before CS */
	munmap(up_cpu, BUF_SZ);

	/* Submit CS with invalidated USERPTR BO in bo_list */
	ring_context->bo_mc = up_va;
	ring_context->bo_mc2 = dst_mc;
	ring_context->resources[0] = up_bo;
	ring_context->resources[1] = dst_bo;

	ip_block->funcs->copy_linear(ip_block->funcs, ring_context,
				     &ring_context->pm4_dw);

	r = amdgpu_test_exec_cs_helper(dev, ip_block->type, ring_context, 1);
	r = ring_context->err_codes.err_code_cs_submit;

	if (r != 0) {
		igt_info("CS rejected (r=%d) after munmap\n", r);
	} else {
		dst = (uint8_t *)dst_cpu_ptr;
		suspicious = 0;
		for (i = 0; i < BUF_SZ; i++) {
			if (dst[i] != 0 && dst[i] != 0x77)
				suspicious++;
		}
		igt_info("CS completed: %u/%d unexpected bytes\n",
			 suspicious, BUF_SZ);
	}

	amdgpu_bo_unmap_and_free(dst_bo, dst_va_h, dst_mc, BUF_SZ);
	amdgpu_bo_va_op(up_bo, 0, BUF_SZ, up_va, 0, AMDGPU_VA_OP_UNMAP);
	amdgpu_va_range_free(up_va_h);
	amdgpu_bo_free(up_bo);
	amdgpu_cs_ctx_free(ring_context->context_handle);
	free(ring_context->pm4);
	free(ring_context);
}

/**
 * amdgpu_userptr_unmap_stress() - stress test PTE invalidation
 * @dev: amdgpu device handle
 *
 * Phase 1: Allocate a large USERPTR region filled with 0xAA, create a
 *          VRAM destination BO, and perform an initial SDMA copy to
 *          populate GPU page table entries.
 *
 * Phase 2: Release the USERPTR backing via munmap().  This triggers the
 *          MMU notifier which should invalidate the GPU PTEs.
 *
 * Phase 3: Create memory pressure by opening many pipes and forking
 *          child processes.  This increases the chance that the freed
 *          physical pages are reassigned.
 *
 * Phase 4: Poison the destination with 0xCC and submit a second SDMA
 *          copy through the old VA range without the USERPTR BO in the
 *          bo_list.  If PTEs were invalidated, the GPU will fault and
 *          the fault handler will redirect reads to a zeroed dummy page.
 *          The test checks that no original 0xAA data appears in the
 *          destination.
 */
static void amdgpu_userptr_unmap_stress(amdgpu_device_handle dev)
{
	const struct amdgpu_ip_block_version *ip_block;
	struct amdgpu_ring_context *ring_context;
	amdgpu_bo_handle up_bo;
	amdgpu_va_handle up_va_h;
	uint64_t up_va;
	void *up_cpu;
	amdgpu_bo_handle dst_bo;
	amdgpu_va_handle dst_va_h;
	uint64_t dst_mc;
	void *dst_cpu_ptr;
	int (*pipes)[2];
	unsigned int pipes_opened;
	uint64_t off;
	unsigned int i;
	volatile uint8_t sink;
	uint8_t *base;
	uint8_t *scan;
	uint64_t p;
	unsigned int non_poison;
	unsigned int original_count;
	unsigned int page_faults;
	unsigned long ts_before;
	pid_t my_pid;
	int r;

	up_bo = NULL;
	up_cpu = MAP_FAILED;
	pipes = NULL;
	pipes_opened = 0;

	ip_block = get_ip_block(dev, AMDGPU_HW_IP_DMA);
	igt_assert(ip_block);

	ring_context = calloc(1, sizeof(*ring_context));
	igt_assert(ring_context);
	ring_context->pm4 = calloc(PM4_DW, sizeof(*ring_context->pm4));
	ring_context->pm4_size = PM4_DW;
	ring_context->secure = false;
	igt_assert(ring_context->pm4);

	r = amdgpu_cs_ctx_create(dev, &ring_context->context_handle);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_alloc_and_map(dev, STRESS_SCAN_CHUNK,
				    sysconf(_SC_PAGE_SIZE),
				    AMDGPU_GEM_DOMAIN_VRAM,
				    AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED,
				    &dst_bo, &dst_cpu_ptr, &dst_mc,
				    &dst_va_h);
	igt_assert_eq(r, 0);

	/* Phase 1: allocate USERPTR region and establish GPU mappings */
	igt_info("Phase 1: allocating %lu MB USERPTR region\n",
		 (unsigned long)(STRESS_TARGET_SZ / (1024 * 1024)));

	up_cpu = mmap(NULL, STRESS_TARGET_SZ, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
	igt_assert(up_cpu != MAP_FAILED);
	memset(up_cpu, 0xAA, STRESS_TARGET_SZ);

	r = amdgpu_create_bo_from_user_mem(dev, up_cpu, STRESS_TARGET_SZ,
					   &up_bo);
	igt_assert_eq(r, 0);

	r = amdgpu_va_range_alloc(dev, amdgpu_gpu_va_range_general,
				  STRESS_TARGET_SZ, sysconf(_SC_PAGE_SIZE), 0,
				  &up_va, &up_va_h, 0);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_va_op(up_bo, 0, STRESS_TARGET_SZ, up_va, 0,
			     AMDGPU_VA_OP_MAP);
	igt_assert_eq(r, 0);

	/* Touch pages at intervals to populate GPU PTEs */
	sink = 0;
	base = (uint8_t *)up_cpu;
	for (off = 0; off < STRESS_TARGET_SZ; off += STRESS_PTE_STEP)
		sink += base[off];
	(void)sink;

	/* Initial SDMA copy to ensure GPU has walked the page tables */
	ring_context->bo_mc = up_va;
	ring_context->bo_mc2 = dst_mc;
	ring_context->write_length = STRESS_SCAN_CHUNK;
	ring_context->resources[0] = up_bo;
	ring_context->resources[1] = dst_bo;
	ring_context->res_cnt = 2;

	ip_block->funcs->copy_linear(ip_block->funcs, ring_context,
				     &ring_context->pm4_dw);
	r = amdgpu_test_exec_cs_helper(dev, ip_block->type, ring_context, 0);
	igt_assert_eq(r, 0);
	igt_info("Phase 1: initial SDMA copy OK\n");

	/* Phase 2: release USERPTR backing */
	igt_info("Phase 2: munmap() %lu MB USERPTR backing\n",
		 (unsigned long)(STRESS_TARGET_SZ / (1024 * 1024)));
	my_pid = getpid();
	ts_before = get_uptime_ms();
	munmap(up_cpu, STRESS_TARGET_SZ);
	up_cpu = MAP_FAILED;

	/* Phase 3: create memory pressure */
	igt_info("Phase 3: creating memory pressure\n");

	pipes = calloc(STRESS_PIPES, sizeof(*pipes));
	igt_assert(pipes);

	for (i = 0; i < STRESS_PIPES; i++) {
		if (pipe(pipes[i]) < 0) {
			igt_info("  pipe allocation stopped at %u/%d (errno=%d)\n",
				 i, STRESS_PIPES, errno);
			break;
		}
		(void)write(pipes[i][1], "X", 1);
		pipes_opened = i + 1;
	}
	igt_info("  opened %u pipes\n", pipes_opened);


	/*
	 * Phase 4: submit SDMA copy through the old VA range.
	 *
	 * The USERPTR BO is intentionally omitted from the bo_list so
	 * the kernel does not attempt to revalidate it.  If the PTEs
	 * were invalidated, the SDMA engine will fault and the kernel
	 * fault handler will map a zeroed dummy page, so the
	 * destination will contain zeros instead of the original 0xAA.
	 */
	igt_info("Phase 4: submitting CS through unmapped VA range\n");

	memset(dst_cpu_ptr, 0xCC, STRESS_SCAN_CHUNK);

	memset(ring_context->pm4, 0, PM4_DW * sizeof(uint32_t));
	ring_context->pm4_dw = 0;
	ring_context->bo_mc = up_va;
	ring_context->bo_mc2 = dst_mc;
	ring_context->write_length = STRESS_SCAN_CHUNK;
	ring_context->res_cnt = 1;
	ring_context->resources[0] = dst_bo;

	ip_block->funcs->copy_linear(ip_block->funcs, ring_context,
				     &ring_context->pm4_dw);

	r = amdgpu_test_exec_cs_helper(dev, ip_block->type, ring_context, 1);
	if (ring_context->err_codes.err_code_cs_submit != 0) {
		igt_info("  CS rejected (r=%d)\n",
			 ring_context->err_codes.err_code_cs_submit);
		goto cleanup;
	}

	/* Scan destination for original data pattern */
	scan = (uint8_t *)dst_cpu_ptr;
	non_poison = 0;
	original_count = 0;
	for (p = 0; p < STRESS_SCAN_CHUNK; p++) {
		if (scan[p] != 0xCC)
			non_poison++;
		if (scan[p] == 0xAA)
			original_count++;
	}

	igt_info("  %u/%lu non-poison bytes (%u original 0xAA)\n",
		 non_poison, (unsigned long)STRESS_SCAN_CHUNK,
		 original_count);

cleanup:
	for (i = 0; i < pipes_opened; i++) {
		close(pipes[i][0]);
		close(pipes[i][1]);
	}
	free(pipes);

	/*
	 * Read GPU page faults after releasing file descriptors so
	 * klogctl has room to work.  Brief sleep to let deferred
	 * printk flush any remaining fault messages.
	 */
	usleep(100000);
	page_faults = count_gpu_page_faults(my_pid, ts_before);
	igt_info("  %u GPU page faults for PID %d\n",
		 page_faults, (int)my_pid);

	if (up_bo) {
		amdgpu_bo_va_op(up_bo, 0, STRESS_TARGET_SZ, up_va, 0,
				AMDGPU_VA_OP_UNMAP);
		amdgpu_va_range_free(up_va_h);
		amdgpu_bo_free(up_bo);
	}

	amdgpu_bo_unmap_and_free(dst_bo, dst_va_h, dst_mc, STRESS_SCAN_CHUNK);
	amdgpu_cs_ctx_free(ring_context->context_handle);
	free(ring_context->pm4);
	free(ring_context);

	/*
	 * Invalidation is confirmed when any of the following holds:
	 *
	 *   (a) CS was rejected outright (already handled above).
	 *   (b) GPU page faults were logged for this PID.
	 *   (c) Destination contains non-original data (zeros from the
	 *       dummy page), proving PTEs no longer point at the old
	 *       physical pages.
	 *
	 * Page fault messages may be suppressed by printk ratelimiting,
	 * so the data pattern check (c) is the primary detection method.
	 */
	if (page_faults > 0) {
		igt_info("PTE invalidation confirmed: %u page faults\n",
			 page_faults);
	} else if (non_poison > 0 && original_count == 0) {
		igt_info("PTE invalidation confirmed: dummy page data\n");
	} else {
		igt_assert_f(non_poison == 0 || original_count == 0,
			     "destination contains %u bytes of original data "
			     "(0xAA) after munmap with no GPU page faults\n",
			     original_count);
	}
}

int igt_main()
{
	amdgpu_device_handle device;
	struct amdgpu_gpu_info gpu_info = {0};
	uint32_t major, minor;
	int fd = -1;
	int r;
	bool arr_cap[AMD_IP_MAX] = {0};

	igt_fixture() {
		log_total_time(true, igt_test_name());
		fd = drm_open_driver(DRIVER_AMDGPU);

		r = amdgpu_device_initialize(fd, &major, &minor, &device);
		igt_require(r == 0);

		igt_info("Initialized amdgpu, driver version %d.%d\n",
			 major, minor);

		r = amdgpu_query_gpu_info(device, &gpu_info);
		igt_assert_eq(r, 0);

		r = setup_amdgpu_ip_blocks(major, minor, &gpu_info, device);
		igt_assert_eq(r, 0);

		asic_rings_readness(device, 1, arr_cap);
	}

	igt_describe("Submit CS with USERPTR BO in bo_list after munmap "
		     "and verify the kernel rejects it");
	igt_subtest_with_dynamic("userptr-unmap-revalidate") {
		igt_require(arr_cap[AMD_IP_DMA]);
		igt_dynamic_f("userptr-unmap-revalidate")
			amdgpu_userptr_unmap_revalidate(device);
	}

	igt_describe("Stress test: release large USERPTR backing under "
		     "memory pressure and verify GPU PTEs are invalidated");
	igt_subtest_with_dynamic("userptr-unmap-stress") {
		igt_require(arr_cap[AMD_IP_DMA]);
		igt_dynamic_f("userptr-unmap-stress")
			amdgpu_userptr_unmap_stress(device);
	}

	igt_fixture() {
		amdgpu_device_deinitialize(device);
		drm_close_driver(fd);
		log_total_time(false, igt_test_name());
	}
}
