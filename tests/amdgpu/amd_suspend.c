// SPDX-License-Identifier: MIT
/*
 * Copyright 2024 Advanced Micro Devices, Inc.
 */


#include <fcntl.h>
#include <unistd.h>

#include "igt.h"
#include "drmtest.h"
#include "lib/igt_amd.h"

#include <amdgpu.h>
#include <amdgpu_drm.h>
#include "igt_sysfs.h"
#include "lib/amdgpu/amd_PM4.h"
#include "lib/amdgpu/amd_ip_blocks.h"
#include "lib/amdgpu/amd_memory.h"

/**
 * TEST: gem exec suspend
 * Description: Exercise simple execbufs runs across various suspend/resume cycles.
 * Feature: cmd_submission, suspend
 * Run type: FULL
 *
 * SUBTEST: basic
 * Description: Check basic functionality without any suspend/resume cycle.
 *
 * SUBTEST: basic-S0
 * Description: Check with suspend-to-idle target state.
 *
 * SUBTEST: basic-S3
 * Description: Check full cycle of suspend-to-mem.
 *
 * SUBTEST: basic-S3-devices
 * Description: Check with suspend-to-mem with devices only.
 *
 * SUBTEST: basic-S4
 * Description: Check full cycle of suspend-to-disk.
 *
 * SUBTEST: basic-S4-devices
 * Description: Check with suspend-to-disk with devices only.
 *
 * SUBTEST: hang-S3
 * Description: Check full cycle of suspend-to-mem with a pending GPU hang.
 *
 * SUBTEST: hang-S4
 * Description: Check full cycle of suspend-to-disk with a pending GPU hang.
 *
 * SUBTEST: power-S0
 * Description: Check power consumption during idle state.
 *
 * SUBTEST: power-S3
 * Description: Check power consumption during suspend-to-mem state.
 */

IGT_TEST_DESCRIPTION("Exercise simple execbufs runs across various suspend/resume cycles.");

#define NOSLEEP 0
#define IDLE 1
#define SUSPEND_DEVICES 2
#define SUSPEND 3
#define HIBERNATE_DEVICES 4
#define HIBERNATE 5

#define HANG (2<<8)

#define RESULT_DWORDS 64

