// SPDX-License-Identifier: MIT
/*
 * Copyright 2023 Advanced Micro Devices, Inc.
 */

#include "amdgpu_drm.h"
#include "amd_vpe.h"

#include "igt.h"

#include "lib/amdgpu/amd_mmd_shared.h"

IGT_TEST_DESCRIPTION("Test VPE functionality");

#define MAX_RESOURCES		16

static bool is_vpe_tests_enabled(amdgpu_device_handle device_handle,
		struct mmd_shared_context *shared_context)
{
	struct drm_amdgpu_info_hw_ip info;
	int r;

	r = amdgpu_query_hw_ip_info(device_handle, AMDGPU_HW_IP_VPE, 0, &info);
	igt_assert_eq(r, 0);

	shared_context->vpe_ip_version_major = info.hw_ip_version_major;
	shared_context->vpe_ip_version_minor = info.hw_ip_version_minor;
	shared_context->vpe_ring = !!info.available_rings;

	if (!shared_context->vpe_ring) {
		igt_info("VPE no available rings");
		igt_info("VPE fence test disable");
		igt_info("VPE blit test disable");

		return false;
	}

	return true;
}

static void amdgpu_cs_vpe_fence(amdgpu_device_handle device_handle,
				struct mmd_context *context)
{
	const uint32_t test_pattern = 0xdeadbeef;
	uint32_t *ib_cpu = context->ib_cpu;
	struct amdgpu_mmd_bo test_bo;
	int r;

	context->num_resources = 0;
	alloc_resource(device_handle, &test_bo, 4096, AMDGPU_GEM_DOMAIN_GTT);
	context->resources[context->num_resources++] = test_bo.handle;

	r = amdgpu_bo_cpu_map(test_bo.handle, (void **)&test_bo.ptr);
	igt_assert_eq(r, 0);

	memset(test_bo.ptr, 0, 4096);

	memset(ib_cpu, 0, IB_SIZE);

	ib_cpu[0] = 0x5;
	ib_cpu[1] = lower_32_bits(test_bo.addr);
	ib_cpu[2] = upper_32_bits(test_bo.addr);
	ib_cpu[3] = test_pattern;
	ib_cpu[4] = 0x0;
	ib_cpu[5] = 0x0;
	ib_cpu[6] = 0x0;
	ib_cpu[7] = 0x0;

	context->resources[context->num_resources++] = context->ib_handle;

	r = submit(device_handle, context, 8, AMDGPU_HW_IP_VPE);
	igt_assert_eq(r, 0);

	igt_assert_eq(((uint32_t *)test_bo.ptr)[0], test_pattern);

	r = amdgpu_bo_cpu_unmap(test_bo.handle);
	igt_assert_eq(r, 0);

	free_resource(&test_bo);
}

/* a in byte 0, b in byte 1, g in byte 2, r in byte 3 */
static void create_rgba8888(void *addr, uint32_t width, uint32_t height)
{
	uint32_t *ptr = (uint32_t *)addr;

	for (int i = 0; i < height; i++) {
		for (int j = 0; j < width; j++)
			ptr[j] = SRC_PLANE_PATTERN;
		ptr += width;
	}
}

/* a in byte 0, b in byte 1, g in byte 2, r in byte 3 */
static void create_rgba8888_hmirror(void *addr, uint32_t width, uint32_t height)
{
	uint32_t *ptr = (uint32_t *)addr;

	for (int i = 0; i < height; i++) {
		for (int j = 0; j < width / 2; j++)
			ptr[j] = LEFT_PLANE_PATTERN;
		for (int j = width / 2; j < width; j++)
			ptr[j] = RIGHT_PLANE_PATTERN;

		ptr += width;
	}
}

/* b in byte 0, g in byte 1, r in byte 2, a in byte 3 */
static int check_argb8888(void *addr, uint32_t width, uint32_t height)
{
	uint32_t *ptr = (uint32_t *)addr;

	for (int i = 0; i < height; i++) {
		for (int j = 0; j < width; j++)
			if (ptr[j] != DST_PLANE_PATTERN)
				return 1;
		ptr += width;
	}

	return 0;
}

