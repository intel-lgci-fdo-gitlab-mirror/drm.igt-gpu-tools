/*
 * Copyright © 2020 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */

#include <sys/ioctl.h>
#include <cairo.h>

#include "i915/gem_create.h"
#include "igt.h"
#include "igt_x86.h"
#include "intel_bufops.h"
#include "intel_mocs.h"
#include "intel_pat.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"

/**
 * SECTION:intel_bufops
 * @short_description: Buffer operation on tiled surfaces
 * @title: Buffer operations
 * @include: igt.h
 *
 * # Buffer operations
 *
 * Intel GPU devices supports different set of tiled surfaces.
 * Checking each time what tile formats are supports is cumbersome and
 * error prone.
 *
 * Buffer operation (buf_ops) provide a wrapper to conditional code which
 * can be used without worrying of implementation details giving:
 * - copy linear to tiled buffer
 * - copy tiled buffer to linear
 *
 * Following code order should be used (linear is plain memory with some
 * image data):
 *
 * |[<!-- language="c" -->
 * struct buf_ops *bops;
 * struct intel_buf ibuf;
 * ...
 * bops = buf_ops_create(fd);
 * intel_buf_init(bops, &ibuf, 512, 512, 32, 64, I915_TILING_X, false);
 * ...
 * linear_to_intel_buf(bops, &ibuf, linear);
 * ...
 * intel_buf_to_linear(bops, &ibuf, linear);
 * ...
 * intel_buf_close(bops, &ibuf);
 * ...
 * buf_ops_destroy(bops);
 * ]|
 *
 * Calling buf_ops_create(fd) probes hardware capabilities (supported fences,
 * swizzling) and returns opaque pointer to buf_ops. From now on
 * intel_buf_to_linear() and linear_to_intel_buf() will choose appropriate
 * function to do the job.
 *
 * Note: bufops doesn't support SW tiling code yet.
 */

//#define BUFOPS_DEBUGGING
#ifdef BUFOPS_DEBUGGING
#define DEBUG(...) printf(__VA_ARGS__)
#define DEBUGFN() DEBUG("\t -> %s()\n", __FUNCTION__)
#else
#define DEBUG(...)
#define DEBUGFN()
#endif

#undef TILE_NONE
#undef TILE_X
#undef TILE_Y
#undef TILE_Yf
#undef TILE_Ys
#undef TILE_4
#undef TILE_64

#define TILE_DEF(x) (1 << (x))
#define TILE_NONE   TILE_DEF(I915_TILING_NONE)
#define TILE_X      TILE_DEF(I915_TILING_X)
#define TILE_Y      TILE_DEF(I915_TILING_Y)
#define TILE_Yf     TILE_DEF(I915_TILING_Yf)
#define TILE_Ys     TILE_DEF(I915_TILING_Ys)
#define TILE_4      TILE_DEF(I915_TILING_4)
#define TILE_64     TILE_DEF(I915_TILING_64)

#define CCS_OFFSET(buf) (buf->ccs[0].offset)
#define CCS_SIZE(gen, buf) \
	(intel_buf_ccs_width(gen, buf) * intel_buf_ccs_height(gen, buf))

typedef void (*bo_copy)(struct buf_ops *, struct intel_buf *, uint32_t *);

struct buf_ops {
	int fd;
	enum intel_driver driver;
	int gen_start;
	int gen_end;
	unsigned int intel_gen;
	uint32_t devid;
	uint32_t supported_tiles;
	uint32_t supported_hw_tiles;
	uint32_t swizzle_x;
	uint32_t swizzle_y;
	bo_copy linear_to;
	bo_copy linear_to_x;
	bo_copy linear_to_y;
	bo_copy linear_to_yf;
	bo_copy linear_to_ys;
	bo_copy linear_to_tile4;
	bo_copy linear_to_tile64;
	bo_copy to_linear;
	bo_copy x_to_linear;
	bo_copy y_to_linear;
	bo_copy yf_to_linear;
	bo_copy ys_to_linear;
	bo_copy tile4_to_linear;
	bo_copy tile64_to_linear;
};

static const char *tiling_str(uint32_t tiling)
{
	switch (tiling) {
	case I915_TILING_NONE: return "NONE";
	case I915_TILING_X:    return "X";
	case I915_TILING_Y:    return "Y";
	case I915_TILING_Yf:   return "Yf";
	case I915_TILING_Ys:   return "Ys";
	case I915_TILING_4:    return "4";
	case I915_TILING_64:   return "64";
	default:               return "UNKNOWN";
	}
}

static const char *bool_str(bool v)
{
	return v ? "yes" : "no";
}

static inline bool is_hw_tiling_supported(struct buf_ops *bops, uint32_t tiling)
{
	return bops->supported_hw_tiles & TILE_DEF(tiling);
}

static inline bool is_tiling_supported(struct buf_ops *bops, uint32_t tiling)
{
	return bops->supported_tiles & TILE_DEF(tiling);
}

static uint32_t get_stride(uint32_t devid, uint32_t tiling)
{
	uint32_t stride = 128;

	if (IS_915G(devid) || IS_915GM(devid) || tiling == I915_TILING_X)
		stride = 512;

	return stride;
}

static int __gem_get_tiling(int fd, struct drm_i915_gem_get_tiling *arg)
{
	int err;

	err = 0;
	if (igt_ioctl(fd, DRM_IOCTL_I915_GEM_GET_TILING, arg)) {
		err = -errno;
		igt_assume(err);
	}
	errno = 0;

	return err;
}

static bool __get_tiling(int fd, uint32_t handle, uint32_t *tiling,
			 uint32_t *swizzle, uint32_t *phys_swizzle)
{
	struct drm_i915_gem_get_tiling get_tiling = { .handle = handle };

	if (__gem_get_tiling(fd, &get_tiling) != 0)
		return false;

	*tiling = get_tiling.tiling_mode;
	*swizzle = get_tiling.swizzle_mode;
	*phys_swizzle = get_tiling.phys_swizzle_mode;
	igt_debug("buf tiling: %s, swizzle: %x, phys_swizzle: %x\n",
		  tiling_str(get_tiling.tiling_mode),
		  get_tiling.swizzle_mode,
		  get_tiling.phys_swizzle_mode);

	return true;
}

static int __set_tiling(int fd, uint32_t handle, uint32_t tiling,
			uint32_t stride,
			uint32_t *ret_tiling, uint32_t *ret_swizzle)
{
	struct drm_i915_gem_set_tiling st;

	memset(&st, 0, sizeof(st));
	do {

		int err;

		st.handle = handle;
		st.tiling_mode = tiling;
		st.stride = tiling ? stride : 0;

		err = 0;
		if (ioctl(fd, DRM_IOCTL_I915_GEM_SET_TILING, &st))
			err = -errno;
		errno = 0;
		if (err != -EINTR) {
			if (ret_tiling)
				*ret_tiling = st.tiling_mode;

			if (ret_swizzle)
				*ret_swizzle = st.swizzle_mode;

			return err;
		}
	} while (1);
}

static void set_hw_tiled(struct buf_ops *bops, struct intel_buf *buf)
{
	uint32_t ret_tiling, ret_swizzle;

	if (buf->tiling != I915_TILING_X && buf->tiling != I915_TILING_Y &&
	    buf->tiling != I915_TILING_4)
		return;

	if (!buf_ops_has_hw_fence(bops, buf->tiling)) {
		igt_debug("No HW fence for tiling: %d\n", buf->tiling);
		return;
	}

	igt_assert_eq(__set_tiling(bops->fd,
				   buf->handle, buf->tiling,
				   buf->surface[0].stride,
				   &ret_tiling, &ret_swizzle),
		      0);

	igt_assert(ret_tiling == buf->tiling);
	buf->swizzle_mode = ret_swizzle;
}

static unsigned long swizzle_bit(unsigned int bit, unsigned long offset)
{
	return (offset & (1ul << bit)) >> (bit - 6);
}

static unsigned long swizzle_addr(void *ptr, uint32_t swizzle)
{
	unsigned long addr = to_user_pointer(ptr);

	switch (swizzle) {
	case I915_BIT_6_SWIZZLE_NONE:
		return addr;
	case I915_BIT_6_SWIZZLE_9:
		return addr ^ swizzle_bit(9, addr);
	case I915_BIT_6_SWIZZLE_9_10:
		return addr ^ swizzle_bit(9, addr) ^ swizzle_bit(10, addr);
	case I915_BIT_6_SWIZZLE_9_11:
		return addr ^ swizzle_bit(9, addr) ^ swizzle_bit(11, addr);
	case I915_BIT_6_SWIZZLE_9_10_11:
		return (addr ^
			swizzle_bit(9, addr) ^
			swizzle_bit(10, addr) ^
			swizzle_bit(11, addr));

	case I915_BIT_6_SWIZZLE_UNKNOWN:
	case I915_BIT_6_SWIZZLE_9_17:
	case I915_BIT_6_SWIZZLE_9_10_17:
	default:
		igt_skip("physical swizzling mode impossible to handle in userspace\n");
		return addr;
	}
}

