/*
 * Copyright 2019 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

/**
 * TEST: kms hdr
 * Category: Display
 * Description: Test HDR metadata interfaces and bpc switch
 * Driver requirement: i915, xe
 * Mega feature: HDR
 */

#include "igt.h"
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include "igt_edid.h"
#include "igt_hdr.h"
#include "igt_sysfs.h"

/**
 * SUBTEST: bpc-switch
 * Description: Tests switching between different display output bpc modes
 *
 * SUBTEST: bpc-switch-dpms
 * Description: Tests switching between different display output bpc modes with dpms
 *
 * SUBTEST: bpc-switch-suspend
 * Description: Tests switching between different display output bpc modes with suspend
 *
 * SUBTEST: invalid-hdr
 * Description: Test to ensure HDR is not enabled on non-HDR panel
 *
 * SUBTEST: invalid-metadata-sizes
 * Description: Tests invalid HDR metadata sizes
 *
 * SUBTEST: static-toggle-dpms
 * Description: Tests static toggle with dpms
 *
 * SUBTEST: static-toggle-suspend
 * Description: Tests static toggle with suspend
 *
 * SUBTEST: brightness-with-hdr
 * Description: Tests brightness with HDR
 *
 * SUBTEST: static-%s
 * Description: Tests %arg[1].
 *
 * arg[1]:
 *
 * @swap:                    swapping static HDR metadata
 * @toggle:                  entering and exiting HDR mode
 */

IGT_TEST_DESCRIPTION("Test HDR metadata interfaces and bpc switch");

#define BACKLIGHT_PATH "/sys/class/backlight"

/* HDR test formats: 10bpc + FP16 */
static const uint32_t hdr_test_formats[] = {
	DRM_FORMAT_XRGB2101010,
	DRM_FORMAT_XRGB16161616F,
};

/* Test flags. */
enum {
	TEST_NONE = 1 << 0,
	TEST_DPMS = 1 << 1,
	TEST_SUSPEND = 1 << 2,
	TEST_SWAP = 1 << 3,
	TEST_INVALID_METADATA_SIZES = 1 << 4,
	TEST_INVALID_HDR = 1 << 5,
	TEST_BRIGHTNESS = 1 << 6,
	TEST_NEEDS_DSC = 1 << 7,
};

/* BPC connector state. */
typedef struct output_bpc {
	unsigned int current;
	unsigned int maximum;
} output_bpc_t;

/* Common test data. */
typedef struct data {
	igt_display_t display;
	igt_plane_t *primary;
	igt_output_t *output;
	igt_crtc_t *crtc;
	igt_pipe_crc_t *pipe_crc;
	drmModeModeInfo *mode;
	int fd;
	int w;
	int h;
	igt_fb_t afb;
} data_t;

/* Common test cleanup. */
static void test_fini(data_t *data)
{
	igt_pipe_crc_free(data->pipe_crc);
	igt_remove_fb(data->fd, &data->afb);
	igt_display_reset(&data->display);
}

static void test_cycle_flags(data_t *data, uint32_t test_flags)
{
	if (test_flags & TEST_DPMS) {
		kmstest_set_connector_dpms(data->fd,
					   data->output->config.connector,
					   DRM_MODE_DPMS_OFF);
		kmstest_set_connector_dpms(data->fd,
					   data->output->config.connector,
					   DRM_MODE_DPMS_ON);
	}

	if (test_flags & TEST_SUSPEND)
		igt_system_suspend_autoresume(SUSPEND_STATE_MEM,
					      SUSPEND_TEST_NONE);
}

/* Fills the FB with a test HDR pattern. */
static void draw_hdr_pattern(igt_fb_t *fb)
{
	igt_paint_test_pattern_color_fb(fb->fd, fb, 1.0, 1.0, 1.0);
}

/* Prepare test data. */
static void prepare_test(data_t *data, igt_output_t *output, igt_crtc_t *crtc)
{
	igt_display_t *display = &data->display;

	data->crtc = crtc;
	igt_assert(data->crtc);

	igt_display_reset(display);

	data->output = output;
	igt_assert(data->output);

	data->mode = igt_output_get_mode(data->output);
	igt_assert(data->mode);

	data->primary =
		igt_crtc_get_plane_type(data->crtc, DRM_PLANE_TYPE_PRIMARY);

	data->pipe_crc = igt_crtc_crc_new(data->crtc,
					  IGT_PIPE_CRC_SOURCE_AUTO);

	igt_output_set_crtc(data->output,
			    data->crtc);
	igt_output_set_prop_value(data->output, IGT_CONNECTOR_MAX_BPC, 10);

	data->w = data->mode->hdisplay;
	data->h = data->mode->vdisplay;
}

