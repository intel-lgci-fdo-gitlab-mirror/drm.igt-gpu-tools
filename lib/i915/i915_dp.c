// SPDX-License-Identifier: MIT
/*
 * Copyright (c) 2020, The Linux Foundation. All rights reserved.
 * Copyright © 2013 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *	Daniel Vetter <daniel.vetter@ffwll.ch>
 *	Damien Lespiau <damien.lespiau@intel.com>
 */

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "i915_dp.h"
#include "igt_core.h"
#include "igt_kms.h"

/**
 * i915_dp_parse_marked_value:
 * @buf: Buffer containing the content to parse
 * @marked_char: The character marking the value to parse
 * @result: Pointer to store the parsed value
 *
 * Finds the integer value in the buffer that is marked by the given character.
 *
 * Returns: 0 on success, -1 on failure
 */
static int i915_dp_parse_marked_value(const char *buf, char marked_char, int *result)
{
	char *marked_ptr, *val_ptr;

	/*
	 * Look for the marked character
	 */
	marked_ptr = strchr(buf, marked_char);

	if (marked_ptr) {
		val_ptr = marked_ptr - 1;
		while (val_ptr > buf && isdigit(*val_ptr))
			val_ptr--;
		val_ptr++;
		if (sscanf(val_ptr, "%d", result) == 1)
			return 0;
	}
	return -1;
}

 /**
  * i915_dp_get_current_link_rate:
  * @drm_fd: A drm file descriptor
  * @output: Target output
  *
  * Returns: link_rate if set for output else -1
  */
int i915_dp_get_current_link_rate(int drm_fd, igt_output_t *output)
{
	char buf[512];
	int res, ret;

	res = igt_debugfs_read_connector_file(drm_fd, igt_output_name(output),
					      "i915_dp_force_link_rate",
					      buf, sizeof(buf));
	igt_assert_f(res == 0, "Unable to read %s/i915_dp_force_link_rate\n",
		     igt_output_name(output));
	res = i915_dp_parse_marked_value(buf, '*', &ret);
	igt_assert_f(res == 0, "Output %s not enabled\n", igt_output_name(output));
	return ret;
}

/**
 * i915_dp_get_current_lane_count:
 * @drm_fd: A drm file descriptor
 * @output: Target output
 *
 * Returns: lane_count if set for output else -1
 */
int i915_dp_get_current_lane_count(int drm_fd, igt_output_t *output)
{
	char buf[512];
	int res, ret;

	res = igt_debugfs_read_connector_file(drm_fd, igt_output_name(output),
					      "i915_dp_force_lane_count",
					      buf, sizeof(buf));
	igt_assert_f(res == 0, "Unable to read %s/i915_dp_force_lane_count\n",
		     igt_output_name(output));
	res = i915_dp_parse_marked_value(buf, '*', &ret);
	igt_assert_f(res == 0, "Output %s not enabled\n", igt_output_name(output));
	return ret;
}

/**
 * i915_dp_get_max_link_rate:
 * @drm_fd: A drm file descriptor
 * @output: Target output
 *
 * Returns: max_link_rate
 */
int i915_dp_get_max_link_rate(int drm_fd, igt_output_t *output)
{
	char buf[512];
	int res, ret;

	res = igt_debugfs_read_connector_file(drm_fd, igt_output_name(output),
					      "i915_dp_max_link_rate",
					      buf, sizeof(buf));
	igt_assert_f(res == 0, "Unable to read %s/i915_dp_max_link_rate\n",
		     igt_output_name(output));

	igt_assert_f(sscanf(buf, "%d", &ret) == 1,
		     "Failed to parse max link rate from %s\n", buf);

	return ret;
}

/**
 * i915_dp_get_max_lane_count:
 * @drm_fd: A drm file descriptor
 * @output: Target output
 *
 * Returns: max_link_rate
 */
int i915_dp_get_max_lane_count(int drm_fd, igt_output_t *output)
{
	char buf[512];
	int res, ret;

	res = igt_debugfs_read_connector_file(drm_fd, igt_output_name(output),
					      "i915_dp_max_lane_count",
					      buf, sizeof(buf));
	igt_assert_f(res == 0, "Unable to read %s/i915_dp_max_lane_count\n",
		     igt_output_name(output));

	igt_assert_f(sscanf(buf, "%d", &ret) == 1,
		     "Failed to parse max lane count from %s\n", buf);

	return ret;
}

/**
 * i915_dp_force_link_retrain:
 * @drm_fd: A drm file descriptor
 * @output: Target output
 * @retrain_count: number of retraining required
 *
 * Force link retrain on the output.
 */
