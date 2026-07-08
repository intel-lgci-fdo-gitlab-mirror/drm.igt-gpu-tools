// SPDX-License-Identifier: MIT
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <errno.h>

#include "igt.h"
#include "igt_core.h"
#include "igt_msm.h"
#include "msm_drm.h"

static int
__configure_counters(struct msm_device *dev, bool global, unsigned int nr_groups,
		     const char **groups, unsigned int count)
{
	uint32_t countables[count];
	struct drm_msm_perfcntr_group group[nr_groups];
	struct drm_msm_perfcntr_config req = {
		.flags = global ? MSM_PERFCNTR_STREAM : 0,
		.nr_groups = nr_groups,
		.groups = VOID2U64(group),
		.period = global ? NSEC_PER_SEC : 0,
		.bufsz_shift = global ? 10 : 0,
		.group_stride = sizeof(struct drm_msm_perfcntr_group),
	};

	memset(group, 0, sizeof(group));
	/* selecting countable 0 for each counter is fine: */
	memset(countables, 0, sizeof(countables));

	for (unsigned int i = 0; i < nr_groups; i++) {
		strcpy(group[i].group_name, groups[i]);
		group[i].nr_countables = count;
		group[i].countables = global ? VOID2U64(countables) : 0;
	}

	return drmIoctl(dev->fd, DRM_IOCTL_MSM_PERFCNTR_CONFIG, &req);
}

static int
configure_counters(struct msm_device *dev, bool global, unsigned int count)
{
	/* CP group is present on all gens.. SP would be another good candidate */
	const char *groups[] = {"CP"};

	return __configure_counters(dev, global, 1, groups, count);
}

static unsigned
get_available_counters(struct msm_device *dev, bool global)
{
	for (unsigned int i = 0; ; i++) {
		int ret = configure_counters(dev, global, i + 1);

		igt_warn("%u: ret=%d\n", i, ret);
		if (ret < 0)
			return i;
		if (global)
			close(ret);
	}
}

int igt_main(void)
{
	/* device instance for global counter collection: */
	struct msm_device *dev_global = NULL;
	/* device instances for local counter reservation: */
	struct msm_device *dev_local_1 = NULL;
	struct msm_device *dev_local_2 = NULL;
	unsigned int num_counters;

	igt_fixture() {
		dev_global = igt_msm_dev_open();
		dev_local_1 = igt_msm_dev_open();
		dev_local_2 = igt_msm_dev_open();

		num_counters = get_available_counters(dev_global, true);
		igt_info("num_counters=%u\n", num_counters);
	}

	igt_describe("Multiple process should be able to reserve the same "
		     "counters for local counter collection");
	igt_subtest("perfcntrs-local-coexist") {
		igt_require(num_counters > 0);

		igt_assert_eq(0, configure_counters(dev_local_1, false, num_counters));
		igt_assert_eq(0, configure_counters(dev_local_2, false, num_counters));

		/* release the reservations: */
		configure_counters(dev_local_1, false, 0);
		configure_counters(dev_local_2, false, 0);
	}

	igt_describe("non-conflict global and local counters");
	igt_subtest("perfcntrs-non-conflict-global-local") {
		int num_local = num_counters - 2;
		int stream_fd;

		igt_require(num_counters > 2);

		igt_assert_eq(0, configure_counters(dev_local_1, false, num_local));
		igt_assert_eq(0, configure_counters(dev_local_2, false, num_local));

		stream_fd = configure_counters(dev_global, true, 2);
		igt_assert_lte(0, stream_fd);
		close(stream_fd);

		/* release the reservations: */
		configure_counters(dev_local_1, false, 0);
		configure_counters(dev_local_2, false, 0);
	}

	igt_describe("conflict, local first");
	igt_subtest("conflict-local-first") {
		int num_local = num_counters - 1;
		int stream_fd;

		igt_require(num_counters > 2);

		igt_assert_eq(0, configure_counters(dev_local_1, false, num_local));
		igt_assert_eq(0, configure_counters(dev_local_2, false, num_local));

		stream_fd = configure_counters(dev_global, true, 2);
		igt_assert_lt(stream_fd, 0);

		/* release the reservation for dev_local_1: */
		configure_counters(dev_local_1, false, 0);

		/* should still fail: */
		stream_fd = configure_counters(dev_global, true, 2);
		igt_assert_lt(stream_fd, 0);

		/* release the reservation for dev_local_2: */
		configure_counters(dev_local_2, false, 0);

		/* now should succeed: */
		stream_fd = configure_counters(dev_global, true, 2);
		igt_assert_lte(0, stream_fd);
		close(stream_fd);
	}

	igt_describe("conflict, global first");
	igt_subtest("conflict-global-first") {
		int num_local = num_counters - 1;
		int stream_fd;

		igt_require(num_counters > 2);

		stream_fd = configure_counters(dev_global, true, 2);
		igt_assert_lte(0, stream_fd);

		/* Should fail because two counters already allocated for global collection: */
		igt_assert_neq(0, configure_counters(dev_local_1, false, num_local));

		/* release global counters: */
		close(stream_fd);

		/* Now reservation should succeed: */
		igt_assert_eq(0, configure_counters(dev_local_1, false, num_local));

		/* release the reservations: */
		configure_counters(dev_local_1, false, 0);
	}

	igt_describe("multiple groups");
	igt_subtest("multiple-groups") {
		const char *groups[] = {"CP", "SP"};

		igt_require(num_counters > 0);

		igt_assert_eq(0,
			      __configure_counters(dev_local_1, false, ARRAY_SIZE(groups), groups, 1));

		/* release the reservations: */
		configure_counters(dev_local_1, false, 0);
	}

	igt_describe("duplicate groups");
	igt_subtest("duplicate-groups") {
		const char *groups[] = {"CP", "CP"};

		igt_require(num_counters > 0);

		igt_assert_neq(0,
			       __configure_counters(dev_local_1, false, ARRAY_SIZE(groups), groups, 1));

		/* release the reservations: */
		configure_counters(dev_local_1, false, 0);
	}

	igt_fixture() {
		igt_msm_dev_close(dev_global);
		igt_msm_dev_close(dev_local_1);
		igt_msm_dev_close(dev_local_2);
	}
}
