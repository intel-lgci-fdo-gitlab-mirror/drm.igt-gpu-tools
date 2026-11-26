/* SPDX-License-Identifier: MIT */
#ifndef AMD_SPM_H
#define AMD_SPM_H

#include <amdgpu.h>

#include <stdbool.h>
#include <stdint.h>

struct amd_spm_capture;
struct amd_spm_cache_profile;
struct amdgpu_cmd_base;
struct drm_amdgpu_info_hw_ip;

struct amd_spm_ops {
	int (*build_layout)(struct amd_spm_capture *capture);
	int (*emit_setup)(struct amdgpu_cmd_base *ib,
			  const struct amd_spm_capture *capture);
	int (*emit_start)(struct amdgpu_cmd_base *ib,
			  const struct amd_spm_capture *capture);
	int (*emit_stop_reset)(struct amdgpu_cmd_base *ib,
			       const struct amd_spm_capture *capture);
};

struct amd_spm_profile {
	const char *name;
	uint32_t timestamp_mux[2];
	uint32_t counter_mux;
	uint32_t muxsel_addr;
	uint32_t muxsel_data;
	uint32_t cpf_select;
	uint32_t pointer_granularity;
	uint32_t data_offset;
	bool windowed_counters;
};

enum amd_spm_cache {
	AMD_SPM_CACHE_L0,
	AMD_SPM_CACHE_L1,
	AMD_SPM_CACHE_L2,
	AMD_SPM_CACHE_INSTRUCTION,
	AMD_SPM_CACHE_SCALAR,
	AMD_SPM_CACHE_COUNT,
};

struct amd_spm_cache_stats {
	uint64_t requests;
	uint64_t hits;
	uint64_t misses;
	uint32_t instances;
	bool saturated;
	bool consistent;
};

struct amd_spm_capture {
	amdgpu_device_handle device;
	amdgpu_context_handle context;
	amdgpu_bo_handle ring_bo;
	amdgpu_va_handle ring_va;
	volatile uint32_t *ring_cpu;
	uint64_t ring_gpu_va;
	uint32_t ring_size;
	uint32_t sample_size;
	uint32_t gfx_major;
	uint32_t gfx_minor;
	struct amd_spm_ops ops;
	struct amd_spm_profile profile;
	struct amd_spm_cache_profile *cache_profile;
	struct amdgpu_cmd_base *ib;
	bool vmid_reserved;
	bool active;
};

bool amd_spm_capture_supported(const struct amdgpu_gpu_info *gpu_info,
			       const struct drm_amdgpu_info_hw_ip *gfx);
int amd_spm_capture_init(struct amd_spm_capture *capture,
			 amdgpu_device_handle device);
int amd_spm_capture_start(struct amd_spm_capture *capture);
int amd_spm_capture_stop(struct amd_spm_capture *capture);
int amd_spm_capture_read(struct amd_spm_capture *capture,
			 uint32_t *num_samples, uint64_t *counter_sum);
void amd_spm_capture_fini(struct amd_spm_capture *capture);

/*
 * GFX10 (Navi1x) only: enable per-cache-block (L0/TCP, L1/GL1C, L2/GL2C,
 * instruction+scalar/SQ) hit-rate counters in place of the single CPF-busy
 * counter. Must be called after amd_spm_capture_init() and before
 * amd_spm_capture_start().
 */
int amd_spm_capture_enable_caches(struct amd_spm_capture *capture);
int amd_spm_capture_read_caches(struct amd_spm_capture *capture,
				uint32_t *num_samples, uint64_t *counter_sum,
				struct amd_spm_cache_stats stats[AMD_SPM_CACHE_COUNT]);

#endif /* AMD_SPM_H */