/* a in byte 0, b in byte 1, g in byte 2, r in byte 3 */
static int check_rgba8888_hmirror(void *addr, uint32_t width, uint32_t height)
{
	uint32_t *ptr = (uint32_t *)addr;

	ptr += width * (height / 2);

	if (ptr[width / 4] != RIGHT_PLANE_PATTERN) {
		igt_info("Unexpected left pattern: 0x%x", ptr[width / 4]);
		return 1;
	}

	if (ptr[width * 3 / 4] != LEFT_PLANE_PATTERN) {
		igt_info("Unexpected right pattern: 0x%x", ptr[width * 3 / 4]);
		return 1;
	}

	return 0;
}

static void amdgpu_cs_vpe1_csc(amdgpu_device_handle device_handle,
				struct mmd_context *context)
{
	int r;
	uint32_t frame_size;
	struct amdgpu_mmd_bo vpep_cmd_bo, src_plane_bo, dst_plane_bo;
	/* same height and pitch for input and output */
	const uint32_t height = 256;
	const uint32_t pitch = 1024;
	const uint32_t vpep_config_offsets[] = { 0x34, 0x128, 0x184, 0x1c0, };

	uint32_t *vpec_csc_cmd = vpe1_get_vpec_csc_cmd();
	uint32_t *vpep_csc_cmd = vpe1_get_vpep_csc_cmd();

	frame_size = pitch * height * 4;
	context->num_resources = 0;

	alloc_resource(device_handle, &vpep_cmd_bo, VPE1_VPEP_CSC_CMD_SIZE, AMDGPU_GEM_DOMAIN_GTT);
	alloc_resource(device_handle, &src_plane_bo, frame_size, AMDGPU_GEM_DOMAIN_GTT);
	alloc_resource(device_handle, &dst_plane_bo, frame_size, AMDGPU_GEM_DOMAIN_GTT);

	r = amdgpu_bo_cpu_map(vpep_cmd_bo.handle, (void **)&vpep_cmd_bo.ptr);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_cpu_map(src_plane_bo.handle, (void **)&src_plane_bo.ptr);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_cpu_map(dst_plane_bo.handle, (void **)&dst_plane_bo.ptr);
	igt_assert_eq(r, 0);

	context->resources[context->num_resources++] = vpep_cmd_bo.handle;
	context->resources[context->num_resources++] = src_plane_bo.handle;
	context->resources[context->num_resources++] = dst_plane_bo.handle;

	/* plane config gpu addr */
	*(uint64_t *)(vpec_csc_cmd + 1) = vpep_cmd_bo.addr;
	/* vpep config0 gpu addr */
	*(uint64_t *)(vpec_csc_cmd + 4) = vpep_cmd_bo.addr + vpep_config_offsets[0];
	/* vpep config1 gpu addr */
	*(uint64_t *)(vpec_csc_cmd + 6) = vpep_cmd_bo.addr + vpep_config_offsets[1];
	/* vpep config2 gpu addr */
	*(uint64_t *)(vpec_csc_cmd + 8) = vpep_cmd_bo.addr + vpep_config_offsets[2];
	/* vpep config3 gpu addr */
	*(uint64_t *)(vpec_csc_cmd + 10) = vpep_cmd_bo.addr + vpep_config_offsets[3];

	memset(src_plane_bo.ptr, 0, frame_size);
	memset(dst_plane_bo.ptr, 0, frame_size);
	create_rgba8888(src_plane_bo.ptr, pitch, height);

	/* gpu address of src */
	*(uint64_t *)(vpep_csc_cmd + 2) = src_plane_bo.addr;
	/* gpu address of dst */
	*(uint64_t *)(vpep_csc_cmd + 8) = dst_plane_bo.addr;

	memset(vpep_cmd_bo.ptr, 0, VPE1_VPEP_CSC_CMD_SIZE);
	memcpy(vpep_cmd_bo.ptr, vpep_csc_cmd, VPE1_VPEP_CSC_CMD_SIZE);

	memset(context->ib_cpu, 0, IB_SIZE);
	memcpy(context->ib_cpu, vpec_csc_cmd, VPE1_VPEC_CSC_CMD_SIZE);

