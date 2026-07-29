// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */

/*
 * Minimal DRM/PCI stubs for tools that need driver detection and device
 * opening but do not link the full libigt.so. Requires libdrm and pciaccess.
 *
 * This reimplements a small subset of lib/drmtest.c (__drm_open_driver()),
 * lib/igt_device.c (is_i915_device(), is_intel_device()) and
 * lib/xe/xe_query.c (xe_dev_id()) so that tools can pull in just this
 * static sub-library instead of the whole libigt.so stack.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pciaccess.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <xf86drm.h>

#include "drmtest.h"

int igt_pci_system_init(void)
{
	return pci_system_init();
}

static bool driver_name_matches(int fd, const char *name)
{
	drmVersionPtr version;
	bool match = false;

	version = drmGetVersion(fd);
	if (!version)
		return false;

	match = version->name_len > 0 && !strcmp(version->name, name);
	drmFreeVersion(version);

	return match;
}

bool is_i915_device(int fd)
{
	return driver_name_matches(fd, "i915");
}

bool is_intel_device(int fd)
{
	return is_i915_device(fd) || driver_name_matches(fd, "xe");
}

static uint16_t read_pci_device_id(int fd)
{
	char path[PATH_MAX], buf[32];
	struct stat st;
	int sysfs, len;
	unsigned int devid;

	if (fstat(fd, &st) || !S_ISCHR(st.st_mode))
		return 0;

	snprintf(path, sizeof(path), "/sys/dev/char/%d:%d/device/device",
		 major(st.st_rdev), minor(st.st_rdev));
	sysfs = open(path, O_RDONLY);
	if (sysfs < 0)
		return 0;

	len = read(sysfs, buf, sizeof(buf) - 1);
	close(sysfs);
	if (len <= 0)
		return 0;

	buf[len] = '\0';
	if (sscanf(buf, "0x%x", &devid) != 1)
		return 0;

	return devid;
}

/*
 * Forward declaration required: xe_dev_id() is declared in the (heavy)
 * lib/xe/xe_query.h, which this file intentionally does not include. The
 * declaration below keeps -Wmissing-prototypes happy for the definition
 * that follows.
 */
uint16_t xe_dev_id(int fd);
uint16_t xe_dev_id(int fd)
{
	return read_pci_device_id(fd);
}

/* Maximum /dev/dri/card<N> minor number probed by __drm_open_driver(). */
#define IGT_DRM_STUB_MAX_CARDS 16

int __drm_open_driver(int chipset)
{
	char path[PATH_MAX];
	int fd;

	for (int i = 0; i < IGT_DRM_STUB_MAX_CARDS; i++) {
		snprintf(path, sizeof(path), "/dev/dri/card%d", i);
		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;

		if ((chipset == DRIVER_INTEL && is_i915_device(fd)) ||
		    (chipset == DRIVER_XE && driver_name_matches(fd, "xe")) ||
		    (chipset == DRIVER_ANY && is_intel_device(fd)))
			return fd;

		close(fd);
	}

	errno = ENODEV;
	return -1;
}
