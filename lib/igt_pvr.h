/* SPDX-License-Identifier: GPL-2.0 or MIT */
/* Copyright (c) 2026 Imagination Technologies Ltd. All Rights Reserved */

#ifndef IGT_PVR_H
#define IGT_PVR_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "pvr_drm.h"

uint32_t igt_pvr_ioctl_create_bo(int fd, size_t *size);
off_t igt_pvr_ioctl_get_bo_mmap_offset(int fd, uint32_t handle);

#endif /* IGT_PVR_H */
