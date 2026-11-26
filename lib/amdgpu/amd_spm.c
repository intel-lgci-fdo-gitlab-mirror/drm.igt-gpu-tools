// SPDX-License-Identifier: MIT
/*
 * Copyright 2026 Advanced Micro Devices, Inc.
 *
 * Direct CPF SPM capture for IGT. Register selections and packet
 * ordering follow Mesa's ac_spm implementation without linking Mesa.
 */

#include "amd_spm.h"
#include "amd_PM4.h"
#include "amd_memory.h"
#include "amd_ip_blocks.h"
#include "amdgpu_asic_addr.h"
#include "shaders/amd_llvm_asm.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AMD_SPM_RING_SIZE_BYTES (4u * 1024 * 1024)
#define AMD_SPM_CACHE_RING_SIZE_BYTES (32u * 1024 * 1024)
#define AMD_SPM_SAMPLE_SIZE 32
#define AMD_SPM_IB_DWORDS 96

#define AMD_SPM_UCFG(reg) (((reg) - 0x30000) >> 2)
#define AMD_SPM_GRBM_GFX_INDEX AMD_SPM_UCFG(0x030800)
#define AMD_SPM_CPF_SELECT AMD_SPM_UCFG(0x03601c)
#define AMD_SPM_CPF_SELECT1 AMD_SPM_UCFG(0x036018)
#define AMD_SPM_CP_PERFMON AMD_SPM_UCFG(0x036020)
#define AMD_SPM_RLC_PERFMON AMD_SPM_UCFG(0x037200)
#define AMD_SPM_RING_BASE_LO AMD_SPM_UCFG(0x037204)
#define AMD_SPM_RING_BASE_HI AMD_SPM_UCFG(0x037208)
#define AMD_SPM_RING_SIZE_REG AMD_SPM_UCFG(0x03720c)
#define AMD_SPM_RING_WRPTR AMD_SPM_UCFG(0x037210)
#define AMD_SPM_SEGMENT_SIZE AMD_SPM_UCFG(0x03721c)
#define AMD_SPM_GLOBAL_MUXSEL_ADDR AMD_SPM_UCFG(0x037220)
#define AMD_SPM_GLOBAL_MUXSEL_DATA_DW (0x037224 >> 2)
#define AMD_SPM_ACCUM_MODE AMD_SPM_UCFG(0x03726c)

/*
 * GFX10 (Navi1x) uses a different RLC_SPM register layout than GFX11+: the
 * global muxsel addr/data pair is shifted by one register slot, and there is
 * a separate per-SE muxsel addr/data pair (GFX11 only has one pair, since its
 * segment-size register scheme differs too). Values confirmed against
 * drivers/gpu/drm/amd/include/asic_reg/gc/gc_10_1_0_offset.h.
 */
#define AMD_SPM_GFX10_GLOBAL_MUXSEL_ADDR AMD_SPM_UCFG(0x037224)
#define AMD_SPM_GFX10_GLOBAL_MUXSEL_DATA_DW (0x037228 >> 2)
#define AMD_SPM_GFX10_SE_MUXSEL_ADDR AMD_SPM_UCFG(0x03721c)
#define AMD_SPM_GFX10_SE_MUXSEL_DATA_DW (0x037220 >> 2)

/*
 * GFX11's SE muxsel addr/data pair (the global pair is the same register
 * offsets already defined above as AMD_SPM_GLOBAL_MUXSEL_ADDR/DATA_DW,
 * which were already GFX11 values). Also GFX11-only: the SQ/SQG perfcounter
 * control registers that must gate SQ_WGP cache-counter collection on.
 */
#define AMD_SPM_GFX11_SE_MUXSEL_ADDR AMD_SPM_UCFG(0x037228)
#define AMD_SPM_GFX11_SE_MUXSEL_DATA_DW (0x03722c >> 2)
#define AMD_SPM_SQ_PERFCOUNTER_CTRL AMD_SPM_UCFG(0x036780)
#define AMD_SPM_SQG_PERFCOUNTER_CTRL AMD_SPM_UCFG(0x036760)

#define AMD_SPM_GRBM_BROADCAST ((1u << 29) | (1u << 30) | (1u << 31))
/*
 * Mesa's ac_spm_init_grbm_gfx_index() targets AC_PC_GLOBAL_BLOCK counters
 * (CPF included) with SE_BROADCAST_WRITES + SH_BROADCAST_WRITES only --
 * INSTANCE_BROADCAST_WRITES (bit 30) is deliberately left clear and
 * INSTANCE_INDEX stays 0, unlike the muxsel RAM upload (which does use full
 * AMD_SPM_GRBM_BROADCAST, confirmed matching ac_emit_spm_muxsel()). The
 * counter-select registers (CPF_PERFCOUNTER0_SELECT/SELECT1) get their own
 * narrower GRBM_GFX_INDEX write in Mesa, separate from whatever was left
 * over from the muxsel upload.
 */
#define AMD_SPM_GRBM_GLOBAL_BLOCK ((1u << 29) | (1u << 31))
#define AMD_SPM_START (1u << 4)
#define AMD_SPM_STOP (2u << 4)

/* GFX10 SQ_PERFCOUNTERn_SELECT: SQC_BANK_MASK(15) | SPM_MODE(3) (32-bit clamp). */
#define AMD_SPM_GFX10_SQ_SELECT_FLAGS (0xfu << 12 | 0x3u << 20)

#define AMD_SPM_MAX_SE 6
#define AMD_SPM_GLOBAL_SEGMENT AMD_SPM_MAX_SE
#define AMD_SPM_MAX_LINES 32
#define AMD_SPM_MAX_CACHE_COUNTERS 1024
#define AMD_SPM_MAX_CACHE_PROGRAMS 512

static int
emit_uconfig(struct amdgpu_cmd_base *ib, uint32_t reg, uint32_t value,
	     bool reset_filter_cam)
{
	if (ib->cdw > ib->max_dw || ib->max_dw - ib->cdw < 3)
		return -E2BIG;
	ib->emit(ib, PACKET3(PACKET3_SET_UCONFIG_REG, 1) |
		 (reset_filter_cam ? 1u << 2 : 0));
	ib->emit(ib, reg);
	ib->emit(ib, value);
	return 0;
}

static int
emit_windowed_counters(struct amdgpu_cmd_base *ib, bool enable)
{
	if (ib->cdw > ib->max_dw || ib->max_dw - ib->cdw < 5)
		return -E2BIG;
	ib->emit(ib, PACKET3(PACKET3_EVENT_WRITE, 0));
	ib->emit(ib, enable ? 0x17 : 0x18);
	ib->emit(ib, PACKET3(PKT3_SET_SH_REG, 1));
	ib->emit(ib, (0x00b82c - 0x00b000) >> 2);
	ib->emit(ib, enable);
	return 0;
}

/*
 * GFX10 cache-hit-rate counter profile: enabled by amd_spm_capture_enable_caches()
 * below in place of the default single CPF-busy counter. Mirrors the GFX11 cache
 * capture design (muxsel RAM segmented into one global segment plus one segment
 * per shader engine, even/odd-wire-interleaved lines), but using GFX10 register
 * offsets, block-select IDs and perf-event IDs (which differ from GFX11's).
 */
enum spm_cache_metric {
	SPM_CACHE_REQUESTS,
	SPM_CACHE_HITS,
	SPM_CACHE_MISSES,
};

struct spm_cache_counter {
	enum amd_spm_cache cache;
	enum spm_cache_metric metric;
	uint32_t offset;
	uint32_t segment;
};

struct spm_cache_program {
	uint32_t grbm_index;
	uint32_t reg;
	uint32_t values[4];
	uint32_t count;
};

struct amd_spm_cache_profile {
	uint32_t (*pack)(uint32_t block, uint32_t sa, uint32_t instance, uint32_t wire);
	uint16_t mux[AMD_SPM_MAX_SE + 1][AMD_SPM_MAX_LINES][16];
	uint32_t used[AMD_SPM_MAX_SE + 1][2];
	uint32_t global_lines;
	uint32_t se_lines;
	uint32_t num_se;
	uint32_t num_counters;
	uint32_t num_programs;
	struct spm_cache_counter counters[AMD_SPM_MAX_CACHE_COUNTERS];
	struct spm_cache_program programs[AMD_SPM_MAX_CACHE_PROGRAMS];
};

/*
 * GFX10 and GFX11 use different muxsel wire-select bitfield layouts.
 * GFX10: counter bits[5:0] (6 bits), block bits[9:6] (4 bits), shader_array
 * bit[10], instance bits[15:11] (5 bits) -- confirmed against Mesa's
 * union ac_spm_muxsel { struct { counter:6; block:4; shader_array:1;
 * instance:5; } gfx10; ... } in ac_spm.h.
 * GFX11: block bits[15:11] (5 bits), shader_array bit[10], instance
 * bits[9:5] (5 bits), counter/wire bits[4:0] (5 bits) -- the 'gfx11'
 * member of that same union.
 */
static uint32_t
pack_gfx10_mux(uint32_t block, uint32_t sa, uint32_t instance, uint32_t wire)
{
	return (instance << 11) | (sa << 10) | (block << 6) | wire;
}

static uint32_t
pack_gfx11_mux(uint32_t block, uint32_t sa, uint32_t instance, uint32_t wire)
{
	return (block << 11) | (sa << 10) | (instance << 5) | wire;
}

static int
spm_cache_add_counter(struct amd_spm_cache_profile *profile,
		     enum amd_spm_cache cache, enum spm_cache_metric metric,
		     uint32_t segment, uint32_t block, uint32_t sa,
		     uint32_t instance, uint32_t wire)
{
	uint32_t parity = wire & 1;
	uint32_t slot = profile->used[segment][parity]++;
	uint32_t line = 2 * (slot / 16) + parity;
	struct spm_cache_counter *counter;

	if (line >= AMD_SPM_MAX_LINES ||
	    profile->num_counters == AMD_SPM_MAX_CACHE_COUNTERS || instance >= 32)
		return -E2BIG;
	profile->mux[segment][line][slot % 16] = profile->pack(block, sa, instance, wire);
	counter = &profile->counters[profile->num_counters++];
	counter->cache = cache;
	counter->metric = metric;
	counter->segment = segment;
	counter->offset = line * 16 + slot % 16;
	return 0;
}

static int
spm_cache_add_program(struct amd_spm_cache_profile *profile,
		     uint32_t grbm_index, uint32_t reg,
		     const uint32_t *values, uint32_t count)
{
	struct spm_cache_program *program;

	if (profile->num_programs == AMD_SPM_MAX_CACHE_PROGRAMS || count > 4)
		return -E2BIG;
	program = &profile->programs[profile->num_programs++];
	program->grbm_index = grbm_index;
	program->reg = AMD_SPM_UCFG(reg);
	program->count = count;
	memcpy(program->values, values, count * sizeof(*values));
	return 0;
}

static int
spm_cache_add_pair(struct amd_spm_cache_profile *profile,
		   enum amd_spm_cache cache, uint32_t segment,
		   uint32_t block, uint32_t sa, uint32_t instance,
		   uint32_t grbm_index, uint32_t reg,
		   uint32_t requests, uint32_t misses)
{
	uint32_t values[2] = { requests | (misses << 10) | (1u << 20), 0 };
	int ret;

	ret = spm_cache_add_program(profile, grbm_index, reg, values, 2);
	if (!ret)
		ret = spm_cache_add_counter(profile, cache, SPM_CACHE_REQUESTS,
					segment, block, sa, instance, 0);
	if (!ret)
		ret = spm_cache_add_counter(profile, cache, SPM_CACHE_MISSES,
					segment, block, sa, instance, 1);
	return ret;
}