static void *linear_ptr(void *ptr,
			unsigned int x, unsigned int y,
			unsigned int stride, unsigned int cpp)
{
	int pos;

	pos = (stride/cpp * y + x) * cpp;

	return ptr + pos;
}

static void *x_ptr(void *ptr,
		   unsigned int x, unsigned int y,
		   unsigned int stride, unsigned int cpp,
		   const int tile_width, const int tile_height)
{
	const int tile_size = tile_width * tile_height;
	int offset_x, offset_y, pos;
	int tile_x, tile_y;

	x *= cpp;
	tile_x = x / tile_width;
	tile_y = y / tile_height;
	offset_x = tile_x * tile_size;
	offset_y = tile_y * stride * tile_height;

	pos = (offset_y + (y % tile_height * tile_width) +
	       offset_x + (x % tile_width));

	return ptr + pos;
}

static void *gen2_x_ptr(void *ptr,
			unsigned int x, unsigned int y,
			unsigned int stride, unsigned int cpp)
{
	return x_ptr(ptr, x, y, stride, cpp, 128, 16);
}

static void *gen3_x_ptr(void *ptr,
			unsigned int x, unsigned int y,
			unsigned int stride, unsigned int cpp)
{
	return x_ptr(ptr, x, y, stride, cpp, 512, 8);
}

static void *y_ptr(void *ptr,
		   unsigned int x, unsigned int y,
		   unsigned int stride, unsigned int cpp,
		   const int tile_width,
		   const int tile_height,
		   const int owords)
{
	const int tile_size = tile_width * tile_height;
	int offset_x, offset_y, pos;
	int shift_x, shift_y;
	int tile_x, tile_y;

	x *= cpp;
	tile_x = x / tile_width;
	tile_y = y / tile_height;
	offset_x = tile_x * tile_size;
	offset_y = tile_y * stride * tile_height;
	shift_x = x % owords + (x % tile_width) / owords * tile_width * cpp;
	shift_y = y % tile_height * owords;

	pos = offset_y + offset_x + shift_x + shift_y;

	return ptr + pos;
}

static void *gen2_y_ptr(void *ptr,
			unsigned int x, unsigned int y,
			unsigned int stride, unsigned int cpp)
{
	return y_ptr(ptr, x, y, stride, cpp, 128, 16, 8);
}

static void *i915_y_ptr(void *ptr,
			unsigned int x, unsigned int y,
			unsigned int stride, unsigned int cpp)
{
	return y_ptr(ptr, x, y, stride, cpp, 512, 8, 32);
}

static void *i945_y_ptr(void *ptr,
			unsigned int x, unsigned int y,
			unsigned int stride, unsigned int cpp)
{
	return y_ptr(ptr, x, y, stride, cpp, 128, 32, 16);
}

/*
 * (x,y) to memory location in tiled-4 surface
 *
 * coverted those divisions and multiplications to shifts and masks
 * in hope this wouldn't be so slow.
 */
static void *tile4_ptr(void *ptr,
			unsigned int x, unsigned int y,
			unsigned int stride, unsigned int cpp)
{
	const int tile_width = 128;
	const int tile_height = 32;
	const int subtile_size = 64;
	const int owords = 16;
	int base, _x, _y, subtile, tile_x, tile_y;
	int x_loc = x << __builtin_ctz(cpp);
	int pos;

	/* Pixel in tile via masks */
	tile_x = x_loc & (tile_width - 1);
	tile_y = y & (tile_height - 1);

	/* subtile in 4k tile */
	_x = tile_x >> __builtin_ctz(owords);
	_y = tile_y >> 2;

	/* tile-4 swizzle */
	subtile = ((_y >> 1) << 4) + ((_y & 1) << 2) + (_x & 3) + ((_x & 4) << 1);

	/* memory location */
	base = (y >> __builtin_ctz(tile_height)) *
		(stride << __builtin_ctz(tile_height)) +
		(((x_loc >> __builtin_ctz(tile_width)) << __builtin_ctz(4096)));

	pos = base + (subtile << __builtin_ctz(subtile_size)) +
		((tile_y & 3) << __builtin_ctz(owords)) +
		(tile_x & (owords - 1));
	igt_assert((pos & (cpp - 1)) == 0);

	return ptr + pos;
}

/*
 * (x,y) to memory location in tile-64 surface.
 */
static void *tile64_ptr(void *ptr,
			unsigned int x, unsigned int y,
			unsigned int stride, unsigned int cpp)
{
	const int tile_size = 64 * 1024;
	int tile_width, tile_height;
	int tile_x, tile_y, _x, _y;
	int base, vaddr, pos;

	switch (cpp) {
	case 1:
		tile_width = 256;
		tile_height = 256;
		break;
	case 2:
		tile_width = 256;
		tile_height = 128;
		break;
	case 4:
		tile_width = 128;
		tile_height = 128;
		break;
	case 8:
	default:
		tile_width = 128;
		tile_height = 64;
		break;
	}

	tile_x = x / tile_width;
	tile_y = y / tile_height;
	_x = x % tile_width;
	_y = y % tile_height;

	switch (cpp) {
	case 1:
		vaddr = (_x & 0xf) | ((_y & 0x3) << 4) |
			((_x & 0x30) << 2) | ((_y & 0x4) << 6) |
			((_x & 0x40) << 3) | ((_y & 0x18) << 7) |
			((_x & 0x80) << 5) | ((_y & 0xe0) << 8);
		break;
	case 2:
		vaddr = ((_x & 0x7) << 1) | ((_y & 0x3) << 4) |
			((_x & 0x18) << 3) | ((_y & 0x4) << 6) |
			((_x & 0x20) << 4) | ((_y & 0x18) << 7) |
			((_x & 0xc0) << 6) | ((_y & 0x60) << 9);
		break;
	case 4:
		vaddr = ((_x & 0x3) << 2) | ((_y & 0x3) << 4) |
			((_x & 0xc) << 4) | ((_y & 0x4) << 6) |
			((_x & 0x10) << 5) | ((_y & 0x18) << 7) |
			((_x & 0x60) << 7) | ((_y & 0x60) << 9);
		break;
	case 8:
	default:
		vaddr = ((_x & 0x1) << 3) | ((_y & 0x3) << 4) |
			((_x & 0x6) << 5) | ((_y & 0x4) << 6) |
			((_x & 0x8) << 6) | ((_y & 0x18) << 7) |
			((_x & 0x70) << 8) | ((_y & 0x20) << 10);
		break;
	}

	/* memory location */
	base = tile_y * (stride * tile_height) + tile_x * tile_size;
	pos = base + vaddr;
	igt_assert((pos & (cpp - 1)) == 0);

	return ptr + pos;
}

static void *yf_ptr(void *ptr,
		    unsigned int x, unsigned int y,
		    unsigned int stride, unsigned int cpp)
{
	const int tile_size = 4 * 1024;
	const int tile_width = 128;
	int row_size = stride / tile_width * tile_size;

	x *= cpp; /* convert to Byte offset */

	/*
	 * Within a 4k Yf tile, the byte swizzling pattern is
	 * msb......lsb
	 * xyxyxyyyxxxx
	 * The tiles themselves are laid out in row major order.
	 */
	return ptr +
		((x & 0xf) * 1) + /* 4x1 pixels(32bpp) = 16B */
		((y & 0x3) * 16) + /* 4x4 pixels = 64B */
		(((y & 0x4) >> 2) * 64) + /* 1x2 64B blocks */
		(((x & 0x10) >> 4) * 128) + /* 2x2 64B blocks = 256B block */
		(((y & 0x8) >> 3) * 256) + /* 2x1 256B blocks */
		(((x & 0x20) >> 5) * 512) + /* 2x2 256B blocks */
		(((y & 0x10) >> 4) * 1024) + /* 4x2 256 blocks */
		(((x & 0x40) >> 6) * 2048) + /* 4x4 256B blocks = 4k tile */
		(((x & ~0x7f) >> 7) * tile_size) + /* row of tiles */
		(((y & ~0x1f) >> 5) * row_size);
}

typedef void *(*tile_fn)(void *, unsigned int, unsigned int,
			unsigned int, unsigned int);
static tile_fn __get_tile_fn_ptr(int fd, int tiling)
{
	const struct intel_device_info *info =
		intel_get_device_info(intel_get_drm_devid(fd));
	tile_fn fn = NULL;

	switch (tiling) {
	case I915_TILING_NONE:
		fn = linear_ptr;
		break;
	case I915_TILING_X:
		if (info->graphics_ver == 2)
			fn = gen2_x_ptr;
		else
			fn = gen3_x_ptr;
		break;
	case I915_TILING_Y:
		if (info->graphics_ver == 2)
			fn = gen2_y_ptr;
		else if (info->is_grantsdale || info->is_alviso)
			fn = i915_y_ptr;
		else
			fn = i945_y_ptr;
		break;
	case I915_TILING_Yf:
		fn = yf_ptr;
		break;
	case I915_TILING_4:
		fn = tile4_ptr;
		break;
	case I915_TILING_64:
		fn = tile64_ptr;
		break;
	case I915_TILING_Ys:
		/* To be implemented */
		break;
	}

	igt_require_f(fn, "Can't find tile function for tiling: %d\n", tiling);
	return fn;
}