static void test_bpc_switch_on_output(data_t *data, igt_crtc_t *crtc,
				      igt_output_t *output,
				      uint32_t format,
				      uint32_t flags)
{
	igt_display_t *display = &data->display;
	igt_crc_t ref_crc, new_crc;
	int afb_id, ret;

	/* 10-bit formats are slow, so limit the size. */
	afb_id = igt_create_fb(data->fd, 512, 512,
			       format, DRM_FORMAT_MOD_LINEAR, &data->afb);
	igt_assert(afb_id);

	draw_hdr_pattern(&data->afb);

	/* Plane may be required to fit fullscreen. Check it here and allow
	 * smaller plane size in following tests.
	 */
	igt_plane_set_fb(data->primary, &data->afb);
	if (igt_crtc_num_scalers(crtc) >= 1)
		igt_plane_set_size(data->primary, data->w, data->h);
	else
		igt_plane_set_size(data->primary, 512, 512);

	ret = igt_display_try_commit_atomic(display, DRM_MODE_ATOMIC_TEST_ONLY, NULL);
	if (!ret) {
		data->w = data->afb.width;
		data->h = data->afb.height;
	}

	/* Start in 8bpc. */
	igt_output_set_prop_value(data->output, IGT_CONNECTOR_MAX_BPC, 8);
	igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	igt_assert_output_bpc_equal(crtc,
				    output, 8);

	/*
	 * amdgpu requires a primary plane when the CRTC is enabled.
	 * However, some older Intel hardware (hsw) have scaling
	 * requirements that are not met by the plane, so remove it
	 * for non-AMD devices.
	 */
	if (!is_amdgpu_device(data->fd))
		igt_plane_set_fb(data->primary, NULL);

	/* Switch to 10bpc. */
	igt_output_set_prop_value(data->output, IGT_CONNECTOR_MAX_BPC, 10);
	igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	igt_assert_output_bpc_equal(crtc,
				    output, 10);

	/* Verify that the CRC are equal after DPMS or suspend. */
	igt_pipe_crc_collect_crc(data->pipe_crc, &ref_crc);
	test_cycle_flags(data, flags);
	igt_pipe_crc_collect_crc(data->pipe_crc, &new_crc);

	/* Drop back to 8bpc. */
	igt_output_set_prop_value(data->output, IGT_CONNECTOR_MAX_BPC, 8);
	igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	igt_assert_output_bpc_equal(crtc,
				    output, 8);

	/* CRC capture is clamped to 8bpc, so capture should match. */
	igt_assert_crc_equal(&ref_crc, &new_crc);
}

/* Returns true if an output supports max bpc property. */
static bool has_max_bpc(igt_output_t *output)
{
	return igt_output_has_prop(output, IGT_CONNECTOR_MAX_BPC) &&
	       igt_output_get_prop(output, IGT_CONNECTOR_MAX_BPC);
}

static void test_bpc_switch(data_t *data, uint32_t flags)
{
	igt_display_t *display = &data->display;
	igt_output_t *output;

	igt_display_reset(display);

	for_each_connected_output(display, output) {
		igt_crtc_t *crtc;
		unsigned int maximum;

		if (!has_max_bpc(output)) {
			igt_info("%s: Doesn't support IGT_CONNECTOR_MAX_BPC.\n",
				 igt_output_name(output));
			continue;
		}

		if (!igt_get_output_max_bpc(output, &maximum)) {
			igt_info("%s: Failed to read max bpc.\n", igt_output_name(output));
			continue;
		}

		if (maximum < 10) {
			igt_info("%s: Doesn't support 10 bpc.\n", igt_output_name(output));
			continue;
		}

		for_each_crtc(display, crtc) {
			igt_output_set_crtc(output,
					    crtc);
			if (!intel_pipe_output_combo_valid(display)) {
				igt_output_set_crtc(output, NULL);
				continue;
			}

			for (int i = 0; i < ARRAY_SIZE(hdr_test_formats); i++) {
				prepare_test(data, output, crtc);

				if (is_intel_device(data->fd) &&
				    !igt_max_bpc_constraint(display, crtc, output, 10)) {
					igt_info("%s: No suitable mode found to use 10 bpc.\n",
						 igt_output_name(output));

					test_fini(data);
					break;
				}

				data->mode = igt_output_get_mode(output);
				data->w = data->mode->hdisplay;
				data->h = data->mode->vdisplay;

				igt_dynamic_f("pipe-%s-%s-%s",
					      igt_crtc_name(crtc), output->name,
					      igt_format_str(hdr_test_formats[i]))
					test_bpc_switch_on_output(data, crtc, output,
								  hdr_test_formats[i], flags);

				test_fini(data);
			}

			/* One pipe is enough */
			break;
		}
	}
}

