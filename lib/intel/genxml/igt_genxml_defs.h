/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2016 Intel Corporation
 * Copyright © 2026 Intel Corporation
 *
 * Self-contained header providing all definitions that genxml-generated
 * pack headers need.  Replaces Mesa's util/bitpack_helpers.h and
 * genX_cl_helpers.h so that IGT can use genxml without pulling in Mesa's
 * build system.
 */

#ifndef IGT_GENXML_DEFS_H
#define IGT_GENXML_DEFS_H

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* -- compiler helpers ----------------------------------------------- */

#ifndef ALWAYS_INLINE
#define ALWAYS_INLINE inline __attribute__((always_inline))
#endif

#ifndef UNUSED
#define UNUSED __attribute__((unused))
#endif

#ifndef CLAMP
#define CLAMP(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))
#endif

/* -- bit-field helpers ---------------------------------------------- */

#ifndef BITFIELD64_BIT
#define BITFIELD64_BIT(b)  (1ull << (b))
#endif

#ifndef BITFIELD64_MASK
#define BITFIELD64_MASK(b) \
	((b) == 64 ? (~0ull) : BITFIELD64_BIT(b) - 1)
#endif

/* -- int-range helpers (Mesa u_math.h equivalents) ------------------ */

static inline ALWAYS_INLINE int64_t
u_intN_min(unsigned bits)
{
	return -(INT64_C(1) << (bits - 1));
}

static inline ALWAYS_INLINE int64_t
u_intN_max(unsigned bits)
{
	return (INT64_C(1) << (bits - 1)) - 1;
}

static inline ALWAYS_INLINE uint64_t
u_uintN_max(unsigned bits)
{
	return (bits == 64) ? UINT64_MAX : (UINT64_C(1) << bits) - 1;
}

/* -- validation hook (no-op outside Valgrind) ----------------------- */

#ifndef util_bitpack_validate_value
#define util_bitpack_validate_value(x)
#endif

/* -- bitpack functions ---------------------------------------------- */

ALWAYS_INLINE static uint64_t
util_bitpack_ones(uint32_t start, uint32_t end)
{
	return (UINT64_MAX >> (64 - (end - start + 1))) << start;
}

ALWAYS_INLINE static uint64_t
util_bitpack_uint(uint64_t v, uint32_t start, uint32_t end)
{
	const int bits = end - start + 1;
	if (bits < 64) {
		const uint64_t max = u_uintN_max(bits);
		assert(v <= max);
	}
	util_bitpack_validate_value(v);
	return v << start;
}

ALWAYS_INLINE static uint64_t
util_bitpack_uint_nonzero(uint64_t v, uint32_t start, uint32_t end)
{
	assert(v != 0ull);
	return util_bitpack_uint(v, start, end);
}

ALWAYS_INLINE static uint64_t
util_bitpack_sint(int64_t v, uint32_t start, uint32_t end)
{
	const int bits = end - start + 1;
	const uint64_t mask = BITFIELD64_MASK(bits);
	util_bitpack_validate_value(v);
	if (bits < 64) {
		const int64_t min = u_intN_min(bits);
		const int64_t max = u_intN_max(bits);
		assert(min <= v && v <= max);
	}
	return (v & mask) << start;
}

ALWAYS_INLINE static uint64_t
util_bitpack_sint_nonzero(int64_t v, uint32_t start, uint32_t end)
{
	assert(v != 0ll);
	return util_bitpack_sint(v, start, end);
}

ALWAYS_INLINE static uint32_t
util_bitpack_float(float v)
{
	union { float f; uint32_t dw; } x;
	util_bitpack_validate_value(v);
	x.f = v;
	return x.dw;
}

ALWAYS_INLINE static uint32_t
util_bitpack_float_nonzero(float v)
{
	assert(v != 0.0f);
	return util_bitpack_float(v);
}

ALWAYS_INLINE static uint64_t
util_bitpack_sfixed(float v, uint32_t start, uint32_t end,
		    uint32_t fract_bits)
{
	const float factor = (1 << fract_bits);
	const int64_t int_val = llroundf(v * factor);
	const uint64_t mask = UINT64_MAX >> (64 - (end - start + 1));
	util_bitpack_validate_value(v);
	{
		const int total_bits = end - start + 1;
		const float min = u_intN_min(total_bits) / factor;
		const float max = u_intN_max(total_bits) / factor;
		assert(min <= v && v <= max);
	}
	return (int_val & mask) << start;
}

ALWAYS_INLINE static uint64_t
util_bitpack_sfixed_clamp(float v, uint32_t start, uint32_t end,
			  uint32_t fract_bits)
{
	const float factor = (1 << fract_bits);
	const int total_bits = end - start + 1;
	const float min = u_intN_min(total_bits) / factor;
	const float max = u_intN_max(total_bits) / factor;
	const int64_t int_val = llroundf(CLAMP(v, min, max) * factor);
	const uint64_t mask = UINT64_MAX >> (64 - (end - start + 1));
	util_bitpack_validate_value(v);
	return (int_val & mask) << start;
}

