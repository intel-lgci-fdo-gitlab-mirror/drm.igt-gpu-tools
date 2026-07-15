// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Google
 *
 * Authors:
 *   Louis Chauvet <louis.chauvet@bootlin.com>
 */

#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <xf86drmMode.h>

#include "drmtest.h"
#include "igt_aux.h"
#include "igt_core.h"
#include "igt_kms.h"
#include "unigraf/unigraf.h"

/**
 * TEST: unigraf connectivity
 * Category: Core
 * Description: Testing connectivity with a unigraf device
 *
 * SUBTEST: unigraf-connect-edid
 * Description: Verify that the unigraf device is properly connected to the DUT
 *              and that the correct EDID is detected and read.
 *
 * SUBTEST: unigraf-connect-mst
 * Description: Ensure that the DUT can correctly detect and handle the unigraf device
 *              when it is configured to operate in MST mode.
 */

IGT_TEST_DESCRIPTION("Test basic unigraf connectivity");
int igt_main()
{
	int drm_fd;

	igt_fixture() {
		drm_fd = drm_open_driver_master(DRIVER_ANY);
	}

	igt_describe("Make sure that the unigraf device is connected to the DUT and EDID is properly detected");
	igt_subtest("unigraf-connect-edid") {
		drmModePropertyBlobPtr edid_blob = NULL;
		igt_display_t display;
		uint64_t edid_blob_id;
		igt_output_t *output;
		uint32_t unigraf_edid_len;
		void *unigraf_edid;
		bool found = false;

		/*
		 * Sleep are required to allow hardware to configure/detect the current
		 * configuration
		 */
		unigraf_require_device(drm_fd);
		unigraf_hpd_deassert();
		sleep(igt_default_display_detect_timeout());
		unigraf_set_sst();
		unigraf_hpd_assert();
		sleep(igt_default_display_detect_timeout());
		igt_display_require(&display, drm_fd);
		sleep(igt_default_display_detect_timeout());

		unigraf_edid = unigraf_read_edid(0, &unigraf_edid_len);

		for_each_connected_output(&display, output) {
			if (output->config.connector->connector_type !=
			    DRM_MODE_CONNECTOR_DisplayPort)
				continue;
			igt_assert(kmstest_get_property(drm_fd,
							output->config.connector->connector_id,
							DRM_MODE_OBJECT_CONNECTOR, "EDID",
							NULL, &edid_blob_id, NULL));
			edid_blob = drmModeGetPropertyBlob(drm_fd, edid_blob_id);
			if (!edid_blob)
				continue;

			if (!memcmp(unigraf_edid, edid_blob->data,
				    min(edid_blob->length, unigraf_edid_len)))
				found = true;

			drmModeFreePropertyBlob(edid_blob);

			if (found)
				break;
		}
		igt_assert_f(found, "No output with the correct EDID was found\n");

		free(unigraf_edid);
	}

	igt_describe("Make sure that the unigraf device can be used as a MST device");
	igt_subtest("unigraf-connect-mst") {
		int newly_connected_count, already_connected_count, diff_len;
		uint32_t *newly_connected = NULL, *already_connected = NULL, *diff = NULL;
		int max_count;

		unigraf_require_device(drm_fd);
		max_count = unigraf_get_mst_stream_max_count();

		unigraf_hpd_deassert();

		already_connected_count = igt_get_connected_connectors(drm_fd, &already_connected);

		igt_debug("Already connected count: %d\n", already_connected_count);

		/* i = 0 is SST so we need to process max_count + 1 streams */
		for (int i = 0; i <= max_count; i++) {
			unigraf_hpd_deassert();
			/* Let the hardware detect the new state  */
			sleep(igt_default_display_detect_timeout());

			unigraf_set_mst_stream_count(max(i, 1));
			if (!i)
				unigraf_set_sst();
			else
				unigraf_set_mst();

			unigraf_hpd_assert();
			/* Let the hardware detect the new state */
			sleep(igt_default_display_detect_timeout());

			newly_connected_count = kms_wait_for_new_connectors(&newly_connected,
									    already_connected,
									    already_connected_count,
									    drm_fd);

			diff_len = get_array_diff(newly_connected, newly_connected_count,
						  already_connected, already_connected_count, &diff);

			igt_assert_f(diff_len == max(i, 1),
				     "Invalid connected connector count, expected %d found %d\n",
				     max(i, 1), diff_len);
		}

		free(already_connected);
		free(newly_connected);
		free(diff);
	}

	igt_fixture() {
		close(drm_fd);
	}
}