static bool is_cache_coherent(int fd, uint32_t handle)
{
	return gem_get_caching(fd, handle) != I915_CACHING_NONE;
}

enum ccs_copy_direction {
	CCS_LINEAR_TO_BUF,
	CCS_BUF_TO_LINEAR,
};

static void __copy_ccs(struct buf_ops *bops, struct intel_buf *buf,
		       uint32_t *linear, enum ccs_copy_direction dir)
{
	uint64_t size, offset, ccs_size;
	unsigned int gen;
	void *map;

	if (!buf->compression || HAS_FLATCCS(intel_get_drm_devid(bops->fd)))
		return;

	gen = bops->intel_gen;
	offset = CCS_OFFSET(buf);
	ccs_size = CCS_SIZE(gen, buf);
	size = offset + ccs_size;

	if (gem_has_lmem(bops->fd)) {
		map = gem_mmap__device_coherent(bops->fd, buf->handle, 0, size,
						PROT_READ | PROT_WRITE);
	} else {
		map = __gem_mmap_offset__wc(bops->fd, buf->handle, 0, size,
					    PROT_READ | PROT_WRITE);
		if (!map)
			map = gem_mmap__wc(bops->fd, buf->handle, 0, size,
					   PROT_READ | PROT_WRITE);
	}

	switch (dir) {
	case CCS_LINEAR_TO_BUF:
		gem_set_domain(bops->fd, buf->handle,
			       I915_GEM_DOMAIN_WC, I915_GEM_DOMAIN_WC);
		igt_memcpy_from_wc(map + offset, (uint8_t *) linear + offset,
				   ccs_size);
	case CCS_BUF_TO_LINEAR:
		gem_set_domain(bops->fd, buf->handle, I915_GEM_DOMAIN_WC, 0);
		igt_memcpy_from_wc((uint8_t *) linear + offset, map + offset,
				   ccs_size);
	}

	munmap(map, size);
}

static void *mmap_write(int fd, const struct intel_buf *buf, bool *malloced)
{
	void *map = NULL;

	*malloced = false;

	if (buf->bops->driver == INTEL_DRIVER_XE)
		return xe_bo_map(fd, buf->handle, buf->surface[0].size);

	if (gem_has_lmem(fd)) {
		/*
		 * set/get_caching and set_domain are no longer supported on
		 * discrete, also the only mmap mode supportd is FIXED.
		 */
		map = gem_mmap_offset__fixed(fd, buf->handle, 0,
					     buf->surface[0].size,
					     PROT_READ | PROT_WRITE);
		igt_assert_eq(gem_wait(fd, buf->handle, 0), 0);
	}

	if (!map && is_cache_coherent(fd, buf->handle)) {
		map = __gem_mmap_offset__cpu(fd, buf->handle, 0, buf->surface[0].size,
					     PROT_READ | PROT_WRITE);
		if (!map)
			map = __gem_mmap__cpu(fd, buf->handle, 0, buf->surface[0].size,
					      PROT_READ | PROT_WRITE);

		if (map)
			gem_set_domain(fd, buf->handle,
				       I915_GEM_DOMAIN_CPU,
				       I915_GEM_DOMAIN_CPU);
	}

	if (!map && gem_mmap__has_wc(fd)) {
		map = __gem_mmap_offset__wc(fd, buf->handle, 0, buf->surface[0].size,
					    PROT_READ | PROT_WRITE);
		if (!map)
			map = gem_mmap__wc(fd, buf->handle, 0, buf->surface[0].size,
					   PROT_READ | PROT_WRITE);

		gem_set_domain(fd, buf->handle,
			       I915_GEM_DOMAIN_WC, I915_GEM_DOMAIN_WC);
	}

	if (!map) {
		map = malloc(buf->surface[0].size);
		igt_assert(map);
		*malloced = true;
	}

	return map;
}

static void munmap_write(void *map, int fd, const struct intel_buf *buf, bool malloced)
{
	if (malloced) {
		igt_assert(__gem_write(fd, buf->handle, 0, map, buf->surface[0].size) == 0);
		free(map);
	} else {
		munmap(map, buf->surface[0].size);
	}
}

static void *mmap_read(int fd, struct intel_buf *buf, bool *malloced)
{
	void *map = NULL;

	*malloced = false;

	if (buf->bops->driver == INTEL_DRIVER_XE)
		return xe_bo_map(fd, buf->handle, buf->surface[0].size);

	if (gem_has_lmem(fd)) {
		/*
		 * set/get_caching and set_domain are no longer supported on
		 * discrete, also the only supported mmap mode is FIXED.
		 */
		map = gem_mmap_offset__fixed(fd, buf->handle, 0,
					     buf->surface[0].size, PROT_READ);
		igt_assert_eq(gem_wait(fd, buf->handle, 0), 0);
	}

	if (!map && (gem_has_llc(fd) || is_cache_coherent(fd, buf->handle))) {
		map = __gem_mmap_offset__cpu(fd, buf->handle, 0,
					     buf->surface[0].size, PROT_READ);
		if (!map)
			map = __gem_mmap__cpu(fd, buf->handle, 0, buf->surface[0].size,
					      PROT_READ);

		if (map)
			gem_set_domain(fd, buf->handle, I915_GEM_DOMAIN_CPU, 0);
	}

	if (!map && gem_mmap__has_wc(fd)) {
		map = __gem_mmap_offset__wc(fd, buf->handle, 0, buf->surface[0].size,
					    PROT_READ);
		if (!map)
			map = gem_mmap__wc(fd, buf->handle, 0, buf->surface[0].size,
					   PROT_READ);

		gem_set_domain(fd, buf->handle, I915_GEM_DOMAIN_WC, 0);
	}

	if (!map) {
		map = malloc(buf->surface[0].size);
		igt_assert(map);
		*malloced = true;

		igt_assert(__gem_read(fd, buf->handle, 0, map, buf->surface[0].size) == 0);
	}

	return map;
}

static void munmap_read(void *map, int fd, const struct intel_buf *buf, bool malloced)
{
	if (malloced)
		free(map);
	else
		munmap(map, buf->surface[0].size);
}

static void __copy_linear_to(int fd, struct intel_buf *buf,
			     const uint32_t *linear,
			     int tiling, uint32_t swizzle)
{
	const tile_fn fn = __get_tile_fn_ptr(fd, tiling);
	int height = intel_buf_height(buf);
	int width = intel_buf_width(buf);
	bool malloced;
	void *map;

	map = mmap_write(fd, buf, &malloced);

	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			uint32_t *ptr = fn(map, x, y, buf->surface[0].stride, buf->bpp/8);

			if (swizzle)
				ptr = from_user_pointer(swizzle_addr(ptr,
								     swizzle));
			*ptr = linear[y * width + x];
		}
	}

	munmap_write(map, fd, buf, malloced);
}

static void copy_linear_to_none(struct buf_ops *bops, struct intel_buf *buf,
				uint32_t *linear)
{
	DEBUGFN();
	__copy_linear_to(bops->fd, buf, linear, I915_TILING_NONE, 0);
}

static void copy_linear_to_x(struct buf_ops *bops, struct intel_buf *buf,
			     uint32_t *linear)
{
	DEBUGFN();
	__copy_linear_to(bops->fd, buf, linear, I915_TILING_X, bops->swizzle_x);
}

static void copy_linear_to_y(struct buf_ops *bops, struct intel_buf *buf,
			     uint32_t *linear)
{
	DEBUGFN();
	__copy_linear_to(bops->fd, buf, linear, I915_TILING_Y, bops->swizzle_y);
}

static void copy_linear_to_yf(struct buf_ops *bops, struct intel_buf *buf,
			      uint32_t *linear)
{
	DEBUGFN();
	__copy_linear_to(bops->fd, buf, linear, I915_TILING_Yf, 0);
}

static void copy_linear_to_ys(struct buf_ops *bops, struct intel_buf *buf,
			      uint32_t *linear)
{
	DEBUGFN();
	__copy_linear_to(bops->fd, buf, linear, I915_TILING_Ys, 0);
}

static void copy_linear_to_tile4(struct buf_ops *bops, struct intel_buf *buf,
				 uint32_t *linear)
{
	DEBUGFN();
	__copy_linear_to(bops->fd, buf, linear, I915_TILING_4, 0);
}

static void copy_linear_to_tile64(struct buf_ops *bops, struct intel_buf *buf,
				  uint32_t *linear)
{
	DEBUGFN();
	__copy_linear_to(bops->fd, buf, linear, I915_TILING_64, 0);
}