	context->resources[context->num_resources++] = context->ib_handle;

	r = submit(device_handle, context, VPE1_VPEC_CSC_CMD_SIZE / 4, AMDGPU_HW_IP_VPE);
	igt_assert_eq(r, 0);

	r = check_argb8888(dst_plane_bo.ptr, pitch, height);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_cpu_unmap(vpep_cmd_bo.handle);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_cpu_unmap(src_plane_bo.handle);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_cpu_unmap(dst_plane_bo.handle);
	igt_assert_eq(r, 0);

	free_resource(&vpep_cmd_bo);
	free_resource(&src_plane_bo);
	free_resource(&dst_plane_bo);
}

static void amdgpu_cs_vpe2_hmirror(amdgpu_device_handle device_handle, struct mmd_context *context)
{
	int i, r;
	uint32_t frame_size;
	struct amdgpu_mmd_bo vpep_cmd_bo, src_plane_bo, dst_plane_bo;
	/* same height and pitch for input and output */
	const uint32_t height = 256;
	const uint32_t pitch = 1024;

	const uint32_t patch_vpec_index[18] = {
	 1,  4,  6,  8, 10, 12, 14, 16,
	18, 20, 22, 24, 26, 28, 30, 32,
	34, 36, };
	const uint32_t patch_vpec_offset[18] = {
	0x0000, 0x0080, 0x00c0, 0x0100, 0x0140, 0x01c0, 0x0740, 0x0b80,
	0x0c40, 0x0cc0, 0x0d40, 0x1280, 0x16c0, 0x1780, 0x1800, 0x18c0,
	0x1900, 0x19c0, };
	const uint32_t patch_src_luma[2] = { 2, 8, };
	const uint32_t patch_dst_luma[2] = { 14, 20, };

	uint32_t *vpec_hmirror_cmd = vpe2_get_vpec_hmirror_cmd();
	uint32_t *vpep_hmirror_cmd = vpe2_get_vpep_hmirror_cmd();

	frame_size = pitch * height * 4;
	context->num_resources = 0;

	/* prepare source image buffer and data */
	alloc_resource(device_handle, &src_plane_bo, frame_size, AMDGPU_GEM_DOMAIN_GTT);
	r = amdgpu_bo_cpu_map(src_plane_bo.handle, (void **)&src_plane_bo.ptr);
	igt_assert_eq(r, 0);
	create_rgba8888_hmirror(src_plane_bo.ptr, pitch, height);
	r = amdgpu_bo_cpu_unmap(src_plane_bo.handle);
	igt_assert_eq(r, 0);

	/* prepare destination image buffer */
	alloc_resource(device_handle, &dst_plane_bo, frame_size, AMDGPU_GEM_DOMAIN_GTT);
	r = amdgpu_bo_cpu_map(dst_plane_bo.handle, (void **)&dst_plane_bo.ptr);
	igt_assert_eq(r, 0);
	memset(dst_plane_bo.ptr, 0, frame_size);
	r = amdgpu_bo_cpu_unmap(dst_plane_bo.handle);
	igt_assert_eq(r, 0);

	/* prepare VPEP command buffer and setup command buffer data */
	alloc_resource(device_handle, &vpep_cmd_bo, VPE2_VPEP_HMIRROR_CMD_SIZE, AMDGPU_GEM_DOMAIN_GTT);
	/* patch gpu addr of vpep cmd */
	for (i = 0; i < sizeof(patch_vpec_index) / sizeof(uint32_t); i++) {
		vpec_hmirror_cmd[patch_vpec_index[i] + 0] = (uint32_t)(vpep_cmd_bo.addr & 0xffffffff) + patch_vpec_offset[i];
		vpec_hmirror_cmd[patch_vpec_index[i] + 1] = (uint32_t)((vpep_cmd_bo.addr & 0xffffffff00000000) >> 32);
	}
	/* patch gpu addr of src/dst plane */
	for (i = 0; i < sizeof(patch_dst_luma) / sizeof(uint32_t); i++) {
		vpep_hmirror_cmd[patch_src_luma[i] + 0] =  (uint32_t)(src_plane_bo.addr & 0xffffffff);
		vpep_hmirror_cmd[patch_src_luma[i] + 1] = (uint32_t)((src_plane_bo.addr & 0xffffffff00000000) >> 32);

		vpep_hmirror_cmd[patch_dst_luma[i] + 0] =  (uint32_t)(dst_plane_bo.addr & 0xffffffff);
		vpep_hmirror_cmd[patch_dst_luma[i] + 1] = (uint32_t)((dst_plane_bo.addr & 0xffffffff00000000) >> 32);
	}
	r = amdgpu_bo_cpu_map(vpep_cmd_bo.handle, (void **)&vpep_cmd_bo.ptr);
	igt_assert_eq(r, 0);
	memset(vpep_cmd_bo.ptr, 0, VPE2_VPEP_HMIRROR_CMD_SIZE);
	memcpy(vpep_cmd_bo.ptr, vpep_hmirror_cmd, VPE2_VPEP_HMIRROR_CMD_SIZE);
	r = amdgpu_bo_cpu_unmap(vpep_cmd_bo.handle);
	igt_assert_eq(r, 0);

	context->resources[context->num_resources++] = context->ib_handle;
	context->resources[context->num_resources++] = vpep_cmd_bo.handle;
	context->resources[context->num_resources++] = src_plane_bo.handle;
	context->resources[context->num_resources++] = dst_plane_bo.handle;

	memset(context->ib_cpu, 0, IB_SIZE);
	memcpy(context->ib_cpu, vpec_hmirror_cmd, VPE2_VPEC_HMIRROR_CMD_SIZE);
	r = submit(device_handle, context, VPE2_VPEC_HMIRROR_CMD_SIZE / 4, AMDGPU_HW_IP_VPE);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_cpu_map(dst_plane_bo.handle, (void **)&dst_plane_bo.ptr);
	igt_assert_eq(r, 0);
	r = check_rgba8888_hmirror(dst_plane_bo.ptr, pitch, height);
	igt_assert_eq(r, 0);
	r = amdgpu_bo_cpu_unmap(dst_plane_bo.handle);
	igt_assert_eq(r, 0);

	free_resource(&vpep_cmd_bo);
	free_resource(&src_plane_bo);
	free_resource(&dst_plane_bo);
}

