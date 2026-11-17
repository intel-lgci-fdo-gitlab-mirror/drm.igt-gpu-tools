/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2026 Advanced Micro Devices, Inc.
 */

#ifndef AMD_VPE_H
#define AMD_VPE_H

#include <amdgpu.h>

/* color space conversion commands
 * source:      1024(pitch 1024) x 256 RGBA
 * destination: 1024(pitch 1024) x 256 ARGB
 */

#define SRC_PLANE_PATTERN	0x12345678
#define DST_PLANE_PATTERN	0xff123456

#define VPE1_VPEC_CSC_CMD_LENGTH	12
#define VPE1_VPEP_CSC_CMD_LENGTH	154
#define VPE1_VPEC_CSC_CMD_SIZE	(VPE1_VPEC_CSC_CMD_LENGTH * sizeof(uint32_t))
#define VPE1_VPEP_CSC_CMD_SIZE	(VPE1_VPEP_CSC_CMD_LENGTH * sizeof(uint32_t))

/* horizontal mirror commands
 * source:      1024(pitch 1024) x 256 RGBA
 * destination: 1024(pitch 1024) x 256 RGBA
 */

#define LEFT_PLANE_PATTERN	0xff123456
#define RIGHT_PLANE_PATTERN	0xff654321

#define VPE2_VPEC_HMIRROR_CMD_LENGTH	38
#define VPE2_VPEP_HMIRROR_CMD_LENGTH	1661
#define VPE2_VPEC_HMIRROR_CMD_SIZE	(VPE2_VPEC_HMIRROR_CMD_LENGTH * sizeof(uint32_t))
#define VPE2_VPEP_HMIRROR_CMD_SIZE	(VPE2_VPEP_HMIRROR_CMD_LENGTH * sizeof(uint32_t))

#define VPE3_VPEC_HMIRROR_CMD_LENGTH	56
#define VPE3_VPEP_HMIRROR_CMD_LENGTH	2601
#define VPE3_VPEC_HMIRROR_CMD_SIZE	(VPE3_VPEC_HMIRROR_CMD_LENGTH * sizeof(uint32_t))
#define VPE3_VPEP_HMIRROR_CMD_SIZE	(VPE3_VPEP_HMIRROR_CMD_LENGTH * sizeof(uint32_t))

uint32_t *vpe1_get_vpec_csc_cmd(void);
uint32_t *vpe1_get_vpep_csc_cmd(void);
uint32_t *vpe2_get_vpec_hmirror_cmd(void);
uint32_t *vpe2_get_vpep_hmirror_cmd(void);
uint32_t *vpe3_get_vpec_hmirror_cmd(void);
uint32_t *vpe3_get_vpep_hmirror_cmd(void);

#endif /* AMD_VPE_H */
