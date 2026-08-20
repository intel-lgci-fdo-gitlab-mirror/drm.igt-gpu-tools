// SPDX-License-Identifier: MIT
// Copyright 2026 Advanced Micro Devices, Inc.
/*
 * AMD-specific platform filtering backend
 *
 * Implements platform_filter_ops callbacks for AMD GPUs, providing
 * platform identification and matching logic based on ASIC family/chip.
 */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "igt.h"
#include "igt_platform_filter.h"
#include "amd_platform.h"
#include "amdgpu_asic_addr.h"

/**
 * AMD platform data structures
 *
 * These structures define how AMD ASICs are matched for filtering.
 * They use family/chip ranges similar to amd_queue_reset.c
 */

/* Maximum ASIC family ranges per skip entry */
#define MAX_ASIC_RANGES 4

/**
 * struct amd_asic_range - ASIC family range for matching
 *
 * Similar to struct used in amd_queue_reset.c for defining ASIC ranges.
 * Uses definitions from amdgpu_asic_addr.h
 */
struct amd_asic_range {
	int family_id;      /* FAMILY_NV, FAMILY_GFX1200, etc. */
	int chip_id_min;    /* Min chip revision */
	int chip_id_max;    /* Max chip revision */
};

/**
 * struct amd_platform_data - AMD platform matching data
 *
 * This is stored in platform_skip_entry->platform_data field.
 * Contains array of ASIC ranges to match against.
 */
struct amd_platform_data {
	struct amd_asic_range ranges[MAX_ASIC_RANGES];
	int num_ranges;
};

/* ASIC name to family/chip mapping table */
struct asic_info {
	const char *name;
	int family_id;
	int chip_id_min;
	int chip_id_max;
};

static const struct asic_info asic_table[] = {
	/* GFX12 - using ranges from amdgpu_asic_addr.h */
	{ "navi48", FAMILY_GFX1200, AMDGPU_GFX1200_RANGE },
	{ "navi44", FAMILY_GFX1200, AMDGPU_GFX1200_RANGE },

	/* GFX11.5 */
	{ "gfx1150", FAMILY_GFX1150, AMDGPU_GFX1150_RANGE },
	{ "gfx1151", FAMILY_GFX1150, AMDGPU_GFX1151_RANGE },
	{ "gfx1152", FAMILY_GFX1150, AMDGPU_GFX1152_RANGE },
	{ "gfx1153", FAMILY_GFX1150, AMDGPU_GFX1153_RANGE },

	/* GFX11 */
	{ "gfx1100", FAMILY_GFX1100, AMDGPU_GFX1100_RANGE },
	{ "gfx1101", FAMILY_GFX1100, AMDGPU_GFX1101_RANGE },
	{ "gfx1102", FAMILY_GFX1100, AMDGPU_GFX1102_RANGE },
	{ "gfx1103_r1", FAMILY_GFX1103, AMDGPU_GFX1103_R1_RANGE },
	{ "gfx1103_r2", FAMILY_GFX1103, AMDGPU_GFX1103_R2_RANGE },
	{ "navi31", FAMILY_GFX1100, AMDGPU_GFX1100_RANGE },
	{ "navi32", FAMILY_GFX1100, AMDGPU_GFX1101_RANGE },
	{ "navi33", FAMILY_GFX1100, AMDGPU_GFX1102_RANGE },

	/* GFX10.3 */
	{ "sienna_cichlid", FAMILY_NV, AMDGPU_SIENNA_CICHLID_RANGE },
	{ "navy_flounder", FAMILY_NV, AMDGPU_NAVY_FLOUNDER_RANGE },
	{ "dimgrey_cavefish", FAMILY_NV, AMDGPU_DIMGREY_CAVEFISH_RANGE },
	{ "beige_goby", FAMILY_NV, AMDGPU_BEIGE_GOBY_RANGE },
	{ "yellow_carp", FAMILY_YC, AMDGPU_YELLOW_CARP_RANGE },
	{ "vangogh", FAMILY_VGH, AMDGPU_VANGOGH_RANGE },

	/* GFX10 */
	{ "navi10", FAMILY_NV, AMDGPU_NAVI10_RANGE },
	{ "navi12", FAMILY_NV, AMDGPU_NAVI12_RANGE },
	{ "navi14", FAMILY_NV, AMDGPU_NAVI14_RANGE },
	{ "navi21", FAMILY_NV, AMDGPU_SIENNA_CICHLID_RANGE },
	{ "navi22", FAMILY_NV, AMDGPU_NAVY_FLOUNDER_RANGE },
	{ "navi23", FAMILY_NV, AMDGPU_DIMGREY_CAVEFISH_RANGE },
	{ "navi24", FAMILY_NV, AMDGPU_BEIGE_GOBY_RANGE },

	/* CDNA */
	{ "arcturus", FAMILY_AI, AMDGPU_ARCTURUS_RANGE },
	{ "aldebaran", FAMILY_AI, AMDGPU_ALDEBARAN_RANGE },

	/* GFX9 */
	{ "vega10", FAMILY_AI, AMDGPU_VEGA10_RANGE },
	{ "vega12", FAMILY_AI, AMDGPU_VEGA12_RANGE },
	{ "vega20", FAMILY_AI, AMDGPU_VEGA20_RANGE },
	{ "raven", FAMILY_RV, AMDGPU_RAVEN_RANGE },
	{ "raven2", FAMILY_RV, AMDGPU_RAVEN2_RANGE },
	{ "renoir", FAMILY_RV, AMDGPU_RENOIR_RANGE },

	/* GFX8 (VI/Polaris) */
	{ "polaris10", FAMILY_VI, AMDGPU_POLARIS10_RANGE },
	{ "polaris11", FAMILY_VI, AMDGPU_POLARIS11_RANGE },
	{ "polaris12", FAMILY_VI, AMDGPU_POLARIS12_RANGE },
	{ "fiji", FAMILY_VI, AMDGPU_FIJI_RANGE },
	{ "tonga", FAMILY_VI, AMDGPU_TONGA_RANGE },
	{ "iceland", FAMILY_VI, AMDGPU_ICELAND_RANGE },
	{ "carrizo", FAMILY_CZ, AMDGPU_CARRIZO_RANGE },
	{ "stoney", FAMILY_CZ, AMDGPU_STONEY_RANGE },

	{ NULL, 0, 0, 0 }
};

