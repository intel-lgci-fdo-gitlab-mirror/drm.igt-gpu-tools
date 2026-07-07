// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (C) 2025 Collabora Ltd.

#include "igt.h"
#include "igt_core.h"
#include "igt_panthor.h"
#include "igt_sizes.h"
#include "panthor_drm.h"

int igt_main() {
	int fd = -1;

	igt_fixture() {
		igt_panthor_skip_on_big_endian();
		fd = drm_open_driver(DRIVER_PANTHOR);
	}

	igt_describe("Create and destroy a VM");
	igt_subtest("vm_create_destroy") {
		uint32_t vm_id;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert_neq(vm_id, 0);

		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_subtest("vm_destroy_invalid") {
		igt_panthor_vm_destroy(fd, 0xdeadbeef, EINVAL);
	}

	igt_describe("Test the VM_BIND API synchronously");
	igt_subtest("vm_bind") {
		uint32_t vm_id;
		struct panthor_bo bo;
		uint64_t bo_size = 0x1000;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert_neq(vm_id, 0);

		igt_panthor_bo_create(fd, &bo, bo_size, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, bo.handle,
				    0x1000, 0x1000, DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);

		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Test unbinding a previously bound range");
	igt_subtest("vm_unbind") {
		uint32_t vm_id;
		struct panthor_bo bo;
		uint64_t bo_size = 0x1000;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert_neq(vm_id, 0);

		igt_panthor_bo_create(fd, &bo, bo_size, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, bo.handle,
				    0x1000, 0x1000, DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);
		igt_panthor_vm_bind(fd, vm_id, 0,
				    0x1000, 0x1000, DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);

		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Test unbinding an address range that was not previously bound");
	igt_subtest("vm_unbind_invalid_address") {
		uint32_t vm_id;
		struct panthor_bo bo;
		uint64_t bo_size = 0x1000;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert_neq(vm_id, 0);

		igt_panthor_bo_create(fd, &bo, bo_size, 0, 0);

		/* This was not bound previously*/
		igt_panthor_vm_bind(fd, vm_id, bo.handle,
				    0x1000, 0x1000, DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, EINVAL);
		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Requested unmapping is identical to existing huge page mapping");
	igt_subtest("vm_unbind_identical_hugepage_single") {
		uint32_t vm_id;
		struct panthor_bo bo;
		uint64_t bo_size = SZ_2M;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert(vm_id != 0);

		igt_panthor_bo_create(fd, &bo, bo_size, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, bo.handle, 0x200000, bo_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);

		igt_panthor_vm_bind(fd, vm_id, 0, 0x200000, bo_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);

		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Requested unmapping is identical to existing huge page mapping, but only a subset of the object's pages are mapped in the VM");
	igt_subtest("vm_unbind_identical_hugepage_single_partial") {
		uint32_t vm_id;
		struct panthor_bo bo;
		uint64_t bo_size = SZ_4M;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert(vm_id != 0);

		igt_panthor_bo_create(fd, &bo, bo_size, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, bo.handle, 0x200000, bo_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);

		igt_panthor_vm_bind(fd, vm_id, 0, 0x200000, bo_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);

		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Requested unmapping is identical to existing multiple huge page mapping");
	igt_subtest("vm_unbind_identical_hugepage_multiple") {
		uint32_t vm_id;
		struct panthor_bo bo;
		uint64_t bo_size = SZ_1M * 6;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert(vm_id != 0);

		igt_panthor_bo_create(fd, &bo, bo_size, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, bo.handle, 0x200000, bo_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);

		igt_panthor_vm_bind(fd, vm_id, 0, 0x200000, bo_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);

		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Requested unmapping is identical to existing huge page mapping, but only part of the BO is mapped");
	igt_subtest("vm_unbind_identical_hugepage_offset") {
		uint32_t vm_id;
		struct panthor_bo bo;
		uint64_t bo_size = SZ_1M * 6;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert(vm_id != 0);

		igt_panthor_bo_create(fd, &bo, bo_size, 0, 0);
		igt_panthor_vm_bind_offset(fd, vm_id, bo.handle, 0x200000,
					   SZ_4M, SZ_2M, DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);

		igt_panthor_vm_bind(fd, vm_id, 0, 0x200000, bo_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);

		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Requested unmapping is left aligned subset of an existing huge page mapping");
	igt_subtest("vm_unbind_hugepage_leftaligned") {
		uint32_t vm_id;
		struct panthor_bo bo;
		uint64_t bo_size = SZ_4M;
		uint64_t unmap_size = SZ_8K;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert(vm_id != 0);

		igt_panthor_bo_create(fd, &bo, bo_size, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, bo.handle, 0x200000, bo_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);

		igt_panthor_vm_bind(fd, vm_id, 0, 0x200000, unmap_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);

		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Requested unmapping is right aligned subset of an existing huge page mapping");
	igt_subtest("vm_unbind_hugepage_rightaligned") {
		uint32_t vm_id;
		struct panthor_bo bo;
		uint64_t bo_size = SZ_4M;
		uint64_t unmap_size = SZ_8K;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert(vm_id != 0);

		igt_panthor_bo_create(fd, &bo, bo_size, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, bo.handle, 0x200000, bo_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);

		igt_panthor_vm_bind(fd, vm_id, 0, 0x200000 + bo_size - unmap_size, unmap_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);

		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Requested unmapping is a superset of existing huge page mapping");
	igt_subtest("vm_unbind_hugepage_superset") {
		uint32_t vm_id;
		struct panthor_bo bo;
		uint64_t bo_size = SZ_2M;
		uint64_t unmap_size = SZ_1M * 5;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert(vm_id != 0);

		igt_panthor_bo_create(fd, &bo, bo_size, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, bo.handle, 0x200000, bo_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);

		igt_panthor_vm_bind(fd, vm_id, 0, 0x100000, unmap_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);

		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Requested unmapping is a subset of an existing mapping");
	igt_subtest("vm_unbind_hugepage_subset") {
		uint32_t vm_id;
		struct panthor_bo bo;
		uint64_t bo_size = SZ_4M;
		uint64_t unmap_size = SZ_8K;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert(vm_id != 0);

		igt_panthor_bo_create(fd, &bo, bo_size, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, bo.handle, 0x200000, bo_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);

		igt_panthor_vm_bind(fd, vm_id, 0, 0x200000 + SZ_1M, unmap_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);
		igt_panthor_vm_bind(fd, vm_id, 0, 0x200000 + SZ_2M + SZ_4K, unmap_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);

		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Perform successive unmaps over the remnants of an original multi huge page mapping");
	igt_subtest("vm_unbind_hugepage_successive") {
		uint32_t vm_id;
		struct panthor_bo bo;
		uint64_t bo_size = SZ_1M * 6;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert(vm_id != 0);

		igt_panthor_bo_create(fd, &bo, bo_size, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, bo.handle, 0x200000, bo_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);

		igt_panthor_vm_bind(fd, vm_id, 0, 0x3fc000, 0x208000,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);
		igt_panthor_vm_bind(fd, vm_id, 0, 0x3fc000, 0x208000,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);
		igt_panthor_vm_bind(fd, vm_id, 0, 0x200000, 0x4000,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);
		igt_panthor_vm_bind(fd, vm_id, 0, 0x401000, 0x4000,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);
		igt_panthor_vm_bind(fd, vm_id, 0, 0x4fb000, 0xA000,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);
		igt_panthor_vm_bind(fd, vm_id, 0, 0x3fb000, 0xA000,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);
		igt_panthor_vm_bind(fd, vm_id, 0, 0x3fc000, 0x4000,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);
		igt_panthor_vm_bind(fd, vm_id, 0, 0x200000, 0x1000,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);

		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Perform a vm_bind in the VM's kernel BOs reserved range");
	igt_subtest("vm_bind_intersect_kbo_range") {
		uint32_t vm_id;
		struct panthor_bo bo;
		uint64_t bo_size = SZ_2M;
		uint64_t uva_range;

		igt_panthor_vm_create_userva_range(fd, &vm_id, 0, &uva_range);
		igt_assert(vm_id != 0);

		igt_panthor_bo_create(fd, &bo, bo_size, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, bo.handle, ALIGN(uva_range, bo_size),
				    bo_size, DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, EINVAL);

		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Perform a simple sparse vm_bind operation");
	igt_subtest("vm_bind_sparse") {
		uint32_t vm_id;
		uint64_t map_size = SZ_4K * 4;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert(vm_id != 0);

		igt_panthor_vm_bind_sparse(fd, vm_id, SZ_2M, map_size, 0);
		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Perform a partial hugepage-unaligned unmap of a sparsely bound region");
	igt_subtest("vm_bind_sparse_partial_unmap_start_size_unaligned") {
		uint32_t vm_id;
		uint64_t map_size = SZ_2M * 3;
		const int INITIAL_VA = SZ_4M;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert(vm_id != 0);

		igt_panthor_vm_bind_sparse(fd, vm_id, INITIAL_VA, map_size, 0);
		igt_panthor_vm_bind(fd, vm_id, 0, INITIAL_VA, SZ_4K * 2,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);
		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Do a partial hugeapge-aligned unmap from start of sparsely-bound region");
	igt_subtest("vm_bind_sparse_partial_unmap_start_size_aligned") {
		uint32_t vm_id;
		uint64_t map_size = SZ_2M * 3;
		const int INITIAL_VA = SZ_4M;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert(vm_id != 0);

		igt_panthor_vm_bind_sparse(fd, vm_id, INITIAL_VA, map_size, 0);
		igt_panthor_vm_bind(fd, vm_id, 0, INITIAL_VA, SZ_2M,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);
		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Do a partial hugeapge-unaligned unmap from start of sparsely-bound region");
	igt_subtest("vm_bind_sparse_partial_unmap_start_aligned_no_hugepage_multiple") {
		uint32_t vm_id;
		uint64_t map_size = SZ_2M * 3;
		const int INITIAL_VA = SZ_4M;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert(vm_id != 0);

		igt_panthor_vm_bind_sparse(fd, vm_id, INITIAL_VA, map_size, 0);
		igt_panthor_vm_bind(fd, vm_id, 0, INITIAL_VA, 4 * SZ_64K,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);
		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Do partial hugeapge-aligned unmap that left-intersects with sparse region");
	igt_subtest("vm_bind_sparse_partial_unmap_below_start") {
		uint32_t vm_id;
		uint64_t map_size = SZ_2M * 3;
		const int INITIAL_VA = SZ_4M;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert(vm_id != 0);

		igt_panthor_vm_bind_sparse(fd, vm_id, INITIAL_VA, map_size, 0);
		igt_panthor_vm_bind(fd, vm_id, 0, INITIAL_VA - SZ_2M, SZ_2M * 3,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);
		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Do partial hugepage-aligned unmap that is proper subset of sparse region");
	igt_subtest("vm_bind_sparse_partial_unmap_above_start") {
		uint32_t vm_id;
		uint64_t map_size = SZ_2M * 3;
		const int INITIAL_VA = SZ_4M;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert(vm_id != 0);

		igt_panthor_vm_bind_sparse(fd, vm_id, INITIAL_VA, map_size, 0);
		igt_panthor_vm_bind(fd, vm_id, 0, INITIAL_VA + SZ_2M, SZ_2M,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);
		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Do normal hugepage-aligned bind that intersects with sparsely-mapped region");
	igt_subtest("vm_bind_sparse_remap") {
		uint32_t vm_id;
		struct panthor_bo bo;
		uint64_t map_size = SZ_2M * 3;
		const int INITIAL_VA = SZ_4M;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert(vm_id != 0);

		igt_panthor_vm_bind_sparse(fd, vm_id, INITIAL_VA, map_size, 0);

		/* Now attempt normal VM_BIND's that intersect with the previous chunk */
		igt_panthor_bo_create(fd, &bo, SZ_8M, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, bo.handle, INITIAL_VA + SZ_2M, SZ_2M * 2,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);

		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Do normal hugepage-unaligned bind that intersects with sparsely-mapped region");
	igt_subtest("vm_bind_sparse_remap_start_unaligned") {
		uint32_t vm_id;
		struct panthor_bo bo;
		uint64_t map_size = SZ_2M * 3;
		const int INITIAL_VA = SZ_4M;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert(vm_id != 0);

		igt_panthor_vm_bind_sparse(fd, vm_id, INITIAL_VA, map_size, 0);

		/* Now attempt normal VM_BIND's that intersect with the previous chunk */
		igt_panthor_bo_create(fd, &bo, SZ_8M, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, bo.handle, INITIAL_VA + map_size - SZ_1M,
				    SZ_4M, DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);

		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Do normal hugepage-unaligned bind that intersects with sparsely-mapped region");
	igt_subtest("vm_bind_sparse_remap_start_size_unaligned") {
		uint32_t vm_id;
		struct panthor_bo bo;
		uint64_t map_size = SZ_2M * 3;
		const int INITIAL_VA = SZ_4M;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert(vm_id != 0);

		igt_panthor_vm_bind_sparse(fd, vm_id, INITIAL_VA, map_size, 0);

		/* Now attempt normal VM_BIND's that intersect with the previous chunk */
		igt_panthor_bo_create(fd, &bo, SZ_8M, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, bo.handle, INITIAL_VA + map_size - SZ_1M,
				    SZ_1M, DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);

		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Do normal hugepage-aligned bind that intersects with sparsely-mapped region");
	igt_subtest("vm_bind_sparse_remap_start_size_aligned") {
		uint32_t vm_id;
		struct panthor_bo bo;
		uint64_t map_size = SZ_2M * 3;
		const int INITIAL_VA = SZ_4M;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert(vm_id != 0);

		igt_panthor_vm_bind_sparse(fd, vm_id, INITIAL_VA, map_size, 0);

		/* Now attempt normal VM_BIND's that intersect with the previous chunk */
		igt_panthor_bo_create(fd, &bo, SZ_8M, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, bo.handle,
				    INITIAL_VA + map_size - SZ_2M, SZ_4M,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);

		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Do normal hugepage-aligned bind that splits original region in two");
	igt_subtest("vm_bind_sparse_remap_aligned_split_original_va") {
		uint32_t vm_id;
		struct panthor_bo bo;
		uint64_t map_size = SZ_2M * 3;
		const int INITIAL_VA = SZ_4M;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert(vm_id != 0);

		igt_panthor_vm_bind_sparse(fd, vm_id, INITIAL_VA, map_size, 0);

		/* Now attempt normal VM_BIND's that intersect with the previous chunk */
		igt_panthor_bo_create(fd, &bo, SZ_8M, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, bo.handle, INITIAL_VA + SZ_2M,
				    SZ_2M, DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);

		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Do normal 2MiB aligned hugepage-size-unaligned bind over sparse region");
	igt_subtest("vm_bind_sparse_remap_start_aligned_size_unaligned") {
		uint32_t vm_id;
		struct panthor_bo bo;
		uint64_t map_size = SZ_2M * 3;
		const int INITIAL_VA = SZ_4M;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert(vm_id != 0);

		igt_panthor_vm_bind_sparse(fd, vm_id, INITIAL_VA, map_size, 0);

		/* Now attempt normal VM_BIND's that intersect with the previous chunk */
		igt_panthor_bo_create(fd, &bo, SZ_8M, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, bo.handle, INITIAL_VA + SZ_2M,
				    SZ_1M, DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);

		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Do normal hugepage-aligned bind over left end of sparse region");
	igt_subtest("vm_bind_sparse_remap_aligned_intersect_left") {
		uint32_t vm_id;
		struct panthor_bo bo;
		uint64_t map_size = SZ_2M * 3;
		const int INITIAL_VA = SZ_4M;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert(vm_id != 0);

		igt_panthor_vm_bind_sparse(fd, vm_id, INITIAL_VA, map_size, 0);

		/* Now attempt normal VM_BIND's that intersect with the previous chunk */
		igt_panthor_bo_create(fd, &bo, SZ_8M, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, bo.handle,
				    INITIAL_VA - SZ_2M, SZ_2M * 2,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);

		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Do normal hugepage-unaligned bind over left end of sparse region");
	igt_subtest("vm_bind_sparse_remap_size_unaligned_intersect_left") {
		uint32_t vm_id;
		struct panthor_bo bo;
		uint64_t map_size = SZ_2M * 3;
		const int INITIAL_VA = SZ_4M;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert(vm_id != 0);

		igt_panthor_vm_bind_sparse(fd, vm_id, INITIAL_VA, map_size, 0);

		/* Now attempt normal VM_BIND's that intersect with the previous chunk */
		igt_panthor_bo_create(fd, &bo, SZ_8M, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, bo.handle,
				    INITIAL_VA - SZ_2M, SZ_1M * 3,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);

		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Do normal hugepage-unaligned bind over right end of sparse region");
	igt_subtest("vm_bind_sparse_remap_start_aligned_intersect_right") {
		uint32_t vm_id;
		struct panthor_bo bo;
		uint64_t map_size = SZ_2M * 3;
		const int INITIAL_VA = SZ_4M;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert(vm_id != 0);

		igt_panthor_vm_bind_sparse(fd, vm_id, INITIAL_VA, map_size, 0);

		/* Now attempt normal VM_BIND's that intersect with the previous chunk */
		igt_panthor_bo_create(fd, &bo, SZ_8M, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, bo.handle,
				    INITIAL_VA + map_size - SZ_2M, SZ_2M + SZ_4K * 6,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);

		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Do normal hugepage-aligned bind that envelopes whole sparse region");
	igt_subtest("vm_bind_sparse_remap_wrap_around_va") {
		uint32_t vm_id;
		struct panthor_bo bo;
		uint64_t map_size = SZ_2M * 3;
		const int INITIAL_VA = SZ_4M;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert(vm_id != 0);

		igt_panthor_vm_bind_sparse(fd, vm_id, INITIAL_VA, map_size, 0);

		/* Now attempt normal VM_BIND's that intersect with the previous chunk */
		igt_panthor_bo_create(fd, &bo, SZ_8M, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, bo.handle, INITIAL_VA - SZ_2M,
				    SZ_8M, DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);

		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Perform sparse binding operation whose range causes an overflow");
	igt_subtest("vm_bind_sparse_overflow") {
		uint32_t vm_id;
		uint64_t map_size = ALIGN_DOWN(UINT64_MAX, SZ_2M) - (SZ_2M * 3);
		const int INITIAL_VA = SZ_512M;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert(vm_id != 0);

		igt_panthor_vm_bind_sparse(fd, vm_id, INITIAL_VA, map_size, EINVAL);
		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Perform sparse unbind operation whose range causes an overflow");
	igt_subtest("vm_unbind_sparse_overflow") {
		uint32_t vm_id;
		uint64_t map_size = SZ_2M * 3;
		uint64_t unmap_size = ALIGN_DOWN(UINT64_MAX, SZ_2M) - map_size;
		const int INITIAL_VA = SZ_512M;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert(vm_id != 0);

		igt_panthor_vm_bind_sparse(fd, vm_id, INITIAL_VA, map_size, 0);
		igt_panthor_vm_bind(fd, vm_id, 0, INITIAL_VA, unmap_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, EINVAL);
		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_fixture() {
		drm_close_driver(fd);
	}
}