/*
 * Upload capture->cache_profile's whole mux-select table to the hardware.
 * Two parts: (1) for each segment (the global block, then one per active SE),
 * select it via GRBM_GFX_INDEX and write its mux lines 8 dwords at a time
 * through WRITE_DATA packets -- each line picks which 8 internal
 * perfcounter-mux signals land in that line's sample slot; (2) for each
 * block-specific "program" the per-generation cache-profile builder queued
 * (e.g. GFX11's SQ_WGP instruction/scalar-cache counter select), broadcast
 * its register writes the same way. Mirrors emit_muxsel() above, which does
 * the same two-part dance for the non-cache (CPF-busy-only) mux table.
 */
static int
emit_cache_profile(struct amdgpu_cmd_base *ib, const struct amd_spm_capture *capture)
{
	const struct amd_spm_cache_profile *profile = capture->cache_profile;
	bool gfx10 = capture->gfx_major == 10;
	uint32_t global_addr = gfx10 ? AMD_SPM_GFX10_GLOBAL_MUXSEL_ADDR : AMD_SPM_GLOBAL_MUXSEL_ADDR;
	uint32_t global_data = gfx10 ? AMD_SPM_GFX10_GLOBAL_MUXSEL_DATA_DW : AMD_SPM_GLOBAL_MUXSEL_DATA_DW;
	uint32_t se_addr = gfx10 ? AMD_SPM_GFX10_SE_MUXSEL_ADDR : AMD_SPM_GFX11_SE_MUXSEL_ADDR;
	uint32_t se_data = gfx10 ? AMD_SPM_GFX10_SE_MUXSEL_DATA_DW : AMD_SPM_GFX11_SE_MUXSEL_DATA_DW;
	int ret;

	for (uint32_t segment = 0; segment <= AMD_SPM_GLOBAL_SEGMENT; segment++) {
		bool global = segment == AMD_SPM_GLOBAL_SEGMENT;
		uint32_t lines = global ? profile->global_lines : profile->se_lines;
		uint32_t grbm = (1u << 29) | (1u << 31);

		if (!global && segment >= profile->num_se)
			continue;
		grbm |= global ? 1u << 30 : segment << 16;
		ret = emit_uconfig(ib, AMD_SPM_GRBM_GFX_INDEX, grbm, false);
		if (ret)
			return ret;
		for (uint32_t line = 0; line < lines; line++) {
			ret = emit_uconfig(ib, global ? global_addr : se_addr,
					   line * 8, true);
			if (ret)
				return ret;
			if (ib->cdw > ib->max_dw || ib->max_dw - ib->cdw < 12)
				return -E2BIG;
			ib->emit(ib, PACKET3(PACKET3_WRITE_DATA, 10));
			ib->emit(ib, WRITE_DATA_DST_SEL(0) | WR_ONE_ADDR | WR_CONFIRM);
			ib->emit(ib, global ? global_data : se_data);
			ib->emit(ib, 0);
			for (uint32_t index = 0; index < 8; index++)
				ib->emit(ib, ((const uint32_t *)profile->mux[segment][line])[index]);
		}
	}
	for (uint32_t index = 0; index < profile->num_programs; index++) {
		const struct spm_cache_program *program = &profile->programs[index];

		ret = emit_uconfig(ib, AMD_SPM_GRBM_GFX_INDEX, program->grbm_index, false);
		if (ret)
			return ret;
		for (uint32_t value = 0; value < program->count; value++) {
			ret = emit_uconfig(ib, program->reg + value, program->values[value], true);
			if (ret)
				return ret;
		}
	}
	return 0;
}

static int
build_gfx12_layout(struct amd_spm_capture *capture)
{
	capture->profile = (struct amd_spm_profile) {
		.name = "gfx12",
		.timestamp_mux = {0xf841f840, 0xf843f842},
		.counter_mux = 0x00001000,
		.muxsel_addr = AMD_SPM_GLOBAL_MUXSEL_ADDR,
		.muxsel_data = AMD_SPM_GLOBAL_MUXSEL_DATA_DW,
		/*
		 * CPF_PERFCOUNTER0_SELECT: PERF_SEL = event 0x18 (CPF busy),
		 * plus CNTR_MODE0 = 1 ("16-bit clamp") in bits 28-31 --
		 * confirmed both from Mesa's generic-block counter-select
		 * path (ac_spm.c's ac_spm_map_counter(), case index 0: sets
		 * PERF_SEL + CNTR_MODE(1), not SPM_MODE) and from UMR's
		 * register database (gc_12_0_0.reg/gc_11_0_0.reg both show
		 * CNTR_MODE0 at bits 28-31, distinct from the bits-20-23
		 * SPM_MODE field this previously, incorrectly, set).
		 */
		.cpf_select = 0x18u | (1u << 28),
		.pointer_granularity = 1,
		/*
		 * Mesa unconditionally skips a reserved 32-byte header at
		 * the start of the ring for every generation (not just
		 * GFX11) -- confirmed in ac_spm.c's derived-trace reader,
		 * no gfx_level check around it. 4 here was wrong.
		 */
		.data_offset = 32,
		/*
		 * Mesa's ac_spm.c emits the windowed-counter arm/disarm
		 * sequence (COMPUTE_PERFCOUNT_ENABLE + PERFCOUNTER_START/STOP)
		 * unconditionally for every generation GFX10+, GFX12
		 * included -- there is no legacy/non-windowed variant in real
		 * hardware programming. Must be true so gfx12_ops uses
		 * emit_windowed_start/emit_windowed_stop_reset and
		 * amd_spm_capture_init() reserves a VMID, matching GFX10/11.
		 */
		.windowed_counters = true,
	};
	return 0;
}

static int
build_gfx11_layout(struct amd_spm_capture *capture)
{
	int ret = build_gfx12_layout(capture);

	if (ret)
		return ret;
	capture->profile.name = capture->gfx_minor == 0 ? "gfx11" :
				capture->gfx_minor == 5 ? "gfx11.5" : "gfx11.7";
	capture->profile.cpf_select = 0x18u | (1u << 20);
	capture->profile.pointer_granularity = 32;
	capture->profile.data_offset = 32;
	capture->profile.windowed_counters = true;
	return 0;
}

static int
build_gfx10_layout(struct amd_spm_capture *capture)
{
	int ret = build_gfx11_layout(capture);

	if (ret)
		return ret;
	capture->profile.name = capture->gfx_minor == 1 ? "gfx10" : "gfx10.3";
	capture->profile.timestamp_mux[0] = 0xf0f0f0f0;
	capture->profile.timestamp_mux[1] = 0xf0f0f0f0;
	capture->profile.counter_mux = 0x00000080;
	capture->profile.muxsel_addr = AMD_SPM_UCFG(0x037224);
	capture->profile.muxsel_data = 0x037228 >> 2;
	capture->profile.pointer_granularity = 1;
	return 0;
}

static int
emit_muxsel(struct amdgpu_cmd_base *ib, const struct amd_spm_capture *capture)
{
	const uint32_t line[8] = {
		capture->profile.timestamp_mux[0],
		capture->profile.timestamp_mux[1],
		capture->profile.counter_mux, 0, 0, 0, 0, 0,
	};
	int ret;

	if (capture->cache_profile)
		return emit_cache_profile(ib, capture);

	ret = emit_uconfig(ib, AMD_SPM_GRBM_GFX_INDEX,
			   AMD_SPM_GRBM_BROADCAST, false);
	if (ret)
		return ret;
	ret = emit_uconfig(ib, capture->profile.muxsel_addr, 0, true);
	if (ret)
		return ret;
	if (ib->cdw > ib->max_dw || ib->max_dw - ib->cdw < 12)
		return -E2BIG;
	ib->emit(ib, PACKET3(PACKET3_WRITE_DATA, 10));
	ib->emit(ib, WRITE_DATA_DST_SEL(0) | WR_ONE_ADDR | WR_CONFIRM);
	ib->emit(ib, capture->profile.muxsel_data);
	ib->emit(ib, 0);
	for (uint32_t index = 0; index < ARRAY_SIZE(line); index++)
		ib->emit(ib, line[index]);
	return 0;
}

static int
emit_setup_common(struct amdgpu_cmd_base *ib,
		  const struct amd_spm_capture *capture)
{
	int ret;

	if (capture->ring_size % AMD_SPM_SAMPLE_SIZE ||
	    capture->ring_gpu_va % AMD_SPM_SAMPLE_SIZE)
		return -EINVAL;
	ret = emit_uconfig(ib, AMD_SPM_CP_PERFMON, 0, false);
	if (ret)
		return ret;
	ret = emit_uconfig(ib, AMD_SPM_GRBM_GFX_INDEX,
			   AMD_SPM_GRBM_BROADCAST, false);
	if (ret)
		return ret;
	ret = emit_uconfig(ib, AMD_SPM_RLC_PERFMON, 4096u << 16, false);
	if (ret)
		return ret;
	ret = emit_uconfig(ib, AMD_SPM_RING_BASE_LO,
			   (uint32_t)capture->ring_gpu_va, false);
	if (ret)
		return ret;
	ret = emit_uconfig(ib, AMD_SPM_RING_BASE_HI,
			   (uint32_t)(capture->ring_gpu_va >> 32), false);
	if (ret)
		return ret;
	return emit_uconfig(ib, AMD_SPM_RING_SIZE_REG, capture->ring_size, false);
}

static int
emit_setup_counters(struct amdgpu_cmd_base *ib,
		    const struct amd_spm_capture *capture)
{
	int ret;

	ret = emit_uconfig(ib, AMD_SPM_ACCUM_MODE, 0, false);
	if (ret)
		return ret;
	ret = emit_muxsel(ib, capture);
	if (ret)
		return ret;
	ret = emit_uconfig(ib, AMD_SPM_GRBM_GFX_INDEX,
			   AMD_SPM_GRBM_GLOBAL_BLOCK, false);
	if (ret)
		return ret;
	if (capture->profile.windowed_counters) {
		ret = emit_uconfig(ib, AMD_SPM_CPF_SELECT1, 0, true);
		if (ret)
			return ret;
	}
	return emit_uconfig(ib, AMD_SPM_CPF_SELECT, capture->profile.cpf_select, true);
}

static int
emit_gfx10_setup(struct amdgpu_cmd_base *ib,
		 const struct amd_spm_capture *capture)
{
	const struct amd_spm_cache_profile *profile = capture->cache_profile;
	uint32_t se3to0_segment_size = 0;
	uint32_t glb_segment_size = 0x00000101;
	int ret;

	ret = emit_setup_common(ib, capture);
	if (ret)
		return ret;
	ret = emit_uconfig(ib, AMD_SPM_UCFG(0x037210), 0, false);
	if (ret)
		return ret;
	if (profile) {
		uint32_t total_lines = profile->global_lines +
				       profile->num_se * profile->se_lines;

		for (uint32_t se = 0; se < profile->num_se && se < 4; se++)
			se3to0_segment_size |= (profile->se_lines & 0xff) << (8 * se);
		glb_segment_size = (total_lines & 0xff) |
				   ((profile->global_lines & 0xff) << 8);
	}
	ret = emit_uconfig(ib, AMD_SPM_UCFG(0x03727c), se3to0_segment_size, false);
	if (ret)
		return ret;
	ret = emit_uconfig(ib, AMD_SPM_UCFG(0x037280), glb_segment_size, false);
	if (ret)
		return ret;
	return emit_setup_counters(ib, capture);
}