static void __copy_to_linear(int fd, struct intel_buf *buf,
			     uint32_t *linear, int tiling, uint32_t swizzle)
{
	const tile_fn fn = __get_tile_fn_ptr(fd, tiling);
	int height = intel_buf_height(buf);
	int width = intel_buf_width(buf);
	bool malloced;
	void *map;

	map = mmap_write(fd, buf, &malloced);

	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			uint32_t *ptr = fn(map, x, y, buf->surface[0].stride, buf->bpp/8);

			if (swizzle)
				ptr = from_user_pointer(swizzle_addr(ptr,
								     swizzle));
			linear[y * width + x] = *ptr;
		}
	}

	munmap_write(map, fd, buf, malloced);
}

static void copy_none_to_linear(struct buf_ops *bops, struct intel_buf *buf,
				uint32_t *linear)
{
	DEBUGFN();
	__copy_to_linear(bops->fd, buf, linear, I915_TILING_NONE, 0);
}

static void copy_x_to_linear(struct buf_ops *bops, struct intel_buf *buf,
			     uint32_t *linear)
{
	DEBUGFN();
	__copy_to_linear(bops->fd, buf, linear, I915_TILING_X, bops->swizzle_x);
}

static void copy_y_to_linear(struct buf_ops *bops, struct intel_buf *buf,
			     uint32_t *linear)
{
	DEBUGFN();
	__copy_to_linear(bops->fd, buf, linear, I915_TILING_Y, bops->swizzle_y);
}

static void copy_yf_to_linear(struct buf_ops *bops, struct intel_buf *buf,
			      uint32_t *linear)
{
	DEBUGFN();
	__copy_to_linear(bops->fd, buf, linear, I915_TILING_Yf, 0);
}

static void copy_ys_to_linear(struct buf_ops *bops, struct intel_buf *buf,
			      uint32_t *linear)
{
	DEBUGFN();
	__copy_to_linear(bops->fd, buf, linear, I915_TILING_Ys, 0);
}

static void copy_tile4_to_linear(struct buf_ops *bops, struct intel_buf *buf,
				 uint32_t *linear)
{
	DEBUGFN();
	__copy_to_linear(bops->fd, buf, linear, I915_TILING_4, 0);
}

static void copy_tile64_to_linear(struct buf_ops *bops, struct intel_buf *buf,
				  uint32_t *linear)
{
	DEBUGFN();
	__copy_to_linear(bops->fd, buf, linear, I915_TILING_64, 0);
}

static void copy_linear_to_gtt(struct buf_ops *bops, struct intel_buf *buf,
			       uint32_t *linear)
{
	void *map;

	DEBUGFN();

	map = gem_mmap__gtt(bops->fd, buf->handle,
			    buf->surface[0].size, PROT_READ | PROT_WRITE);

	gem_set_domain(bops->fd, buf->handle,
		       I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);

	memcpy(map, linear, buf->surface[0].size);

	munmap(map, buf->surface[0].size);
}

static void copy_gtt_to_linear(struct buf_ops *bops, struct intel_buf *buf,
			       uint32_t *linear)
{
	void *map;

	DEBUGFN();

	map = gem_mmap__gtt(bops->fd, buf->handle,
			    buf->surface[0].size, PROT_READ);

	gem_set_domain(bops->fd, buf->handle,
		       I915_GEM_DOMAIN_GTT, 0);

	igt_memcpy_from_wc(linear, map, buf->surface[0].size);

	munmap(map, buf->surface[0].size);
}

static void copy_linear_to_wc(struct buf_ops *bops, struct intel_buf *buf,
			      uint32_t *linear)
{
	bool malloced;
	void *map;

	DEBUGFN();

	map = mmap_write(bops->fd, buf, &malloced);
	memcpy(map, linear, buf->surface[0].size);
	munmap_write(map, bops->fd, buf, malloced);
}

static void copy_wc_to_linear(struct buf_ops *bops, struct intel_buf *buf,
			      uint32_t *linear)
{
	bool malloced;
	void *map;

	DEBUGFN();

	map = mmap_read(bops->fd, buf, &malloced);
	igt_memcpy_from_wc(linear, map, buf->surface[0].size);
	munmap_read(map, bops->fd, buf, malloced);
}

void intel_buf_to_linear(struct buf_ops *bops, struct intel_buf *buf,
			 uint32_t *linear)
{
	igt_assert(bops);

	switch (buf->tiling) {
	case I915_TILING_NONE:
		igt_assert(bops->to_linear);
		bops->to_linear(bops, buf, linear);
		break;
	case I915_TILING_X:
		igt_assert(bops->x_to_linear);
		bops->x_to_linear(bops, buf, linear);
		break;
	case I915_TILING_Y:
		igt_assert(bops->y_to_linear);
		bops->y_to_linear(bops, buf, linear);
		break;
	case I915_TILING_Yf:
		igt_assert(bops->yf_to_linear);
		bops->yf_to_linear(bops, buf, linear);
		break;
	case I915_TILING_Ys:
		igt_assert(bops->ys_to_linear);
		bops->ys_to_linear(bops, buf, linear);
		break;
	case I915_TILING_4:
		igt_assert(bops->tile4_to_linear);
		bops->tile4_to_linear(bops, buf, linear);
		break;
	case I915_TILING_64:
		igt_assert(bops->tile64_to_linear);
		bops->tile64_to_linear(bops, buf, linear);
		break;
	}

	if (buf->compression)
		__copy_ccs(bops, buf, linear, CCS_BUF_TO_LINEAR);
}

void linear_to_intel_buf(struct buf_ops *bops, struct intel_buf *buf,
			 uint32_t *linear)
{
	igt_assert(bops);

	switch (buf->tiling) {
	case I915_TILING_NONE:
		igt_assert(bops->linear_to);
		bops->linear_to(bops, buf, linear);
		break;
	case I915_TILING_X:
		igt_assert(bops->linear_to_x);
		bops->linear_to_x(bops, buf, linear);
		break;
	case I915_TILING_Y:
		igt_assert(bops->linear_to_y);
		bops->linear_to_y(bops, buf, linear);
		break;
	case I915_TILING_Yf:
		igt_assert(bops->linear_to_yf);
		bops->linear_to_yf(bops, buf, linear);
		break;
	case I915_TILING_Ys:
		igt_assert(bops->linear_to_ys);
		bops->linear_to_ys(bops, buf, linear);
		break;
	case I915_TILING_4:
		igt_assert(bops->linear_to_tile4);
		bops->linear_to_tile4(bops, buf, linear);
		break;
	case I915_TILING_64:
		igt_assert(bops->linear_to_tile64);
		bops->linear_to_tile64(bops, buf, linear);
		break;
	}

	if (buf->compression)
		__copy_ccs(bops, buf, linear, CCS_LINEAR_TO_BUF);
}

static uint32_t __get_min_stride(uint32_t width, uint32_t bpp, int tiling)
{
	switch (tiling) {
	case I915_TILING_NONE:
		return width * bpp / 8;
	case I915_TILING_X:
		return ALIGN(width * bpp / 8, 512);
	case I915_TILING_64:
		if (bpp == 8)
			return ALIGN(width, 256);
		else if (bpp == 16 || bpp == 32)
			return ALIGN(width * bpp / 8, 512);
		return ALIGN(width * bpp / 8, 1024);

	default:
		return ALIGN(width * bpp / 8, 128);
	}
}

static uint32_t __get_aligned_height(uint32_t height, uint32_t bpp, int tiling)
{
	switch (tiling) {
	case I915_TILING_NONE:
		return height;
	case I915_TILING_X:
		return ALIGN(height, 8);
	case I915_TILING_64:
		if (bpp == 8)
			return ALIGN(height, 256);
		else if (bpp == 16 || bpp == 32)
			return ALIGN(height, 128);
		return ALIGN(height, 64);
	default:
		return ALIGN(height, 32);
	}
}