void i915_dp_force_link_retrain(int drm_fd, igt_output_t *output, int retrain_count)
{
	char value[2];
	int res;

	snprintf(value, sizeof(value), "%d", retrain_count);
	res = igt_debugfs_write_connector_file(drm_fd, igt_output_name(output),
					       "i915_dp_force_link_retrain",
					       value, strlen(value));
	igt_assert_f(res == 0, "Unable to write to %s/i915_dp_force_link_retrain\n",
		     igt_output_name(output));
}

/**
 * i915_dp_force_lt_failure:
 * @drm_fd: A drm file descriptor
 * @output: Target output
 * @failure_count: 1 for same link param and
 *		   2 for reduced link params
 *
 * Force link training failure on the output.
 * @failure_count: 1 for retraining with same link params
 *		   2 for retraining with reduced link params
 */
void i915_dp_force_lt_failure(int drm_fd, igt_output_t *output, int failure_count)
{
	char value[2];
	int res;

	snprintf(value, sizeof(value), "%d", failure_count);
	res = igt_debugfs_write_connector_file(drm_fd, igt_output_name(output),
					       "i915_dp_force_link_training_failure",
					       value, strlen(value));
	igt_assert_f(res == 0, "Unable to write to %s/i915_dp_force_link_training_failure\n",
		     igt_output_name(output));
}

/**
 * i915_dp_get_link_retrain_disabled:
 * @drm_fd: A drm file descriptor
 * @output: Target output
 *
 * Returns: True if link retrain disabled, false otherwise
 */
bool i915_dp_get_link_retrain_disabled(int drm_fd, igt_output_t *output)
{
	char buf[512];
	int res;

	res = igt_debugfs_read_connector_file(drm_fd, igt_output_name(output),
					      "i915_dp_link_retrain_disabled",
					      buf, sizeof(buf));
	igt_assert_f(res == 0, "Unable to read %s/i915_dp_link_retrain_disabled\n",
		     igt_output_name(output));
	return strstr(buf, "yes");
}

/**
 * i915_dp_has_force_link_training_failure_debugfs:
 * Checks if the force link training failure debugfs
 * is available for a specific output.
 *
 * @drmfd: file descriptor of the DRM device.
 * @output: output to check.
 * Returns:
 *  true if the debugfs is available, false otherwise.
 */
bool i915_dp_has_force_link_training_failure_debugfs(int drmfd, igt_output_t *output)
{
	char buf[512];
	int res;

	res = igt_debugfs_read_connector_file(drmfd, igt_output_name(output),
					      "i915_dp_link_retrain_disabled",
					      buf, sizeof(buf));
	return res == 0;
}

/**
 * i915_dp_get_pending_lt_failures:
 * @drm_fd: A drm file descriptor
 * @output: Target output
 *
 * Returns: Number of pending link training failures.
 */
int i915_dp_get_pending_lt_failures(int drm_fd, igt_output_t *output)
{
	char buf[512];
	int res, ret;

	res = igt_debugfs_read_connector_file(drm_fd, igt_output_name(output),
					      "i915_dp_force_link_training_failure",
					      buf, sizeof(buf));
	igt_assert_f(res == 0, "Unable to read %s/i915_dp_force_link_training_failure\n",
		     igt_output_name(output));

	igt_assert_f(sscanf(buf, "%d", &ret) == 1,
		     "Failed to parse pending link training failures from %s\n", buf);

	return ret;
}

/**
 * i915_dp_get_pending_retrain:
 * @drm_fd: A drm file descriptor
 * @output: Target output
 *
 * Returns: Number of pending link retrains.
 */
int i915_dp_get_pending_retrain(int drm_fd, igt_output_t *output)
{
	char buf[512];
	int res, ret;

	res = igt_debugfs_read_connector_file(drm_fd, igt_output_name(output),
					      "i915_dp_force_link_retrain",
					      buf, sizeof(buf));
	igt_assert_f(res == 0, "Unable to read %s/i915_dp_force_link_retrain\n",
		     igt_output_name(output));

	igt_assert_f(sscanf(buf, "%d", &ret) == 1,
		     "Failed to parse pending link retrains from %s\n", buf);

	return ret;
}

/**
 * i915_dp_reset_link_params:
 * @drm_fd: A drm file descriptor
 * @output: Target output
 *
 * Reset link rate and lane count to auto, also installs exit handler
 * to set link rate and lane count to auto on exit
 */