/* Helper: Get ASIC info by name (case-insensitive) */
static const struct asic_info *get_asic_info(const char *name)
{
	const struct asic_info *info;

	if (!name)
		return NULL;

	for (info = asic_table; info->name; info++) {
		if (strcasecmp(info->name, name) == 0)
			return info;
	}
	return NULL;
}

/* Helper: Get ASIC name by family/chip */
static const char *get_asic_name(int family_id, int chip_rev)
{
	const struct asic_info *info;

	for (info = asic_table; info->name; info++) {
		if (info->family_id == family_id &&
		    chip_rev >= info->chip_id_min &&
		    chip_rev < info->chip_id_max)
			return info->name;
	}
	return "unknown";
}

/* ================================================================
 * AMD PLATFORM FILTER OPS IMPLEMENTATION
 * ================================================================
 */

static const char *amd_get_platform_name(const void *platform_info)
{
	const struct amdgpu_gpu_info *gpu_info = platform_info;

	if (!gpu_info)
		return "unknown";

	return get_asic_name(gpu_info->family_id, gpu_info->chip_rev);
}

static bool amd_match_platform(const void *platform_info, const void *platform_data)
{
	const struct amdgpu_gpu_info *gpu_info = platform_info;
	const struct amd_platform_data *amd_data = platform_data;
	int i;

	if (!gpu_info || !amd_data)
		return false;

	/* If no ranges specified, match all platforms */
	if (amd_data->num_ranges == 0)
		return true;

	/* Check if GPU matches any of the ASIC ranges */
	for (i = 0; i < amd_data->num_ranges && i < MAX_ASIC_RANGES; i++) {
		if (amd_data->ranges[i].family_id == gpu_info->family_id) {
			int chip_rev = gpu_info->chip_rev;

			if (chip_rev >= amd_data->ranges[i].chip_id_min &&
			    chip_rev < amd_data->ranges[i].chip_id_max) {
				return true;
			}
		}
	}

	return false;
}