static void __intel_buf_init(struct buf_ops *bops,
			     uint32_t handle,
			     struct intel_buf *buf,
			     int width, int height, int bpp, int alignment,
			     uint32_t req_tiling, uint32_t compression,
			     uint64_t bo_size, int bo_stride,
			     uint64_t region, uint8_t pat_index,
			     uint8_t mocs_index)
{
	uint32_t tiling = req_tiling;
	uint64_t size;
	int tile_width, aligned_height;

	igt_assert(bops);
	igt_assert(buf);
	igt_assert(width > 0 && height > 0);
	igt_assert(bpp == 8 || bpp == 16 || bpp == 32 || bpp == 64);
	igt_assert(alignment >= 0);

	memset(buf, 0, sizeof(*buf));

	buf->bops = bops;
	buf->width = width;
	buf->height = height;
	buf->tiling = tiling;
	buf->bpp = bpp;
	buf->compression = compression;
	buf->addr.offset = INTEL_BUF_INVALID_ADDRESS;
	buf->pat_index = pat_index;
	if (mocs_index == DEFAULT_MOCS_INDEX)
		mocs_index = intel_get_uc_mocs_index(bops->fd);
	else if (mocs_index == DISPLAYABLE_MOCS_INDEX)
		mocs_index = intel_get_displayable_mocs_index(bops->fd);
	buf->mocs_index = mocs_index;
	IGT_INIT_LIST_HEAD(&buf->link);

	tile_width = __get_min_stride(width, bpp, tiling);
	aligned_height = __get_aligned_height(height, bpp, tiling);

	if (bo_stride)
		buf->surface[0].stride = bo_stride;
	else
		buf->surface[0].stride = tile_width;

	size = buf->surface[0].size = buf->surface[0].stride * aligned_height;

	if (compression && !HAS_FLATCCS(buf_ops_get_devid(bops))) {
		int aux_width, aux_height;

		igt_require(bops->intel_gen >= 9);
		igt_assert(req_tiling == I915_TILING_Y ||
			   req_tiling == I915_TILING_Yf ||
			   req_tiling == I915_TILING_4);
		/*
		 * On GEN12+ we align the main surface to 4 * 4 main surface
		 * tiles, which is 64kB. These 16 tiles are mapped by 4 AUX
		 * CCS units, that is 4 * 64 bytes. These 4 CCS units are in
		 * turn mapped by one L1 AUX page table entry.
		 */
		aux_width = intel_buf_ccs_width(bops->intel_gen, buf);
		aux_height = intel_buf_ccs_height(bops->intel_gen, buf);

		buf->ccs[0].offset = buf->surface[0].stride * ALIGN(height, 32);
		buf->ccs[0].stride = aux_width;
		size = buf->ccs[0].offset + aux_width * aux_height;
	}

	/* Store buffer size to avoid mistakes in calculating it again */
	buf->size = size;
	buf->handle = handle;

	if (bops->driver == INTEL_DRIVER_XE)
		igt_assert_f(region != -1, "Xe requires region awareness, "
					   "use api which passes valid region\n");
	buf->region = region;

	if (!handle) {
		if (!bo_size)
			bo_size = size;

		if (bops->driver == INTEL_DRIVER_I915) {
			if (__gem_create_in_memory_regions(bops->fd, &buf->handle, &bo_size, region))
				igt_assert_eq(__gem_create(bops->fd, &bo_size, &buf->handle), 0);
		} else {
			uint16_t cpu_caching = __xe_default_cpu_caching(bops->fd, region, 0);

			if (intel_gen(bops->devid) >= 20 && compression)
				cpu_caching = DRM_XE_GEM_CPU_CACHING_WC;

			bo_size = ALIGN(bo_size, xe_get_default_alignment(bops->fd));
			buf->handle = xe_bo_create_caching(bops->fd, 0, bo_size, region,
							   DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM,
							   cpu_caching);
		}
	}

	igt_assert(bo_size >= size);

	/* Store gem bo size */
	buf->bo_size = bo_size;

	if (bops->driver == INTEL_DRIVER_I915)
		set_hw_tiled(bops, buf);
}

/**
 * intel_buf_init
 * @bops: pointer to buf_ops
 * @buf: pointer to intel_buf structure to be filled
 * @width: surface width
 * @height: surface height
 * @bpp: bits-per-pixel (8 / 16 / 32 / 64)
 * @alignment: alignment of the stride for linear surfaces
 * @tiling: surface tiling
 * @compression: surface compression type
 *
 * Function creates new BO within intel_buf structure and fills all
 * structure fields. Takes bo handle ownership.
 *
 * Note. For X / Y if GPU supports fences HW tiling is configured.
 */
void intel_buf_init(struct buf_ops *bops,
		    struct intel_buf *buf,
		    int width, int height, int bpp, int alignment,
		    uint32_t tiling, uint32_t compression)
{
	uint64_t region;
	uint8_t pat_index = DEFAULT_PAT_INDEX;

	if (compression && intel_gen(bops->devid) >= 20)
		pat_index = intel_get_pat_idx_uc_comp(bops->fd);

	region = bops->driver == INTEL_DRIVER_I915 ? I915_SYSTEM_MEMORY :
						     system_memory(bops->fd);
	__intel_buf_init(bops, 0, buf, width, height, bpp, alignment,
			 tiling, compression, 0, 0, region,
			 pat_index,
			 DEFAULT_MOCS_INDEX);

	intel_buf_set_ownership(buf, true);
}

/**
 * intel_buf_init_in_region
 *
 * Same as intel_buf_init with the additional region argument
 */
void intel_buf_init_in_region(struct buf_ops *bops,
			      struct intel_buf *buf,
			      int width, int height, int bpp, int alignment,
			      uint32_t tiling, uint32_t compression,
			      uint64_t region)
{
	uint8_t pat_index = DEFAULT_PAT_INDEX;

	if (compression && intel_gen(bops->devid) >= 20)
		pat_index = intel_get_pat_idx_uc_comp(bops->fd);

	__intel_buf_init(bops, 0, buf, width, height, bpp, alignment,
			 tiling, compression, 0, 0, region,
			 pat_index,
			 DEFAULT_MOCS_INDEX);

	intel_buf_set_ownership(buf, true);
}

/**
 * intel_buf_close
 * @bops: pointer to buf_ops
 * @buf: pointer to intel_buf structure
 *
 * Function closes gem BO inside intel_buf if bo is owned by intel_buf.
 * For handle passed from the caller intel_buf doesn't take ownership and
 * doesn't close it in close()/destroy() paths. When intel_buf was previously
 * added to intel_bb (intel_bb_add_intel_buf() call) it is tracked there and
 * must be removed from its internal structures.
 */
void intel_buf_close(struct buf_ops *bops, struct intel_buf *buf)
{
	igt_assert(bops);
	igt_assert(buf);

	/* If buf is tracked by some intel_bb ensure it will be removed there */
	if (buf->ibb) {
		intel_bb_remove_intel_buf(buf->ibb, buf);
		buf->addr.offset = INTEL_BUF_INVALID_ADDRESS;
		buf->ibb = NULL;
		IGT_INIT_LIST_HEAD(&buf->link);
	}

	if (buf->is_owner)
		gem_close(bops->fd, buf->handle);
}

/**
 * intel_buf_init_using_handle_and_size
 * @bops: pointer to buf_ops
 * @handle: BO handle created by the caller
 * @buf: pointer to intel_buf structure to be filled
 * @width: surface width
 * @height: surface height
 * @bpp: bits-per-pixel (8 / 16 / 32 / 64)
 * @alignment: alignment of the stride for linear surfaces
 * @tiling: surface tiling
 * @compression: surface compression type
 * @size: real bo size
 *
 * Function configures BO handle within intel_buf structure passed by the caller
 * (with all its metadata - width, height, ...). Useful if BO was created
 * outside.
 *
 * Note: intel_buf_close() can be used because intel_buf is aware it is not
 * buffer owner so it won't close it underneath.
 */
void intel_buf_init_using_handle_and_size(struct buf_ops *bops,
					  uint32_t handle,
					  struct intel_buf *buf,
					  int width, int height, int bpp, int alignment,
					  uint32_t req_tiling, uint32_t compression,
					  uint64_t size)
{
	uint8_t pat_index = DEFAULT_PAT_INDEX;

	igt_assert(handle);
	igt_assert(size);

	if (compression && intel_gen(bops->devid) >= 20)
		pat_index = intel_get_pat_idx_uc_comp(bops->fd);

	__intel_buf_init(bops, handle, buf, width, height, bpp, alignment,
			 req_tiling, compression, size, 0, -1, pat_index,
			 DEFAULT_MOCS_INDEX);
}

/**
 * intel_buf_init_full
 * @bops: pointer to buf_ops
 * @handle: BO handle created by the caller
 * @buf: pointer to intel_buf structure to be filled
 * @width: surface width
 * @height: surface height
 * @bpp: bits-per-pixel (8 / 16 / 32 / 64)
 * @alignment: alignment of the stride for linear surfaces
 * @req_tiling: surface tiling
 * @compression: surface compression type
 * @size: real bo size
 * @stride: bo stride
 * @region: region
 * @pat_index: pat_index to use for the binding (only used on xe)
 * @pat_index: mocs_index to use for operations using this intel_buf, like render
 * copy.
 *
 * Function configures BO handle within intel_buf structure passed by the caller
 * (with all its metadata - width, height, ...). Useful if BO was created
 * outside. Allows passing real size which caller is aware of.
 *
 * Note: intel_buf_close() can be used because intel_buf is aware it is not
 * buffer owner so it won't close it underneath.
 */
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
			 uint8_t mocs_index)
{
	__intel_buf_init(bops, handle, buf, width, height, bpp, alignment,
			 req_tiling, compression, size, stride, region,
			 pat_index, mocs_index);
}

