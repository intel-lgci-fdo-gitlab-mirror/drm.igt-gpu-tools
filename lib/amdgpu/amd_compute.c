/* SPDX-License-Identifier: MIT
 * Copyright 2014 Advanced Micro Devices, Inc.
 * Copyright 2022 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 *
 */
#include "amd_PM4.h"
#include "amd_memory.h"
#include "amd_compute.h"
#include "amd_sdma.h"

/**
 *
 * @param device
 * @param user_queue
 */
void amdgpu_command_submission_nop(amdgpu_device_handle device, enum amd_ip_block_type type, bool user_queue)
{
	amdgpu_context_handle context_handle;
	amdgpu_bo_handle ib_result_handle;
	void *ib_result_cpu;
	uint64_t ib_result_mc_address;
	struct amdgpu_cs_request ibs_request;
	struct amdgpu_cs_ib_info ib_info;
	struct amdgpu_cs_fence fence_status;
	const struct amdgpu_ip_block_version *ip_block = NULL;
	uint32_t *ptr;
	uint32_t expired;
	int r, instance;
	amdgpu_bo_list_handle bo_list;
	amdgpu_va_handle va_handle;
	uint32_t available_rings = 0;
	struct amdgpu_ring_context *ring_context;

	ip_block = get_ip_block(device, type);
	ring_context = calloc(1, sizeof(*ring_context));
	igt_assert(ring_context);

	r = amdgpu_query_hw_ip_info(device, type, 0, &ring_context->hw_ip_info);
	igt_assert_eq(r, 0);

	if (user_queue)
		available_rings = ring_context->hw_ip_info.userq_num_slots ?
			((1 << ring_context->hw_ip_info.userq_num_slots) -1) : 1;
	else
		available_rings = ring_context->hw_ip_info.available_rings;

	if (user_queue) {
		ip_block->funcs->userq_create(device, ring_context, type);
	} else {
		r = amdgpu_cs_ctx_create(device, &context_handle);
		igt_assert_eq(r, 0);
	}

	for (instance = 0; available_rings & (1 << instance); instance++) {
		r = amdgpu_bo_alloc_and_map_sync(device, 4096, 4096,
						 AMDGPU_GEM_DOMAIN_GTT, 0,
						 AMDGPU_VM_MTYPE_UC,
						 &ib_result_handle, (void **)&ib_result_cpu,
						 &ib_result_mc_address, &va_handle,
						 ring_context->timeline_syncobj_handle,
						 ++ring_context->point, user_queue);
		igt_assert_eq(r, 0);

		if (user_queue) {
			r = amdgpu_timeline_syncobj_wait(device,
							 ring_context->timeline_syncobj_handle,
							 ring_context->point);
			igt_assert_eq(r, 0);
		}

		r = amdgpu_get_bo_list(device, ib_result_handle, NULL,
				       &bo_list);
		igt_assert_eq(r, 0);

		ptr = ib_result_cpu;
		memset(ptr, 0, 16);
		if (type == AMDGPU_HW_IP_DMA)
			ptr[0] = SDMA_NOP;
		else
			ptr[0] = PACKET3(PACKET3_NOP, 14);
		ring_context->pm4_dw = 16;

		if (user_queue) {
			ip_block->funcs->userq_submit(device, ring_context, type,
						 ib_result_mc_address);
		} else {
			memset(&ib_info, 0, sizeof(struct amdgpu_cs_ib_info));
			ib_info.ib_mc_address = ib_result_mc_address;
			ib_info.size = 16;

			memset(&ibs_request, 0, sizeof(struct amdgpu_cs_request));
			ibs_request.ip_type = type;
			ibs_request.ring = instance;
			ibs_request.number_of_ibs = 1;
			ibs_request.ibs = &ib_info;
			ibs_request.resources = bo_list;
			ibs_request.fence_info.handle = NULL;

			memset(&fence_status, 0, sizeof(struct amdgpu_cs_fence));
			r = amdgpu_cs_submit(context_handle, 0, &ibs_request, 1);
			igt_assert_eq(r, 0);

			fence_status.context = context_handle;
			fence_status.ip_type = type;
			fence_status.ip_instance = 0;
			fence_status.ring = instance;
			fence_status.fence = ibs_request.seq_no;

			r = amdgpu_cs_query_fence_status(&fence_status,
							 AMDGPU_TIMEOUT_INFINITE,
							 0, &expired);
			igt_assert_eq(r, 0);

			r = amdgpu_bo_list_destroy(bo_list);
			igt_assert_eq(r, 0);
		}
		amdgpu_bo_unmap_and_free(ib_result_handle, va_handle,
					 ib_result_mc_address, 4096);
	}

	if (user_queue) {
		ip_block->funcs->userq_destroy(device, ring_context, type);
	} else {
		r = amdgpu_cs_ctx_free(context_handle);
		igt_assert_eq(r, 0);
	}

	free(ring_context);
}