static void amdgpu_cs_vpe_blit(amdgpu_device_handle device_handle,
				struct mmd_context *context, struct mmd_shared_context *shared_context)
{
	switch (shared_context->vpe_ip_version_major) {
	case 6:
		/* VPE1 has major version as 6 */
		amdgpu_cs_vpe1_csc(device_handle, context);
		break;
	case 2:
		amdgpu_cs_vpe2_hmirror(device_handle, context);
		break;
	default:
		igt_info("Unsupported VPE major version %d in blitting test",
			 shared_context->vpe_ip_version_major);
		igt_assert(false);
		break;
	}
}

int igt_main()
{
	struct mmd_context context = {};
	struct mmd_shared_context shared_context = {};
	amdgpu_device_handle device;
	int fd = -1;

	igt_fixture() {
		uint32_t major, minor;
		int r;

		fd = drm_open_driver(DRIVER_AMDGPU);
		igt_require(fd > 0);

		r = amdgpu_device_initialize(fd, &major, &minor, &device);
		igt_require(r == 0);

		igt_info("Initialized amdgpu, driver version %d.%d\n", major, minor);

		r = mmd_shared_context_init(device, &shared_context);
		igt_require(r == 0);
		r = mmd_context_init(device, &context);
		igt_require(r == 0);

		igt_skip_on(!is_vpe_tests_enabled(device, &shared_context));
	}

	igt_describe("Test VPE fence");
	igt_subtest("vpe-fence-test")
		amdgpu_cs_vpe_fence(device, &context);

	igt_describe("Test VPE blit");
	igt_subtest("vpe-blit-test")
		amdgpu_cs_vpe_blit(device, &context, &shared_context);

	igt_fixture() {
		amdgpu_device_deinitialize(device);
		drm_close_driver(fd);
	}

}