/**
 * intel_buf_create
 * @bops: pointer to buf_ops
 * @width: surface width
 * @height: surface height
 * @bpp: bits-per-pixel (8 / 16 / 32 / 64)
 * @alignment: alignment of the stride for linear surfaces
 * @tiling: surface tiling
 * @compression: surface compression type
 *
 * Function creates intel_buf with created BO handle. Takes ownership of the
 * buffer.
 */
struct intel_buf *intel_buf_create(struct buf_ops *bops,
				   int width, int height,
				   int bpp, int alignment,
				   uint32_t req_tiling, uint32_t compression)
{
	struct intel_buf *buf;

	igt_assert(bops);

	buf = calloc(1, sizeof(*buf));
	igt_assert(buf);

	intel_buf_init(bops, buf, width, height, bpp, alignment,
		       req_tiling, compression);

	return buf;
}

/**
 * intel_buf_create_using_handle_and_size
 * @bops: pointer to buf_ops
 * @handle: BO handle created by the caller
 * @width: surface width
 * @height: surface height
 * @bpp: bits-per-pixel (8 / 16 / 32 / 64)
 * @alignment: alignment of the stride for linear surfaces
 * @tiling: surface tiling
 * @compression: surface compression type
 * @size: real bo size
 *
 * Function creates intel_buf with passed BO handle from the caller. Doesn't
 * take ownership of the buffer. close()/destroy() paths doesn't close
 * passed handle unless buffer will take ownership using set_ownership().
 */
struct intel_buf *intel_buf_create_using_handle_and_size(struct buf_ops *bops,
							 uint32_t handle,
							 int width, int height,
							 int bpp, int alignment,
							 uint32_t req_tiling,
							 uint32_t compression,
							 uint64_t size)
{
	igt_assert(handle);
	igt_assert(size);
	return intel_buf_create_full(bops, handle, width, height, bpp, alignment,
				     req_tiling, compression, size, 0, -1,
				     DEFAULT_PAT_INDEX, DEFAULT_MOCS_INDEX);
}

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
					uint8_t mocs_index)
{
	struct intel_buf *buf;

	igt_assert(bops);

	buf = calloc(1, sizeof(*buf));
	igt_assert(buf);

	__intel_buf_init(bops, handle, buf, width, height, bpp, alignment,
			 req_tiling, compression, size, stride, region,
			 pat_index, mocs_index);

	return buf;
}

/**
 * intel_buf_destroy
 * @buf: intel_buf
 *
 * Function frees intel_buf memory. It closes bo handle if intel_buf has
 * buffer ownership.
 */
void intel_buf_destroy(struct intel_buf *buf)
{
	igt_assert(buf);
	igt_assert(buf->ptr == NULL);

	intel_buf_close(buf->bops, buf);

	free(buf);
}

void *intel_buf_cpu_map(struct intel_buf *buf, bool write)
{
	int fd = buf_ops_get_fd(buf->bops);

	igt_assert(buf);
	igt_assert(buf->ptr == NULL); /* already mapped */

	buf->cpu_write = write;

	if (is_xe_device(fd)) {
		buf->ptr = xe_bo_map(fd, buf->handle, buf->bo_size);
	} else {
		buf->ptr = gem_mmap__cpu_coherent(fd, buf->handle, 0,
						  buf->bo_size,
						  write ? PROT_WRITE : PROT_READ);

		gem_set_domain(fd, buf->handle,
			       I915_GEM_DOMAIN_CPU,
			       write ? I915_GEM_DOMAIN_CPU : 0);
	}

	return buf->ptr;
}

void *intel_buf_device_map(struct intel_buf *buf, bool write)
{
	int fd = buf_ops_get_fd(buf->bops);

	igt_assert(buf);
	igt_assert(buf->ptr == NULL); /* already mapped */

	if (is_xe_device(fd)) {
		buf->ptr = xe_bo_map(fd, buf->handle, buf->bo_size);
	} else {
		buf->ptr = gem_mmap__device_coherent(fd, buf->handle, 0,
						     buf->bo_size,
						     write ? PROT_WRITE : PROT_READ);

		gem_set_domain(fd, buf->handle,
			       I915_GEM_DOMAIN_WC,
			       write ? I915_GEM_DOMAIN_WC : 0);
	}

	return buf->ptr;
}

void intel_buf_unmap(struct intel_buf *buf)
{
	igt_assert(buf);
	igt_assert(buf->ptr);

	munmap(buf->ptr, buf->bo_size);
	buf->ptr = NULL;
}

void intel_buf_flush_and_unmap(struct intel_buf *buf)
{
	igt_assert(buf);
	igt_assert(buf->ptr);

	if (buf->cpu_write)
		gem_sw_finish(buf_ops_get_fd(buf->bops), buf->handle);

	intel_buf_unmap(buf);
}

void intel_buf_print(const struct intel_buf *buf)
{
	igt_info("[name: %s]\n", buf->name);
	igt_info("[%u]: w: %u, h: %u, stride: %u, size: %" PRIx64
		 ", buf-size: %" PRIx64 ", bo-size: %" PRIx64
		 ", bpp: %u, tiling: %u, compress: %u\n",
		 buf->handle, intel_buf_width(buf), intel_buf_height(buf),
		 buf->surface[0].stride, buf->surface[0].size,
		 intel_buf_size(buf), intel_buf_bo_size(buf), buf->bpp,
		 buf->tiling, buf->compression);
	igt_info(" ccs <offset: %u, stride: %u, w: %u, h: %u> cc <offset: %u>\n",
		 buf->ccs[0].offset,
		 intel_buf_ccs_width(buf->bops->intel_gen, buf),
		 intel_buf_ccs_height(buf->bops->intel_gen, buf),
		 buf->ccs[0].stride, buf->cc.offset);
	igt_info(" addr <offset: %p, ctx: %u>\n",
		 from_user_pointer(buf->addr.offset), buf->addr.ctx);
}

void intel_buf_dump(const struct intel_buf *buf, const char *filename)
{
	int i915 = buf_ops_get_fd(buf->bops);
	uint64_t size = intel_buf_size(buf);
	FILE *out;
	void *ptr;

	ptr = gem_mmap__device_coherent(i915, buf->handle, 0, size, PROT_READ);
	out = fopen(filename, "wb");
	igt_assert(out);
	fwrite(ptr, size, 1, out);
	fclose(out);
	munmap(ptr, size);
}

const char *intel_buf_set_name(struct intel_buf *buf, const char *name)
{
	return strncpy(buf->name, name, INTEL_BUF_NAME_MAXSIZE);
}

static void __intel_buf_write_to_png(struct buf_ops *bops,
				     struct intel_buf *buf,
				     const char *filename,
				     bool write_ccs)
{
	cairo_surface_t *surface;
	cairo_status_t ret;
	void *linear;
	int format, width, height, stride, offset;
	unsigned int gen = bops->intel_gen;

	igt_assert_eq(posix_memalign(&linear, 16, intel_buf_size(buf)), 0);

	format = write_ccs ? CAIRO_FORMAT_A8 : CAIRO_FORMAT_RGB24;
	width = write_ccs ? intel_buf_ccs_width(gen, buf) : intel_buf_width(buf);
	height = write_ccs ? intel_buf_ccs_height(gen, buf) : intel_buf_height(buf);
	stride = write_ccs ? buf->ccs[0].stride : buf->surface[0].stride;
	offset = write_ccs ? buf->ccs[0].offset : 0;

	intel_buf_to_linear(bops, buf, linear);

	surface = cairo_image_surface_create_for_data((uint8_t *) linear + offset,
						      format, width, height,
						      stride);
	ret = cairo_surface_write_to_png(surface, filename);
	igt_assert(ret == CAIRO_STATUS_SUCCESS);
	cairo_surface_destroy(surface);

	free(linear);
}

void intel_buf_write_to_png(struct intel_buf *buf, const char *filename)
{
	__intel_buf_write_to_png(buf->bops, buf, filename, false);
}

void intel_buf_write_aux_to_png(struct intel_buf *buf, const char *filename)
{
	igt_assert(buf->compression);

	__intel_buf_write_to_png(buf->bops, buf, filename, true);
}
static void __intel_buf_raw_write_to_png(struct buf_ops *bops,
					 struct intel_buf *buf,
					 const char *filename)
{
	cairo_surface_t *surface;
	cairo_status_t ret;
	uint8_t *linear;
	int format, width, height, stride;

	format = CAIRO_FORMAT_RGB24;
	width = buf->surface[0].stride / 4;
	height = __get_aligned_height(intel_buf_height(buf),
				      buf->bpp, buf->tiling);
	stride = buf->surface[0].stride;

	if (is_xe_device(bops->fd))
		linear = xe_bo_map(bops->fd, buf->handle, buf->bo_size);
	else
		linear = gem_mmap__device_coherent(bops->fd, buf->handle,
						   0, buf->bo_size, PROT_READ);
	surface = cairo_image_surface_create_for_data(linear,
						      format, width, height,
						      stride);
	ret = cairo_surface_write_to_png(surface, filename);
	igt_assert(ret == CAIRO_STATUS_SUCCESS);
	cairo_surface_destroy(surface);

	munmap(linear, buf->bo_size);
}

