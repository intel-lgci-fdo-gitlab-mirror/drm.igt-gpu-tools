// SPDX-License-Identifier: MIT
/*
 * Copyright(c) 2026 Intel Corporation. All rights reserved.
 */

#include "igt.h"
#include "xe/xe_ggtt.h"

xe_ggtt_pte_mask_t xe_ggtt_get_vfid_mask(int pf_fd)
{
	uint16_t dev_id = intel_get_drm_devid(pf_fd);

	return (intel_graphics_ver(dev_id) >= IP_VER(12, 50)) ?
		GGTT_PTE_VFID_MASK : TGL_GGTT_PTE_VFID_MASK;
}

xe_ggtt_pte_mask_t xe_ggtt_get_gpa_mask(int pf_fd)
{
	if (IS_TIGERLAKE(intel_get_drm_devid(pf_fd)))
		return TGL_GGTT_PTE_ADDR_MASK;

	return GGTT_PTE_ADDR_MASK;
}

uint8_t xe_ggtt_pte_get_vfid(int pf_fd, xe_ggtt_pte_t pte)
{
	xe_ggtt_pte_mask_t mask = xe_ggtt_get_vfid_mask(pf_fd);

	return (uint8_t)((pte & mask) >> GGTT_PTE_VFID_SHIFT);
}

uint64_t xe_ggtt_pte_get_gpa(int pf_fd, xe_ggtt_pte_t pte)
{
	xe_ggtt_pte_mask_t mask = xe_ggtt_get_gpa_mask(pf_fd);

	return (uint64_t)((pte & mask) >> GGTT_PTE_ADDR_SHIFT);
}