static bool amd_parse_platform_config(const char *platform_str, void **platform_data_out)
{
	const struct asic_info *info;
	struct amd_platform_data *amd_data;

	info = get_asic_info(platform_str);
	if (!info) {
		igt_warn("Unknown AMD ASIC name: %s\n", platform_str);
		return false;
	}

	amd_data = malloc(sizeof(*amd_data));
	if (!amd_data)
		return false;

	memset(amd_data, 0, sizeof(*amd_data));
	amd_data->ranges[0].family_id = info->family_id;
	amd_data->ranges[0].chip_id_min = info->chip_id_min;
	amd_data->ranges[0].chip_id_max = info->chip_id_max;
	amd_data->num_ranges = 1;

	*platform_data_out = amd_data;
	return true;
}

static void amd_dump_platform_data(const void *platform_data)
{
	const struct amd_platform_data *amd_data = platform_data;
	int i;

	if (!amd_data) {
		printf("(all platforms)");
		return;
	}

	for (i = 0; i < amd_data->num_ranges && i < MAX_ASIC_RANGES; i++) {
		if (i > 0)
			printf(", ");
		printf("{0x%02X, 0x%02X-0x%02X}",
		       amd_data->ranges[i].family_id,
		       amd_data->ranges[i].chip_id_min,
		       amd_data->ranges[i].chip_id_max);
	}
}

/* ================================================================
 * AMD BUILT-IN SKIP RULES
 * ================================================================
 *
 * These are production skip rules. They are checked FIRST before
 * config file or environment variable.
 *
 * To add a skip rule:
 * 1. Define platform data with ASIC ranges
 * 2. Add entry to builtin_skip_table[]
 * 3. Rebuild IGT
 *
 * Example formats (uncomment to use):
 *
 * Single ASIC:
 *   static struct amd_platform_data navi44_data = {
 *       .ranges = { {FAMILY_GFX1200, AMDGPU_GFX1200_RANGE} },
 *       .num_ranges = 1
 *   };
 *   { "amd_basic", "*-UMQ", "UMQ not supported on Navi44", &navi44_data },
 *
 * Multiple ASICs:
 *   static struct amd_platform_data navi10_12_14_data = {
 *       .ranges = {
 *           {FAMILY_NV, AMDGPU_NAVI10_RANGE},
 *           {FAMILY_NV, AMDGPU_NAVI12_RANGE},
 *           {FAMILY_NV, AMDGPU_NAVI14_RANGE}
 *       },
 *       .num_ranges = 3
 *   };
 *   { "amd_userq_abort", "*", "Queue reset unstable", &navi10_12_14_data },
 *
 * All platforms (no platform restriction):
 *   { "test_name", "subtest", "reason", NULL },
 */

static const struct platform_skip_entry builtin_skip_table[] = {
	/* Add production skip rules here */

	/* Sentinel */
	{}
};

static const struct platform_skip_entry *amd_get_builtin_rules(int *count_out)
{
	int count = 0;

	/* Count entries (stop at sentinel) */
	while (builtin_skip_table[count].test_name ||
	       builtin_skip_table[count].subtest_glob ||
	       builtin_skip_table[count].reason)
		count++;

	*count_out = count;
	return builtin_skip_table;
}

/* AMD platform filter operations */
static const struct platform_filter_ops amd_platform_ops = {
	.name = "amd",
	.get_platform_name = amd_get_platform_name,
	.match_platform = amd_match_platform,
	.parse_platform_config = amd_parse_platform_config,
	.get_builtin_rules = amd_get_builtin_rules,
	.dump_platform_data = amd_dump_platform_data,
};

/* ================================================================
 * PUBLIC API
 * ================================================================
 */

const struct platform_filter_ops *amd_platform_get_ops(void)
{
	return &amd_platform_ops;
}

/**
 * amd_platform_filter_init:
 * @gpu_info: AMD GPU information from amdgpu query
 *
 * Initialize platform filtering for AMD GPUs. This is a convenience
 * wrapper that sets up the generic filtering framework with AMD-specific
 * callbacks and GPU identification data.
 *
 * Must be called before using igt_platform_require() in AMD tests.
 */
void amd_platform_filter_init(const struct amdgpu_gpu_info *gpu_info)
{
	igt_platform_filter_init(&amd_platform_ops, gpu_info);
}