__attribute__((format(printf, 2, 3)))
void intel_buf_raw_write_to_png(struct intel_buf *buf, const char *namefmt, ...)
{
	char *filename;
	int ret;
	va_list ap;

	va_start(ap, namefmt);
	ret = vasprintf(&filename, namefmt, ap);
	igt_assert(ret >= 0);
	va_end(ap);

	__intel_buf_raw_write_to_png(buf->bops, buf, filename);

	free(filename);
}

static void *alloc_aligned(uint64_t size)
{
	void *p;

	igt_assert_eq(posix_memalign(&p, 16, size), 0);

	return p;
}

void intel_buf_draw_pattern(struct buf_ops *bops, struct intel_buf *buf,
			    int x, int y, int w, int h,
			    int cx, int cy, int cw, int ch,
			    bool use_alternate_colors)
{
	cairo_surface_t *surface;
	cairo_pattern_t *pat;
	cairo_t *cr;
	void *linear;

	linear = alloc_aligned(buf->surface[0].size);

	surface = cairo_image_surface_create_for_data(linear,
						      CAIRO_FORMAT_RGB24,
						      intel_buf_width(buf),
						      intel_buf_height(buf),
						      buf->surface[0].stride);

	cr = cairo_create(surface);

	cairo_rectangle(cr, cx, cy, cw, ch);
	cairo_clip(cr);

	pat = cairo_pattern_create_mesh();
	cairo_mesh_pattern_begin_patch(pat);
	cairo_mesh_pattern_move_to(pat, x,   y);
	cairo_mesh_pattern_line_to(pat, x+w, y);
	cairo_mesh_pattern_line_to(pat, x+w, y+h);
	cairo_mesh_pattern_line_to(pat, x,   y+h);
	if (use_alternate_colors) {
		cairo_mesh_pattern_set_corner_color_rgb(pat, 0, 0.0, 1.0, 1.0);
		cairo_mesh_pattern_set_corner_color_rgb(pat, 1, 1.0, 0.0, 1.0);
		cairo_mesh_pattern_set_corner_color_rgb(pat, 2, 1.0, 1.0, 0.0);
		cairo_mesh_pattern_set_corner_color_rgb(pat, 3, 0.0, 0.0, 0.0);
	} else {
		cairo_mesh_pattern_set_corner_color_rgb(pat, 0, 1.0, 0.0, 0.0);
		cairo_mesh_pattern_set_corner_color_rgb(pat, 1, 0.0, 1.0, 0.0);
		cairo_mesh_pattern_set_corner_color_rgb(pat, 2, 0.0, 0.0, 1.0);
		cairo_mesh_pattern_set_corner_color_rgb(pat, 3, 1.0, 1.0, 1.0);
	}
	cairo_mesh_pattern_end_patch(pat);

	cairo_rectangle(cr, x, y, w, h);
	cairo_set_source(cr, pat);
	cairo_fill(cr);
	cairo_pattern_destroy(pat);

	cairo_destroy(cr);

	cairo_surface_destroy(surface);

	linear_to_intel_buf(bops, buf, linear);

	free(linear);
}

#define DEFAULT_BUFOPS(__gen_start, __gen_end) \
	.gen_start          = __gen_start, \
	.gen_end            = __gen_end, \
	.supported_hw_tiles = TILE_X | TILE_Y, \
	.linear_to          = copy_linear_to_wc, \
	.linear_to_x        = copy_linear_to_gtt, \
	.linear_to_y        = copy_linear_to_gtt, \
	.linear_to_yf       = copy_linear_to_yf, \
	.linear_to_ys       = copy_linear_to_ys, \
	.linear_to_tile4    = copy_linear_to_tile4, \
	.linear_to_tile64   = copy_linear_to_tile64, \
	.to_linear          = copy_wc_to_linear, \
	.x_to_linear        = copy_gtt_to_linear, \
	.y_to_linear        = copy_gtt_to_linear, \
	.yf_to_linear       = copy_yf_to_linear, \
	.ys_to_linear       = copy_ys_to_linear, \
	.tile4_to_linear    = copy_tile4_to_linear, \
	.tile64_to_linear   = copy_tile64_to_linear

static const struct buf_ops buf_ops_arr[] = {
	{
		DEFAULT_BUFOPS(2, 8),
		.supported_tiles    = TILE_NONE | TILE_X | TILE_Y,
	},

	{
		DEFAULT_BUFOPS(9, 11),
		.supported_tiles    = TILE_NONE | TILE_X | TILE_Y | TILE_Yf,
	},

	{
		DEFAULT_BUFOPS(12, ~0U),
		.supported_tiles   = TILE_NONE | TILE_X | TILE_Y | TILE_Yf | TILE_Ys | TILE_4 | TILE_64,
	},
};

static bool probe_hw_tiling(struct buf_ops *bops, uint32_t tiling,
			    bool *swizzling_supported)
{
	uint64_t size = 256 * 256;
	uint32_t handle, buf_tiling, buf_swizzle, phys_swizzle;
	uint32_t stride;
	int ret;
	bool is_set = false;

	stride = get_stride(bops->devid, tiling);
	handle = gem_create(bops->fd, size);

	/* Single shot, if no fences are available we fail immediately */
	ret = __set_tiling(bops->fd, handle, tiling, stride, NULL, NULL);
	if (ret)
		goto end;

	is_set = __get_tiling(bops->fd, handle, &buf_tiling, &buf_swizzle,
			      &phys_swizzle);
	if (is_set) {
		if (tiling == I915_TILING_X)
			bops->swizzle_x = buf_swizzle;
		else if (tiling == I915_TILING_Y)
			bops->swizzle_y = buf_swizzle;

		*swizzling_supported = buf_swizzle == phys_swizzle;
	}
end:
	gem_close(bops->fd, handle);

	return is_set;
}

/*
 * Simple idempotency test between HW -> SW and SW -> HW BO.
 */
static void idempotency_selftest(struct buf_ops *bops, uint32_t tiling)
{
	struct intel_buf buf;
	uint8_t *linear_in, *linear_out, *map;
	int i;
	const int width = 512;
	const int height = 512;
	const int bpp = 32;
	const int size = width * height * bpp / 8;
	bool software_tiling = false;

	if (!is_hw_tiling_supported(bops, tiling))
		return;

	linear_in = alloc_aligned(size);
	linear_out = alloc_aligned(size);

	for (i = 0; i < size; i++)
		linear_in[i] = i % 253; /* prime chosen intentionally */

	do {
		igt_debug("Checking idempotency, SW: %s, HW: %s, tiling: %s\n",
			  bool_str(software_tiling),
			  bool_str(!software_tiling),
			  tiling_str(tiling));
		intel_buf_init(bops, &buf, width, height, bpp, 0, tiling, false);
		buf_ops_set_software_tiling(bops, tiling, software_tiling);

		linear_to_intel_buf(bops, &buf, (uint32_t *) linear_in);
		map = __gem_mmap_offset__cpu(bops->fd, buf.handle, 0,
					     buf.surface[0].size, PROT_READ);
		if (!map)
			map = gem_mmap__cpu(bops->fd, buf.handle, 0,
					    buf.surface[0].size, PROT_READ);
		gem_set_domain(bops->fd, buf.handle, I915_GEM_DOMAIN_CPU, 0);
		igt_assert(memcmp(linear_in, map, size));
		munmap(map, size);

		buf_ops_set_software_tiling(bops, tiling, !software_tiling);
		intel_buf_to_linear(bops, &buf, (uint32_t *) linear_out);
		igt_assert(!memcmp(linear_in, linear_out, size));

		intel_buf_close(bops, &buf);

		software_tiling = !software_tiling;
	} while (software_tiling);

	igt_debug("Idempotency for %s tiling OK\n", tiling_str(tiling));
	buf_ops_set_software_tiling(bops, tiling, false);
}

uint64_t intel_buf_size(const struct intel_buf *buf)
{
	return buf->size;
}

uint64_t intel_buf_bo_size(const struct intel_buf *buf)
{
	return buf->bo_size;
}

static struct buf_ops *__buf_ops_create(int fd, bool check_idempotency)
{
	struct buf_ops *bops = calloc(1, sizeof(*bops));
	unsigned int generation;
	uint32_t devid;

	igt_assert(bops);

	devid = intel_get_drm_devid(fd);
	generation = intel_gen(devid);

	/* Predefined settings: see intel_device_info? */
	for (int i = 0; i < ARRAY_SIZE(buf_ops_arr); i++) {
		if (generation >= buf_ops_arr[i].gen_start &&
		    generation <= buf_ops_arr[i].gen_end) {
			memcpy(bops, &buf_ops_arr[i], sizeof(*bops));
			break;
		}
	}

