/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2024 Collabora Ltd.
 *
 * Author: Adrian Larumbe <adrian.larumbe@collabora.com>
 *
 */

#ifndef IGT_PROFILING_H
#define IGT_PROFILING_H

struct igt_profiled_device {
	char *syspath;
	char original_state;
	char enable_state;
};

void igt_devices_configure_profiling(struct igt_profiled_device *devices, bool enable);
struct igt_profiled_device *igt_devices_profiled(void);
void igt_devices_free_profiling(struct igt_profiled_device *devices);
void igt_devices_update_original_profiling_state(struct igt_profiled_device *devices);

#endif /* IGT_PROFILING_H */