static int
emit_gfx11_setup(struct amdgpu_cmd_base *ib,
		 const struct amd_spm_capture *capture)
{
	const struct amd_spm_cache_profile *profile = capture->cache_profile;
	uint32_t segment_size = 0x00010001;
	int ret;

	if (profile) {
		/* Gate SQ_WGP instruction/scalar cache-counter collection on. */
		ret = emit_uconfig(ib, AMD_SPM_SQ_PERFCOUNTER_CTRL, 0x55, false);
		if (ret)
			return ret;
		ret = emit_uconfig(ib, AMD_SPM_SQG_PERFCOUNTER_CTRL, 0x55, false);
		if (ret)
			return ret;
	}
	ret = emit_setup_common(ib, capture);
	if (ret)
		return ret;
	ret = emit_uconfig(ib, AMD_SPM_RING_WRPTR, 0, false);
	if (ret)
		return ret;
	if (profile)
		segment_size = (capture->sample_size / 32) |
			       (profile->global_lines << 16) |
			       (profile->se_lines << 24);
	ret = emit_uconfig(ib, AMD_SPM_SEGMENT_SIZE, segment_size, false);
	if (ret)
		return ret;
	return emit_setup_counters(ib, capture);
}

static int
emit_legacy_start(struct amdgpu_cmd_base *ib,
		  const struct amd_spm_capture *capture)
{
	return emit_uconfig(ib, AMD_SPM_CP_PERFMON, AMD_SPM_START, false);
}

static int
emit_windowed_start(struct amdgpu_cmd_base *ib,
		    const struct amd_spm_capture *capture)
{
	int ret = emit_legacy_start(ib, capture);

	if (ret)
		return ret;
	return emit_windowed_counters(ib, true);
}

static int
emit_legacy_stop_reset(struct amdgpu_cmd_base *ib,
		       const struct amd_spm_capture *capture)
{
	int ret;

	ret = emit_uconfig(ib, AMD_SPM_CP_PERFMON, AMD_SPM_STOP, false);
	if (ret)
		return ret;
	return emit_uconfig(ib, AMD_SPM_CP_PERFMON, 0, false);
}

static int
emit_windowed_stop_reset(struct amdgpu_cmd_base *ib,
			 const struct amd_spm_capture *capture)
{
	int ret = emit_windowed_counters(ib, false);

	if (ret)
		return ret;
	return emit_legacy_stop_reset(ib, capture);
}

/*
 * GFX11/GFX10 cache working-set/access-pattern/scalar/instruction benchmark
 * workload: per-generation shader bytes (build_cache_bench_shader hook) and
 * the PM4 that binds the source/destination buffers to it
 * (emit_cache_bench_descriptors hook). Used by
 * amd_spm_capture_build_cache_bench_shader() /
 * amd_spm_capture_emit_cache_bench_descriptors() below, which
 * tests/amdgpu/amd_spm.c's spm-cache-benchmark dynamic subtest calls instead
 * of ever branching on gfx_major/gfx_minor or building PM4 packets itself.
 */

/*
 * Hand-encoded RDNA3 (gfx1100) machine code, decoded below via
 * `llvm-mc --disassemble --triple=amdgcn--amdhsa --mcpu=gfx1100`. All three
 * shaders share the same prologue/epilogue and ABI (see the big comment
 * above gfx10_vector_isa_fmt further down for the full register ABI); only
 * the inner loop body differs. Index numbers are array/word indices (0-based)
 * into cache_vector_shader[], matching the [N] annotations
 * build_gfx11_cache_bench_shader() uses to patch literals in at runtime.
 *
 * [0]      s_version UC_VERSION_GFX11|UC_VERSION_W64_BIT   -- mandatory header word
 * [1]      v_and_b32_e32  v0, 0x3ff, v0       -- v0 = tid.x & 0x3ff (clamp/sanitize thread ID)
 * [2]        (0x3ff literal operand for [1])
 * [3,4]    v_lshl_add_u32 v7, s8, 6, v0       -- v7 = (wg.x << 6) + tid.x = global work-item index
 * [5]      v_mov_b32_e32  v6, 0              -- v6 = running sum (this work-item's result)
 * [6]      s_mov_b32      s9, <iterations>   -- s9 = loop trip count           [patched: shader[7]]
 * [7]        (iterations literal for [6])
 * LOOP:
 * [8]      v_and_b32_e32  v1, <mask>, v7     -- v1 = v7 & mask: wraps the work-item
 *                                                index into the configured working set
 *                                                (mask = working_set/16 - 1, so working_set
 *                                                must be a power of two) [patched: shader[9]]
 * [9]        (mask literal for [8])
 * [10,11]  buffer_load_format_xyzw v[2:5], v1, s[0:3], 0 idxen
 *                                            -- load one dwordx4 "cache line" from the
 *                                                source buffer at index v1 -- this is the
 *                                                actual vector-cache access under measurement
 * [12]     s_waitcnt      vmcnt(0)           -- wait for the load to complete
 * [13]     v_add_nc_u32_e32 v6, v6, v2       -- v6 += loaded value (keeps the load live,
 *                                                not dead code)
 * [14]     v_add_nc_u32_e32 v1, <step>, v1   -- v1 += step: stride to the next address
 *                                                                    [patched: shader[15]]
 * [15]       (step literal for [14])
 * [16]     v_and_b32_e32  v1, <mask>, v1     -- re-wrap the strided address back into
 *                                                the working set
 *                                                            [patched: shader[17] == shader[9]]
 * [17]       (mask literal for [16], same value as [9])
 * [18]     s_sub_u32      s9, s9, 1          -- s9 -= 1
 * [19]     s_cmp_lg_u32   s9, 0              -- s9 != 0 ?
 * [20]     s_cbranch_scc1 LOOP               -- loop while s9 != 0 (fixed -11 dword
 *                                                offset, valid because this loop body
 *                                                is always 11 words long)
 * [21-24]  v_mov_b32_e32  v2/v3/v4/v5, v6    -- replicate the accumulated sum across a dwordx4
 * [25,26]  buffer_store_format_xyzw v[2:5], v7, s[4:7], 0 idxen
 *                                            -- store it to dst[work-item index]
 * [27]     s_waitcnt      vmcnt(0)
 * [28]     s_endpgm
 *
 * Net effect: each work-item repeatedly re-reads (iterations times) from a
 * power-of-two-sized, address-wrapped window of the source buffer -- a
 * vector L0/L1/L2 cache working-set/stride benchmark.
 *
 * Equivalent C, one work-item (one lane):
 *
 *   uint32_t tid = get_local_id(0) & 0x3ff;
 *   uint32_t gid = get_group_id(0) * 64 + tid;       // v7, also the dst index
 *   uint32_t mask = working_set / 16 - 1;            // working_set must be a power of two
 *   uint32_t addr = gid & mask;                      // v1
 *   uint32_t sum = 0;                                // v6
 *
 *   for (uint32_t i = 0; i < iterations; i++) {
 *       uint4 line = src_buffer[addr];                // the vector-cache load under measurement
 *       sum += line.x;
 *       addr = (addr + step) & mask;
 *   }
 *   dst_buffer[gid] = (uint4){ sum, sum, sum, sum };
 */
static const uint32_t cache_vector_shader[] = {
	0xb0802006, 0x360000ff, 0x000003ff, 0xd6460007, 0x04010c08,
	0x7e0c0280, 0xbe8900ff, 0x13572468, 0x36020eff, 0x24681357,
	0xe00c0000, 0x80800201, 0xbf8903f7, 0x4a0c0506, 0x4a0202ff,
	0x35792468, 0x360202ff, 0x24681357, 0x80898109, 0xbf078009,
	0xbfa2fff5, 0x7e040306, 0x7e060306, 0x7e080306, 0x7e0a0306,
	0xe01c0000, 0x80810207, 0xbf8903f7, 0xbfb00000,
};

/*
 * Same shape as cache_vector_shader above, but the per-iteration address is
 * computed and loaded in SCALAR registers (uniform across the whole wave)
 * instead of per-thread VGPRs, which routes the load through the
 * scalar-data cache instead of the vector L0/L1/L2 caches:
 *
 * [0]      s_version UC_VERSION_GFX11|UC_VERSION_W64_BIT
 * [1,2]    v_and_b32_e32  v0, 0x3ff, v0
 * [3,4]    v_lshl_add_u32 v7, s8, 6, v0       -- still needed for the final dst store index
 * [5]      s_lshl_b32     s10, s8, 4          -- s10 = wg.x << 4 (uniform per-wave
 *                                                address seed; deliberately ignores tid.x --
 *                                                same scalar load for every lane in the wave,
 *                                                which is what makes it "scalar")
 * [6,7]    s_mov_b32      s9, <iterations>                           [patched: shader[7]]
 * [8]      s_mov_b32      s11, 0              -- s11 = running sum
 * LOOP:
 * [9,10]   s_and_b32      s10, s10, <mask>    -- mask = working_set - 1 (byte-granular,
 *                                                no /16 here since this is a direct scalar
 *                                                dword address, not a dwordx4 buffer-format
 *                                                index)                [patched: shader[10]]
 * [11,12]  s_buffer_load_b32 s12, s[0:3], s10 -- the scalar-cache load under measurement
 * [13]     s_waitcnt      lgkmcnt(0)
 * [14]     s_add_u32      s11, s11, s12       -- accumulate the loaded value
 * [15,16]  s_add_u32      s10, s10, <step>                           [patched: shader[16]]
 * [17,18]  s_and_b32      s10, s10, <mask>    -- re-wrap, same mask as [9,10]
 *                                                       [patched: shader[18] == shader[10]]
 * [19]     s_sub_u32      s9, s9, 1
 * [20]     s_cmp_lg_u32   s9, 0
 * [21]     s_cbranch_scc1 LOOP                -- fixed -11 dword offset, same reasoning
 *                                                as the vector shader
 * [22-25]  v_mov_b32_e32  v2/v3/v4/v5, s11    -- broadcast the scalar sum into the
 *                                                vector dst payload
 * [26,27]  buffer_store_format_xyzw v[2:5], v7, s[4:7], 0 idxen
 * [28]     s_waitcnt      vmcnt(0)
 * [29]     s_endpgm
 *
 * Equivalent C, one wave (addr/sum are computed ONCE per wave, not per lane --
 * every lane in the wave reads the exact same address, which is what makes
 * this a scalar- instead of vector-cache test):
 *
 *   uint32_t tid = get_local_id(0) & 0x3ff;
 *   uint32_t gid = get_group_id(0) * 64 + tid;       // only used for the dst index
 *   uint32_t mask = working_set - 1;                 // byte-granular, must be power of two
 *   uint32_t addr = (get_group_id(0) << 4) & mask;   // wave-uniform, ignores tid entirely
 *   uint32_t sum = 0;
 *
 *   for (uint32_t i = 0; i < iterations; i++) {
 *       uint32_t value = *(const uint32_t *)((const char *)src_buffer + addr);
 *       // ^ the scalar-cache load
 *       sum += value;
 *       addr = (addr + step) & mask;
 *   }
 *   dst_buffer[gid] = (uint4){ sum, sum, sum, sum };
 */