	bops->fd = fd;
	bops->intel_gen = generation;
	bops->devid = devid;
	bops->driver = is_i915_device(fd) ? INTEL_DRIVER_I915 :
					    is_xe_device(fd) ? INTEL_DRIVER_XE : 0;
	igt_assert(bops->driver);
	igt_debug("generation: %d, supported tiles: 0x%02x, driver: %s\n",
		  bops->intel_gen, bops->supported_tiles,
		  bops->driver == INTEL_DRIVER_I915 ? "i915" : "xe");

	if (bops->driver == INTEL_DRIVER_XE) {
		bops->linear_to = copy_linear_to_none;
		bops->to_linear = copy_none_to_linear;
		bops->linear_to_x = copy_linear_to_x;
		bops->x_to_linear = copy_x_to_linear;
		bops->linear_to_y = copy_linear_to_y;
		bops->y_to_linear = copy_y_to_linear;

		return bops;
	}

	/* Let's probe X and Y hw tiling support */
	if (is_hw_tiling_supported(bops, I915_TILING_X)) {
		bool swizzling_supported;
		bool supported = probe_hw_tiling(bops, I915_TILING_X,
						 &swizzling_supported);

		if (!swizzling_supported && bops->intel_gen < 12) {
			igt_debug("Swizzling for X is not supported\n");
			bops->supported_tiles &= ~TILE_X;
		}

		igt_debug("X fence support: %s\n", bool_str(supported));
		if (!supported) {
			bops->supported_hw_tiles &= ~TILE_X;
			bops->linear_to_x = copy_linear_to_x;
			bops->x_to_linear = copy_x_to_linear;
		}
	}

	if (is_hw_tiling_supported(bops, I915_TILING_Y)) {
		bool swizzling_supported;
		bool supported = probe_hw_tiling(bops, I915_TILING_Y,
						 &swizzling_supported);

		if (!swizzling_supported && bops->intel_gen < 12) {
			igt_debug("Swizzling for Y is not supported\n");
			bops->supported_tiles &= ~TILE_Y;
		}

		igt_debug("Y fence support: %s\n", bool_str(supported));
		if (!supported) {
			bops->supported_hw_tiles &= ~TILE_Y;
			bops->linear_to_y = copy_linear_to_y;
			bops->y_to_linear = copy_y_to_linear;
		}
	}

	/* Disable other tiling format functions if not supported */
	if (!is_tiling_supported(bops, I915_TILING_Yf))
		igt_debug("Yf format not supported\n");

	if (!is_tiling_supported(bops, I915_TILING_Ys))
		igt_debug("Ys format not supported\n");

	if (!is_tiling_supported(bops, I915_TILING_4))
		igt_debug("Tile4 format not supported\n");

	if (!is_tiling_supported(bops, I915_TILING_64))
		igt_debug("Tile64 format not supported\n");

	if (check_idempotency) {
		idempotency_selftest(bops, I915_TILING_X);
		idempotency_selftest(bops, I915_TILING_Y);
	}

	return bops;
}

/**
 * buf_ops_create
 * @fd: device filedescriptor
 *
 * Create buf_ops structure depending on fd-device capabilities.
 *
 * Returns: opaque pointer to buf_ops.
 *
 */
struct buf_ops *buf_ops_create(int fd)
{
	return __buf_ops_create(fd, false);
}

/**
 * buf_ops_create_with_selftest
 * @fd: device filedescriptor
 *
 * Create buf_ops structure depending on fd-device capabilities.
 * Runs with idempotency selftest to verify software tiling gives same
 * result like hardware tiling (gens with mappable gtt).
 *
 * Returns: opaque pointer to buf_ops.
 *
 */
struct buf_ops *buf_ops_create_with_selftest(int fd)
{
	return __buf_ops_create(fd, true);
}

/**
 * buf_ops_destroy
 * @bops: pointer to buf_ops
 *
 * Function frees buf_ops structure.
 */
void buf_ops_destroy(struct buf_ops *bops)
{
	igt_assert(bops);
	free(bops);
}

/**
 * buf_ops_get_fd
 * @bops: pointer to buf_ops
 *
 * Returns: drm fd
 */
int buf_ops_get_fd(struct buf_ops *bops)
{
	igt_assert(bops);

	return bops->fd;
}

/**
 * buf_ops_get_devid
 * @bops: pointer to buf_ops
 *
 * Returns: device id
 */
uint32_t buf_ops_get_devid(struct buf_ops *bops)
{
	igt_assert(bops);

	return bops->devid;
}

/**
 * buf_ops_get_driver
 * @bops: pointer to buf_ops
 *
 * Returns: intel driver enum value
 */
enum intel_driver buf_ops_get_driver(struct buf_ops *bops)
{
	igt_assert(bops);

	return bops->driver;
}

/**
 * buf_ops_set_software_tiling
 * @bops: pointer to buf_ops
 * @tiling: surface tiling
 * @use_software_tiling: if true use software copying methods, otherwise
 * use hardware (via gtt)
 *
 * Function allows switch X / Y surfaces to software / hardware copying methods
 * which honors tiling and swizzling.
 *
 * Returns:
 * false - switch wasn't possible.
 * true - switch to software / hardware method succeed.
 */
bool buf_ops_set_software_tiling(struct buf_ops *bops,
				 uint32_t tiling,
				 bool use_software_tiling)
{
	bool was_changed = true;

	igt_assert(bops);

	/* Until appropriate code is added we don't support SW tiling on Gen2 */
	if (bops->intel_gen == 2) {
		igt_warn("Change to software tiling on Gen2 is not supported!");
		return false;
	}

	switch (tiling) {
	case I915_TILING_NONE:
		igt_debug("-> use SW on tiling NONE\n");
		break;

	case I915_TILING_X:
		if (use_software_tiling) {
			bool supported = buf_ops_has_tiling_support(bops, tiling);

			igt_assert_f(supported, "Cannot switch to X software tiling\n");
			igt_debug("-> change X to SW\n");
			bops->linear_to_x = copy_linear_to_x;
			bops->x_to_linear = copy_x_to_linear;
		} else {
			if (is_hw_tiling_supported(bops, I915_TILING_X)) {
				igt_debug("-> change X to HW\n");
				bops->linear_to_x = copy_linear_to_gtt;
				bops->x_to_linear = copy_gtt_to_linear;
			} else {
				igt_debug("-> X cannot be changed to HW\n");
				was_changed = false;
			}
		}
		break;

	case I915_TILING_Y:
		if (use_software_tiling) {
			bool supported = buf_ops_has_tiling_support(bops, tiling);

			igt_assert_f(supported, "Cannot switch to Y software tiling\n");
			igt_debug("-> change Y to SW\n");
			bops->linear_to_y = copy_linear_to_y;
			bops->y_to_linear = copy_y_to_linear;
		} else {
			if (is_hw_tiling_supported(bops, I915_TILING_Y)) {
				igt_debug("-> change Y to HW\n");
				bops->linear_to_y = copy_linear_to_gtt;
				bops->y_to_linear = copy_gtt_to_linear;
			} else {
				igt_debug("-> Y cannot be changed to HW\n");
				was_changed = false;
			}
		}
		break;

	case I915_TILING_4:
		igt_debug("-> use SW on tiling 4\n");
		break;

	case I915_TILING_64:
		igt_debug("-> use SW on tiling 64\n");
		break;

	case I915_TILING_Yf:
		igt_debug("-> use SW on tiling Yf\n");
		break;

	case I915_TILING_Ys:
		igt_debug("-> use SW on tiling Ys\n");
		break;

	default:
		igt_warn("Invalid tiling: %d\n", tiling);
		was_changed = false;
	}

	return was_changed;
}

/**
 * buf_ops_has_hw_fence
 * @bops: pointer to buf_ops
 * @tiling: surface tiling
 *
 * Function checks if surface with tiling has HW fences which can be used
 * to copy it via gtt.
 *
 * Returns:
 * false - fence for tiling is not supported.
 * true - fence for tiling is supported.
 */
bool buf_ops_has_hw_fence(struct buf_ops *bops, uint32_t tiling)
{
	uint32_t tile_mask = TILE_DEF(tiling);

	igt_assert(bops);

	if (tile_mask & bops->supported_hw_tiles)
		return true;

	return false;
}

/**
 * buf_ops_has_tiling_support
 * @bops: pointer to buf_ops
 * @tiling: surface tiling
 *
 * Function checks capabilities to handle surfaces with tiling in GPU.
 *
 * Returns:
 * false - GPU does not support tiling.
 * true - GPU supports tiling.
 */
bool buf_ops_has_tiling_support(struct buf_ops *bops, uint32_t tiling)
{
	uint32_t tile_mask = TILE_DEF(tiling);

	igt_assert(bops);

	if (tile_mask & bops->supported_tiles)
		return true;

	return false;
}
