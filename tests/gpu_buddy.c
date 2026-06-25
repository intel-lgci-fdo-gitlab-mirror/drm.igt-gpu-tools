// SPDX-License-Identifier: MIT
/*
 * Copyright © 2019 Intel Corporation
 */

#include "igt.h"
#include "igt_kmod.h"

IGT_TEST_DESCRIPTION("Basic sanity check of the GPU buddy allocator (struct gpu_buddy)");

int igt_main()
{
	igt_kunit("gpu_buddy_tests", NULL, NULL);
}
