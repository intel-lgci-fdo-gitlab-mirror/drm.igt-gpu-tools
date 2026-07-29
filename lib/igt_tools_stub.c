/*
 * Copyright © 2020 Intel Corporation
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
 */
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "igt_core.h"
#include "igt_debugfs.h"

/*
 * Forward declaration only - avoids pulling in the heavy igt_gt.h header.
 * The type is used as an opaque pointer; this stub always returns -1.
 */
struct pci_device;

int igt_open_forcewake_handle_for_pcidev(const struct pci_device *pci_dev);

/* Stub for igt_log: print message to stderr; domain and log level are ignored. */
void igt_log(const char *domain, enum igt_log_level level, const char *format, ...)
{
	va_list args;

	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
}

/* Stub for __igt_fail_assert: print failed assertion to stderr and exit. */
void __igt_fail_assert(const char *domain, const char *file,
		       const int line, const char *func, const char *assertion,
		       const char *format, ...)
{
	fprintf(stderr, "%s: %d %s Failed assertion: %s\n", file, line,
		func, assertion);
	exit(1);
}

/* Stub for __igt_skip_check: print skip reason to stderr and exit with skip code. */
void __igt_skip_check(const char *file, const int line,
		      const char *func, const char *check, const char *format, ...)
{
	fprintf(stderr, "%s: %d %s Skipped: %s", file, line, func, check);
	if (format) {
		va_list args;

		fprintf(stderr, ": ");
		va_start(args, format);
		vfprintf(stderr, format, args);
		va_end(args);
	}

	fprintf(stderr, "\n");
	/* IGT_EXIT_SKIP (77) is the exit code igt_core.c uses for skipped tests. */
	exit(IGT_EXIT_SKIP);
}

/* Stub for igt_exit: tools use exit(0) rather than the full igt teardown sequence. */
__noreturn void igt_exit(void)
{
	exit(0);
}

/*
 * Stub for igt_debugfs_open: open a debugfs entry for a DRM device directly
 * by path. device == -1 follows the igt_debugfs_path() convention of
 * defaulting to the first DRM device (dri/0). When a valid fd is given, the
 * DRM minor is resolved via fstat() so the correct dri/<minor> directory is
 * used.
 */
int igt_debugfs_open(int device, const char *filename, int mode)
{
	char path[PATH_MAX];
	int drm_minor = 0;

	if (device >= 0) {
		struct stat st;

		if (fstat(device, &st) == 0 && S_ISCHR(st.st_mode))
			drm_minor = minor(st.st_rdev);
	}

	snprintf(path, sizeof(path),
		 "/sys/kernel/debug/dri/%d/%s", drm_minor, filename);
	return open(path, mode);
}

/*
 * Stub for igt_open_forcewake_handle_for_pcidev: in tool context the driver
 * keeps the GPU awake; no forcewake handle needed.
 */
int igt_open_forcewake_handle_for_pcidev(const struct pci_device *pci_dev)
{
	return -1;
}