#define suspend_kmsg(fmt, ...) \
	igt_kmsg(KMSG_INFO "amd_suspend/%s: " fmt, \
		 igt_subtest_name() ?: "?", ##__VA_ARGS__)

static void
run_amd_test(int fd, amdgpu_device_handle device, amdgpu_context_handle ctx,
	     unsigned int engine, unsigned int mode, uint32_t region);

static void amdgpu_cs_sync(amdgpu_context_handle context,
			   unsigned int ip_type, int ring, unsigned int seqno)
{
	struct amdgpu_cs_fence fence = {
		.context = context,
		.ip_type = ip_type,
		.ring = ring,
		.fence = seqno,
	};
	uint32_t expired;
	int err;

	err = amdgpu_cs_query_fence_status(&fence, AMDGPU_TIMEOUT_INFINITE,
					   0, &expired);
	igt_assert_eq(err, 0);
}

static void
run_amd_test(int fd, amdgpu_device_handle device, amdgpu_context_handle ctx,
	     unsigned int engine, unsigned int mode, uint32_t region)
{
	amdgpu_bo_handle ib_bo, result_bo;
	amdgpu_bo_handle resources[2];
	amdgpu_bo_list_handle bo_list;
	amdgpu_va_handle ib_va_handle, result_va_handle;
	uint32_t *ib_cpu;
	uint32_t *result_cpu;
	uint64_t ib_mc, result_mc;
	struct amdgpu_cs_ib_info ib_info = {};
	struct amdgpu_cs_request request = {};
	unsigned int i;
	int r;

	suspend_kmsg("enter mode=%u region=%u\n", mode, region);

	r = amdgpu_bo_alloc_and_map(device, 4096, 4096, AMDGPU_GEM_DOMAIN_GTT,
				   AMDGPU_GEM_CREATE_CPU_GTT_USWC, &ib_bo,
				   (void **)&ib_cpu, &ib_mc, &ib_va_handle);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_alloc_and_map(device, RESULT_DWORDS * sizeof(uint32_t), 4096,
				   AMDGPU_GEM_DOMAIN_GTT, AMDGPU_GEM_CREATE_CPU_GTT_USWC,
				   &result_bo, (void **)&result_cpu, &result_mc,
				   &result_va_handle);
	igt_assert_eq(r, 0);
	memset(result_cpu, 0, RESULT_DWORDS * sizeof(uint32_t));

	/* PACKET3_WRITE_DATA: write a recognizable dword[i] == i pattern into
	 * result_bo from the GPU, the direct analog of gem_exec_suspend.c's
	 * MI_STORE_DWORD_IMM + check_bo() loop.
	 */
	ib_cpu[0] = PACKET3(PACKET3_WRITE_DATA, 2 + RESULT_DWORDS);
	ib_cpu[1] = WRITE_DATA_DST_SEL(5) | WR_CONFIRM | WRITE_DATA_CACHE_POLICY(3);
	ib_cpu[2] = lower_32_bits(result_mc);
	ib_cpu[3] = upper_32_bits(result_mc);
	for (i = 0; i < RESULT_DWORDS; i++)
		ib_cpu[4 + i] = i;

	resources[0] = ib_bo;
	resources[1] = result_bo;
	r = amdgpu_bo_list_create(device, 2, resources, NULL, &bo_list);
	igt_assert_eq(r, 0);

	ib_info.ib_mc_address = ib_mc;
	ib_info.size = 4 + RESULT_DWORDS;

	request.ip_type = AMDGPU_HW_IP_GFX;
	request.ring = 0;
	request.number_of_ibs = 1;
	request.ibs = &ib_info;
	request.resources = bo_list;

	suspend_kmsg("submitting write-data command buffer\n");
	r = amdgpu_cs_submit(ctx, 0, &request, 1);
	igt_assert_eq(r, 0);

	amdgpu_cs_sync(ctx, AMDGPU_HW_IP_GFX, 0, request.seq_no);
	suspend_kmsg("write confirmed landed, pre-suspend\n");

	/* mask off HANG (and any other high-bit flag) - hang-S3/hang-S4 pass
	 * SUSPEND|HANG / HIBERNATE|HANG as mode, which must still select the
	 * SUSPEND/HIBERNATE case below.
	 */
	switch (mode & 0xff) {
	case NOSLEEP:
		break;
	case IDLE:
		suspend_kmsg("entering suspend-to-idle (freeze)\n");
		igt_system_suspend_autoresume(SUSPEND_STATE_FREEZE,
					      SUSPEND_TEST_NONE);
		break;
	case SUSPEND_DEVICES:
		suspend_kmsg("entering suspend-to-mem (devices only)\n");
		igt_system_suspend_autoresume(SUSPEND_STATE_MEM,
					      SUSPEND_TEST_DEVICES);
		break;
	case SUSPEND:
		suspend_kmsg("entering suspend-to-mem\n");
		igt_system_suspend_autoresume(SUSPEND_STATE_MEM,
					      SUSPEND_TEST_NONE);
		break;
	case HIBERNATE_DEVICES:
		suspend_kmsg("entering suspend-to-disk (devices only)\n");
		igt_system_suspend_autoresume(SUSPEND_STATE_DISK,
					      SUSPEND_TEST_DEVICES);
		break;
	case HIBERNATE:
		suspend_kmsg("entering suspend-to-disk\n");
		igt_system_suspend_autoresume(SUSPEND_STATE_DISK,
					      SUSPEND_TEST_NONE);
		break;
	}
	if ((mode & 0xff) != NOSLEEP)
		suspend_kmsg("resumed from suspend\n");

	/* TODO: HANG has no amdgpu equivalent wired up yet - intel's version
	 * injects a hang via igt_spin_new() before suspending, and its fixture
	 * calls igt_stop_hang_detector()/igt_allow_hang() (lib/igt_gt.h) so the
	 * injected hang doesn't fail the test. Both of those are i915-only
	 * APIs with no amdgpu port; hang-S3/hang-S4 currently only exercise
	 * the plain suspend/resume cycle above, same as basic-S3/basic-S4.
	 */

	suspend_kmsg("verifying result buffer post-resume\n");
	for (i = 0; i < RESULT_DWORDS; i++)
		igt_assert_eq_u32(result_cpu[i], i);
	suspend_kmsg("PASSED\n");

	amdgpu_bo_list_destroy(bo_list);
	amdgpu_bo_unmap_and_free(result_bo, result_va_handle, result_mc,
				 RESULT_DWORDS * sizeof(uint32_t));
	amdgpu_bo_unmap_and_free(ib_bo, ib_va_handle, ib_mc, 4096);
}

struct battery_sample {
	struct timespec tv;
	uint64_t charge;
};

static bool get_power(int dir, struct battery_sample *s)
{
	return (clock_gettime(CLOCK_REALTIME, &s->tv) == 0 &&
		igt_sysfs_scanf(dir, "charge_now", "%"PRIu64, &s->charge) == 1);
}

static double d_charge(const struct battery_sample *after,
		       const struct battery_sample *before)
{
	return (before->charge - after->charge) * 1e-3; /* mWh */
}

static double d_time(const struct battery_sample *after,
		     const struct battery_sample *before)
{
	return ((after->tv.tv_sec - before->tv.tv_sec) +
		(after->tv.tv_nsec - before->tv.tv_nsec) * 1e-9); /* s */
}

static void power_test(int i915, amdgpu_device_handle device, amdgpu_context_handle ctx,
		       unsigned int engine, unsigned int flags, uint32_t region)
{
	struct battery_sample before, after;
	char *status;
	int dir;

	suspend_kmsg("power_test: enter flags=%u region=%u\n", flags, region);

	dir = open("/sys/class/power_supply/BAT0", O_RDONLY);
	igt_require_f(dir != -1, "/sys/class/power_supply/BAT0 not available\n");

	igt_require_f(get_power(dir, &before),
		      "power test needs reported energy level\n");

	status = igt_sysfs_get(dir, "status");
	igt_require_f(status && strcmp(status, "Discharging") == 0,
		      "power test needs to be on battery, not mains, power\n");
	free(status);

	igt_set_autoresume_delay(5 * 60); /* 5 minutes; longer == more stable */

	igt_assert(get_power(dir, &before));
	suspend_kmsg("power_test: pre-suspend battery sample taken\n");
	run_amd_test(i915, device, ctx, engine, flags, region);
	igt_assert(get_power(dir, &after));
	suspend_kmsg("power_test: post-resume battery sample taken\n");

	igt_set_autoresume_delay(0);

	igt_info("Power consumed while suspended: %.3fmWh\n",
		 d_charge(&after, &before));
	igt_info("Discharge rate while suspended: %.3fmW\n",
		 d_charge(&after, &before) * 3600 / d_time(&after, &before));
	suspend_kmsg("power_test: PASSED\n");
}

int igt_main()
{
	struct test {
		const char *name;
		unsigned int flags;
		void (*fn)(int, amdgpu_device_handle, amdgpu_context_handle,
			   unsigned int, unsigned int, uint32_t);
		const char *describe;
	} *test, tests_all_engines[] = {
		{ "basic", NOSLEEP, run_amd_test, "Check basic functionality without any "
					      "suspend/resume cycle." },
		{ "basic-S0", IDLE, run_amd_test, "Check with suspend-to-idle target state." },
		{ "basic-S3-devices", SUSPEND_DEVICES, run_amd_test, "Check with suspend-to-mem "
								 "with devices only." },
		{ "basic-S3", SUSPEND, run_amd_test, "Check full cycle of suspend-to-mem." },
		{ "basic-S4-devices", HIBERNATE_DEVICES, run_amd_test, "Check with suspend-to-disk "
								   "with devices only." },
		{ "basic-S4", HIBERNATE, run_amd_test, "Check full cycle of suspend-to-disk." },
		{ }
	}, tests_power_hang[] = {
		{ "hang-S3", SUSPEND | HANG, run_amd_test, "Check full cycle of suspend-to-mem with a "
						       "pending GPU hang." },
		{ "hang-S4", HIBERNATE | HANG, run_amd_test, "Check full cycle of suspend-to-disk with "
							 "a pending GPU hang." },
		{ "power-S0", IDLE, power_test, "Check power consumption during idle state." },
		{ "power-S3", SUSPEND, power_test, "Check power consumption during "
						   "suspend-to-mem state." },
		{ }
	};
	amdgpu_device_handle device;
	struct amdgpu_gpu_info gpu_info = {};
	uint32_t major, minor;
	int err;

	amdgpu_context_handle ctx;
	int fd;
	uint32_t region;
	unsigned int engine;

	igt_fixture() {
		fd = drm_open_driver_master(DRIVER_AMDGPU);
		err = amdgpu_device_initialize(fd, &major, &minor, &device);
		igt_require(err == 0);
		igt_info("Initialized amdgpu, driver version %d.%d\n",
			 major, minor);
		err = amdgpu_query_gpu_info(device, &gpu_info);
		igt_assert_eq(err, 0);
		err = amdgpu_cs_ctx_create(device, &ctx);
	}

	region  = 0;
	engine  = 0;

	for (test = tests_all_engines; test->name; test++) {
		igt_describe(test->describe);
		igt_subtest_with_dynamic(test->name)
		igt_dynamic_f("%s", test->name)
			test->fn(fd, device, ctx, engine, test->flags, region);
	}

	for (test = tests_power_hang; test->name; test++) {
		igt_describe(test->describe);
		igt_subtest_with_dynamic(test->name)
		igt_dynamic_f("%s", test->name)
			test->fn(fd, device, ctx, engine, test->flags, region);
	}

	igt_fixture() {
		amdgpu_cs_ctx_free( ctx);
		drm_close_driver(fd);
	}
}