static const uint32_t cache_scalar_shader[] = {
	0xb0802006, 0x360000ff, 0x000003ff, 0xd6460007, 0x04010c08,
	0x840a8408, 0xbe8900ff, 0x13572468, 0xbe8b0080, 0x8b0aff0a,
	0x24681357, 0xf4200300, 0x14000000, 0xbf89fc07, 0x800b0c0b,
	0x800aff0a, 0x35792468, 0x8b0aff0a, 0x24681357, 0x80898109,
	0xbf078009, 0xbfa2fff5, 0x7e04020b, 0x7e06020b, 0x7e08020b,
	0x7e0a020b, 0xe01c0000, 0x80810207, 0xbf8903f7, 0xbfb00000,
};

/*
 * Unlike the other two shaders, this array is only a *template*: it does no
 * memory access at all, and its inner loop body is exactly one filler
 * instruction long (word [8]). build_gfx11_cache_bench_shader() builds the
 * real shader by repeating word [8] test->instructions times -- stressing
 * the SQ instruction cache in proportion to code size, which is the whole
 * point of this benchmark -- then re-appending the loop-control tail
 * ([9],[10]) and a freshly computed branch-back offset (word [11]'s -4 only
 * happens to be correct for exactly one filler instruction; see below).
 *
 * [0]      s_version UC_VERSION_GFX11|UC_VERSION_W64_BIT
 * [1,2]    v_and_b32_e32  v0, 0x3ff, v0
 * [3,4]    v_lshl_add_u32 v7, s8, 6, v0
 * [5]      v_mov_b32_e32  v6, 0               -- v6 = running sum
 * [6,7]    s_mov_b32      s9, <iterations>                                    [patched: shader[7]]
 * LOOP (this word repeated test->instructions times at runtime):
 * [8]      v_add_nc_u32_e32 v6, 1, v6         -- the "filler": one cheap instruction
 *                                                per repetition, purely to occupy
 *                                                instruction-cache lines; v6's value doesn't
 *                                                matter beyond matching the iterations *
 *                                                instructions check in run_cache_benchmark()
 * [9]      s_sub_u32      s9, s9, 1           -- appended once after the repeated fillers
 * [10]     s_cmp_lg_u32   s9, 0               -- appended once after
 * [11]     s_cbranch_scc1 65532 (-4)          -- static placeholder only valid when
 *                                                instructions == 1; build_gfx11_cache_bench_shader()
 *                                                never copies this word -- it always
 *                                                recomputes 0xbfa20000 |
 *                                                (uint16_t)-(instructions+3) instead, so the
 *                                                branch correctly skips back over however many
 *                                                filler instructions were actually emitted
 * [12-15]  v_mov_b32_e32  v2/v3/v4/v5, v6
 * [16,17]  buffer_store_format_xyzw v[2:5], v7, s[4:7], 0 idxen
 * [18]     s_waitcnt      vmcnt(0)
 * [19]     s_endpgm
 *
 * Equivalent C, one work-item. The inner "instructions" loop is NOT a real
 * loop in the actual shader -- build_gfx11_cache_bench_shader() fully
 * unrolls it by emitting word [8] "instructions" times back to back, so code
 * size (and hence instruction-cache footprint) scales directly with
 * "instructions":
 *
 *   uint32_t tid = get_local_id(0) & 0x3ff;
 *   uint32_t gid = get_group_id(0) * 64 + tid;
 *   uint32_t sum = 0;
 *
 *   for (uint32_t i = 0; i < iterations; i++) {
 *       for (uint32_t j = 0; j < instructions; j++)  // unrolled at build time, not a real loop
 *           sum += 1;                                 // one v_add_nc_u32 per "instructions"
 *   }
 *   dst_buffer[gid] = (uint4){ sum, sum, sum, sum };  // expected = iterations * instructions
 */
static const uint32_t cache_instruction_shader[] = {
	0xb0802006, 0x360000ff, 0x000003ff, 0xd6460007, 0x04010c08,
	0x7e0c0280, 0xbe8900ff, 0x13572468, 0x4a0c0c81, 0x80898109,
	0xbf078009, 0xbfa2fffc, 0x7e040306, 0x7e060306, 0x7e080306,
	0x7e0a0306, 0xe01c0000, 0x80810207, 0xbf8903f7, 0xbfb00000,
};

/*
 * Select the hand-coded GFX11 machine code matching @test->kind (see the
 * three shader arrays and their decode comments above), patch the
 * case-specific literals (iterations/mask/step, or the unrolled filler-
 * instruction count) into the documented [N] word offsets, then pad past
 * s_endpgm with s_code_end so the instruction prefetcher never reads past
 * the allocated shader buffer. @dst_size/@gfx_minor are unused here -- GFX11
 * hand-codes one fixed binary per case regardless of minor version or buffer
 * size (the caller is trusted to have sized the buffer; see
 * cache_benchmark_shader()'s original caller for why).
 */
static uint32_t
build_gfx11_cache_bench_shader(void *dst, size_t dst_size,
			       const struct amd_spm_cache_bench_case *test,
			       uint32_t gfx_minor)
{
	uint32_t *shader = dst;
	uint32_t words;

	(void)dst_size;
	(void)gfx_minor;

	if (test->kind == AMD_SPM_CACHE_BENCH_VECTOR) {
		memcpy(shader, cache_vector_shader, sizeof(cache_vector_shader));
		shader[7] = test->iterations;
		shader[9] = test->working_set / 16 - 1;
		shader[15] = test->step;
		shader[17] = shader[9];
		words = ARRAY_SIZE(cache_vector_shader);
	} else if (test->kind == AMD_SPM_CACHE_BENCH_SCALAR) {
		memcpy(shader, cache_scalar_shader, sizeof(cache_scalar_shader));
		shader[7] = test->iterations;
		shader[10] = test->working_set - 1;
		shader[16] = test->step;
		shader[18] = shader[10];
		words = ARRAY_SIZE(cache_scalar_shader);
	} else {
		memcpy(shader, cache_instruction_shader, 8 * sizeof(*shader));
		shader[7] = test->iterations;
		words = 8;
		for (uint32_t index = 0; index < test->instructions; index++)
			shader[words++] = cache_instruction_shader[8];
		shader[words++] = cache_instruction_shader[9];
		shader[words++] = cache_instruction_shader[10];
		shader[words++] = 0xbfa20000 | ((uint16_t)(-(int32_t)(test->instructions + 3)));
		memcpy(&shader[words], &cache_instruction_shader[12], 8 * sizeof(*shader));
		words += 8;
	}
	/*
	 * 0xbf9f0000 = s_code_end. Pads past s_endpgm so the instruction
	 * prefetcher never walks off the end of the allocated shader buffer.
	 */
	for (uint32_t padding = 0; padding < 64; padding++)
		shader[words++] = 0xbf9f0000;
	return words * sizeof(*shader);
}

/*
 * GFX10.1 (Navi10) equivalents of cache_vector_shader/cache_scalar_shader/
 * cache_instruction_shader above. The GFX11 shaders are hand-encoded RDNA3
 * machine code and will not run correctly (or safely) on GFX10 hardware.
 * Rather than hand-encoding RDNA1 machine code the same fragile way, these
 * are portable AMDGCN assembly *text*, assembled at test run time for the
 * exact detected ASIC via llvm-mc (lib/amdgpu/shaders/amd_llvm_asm.c), the
 * same mechanism lib/amdgpu/compute_utils/amd_dispatch_helpers.c already
 * uses for CopyDwordIsa/amdgpu_compute_copy_dword().
 *
 * Unlike the GFX11 shaders, these use flat addressing (vector test) and an
 * SMEM s_load_dword (scalar test) instead of buffer-descriptor (V#)
 * addressing -- SPM's L0/L1/L2 counters measure the cache hardware itself
 * regardless of which instruction family issued the request, so there is no
 * correctness reason to replicate the GFX11 shaders' descriptor setup, and
 * skipping it avoids a whole class of GFX-version-specific buffer-descriptor
 * format-tag risk.
 *
 * Register ABI (deliberately mirrors amdgpu_dispatch_write2hw()'s existing
 * gfx9/gfx10 resource table -- RSRC1=0x000C0041, RSRC2=0x00000090, which
 * declares 8 user SGPRs, so TGID_X lands in s8, not s4):
 *   s[0:1] = source buffer VA (filled with 1u32 by the test)
 *   s[2:3] = destination buffer VA (one 16-byte slot per work-item)
 *   s8     = workgroup ID X (hardware-provided, since 8 user SGPRs declared)
 *   v0     = local thread ID X (hardware-provided)
 * Every kernel accumulates a per-work-item iteration counter and stores it
 * (replicated across a dwordx4) to dst[(wg.x*64+tid.x)*16], matching the
 * expected = iterations * (instructions, for the instruction kind)
 * correctness check tests/amdgpu/amd_spm.c's run_cache_benchmark() performs.
 */
static const char gfx10_vector_isa_fmt[] =
	".text\n"
	"\tv_lshlrev_b32 v1, 4, v0\n"
	"\tv_and_b32 v1, %#x, v1\n"
	"\tv_mov_b32 v2, 0\n"
	"\ts_mov_b32 s6, %u\n"
	"LOOP_VEC:\n"
	"\tv_add_co_u32 v4, vcc_lo, s0, v1\n"
	"\tv_mov_b32 v5, s1\n"
	"\tv_add_co_ci_u32 v5, vcc_lo, v5, 0, vcc_lo\n"
	"\tflat_load_dword v6, v[4:5] glc slc\n"
	"\ts_waitcnt 0\n"
	"\tv_add_nc_u32 v2, v2, 1\n"
	"\tv_add_nc_u32 v1, v1, %u\n"
	"\tv_and_b32 v1, %#x, v1\n"
	"\ts_sub_u32 s6, s6, 1\n"
	"\ts_cmp_lg_u32 s6, 0\n"
	"\ts_cbranch_scc1 LOOP_VEC\n"
	"\tv_mov_b32 v1, s8\n"
	"\tv_lshlrev_b32 v1, 6, v1\n"
	"\tv_add_nc_u32 v1, v1, v0\n"
	"\tv_lshlrev_b32 v1, 4, v1\n"
	"\tv_add_co_u32 v6, vcc_lo, s2, v1\n"
	"\tv_mov_b32 v7, s3\n"
	"\tv_add_co_ci_u32 v7, vcc_lo, v7, 0, vcc_lo\n"
	"\tv_mov_b32 v3, v2\n"
	"\tv_mov_b32 v4, v2\n"
	"\tv_mov_b32 v5, v2\n"
	"\tflat_store_dwordx4 v[6:7], v[2:5] glc slc\n"
	"\ts_endpgm\n";

