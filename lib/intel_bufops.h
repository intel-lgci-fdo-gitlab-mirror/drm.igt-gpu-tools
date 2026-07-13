#ifndef __INTEL_BUFOPS_H__
#define __INTEL_BUFOPS_H__

#include <stdint.h>
#include "igt_list.h"
#include "igt_aux.h"
#include "intel_batchbuffer.h"

struct buf_ops;

#define INTEL_BUF_INVALID_ADDRESS (-1ull)
#define INTEL_BUF_NAME_MAXSIZE 32
#define INVALID_ADDR(x) ((x) == INTEL_BUF_INVALID_ADDRESS)

struct intel_buf {
	struct buf_ops *bops;

	bool is_owner;
	uint32_t handle;
	uint64_t size;
	uint32_t width;
	uint32_t height;
	uint32_t tiling;
	uint32_t bpp, depth;
	uint32_t compression;
	uint32_t swizzle_mode;
	uint32_t yuv_semiplanar_bpp;
	bool format_is_yuv;
	bool format_is_yuv_semiplanar;
	struct {
		uint32_t offset;
		uint32_t stride;
		uint64_t size;
	} surface[2];
	struct {
		uint32_t offset;
		uint32_t stride;
	} ccs[2];
	struct {
		uint32_t offset;
		bool disable;
	} cc;
	struct {
		uint64_t offset;
		uint32_t ctx;
	} addr;

	uint64_t bo_size;
	uint64_t region;

	/* Tracking */
	struct intel_bb *ibb;
	struct igt_list_head link;

	/* CPU mapping */
	uint32_t *ptr;
	bool cpu_write;

	/* Content Protection*/
	bool is_protected;

	/* pat_index to use for mapping this buf. Only used in Xe. */
	uint8_t pat_index;

	/* mocs_index to use for operations using this intel_buf, like render_copy  */
	uint8_t mocs_index;

	/* For debugging purposes */
	char name[INTEL_BUF_NAME_MAXSIZE + 1];
};

static inline bool intel_buf_compressed(const struct intel_buf *buf)
{
	return buf->compression != I915_COMPRESSION_NONE;
}

static inline unsigned int intel_buf_width(const struct intel_buf *buf)
{
	return buf->width;
}

static inline unsigned int intel_buf_height(const struct intel_buf *buf)
{
	return buf->height;
}

static inline unsigned int
intel_buf_ccs_width(int gen, const struct intel_buf *buf)
{
	/*
	 * GEN12+: The AUX CCS unit size is 64 bytes mapping 4 main surface
	 * tiles. Thus the width of the CCS unit is 4*32=128 pixels on the
	 * main surface.
	 */
	if (gen >= 12)
		return DIV_ROUND_UP(intel_buf_width(buf), 512 / (buf->bpp / 8)) * 64;

	return DIV_ROUND_UP(intel_buf_width(buf), 1024) * 128;
}

static inline unsigned int
intel_buf_ccs_height(int gen, const struct intel_buf *buf)
{
	/*
	 * GEN12+: The AUX CCS unit size is 64 bytes mapping 4 main surface
	 * tiles. Thus the height of the CCS unit is 32 pixel rows on the main
	 * surface.
	 */
	if (gen >= 12)
		return DIV_ROUND_UP(intel_buf_height(buf), 32);

	return DIV_ROUND_UP(intel_buf_height(buf), 512) * 32;
}

uint64_t intel_buf_size(const struct intel_buf *buf);
uint64_t intel_buf_bo_size(const struct intel_buf *buf);

struct buf_ops *buf_ops_create(int fd);
struct buf_ops *buf_ops_create_with_selftest(int fd);
void buf_ops_destroy(struct buf_ops *bops);
int buf_ops_get_fd(struct buf_ops *bops);
uint32_t buf_ops_get_devid(struct buf_ops *bops);
enum intel_driver buf_ops_get_driver(struct buf_ops *bops);

bool buf_ops_set_software_tiling(struct buf_ops *bops,
				 uint32_t tiling,
				 bool use_software_tiling);

void intel_buf_to_linear(struct buf_ops *bops, struct intel_buf *buf,
			 uint32_t *linear);