/* Sets the HDR output metadata prop with invalid size. */
static int set_invalid_hdr_output_metadata(data_t *data,
					   struct hdr_output_metadata const *meta,
					   size_t length)
{
	igt_output_replace_prop_blob(data->output,
				     IGT_CONNECTOR_HDR_OUTPUT_METADATA, meta,
				     meta ? length : 0);

	return igt_display_try_commit_atomic(&data->display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
}

static void adjust_brightness(data_t *data, uint32_t flags)
{
	igt_backlight_context_t context;
	int r_bright, w_bright;

	snprintf(context.path, PATH_MAX, "intel_backlight");
	snprintf(context.backlight_dir_path, PATH_MAX, "%s", BACKLIGHT_PATH);

	igt_assert(igt_backlight_read(&context.max, "max_brightness", &context) > -1);
	igt_assert(context.max);
	igt_assert(igt_backlight_read(&context.old, "brightness", &context) > -1);

	for (w_bright = 0; w_bright <= context.max ; w_bright += 50) {
		igt_assert_eq(igt_backlight_write(w_bright, "brightness", &context), 0);
		igt_display_commit_atomic(&data->display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
		igt_assert_eq(igt_backlight_read(&r_bright, "brightness", &context), 0);
		igt_assert_eq(w_bright, r_bright);
	}

	igt_assert_eq(igt_backlight_write(context.old, "brightness", &context), 0);
}

static void test_static_toggle(data_t *data, igt_crtc_t *crtc,
			       uint32_t format, uint32_t flags)
{
	igt_display_t *display = &data->display;
	struct hdr_output_metadata hdr;
	igt_crc_t ref_crc, new_crc;
	int afb_id;

	/* 10-bit formats are slow, so limit the size. */
	afb_id = igt_create_fb(data->fd, 512, 512,
			       format, DRM_FORMAT_MOD_LINEAR, &data->afb);
	igt_assert(afb_id);

	draw_hdr_pattern(&data->afb);

	igt_hdr_fill_st2084(&hdr);

	/* Start with no metadata. */
	igt_plane_set_fb(data->primary, &data->afb);
	igt_plane_set_size(data->primary, data->w, data->h);
	igt_hdr_set_metadata(data->output, NULL);
	igt_output_set_prop_value(data->output, IGT_CONNECTOR_MAX_BPC, 8);

	if (flags & TEST_NEEDS_DSC) {
		igt_force_dsc_enable(data->fd, data->output->name);
		igt_assert(igt_is_force_dsc_enabled(data->fd, data->output->name));
	}

	igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	igt_assert_output_bpc_equal(crtc,
				    data->output, 8);

	if (flags & TEST_NEEDS_DSC) {
		igt_force_dsc_disable(data->fd, data->output->name);
		igt_assert(igt_is_force_dsc_disabled(data->fd, data->output->name));
	}

	/* Apply HDR metadata and 10bpc. We expect a modeset for entering. */
	igt_hdr_set_metadata(data->output, &hdr);
	igt_output_set_prop_value(data->output, IGT_CONNECTOR_MAX_BPC, 10);
	igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	if (flags & TEST_INVALID_HDR) {
		igt_assert_eq(system("dmesg|tail -n 1000|grep -E \"Unknown EOTF [0-9]+\""), 0);
		return;
	}

	if (flags & TEST_BRIGHTNESS) {
		igt_require_f(is_intel_device(data->fd), "Only supported on Intel devices\n");
		adjust_brightness(data, flags);
	}

	igt_assert_output_bpc_equal(crtc,
				    data->output, 10);

	/* Verify that the CRC are equal after DPMS or suspend. */
	igt_pipe_crc_collect_crc(data->pipe_crc, &ref_crc);
	test_cycle_flags(data, flags);
	igt_pipe_crc_collect_crc(data->pipe_crc, &new_crc);

	/* Disable HDR metadata and drop back to 8bpc. We expect a modeset for exiting. */
	igt_hdr_set_metadata(data->output, NULL);
	igt_output_set_prop_value(data->output, IGT_CONNECTOR_MAX_BPC, 8);

	if (flags & TEST_NEEDS_DSC) {
		igt_force_dsc_enable(data->fd, data->output->name);
		igt_assert(igt_is_force_dsc_enabled(data->fd, data->output->name));
	}

	igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	igt_assert_output_bpc_equal(crtc,
				    data->output, 8);

	igt_assert_crc_equal(&ref_crc, &new_crc);

	if (flags & TEST_NEEDS_DSC) {
		igt_force_dsc_disable(data->fd, data->output->name);
		igt_assert(igt_is_force_dsc_disabled(data->fd, data->output->name));
	}
}

static void test_static_swap(data_t *data, igt_crtc_t *crtc,
			     uint32_t format, uint32_t flags)
{
	igt_display_t *display = &data->display;
	igt_crc_t ref_crc, new_crc;
	int afb_id;
	struct hdr_output_metadata hdr;

	/* 10-bit formats are slow, so limit the size. */
	afb_id = igt_create_fb(data->fd, 512, 512,
			       format, DRM_FORMAT_MOD_LINEAR, &data->afb);
	igt_assert(afb_id);

	draw_hdr_pattern(&data->afb);

	/* Start in SDR. */
	igt_plane_set_fb(data->primary, &data->afb);
	igt_plane_set_size(data->primary, data->w, data->h);
	igt_output_set_prop_value(data->output, IGT_CONNECTOR_MAX_BPC, 8);

	if (flags & TEST_NEEDS_DSC) {
		igt_force_dsc_enable(data->fd, data->output->name);
		igt_assert(igt_is_force_dsc_enabled(data->fd, data->output->name));
	}

	igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	igt_assert_output_bpc_equal(crtc,
				    data->output, 8);

	if (flags & TEST_NEEDS_DSC) {
		igt_force_dsc_disable(data->fd, data->output->name);
		igt_assert(igt_is_force_dsc_disabled(data->fd, data->output->name));
	}

	/* Enter HDR, a modeset is allowed here. */
	igt_hdr_fill_st2084(&hdr);
	igt_hdr_set_metadata(data->output, &hdr);
	igt_output_set_prop_value(data->output, IGT_CONNECTOR_MAX_BPC, 10);
	igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	igt_assert_output_bpc_equal(crtc,
				    data->output, 10);
	igt_debug_interactive_mode_check("smpte2084",
					 "HDR pattern displayed with SMPTE ST 2084 metadata");

	igt_pipe_crc_collect_crc(data->pipe_crc, &ref_crc);

	/* Change the mastering information, no modeset required
	 * as only infoframe content changes. */
	hdr.hdmi_metadata_type1.max_display_mastering_luminance = 200;
	hdr.hdmi_metadata_type1.max_fall = 200;
	hdr.hdmi_metadata_type1.max_cll = 100;

	igt_hdr_set_metadata(data->output, &hdr);
	igt_display_commit_atomic(display, 0, NULL);

	if (flags & TEST_NEEDS_DSC) {
		igt_force_dsc_enable(data->fd, data->output->name);
		igt_assert(igt_is_force_dsc_enabled(data->fd, data->output->name));
	}
	/* Enter SDR via metadata. When DSC is forced, a full modeset
	 * might be needed due to DSC parameter changes; otherwise only
	 * infoframe content changes. */
	igt_hdr_fill_sdr(&hdr);
	igt_hdr_set_metadata(data->output, &hdr);
	if (flags & TEST_NEEDS_DSC)
		igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	else
		igt_display_commit_atomic(display, 0, NULL);
	igt_debug_interactive_mode_check("traditional-sdr",
					 "SDR pattern displayed with traditional SDR metadata");

	igt_pipe_crc_collect_crc(data->pipe_crc, &new_crc);

	/* Exit SDR and enter 8bpc, cleanup. */
	igt_hdr_set_metadata(data->output, NULL);
	igt_output_set_prop_value(data->output, IGT_CONNECTOR_MAX_BPC, 8);
	igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	igt_assert_output_bpc_equal(crtc,
				    data->output, 8);

	/* Verify that the CRC didn't change while cycling metadata. */
	igt_assert_crc_equal(&ref_crc, &new_crc);

	if (flags & TEST_NEEDS_DSC) {
		igt_force_dsc_disable(data->fd, data->output->name);
		igt_assert(igt_is_force_dsc_disabled(data->fd, data->output->name));
	}
}

static void test_invalid_metadata_sizes(data_t *data)
{
	struct hdr_output_metadata hdr;
	size_t metadata_size = sizeof(hdr);

	igt_hdr_fill_st2084(&hdr);

	igt_assert_eq(set_invalid_hdr_output_metadata(data, &hdr, 1), -EINVAL);
	igt_assert_eq(set_invalid_hdr_output_metadata(data, &hdr, metadata_size + 1), -EINVAL);
	igt_assert_eq(set_invalid_hdr_output_metadata(data, &hdr, metadata_size - 1), -EINVAL);
	igt_assert_eq(set_invalid_hdr_output_metadata(data, &hdr, metadata_size * 2), -EINVAL);
}

static void test_hdr(data_t *data, uint32_t flags)
{
	igt_display_t *display = &data->display;
	igt_output_t *output;
	struct hdr_output_metadata hdr;

	igt_display_reset(display);

	for_each_connected_output(display, output) {
		igt_crtc_t *crtc;
		unsigned int maximum;

		/* To test HDR, 10 bpc is required, so we need to
		 * set MAX_BPC property to 10bpc prior to setting
		 * HDR metadata property. Therefore, checking.
		 */
		if (!has_max_bpc(output) || !igt_output_supports_hdr(output)) {
			igt_info("%s: Doesn't support IGT_CONNECTOR_MAX_BPC or IGT_CONNECTOR_HDR_OUTPUT_METADATA.\n",
				 igt_output_name(output));
			continue;
		}

		/* For negative test, panel should be non-hdr. */
		if ((flags & TEST_INVALID_HDR) && igt_is_panel_hdr(data->fd, output)) {
			igt_info("%s: Can't run negative test on HDR panel.\n",
				 igt_output_name(output));
			continue;
		}

		if (!(flags & TEST_INVALID_HDR) && !igt_is_panel_hdr(data->fd, output)) {
			igt_info("%s: Can't run HDR tests on non-HDR panel.\n",
				 igt_output_name(output));
			continue;
		}

		if (!igt_get_output_max_bpc(output, &maximum)) {
			igt_info("%s: Failed to read max bpc.\n", igt_output_name(output));
			continue;
		}

		if (maximum < 10) {
			igt_info("%s: Doesn't support 10 bpc.\n", igt_output_name(output));
			continue;
		}

		if ((flags & TEST_BRIGHTNESS) && !output_is_internal_panel(output)) {
			igt_info("%s: Can't run brightness test on non-internal panel.\n",
				 igt_output_name(output));
			continue;
		}

		for_each_crtc(display, crtc) {
			igt_output_set_crtc(output, crtc);
			if (!intel_pipe_output_combo_valid(display)) {
				igt_output_set_crtc(output, NULL);
				continue;
			}

			for (int i = 0; i < ARRAY_SIZE(hdr_test_formats); i++) {
				prepare_test(data, output,
					     crtc);

				data->mode = igt_output_get_mode(output);
				data->w = data->mode->hdisplay;
				data->h = data->mode->vdisplay;

				/* Signal HDR requirement via metadata.
				 * A framebuffer must be present for the driver to accept
				 * a metadata commit. Use TEST_ONLY so hardware state is
				 * unchanged.
				 */
				igt_create_fb(data->fd,
					      data->w, data->h,
					      hdr_test_formats[i],
					      DRM_FORMAT_MOD_LINEAR,
					      &data->afb);
				igt_plane_set_fb(data->primary, &data->afb);
				igt_plane_set_size(data->primary, data->w, data->h);
				igt_output_set_prop_value(data->output, IGT_CONNECTOR_MAX_BPC, 10);
				igt_hdr_fill_st2084(&hdr);
				igt_hdr_set_metadata(data->output, &hdr);
				if (igt_display_try_commit_atomic(display,
								  DRM_MODE_ATOMIC_TEST_ONLY |
								  DRM_MODE_ATOMIC_ALLOW_MODESET,
								  NULL)) {
					igt_info("%s: Couldn't set HDR metadata\n",
						 igt_output_name(output));
					igt_hdr_set_metadata(data->output, NULL);
					igt_plane_set_fb(data->primary, NULL);
					igt_remove_fb(data->fd, &data->afb);
					test_fini(data);
					continue;
				}

				/* Reset IGT display state; hardware was not changed. */
				igt_hdr_set_metadata(data->output, NULL);
				igt_plane_set_fb(data->primary, NULL);
				igt_remove_fb(data->fd, &data->afb);

				if (is_intel_device(data->fd) &&
				    !igt_max_bpc_constraint(display, crtc, output, 10)) {
					igt_info("%s: No suitable mode found to use 10 bpc.\n",
						 igt_output_name(output));

					test_fini(data);
					break;
				}

				if (igt_is_dsc_enabled(data->fd, output->name))
					flags |= TEST_NEEDS_DSC;
				else
					flags &= ~TEST_NEEDS_DSC;

				igt_dynamic_f("pipe-%s-%s-%s",
					      igt_crtc_name(crtc), output->name,
					      igt_format_str(hdr_test_formats[i])) {
					if (flags & (TEST_NONE | TEST_DPMS | TEST_SUSPEND |
						     TEST_INVALID_HDR | TEST_BRIGHTNESS))
						test_static_toggle(data,
								   crtc,
								   hdr_test_formats[i], flags);
					if (flags & TEST_SWAP)
						test_static_swap(data,
								 crtc,
								 hdr_test_formats[i], flags);
					if (flags & TEST_INVALID_METADATA_SIZES)
						test_invalid_metadata_sizes(data);
				}

				test_fini(data);
			}

			/* One pipe is enough */
			break;
		}
	}
}

int igt_main()
{
	data_t data = {};

	igt_fixture() {
		data.fd = drm_open_driver_master(DRIVER_ANY);

		kmstest_set_vt_graphics_mode();

		igt_display_require(&data.display, data.fd);
		igt_require(data.display.is_atomic);

		igt_display_require_output(&data.display);
	}

	igt_describe("Tests switching between different display output bpc modes");
	igt_subtest_with_dynamic("bpc-switch")
		test_bpc_switch(&data, TEST_NONE);
	igt_describe("Tests bpc switch with dpms");
	igt_subtest_with_dynamic("bpc-switch-dpms")
		test_bpc_switch(&data, TEST_DPMS);
	igt_describe("Tests bpc switch with suspend");
	igt_subtest_with_dynamic("bpc-switch-suspend")
		test_bpc_switch(&data, TEST_SUSPEND);

	igt_describe("Tests entering and exiting HDR mode");
	igt_subtest_with_dynamic("static-toggle")
		test_hdr(&data, TEST_NONE);
	igt_describe("Tests static toggle with dpms");
	igt_subtest_with_dynamic("static-toggle-dpms")
		test_hdr(&data, TEST_DPMS);
	igt_describe("Tests static toggle with suspend");
	igt_subtest_with_dynamic("static-toggle-suspend")
		test_hdr(&data, TEST_SUSPEND);

	igt_describe("Tests brightness while in HDR mode");
	igt_subtest_with_dynamic("brightness-with-hdr")
		test_hdr(&data, TEST_BRIGHTNESS);

	igt_describe("Tests swapping static HDR metadata");
	igt_subtest_with_dynamic("static-swap")
		test_hdr(&data, TEST_SWAP);

	igt_describe("Tests invalid HDR metadata sizes");
	igt_subtest_with_dynamic("invalid-metadata-sizes")
		test_hdr(&data, TEST_INVALID_METADATA_SIZES);

	igt_describe("Test to ensure HDR is not enabled on non-HDR panel");
	igt_subtest_with_dynamic("invalid-hdr") {
		igt_install_exit_handler(igt_drm_debug_mask_reset_exit_handler);
		igt_drm_debug_mask_update(DRM_UT_KMS);
		test_hdr(&data, TEST_INVALID_HDR);
		igt_drm_debug_mask_reset();
	}

	igt_fixture() {
		igt_display_fini(&data.display);
		drm_close_driver(data.fd);
	}
}