static const char gfx10_scalar_isa_fmt[] =
	".text\n"
	"\ts_mov_b32 s6, 0\n"
	"\ts_mov_b32 s7, 0\n"
	"\ts_mov_b32 s11, %u\n"
	"LOOP_SCALAR:\n"
	"\ts_load_dword s9, s[0:1], s6 glc\n"
	"\ts_waitcnt lgkmcnt(0)\n"
	"\ts_add_i32 s7, s7, 1\n"
	"\ts_add_i32 s6, s6, %u\n"
	"\ts_and_b32 s6, s6, %#x\n"
	"\ts_sub_u32 s11, s11, 1\n"
	"\ts_cmp_lg_u32 s11, 0\n"
	"\ts_cbranch_scc1 LOOP_SCALAR\n"
	"\tv_mov_b32 v1, s8\n"
	"\tv_lshlrev_b32 v1, 6, v1\n"
	"\tv_add_nc_u32 v1, v1, v0\n"
	"\tv_lshlrev_b32 v1, 4, v1\n"
	"\tv_add_co_u32 v6, vcc_lo, s2, v1\n"
	"\tv_mov_b32 v7, s3\n"
	"\tv_add_co_ci_u32 v7, vcc_lo, v7, 0, vcc_lo\n"
	"\tv_mov_b32 v2, s7\n"
	"\tv_mov_b32 v3, s7\n"
	"\tv_mov_b32 v4, s7\n"
	"\tv_mov_b32 v5, s7\n"
	"\tflat_store_dwordx4 v[6:7], v[2:5] glc slc\n"
	"\ts_endpgm\n";

/*
 * instruction-large inflates this to 16384 repeated body instructions, which
 * exceeds s_cbranch's +/-simm16 relative-branch range (confirmed empirically:
 * llvm-mc rejects it with "branch size exceeds simm16"). Loop back via an
 * absolute jump instead (s_getpc_b64 once to capture the body's address,
 * s_setpc_b64 to return to it each iteration), which has no range limit. The
 * body instruction uses a register operand (v2, preloaded with 1) rather
 * than an immediate: v_add_nc_u32 with an immediate forces the 8-byte VOP3
 * encoding, which would double the code footprint and overflow the
 * 131072-byte shader buffer at 16384 repeats (confirmed: 16384 x 8 = 131072
 * body bytes alone, before prologue/epilogue); the register-operand form is
 * the compact 4-byte VOP2 encoding, halving it back to a safe size.
 */
static const char gfx10_instruction_isa_prologue_fmt[] =
	".text\n"
	"\tv_mov_b32 v1, 0\n"
	"\tv_mov_b32 v2, 1\n"
	"\ts_mov_b32 s6, %u\n"
	"LOOP_START:\n"
	"\ts_getpc_b64 s[10:11]\n"
	"LOOP_INSTR:\n";

static const char gfx10_instruction_isa_body[] =
	"\tv_add_nc_u32 v1, v1, v2\n";

static const char gfx10_instruction_isa_epilogue[] =
	"\ts_sub_u32 s6, s6, 1\n"
	"\ts_cmp_lg_u32 s6, 0\n"
	"\ts_cbranch_scc0 LOOP_END\n"
	"\ts_setpc_b64 s[10:11]\n"
	"LOOP_END:\n"
	"\tv_mov_b32 v5, s8\n"
	"\tv_lshlrev_b32 v5, 6, v5\n"
	"\tv_add_nc_u32 v5, v5, v0\n"
	"\tv_lshlrev_b32 v5, 4, v5\n"
	"\tv_add_co_u32 v6, vcc_lo, s2, v5\n"
	"\tv_mov_b32 v7, s3\n"
	"\tv_add_co_ci_u32 v7, vcc_lo, v7, 0, vcc_lo\n"
	"\tv_mov_b32 v2, v1\n"
	"\tv_mov_b32 v3, v1\n"
	"\tv_mov_b32 v4, v1\n"
	"\tflat_store_dwordx4 v[6:7], v[1:4] glc slc\n"
	"\ts_endpgm\n";

/*
 * Build the GFX10 ISA text for @test into a freshly malloc'd buffer (caller
 * frees with free()). Returns NULL on allocation failure.
 */
static char *
build_gfx10_cache_bench_isa_text(const struct amd_spm_cache_bench_case *test)
{
	uint32_t mask;
	char *text;

	if (test->kind == AMD_SPM_CACHE_BENCH_VECTOR) {
		mask = test->working_set / 16 - 1;
		text = malloc(sizeof(gfx10_vector_isa_fmt) + 64);
		if (!text)
			return NULL;
		sprintf(text, gfx10_vector_isa_fmt, mask, test->iterations,
			test->step, mask);
		return text;
	}
	if (test->kind == AMD_SPM_CACHE_BENCH_SCALAR) {
		mask = test->working_set - 1;
		text = malloc(sizeof(gfx10_scalar_isa_fmt) + 64);
		if (!text)
			return NULL;
		sprintf(text, gfx10_scalar_isa_fmt, test->iterations,
			test->step, mask);
		return text;
	}

	/* AMD_SPM_CACHE_BENCH_INSTRUCTION: prologue + test->instructions repeated
	 * add instructions (code-size inflation to stress the icache) +
	 * epilogue.
	 */
	{
		size_t prologue_len, body_len, epilogue_len, total;

		text = malloc(sizeof(gfx10_instruction_isa_prologue_fmt) + 32);
		if (!text)
			return NULL;
		sprintf(text, gfx10_instruction_isa_prologue_fmt, test->iterations);
		prologue_len = strlen(text);
		free(text);

		body_len = strlen(gfx10_instruction_isa_body);
		epilogue_len = strlen(gfx10_instruction_isa_epilogue);
		total = prologue_len + (size_t)test->instructions * body_len + epilogue_len + 1;
		text = malloc(total);
		if (!text)
			return NULL;
		sprintf(text, gfx10_instruction_isa_prologue_fmt, test->iterations);
		for (uint32_t index = 0; index < test->instructions; index++)
			memcpy(text + prologue_len + (size_t)index * body_len,
			       gfx10_instruction_isa_body, body_len);
		memcpy(text + prologue_len + (size_t)test->instructions * body_len,
		       gfx10_instruction_isa_epilogue, epilogue_len + 1);
		return text;
	}
}

static uint32_t
build_gfx10_cache_bench_shader(void *dst, size_t dst_size,
			       const struct amd_spm_cache_bench_case *test,
			       uint32_t gfx_minor)
{
	char mcpu[32];
	char *text;
	size_t isa_sz = 0;
	int r;

	text = build_gfx10_cache_bench_isa_text(test);
	if (!text)
		return 0;
	amdgpu_format_mcpu(10, gfx_minor, 0, mcpu, sizeof(mcpu));
	if (amdgpu_llvm_asm_init() != 0) {
		free(text);
		return 0;
	}
	r = amdgpu_llvm_assemble(mcpu, text, dst, dst_size, &isa_sz);
	free(text);
	amdgpu_llvm_asm_shutdown();
	return (r == 0) ? (uint32_t)isa_sz : 0;
}

/*
 * DWORD3 of a GCN/RDNA 4-DWORD raw-buffer resource descriptor ("V#"), used by
 * the GFX11 cache-benchmark shaders' buffer_load/store_format_xyzw
 * instructions. lib/amdgpu/compute_utils/amd_dispatch.c's shader-UAV setup
 * programs the identical word for the same generation (its own GFX9/10/11/12
 * constants 0x74fac/0x1104bfac/0x1003dfac/0x1203dfac), decoded here once:
 *
 *   DST_SEL_X/Y/Z/W   bits [2:0]/[5:3]/[8:6]/[11:9], value 4/5/6/7 ->
 *                     identity channel swizzle (X<-X, Y<-Y, Z<-Z, W<-W).
 *   FORMAT            bits [18:12] (7 bits; GFX10+ unified format
 *                     enum that replaces the separate NUM_FORMAT[14:12] +
 *                     DATA_FORMAT[20:15] fields pre-GFX10 hardware uses):
 *                     this generation's enum value for 32_32_32_32_UINT --
 *                     four 32-bit components, i.e. 16 bytes/record, matching
 *                     the dwordx4 buffer_load/store_format_xyzw instructions
 *                     and the "records = size / 16" byte accounting around
 *                     every caller of this descriptor. GFX10's enum value
 *                     (0x4b) differs from GFX11/GFX12's (0x3d) because the
 *                     format enum was renumbered between those generations,
 *                     not because the format itself differs.
 *
 * The remaining high bits are generation-specific OOB-select/resource-level
 * control; reused verbatim from amd_dispatch.c's already-working descriptor
 * word for GFX11 (0x10000000) rather than re-derived here, since their exact
 * names aren't load-bearing for this benchmark.
 */
#define CACHE_BENCH_VBUF_DST_SEL(x, y, z, w) \
	(((x) & 0x7) | (((y) & 0x7) << 3) | (((z) & 0x7) << 6) | (((w) & 0x7) << 9))
#define CACHE_BENCH_VBUF_IDENTITY_SWIZZLE CACHE_BENCH_VBUF_DST_SEL(4, 5, 6, 7)
#define CACHE_BENCH_VBUF_FORMAT(fmt) (((fmt) & 0x7f) << 12)
#define CACHE_BENCH_VBUF_FORMAT_32_32_32_32_UINT_GFX11 0x3d
#define CACHE_BENCH_VBUF_RESOURCE_CTRL_GFX11 0x10000000
#define CACHE_BENCH_VBUF_WORD3_GFX11 \
	(CACHE_BENCH_VBUF_IDENTITY_SWIZZLE | \
	 CACHE_BENCH_VBUF_FORMAT(CACHE_BENCH_VBUF_FORMAT_32_32_32_32_UINT_GFX11) | \
	 CACHE_BENCH_VBUF_RESOURCE_CTRL_GFX11)

static int
emit_gfx11_cache_bench_descriptors(struct amdgpu_cmd_base *ib,
				   uint64_t source_va, uint64_t destination_va,
				   uint32_t records)
{
	ib->emit(ib, PACKET3_COMPUTE(PKT3_SET_SH_REG, 8));
	ib->emit(ib, 0x240);
	ib->emit(ib, source_va);
	ib->emit(ib, (source_va >> 32) | 0x100000);
	ib->emit(ib, records);
	ib->emit(ib, CACHE_BENCH_VBUF_WORD3_GFX11);
	ib->emit(ib, destination_va);
	ib->emit(ib, (destination_va >> 32) | 0x100000);
	ib->emit(ib, 4096);
	ib->emit(ib, CACHE_BENCH_VBUF_WORD3_GFX11);
	return 0;
}

static int
emit_gfx10_cache_bench_descriptors(struct amdgpu_cmd_base *ib,
				   uint64_t source_va, uint64_t destination_va,
				   uint32_t records)
{
	/*
	 * No buffer descriptors: the GFX10 kernels use flat/SMEM addressing
	 * directly from raw VAs (see the ABI comment above
	 * build_gfx10_cache_bench_isa_text()). 8 user SGPRs declared
	 * (matching write2hw's RSRC2=0x90) so TGID_X lands in s8; the extra 4
	 * dwords beyond the two VAs are unused padding. @records is unused
	 * on this generation -- there is no NUM_RECORDS field without a
	 * buffer descriptor.
	 */
	(void)records;
	ib->emit(ib, PACKET3_COMPUTE(PKT3_SET_SH_REG, 8));
	ib->emit(ib, 0x240);
	ib->emit(ib, (uint32_t)source_va);
	ib->emit(ib, (uint32_t)(source_va >> 32));
	ib->emit(ib, (uint32_t)destination_va);
	ib->emit(ib, (uint32_t)(destination_va >> 32));
	ib->emit(ib, 0);
	ib->emit(ib, 0);
	ib->emit(ib, 0);
	ib->emit(ib, 0);
	return 0;
}

