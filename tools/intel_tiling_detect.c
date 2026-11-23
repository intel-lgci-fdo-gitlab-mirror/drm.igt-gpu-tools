// SPDX-License-Identifier: MIT
/*
 * Copyright © 2024 Intel Corporation
 */

#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include "drm.h"
#include "igt.h"
#include "intel_blt.h"
#include "intel_common.h"
#include "intel_mocs.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"

static struct param {
	char *device;
	int tiling;
	bool write_png;
	bool print_bb;
	bool print_surface_info;
	int width;
	int height;
} param = {
	.device = NULL,
	.tiling = -1,
	.write_png = false,
	.print_bb = false,
	.print_surface_info = false,
	.width = 256,
	.height = 256,
};

#define NUM_REFS (I915_TILING_64 + 1)
struct intel_buf refs[NUM_REFS] = {};

#define PRINT_SURFACE_INFO(name, obj) do { \
	if (param.print_surface_info) \
		blt_surface_info((name), (obj)); } while (0)

#define WRITE_PNG(fd, id, name, obj, w, h, bpp) do { \
	if (param.write_png) \
		blt_surface_to_png((fd), (id), (name), (obj), (w), (h), (bpp)); } while (0)

const char *help_str =
	"  -b\t\tPrint bb\n"
	"  -d path\tOpen device at path\n"
	"  -s\t\tPrint surface info\n"
	"  -p\t\tWrite PNG\n"
	"  -W\t\tWidth (default 256)\n"
	"  -H\t\tHeight (default 256)\n"
	"  -h\t\tHelp\n"
	;

enum copy_fn {
	FAST_COPY,
	BLOCK_COPY,
	RENDER_COPY,
};

static const char * const copy_fn_name[] = {
	[FAST_COPY] = "fast-copy",
	[BLOCK_COPY] = "block-copy",
	[RENDER_COPY] = "render-copy",
};

static void detect_blt_tiling(const struct blt_copy_object *buf)
{
	bool detected = false;
	int i;

	for (i = 0; i < ARRAY_SIZE(refs); i++) {
		if (!refs[i].bops)
			continue;

		if (!memcmp(buf->ptr, refs[i].ptr, buf->size)) {
			detected = true;
			break;
		}
	}

	igt_info("buffer tiling (claimed): %s, detected: %s\n",
		 blt_tiling_name(buf->tiling),
		 detected ? blt_tiling_name(i915_tile_to_blt_tile(i)) : "unknown");
}

static void blt_copy(int fd,
		     intel_ctx_t *ctx,
		     const struct intel_execution_engine2 *e,
		     uint32_t width, uint32_t height,
		     enum blt_tiling_type tiling,
		     enum copy_fn fn)
{
	struct blt_copy_data blt = {};
	struct blt_block_copy_data_ext ext = {}, *pext = &ext;
	struct blt_copy_object *src, *dst;
	const uint32_t bpp = 32;
	uint64_t bb_size;
	uint64_t ahnd = intel_allocator_open(fd, ctx->vm, INTEL_ALLOCATOR_RELOC);
	uint32_t run_id = tiling;
	uint32_t src_region, dst_region;
	uint32_t bb;
	uint8_t uc_mocs = intel_get_uc_mocs_index(fd);
	bool is_xe = is_xe_device(fd);

	if (is_xe) {
		bb_size = xe_bb_size(fd, SZ_4K);
		src_region = system_memory(fd);
		dst_region = vram_if_possible(fd, 0);
		bb = xe_bo_create(fd, 0, bb_size, src_region, 0);
	} else {
		bb_size = SZ_4K;
		src_region = REGION_SMEM;
		dst_region = gem_has_lmem(fd) ? REGION_LMEM(0) : REGION_SMEM;
		igt_assert(__gem_create_in_memory_regions(fd, &bb, &bb_size, src_region) == 0);
	}

	if (!blt_uses_extended_block_copy(fd))
		pext = NULL;

	blt_copy_init(fd, &blt);

	src = blt_create_object(&blt, src_region, width, height, bpp, uc_mocs,
				T_LINEAR, COMPRESSION_DISABLED,
				COMPRESSION_TYPE_3D, true);
	dst = blt_create_object(&blt, dst_region, width, height, bpp, uc_mocs,
				tiling, COMPRESSION_DISABLED,
				COMPRESSION_TYPE_3D, true);
	PRINT_SURFACE_INFO("src", src);
	PRINT_SURFACE_INFO("dst", dst);

	blt_surface_fill_rect(fd, src, width, height);

