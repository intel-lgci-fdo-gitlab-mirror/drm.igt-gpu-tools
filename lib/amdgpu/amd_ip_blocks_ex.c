// SPDX-License-Identifier: MIT
/*
 * Copyright 2025 Advanced Micro Devices, Inc.
 */

#include <stdint.h>
#include "ioctl_wrappers.h"
#include "amd_ip_blocks.h"
#include "amd_PM4.h"
#include "amdgpu_asic_addr.h"

static void gfx_program_compute_default(
	const struct amdgpu_ip_funcs *f,
	struct amdgpu_cmd_base *base,
	uint64_t code_addr,
	uint64_t user_data0_addr,
	uint32_t rsrc1_dw,
	uint32_t rsrc2_or_tmp,
	uint32_t thr_x, uint32_t thr_y, uint32_t thr_z)
{
	base->emit(base, PACKET3(PKT3_CONTEXT_CONTROL, 1));
	base->emit(base, 0x80000000);
	base->emit(base, 0x80000000);

	base->emit(base, PACKET3(PKT3_CLEAR_STATE, 0));
	base->emit(base, 0x80000000);

	base->emit(base, PACKET3(PKT3_SET_SH_REG, 2));
	base->emit(base, f->get_reg_offset(COMPUTE_PGM_LO));
	base->emit(base, (uint32_t)(code_addr >> 8));
	base->emit(base, (uint32_t)(code_addr >> 40));

	base->emit(base, PACKET3(PKT3_SET_SH_REG, 2));
	base->emit(base, f->get_reg_offset(COMPUTE_PGM_RSRC1));
	base->emit(base, rsrc1_dw);
	base->emit(base, rsrc2_or_tmp);

	base->emit(base, PACKET3(PKT3_SET_SH_REG, 1));
	base->emit(base, f->get_reg_offset(COMPUTE_TMPRING_SIZE));
	base->emit(base, 0x00000100);

	base->emit(base, PACKET3(PKT3_SET_SH_REG, 2));
	base->emit(base, f->get_reg_offset(COMPUTE_USER_DATA_0));
	base->emit(base, (uint32_t)user_data0_addr);
	base->emit(base, (uint32_t)(user_data0_addr >> 32));

	base->emit(base, PACKET3(PKT3_SET_SH_REG, 1));
	base->emit(base, f->get_reg_offset(COMPUTE_RESOURCE_LIMITS));
	base->emit(base, 0);

	base->emit(base, PACKET3(PKT3_SET_SH_REG, 3));
	base->emit(base, f->get_reg_offset(COMPUTE_NUM_THREAD_X));
	base->emit(base, thr_x);
	base->emit(base, thr_y);
	base->emit(base, thr_z);
}

static void gfx_dispatch_direct_default(
	const struct amdgpu_ip_funcs *f,
	struct amdgpu_cmd_base *base,
	uint32_t gx, uint32_t gy, uint32_t gz,
	uint32_t flags)
{
	base->emit(base, PACKET3(PACKET3_DISPATCH_DIRECT, 3));
	base->emit(base, gx);
	base->emit(base, gy);
	base->emit(base, gz);
	base->emit(base, flags);
	base->emit_aligned(base, 7, GFX_COMPUTE_NOP);
}

static void gfx_write_confirm_default(
	const struct amdgpu_ip_funcs *f,
	struct amdgpu_cmd_base *base,
	uint64_t dst, uint32_t val)
{
	base->emit(base, PACKET3(PACKET3_WRITE_DATA, 3));
	base->emit(base, WRITE_DATA_DST_SEL(5) | WR_CONFIRM);
	base->emit(base, (uint32_t)dst);
	base->emit(base, (uint32_t)(dst >> 32));
	base->emit(base, val);
	base->emit_aligned(base, 7, GFX_COMPUTE_NOP);
}

static void gfx_dispatch_direct_gfx9(
	const struct amdgpu_ip_funcs *f,
	struct amdgpu_cmd_base *base,
	uint32_t gx, uint32_t gy, uint32_t gz,
	uint32_t flags_unused)
{
	gfx_dispatch_direct_default(f, base, gx, gy, gz, 0x00000000);
}