/*
 * Per-generation hook tables, mirroring amd_ip_blocks.c's registration-table
 * design: each GFX generation gets one named, static amd_spm_ops table plus a
 * family/minor support predicate; select_ops() below is a generic lookup over
 * amd_spm_generations[], not a switch statement re-implemented per caller.
 * Everything past select_ops() only ever calls capture->ops.*, never branches
 * on gfx_major/gfx_minor itself.
 */
static const struct amd_spm_ops gfx10_ops = {
	.build_layout = build_gfx10_layout,
	.emit_setup = emit_gfx10_setup,
	.emit_start = emit_windowed_start,
	.emit_stop_reset = emit_windowed_stop_reset,
	.build_cache_bench_shader = build_gfx10_cache_bench_shader,
	.emit_cache_bench_descriptors = emit_gfx10_cache_bench_descriptors,
};

static const struct amd_spm_ops gfx11_ops = {
	.build_layout = build_gfx11_layout,
	.emit_setup = emit_gfx11_setup,
	.emit_start = emit_windowed_start,
	.emit_stop_reset = emit_windowed_stop_reset,
	.build_cache_bench_shader = build_gfx11_cache_bench_shader,
	.emit_cache_bench_descriptors = emit_gfx11_cache_bench_descriptors,
};

static const struct amd_spm_ops gfx12_ops = {
	.build_layout = build_gfx12_layout,
	.emit_setup = emit_gfx11_setup,
	.emit_start = emit_windowed_start,
	.emit_stop_reset = emit_windowed_stop_reset,
};

static bool
gfx10_minor_supported(uint32_t minor)
{
	return minor == 1 || minor == 3;
}

static bool
gfx10_family_supported(uint32_t family_id)
{
	switch (family_id) {
	case FAMILY_NV:
	case FAMILY_VGH:
	case FAMILY_YC:
	case FAMILY_GFX1036:
	case FAMILY_GFX1037:
		return true;
	default:
		return false;
	}
}

static bool
gfx11_minor_supported(uint32_t minor)
{
	return minor == 0 || minor == 5 || minor == 7;
}

static bool
gfx11_family_supported(uint32_t family_id)
{
	switch (family_id) {
	case FAMILY_GFX1100:
	case FAMILY_GFX1103:
	case FAMILY_GFX1150:
		return true;
	default:
		return false;
	}
}

static bool
gfx12_minor_supported(uint32_t minor)
{
	/*
	 * Previously an unconditional return-true stub, never validated:
	 * tried against a real Navi48 (RDNA4) part, this faulted (RLC page
	 * fault writing its first SPM sample; GPU recovered via MODE1
	 * reset). Root-caused and fixed two real bugs in amd_spm.c itself
	 * (not this chip): gfx12_ops used the non-windowed start/stop path
	 * with no VMID reservation even though Mesa's ac_spm.c confirms the
	 * windowed-counter enable sequence is unconditional for every
	 * generation, and build_gfx12_layout()'s data_offset ignored the
	 * reserved 32-byte ring header every generation has. Both fixed
	 * below -- capture now runs end-to-end with no fault, real ring
	 * samples, and a correctly-ticking hardware timestamp (confirmed
	 * live on this card). Still disabled: the actual CPF-busy counter
	 * value itself reads a flat, stable zero (not flaky) despite the
	 * event-select register being programmed correctly per Mesa's
	 * generic-block path and UMR's gc_12_0_0.reg bitfield layout, and
	 * despite ruling out GFXOFF (confirmed a red herring for the real,
	 * ~7ms-fast capture path). Root cause of that remaining piece is
	 * still open -- re-enable only once CPF-busy reads real nonzero
	 * data, not before.
	 */
	return false;
}

static bool
gfx12_family_supported(uint32_t family_id)
{
	return false;
}

struct amd_spm_generation {
	uint32_t gfx_major;
	bool (*minor_supported)(uint32_t minor);
	bool (*family_supported)(uint32_t family_id);
	const struct amd_spm_ops *ops;
};

static const struct amd_spm_generation spm_generations[] = {
	{ 10, gfx10_minor_supported, gfx10_family_supported, &gfx10_ops },
	{ 11, gfx11_minor_supported, gfx11_family_supported, &gfx11_ops },
	{ 12, gfx12_minor_supported, gfx12_family_supported, &gfx12_ops },
};

static int
select_ops(const struct amdgpu_gpu_info *gpu_info,
	   const struct drm_amdgpu_info_hw_ip *gfx,
	   struct amd_spm_capture *capture)
{
	uint32_t gfx_minor = gfx->hw_ip_version_minor;

	/*
	 * Pre-IP-discovery-reporting ASICs report hw_ip_version_minor as 0
	 * instead of the real minor version (confirmed on this kernel for
	 * both GFX10.1/Navi10 and GFX10.3/Sienna Cichlid, via the kernel's
	 * ip_discovery debug sysfs for the GC block showing the correct
	 * value while this ioctl reports 0). Apply the same two-way
	 * correction Mesa uses in ac_gpu_info.c (fixing incorrect IP
	 * versions reported by the kernel): Navi10/12/14 (and GFX1013) are
	 * GFX10.1, every other pre-discovery NV/VGH part is GFX10.3.
	 */
	if (gfx->hw_ip_version_major == 10 && gfx_minor == 0 &&
	    (gpu_info->family_id == FAMILY_NV || gpu_info->family_id == FAMILY_VGH)) {
		if (gpu_info->family_id == FAMILY_NV &&
		    (ASICREV_IS_NAVI10_P(gpu_info->chip_external_rev) ||
		     ASICREV_IS_NAVI12_P(gpu_info->chip_external_rev) ||
		     ASICREV_IS_NAVI14_M(gpu_info->chip_external_rev)))
			gfx_minor = 1;
		else
			gfx_minor = 3;
	}

	for (uint32_t index = 0; index < ARRAY_SIZE(spm_generations); index++) {
		const struct amd_spm_generation *generation = &spm_generations[index];

		if (generation->gfx_major != gfx->hw_ip_version_major)
			continue;
		if (!generation->minor_supported(gfx_minor) ||
		    !generation->family_supported(gpu_info->family_id))
			return -EOPNOTSUPP;
		capture->gfx_major = generation->gfx_major;
		capture->gfx_minor = gfx_minor;
		capture->ops = *generation->ops;
		return capture->ops.build_layout(capture);
	}
	return -EOPNOTSUPP;
}

bool
amd_spm_capture_supported(const struct amdgpu_gpu_info *gpu_info,
			  const struct drm_amdgpu_info_hw_ip *gfx)
{
	struct amd_spm_capture capture = {};

	return gpu_info && gfx && (gfx->available_rings & 1) &&
	       !select_ops(gpu_info, gfx, &capture);
}

/*
 * One-shot helper shared by every caller that needs to run a short command
 * buffer and know it finished before returning: copy @ib into a fresh GTT BO,
 * submit it on capture's existing GFX context (alongside the SPM ring BO, so
 * the kernel keeps both resident), and block on the resulting fence. Used for
 * the setup/start/stop-reset sequences and, separately, whenever a cache
 * counter read needs a quick register probe.
 */
static int
submit_ib(struct amd_spm_capture *capture, const struct amdgpu_cmd_base *ib)
{
	amdgpu_bo_handle ib_bo = NULL;
	amdgpu_bo_handle resources[2];
	amdgpu_bo_list_handle bo_list = NULL;
	amdgpu_va_handle ib_va = NULL;
	void *ib_cpu = NULL;
	uint64_t ib_gpu_va = 0;
	struct amdgpu_cs_ib_info ib_info = {};
	struct amdgpu_cs_request request = {};
	struct amdgpu_cs_fence fence = {};
	uint32_t expired = 0;
	uint32_t ib_size;
	int ret;

	if (!ib->cdw || ib->cdw > ib->max_dw)
		return -EINVAL;
	ib_size = (ib->cdw * sizeof(uint32_t) + 4095u) & ~4095u;
	ret = amdgpu_bo_alloc_and_map(capture->device, ib_size, 4096,
				       AMDGPU_GEM_DOMAIN_GTT, 0,
				       &ib_bo, &ib_cpu, &ib_gpu_va, &ib_va);
	if (ret)
		return ret;
	memset(ib_cpu, 0, ib_size);
	memcpy(ib_cpu, ib->buf, ib->cdw * sizeof(uint32_t));
	resources[0] = ib_bo;
	resources[1] = capture->ring_bo;
	ret = amdgpu_bo_list_create(capture->device, 2, resources, NULL, &bo_list);
	if (ret)
		goto out;
	ib_info.ib_mc_address = ib_gpu_va;
	ib_info.size = ib->cdw;
	request.ip_type = AMDGPU_HW_IP_GFX;
	request.ring = 0;
	request.resources = bo_list;
	request.number_of_ibs = 1;
	request.ibs = &ib_info;
	ret = amdgpu_cs_submit(capture->context, 0, &request, 1);
	if (ret)
		goto out;
	fence.context = capture->context;
	fence.ip_type = AMDGPU_HW_IP_GFX;
	fence.ip_instance = 0;
	fence.ring = 0;
	fence.fence = request.seq_no;
	ret = amdgpu_cs_query_fence_status(&fence, AMDGPU_TIMEOUT_INFINITE,
					   0, &expired);
	if (!ret && !expired)
		ret = -ETIME;
out:
	if (bo_list)
		amdgpu_bo_list_destroy(bo_list);
	if (ib_bo)
		amdgpu_bo_unmap_and_free(ib_bo, ib_va, ib_gpu_va, ib_size);
	return ret;
}

int
amd_spm_capture_init(struct amd_spm_capture *capture,
		     amdgpu_device_handle device)
{
	struct drm_amdgpu_info_hw_ip gfx = {};
	struct amdgpu_gpu_info gpu_info = {};
	int ret;

	if (!capture || !device)
		return -EINVAL;
	memset(capture, 0, sizeof(*capture));
	ret = amdgpu_query_hw_ip_info(device, AMDGPU_HW_IP_GFX, 0, &gfx);
	if (ret)
		return ret;
	if (!(gfx.available_rings & 1))
		return -ENODEV;
	ret = amdgpu_query_gpu_info(device, &gpu_info);
	if (ret)
		return ret;
	ret = select_ops(&gpu_info, &gfx, capture);
	if (ret)
		return ret;
	capture->device = device;
	capture->ring_size = AMD_SPM_RING_SIZE_BYTES;
	capture->sample_size = AMD_SPM_SAMPLE_SIZE;
	capture->ib = get_cmd_base();
	if (!capture->ib)
		return -ENOMEM;
	if (capture->ib->allocate_buf(capture->ib, AMD_SPM_IB_DWORDS)) {
		ret = -ENOMEM;
		goto fail;
	}
	if (capture->profile.windowed_counters) {
		ret = amdgpu_vm_reserve_vmid(device, 0);
		if (ret)
			goto fail;
		capture->vmid_reserved = true;
	}
	ret = amdgpu_cs_ctx_create(device, &capture->context);
	if (ret)
		goto fail;
	ret = amdgpu_bo_alloc_and_map(device, capture->ring_size, 4096,
				      AMDGPU_GEM_DOMAIN_VRAM,
				      AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED,
				      &capture->ring_bo, (void **)&capture->ring_cpu,
				      &capture->ring_gpu_va, &capture->ring_va);
	if (ret) {
		amdgpu_cs_ctx_free(capture->context);
		goto fail;
	}
	memset((void *)capture->ring_cpu, 0, capture->ring_size);
	return 0;
fail:
	if (capture->vmid_reserved)
		amdgpu_vm_unreserve_vmid(device, 0);
	free_cmd_base(capture->ib);
	memset(capture, 0, sizeof(*capture));
	return ret;
}

