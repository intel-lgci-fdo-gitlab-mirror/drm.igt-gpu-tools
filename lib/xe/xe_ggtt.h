/* SPDX-License-Identifier: MIT */
/*
 * Copyright(c) 2026 Intel Corporation. All rights reserved.
 */

#ifndef __XE_GGTT_H__
#define __XE_GGTT_H__

#include <stdint.h>

#define GGTT_PTE_ADDR_MASK		GENMASK_ULL(45, 12)
#define TGL_GGTT_PTE_ADDR_MASK		GENMASK_ULL(38, 12)
#define   GGTT_PTE_ADDR_SHIFT		12

typedef uint64_t xe_ggtt_pte_t;
typedef uint64_t xe_ggtt_pte_mask_t;

xe_ggtt_pte_mask_t xe_ggtt_get_gpa_mask(int pf_fd);
uint64_t xe_ggtt_pte_get_gpa(int pf_fd, xe_ggtt_pte_t pte);

#endif /* __XE_GGTT_H__ */