static void gfx_dispatch_direct_gfx10(
	const struct amdgpu_ip_funcs *f,
	struct amdgpu_cmd_base *base,
	uint32_t gx, uint32_t gy, uint32_t gz,
	uint32_t flags_unused)
{
	gfx_dispatch_direct_default(f, base, gx, gy, gz, 0x00000045);
}

static void gfx_program_compute_gfx11(
	const struct amdgpu_ip_funcs *f,
	struct amdgpu_cmd_base *base,
	uint64_t code, uint64_t udata0,
	uint32_t rsrc1, uint32_t rsrc2,
	uint32_t tx, uint32_t ty, uint32_t tz)
{
	base->emit(base, PACKET3(PKT3_CONTEXT_CONTROL, 1));
	base->emit(base, 0x80000000);
	base->emit(base, 0x80000000);

	base->emit(base, PACKET3(PKT3_CLEAR_STATE, 0));
	base->emit(base, 0x80000000);

	base->emit(base, PACKET3(PKT3_SET_SH_REG, 2));
	base->emit(base, f->get_reg_offset(COMPUTE_PGM_LO));
	base->emit(base, (uint32_t)(code >> 8));
	base->emit(base, (uint32_t)(code >> 40));

	base->emit(base, PACKET3(PKT3_SET_SH_REG, 2));
	base->emit(base, f->get_reg_offset(COMPUTE_PGM_RSRC1));
	base->emit(base, rsrc1);
	base->emit(base, rsrc2);

	base->emit(base, PACKET3(PKT3_SET_SH_REG, 2));
	base->emit(base, f->get_reg_offset(COMPUTE_USER_DATA_0));
	base->emit(base, (uint32_t)udata0);
	base->emit(base, (uint32_t)(udata0 >> 32));

	base->emit(base, PACKET3(PKT3_SET_SH_REG, 1));
	base->emit(base, f->get_reg_offset(COMPUTE_RESOURCE_LIMITS));
	base->emit(base, 0);

	base->emit(base, PACKET3(PKT3_SET_SH_REG, 3));
	base->emit(base, f->get_reg_offset(COMPUTE_NUM_THREAD_X));
	base->emit(base, tx);
	base->emit(base, ty);
	base->emit(base, tz);
}

static void gfx_program_compute_gfx12(
	const struct amdgpu_ip_funcs *f,
	struct amdgpu_cmd_base *base,
	uint64_t code, uint64_t udata0,
	uint32_t rsrc1, uint32_t rsrc2,
	uint32_t tx, uint32_t ty, uint32_t tz)
{
	base->emit(base, PACKET3(PKT3_CONTEXT_CONTROL, 1));
	base->emit(base, 0x80000000);
	base->emit(base, 0x80000000);

	base->emit(base, PACKET3(PKT3_SET_SH_REG, 2));
	base->emit(base, f->get_reg_offset(COMPUTE_PGM_LO));
	base->emit(base, (uint32_t)(code >> 8));
	base->emit(base, (uint32_t)(code >> 40));

	base->emit(base, PACKET3(PKT3_SET_SH_REG, 2));
	base->emit(base, f->get_reg_offset(COMPUTE_PGM_RSRC1));
	base->emit(base, rsrc1);
	base->emit(base, rsrc2);

	base->emit(base, PACKET3(PKT3_SET_SH_REG, 2));
	base->emit(base, f->get_reg_offset(COMPUTE_USER_DATA_0));
	base->emit(base, (uint32_t)udata0);
	base->emit(base, (uint32_t)(udata0 >> 32));

	base->emit(base, PACKET3(PKT3_SET_SH_REG, 1));
	base->emit(base, f->get_reg_offset(COMPUTE_RESOURCE_LIMITS));
	base->emit(base, 0);

	base->emit(base, PACKET3(PKT3_SET_SH_REG, 3));
	base->emit(base, f->get_reg_offset(COMPUTE_NUM_THREAD_X));
	base->emit(base, tx);
	base->emit(base, ty);
	base->emit(base, tz);
}


static void gfx_dispatch_direct_gfx11(
	const struct amdgpu_ip_funcs *f,
	struct amdgpu_cmd_base *base,
	uint32_t gx, uint32_t gy, uint32_t gz,
	uint32_t flags_unused)
{
	gfx_dispatch_direct_default(f, base, gx, gy, gz, 0x00000045);
}

