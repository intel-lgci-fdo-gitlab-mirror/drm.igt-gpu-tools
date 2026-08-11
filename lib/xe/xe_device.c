// SPDX-License-Identifier: MIT
/*
 * Copyright(c) 2026 Intel Corporation. All rights reserved.
 */

#include "igt.h"
#include "igt_syncobj.h"
#include "xe/xe_device.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"

/**
 * xe_device_exec_healthcheck:
 * @fd: Xe DRM file descriptor
 *
 * Basic check for the Xe execution path.
 * It binds a BO into VM address space, executes a minimal batch,
 * waits for completion, and then unbinds the BO.
 */
void xe_device_exec_healthcheck(int fd)
{
	struct drm_xe_engine_class_instance *hwe = NULL;
	struct drm_xe_sync sync = {
		.type = DRM_XE_SYNC_TYPE_SYNCOBJ,
		.flags = DRM_XE_SYNC_FLAG_SIGNAL,
	};
	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.num_syncs = 1,
		.syncs = to_user_pointer(&sync),
	};
	uint64_t addr = 0x1a0000;
	uint32_t vm;
	uint32_t exec_queue;
	uint32_t bo;
	uint32_t *batch;
	size_t bo_size;

	xe_for_each_engine(fd, hwe)
		break;

	igt_assert(hwe);

	vm = xe_vm_create(fd, 0, 0);
	bo_size = xe_bb_size(fd, sizeof(*batch));
	bo = xe_bo_create(fd, vm, bo_size,
			  vram_if_possible(fd, hwe->gt_id),
			  DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
	batch = xe_bo_map(fd, bo, bo_size);
	exec_queue = xe_exec_queue_create(fd, vm, hwe, 0);
	sync.handle = syncobj_create(fd, 0);

	xe_vm_bind_async(fd, vm, 0, bo, 0, addr, bo_size, &sync, 1);
	igt_assert(syncobj_wait(fd, &sync.handle, 1, INT64_MAX, 0, NULL));

	batch[0] = MI_BATCH_BUFFER_END;

	syncobj_reset(fd, &sync.handle, 1);
	exec.exec_queue_id = exec_queue;
	exec.address = addr;
	xe_exec(fd, &exec);
	igt_assert(syncobj_wait(fd, &sync.handle, 1, INT64_MAX, 0, NULL));

	syncobj_reset(fd, &sync.handle, 1);
	xe_vm_unbind_async(fd, vm, 0, 0, addr, bo_size, &sync, 1);
	igt_assert(syncobj_wait(fd, &sync.handle, 1, INT64_MAX, 0, NULL));

	syncobj_destroy(fd, sync.handle);
	xe_exec_queue_destroy(fd, exec_queue);
	munmap(batch, bo_size);
	gem_close(fd, bo);
	xe_vm_destroy(fd, vm);
}
