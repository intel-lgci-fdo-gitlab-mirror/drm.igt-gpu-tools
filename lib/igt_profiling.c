// SPDX-License-Identifier: MIT
/*
 * Copyright © 2024 Collabora Ltd.
 *
 * Author: Adrian Larumbe <adrian.larumbe@collabora.com>
 *
 */

#include <unistd.h>
#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "igt_profiling.h"

#define SYSFS_DRM	"/sys/class/drm"
#define NUM_DEVICES	10

/**
 * igt_devices_profiled
 *
 * Gives us an array of igt_profiled_device structures, each of which contains
 * the full path of the DRM device's sysfs profiling knob and its original
 * state, so that it can be restored later on.
 *
 * Returns: NULL-terminated array of struct igt_profiled_device pointers, or
 * NULL on failure.
 */
struct igt_profiled_device *igt_devices_profiled(void)
{
	struct igt_profiled_device *profiled_devices;
	unsigned int devlist_len = NUM_DEVICES;
	unsigned int i = 0;
	struct dirent *entry;
	DIR *dev_dir;

	/* The return array will be resized in case there are too many devices */
	profiled_devices = malloc(devlist_len * sizeof(struct igt_profiled_device));
	if (!profiled_devices)
		return NULL;

	dev_dir = opendir(SYSFS_DRM);
	if (!dev_dir)
		goto end;

	while ((entry = readdir(dev_dir)) != NULL) {
		char path[PATH_MAX];
		char driver_path[PATH_MAX];
		char driver_link[PATH_MAX];
		char orig_state;
		ssize_t len;
		int sysfs_fd;

		/* All DRM device entries are symlinks to other paths within sysfs */
		if (entry->d_type != DT_LNK)
			continue;

		/* We're only interested in render nodes */
		if (strstr(entry->d_name, "render") != entry->d_name)
			continue;

		snprintf(path, sizeof(path), "%s/%s/device/%s",
			 SYSFS_DRM, entry->d_name, "profiling");

		if (access(path, F_OK))
			continue;

		sysfs_fd = open(path, O_RDONLY);
		if (sysfs_fd == -1)
			continue;

		if (read(sysfs_fd, &orig_state, 1) <= 0) {
			close(sysfs_fd);
			continue;
		}

		if (i == (devlist_len - 1)) {
			struct igt_profiled_device *new_profiled_devices;

			devlist_len += NUM_DEVICES;
			new_profiled_devices = realloc(profiled_devices, devlist_len);
			if (!new_profiled_devices)
				goto end;
			profiled_devices = new_profiled_devices;
		}

		profiled_devices[i].syspath = strdup(path);
		profiled_devices[i].original_state = orig_state;
		profiled_devices[i].enable_state = '1';

		snprintf(driver_path, sizeof(driver_path), "%s/%s/device/driver",
			 SYSFS_DRM, entry->d_name);

		len = readlink(driver_path, driver_link, sizeof(driver_link) - 1);
		if (len >= 0) {
			driver_link[len] = '\0';

			if (strstr(driver_link, "/panthor")) {
				/* Enable both cycle and timestamp sampling. */
				profiled_devices[i].enable_state = '3';
			}
		}

		i++;

		close(sysfs_fd);
	}

	if (i == 0)
		goto end;
	else
		profiled_devices[i].syspath = NULL; /* Array terminator */

	return profiled_devices;

end:
	free(profiled_devices);
	return NULL;
}

/**
 * igt_devices_configure_profiling
 * @devices: NULL-terminated array of igt_profiled_device structures.
 * @enable: If True, then enable profiling, otherwise restore to original state
 *
 * For every single device's profiling knob sysfs path in the NULL-terminated
 * 'devices' array, set it to '1' if bool equals true. Otherwise set it to
 * its original state at the time it was first probed in igt_devices_profiled
 *
 */
void igt_devices_configure_profiling(struct igt_profiled_device *devices, bool enable)
{
	assert(devices);

	for (unsigned int i = 0; devices[i].syspath; i++) {
		int sysfs_fd = open(devices[i].syspath, O_WRONLY);

		if (sysfs_fd < 0)
			continue;

		write(sysfs_fd, enable ? &devices[i].enable_state : &devices[i].original_state, 1);
		close(sysfs_fd);
	}
}

/**
 * igt_devices_configure_profiling
 * @devices: NULL-terminated array of igt_profiled_device structures.
 *
 * For every single struct igt_profiled_device in the 'devices' array,
 * free its duplicated syspath string, and then free the array itself.
 *
 */
void igt_devices_free_profiling(struct igt_profiled_device *devices)
{
	assert(devices);

	for (unsigned int i = 0; devices[i].syspath; i++)
		free(devices[i].syspath);

	free(devices);
}

/**
 * igt_devices_configure_profiling
 * @devices: NULL-terminated array of igt_profiled_device structures.
 *
 * For every single struct igt_profiled_device in the 'devices' array,
 * check whether the sysfs profiling knob has changed its state since
 * the last time its original state was registered, and then update it
 * accordingly. This is usually a symptom that there are other profilers
 * currently trying to toggle the sysfs knob, or perhaps more than one
 * instance of the same profiler.
 * The goal of this function is ensuring the sysfs knob is eventually
 * restored to a coherent state, even though a small race window is
 * possible. There's nothing we can do about this, so this function
 * tries to mitigate that situation in a best-effort fashion.
 *
 */
void igt_devices_update_original_profiling_state(struct igt_profiled_device *devices)
{
	assert(devices);

	for (unsigned int i = 0; devices[i].syspath; i++) {
		char new_state;
		int sysfs_fd;

		sysfs_fd = open(devices[i].syspath, O_RDWR);
		if (sysfs_fd == -1)
			continue;

		if (!read(sysfs_fd, &new_state, 1)) {
			close(sysfs_fd);
			continue;
		}

		if (new_state == '0') {
			write(sysfs_fd, "1", 1);
			devices[i].original_state = new_state;
		}

		close(sysfs_fd);
	}
}
