/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2026 Intel Corporation
 *
 * Emit / pack macros for using genxml-generated pack headers with
 * IGT's intel_bb batch-buffer infrastructure.
 *
 * Relocations are handled automatically: when an address field in a
 * genxml struct is assigned a struct igt_address with a non-zero
 * handle, __gen_combine_address (in igt_genxml_defs.h) registers the
 * relocation during packing.
 */

#ifndef IGT_GENXML_H
#define IGT_GENXML_H

#include <stdint.h>
#include "igt_core.h"
#include "intel_batchbuffer.h"
#include "intel_bufops.h"
#include "igt_genxml_defs.h"

/*
 * igt_genxml_emit - emit a fixed-length GPU command into the batch.
 *
 * Usage:
 *   igt_genxml_emit(ibb, GFX9_3DSTATE_CLIP, clip) {
 *       clip.MaximumVPIndex = 0;
 *       ...
 *   }
 *
 * The struct is zero-initialised with the default header fields, packed
 * into the batch at the current pointer, and the pointer is advanced by
 * cmd##_length dwords.
 */
#define igt_genxml_emit(ibb, cmd, name)                                    \
	for (struct cmd name = { cmd##_header },                           \
	     *_dst = (struct cmd *)intel_bb_ptr(ibb);                      \
	     __builtin_expect(_dst != NULL, 1);                            \
	     ({                                                            \
		     cmd##_pack((ibb), (void *)_dst, &name);               \
		     intel_bb_ptr_add((ibb), cmd##_length * 4);            \
		     _dst = NULL;                                          \
	     }))

/*
 * igt_genxml_pack_state - pack a state object at an arbitrary location.
 *
 * Usage:
 *   void *ptr = intel_bb_ptr_align(ibb, 64);
 *   igt_genxml_pack_state(ibb, GFX9_RENDER_SURFACE_STATE, ptr, rss) {
 *       rss.SurfaceType = GFX9_SURFTYPE_2D;
 *       rss.SurfaceBaseAddress = igt_address_of(buf, 0, rd, wd);
 *       ...
 *   }
 *   intel_bb_ptr_add(ibb, GFX9_RENDER_SURFACE_STATE_length * 4);
 *
 * Address fields with a non-zero handle get their relocations
 * registered automatically during packing.
 */
#define igt_genxml_pack_state(ibb, cmd, dst_ptr, name)                     \
	for (struct cmd name = { 0 },                                      \
	     *_done = (struct cmd *)1;                                     \
	     __builtin_expect(_done != NULL, 1);                           \
	     ({                                                            \
		     cmd##_pack((ibb), (void *)(dst_ptr), &name);          \
		     _done = NULL;                                         \
	     }))

/*
 * igt_address_of - construct an igt_address for a buffer object.
 *
 * @buf:          intel_buf owning the BO
 * @bo_offset:    offset within the BO (e.g. surface[0].offset, cc.offset)
 * @read_domains: GEM read domains
 * @write_domain: GEM write domain
 */
static inline struct igt_address
igt_address_of(const struct intel_buf *buf, uint64_t bo_offset,
	       uint32_t read_domains, uint32_t write_domain)
{
	return (struct igt_address){
		.offset = buf->addr.offset + bo_offset,
		.handle = buf->handle,
		.read_domains = read_domains,
		.write_domain = write_domain,
		.presumed_offset = buf->addr.offset,
	};
}

/*
 * igt_address_of_batch - construct an igt_address pointing into the
 * batch buffer itself (e.g. for STATE_BASE_ADDRESS).
 *
 * @ibb:          batch buffer
 * @read_domains: GEM read domains
 * @write_domain: GEM write domain
 */
static inline struct igt_address
igt_address_of_batch(struct intel_bb *ibb,
		     uint32_t read_domains, uint32_t write_domain)
{
	return (struct igt_address){
		.offset = ibb->batch_offset,
		.handle = ibb->handle,
		.read_domains = read_domains,
		.write_domain = write_domain,
		.presumed_offset = ibb->batch_offset,
	};
}

#endif /* IGT_GENXML_H */
