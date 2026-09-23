// SPDX-License-Identifier: GPL-2.0 or MIT
/* Copyright (c) 2026 Imagination Technologies Ltd. All Rights Reserved */

/* TODO: This file is currently hand-maintained. However, the intention is to
 * auto-generate it in the future based on the hwdefs.
 */

#include <assert.h>
#include <errno.h>
#include <string.h>

#include "drmtest.h"
#include "pvr_device_info.h"

#include "device_info/g6110.h"
#include "device_info/gx6250.h"
#include "device_info/gx6650.h"
#include "device_info/ge7800.h"
#include "device_info/ge8300.h"
#include "device_info/axe-1-16m.h"
#include "device_info/bxe-2-32.h"
#include "device_info/bxe-4-32.h"
#include "device_info/bxm-4-64.h"
#include "device_info/bxs-4-64.h"

static const struct pvr_device_info *device_infos[] = {
	/* clang-format off */
	&pvr_device_info_4_40_2_51,
	&pvr_device_info_4_45_2_58,
	&pvr_device_info_4_46_6_62,
	&pvr_device_info_5_9_1_46,
	&pvr_device_info_15_5_1_64,
	&pvr_device_info_22_67_54_30,
	&pvr_device_info_22_68_54_30,
	&pvr_device_info_22_102_54_38,
	&pvr_device_info_33_15_11_3,
	&pvr_device_info_36_29_52_182,
	&pvr_device_info_36_50_54_182,
	&pvr_device_info_36_52_104_182,
	&pvr_device_info_36_53_104_796,
	&pvr_device_info_36_56_104_183,
	/* clang-format on */
};

/**
 * Initialize PowerVR device information.
 *
 * \param info Device info structure to initialize.
 * \param bvnc Packed BVNC.
 * \return
 *  * 0 on success, or
 *  * -%ENODEV if the device is not supported.
 */
int pvr_device_info_init(struct pvr_device_info *info, uint64_t bvnc)
{
#define CASE_PACKED_BVNC_DEVICE_INFO(_b, _v, _n, _c)			\
	case PVR_BVNC_PACK(_b, _v, _n, _c):				\
		*info = pvr_device_info_##_b##_##_v##_##_n##_##_c;	\
		return 0

	switch (bvnc) {
		CASE_PACKED_BVNC_DEVICE_INFO(4, 40, 2, 51);
		CASE_PACKED_BVNC_DEVICE_INFO(4, 45, 2, 58);
		CASE_PACKED_BVNC_DEVICE_INFO(4, 46, 6, 62);
		CASE_PACKED_BVNC_DEVICE_INFO(5, 9, 1, 46);
		CASE_PACKED_BVNC_DEVICE_INFO(15, 5, 1, 64);
		CASE_PACKED_BVNC_DEVICE_INFO(22, 67, 54, 30);
		CASE_PACKED_BVNC_DEVICE_INFO(22, 68, 54, 30);
		CASE_PACKED_BVNC_DEVICE_INFO(22, 102, 54, 38);
		CASE_PACKED_BVNC_DEVICE_INFO(33, 15, 11, 3);
		CASE_PACKED_BVNC_DEVICE_INFO(36, 29, 52, 182);
		CASE_PACKED_BVNC_DEVICE_INFO(36, 50, 54, 182);
		CASE_PACKED_BVNC_DEVICE_INFO(36, 52, 104, 182);
		CASE_PACKED_BVNC_DEVICE_INFO(36, 53, 104, 796);
		CASE_PACKED_BVNC_DEVICE_INFO(36, 56, 104, 183);
	}

#undef CASE_PACKED_BVNC_DEVICE_INFO

	assert(!"Unsupported Device");

	return -ENODEV;
}

/**
 * Initialize PowerVR device information from a public name.
 *
 * \param info Device info structure to initialize.
 * \param public_name Device public name.
 * \return True if successful.
 */
bool pvr_device_info_init_public_name(struct pvr_device_info *info,
				      const char *public_name)
{
	for (unsigned int d = 0; d < ARRAY_SIZE(device_infos); ++d) {
		if (strcasecmp(public_name, device_infos[d]->ident.public_name))
			continue;

		memcpy(info, device_infos[d], sizeof(*info));
		return true;
	}

	return false;
}
