/*
 * Copyright © 2018 Intel Corporation
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
 */

#include "drmtest.h"
#include "igt_params.h"
#include "igt_psr.h"
#include "igt_sysfs.h"
#include <errno.h>

#define SET_DEBUGFS_PATH(output, path) \
	sprintf(path, "%s%s%s", output ? output->name : "", output ? "/" : "", \
			output ? "i915_psr_status" : "i915_edp_psr_status")

bool psr_disabled_check(int debugfs_fd)
{
	char buf[PSR_STATUS_MAX_LEN];

	igt_debugfs_simple_read(debugfs_fd, "i915_edp_psr_status", buf,
				sizeof(buf));

	return strstr(buf, "PSR mode: disabled\n");
}

bool selective_fetch_check(int debugfs_fd, igt_output_t *output)
{
	char buf[PSR_STATUS_MAX_LEN];
	char debugfs_file[128] = {0};

	SET_DEBUGFS_PATH(output, debugfs_file);
	igt_debugfs_simple_read(debugfs_fd, debugfs_file, buf,
				sizeof(buf));

	return strstr(buf, "PSR2 selective fetch: enabled");
}
static bool psr_active_sink_check(int debugfs_fd, igt_output_t *output)
{
	char debugfs_file[128] = {0};
	char buf[PSR_STATUS_MAX_LEN];
	int ret;

	sprintf(debugfs_file, "%s/i915_psr_sink_status", output->name);
	ret = igt_debugfs_simple_read(debugfs_fd, debugfs_file, buf,
				      sizeof(buf));
	igt_assert_f(ret >= 1, "Failed to read sink status\n");

	return strstr(buf, "0x2 [active, display from RFB]");
}

/*
 * Checks if Early Transport is enabled in PSR status by reading the debugfs.
 */
bool early_transport_check(int debugfs_fd)
{
	char buf[PSR_STATUS_MAX_LEN];

	igt_debugfs_simple_read(debugfs_fd, "i915_edp_psr_status", buf,
				sizeof(buf));

	return strstr(buf, "enabled (Early Transport)");
}

static bool sink_status_checks(void)
{
	const char *env;

	env = getenv("IGT_PSR_SINK_STATUS_CHECKS");

	return env && atoi(env);
}

static bool psr_active_check(int debugfs_fd, enum psr_mode mode, igt_output_t *output)
{
	char debugfs_file[128] = {0};
	char buf[PSR_STATUS_MAX_LEN];
	drmModeConnector *c = NULL;
	const char *state;
	bool active;
	int ret;

	if (mode == PR_MODE || mode == PR_MODE_SEL_FETCH) {
		igt_assert_f(output, "Output not given\n");
		c = output->config.connector;
		if (c->connector_type == DRM_MODE_CONNECTOR_DisplayPort)
			state = "SRDENT_ON";
		else if (c->connector_type == DRM_MODE_CONNECTOR_eDP)
			state = "SLEEP";
	} else if (mode == PSR_MODE_1)
		state = "SRDENT";
	else if (mode == PSR_MODE_2 || mode == PSR_MODE_2_SEL_FETCH)
		state = "DEEP_SLEEP";
	else
		igt_assert_f(false, "Invalid psr mode\n");

	SET_DEBUGFS_PATH(output, debugfs_file);
	ret = igt_debugfs_simple_read(debugfs_fd, debugfs_file,
				     buf, sizeof(buf));
	if (ret < 0) {
		igt_info("Could not read i915_edp_psr_status: %s\n",
			 strerror(-ret));
		return false;
	}

	igt_skip_on(strstr(buf, "PSR sink not reliable: yes"));

	igt_skip_on(strstr(buf, "PSR setup timing not met"));

	active = strstr(buf, state) ||
		 (c && (c->connector_type == DRM_MODE_CONNECTOR_DisplayPort) &&
		  strstr(buf, "SU_STANDBY"));

	if (active && output && sink_status_checks()) {
		active = psr_active_sink_check(debugfs_fd, output);
		igt_assert_f(active, "PSR sink/source state mismatch\n");
	}

	return active;
}

/*
 * For PSR1, we wait until PSR is active. We wait until DEEP_SLEEP for PSR2.
 */
bool psr_wait_entry(int debugfs_fd, enum psr_mode mode, igt_output_t *output)
{
	return igt_wait(psr_active_check(debugfs_fd, mode, output), 500, 20);
}