ALWAYS_INLINE static uint64_t
util_bitpack_sfixed_nonzero(float v, uint32_t start, uint32_t end,
			    uint32_t fract_bits)
{
	assert(v != 0.0f);
	return util_bitpack_sfixed(v, start, end, fract_bits);
}

ALWAYS_INLINE static uint64_t
util_bitpack_ufixed(float v, uint32_t start, uint32_t end,
		    uint32_t fract_bits)
{
	const float factor = (1 << fract_bits);
	const uint64_t uint_val = llroundf(v * factor);
	util_bitpack_validate_value(v);
	{
		const int total_bits = end - start + 1;
		const float min = 0.0f;
		const float max = u_uintN_max(total_bits) / factor;
		assert(min <= v && v <= max);
	}
	return uint_val << start;
}

ALWAYS_INLINE static uint64_t
util_bitpack_ufixed_clamp(float v, uint32_t start, uint32_t end,
			  uint32_t fract_bits)
{
	const float factor = (1 << fract_bits);
	const int total_bits = end - start + 1;
	const float min = 0.0f;
	const float max = u_uintN_max(total_bits) / factor;
	const uint64_t uint_val = llroundf(CLAMP(v, min, max) * factor);
	util_bitpack_validate_value(v);
	return uint_val << start;
}

ALWAYS_INLINE static uint64_t
util_bitpack_ufixed_nonzero(float v, uint32_t start, uint32_t end,
			    uint32_t fract_bits)
{
	assert(v != 0.0f);
	return util_bitpack_ufixed(v, start, end, fract_bits);
}

/* -- address type and combine function ------------------------------ */

struct igt_address {
	uint64_t offset;          /* full GPU address to pack into dwords */
	uint32_t handle;          /* GEM handle; 0 = no reloc needed */
	uint32_t read_domains;
	uint32_t write_domain;
	uint64_t presumed_offset; /* BO base address (for computing bo_delta) */
};

struct intel_bb;

/*
 * Declared here so __gen_combine_address can call it.
 * Also declared in intel_batchbuffer.h; suppress the warning
 * when both headers are included.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wredundant-decls"
uint64_t intel_bb_offset_reloc_with_delta(struct intel_bb *ibb,
					  uint32_t handle,
					  uint32_t read_domains,
					  uint32_t write_domain,
					  uint32_t delta,
					  uint32_t offset,
					  uint64_t presumed_offset);
#pragma GCC diagnostic pop

#define __gen_address_type  struct igt_address
#define __gen_user_data     struct intel_bb

/*
 * __gen_combine_address - called by genxml pack functions for every
 * address field.  If the address carries a GEM handle, automatically
 * register a relocation with intel_bb.
 *
 * @ibb:      batch buffer (passed as __gen_user_data*)
 * @location: pointer to the dword(s) being written in the batch
 * @addr:     address descriptor (offset + optional reloc metadata)
 * @delta:    flag bits that genxml packed below the address field
 */
static inline ALWAYS_INLINE uint64_t
__gen_combine_address(struct intel_bb *ibb,
		      void *location,
		      struct igt_address addr, uint32_t delta)
{
	if (addr.handle) {
		uint32_t batch_offset =
			(uint8_t *)location - (uint8_t *)ibb->batch;
		uint32_t bo_delta =
			(uint32_t)(addr.offset - addr.presumed_offset);

		intel_bb_offset_reloc_with_delta(ibb, addr.handle,
						 addr.read_domains,
						 addr.write_domain,
						 bo_delta | delta,
						 batch_offset,
						 addr.presumed_offset);
	}

	return addr.offset | delta;
}

/* -- validation hook (no-op) ---------------------------------------- */

#ifndef __gen_validate_value
#define __gen_validate_value(x)
#endif

/* -- offset helper -------------------------------------------------- */

static inline ALWAYS_INLINE uint64_t
__gen_offset(uint64_t v, uint32_t start, uint32_t end)
{
	uint64_t mask = (~0ull >> (64 - (end - start + 1))) << start;
	assert((v & ~mask) == 0);
	__gen_validate_value(v);
	return v;
}

static inline ALWAYS_INLINE uint64_t
__gen_offset_nonzero(uint64_t v, uint32_t start, uint32_t end)
{
	assert(v != 0ull);
	return __gen_offset(v, start, end);
}

/* -- address helper ------------------------------------------------- */

static inline ALWAYS_INLINE uint64_t
__gen_address(__gen_user_data *data, void *location,
	      __gen_address_type address, uint32_t delta,
	      __attribute__((unused)) uint32_t start, uint32_t end)
{
	uint64_t addr_u64 = __gen_combine_address(data, location, address, delta);
	if (end == 31) {
		return addr_u64;
	} else if (end < 63) {
		const unsigned shift = 63 - end;
		return (addr_u64 << shift) >> shift;
	} else {
		return addr_u64;
	}
}

#endif /* IGT_GENXML_DEFS_H */