static void gfx_emit_nops_default(struct amdgpu_cmd_base *base, uint32_t count)
{
	uint32_t i;

	for (i = 0; i < count; i++)
		base->emit(base, PACKET3(PACKET3_NOP, 0));
}

static void gfx_write_data_mem_default(
	const struct amdgpu_ip_funcs *funcs,
	struct amdgpu_cmd_base *base,
	uint32_t engine_sel,
	uint64_t dst_addr,
	uint32_t value,
	bool wr_confirm)
{
	uint32_t flags = WRITE_DATA_DST_SEL(5);	/* memory address */

	flags |= WRITE_DATA_ENGINE_SEL(engine_sel);
	if (wr_confirm)
		flags |= WR_CONFIRM;

	base->emit(base, PACKET3(PACKET3_WRITE_DATA, 3));
	base->emit(base, flags);
	base->emit(base, lower_32_bits(dst_addr));
	base->emit(base, upper_32_bits(dst_addr));
	base->emit(base, value);
}


static int gfx_ring_wait_reg_mem_hang(const struct amdgpu_ip_funcs *func,
				      const struct amdgpu_ring_context *ring_context,
				      uint32_t *pm4_dw);
static int gfx_ring_priv_fault_hang(const struct amdgpu_ip_funcs *func,
				    const struct amdgpu_ring_context *ring_context,
				    uint32_t *pm4_dw);

static int gfx_ring_priv_inst_hang(const struct amdgpu_ip_funcs *func,
				   const struct amdgpu_ring_context *ring_context,
				   uint32_t *pm4_dw);

void amd_ip_blocks_ex_init(struct amdgpu_ip_funcs *funcs)
{
	funcs->gfx_program_compute = gfx_program_compute_default;
	funcs->gfx_dispatch_direct = gfx_dispatch_direct_default;
	funcs->gfx_write_confirm = gfx_write_confirm_default;
	funcs->gfx_emit_nops = gfx_emit_nops_default;
	funcs->gfx_write_data_mem = gfx_write_data_mem_default;

	/* Deadlock/hang test hooks */
	funcs->wait_reg_mem_hang = gfx_ring_wait_reg_mem_hang;
	funcs->priv_fault_hang = gfx_ring_priv_fault_hang;
	funcs->priv_inst_hang = gfx_ring_priv_inst_hang;

	switch (funcs->family_id) {
	case AMDGPU_FAMILY_RV:
	case AMDGPU_FAMILY_NV:
	case AMDGPU_FAMILY_VGH:
		funcs->gfx_dispatch_direct = gfx_dispatch_direct_gfx9;
		break;
	case AMDGPU_FAMILY_YC:
	case AMDGPU_FAMILY_GC_10_3_6:
	case AMDGPU_FAMILY_GC_10_3_7:
		funcs->gfx_dispatch_direct = gfx_dispatch_direct_gfx10;
		break;
	case AMDGPU_FAMILY_GC_11_0_0:
	case AMDGPU_FAMILY_GC_11_0_1:
	case AMDGPU_FAMILY_GC_11_5_0:
		funcs->gfx_program_compute = gfx_program_compute_gfx11;
		funcs->gfx_dispatch_direct = gfx_dispatch_direct_gfx11;
		break;
	case AMDGPU_FAMILY_GC_12_0_0:
		funcs->gfx_program_compute =gfx_program_compute_gfx12;
		funcs->gfx_dispatch_direct = gfx_dispatch_direct_gfx11;
		break;
	default:
		break;
	}
}

/*
 * Emit PACKET3_WAIT_REG_MEM for deadlock/hang tests. Uses FUNCTION(4) for !=
 * comparison, polling a memory location (initialised to 0) for a value that
 * never arrives, so the queue hangs cleanly for per-queue reset testing.
 */