bool psr_wait_update(int debugfs_fd, enum psr_mode mode, igt_output_t *output)
{
	/*
	 * TODO: After enabling Panel Replay on DP2.1, observe that the SRD status
	 * remains in the SRDENT_ON state. Remove the polling mechanism for the SRD
	 * status change for the DP2.1 output.
	 */
	if (output != NULL &&
	    output->config.connector->connector_type == DRM_MODE_CONNECTOR_DisplayPort)
		return igt_wait(psr_active_check(debugfs_fd, mode, output), 40, 1);
	else
		return igt_wait(!psr_active_check(debugfs_fd, mode, output), 40, 1);
}

bool psr_long_wait_update(int debugfs_fd, enum psr_mode mode, igt_output_t *output)
{
	/*
	 * TODO: After enabling Panel Replay on DP2.1, observe that the SRD status
	 * remains in the SRDENT_ON state. Remove the polling mechanism for the SRD
	 * status change for the DP2.1 output.
	 */
	if (output != NULL &&
	    output->config.connector->connector_type == DRM_MODE_CONNECTOR_DisplayPort)
		return igt_wait(psr_active_check(debugfs_fd, mode, output), 500, 1);
	else
		return igt_wait(!psr_active_check(debugfs_fd, mode, output), 500, 1);
}

static ssize_t psr_write(int debugfs_fd, const char *buf, igt_output_t *output)
{
	/*
	 * FIXME: Currently we don't have separate psr_debug file for each output.
	 * so, we are using i915_edp_psr_debug file for all outputs.
	 * Later we need to add support for separate psr_debug file for each output.
	 */
	return igt_sysfs_write(debugfs_fd, "i915_edp_psr_debug", buf,
						   strlen(buf));
}

static int has_psr_debugfs(int debugfs_fd, igt_output_t *output)
{
	int ret;

	/*
	 * Check if new PSR debugfs api is usable by writing an invalid value.
	 * Legacy mode will return OK here, debugfs api will return -EINVAL.
	 * -ENODEV is returned when PSR is unavailable.
	 */
	ret = psr_write(debugfs_fd, "0xf", output);
	if (ret == -EINVAL) {
		errno = 0;
		return 0;
	} else if (ret < 0)
		return ret;

	/* legacy debugfs api, we enabled irqs by writing, disable them. */
	psr_write(debugfs_fd, "0", output);
	return -EINVAL;
}

static bool psr_modparam_set(int device, int val)
{
	static int oldval = -1;

	igt_set_module_param_int(device, "enable_psr", val);

	if (val == oldval)
		return false;

	oldval = val;
	return true;
}

static int psr_restore_debugfs_fd = -1;

static void restore_psr_debugfs(int sig)
{
	psr_write(psr_restore_debugfs_fd, "0", NULL);
}

static bool psr_set(int device, int debugfs_fd, int mode, igt_output_t *output)
{
	int ret;

	ret = has_psr_debugfs(debugfs_fd, output);
	if (ret == -ENODEV) {
		igt_skip("PSR not available\n");
		return false;
	}

	if (ret == -EINVAL) {
		/*
		 * We can not control what PSR version is going to be enabled
		 * by setting enable_psr parameter, when unmatched the PSR
		 * version enabled and the PSR version of the test, it will
		 * fail in the first psr_wait_entry() of the test.
		 */
		ret = psr_modparam_set(device, mode >= PSR_MODE_1);
	} else {
		int debug_val = mode <= PSR_MODE_2_SEL_FETCH ? 0x40 : 0;

		switch (mode) {
		case PSR_MODE_1:
		case PR_MODE:
			debug_val |= 0x3;
				break;
		case PSR_MODE_2:
			debug_val |= 0x2;
				break;
		case PSR_MODE_2_SEL_FETCH:
		case PR_MODE_SEL_FETCH:
			debug_val |= 0x4;
			break;
		default:
			/* Disables PSR */
			debug_val = 0x1;
		}

		/* old debugfs interface doesn't recognize Panel Replay disable bit */
		do {
			char debug_str[8];

			sprintf(debug_str, "0x%x", debug_val);
			ret = psr_write(debugfs_fd, debug_str, output);
			if (!(debug_val & 0x40))
				break;

			debug_val &= ~0x40;
		} while (ret <= 0);
		igt_require_f(ret > 0, "PSR2 SF feature not available\n");
	}

	/* Restore original value on exit */
	if (psr_restore_debugfs_fd == -1) {
		psr_restore_debugfs_fd = dup(debugfs_fd);
		igt_assert(psr_restore_debugfs_fd >= 0);
		igt_install_exit_handler(restore_psr_debugfs);
	}

	return ret;
}