void linear_to_intel_buf(struct buf_ops *bops, struct intel_buf *buf,
			 uint32_t *linear);

bool buf_ops_has_hw_fence(struct buf_ops *bops, uint32_t tiling);
bool buf_ops_has_tiling_support(struct buf_ops *bops, uint32_t tiling);

static inline void intel_buf_set_ownership(struct intel_buf *buf, bool is_owner)
{
	buf->is_owner = is_owner;
}

void intel_buf_init(struct buf_ops *bops, struct intel_buf *buf,
		    int width, int height, int bpp, int alignment,
		    uint32_t tiling, uint32_t compression);
void intel_buf_init_in_region(struct buf_ops *bops,
			      struct intel_buf *buf,
			      int width, int height, int bpp, int alignment,
			      uint32_t tiling, uint32_t compression,
			      uint64_t region);
void intel_buf_close(struct buf_ops *bops, struct intel_buf *buf);

void intel_buf_init_full(struct buf_ops *bops,
			 uint32_t handle,
			 struct intel_buf *buf,
			 int width, int height,
			 int bpp, int alignment,
			 uint32_t req_tiling,
			 uint32_t compression,
			 uint64_t size,
			 int stride,
			 uint64_t region,
			 uint8_t pat_index,
			 uint8_t mocs_index);

struct intel_buf *intel_buf_create(struct buf_ops *bops,
				   int width, int height,
				   int bpp, int alignment,
				   uint32_t req_tiling, uint32_t compression);

void intel_buf_init_using_handle_and_size(struct buf_ops *bops,
					  uint32_t handle,
					  struct intel_buf *buf,
					  int width, int height,
					  int bpp, int alignment,
					  uint32_t req_tiling,
					  uint32_t compression,
					  uint64_t size);

struct intel_buf *intel_buf_create_using_handle_and_size(struct buf_ops *bops,
							 uint32_t handle,
							 int width, int height,
							 int bpp, int alignment,
							 uint32_t req_tiling,
							 uint32_t compression,
							 uint64_t size);

struct intel_buf *intel_buf_create_full(struct buf_ops *bops,
					uint32_t handle,
					int width, int height,
					int bpp, int alignment,
					uint32_t req_tiling,
					uint32_t compression,
					uint64_t size,
					int stride,
					uint64_t region,
					uint8_t pat_index,
					uint8_t mocs_index);
void intel_buf_destroy(struct intel_buf *buf);

static inline void intel_buf_set_pxp(struct intel_buf *buf, bool new_pxp_state)
{
	igt_assert(buf);
	buf->is_protected = new_pxp_state;
}

static inline bool intel_buf_pxp(const struct intel_buf *buf)
{
	igt_assert(buf);
	return buf->is_protected;
}

/**
 * intel_buf_mocs:
 * @buf: the intel_buf
 *
 * Returns the full 7-bit MOCS field value for @buf, encoding the 6-bit
 * table index at bits 6:1 and the PXP protected-content bit at bit 0.
 * Use this when assigning to a genxml-generated MOCS field.
 */
static inline uint8_t intel_buf_mocs(const struct intel_buf *buf)
{
	return (buf->mocs_index << 1) | (intel_buf_pxp(buf) ? 1 : 0);
}

void *intel_buf_cpu_map(struct intel_buf *buf, bool write);
void *intel_buf_device_map(struct intel_buf *buf, bool write);
void intel_buf_unmap(struct intel_buf *buf);
void intel_buf_flush_and_unmap(struct intel_buf *buf);

void intel_buf_print(const struct intel_buf *buf);
void intel_buf_dump(const struct intel_buf *buf, const char *filename);
const char *intel_buf_set_name(struct intel_buf *buf, const char *name);

void intel_buf_write_to_png(struct intel_buf *buf, const char *filename);
void intel_buf_write_aux_to_png(struct intel_buf *buf, const char *filename);
void intel_buf_raw_write_to_png(struct intel_buf *buf, const char *namefmt, ...);
void intel_buf_draw_pattern(struct buf_ops *bops, struct intel_buf *buf,
			    int x, int y, int w, int h,
			    int cx, int cy, int cw, int ch,
			    bool use_alternate_colors);

#endif
