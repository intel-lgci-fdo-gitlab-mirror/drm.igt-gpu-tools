/* SPDX-License-Identifier: MIT */

#ifndef _I915_DP_H_
#define _I915_DP_H_

#include <stdbool.h>

#include "igt_kms.h"

/* DP link rates are in 10 kbit/s units; UHBR10 is 10 Gbps. */
#define I915_DP_UHBR10_LINK_RATE	1000000

/**
 * i915_dp_is_uhbr_rate:
 * @link_rate: DP link rate in 10 kbit/s units, as reported by the
 *	       i915_dp_*_link_rate debugfs files
 *
 * UHBR (Ultra High Bit Rate) link rates use 128b/132b channel encoding,
 * everything below uses legacy 8b/10b. Mirrors the kernel's
 * drm_dp_is_uhbr_rate().
 *
 * Returns: true if @link_rate is a UHBR rate, false otherwise.
 */
static inline bool i915_dp_is_uhbr_rate(int link_rate)
{
	return link_rate >= I915_DP_UHBR10_LINK_RATE;
}

int i915_dp_get_current_link_rate(int drm_fd, igt_output_t *output);
int i915_dp_get_current_lane_count(int drm_fd, igt_output_t *output);
int i915_dp_get_max_link_rate(int drm_fd, igt_output_t *output);
int i915_dp_get_max_lane_count(int drm_fd, igt_output_t *output);
void i915_dp_force_link_retrain(int drm_fd, igt_output_t *output, int retrain_count);
void i915_dp_force_lt_failure(int drm_fd, igt_output_t *output, int failure_count);
bool i915_dp_get_link_retrain_disabled(int drm_fd, igt_output_t *output);
bool i915_dp_has_force_link_training_failure_debugfs(int drmfd, igt_output_t *output);
int i915_dp_get_pending_lt_failures(int drm_fd, igt_output_t *output);
int i915_dp_get_pending_retrain(int drm_fd, igt_output_t *output);
void i915_dp_reset_link_params(int drm_fd, igt_output_t *output);
void i915_dp_set_link_params(int drm_fd, igt_output_t *output,
			     const char *link_rate, const char *lane_count);
int i915_dp_get_max_supported_rate(int drm_fd, const igt_output_t *output);
int i915_dp_get_next_lower_rate(int drm_fd, igt_output_t *output, int rate);

#endif