	blt.color_depth = CD_32bit;
	blt.print_bb = param.print_bb;
	blt_set_copy_object(&blt.src, src);
	blt_set_copy_object(&blt.dst, dst);
	blt_set_object_ext(&ext.src, 0, width, height, SURFACE_TYPE_2D);
	blt_set_object_ext(&ext.dst, 0, width, height, SURFACE_TYPE_2D);
	blt_set_batch(&blt.bb, bb, bb_size, src_region);
	if (fn == BLOCK_COPY)
		blt_block_copy(fd, ctx, e, ahnd, &blt, pext);
	else if (fn == FAST_COPY)
		blt_fast_copy(fd, ctx, e, ahnd, &blt);
	if (is_xe)
		intel_ctx_xe_sync(ctx, true);
	else
		gem_sync(fd, dst->handle);

	WRITE_PNG(fd, run_id, copy_fn_name[fn], &blt.dst, width, height, bpp);

	detect_blt_tiling(dst);

	/* Politely clean vm */
	put_offset(ahnd, src->handle);
	put_offset(ahnd, dst->handle);
	put_offset(ahnd, bb);
	intel_allocator_bind(ahnd, 0, 0);
	blt_destroy_object(fd, src);
	blt_destroy_object(fd, dst);
	gem_close(fd, bb);
	put_ahnd(ahnd);
}

static void detect_render_tiling(struct intel_buf *buf)
{
	bool detected = false;
	int i;

	intel_buf_device_map(buf, false);

	for (i = 0; i < ARRAY_SIZE(refs); i++) {
		if (!refs[i].bops)
			continue;

		if (!memcmp(buf->ptr, refs[i].ptr, buf->size)) {
			detected = true;
			break;
		}
	}

	intel_buf_unmap(buf);

	igt_info("buffer tiling (claimed): %s, detected: %s\n",
		 blt_tiling_name(buf->tiling),
		 detected ? blt_tiling_name(i915_tile_to_blt_tile(i)) : "unknown");
}

static void scratch_buf_init(struct buf_ops *bops,
			     struct intel_buf *buf,
			     int width, int height,
			     uint32_t tiling,
			     enum i915_compression compression)
{
	int fd = buf_ops_get_fd(bops);
	int bpp = 32;

	/*
	 * We use system memory even if vram is possible because wc mapping
	 * is extremely slow.
	 */
	intel_buf_init_in_region(bops, buf, width, height, bpp, 0,
				 tiling, compression,
				 is_xe_device(fd) ? system_memory(fd) : REGION_SMEM);

	igt_assert(intel_buf_width(buf) == width);
	igt_assert(intel_buf_height(buf) == height);
}

static void render(int fd, uint32_t width, uint32_t height, uint32_t tiling)
{
	struct buf_ops *bops;
	struct intel_bb *ibb;
	struct intel_buf src, dst;
	uint32_t devid = intel_get_drm_devid(fd);
	igt_render_copyfunc_t render_copy = NULL;

	bops = buf_ops_create(fd);

	igt_debug("%s() gen: %d\n", __func__, intel_gen(devid));

	ibb = intel_bb_create(fd, SZ_4K);

	scratch_buf_init(bops, &src, width, height, I915_TILING_NONE,
			 I915_COMPRESSION_NONE);
	scratch_buf_init(bops, &dst, width, height, tiling,
			 I915_COMPRESSION_NONE);

	/* Copy reference linear image */
	intel_buf_device_map(&src, true);
	memcpy(src.ptr, refs[0].ptr, src.bo_size);
	intel_buf_unmap(&src);

	render_copy = igt_get_render_copyfunc(fd);
	igt_assert(render_copy);

	render_copy(ibb,
		    &src,
		    0, 0, width, height,
		    &dst,
		    0, 0);

	intel_bb_sync(ibb);
	intel_bb_destroy(ibb);

	detect_render_tiling(&dst);

	if (param.write_png)
		intel_buf_raw_write_to_png(&dst, "render-tile-%s.png",
					   blt_tiling_name(i915_tile_to_blt_tile(tiling)));

	intel_buf_close(bops, &src);
	intel_buf_close(bops, &dst);

	buf_ops_destroy(bops);
}

