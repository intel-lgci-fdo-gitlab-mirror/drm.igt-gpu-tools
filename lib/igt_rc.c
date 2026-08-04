// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */

#include <stdio.h>
#include <stdlib.h>

#ifndef ANDROID
#include <glib.h>
#else
#include "android/glib.h"
#endif

#include "igt_rc.h"

/**
 * igt_load_igtrc:
 *
 * Load .igtrc from the path pointed to by #IGT_CONFIG_PATH or from
 * home directory if that is not set. The returned keyfile needs to be
 * deallocated using g_key_file_free().
 *
 * Returns: Pointer to the keyfile, NULL on error.
 */
GKeyFile *igt_load_igtrc(void)
{
	char *key_file_env = NULL;
	char *key_file_loc = NULL;
	GError *error = NULL;
	GKeyFile *file;
	int ret;

	/* Determine igt config path */
	key_file_env = getenv("IGT_CONFIG_PATH");
	if (key_file_env) {
		key_file_loc = key_file_env;
	} else {
		key_file_loc = malloc(100);
		snprintf(key_file_loc, 100, "%s/.igtrc", g_get_home_dir());
	}

	/* Load igt config file */
	file = g_key_file_new();
	ret = g_key_file_load_from_file(file, key_file_loc,
					G_KEY_FILE_NONE, &error);
	if (!ret) {
		g_error_free(error);
		g_key_file_free(file);
		file = NULL;

		goto out;
	}

	g_clear_error(&error);

 out:
	if (!key_file_env && key_file_loc)
		free(key_file_loc);

	return file;
}