int
amd_spm_capture_start(struct amd_spm_capture *capture)
{
	int ret;

	if (!capture || !capture->ring_bo || !capture->ring_cpu ||
	    !capture->ib || capture->active)
		return -EINVAL;
	memset((void *)capture->ring_cpu, 0, capture->ring_size);
	capture->ib->cdw = 0;
	ret = capture->ops.emit_setup(capture->ib, capture);
	if (!ret)
		ret = capture->ops.emit_start(capture->ib, capture);
	if (ret)
		return ret;
	capture->active = true;
	return submit_ib(capture, capture->ib);
}

int
amd_spm_capture_stop(struct amd_spm_capture *capture)
{
	int ret;

	if (!capture || !capture->active)
		return 0;
	capture->ib->cdw = 0;
	ret = capture->ops.emit_stop_reset(capture->ib, capture);
	if (!ret)
		ret = submit_ib(capture, capture->ib);
	if (!ret)
		capture->active = false;
	return ret;
}

static int
decode_samples(const struct amd_spm_capture *capture,
	       uint32_t *num_samples, uint64_t *counter_sum)
{
	const volatile uint16_t *samples;
	uint64_t data_size;
	uint32_t sample_index;
	uint64_t sum = 0;

	*num_samples = 0;
	*counter_sum = 0;
	if (capture->ring_size < capture->profile.data_offset)
		return -EINVAL;
	data_size = (uint64_t)capture->ring_cpu[0] *
		    capture->profile.pointer_granularity;
	if (!data_size || data_size % capture->sample_size ||
	    data_size > capture->ring_size - capture->profile.data_offset)
		return -ENODATA;
	*num_samples = data_size / capture->sample_size;
	samples = (const volatile uint16_t *)((const volatile uint8_t *)capture->ring_cpu +
					     capture->profile.data_offset);
	for (sample_index = 0; sample_index < *num_samples; sample_index++)
		sum += samples[sample_index * (capture->sample_size / 2) + 4];
	*counter_sum = sum;
	return 0;
}

int
amd_spm_capture_read(struct amd_spm_capture *capture,
		     uint32_t *num_samples, uint64_t *counter_sum)
{
	bool busy = false;
	int ret;

	if (!capture || capture->active || !capture->ring_cpu ||
	    !num_samples || !counter_sum)
		return -EINVAL;
	ret = amdgpu_bo_wait_for_idle(capture->ring_bo, AMDGPU_TIMEOUT_INFINITE,
				      &busy);
	if (ret)
		return ret;
	if (busy)
		return -EBUSY;
	ret = amdgpu_bo_cpu_unmap(capture->ring_bo);
	if (ret)
		return ret;
	ret = amdgpu_bo_cpu_map(capture->ring_bo, (void **)&capture->ring_cpu);
	if (ret)
		return ret;
	__sync_synchronize();
	return decode_samples(capture, num_samples, counter_sum);
}

/*
 * Per-generation cache-profile builders, same registration-table idiom as
 * spm_generations[] above: each supported generation gets one named builder
 * function (which also picks that generation's muxsel pack() function) plus
 * a minor-version predicate (amd_spm_capture_enable_caches() itself never
 * branches on gfx_major/gfx_minor -- it only looks the generation up once).
 */
static int
build_gfx10_cache_profile(struct amd_spm_cache_profile *profile,
			  const struct drm_amdgpu_info_device *gpu,
			  uint32_t num_tcc, uint32_t num_tcp)
{
	int ret;

	profile->pack = pack_gfx10_mux;

	/* L2 / GL2C: global block, one request+miss pair per live TCC instance. */
	for (uint32_t instance = 0; instance < num_tcc; instance++) {
		ret = spm_cache_add_pair(profile, AMD_SPM_CACHE_L2,
					AMD_SPM_GLOBAL_SEGMENT, 8, 0, instance,
					(1u << 31) | (1u << 29) | instance,
					0x036e00, 0x03, 0x23);
		if (ret)
			return ret;
	}
	for (uint32_t se = 0; se < profile->num_se; se++) {
		for (uint32_t sa = 0; sa < gpu->num_shader_arrays_per_engine; sa++) {
			uint32_t grbm = (se << 16) | (sa << 8);

			/* L0 / TCP: per-SE, per-SA, one pair per live CU/TCP instance. */
			for (uint32_t instance = 0; instance < num_tcp; instance++) {
				ret = spm_cache_add_pair(profile, AMD_SPM_CACHE_L0,
							se, 7, sa, instance, grbm | instance,
							0x036d00, 0x09, 0x12);
				if (ret)
					return ret;
			}
			/* L1 / GL1C: per-SE, per-SA, fixed 4 instances. */
			for (uint32_t instance = 0; instance < 4; instance++) {
				ret = spm_cache_add_pair(profile, AMD_SPM_CACHE_L1,
							se, 12, sa, instance, grbm | instance,
							0x036e80, 0x0e, 0x12);
				if (ret)
					return ret;
			}
		}

		/*
		 * Instruction + scalar cache: plain GFX10 SQ block (not GFX11's
		 * SQ_WGP), block select AC_SPM_SE_BLOCK_SQG, one broadcast
		 * instance per SE (not per-WGP), 4 independent 32-bit counters
		 * at consecutive SQ_PERFCOUNTERn_SELECT registers.
		 */
		{
			uint32_t sq_grbm = (se << 16) | (1u << 29);
			const uint32_t sq_values[4] = {
				0x12c | AMD_SPM_GFX10_SQ_SELECT_FLAGS,
				0x12d | AMD_SPM_GFX10_SQ_SELECT_FLAGS,
				0x14f | AMD_SPM_GFX10_SQ_SELECT_FLAGS,
				0x150 | AMD_SPM_GFX10_SQ_SELECT_FLAGS,
			};

			ret = spm_cache_add_program(profile, sq_grbm, 0x036700, sq_values, 4);
			if (ret)
				return ret;
			for (uint32_t wire = 0; wire < 4; wire++) {
				ret = spm_cache_add_counter(profile,
					wire < 2 ? AMD_SPM_CACHE_INSTRUCTION : AMD_SPM_CACHE_SCALAR,
					wire & 1 ? SPM_CACHE_MISSES : SPM_CACHE_HITS,
					se, 9, 0, 0, wire);
				if (ret)
					return ret;
			}
		}
	}
	return 0;
}

static int
build_gfx11_cache_profile(struct amd_spm_cache_profile *profile,
			  const struct drm_amdgpu_info_device *gpu,
			  uint32_t num_tcc, uint32_t num_tcp)
{
	int ret;

	profile->pack = pack_gfx11_mux;

	/* L2 / GL2C: global block, one request+miss pair per live TCC instance. */
	for (uint32_t instance = 0; instance < num_tcc; instance++) {
		ret = spm_cache_add_pair(profile, AMD_SPM_CACHE_L2,
					AMD_SPM_GLOBAL_SEGMENT, 8, 0, instance,
					(1u << 29) | (1u << 30) | instance,
					0x036e00, 0x03, 0x2b);
		if (ret)
			return ret;
	}
	for (uint32_t se = 0; se < profile->num_se; se++) {
		for (uint32_t sa = 0; sa < gpu->num_shader_arrays_per_engine; sa++) {
			uint32_t grbm = (se << 16) | (sa << 8);

			/* L0 / TCP: per-SE, per-SA, one pair per live CU/TCP instance. */
			for (uint32_t instance = 0; instance < num_tcp; instance++) {
				ret = spm_cache_add_pair(profile, AMD_SPM_CACHE_L0,
							se, 7, sa, instance, grbm | instance,
							0x036d00, 0x09, 0x11);
				if (ret)
					return ret;
			}
			/*
			 * L1 / GL1C (fixed 4 instances), plus the instruction +
			 * scalar cache: GFX11's SQ_WGP block, block select 17,
			 * one 4-value windowed/paired program per WGP instance
			 * (shares the GL1C instance loop, matching the proven
			 * spm_cache_with_hooks reference).
			 */
			for (uint32_t instance = 0; instance < 4; instance++) {
				const uint32_t sq_values[4] = {
					0x10e | (1u << 20), 0x10f | (1u << 20),
					0x126 | (1u << 20), 0x127 | (1u << 20),
				};

				ret = spm_cache_add_pair(profile, AMD_SPM_CACHE_L1,
							se, 12, sa, instance, grbm | instance,
							0x036e80, 0x0e, 0x12);
				if (ret)
					return ret;
				ret = spm_cache_add_program(profile, grbm | (instance << 2),
							   0x036700, sq_values, 4);
				if (ret)
					return ret;
				for (uint32_t event = 0; event < 4; event++) {
					ret = spm_cache_add_counter(profile,
						event < 2 ? AMD_SPM_CACHE_INSTRUCTION :
						AMD_SPM_CACHE_SCALAR,
						event & 1 ? SPM_CACHE_MISSES : SPM_CACHE_HITS,
						se, 17, sa, instance, event);
					if (ret)
						return ret;
				}
			}
		}
	}
	return 0;
}

typedef int (*spm_cache_build_fn)(struct amd_spm_cache_profile *profile,
				  const struct drm_amdgpu_info_device *gpu,
				  uint32_t num_tcc, uint32_t num_tcp);

struct amd_spm_cache_generation {
	uint32_t gfx_major;
	bool (*minor_supported)(uint32_t minor);
	spm_cache_build_fn build;
};

/*
 * Deliberately GFX10.1/Navi10 only (minor == 1), not also accepting
 * minor == 3 (GFX10.3, e.g. Sienna Cichlid): build_gfx10_cache_profile()'s
 * event IDs were validated against real Navi10 hardware only. GFX10.3 uses
 * a different L2/GL2C miss event ID (0x2b, like GFX11, not GFX10.1's 0x23 --
 * confirmed from Mesa's ac_spm.c gfx103_gl2c_perf_sel_miss), so reusing the
 * GFX10.1 profile unchanged would silently produce wrong L2 hit-rate data
 * instead of cleanly reporting unsupported. Extend this (and
 * build_gfx10_cache_profile()'s event table) once GFX10.3 is validated.
 */
static bool
cache_gfx10_minor_supported(uint32_t minor)
{
	return minor == 1;
}

static bool
cache_gfx11_minor_supported(uint32_t minor)
{
	return minor == 0;
}

static const struct amd_spm_cache_generation spm_cache_generations[] = {
	{ 10, cache_gfx10_minor_supported, build_gfx10_cache_profile },
	{ 11, cache_gfx11_minor_supported, build_gfx11_cache_profile },
};