void i915_dp_reset_link_params(int drm_fd, igt_output_t *output)
{
	bool valid;
	drmModeConnector *temp;

	valid = true;
	valid = valid && connector_attr_set_debugfs(drm_fd, output->config.connector,
						    "i915_dp_force_link_rate",
						    "auto", "auto", true);
	valid = valid && connector_attr_set_debugfs(drm_fd, output->config.connector,
						    "i915_dp_force_lane_count",
						    "auto", "auto", true);
	igt_assert_f(valid, "Unable to set attr or install exit handler\n");
	dump_connector_attrs();
	igt_install_exit_handler(reset_connectors_at_exit);

	/*
	 * To allow callers to always use GetConnectorCurrent we need to force a
	 * redetection here.
	 */
	temp = drmModeGetConnector(drm_fd, output->config.connector->connector_id);
	drmModeFreeConnector(temp);
}

/**
 * i915_dp_set_link_params:
 * @drm_fd: A drm file descriptor
 * @output: Target output
 *
 * set link rate and lane count to given value, also installs exit handler
 * to set link rate and lane count to auto on exit
 */
void i915_dp_set_link_params(int drm_fd, igt_output_t *output,
			     const char *link_rate, const char *lane_count)
{
	bool valid;
	drmModeConnector *temp;

	valid = true;
	valid = valid && connector_attr_set_debugfs(drm_fd, output->config.connector,
						    "i915_dp_force_link_rate",
						    link_rate, "auto", true);
	valid = valid && connector_attr_set_debugfs(drm_fd, output->config.connector,
						    "i915_dp_force_lane_count",
						    lane_count, "auto", true);
	igt_assert_f(valid, "Unable to set attr or install exit handler\n");
	dump_connector_attrs();
	igt_install_exit_handler(reset_connectors_at_exit);

	/*
	 * To allow callers to always use GetConnectorCurrent we need to force a
	 * redetection here.
	 */
	temp = drmModeGetConnector(drm_fd, output->config.connector->connector_id);
	drmModeFreeConnector(temp);
}

/**
 * i915_dp_get_max_supported_rate:
 * @drm_fd: A drm file descriptor
 * @output: Target output
 *
 * Returns: Max supported link rate available for output, else -1
 */
int i915_dp_get_max_supported_rate(int drm_fd, const igt_output_t *output)
{
	char buf[512];
	int res, max_rate = -EINVAL;
	char *token;

	res = igt_debugfs_read_connector_file(drm_fd, igt_output_name(output),
					      "i915_dp_force_link_rate",
					      buf, sizeof(buf));
	igt_assert_f(!res, "Unable to read %s/i915_dp_force_link_rate\n",
		     igt_output_name(output));

	token = strtok(buf, " ");
	while (token) {
		int rate;

		errno = 0;
		rate = strtol(token, NULL, 0);
		if (!errno && rate > max_rate)
			max_rate = rate;
		token = strtok(NULL, " ");
	}

	return max_rate;
}

/**
 * i915_dp_get_next_lower_rate:
 * @drm_fd: A drm file descriptor
 * @output: Target output
 * @rate: reference link rate in 10 kbit/s units
 *
 * Parse the intel_dp_allowed_link_configs debugfs file and return the highest
 * allowed link rate strictly below @rate.
 *
 * This file lists the configurations the driver would actually pick from, i.e.
 * the intersection of the source rates, the rates the sink advertises and the
 * current link limits, as "<lanes>x<rate>" entries. The i915_dp_force_link_rate
 * list is only the source rates: writing a rate from it that the sink never
 * advertised still succeeds and the kernel silently clamps the effective rate
 * down, so the caller would report a rate the link was never trained at.
 *
 * Returns: highest allowed rate below @rate in 10 kbit/s units, or 0 if none.
 */
int i915_dp_get_next_lower_rate(int drm_fd, igt_output_t *output, int rate)
{
	char buf[4096];
	const char *p;
	int res, next = 0;

	res = igt_debugfs_read_connector_file(drm_fd, igt_output_name(output),
					      "intel_dp_allowed_link_configs",
					      buf, sizeof(buf));
	igt_assert_f(res == 0,
		     "Unable to read %s/intel_dp_allowed_link_configs\n",
		     igt_output_name(output));

	/*
	 * Entries are "<lanes>x<rate>". Key off the 'x' separator rather than
	 * tokenising the whole file, so the header and any decoration around
	 * the list are skipped without having to model them.
	 */
	for (p = buf; (p = strchr(p, 'x')); p++) {
		char *endptr;
		long r;

		/* Must be preceded by the lane count to be a config entry. */
		if (p == buf || !isdigit((unsigned char)p[-1]))
			continue;

		errno = 0;
		r = strtol(p + 1, &endptr, 10);
		if (errno || endptr == p + 1)
			continue;

		if (r < rate && r > next)
			next = (int)r;
	}

	return next;
}