static void single_copy(int fd,
			uint32_t width, uint32_t height,
			int tiling, enum copy_fn fn)
{
	/* for potential hangs */
	fd = drm_reopen_driver(fd);

	switch (fn) {
	case BLOCK_COPY:
	case FAST_COPY:
		if (is_xe_device(fd)) {
			struct drm_xe_engine_class_instance inst = {
				.engine_class = DRM_XE_ENGINE_CLASS_COPY,
			};
			uint32_t vm, exec_queue;
			intel_ctx_t *ctx;

			vm = xe_vm_create(fd, 0, 0);
			exec_queue = xe_exec_queue_create(fd, vm, &inst, 0);
			ctx = intel_ctx_xe(fd, vm, exec_queue, 0, 0, 0);

			blt_copy(fd, ctx, NULL, width, height, tiling, fn);

			xe_exec_queue_destroy(fd, exec_queue);
			xe_vm_destroy(fd, vm);
			free(ctx);
		} else {
			const struct intel_execution_engine2 *e;
			const intel_ctx_t *ctx;

			ctx = intel_ctx_create_all_physical(fd);
			for_each_ctx_engine(fd, ctx, e) {
				if (e->class != I915_ENGINE_CLASS_COPY)
					continue;

				if (fn == BLOCK_COPY && !gem_engine_can_block_copy(fd, e))
					continue;

				blt_copy(fd, (intel_ctx_t *)ctx, e,
					 width, height, tiling, fn);
				break;
			}
			intel_ctx_destroy(fd, ctx);
		}
		break;

	case RENDER_COPY:
		render(fd, width, height, blt_tile_to_i915_tile(tiling));
		break;
	}

	drm_close_driver(fd);
}

static void soft_tile(struct buf_ops *bops, struct intel_buf *buf,
		      uint32_t width, uint32_t height, uint32_t tiling)
{
	struct blt_copy_data blt = {};
	struct blt_copy_object *src;
	int fd = buf_ops_get_fd(bops);
	uint8_t uc_mocs = intel_get_uc_mocs_index(fd);
	enum blt_compression_type comp_type = COMPRESSION_TYPE_3D;
	uint64_t sys_region;
	const int bpp = 32;

	sys_region = is_xe_device(fd) ? system_memory(fd) : REGION_SMEM;
	blt_copy_init(fd, &blt);
	src = blt_create_object(&blt, sys_region, width, height, bpp, uc_mocs,
				T_LINEAR, COMPRESSION_DISABLED, comp_type, true);
	blt_surface_fill_rect(fd, src, width, height);

	intel_buf_init(bops, buf, width, height, bpp, 0, tiling, false);
	buf_ops_set_software_tiling(bops, tiling, true);

	linear_to_intel_buf(bops, buf, src->ptr);

	if (param.write_png)
		intel_buf_raw_write_to_png(buf, "reference-tile-%s.png",
					   blt_tiling_name(i915_tile_to_blt_tile(tiling)));
}

static bool try_tile[NUM_REFS] = {
	[I915_TILING_NONE] = true,
	[I915_TILING_X] = true,
	[I915_TILING_Y] = true,
	[I915_TILING_4] = true,
	[I915_TILING_Yf] = true,
	[I915_TILING_64] = true,
};

int main(int argc, char *argv[])
{
	struct buf_ops *bops;
	int fd, i, fn, opt;

	while ((opt = getopt(argc, argv, "bd:psW:H:h")) != -1) {
		switch (opt) {
		case 'b':
			param.print_bb = true;
			break;

		case 'd':
			param.device = strdup(optarg);
			break;

		case 'p':
			param.write_png = true;
			break;

		case 's':
			param.print_surface_info = true;
			break;

		case 'W':
			param.width = atoi(optarg);
			break;

		case 'H':
			param.height = atoi(optarg);
			break;

		case 'h':
			igt_info("%s", help_str);
			exit(0);

		default:
			break;
		}
	}

	if (!param.device)
		fd = drm_open_driver(DRIVER_INTEL | DRIVER_XE);
	else
		fd = open(param.device, O_RDWR);
	if (fd < 0) {
		if (param.device)
			igt_info("Can't open device: %s\n", param.device);
		else
			igt_info("Can't open default device\n");
		exit(0);
	}

	if (is_xe_device(fd))
		xe_device_get(fd);

	bops = buf_ops_create(fd);

	for (i = 0; i < ARRAY_SIZE(refs); i++) {
		igt_info("Building reference tile[%-7s] = %s\n",
			 blt_tiling_name(i915_tile_to_blt_tile(i)),
			 try_tile[i] ? "yes" : "no");
		if (try_tile[i]) {
			soft_tile(bops, &refs[i],
				  param.width, param.height, i);
			intel_buf_device_map(&refs[i], false);
		}
	}

	for (fn = FAST_COPY; fn <= RENDER_COPY; fn++) {
		if (fn == FAST_COPY && !blt_has_fast_copy(fd))
			continue;

		if (fn == BLOCK_COPY && !blt_has_block_copy(fd))
			continue;

		igt_info("[%s]:\n", copy_fn_name[fn]);

		for (i = 0; i < ARRAY_SIZE(refs); i++)
			if (try_tile[i])
				single_copy(fd, param.width, param.height,
					    i915_tile_to_blt_tile(i), fn);
	}

	for (i = 0; i < ARRAY_SIZE(refs); i++) {
		if (try_tile[i])
			intel_buf_unmap(&refs[i]);
	}

	if (is_xe_device(fd))
		xe_device_put(fd);
	close(fd);
}