/*
 * Switch an already-initialized capture from its default single CPF-busy
 * counter over to per-cache-block (L0/L1/L2/instruction/scalar) hit-rate
 * counters. Broad strokes:
 *   1. Look up this generation's cache_build() callback and validate the GPU
 *      topology it needs (shader engine/array counts, TCC/TCP instance
 *      counts derived from the disabled-unit bitmasks).
 *   2. Allocate a fresh, empty amd_spm_cache_profile and pre-seed its
 *      global-segment line 0 with the same timestamp/CPF-busy mux values the
 *      non-cache profile uses, so decode_samples()'s hardcoded CPF-busy
 *      offset keeps working unchanged even in cache-capture mode.
 *   3. Let the per-generation builder append every cache counter's mux
 *      selects (and any block-specific "programs") to the profile.
 *   4. Size each segment: SPM's windowed counters pack two independent
 *      "channels" (even/odd) side by side in the same mux line, so the line
 *      count for N occupied mux slots is ceil(N/16) per channel, and the two
 *      channels only share lines up to the smaller channel's length --
 *      hence `2*even - 1` lines when even > odd (the extra odd line borrows
 *      half of the last even line) or plainly `2*odd` otherwise.
 *   5. Counter offsets the builder computed assume segment 0 starts at
 *      sample offset 0; now that global_lines/se_lines are known, shift every
 *      per-SE counter's offset past the global segment's samples.
 *   6. Replace the small CPF-busy ring with a larger one sized for the full
 *      per-segment sample layout, and store the finished profile.
 */
int
amd_spm_capture_enable_caches(struct amd_spm_capture *capture)
{
	struct drm_amdgpu_info_device gpu = {};
	struct amd_spm_cache_profile *profile;
	const struct amd_spm_cache_generation *generation = NULL;
	amdgpu_bo_handle ring_bo = NULL;
	amdgpu_va_handle ring_va = NULL;
	void *ring_cpu = NULL;
	uint64_t ring_gpu_va = 0;
	uint32_t num_tcc, num_tcp = 0;
	int ret;

	if (!capture || !capture->ring_bo || capture->active || capture->cache_profile)
		return -EINVAL;
	for (uint32_t index = 0; index < ARRAY_SIZE(spm_cache_generations); index++) {
		if (spm_cache_generations[index].gfx_major == capture->gfx_major) {
			generation = &spm_cache_generations[index];
			break;
		}
	}
	if (!generation || !generation->minor_supported(capture->gfx_minor))
		return -EOPNOTSUPP;
	if (capture->ib->allocate_buf(capture->ib, 8192))
		return -ENOMEM;
	ret = amdgpu_query_info(capture->device, AMDGPU_INFO_DEV_INFO, sizeof(gpu), &gpu);
	if (ret)
		return ret;
	if (!gpu.num_shader_engines || gpu.num_shader_engines > AMD_SPM_MAX_SE ||
	    !gpu.num_shader_arrays_per_engine || gpu.num_shader_arrays_per_engine > 2 ||
	    !gpu.num_tcc_blocks || gpu.num_tcc_blocks > 32)
		return -EOPNOTSUPP;
	if ((uint32_t)__builtin_popcountll(gpu.tcc_disabled_mask) >= gpu.num_tcc_blocks)
		return -EOPNOTSUPP;
	num_tcc = gpu.num_tcc_blocks - __builtin_popcountll(gpu.tcc_disabled_mask);
	for (uint32_t se = 0; se < gpu.num_shader_engines; se++) {
		for (uint32_t sa = 0; sa < gpu.num_shader_arrays_per_engine; sa++) {
			uint32_t mask = gpu.cu_bitmap[se % 4][(se / 4) * 2 + sa];
			uint32_t count = __builtin_popcount(mask);

			if (!mask)
				return -EOPNOTSUPP;
			if (count > num_tcp)
				num_tcp = count;
		}
	}
	if (!num_tcc || !num_tcp || num_tcp > 32)
		return -EOPNOTSUPP;
	profile = calloc(1, sizeof(*profile));
	if (!profile)
		return -ENOMEM;
	profile->num_se = gpu.num_shader_engines;

	/*
	 * Pre-seed the global segment's line 0 with the same timestamp/CPF-busy
	 * layout build_gfx10_layout()/build_gfx11_layout()/emit_muxsel() already use
	 * for the non-cache capture, so the existing CPF-busy decode in
	 * decode_samples() (hardcoded sample offset 4) keeps working unchanged
	 * alongside the new cache counters. The always-zero high 16 bits of
	 * counter_mux are intentionally not allocated a slot (matching the
	 * proven GFX11 reference, which only ever used 5 pre-seeded slots).
	 */
	profile->used[AMD_SPM_GLOBAL_SEGMENT][0] = 5;
	profile->mux[AMD_SPM_GLOBAL_SEGMENT][0][0] = capture->profile.timestamp_mux[0] & 0xffff;
	profile->mux[AMD_SPM_GLOBAL_SEGMENT][0][1] = capture->profile.timestamp_mux[0] >> 16;
	profile->mux[AMD_SPM_GLOBAL_SEGMENT][0][2] = capture->profile.timestamp_mux[1] & 0xffff;
	profile->mux[AMD_SPM_GLOBAL_SEGMENT][0][3] = capture->profile.timestamp_mux[1] >> 16;
	profile->mux[AMD_SPM_GLOBAL_SEGMENT][0][4] = capture->profile.counter_mux & 0xffff;

	ret = generation->build(profile, &gpu, num_tcc, num_tcp);
	if (ret)
		goto fail;
	for (uint32_t segment = 0; segment <= AMD_SPM_GLOBAL_SEGMENT; segment++) {
		uint32_t even = (profile->used[segment][0] + 15) / 16;
		uint32_t odd = (profile->used[segment][1] + 15) / 16;
		uint32_t lines = even > odd ? 2 * even - 1 : 2 * odd;

		if (segment == AMD_SPM_GLOBAL_SEGMENT)
			profile->global_lines = lines;
		else if (lines > profile->se_lines)
			profile->se_lines = lines;
	}
	for (uint32_t counter = 0; counter < profile->num_counters; counter++) {
		struct spm_cache_counter *info = &profile->counters[counter];

		if (info->segment != AMD_SPM_GLOBAL_SEGMENT)
			info->offset += 16 * (profile->global_lines +
					     info->segment * profile->se_lines);
	}
	ret = amdgpu_bo_alloc_and_map(capture->device, AMD_SPM_CACHE_RING_SIZE_BYTES,
				     4096, AMDGPU_GEM_DOMAIN_VRAM,
				     AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED,
				     &ring_bo, &ring_cpu, &ring_gpu_va, &ring_va);
	if (ret)
		goto fail;
	amdgpu_bo_unmap_and_free(capture->ring_bo, capture->ring_va,
				capture->ring_gpu_va, capture->ring_size);
	capture->ring_bo = ring_bo;
	capture->ring_va = ring_va;
	capture->ring_cpu = ring_cpu;
	capture->ring_gpu_va = ring_gpu_va;
	capture->ring_size = AMD_SPM_CACHE_RING_SIZE_BYTES;
	memset(ring_cpu, 0, capture->ring_size);
	capture->sample_size = 32 * (profile->global_lines + profile->num_se * profile->se_lines);
	capture->cache_profile = profile;
	return 0;
fail:
	free(profile);
	return ret;
}

/*
 * Decode every sample the ring collected (via the shared amd_spm_capture_read())
 * into per-cache hit/miss/request totals. Two counter shapes exist:
 *   - L0/L1/L2 each program a request counter AND a miss counter, so misses
 *     is the direct hardware sum and requests is too -- hits is derived
 *     (requests - misses) only when that's internally consistent (misses no
 *     greater than requests; a transient inconsistency is flagged via
 *     !consistent rather than asserted, since clamped/saturated 16-bit
 *     counters can otherwise look contradictory).
 *   - Instruction/scalar instead program one hit counter and one miss
 *     counter per event (no separate request counter), so requests is
 *     derived as hits + misses instead of read directly.
 * `saturated` flags any individual 16-bit sample that hit UINT16_MAX, which
 * the caller uses to suppress a hit-rate percentage that would otherwise be
 * silently wrong (clamped, not actually that value).
 */
int
amd_spm_capture_read_caches(struct amd_spm_capture *capture,
			    uint32_t *num_samples, uint64_t *counter_sum,
			    struct amd_spm_cache_stats stats[AMD_SPM_CACHE_COUNT])
{
	const volatile uint16_t *samples;
	int ret;

	if (!capture || !capture->cache_profile || !stats)
		return -EINVAL;
	ret = amd_spm_capture_read(capture, num_samples, counter_sum);
	if (ret)
		return ret;
	memset(stats, 0, AMD_SPM_CACHE_COUNT * sizeof(*stats));
	samples = (const volatile uint16_t *)((const volatile uint8_t *)capture->ring_cpu +
					     capture->profile.data_offset);
	for (uint32_t index = 0; index < capture->cache_profile->num_counters; index++) {
		const struct spm_cache_counter *counter = &capture->cache_profile->counters[index];
		struct amd_spm_cache_stats *result = &stats[counter->cache];
		uint64_t sum = 0;

		for (uint32_t sample = 0; sample < *num_samples; sample++) {
			uint16_t value = samples[sample * (capture->sample_size / 2) +
						counter->offset];

			result->saturated |= value == UINT16_MAX;
			sum += value;
		}
		if (counter->metric == SPM_CACHE_MISSES)
			result->misses += sum;
		else {
			result->instances++;
			if (counter->metric == SPM_CACHE_REQUESTS)
				result->requests += sum;
			else
				result->hits += sum;
		}
	}
	for (uint32_t cache = 0; cache < AMD_SPM_CACHE_COUNT; cache++) {
		struct amd_spm_cache_stats *result = &stats[cache];

		if (cache >= AMD_SPM_CACHE_INSTRUCTION)
			result->requests = result->hits + result->misses;
		result->consistent = result->misses <= result->requests;
		if (cache < AMD_SPM_CACHE_INSTRUCTION && result->consistent)
			result->hits = result->requests - result->misses;
	}
	return 0;
}

uint32_t
amd_spm_capture_build_cache_bench_shader(struct amd_spm_capture *capture,
					 void *dst, size_t dst_size,
					 const struct amd_spm_cache_bench_case *test)
{
	if (!capture->ops.build_cache_bench_shader)
		return 0;
	return capture->ops.build_cache_bench_shader(dst, dst_size, test, capture->gfx_minor);
}

int
amd_spm_capture_emit_cache_bench_descriptors(struct amd_spm_capture *capture,
					     struct amdgpu_cmd_base *ib,
					     uint64_t source_va, uint64_t destination_va,
					     uint32_t records)
{
	if (!capture->ops.emit_cache_bench_descriptors)
		return -EOPNOTSUPP;
	return capture->ops.emit_cache_bench_descriptors(ib, source_va, destination_va, records);
}

void
amd_spm_capture_fini(struct amd_spm_capture *capture)
{
	if (!capture)
		return;
	if (capture->active && amd_spm_capture_stop(capture))
		return;
	if (capture->ring_bo)
		amdgpu_bo_unmap_and_free(capture->ring_bo, capture->ring_va,
					 capture->ring_gpu_va, capture->ring_size);
	if (capture->context)
		amdgpu_cs_ctx_free(capture->context);
	if (capture->vmid_reserved)
		amdgpu_vm_unreserve_vmid(capture->device, 0);
	free_cmd_base(capture->ib);
	free(capture->cache_profile);
	memset(capture, 0, sizeof(*capture));
}