static int
gfx_ring_wait_reg_mem_hang(const struct amdgpu_ip_funcs *func,
			   const struct amdgpu_ring_context *ring_context,
			   uint32_t *pm4_dw)
{
	uint32_t i = *pm4_dw;

	ring_context->pm4[i++] = PACKET3(PACKET3_WAIT_REG_MEM, 5);
	ring_context->pm4[i++] = (WAIT_REG_MEM_MEM_SPACE(1) | /* memory */
				  WAIT_REG_MEM_FUNCTION(4) | /* != */
				  WAIT_REG_MEM_ENGINE(0));   /* me */
	ring_context->pm4[i++] = lower_32_bits(ring_context->bo_mc) & 0xfffffffc;
	ring_context->pm4[i++] = upper_32_bits(ring_context->bo_mc);
	ring_context->pm4[i++] = 0;          /* reference value */
	ring_context->pm4[i++] = 0xffffffff; /* and mask */
	ring_context->pm4[i++] = 0x00000004; /* poll interval */
	*pm4_dw = i;

	return 0;
}

/*
 * Emit a privileged INDIRECT_BUFFER (PRIV bit set) launched from an
 * unprivileged user queue, followed by a WAIT_REG_MEM hang. The privileged IB
 * launch is rejected by the CP with a fault interrupt and the queue then hangs
 * cleanly, so the driver recovers it with a per-queue reset.
 */
static int
gfx_ring_priv_inst_hang(const struct amdgpu_ip_funcs *func,
			const struct amdgpu_ring_context *ring_context,
			uint32_t *pm4_dw)
{
	uint32_t i = *pm4_dw;

	/* Privileged IB launch on an unprivileged UMQ -> priv-instruction fault. */
	ring_context->pm4[i++] = PACKET3(PACKET3_INDIRECT_BUFFER, 2);
	ring_context->pm4[i++] = lower_32_bits(ring_context->bo_mc) & 0xfffffffc;
	ring_context->pm4[i++] = upper_32_bits(ring_context->bo_mc);
	ring_context->pm4[i++] = 4 | (1u << 31); /* IB size (dwords) | PRIV bit */

	/* Then hang cleanly on a WAIT_REG_MEM that never completes. */
	ring_context->pm4[i++] = PACKET3(PACKET3_WAIT_REG_MEM, 5);
	ring_context->pm4[i++] = (WAIT_REG_MEM_MEM_SPACE(1) |
				  WAIT_REG_MEM_FUNCTION(4) |
				  WAIT_REG_MEM_ENGINE(0));
	ring_context->pm4[i++] = lower_32_bits(ring_context->bo_mc) & 0xfffffffc;
	ring_context->pm4[i++] = upper_32_bits(ring_context->bo_mc);
	ring_context->pm4[i++] = 0;          /* reference value */
	ring_context->pm4[i++] = 0xffffffff; /* and mask */
	ring_context->pm4[i++] = 0x00000004; /* poll interval */
	*pm4_dw = i;

	return 0;
}

/*
 * Emit an invalid opcode followed by a WAIT_REG_MEM hang for priv-fault tests.
 * The invalid opcode raises CP_BAD_OPCODE_ERROR (a gfx priv-fault) without
 * running the CP off into memory, then the WAIT_REG_MEM hangs the queue cleanly.
 */
static int
gfx_ring_priv_fault_hang(const struct amdgpu_ip_funcs *func,
			 const struct amdgpu_ring_context *ring_context,
			 uint32_t *pm4_dw)
{
	uint32_t i = *pm4_dw;

	/* Invalid opcode: CP raises CP_BAD_OPCODE_ERROR (gfx priv-fault). */
	ring_context->pm4[i++] = PACKET3(0xf2, 0);
	ring_context->pm4[i++] = 0x0;

	/* Then hang cleanly on a WAIT_REG_MEM that never completes. */
	ring_context->pm4[i++] = PACKET3(PACKET3_WAIT_REG_MEM, 5);
	ring_context->pm4[i++] = (WAIT_REG_MEM_MEM_SPACE(1) |
				  WAIT_REG_MEM_FUNCTION(4) |
				  WAIT_REG_MEM_ENGINE(0));
	ring_context->pm4[i++] = lower_32_bits(ring_context->bo_mc) & 0xfffffffc;
	ring_context->pm4[i++] = upper_32_bits(ring_context->bo_mc);
	ring_context->pm4[i++] = 0;          /* reference value */
	ring_context->pm4[i++] = 0xffffffff; /* and mask */
	ring_context->pm4[i++] = 0x00000004; /* poll interval */
	*pm4_dw = i;

	return 0;
}

