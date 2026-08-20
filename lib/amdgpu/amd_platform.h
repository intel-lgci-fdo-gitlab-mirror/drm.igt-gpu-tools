/* SPDX-License-Identifier: MIT
 * Copyright 2026 Advanced Micro Devices, Inc.
 */

#ifndef AMD_PLATFORM_H
#define AMD_PLATFORM_H

#include "igt_platform_filter.h"
#include "amd_ip_blocks.h"

/**
 * SECTION: amd_platform
 * @short_description: AMD-specific platform filtering backend
 * @title: AMD Platform
 * @include: amd_platform.h
 *
 * AMD implementation of platform filtering that plugs into the generic
 * IGT platform filter framework.
 *
 * This backend provides:
 * - ASIC identification and matching based on family/chip ranges
 * - Built-in skip rules for AMD GPUs
 * - Integration with amdgpu_asic_addr.h definitions
 *
 * Usage in AMD tests:
 *   igt_fixture() {
 *       setup_amdgpu_ip_blocks(...);
 *       amd_platform_filter_init(&gpu_info);
 *   }
 *
 *   igt_subtest("my-test") {
 *       // Automatic filtering - no manual call needed!
 *       test_code();
 *   }
 */

/**
 * amd_platform_filter_init - Initialize AMD platform filtering
 * @gpu_info: AMDGPU GPU information structure
 *
 * Convenience wrapper that initializes the generic platform filter
 * with AMD-specific operations and GPU info.
 */
void amd_platform_filter_init(const struct amdgpu_gpu_info *gpu_info);

/**
 * amd_platform_get_ops - Get AMD platform filter operations
 *
 * Returns: AMD platform_filter_ops structure
 */
const struct platform_filter_ops *amd_platform_get_ops(void);

#endif /* AMD_PLATFORM_H */
