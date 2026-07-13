/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef _INTEL_MOCS_H
#define _INTEL_MOCS_H

#include <stdint.h>

#define DEFAULT_MOCS_INDEX ((uint8_t)-1)
#define DISPLAYABLE_MOCS_INDEX ((uint8_t)-2)

uint8_t intel_get_wb_mocs_index(int fd);
uint8_t intel_get_wb_mocs(int fd);
uint8_t intel_get_uc_mocs(int fd);
uint8_t intel_get_uc_mocs_index(int fd);
uint8_t intel_get_displayable_mocs_index(int fd);
uint8_t intel_get_defer_to_pat_mocs_index(int fd);

#endif /* _INTEL_MOCS_H */
