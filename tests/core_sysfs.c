// SPDX-License-Identifier: MIT
/*
 * Copyright © 2025 Intel Corporation
 */

#include <fcntl.h>
#include <sys/stat.h>

#include "igt.h"
#include "igt_dir.h"
#include "igt_sysfs.h"

/**
 * TEST: sysfs test
 * Description: Read entries from sysfs path.
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: uapi
 * Functionality: sysfs
 * Feature: core
 * Test category: uapi
 *
 * SUBTEST: read-all-entries
 * Description: Read all entries from sysfs path
 *
 */

IGT_TEST_DESCRIPTION("Read entries from sysfs paths.");

static int not_read_count;
static int ok_count;
static int read_fails;
static int open_fails;
static int stat_fails;

static int read_sysfs(const char *filename, void *data)
{
	struct stat sb;
	ssize_t bytes_read;
	char buf[4096];
	int len;
	int fd;

	len = data ? to_user_pointer(data) + 1 : 0;
	if (strlen(filename) <= len || len < 0)
		len = 0;

	if (lstat(filename, &sb) == -1) {
		igt_info("Stat fails errno=%d %s\n", errno, filename);
		stat_fails++;
		return 0;
	}

	if ((sb.st_mode & S_IFMT) != S_IFREG) {
		igt_info("Not a regular file %s\n", filename);
		not_read_count++;
		return 0;
	}

	if (!(sb.st_mode & 0444)) {
		igt_info("Not readable %s\n", filename);
		not_read_count++;
		return 0;
	}

	fd = open(filename, O_RDONLY);
	if (fd < 0) {
		igt_info("Open fails errno=%d %o %s\n", errno, sb.st_mode & 0777, filename);
		open_fails++;

		return 0;
	}

	bytes_read = read(fd, buf, sizeof(buf) - 1);
	if (bytes_read < 0) {
		if (errno == EIO && strstr(filename, "/power/")) {
			igt_info("power management EIO %s\n", filename);
			not_read_count++;
		} else {
			igt_info("Read fails errno=%d %s\n", errno, filename);
			read_fails++;
		}

		close(fd);

		return 0;
	}

	buf[bytes_read] = '\0';
	igt_info("ok %zd\t%s\n", bytes_read, filename + len);
	ok_count++;

	igt_debug("File %s\nData: %s\n", filename + len, buf);
	close(fd);

	return 0;
}

static int process_sysfs(int dirfd)
{
	char syspath[PATH_MAX];
	igt_dir_t *config;
	int ret;

	ret = igt_dir_get_fd_path(dirfd, syspath, PATH_MAX - 1);
	igt_assert_f(ret >= 0, "Cannot get path\n");
	igt_info("Reading sysfs files from %s\n", syspath);

	config = igt_dir_create(dirfd);
	igt_assert_f(config, "Cannot create dir config\n");

	ret = igt_dir_scan_dirfd(config, -1);
	igt_assert_f(ret >= 0, "Cannot scan dir\n");

	ret = igt_dir_process_files(config, read_sysfs,
				    from_user_pointer(strlen(syspath)));

	igt_dir_destroy(config);

	return ret;
}

int igt_main()
{
	int fd = -1;
	int sysfs = -1;

	igt_fixture() {
		fd = drm_open_driver_master(DRIVER_ANY);
		sysfs = igt_sysfs_open(fd);
		igt_require(sysfs >= 0);

		kmstest_set_vt_graphics_mode();
	}

	igt_describe("Read all entries from sysfs path.");
	igt_subtest("read-all-entries") {
		process_sysfs(sysfs);
		igt_info("Summary: reads=%d fails=%d not_read=%d\n", ok_count,
			 read_fails + open_fails + stat_fails,
			 not_read_count);

		igt_assert_eq(read_fails, 0);
		igt_assert_eq(open_fails, 0);
		igt_assert_eq(stat_fails, 0);
	}

	igt_fixture() {
		close(sysfs);
		drm_close_driver(fd);
	}
}