bool psr_enable(int device, int debugfs_fd, enum psr_mode mode, igt_output_t *output)
{
	return psr_set(device, debugfs_fd, mode, output);
}

bool psr_disable(int device, int debugfs_fd, igt_output_t *output)
{
	/* Any mode different than PSR_MODE_1/2 will disable PSR */
	return psr_set(device, debugfs_fd, -1, output);
}

bool psr_sink_support(int device, int debugfs_fd, enum psr_mode mode, igt_output_t *output)
{
	char *line;
	char debugfs_file[128] = {0};
	char buf[PSR_STATUS_MAX_LEN];
	int ret;

	SET_DEBUGFS_PATH(output, debugfs_file);
	ret = igt_debugfs_simple_read(debugfs_fd, debugfs_file, buf,
				      sizeof(buf));
	if (ret < 1)
		return false;

	line = strstr(buf, "Sink support: ");
	if (!line)
		return false;

	switch (mode) {
	case PSR_MODE_1:
		return strstr(line, "PSR = yes") ||
		       strstr(buf, "Sink_Support: yes\n") ||
		       strstr(buf, "Sink support: yes");
	case PSR_MODE_2:
	case PSR_MODE_2_SEL_FETCH:
		/*
		 * i915 requires PSR version 0x03 that is PSR2 + SU with
		 * Y-coordinate to support PSR2
		 * or
		 *
		 * PSR version 0x4 that is PSR2 + SU w/ Y-coordinate and SU
		 * Region Early Transport to support PSR2 (eDP 1.5)
		 */
		return strstr(buf, "Sink support: yes [0x03]") ||
		       strstr(buf, "Sink support: yes [0x04]") ||
		       (strstr(line, "PSR = yes") &&
		       (strstr(line, "[0x03]") || strstr(line, "[0x04]")));
	case PSR_MODE_2_ET:
		return strstr(buf, "Sink support: yes [0x04]") ||
		       (strstr(line, "PSR = yes") && strstr(line, "[0x04]"));
	case PR_MODE:
		return strstr(line, "Panel Replay = yes");
	case PR_MODE_SEL_FETCH:
		return strstr(line, "Panel Replay = yes, Panel Replay Selective Update = yes");
	case PR_MODE_SEL_FETCH_ET:
		return strstr(line, "Panel Replay Selective Update = yes (Early Transport)");
	default:
		igt_assert_f(false, "Invalid psr mode\n");
		return false;
	}
}

/**
 * psr_sink_error_check
 * Check and assert on PSR errors detected by panel
 *
 * Returns:
 * None
 */
void psr_sink_error_check(int debugfs_fd, enum psr_mode mode, igt_output_t *output)
{
	char *line;
	char debugfs_file[128] = {0};
	char buf[PSR_STATUS_MAX_LEN];
	int ret;

	if (!sink_status_checks())
		return;

	sprintf(debugfs_file, "%s/i915_psr_sink_status", output->name);
	ret = igt_debugfs_simple_read(debugfs_fd, debugfs_file, buf,
				      sizeof(buf));
	igt_assert_f(ret >= 1, "Failed to read sink status\n");

	line = strstr(buf, "error status: 0x0");

	/*
	 * On certain PSR1 panels we are seeing "PSR VSC SDP
	 * uncorrectable error" bit set even it is applicable for PSR1
	 * only
	 */
	if (!line && mode == PSR_MODE_1)
		line = strstr(buf, "Sink PSR error status: 0x4");

	igt_assert_f(line, "Sink detected PSR error(s):\n%s\n", buf);
}

#define PSR2_SU_BLOCK_STR_LOOKUP "PSR2 SU blocks:\n0\t"

/* Return the the last or last but one su blocks */
static bool
psr2_read_last_num_su_blocks_val(int debugfs_fd, uint16_t *num_su_blocks)
{
	char buf[PSR_STATUS_MAX_LEN];
	char *str, *str2;
	int ret;

	ret = igt_debugfs_simple_read(debugfs_fd, "i915_edp_psr_status", buf,
				      sizeof(buf));
	if (ret < 0)
		return false;

	str = strstr(buf, PSR2_SU_BLOCK_STR_LOOKUP);
	if (!str)
		return false;

	str = &str[strlen(PSR2_SU_BLOCK_STR_LOOKUP)];
	*num_su_blocks = (uint16_t)strtol(str, &str2, 10);
	if (*num_su_blocks != 0)
		return true;

	str = str2;
	/* Jump '\n''1''\t' */
	str += 3;
	*num_su_blocks = (uint16_t)strtol(str, NULL, 10);

	return true;
}

