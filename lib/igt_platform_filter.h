/* SPDX-License-Identifier: MIT
 * Copyright 2026 Advanced Micro Devices, Inc.
 */

#ifndef IGT_PLATFORM_FILTER_H
#define IGT_PLATFORM_FILTER_H

#include <stdbool.h>

/**
 * SECTION: igt_platform_filter
 * @short_description: Generic platform-based test filtering framework
 * @title: Platform Filter
 * @include: igt_platform_filter.h
 *
 * Generic test filtering system that allows skipping tests/subtests based
 * on platform characteristics. Designed to be vendor-agnostic with
 * vendor-specific backends.
 *
 * Three-tier priority system (checked in sequence, first match wins):
 *   1. PRODUCTION: Built-in compile-time rules (vendor-specific)
 *   2. DEVELOPMENT: Config file /etc/igt/platform_skip.conf
 *   3. RUNTIME: Environment variable IGT_PLATFORM_SKIP_CONFIG
 *
 * Vendor Implementation:
 *   Each vendor implements platform_filter_ops callbacks to provide:
 *   - Platform identification and matching logic
 *   - Platform-specific data structures
 *   - Built-in skip rules
 *
 * Usage in tests:
 *   igt_fixture() {
 *       igt_platform_filter_init(vendor_ops, platform_info);
 *   }
 *
 *   igt_subtest("my-test") {
 *       // Automatic filtering - no manual call needed!
 *       test_code();
 *   }
 *
 * Config file format (/etc/igt/platform_skip.conf):
 *   # Lines starting with # are comments
 *   # Format: platform:test:subtest:reason
 *   # Use * as wildcard
 *
 *   navi48:*:*:All tests disabled on Navi48
 *   alderlake:i915_pm:*:Power management tests broken
 *
 * Environment variable format (IGT_PLATFORM_SKIP_CONFIG):
 *   Same as config file, semicolon-separated entries:
 *   export IGT_PLATFORM_SKIP_CONFIG="navi48:*:*:Testing;navi10:amd_basic:*:Broken"
 */

/* Maximum platform ranges per skip entry */
#define MAX_PLATFORM_RANGES 4

/**
 * enum skip_source - Source of skip rule
 */
enum skip_source {
	SKIP_SOURCE_BUILTIN,    /* From vendor built-in array */
	SKIP_SOURCE_CONFIG,     /* From /etc/igt/platform_skip.conf */
	SKIP_SOURCE_ENV,        /* From IGT_PLATFORM_SKIP_CONFIG */
	SKIP_SOURCE_NONE,       /* Not skipped */
};

/**
 * struct platform_skip_entry - Generic skip rule entry
 *
 * Generic structure for skip rules. Vendor-specific data is stored
 * in platform_data field and interpreted by vendor callbacks.
 */
struct platform_skip_entry {
	const char *test_name;          /* Test binary name or "*" for all */
	const char *subtest_glob;       /* Subtest pattern (fnmatch) or "*" */
	const char *reason;             /* Human-readable reason (required) */
	void *platform_data;            /* Vendor-specific platform matching data */
};

/**
 * struct platform_filter_ops - Vendor-specific operations
 *
 * Callback structure that vendors implement to provide platform-specific
 * filtering logic. This allows the core filtering framework to remain
 * vendor-agnostic.
 */
struct platform_filter_ops {
	/** @name: Vendor name (e.g., "amd", "intel") */

	const char *name;

	/** @get_platform_name: Get current platform name */
	const char *(*get_platform_name)(const void *platform_info);

	/** @match_platform: Check if skip entry matches current platform */
	bool (*match_platform)(const void *platform_info, const void *platform_data);

	/** @parse_platform_config: Parse platform string from config file */
	bool (*parse_platform_config)(const char *platform_str, void **platform_data_out);

	/** @get_builtin_rules: Get vendor-specific built-in skip rules */
	const struct platform_skip_entry *(*get_builtin_rules)(int *count_out);

	/** @dump_platform_data: Dump platform_data for debugging (optional) */
	void (*dump_platform_data)(const void *platform_data);
};

bool igt_platform_filter_is_initialized(void);

void igt_platform_filter_init(const struct platform_filter_ops *ops,
			      const void *platform_info);

void igt_platform_require(const char *subtest_name);

bool igt_platform_should_skip(const char *test_name,
			      const char *subtest_name,
			       enum skip_source *source,
			       const char **reason);

void igt_platform_filter_dump(void);

int igt_platform_filter_dump_to_file(const char *filename);

#endif /* IGT_PLATFORM_FILTER_H */