bool psr2_wait_su(int debugfs_fd, uint16_t *num_su_blocks)
{
	return igt_wait(psr2_read_last_num_su_blocks_val(debugfs_fd, num_su_blocks), 40, 1);
}

void psr_print_debugfs(int debugfs_fd)
{
	char buf[PSR_STATUS_MAX_LEN];
	int ret;

	ret = igt_debugfs_simple_read(debugfs_fd, "i915_edp_psr_status", buf,
				      sizeof(buf));
	if (ret < 0) {
		igt_info("Could not read i915_edp_psr_status: %s\n",
			 strerror(-ret));
		return;
	}

	igt_info("%s", buf);
}

bool i915_psr2_selective_fetch_check(int drm_fd, igt_output_t *output)
{
	int debugfs_fd;
	bool ret;

	if (!is_intel_device(drm_fd))
		return false;

	debugfs_fd = igt_debugfs_dir(drm_fd);
	ret = selective_fetch_check(debugfs_fd, output);
	close(debugfs_fd);

	return ret;
}

/**
 * i915_psr2_sel_fetch_to_psr1
 *
 * Check if PSR2 selective fetch is enabled, if yes switch to PSR1 and returns
 * true otherwise returns false.
 * This function should be called from tests that are not compatible with PSR2
 * selective fetch.
 *
 * Returns:
 * True if PSR mode changed to PSR1, false otherwise.
 */
bool i915_psr2_sel_fetch_to_psr1(int drm_fd, igt_output_t *output)
{
	int debugfs_fd;
	bool ret = false;

	if (!is_intel_device(drm_fd))
		return ret;

	debugfs_fd = igt_debugfs_dir(drm_fd);
	if (selective_fetch_check(debugfs_fd, output)) {
		/*
		 * With below we switch to PSR1 incase of PSR and
		 * Panel Replay Full Frame update in case of Panel Replay.
		 */
		psr_set(drm_fd, debugfs_fd, PSR_MODE_1, output);
		ret = true;
	}

	close(debugfs_fd);
	return ret;
}

/**
 * i915_psr2_sel_fetch_restore
 *
 * Restore PSR2 selective fetch after tests were executed, this function should
 * only be called if i915_psr2_sel_fetch_to_psr1() returned true.
 */
void i915_psr2_sel_fetch_restore(int drm_fd, igt_output_t *output)
{
	int debugfs_fd;

	debugfs_fd = igt_debugfs_dir(drm_fd);
	psr_set(drm_fd, debugfs_fd, PSR_MODE_2_SEL_FETCH, output);
	close(debugfs_fd);
}

/**
 * psr_get_mode
 *
 * Return the current PSR mode.
 */
enum psr_mode psr_get_mode(int debugfs_fd, igt_output_t *output)
{
	char buf[PSR_STATUS_MAX_LEN];
	char debugfs_file[128] = {0};
	int ret;

	SET_DEBUGFS_PATH(output, debugfs_file);
	ret = igt_debugfs_simple_read(debugfs_fd, debugfs_file, buf,
				      sizeof(buf));
	if (ret < 0) {
		igt_info("Could not read psr status: %s\n",
			 strerror(-ret));
		return PSR_DISABLED;
	}

	if (strstr(buf, "Panel Replay enabled"))
		return PR_MODE;
	else if (strstr(buf, "Panel Replay Selective Update enabled"))
		return PR_MODE_SEL_FETCH;
	else if (strstr(buf, "PSR2 selective fetch: enabled"))
		return PSR_MODE_2_SEL_FETCH;
	else if (strstr(buf, "PSR2"))
		return PSR_MODE_2;
	else if (strstr(buf, "PSR1"))
		return PSR_MODE_1;

	return PSR_DISABLED;
}

/**
 * is_psr_enable_possible
 * Check if given psr mode can be enabled by reading enable_psr
 * modparam
 *
 * Returns:
 * True if given psr mode can be enabled, false otherwise.
 */
bool is_psr_enable_possible(int drm_fd, enum psr_mode mode)
{
	char *param_value;
	int enable_psr;

	param_value = __igt_params_get(drm_fd, "enable_psr");
	igt_assert_f(param_value, "Could not read enable_psr modparam\n");
	enable_psr = atoi(param_value);
	free(param_value);
	return enable_psr > mode;
}
