// SPDX-License-Identifier: MIT
/*
 * Copyright © 2025 Intel Corporation
 */
#include "igt.h"
#include "igt_perf.h"
#include "igt_sriov_device.h"
#include "igt_syncobj.h"
#include "igt_sysfs.h"
#include "xe_drm.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_spin.h"
#include "xe/xe_sriov_admin.h"
#include "xe/xe_sriov_provisioning.h"

/**
 * TEST: Tests for SR-IOV scheduling parameters.
 * Category: Core
 * Mega feature: SR-IOV
 * Sub-category: scheduling
 * Functionality: vGPU profiles scheduling parameters
 * Description: Verify behavior after modifying scheduling attributes.
 */

enum subm_sync_method { SYNC_NONE, SYNC_BARRIER };

struct subm_opts {
	enum subm_sync_method sync_method;
	uint32_t exec_quantum_ms;
	uint32_t preempt_timeout_us;
	double outlier_treshold;
	/* --inflight=0 => auto; >=1 => explicit K */
	unsigned int inflight;
};

struct subm_work_desc {
	uint64_t duration_ms;
	bool preempt;
	unsigned int repeats;
};

struct subm_stats {
	igt_stats_t samples;
	uint64_t start_timestamp;
	uint64_t end_timestamp;
	uint64_t *complete_ts; /* absolute completion timestamps (ns) */
	unsigned int num_early_finish;
	unsigned int concurrent_execs;
	double concurrent_rate;
	double concurrent_mean;
};

struct subm {
	char id[32];
	int fd;
	int vf_num;
	struct subm_work_desc work;
	uint32_t expected_ticks;
	uint32_t vm;
	struct drm_xe_engine_class_instance hwe;
	uint32_t exec_queue_id;
	/* K slots (K BOs / addresses / mapped spinners / done fences / submit timestamps) */
	unsigned int slots;
	uint64_t *addr;
	uint32_t *bo;
	size_t bo_size;
	struct xe_spin **spin;
	uint32_t *done_fence;
	uint64_t *submit_ts;
	struct drm_xe_sync sync[1];
	struct drm_xe_exec exec;
};

struct subm_thread_data {
	struct subm subm;
	struct subm_stats stats;
	const struct subm_opts *opts;
	pthread_t thread;
	pthread_barrier_t *barrier;
};

struct subm_set {
	struct subm_thread_data *data;
	int ndata;
	enum subm_sync_method sync_method;
	pthread_barrier_t barrier;
};

static uint64_t current_timestamp_ns(void)
{
	struct timespec tv;

	igt_gettime(&tv);

	return tv.tv_sec * (uint64_t)NSEC_PER_SEC + (uint64_t)tv.tv_nsec;
}

static void subm_init(struct subm *s, int fd, int vf_num, uint64_t addr,
		      struct drm_xe_engine_class_instance hwe,
		      unsigned int inflight)
{
	uint64_t base, stride;

	memset(s, 0, sizeof(*s));
	s->fd = fd;
	s->vf_num = vf_num;
	s->hwe = hwe;
	snprintf(s->id, sizeof(s->id), "VF%d %d:%d:%d", vf_num,
		 hwe.engine_class, hwe.engine_instance, hwe.gt_id);
	s->slots = inflight ? inflight : 1;
	s->vm = xe_vm_create(s->fd, 0, 0);
	s->exec_queue_id = xe_exec_queue_create(s->fd, s->vm, &s->hwe, 0);
	s->bo_size = ALIGN(sizeof(struct xe_spin) + xe_cs_prefetch_size(s->fd),
			   xe_get_default_alignment(s->fd));
	s->addr = calloc(s->slots, sizeof(*s->addr));
	s->bo = calloc(s->slots, sizeof(*s->bo));
	s->spin = calloc(s->slots, sizeof(*s->spin));
	s->done_fence = calloc(s->slots, sizeof(*s->done_fence));
	s->submit_ts = calloc(s->slots, sizeof(*s->submit_ts));

	igt_assert(s->addr && s->bo && s->spin && s->done_fence && s->submit_ts);

	base = addr ? addr : 0x1a0000;
	stride = ALIGN(s->bo_size, 0x10000);
	for (unsigned int i = 0; i < s->slots; i++) {
		s->addr[i] = base + i * stride;
		s->bo[i] = xe_bo_create(s->fd, s->vm, s->bo_size,
					vram_if_possible(fd, s->hwe.gt_id),
					DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
		s->spin[i] = xe_bo_map(s->fd, s->bo[i], s->bo_size);
		xe_vm_bind_sync(s->fd, s->vm, s->bo[i], 0, s->addr[i], s->bo_size);
		s->done_fence[i] = syncobj_create(s->fd, 0);
	}

	s->exec.num_batch_buffer = 1;
	s->exec.exec_queue_id = s->exec_queue_id;
	/* s->exec.address set per submission */
}

static void subm_fini(struct subm *s)
{
	for (unsigned int i = 0; i < s->slots; i++) {
		xe_vm_unbind_sync(s->fd, s->vm, 0, s->addr[i], s->bo_size);
		gem_munmap(s->spin[i], s->bo_size);
		gem_close(s->fd, s->bo[i]);
		if (s->done_fence[i])
			syncobj_destroy(s->fd, s->done_fence[i]);
	}
	xe_exec_queue_destroy(s->fd, s->exec_queue_id);
	xe_vm_destroy(s->fd, s->vm);
	free(s->addr);
	free(s->bo);
	free(s->spin);
	free(s->done_fence);
	free(s->submit_ts);
}

static void subm_workload_init(struct subm *s, struct subm_work_desc *work)
{
	s->work = *work;

	s->expected_ticks = xe_spin_nsec_to_ticks(s->fd, s->hwe.gt_id,
						  s->work.duration_ms * 1000000);
	for (unsigned int i = 0; i < s->slots; i++)
		xe_spin_init_opts(s->spin[i], .addr = s->addr[i],
				  .preempt = s->work.preempt,
				  .ctx_ticks = s->expected_ticks);
}

static void subm_wait_slot(struct subm *s, unsigned int slot, uint64_t abs_timeout_nsec)
{
	igt_assert(syncobj_wait(s->fd, &s->done_fence[slot], 1,
				abs_timeout_nsec, 0, NULL));
}

static void subm_exec_slot(struct subm *s, unsigned int slot)
{
	syncobj_reset(s->fd, &s->done_fence[slot], 1);
	memset(&s->sync[0], 0, sizeof(s->sync));
	s->sync[0].type = DRM_XE_SYNC_TYPE_SYNCOBJ;
	s->sync[0].flags = DRM_XE_SYNC_FLAG_SIGNAL;
	s->sync[0].handle = s->done_fence[slot];
	s->exec.num_syncs = 1;
	s->exec.syncs = to_user_pointer(&s->sync[0]);
	s->exec.address = s->addr[slot];
	s->submit_ts[slot] = current_timestamp_ns();
	xe_exec(s->fd, &s->exec);
}

static bool subm_is_work_complete(struct subm *s, unsigned int slot)
{
	return s->expected_ticks <= s->spin[slot]->ticks_delta;
}

static bool subm_is_exec_queue_banned(struct subm *s)
{
	struct drm_xe_exec_queue_get_property args = {
		.exec_queue_id = s->exec_queue_id,
		.property = DRM_XE_EXEC_QUEUE_GET_PROPERTY_BAN,
	};
	int ret = igt_ioctl(s->fd, DRM_IOCTL_XE_EXEC_QUEUE_GET_PROPERTY, &args);

	return ret || args.value;
}

static void subm_exec_loop(struct subm *s, struct subm_stats *stats,
			   const struct subm_opts *opts)
{
	const unsigned int inflight = s->slots;
	unsigned int submitted = 0;
	unsigned int i;

	stats->start_timestamp = current_timestamp_ns();
	igt_debug("[%s] start_timestamp: %f\n", s->id, stats->start_timestamp * 1e-9);

	/* Prefill */
	if (s->work.repeats) {
		unsigned int can_prefill = min(inflight, s->work.repeats);

		for (i = 0; i < can_prefill; i++)
			subm_exec_slot(s, i % inflight);
		submitted = can_prefill;
	}

	/* Process completions in order: sample i -> slot (i % inflight) */
	for (i = 0; i < s->work.repeats; ++i) {
		unsigned int slot = i % inflight;

		subm_wait_slot(s, slot, INT64_MAX);
		stats->complete_ts[i] = current_timestamp_ns();
		igt_stats_push(&stats->samples, stats->complete_ts[i] - s->submit_ts[slot]);

		if (!subm_is_work_complete(s, slot)) {
			stats->num_early_finish++;

			igt_debug("[%s] subm #%d early_finish=%u\n",
				  s->id, i, stats->num_early_finish);

			if (subm_is_exec_queue_banned(s))
				break;
		}

		/* Keep the pipeline full */
		if (submitted < s->work.repeats) {
			unsigned int next_slot = submitted % inflight;

			subm_exec_slot(s, next_slot);
			submitted++;
		}
	}

	stats->end_timestamp = current_timestamp_ns();
	igt_debug("[%s] end_timestamp: %f\n", s->id, stats->end_timestamp * 1e-9);
}

static void *subm_thread(void *thread_data)
{
	struct subm_thread_data *td = thread_data;
	struct timespec tv;

	igt_gettime(&tv);
	igt_debug("[%s] thread started %ld.%ld\n", td->subm.id, tv.tv_sec,
		  tv.tv_nsec);

	if (td->barrier)
		pthread_barrier_wait(td->barrier);

	subm_exec_loop(&td->subm, &td->stats, td->opts);

	return NULL;
}

static void subm_set_dispatch_and_wait_threads(struct subm_set *set)
{
	int i;

	for (i = 0; i < set->ndata; ++i)
		igt_assert_eq(0, pthread_create(&set->data[i].thread, NULL,
						subm_thread, &set->data[i]));

	for (i = 0; i < set->ndata; ++i)
		pthread_join(set->data[i].thread, NULL);
}

static void subm_set_alloc_data(struct subm_set *set, unsigned int ndata)
{
	igt_assert(!set->data);
	set->ndata = ndata;
	set->data = calloc(set->ndata, sizeof(struct subm_thread_data));
	igt_assert(set->data);
}

static void subm_set_free_data(struct subm_set *set)
{
	free(set->data);
	set->data = NULL;
	set->ndata = 0;
}

static void subm_set_init_sync_method(struct subm_set *set, enum subm_sync_method sm)
{
	set->sync_method = sm;
	if (set->sync_method == SYNC_BARRIER)
		pthread_barrier_init(&set->barrier, NULL, set->ndata);
}

static void subm_set_close_handles(struct subm_set *set)
{
	struct subm *s;
	int i;

	if (!set->ndata)
		return;

	for (i = 0; i < set->ndata; ++i) {
		s = &set->data[i].subm;

		if (s->fd != -1) {
			subm_fini(s);
			drm_close_driver(s->fd);
			s->fd = -1;
		}
	}
}

static void subm_set_fini(struct subm_set *set)
{
	int i;

	if (!set->ndata)
		return;

	if (set->sync_method == SYNC_BARRIER)
		pthread_barrier_destroy(&set->barrier);

	subm_set_close_handles(set);

	for (i = 0; i < set->ndata; ++i) {
		igt_stats_fini(&set->data[i].stats.samples);
		free(set->data[i].stats.complete_ts);
	}

	subm_set_free_data(set);
}

struct init_vf_ids_opts {
	bool shuffle;
	bool shuffle_pf;
};

static void init_vf_ids(uint8_t *array, size_t n,
			const struct init_vf_ids_opts *opts)
{
	size_t i, j;

	if (!opts->shuffle_pf && n) {
		array[0] = 0;
		n -= 1;
		array = array + 1;
	}

	for (i = 0; i < n; i++) {
		j = (opts->shuffle) ? rand() % (i + 1) : i;

		if (j != i)
			array[i] = array[j];

		array[j] = i + (opts->shuffle_pf ? 0 : 1);
	}
}

static bool check_within_epsilon(const double x, const double ref, const double tol)
{
	return x <= (1.0 + tol) * ref && x >= (1.0 - tol) * ref;
}

static void compute_common_time_frame_stats(struct subm_set *set)
{
	struct subm_thread_data *data = set->data;
	int i, j, ndata = set->ndata;
	struct subm_stats *stats;
	uint64_t common_start = 0;
	uint64_t common_end = UINT64_MAX;
	uint64_t first_ts, last_ts;

	/* Find common window from completion timestamps */
	for (i = 0; i < ndata; i++) {
		stats = &data[i].stats;

		if (!stats->samples.n_values)
			continue;

		first_ts = stats->complete_ts[0];
		last_ts = stats->complete_ts[stats->samples.n_values - 1];

		if (first_ts > common_start)
			common_start = first_ts;
		if (last_ts < common_end)
			common_end = last_ts;
	}

	igt_info("common time frame: [%" PRIu64 ";%" PRIu64 "] %.2fms\n",
		 common_start, common_end, (common_end - common_start) / 1e6);

	if (igt_warn_on_f(common_end <= common_start, "No common time frame for all sets found\n"))
		return;

	/* Compute concurrent_rate for each sample set within the common time frame */
	for (i = 0; i < ndata; i++) {
		const double window_s = (common_end - common_start) * 1e-9;

		stats = &data[i].stats;
		stats->concurrent_execs = 0;
		stats->concurrent_rate = 0.0;
		stats->concurrent_mean = 0.0;

		for (j = 0; j < stats->samples.n_values; j++) {
			uint64_t cts = stats->complete_ts[j];

			if (cts >= common_start && cts <= common_end) {
				stats->concurrent_execs++;
				stats->concurrent_mean += stats->samples.values_u64[j];
			}
		}

		stats->concurrent_rate = (window_s > 0.0) ?
					 ((double)stats->concurrent_execs / window_s) : 0.0;
		stats->concurrent_mean = stats->concurrent_execs ?
					 (double)stats->concurrent_mean /
					 stats->concurrent_execs : 0.0;
		igt_info("[%s] Throughput = %.4f execs/s mean submit->signal latency=%.4fms nsamples=%d\n",
			 data[i].subm.id, stats->concurrent_rate, stats->concurrent_mean * 1e-6,
			 stats->concurrent_execs);
	}
}

#define MIN_NUM_REPEATS 25
#define MIN_EXEC_QUANTUM_MS 2
#define MAX_EXEC_QUANTUM_MS 32
#define MIN_JOB_DURATION_MS 2
#define MAX_TOTAL_DURATION_MS 15000
#define PREFERRED_TOTAL_DURATION_MS 10000
#define MAX_PREFERRED_REPEATS 100

struct job_sched_params {
	int duration_ms;
	int num_repeats;
	struct xe_sriov_sched_params sched_params;
};

static uint32_t sysfs_get_job_timeout_ms(int fd, const struct drm_xe_engine_class_instance *eci)
{
	int engine_dir;
	uint32_t ret;

	engine_dir = xe_sysfs_engine_open(fd, eci->gt_id, eci->engine_class);
	ret = igt_sysfs_get_u32(engine_dir, "job_timeout_ms");
	close(engine_dir);

	return ret;
}

static uint32_t derive_preempt_timeout_us(const uint32_t exec_quantum_ms)
{
	return exec_quantum_ms * 2 * USEC_PER_MSEC;
}

static int calculate_job_duration_ms(int execution_ms)
{
	return execution_ms * 2 > MIN_JOB_DURATION_MS ? execution_ms * 2 :
							MIN_JOB_DURATION_MS;
}

static bool compute_max_exec_quantum_ms(uint32_t *exec_quantum_ms,
					int num_threads,
					int min_num_repeats,
					int job_timeout_ms)
{
	for (int test_execution_ms = MAX_EXEC_QUANTUM_MS;
	     test_execution_ms >= MIN_EXEC_QUANTUM_MS; test_execution_ms--) {
		int test_duration_ms =
			calculate_job_duration_ms(test_execution_ms);
		int max_delay_ms = (num_threads - 1) * test_execution_ms;

		/*
		 * Check if the job can complete within job_timeout_ms,
		 * including the maximum scheduling delay
		 */
		if (test_duration_ms + max_delay_ms <= job_timeout_ms) {
			int estimated_num_repeats =
				MAX_TOTAL_DURATION_MS /
				(num_threads * test_duration_ms);

			if (estimated_num_repeats >= min_num_repeats) {
				*exec_quantum_ms = test_execution_ms;
				return true;
			}
		}
	}
	return false;
}

static int adjust_num_repeats(int duration_ms, int num_threads)
{
	int preferred_max_repeats = PREFERRED_TOTAL_DURATION_MS /
				    (num_threads * duration_ms);
	int optimal_repeats = min(preferred_max_repeats, MAX_PREFERRED_REPEATS);

	return max(optimal_repeats, MIN_NUM_REPEATS);
}

/* inflight K selection:
 *   user_k == 0  => auto
 *   user_k >= 1  => explicit K
 */
static unsigned int select_inflight_k(unsigned int duration_ms,
				      unsigned int user_k,
				      bool nonpreempt)
{
	if (user_k)
		return user_k >= 1 ? user_k : 1;
	if (nonpreempt)
		return 1;
	if (duration_ms <= 12)
		return 4;
	if (duration_ms <= 20)
		return 3;
	return 2;
}

static struct xe_sriov_sched_params prepare_vf_sched_params(int num_threads,
							    int min_num_repeats,
							    int job_timeout_ms,
							    const struct subm_opts *opts,
							    enum xe_sriov_sched_priority priority)
{
	struct xe_sriov_sched_params params = {
		.exec_quantum_ms = MIN_EXEC_QUANTUM_MS,
		.preempt_timeout_us = derive_preempt_timeout_us(MIN_EXEC_QUANTUM_MS),
		.priority = priority,
	};

	if (opts->exec_quantum_ms || opts->preempt_timeout_us) {
		if (opts->exec_quantum_ms)
			params.exec_quantum_ms = opts->exec_quantum_ms;
		if (opts->preempt_timeout_us)
			params.preempt_timeout_us = opts->preempt_timeout_us;
	} else {
		if (igt_debug_on(!compute_max_exec_quantum_ms(&params.exec_quantum_ms,
							      num_threads,
							      min_num_repeats,
							      job_timeout_ms)))
			return params;

		/*
		 * After computing a feasible max_exec_quantum_ms,
		 * select a random exec_quantum_ms within the new range
		 */
		params.exec_quantum_ms = MIN_EXEC_QUANTUM_MS +
					 rand() % (params.exec_quantum_ms -
						   MIN_EXEC_QUANTUM_MS + 1);
		params.preempt_timeout_us = derive_preempt_timeout_us(params.exec_quantum_ms);
	}

	return params;
}

static struct job_sched_params
prepare_job_sched_params(int num_threads, int job_timeout_ms, const struct subm_opts *opts,
			 enum xe_sriov_sched_priority priority)
{
	struct job_sched_params params = { };

	params.sched_params = prepare_vf_sched_params(num_threads, MIN_NUM_REPEATS,
						      job_timeout_ms, opts, priority);
	params.duration_ms = calculate_job_duration_ms(params.sched_params.exec_quantum_ms);
	params.num_repeats = adjust_num_repeats(params.duration_ms, num_threads);

	return params;
}

struct vf_config {
	unsigned int vf_id;
	struct xe_sriov_sched_params sched_params;
	bool run_workload;
};

struct runtime_metrics {
	unsigned int vf_id;
	uint64_t engine_active_ticks;
	uint64_t engine_total_ticks;
	double measured_total_tick_share;
	double active_to_total_ratio;
	double measured_throughput_share;
	double expected_throughput_share;
	double expected_active_to_total_ratio;
	double expected_total_tick_share;
};

struct perf_counters {
	int pmu_fd[2];
	uint64_t before[2];
	uint64_t after[2];
};

static char perf_device[NAME_MAX];

static bool has_perf_event(const char *device, const char *event)
{
	char path[512];

	snprintf(path, sizeof(path),
		 "/sys/bus/event_source/devices/%s/events/%s",
		 device, event);

	return access(path, F_OK) == 0;
}

static int perf_open_group(int xe, uint64_t config, int group)
{
	int fd;

	fd = igt_perf_open_group(xe_perf_type_id(xe), config, group);
	igt_skip_on(fd < 0 && errno == ENODEV);
	igt_assert_fd(fd);

	return fd;
}

static uint64_t perf_read_values(int fd, unsigned int num, uint64_t *val)
{
	uint64_t buf[2 + num];
	unsigned int i;

	igt_assert_eq(read(fd, buf, sizeof(buf)), sizeof(buf));

	for (i = 0; i < num; i++)
		val[i] = buf[2 + i];

	return buf[1];
}

static uint64_t perf_add_format_config(const char *format, uint64_t val)
{
	uint64_t config;
	uint32_t shift;
	int ret;

	ret = perf_event_format(perf_device, format, &shift);
	igt_assert(ret >= 0);
	config = val << shift;

	return config;
}

static uint64_t perf_get_event_config(unsigned int gt,
				      const struct drm_xe_engine_class_instance *eci,
				      const char *event)
{
	uint64_t perf_config = 0;
	int ret;

	ret = perf_event_config(perf_device, event, &perf_config);
	igt_assert(ret >= 0);
	perf_config |= perf_add_format_config("gt", gt);

	if (eci) {
		perf_config |= perf_add_format_config("engine_class", eci->engine_class);
		perf_config |= perf_add_format_config("engine_instance", eci->engine_instance);
	}

	return perf_config;
}

static uint64_t perf_get_event_config_vf(unsigned int gt, unsigned int vf_id,
					 const struct drm_xe_engine_class_instance *eci,
					 const char *event)
{
	return perf_get_event_config(gt, eci, event) |
	       perf_add_format_config("function", vf_id);
}

static double expected_total_tick_weight(const struct vf_config *config)
{
	return config->sched_params.exec_quantum_ms;
}

static double expected_throughput_weight(const struct vf_config *config)
{
	if (!config->run_workload)
		return 0.0;

	return config->sched_params.exec_quantum_ms;
}

static void init_perf_counters(int pf_fd,
			       const struct drm_xe_engine_class_instance *eci,
			       const struct vf_config *configs,
			       size_t num_configs,
			       struct perf_counters *counters)
{
	int leader = -1;

	for (size_t i = 0; i < num_configs; i++) {
		uint64_t config;

		counters[i].pmu_fd[0] = -1;
		counters[i].pmu_fd[1] = -1;

		config = perf_get_event_config_vf(eci->gt_id, configs[i].vf_id, eci,
						  "engine-active-ticks");
		counters[i].pmu_fd[0] = perf_open_group(pf_fd, config, leader);
		if (leader < 0)
			leader = counters[i].pmu_fd[0];

		config = perf_get_event_config_vf(eci->gt_id, configs[i].vf_id, eci,
						  "engine-total-ticks");
		counters[i].pmu_fd[1] = perf_open_group(pf_fd, config, leader);
	}
}

static void fini_perf_counters(struct perf_counters *counters,
			       size_t num_configs)
{
	for (size_t i = 0; i < num_configs; i++) {
		if (counters[i].pmu_fd[0] >= 0)
			close(counters[i].pmu_fd[0]);
		if (counters[i].pmu_fd[1] >= 0)
			close(counters[i].pmu_fd[1]);
	}
}

static uint64_t start_perf_counters(struct perf_counters *counters,
				    size_t num_configs)
{
	uint64_t values[2 * num_configs];

	perf_read_values(counters[0].pmu_fd[0], 2 * num_configs, values);

	for (size_t i = 0; i < num_configs; i++) {
		counters[i].before[0] = values[2 * i];
		counters[i].before[1] = values[2 * i + 1];
	}

	return current_timestamp_ns();
}

static void
init_expected_runtime_metrics(const struct vf_config *configs,
			      size_t num_configs,
			      struct runtime_metrics *metrics)
{
	double total_expected_total_tick_weight = 0.0;
	double total_expected_throughput_weight = 0.0;

	for (size_t i = 0; i < num_configs; i++) {
		total_expected_total_tick_weight += expected_total_tick_weight(&configs[i]);
		total_expected_throughput_weight += expected_throughput_weight(&configs[i]);
	}

	for (size_t i = 0; i < num_configs; i++) {
		metrics[i].vf_id = configs[i].vf_id;
		metrics[i].expected_throughput_share = total_expected_throughput_weight ?
			expected_throughput_weight(&configs[i]) /
			total_expected_throughput_weight : 0.0;
		metrics[i].expected_total_tick_share = total_expected_total_tick_weight ?
			expected_total_tick_weight(&configs[i]) /
			total_expected_total_tick_weight : 0.0;
		metrics[i].expected_active_to_total_ratio =
			configs[i].sched_params.priority == XE_SRIOV_SCHED_PRIORITY_NORMAL ?
				(configs[i].run_workload ? 1.0 : 0.0) :
				(metrics[i].expected_total_tick_share ?
				 metrics[i].expected_throughput_share /
				 metrics[i].expected_total_tick_share : 0.0);
	}
}

static void compute_perf_metrics(const struct vf_config *configs,
				 const struct perf_counters *counters,
				 size_t num_configs,
				 struct runtime_metrics *metrics)
{
	uint64_t total_engine_total_ticks = 0;

	init_expected_runtime_metrics(configs, num_configs, metrics);

	for (size_t i = 0; i < num_configs; i++) {
		metrics[i].engine_active_ticks = counters[i].after[0] - counters[i].before[0];
		metrics[i].engine_total_ticks = counters[i].after[1] - counters[i].before[1];
		total_engine_total_ticks += metrics[i].engine_total_ticks;
	}

	for (size_t i = 0; i < num_configs; i++) {
		metrics[i].active_to_total_ratio = metrics[i].engine_total_ticks ?
			(double)metrics[i].engine_active_ticks /
			metrics[i].engine_total_ticks : 0.0;
		metrics[i].measured_total_tick_share = total_engine_total_ticks ?
			(double)metrics[i].engine_total_ticks /
			total_engine_total_ticks : 0.0;

		igt_info("%s actual={active_ticks=%" PRIu64 ",total_ticks=%" PRIu64
			 ",active/total=%.4f,total_share=%.4f} "
			 "expected={active/total=%.4f,total_share=%.4f,throughput_share=%.4f}\n",
			 igt_sriov_func_str(configs[i].vf_id),
			 metrics[i].engine_active_ticks, metrics[i].engine_total_ticks,
			 metrics[i].active_to_total_ratio,
			 metrics[i].measured_total_tick_share,
			 metrics[i].expected_active_to_total_ratio,
			 metrics[i].expected_total_tick_share,
			 metrics[i].expected_throughput_share);
	}
}

static uint64_t stop_perf_counters(const struct vf_config *configs,
				   struct perf_counters *counters,
				   size_t num_configs,
				   struct runtime_metrics *metrics)
{
	uint64_t end_timestamp = current_timestamp_ns();
	uint64_t values[2 * num_configs];

	perf_read_values(counters[0].pmu_fd[0], 2 * num_configs, values);

	for (size_t i = 0; i < num_configs; i++) {
		counters[i].after[0] = values[2 * i];
		counters[i].after[1] = values[2 * i + 1];
	}

	compute_perf_metrics(configs, counters, num_configs, metrics);

	return end_timestamp;
}

static void init_vf_configs_from_set(int pf_fd,
				     const struct subm_set *set,
				     struct vf_config *configs)
{
	const struct drm_xe_engine_class_instance *eci = &set->data[0].subm.hwe;

	for (int n = 0; n < set->ndata; ++n) {
		const struct subm *subm = &set->data[n].subm;
		const int vf_num = subm->vf_num;

		igt_assert_eq(subm->hwe.gt_id, eci->gt_id);
		igt_assert_eq(subm->hwe.engine_class, eci->engine_class);
		igt_assert_eq(subm->hwe.engine_instance, eci->engine_instance);
		configs[n] = (struct vf_config) {
			.vf_id = vf_num,
			.sched_params = {
				.exec_quantum_ms = xe_sriov_admin_get_exec_quantum_ms(pf_fd,
								      vf_num),
				.preempt_timeout_us = xe_sriov_admin_get_preempt_timeout_us(pf_fd,
									    vf_num),
				.priority = xe_sriov_admin_get_sched_priority(pf_fd, vf_num, NULL),
			},
			.run_workload = true,
		};
	}
}

static unsigned int count_window_completions(const struct subm_stats *stats,
					     uint64_t window_start,
					     uint64_t window_end)
{
	unsigned int completions = 0;

	for (int i = 0; i < stats->samples.n_values; i++) {
		uint64_t cts = stats->complete_ts[i];

		if (cts >= window_start && cts <= window_end)
			completions++;
	}

	return completions;
}

static void compute_measured_throughput_share(const struct subm_set *set,
					      uint64_t window_start,
					      uint64_t window_end,
					      struct runtime_metrics *metrics)
{
	unsigned int completions[set->ndata];
	unsigned int total_completions = 0;

	for (int n = 0; n < set->ndata; ++n) {
		completions[n] = count_window_completions(&set->data[n].stats,
							  window_start,
							  window_end);
		total_completions += completions[n];
	}

	for (int n = 0; n < set->ndata; ++n) {
		metrics[n].measured_throughput_share = total_completions ?
			(double)completions[n] / total_completions : 0.0;
	}
}

static void compute_concurrent_rate_share(const struct subm_set *set,
					  struct runtime_metrics *metrics)
{
	double total_rate = 0.0;

	for (int n = 0; n < set->ndata; ++n)
		total_rate += set->data[n].stats.concurrent_rate;

	for (int n = 0; n < set->ndata; ++n) {
		metrics[n].measured_throughput_share = total_rate ?
			set->data[n].stats.concurrent_rate / total_rate : 0.0;
	}
}

static void run_subm_set_and_collect_metrics(int pf_fd,
					     struct subm_set *set,
					     struct vf_config *vf_configs,
					     struct runtime_metrics *metrics,
					     bool verify_with_perf_counters,
					     uint64_t *measurement_start_ns,
					     uint64_t *measurement_end_ns)
{
	struct perf_counters *perf_counters = NULL;
	const struct drm_xe_engine_class_instance *eci = &set->data[0].subm.hwe;

	init_vf_configs_from_set(pf_fd, set, vf_configs);
	init_expected_runtime_metrics(vf_configs, set->ndata, metrics);

	if (verify_with_perf_counters) {
		perf_counters = calloc(set->ndata, sizeof(*perf_counters));
		igt_assert(perf_counters);
		init_perf_counters(pf_fd, eci,
				   vf_configs, set->ndata, perf_counters);
		*measurement_start_ns = start_perf_counters(perf_counters,
							    set->ndata);
	}

	subm_set_dispatch_and_wait_threads(set);

	if (verify_with_perf_counters) {
		*measurement_end_ns = stop_perf_counters(vf_configs,
							 perf_counters,
							 set->ndata,
							 metrics);
		fini_perf_counters(perf_counters, set->ndata);
		free(perf_counters);
	}

	subm_set_close_handles(set);
	compute_common_time_frame_stats(set);
	compute_concurrent_rate_share(set, metrics);

	if (verify_with_perf_counters)
		compute_measured_throughput_share(set,
						  *measurement_start_ns,
						  *measurement_end_ns,
						  metrics);
}

static void assert_share_matches_expected(const struct runtime_metrics *metrics,
					  size_t num_metrics,
					  double tolerance,
					  bool use_total_tick_share)
{
	for (size_t n = 0; n < num_metrics; ++n) {
		double measured_share = use_total_tick_share ?
			metrics[n].measured_total_tick_share :
			metrics[n].measured_throughput_share;
		double expected_share = use_total_tick_share ?
			metrics[n].expected_total_tick_share :
			metrics[n].expected_throughput_share;
		const char *share_name = use_total_tick_share ?
			"total tick share" : "throughput share";

		igt_assert_f(check_within_epsilon(measured_share,
						  expected_share,
						  tolerance),
			     "%s=%0.4f not within +-%.0f%% of expected=%0.4f for %s\n",
			     share_name, measured_share, tolerance * 100,
			     expected_share,
			     igt_sriov_func_str(metrics[n].vf_id));
	}
}

static void assert_throughput_share_matches_total_tick(const struct runtime_metrics *metrics,
						       size_t num_metrics,
						       double tolerance)
{
	for (size_t n = 0; n < num_metrics; ++n) {
		double expected_ratio = metrics[n].expected_total_tick_share ?
			metrics[n].expected_throughput_share /
			metrics[n].expected_total_tick_share : 0.0;
		double expected_throughput_share =
			metrics[n].measured_total_tick_share * expected_ratio;

		assert_within_epsilon(metrics[n].measured_throughput_share,
				      expected_throughput_share,
				      tolerance);
	}
}

static void warn_active_ticks_mismatch(const struct runtime_metrics *metrics,
				       size_t num_metrics,
				       double tolerance)
{
	for (size_t n = 0; n < num_metrics; ++n) {
		igt_warn_on_f(!check_within_epsilon(metrics[n].active_to_total_ratio,
						    metrics[n].expected_active_to_total_ratio,
						    tolerance),
			      "active/total=%0.4f not within +-%.0f%% of expected=%0.4f for %s\n",
			      metrics[n].active_to_total_ratio, tolerance * 100,
			      metrics[n].expected_active_to_total_ratio,
			      igt_sriov_func_str(metrics[n].vf_id));
	}
}

static void verify_applied_scheduling_behavior(int pf_fd,
					       const uint8_t *vf_ids,
					       size_t num_functions,
					       const struct subm_opts *opts,
					       bool verify_with_perf_counters,
					       const struct drm_xe_engine_class_instance *eci,
					       const struct job_sched_params *job_sched_params)
{
	struct subm_set set_ = {}, *set = &set_;
	struct vf_config *vf_configs = NULL;
	struct runtime_metrics *metrics = NULL;
	uint64_t measurement_start_ns = 0;
	uint64_t measurement_end_ns = 0;
	uint32_t job_timeout_ms = sysfs_get_job_timeout_ms(pf_fd, eci);
	unsigned int k = select_inflight_k(job_sched_params->duration_ms,
					   opts->inflight, false);

	igt_info("eq=%ums pt=%uus prio=%s duration=%ums repeats=%d inflight=%u num_functions=%zu job_timeout=%ums\n",
		 job_sched_params->sched_params.exec_quantum_ms,
		 job_sched_params->sched_params.preempt_timeout_us,
		 xe_sriov_sched_priority_to_string(job_sched_params->sched_params.priority),
		 job_sched_params->duration_ms, job_sched_params->num_repeats,
		 k, num_functions, job_timeout_ms);

	subm_set_alloc_data(set, num_functions);
	subm_set_init_sync_method(set, opts->sync_method);
	vf_configs = calloc(set->ndata, sizeof(*vf_configs));
	metrics = calloc(set->ndata, sizeof(*metrics));
	igt_assert(vf_configs && metrics);

	for (int n = 0; n < set->ndata; ++n) {
		int vf_fd =
			vf_ids[n] ?
				igt_sriov_open_vf_drm_device(pf_fd, vf_ids[n]) :
				drm_reopen_driver(pf_fd);

		igt_assert_fd(vf_fd);
		set->data[n].opts = opts;
		subm_init(&set->data[n].subm, vf_fd, vf_ids[n], 0, *eci, k);
		subm_workload_init(&set->data[n].subm,
				   &(struct subm_work_desc){
					.duration_ms = job_sched_params->duration_ms,
					.preempt = true,
					.repeats = job_sched_params->num_repeats });
		igt_stats_init_with_size(&set->data[n].stats.samples,
					 set->data[n].subm.work.repeats);
		set->data[n].stats.complete_ts = calloc(set->data[n].subm.work.repeats,
							sizeof(uint64_t));
		igt_assert(set->data[n].stats.complete_ts);
		if (set->sync_method == SYNC_BARRIER)
			set->data[n].barrier = &set->barrier;
	}

	run_subm_set_and_collect_metrics(pf_fd, set, vf_configs, metrics,
					 verify_with_perf_counters,
					 &measurement_start_ns,
					 &measurement_end_ns);

	for (int n = 0; n < set->ndata; ++n)
		igt_assert_eq(0, set->data[n].stats.num_early_finish);

	assert_share_matches_expected(metrics, set->ndata,
				      opts->outlier_treshold, false);

	if (verify_with_perf_counters) {
		warn_active_ticks_mismatch(metrics, set->ndata,
					   opts->outlier_treshold);
		assert_share_matches_expected(metrics, set->ndata,
					      opts->outlier_treshold, true);
		assert_throughput_share_matches_total_tick(metrics,
							   set->ndata,
							   opts->outlier_treshold);
	}

	free(vf_configs);
	free(metrics);
	subm_set_fini(set);
}

/**
 * SUBTEST: equal-throughput-%s-priority
 * Description:
 *   Check all VFs with same scheduling settings running same workload
 *   achieve the same throughput.
 *
 * arg[1]:
 *
 * @normal: normal
 * @low: low
 */
static void throughput_ratio(int pf_fd, int num_vfs, const struct subm_opts *opts,
			     bool verify_with_perf_counters,
			     struct job_sched_params *job_sched_params,
			     enum xe_sriov_sched_priority priority,
			     const struct drm_xe_engine_class_instance *eci)
{
	uint8_t vf_ids[num_vfs + 1 /*PF*/];

	igt_assert(job_sched_params);

	if (!job_sched_params->num_repeats) {
		uint32_t job_timeout_ms = sysfs_get_job_timeout_ms(pf_fd, eci);

		*job_sched_params = prepare_job_sched_params(num_vfs + 1,
							     job_timeout_ms,
							     opts, priority);
		xe_sriov_disable_vfs_restore_auto_provisioning(pf_fd);
		xe_sriov_admin_bulk_set_sched_params(pf_fd,
						     &job_sched_params->sched_params);
		igt_sriov_enable_driver_autoprobe(pf_fd);
		igt_sriov_enable_vfs(pf_fd, num_vfs);
	}

	init_vf_ids(vf_ids, ARRAY_SIZE(vf_ids),
		    &(struct init_vf_ids_opts){ .shuffle = true,
						.shuffle_pf = true });

	verify_applied_scheduling_behavior(pf_fd, vf_ids, ARRAY_SIZE(vf_ids), opts,
					   verify_with_perf_counters, eci,
					   job_sched_params);
}

/**
 * SUBTEST: nonpreempt-engine-resets-%s-priority
 * Description:
 *   Check all VFs running a non-preemptible workload with a duration
 *   exceeding the sum of its execution quantum and preemption timeout,
 *   will experience engine reset due to preemption timeout.
 *
 * arg[1]:
 *
 * @normal: normal
 * @low: low
 */
static void nonpreempt_engine_resets(int pf_fd, int num_vfs,
				     const struct subm_opts *opts,
				     struct job_sched_params *job_sched_params,
				     enum xe_sriov_sched_priority priority,
				     const struct drm_xe_engine_class_instance *eci)
{
	struct subm_set set_ = {}, *set = &set_;
	uint32_t job_timeout_ms = sysfs_get_job_timeout_ms(pf_fd, eci);
	int preemptible_end = 1;
	uint8_t vf_ids[num_vfs + 1 /*PF*/];
	unsigned int k;

	igt_assert(job_sched_params);

	if (!job_sched_params->num_repeats) {
		struct xe_sriov_sched_params vf_sched_params =
			prepare_vf_sched_params(num_vfs, 1, job_timeout_ms,
						opts, priority);

		*job_sched_params = (struct job_sched_params) {
			.sched_params = vf_sched_params,
			.duration_ms = 2 * vf_sched_params.exec_quantum_ms +
				       vf_sched_params.preempt_timeout_us / USEC_PER_MSEC,
			.num_repeats = 1,
		};
		xe_sriov_disable_vfs_restore_auto_provisioning(pf_fd);
		xe_sriov_admin_bulk_set_sched_params(pf_fd,
						     &job_sched_params->sched_params);
		igt_sriov_enable_driver_autoprobe(pf_fd);
		igt_sriov_enable_vfs(pf_fd, num_vfs);
	}
	k = select_inflight_k(job_sched_params->duration_ms, opts->inflight, true);

	igt_info("eq=%ums pt=%uus prio=%s duration=%dms inflight=%u num_functions=%d job_timeout=%ums\n",
		 job_sched_params->sched_params.exec_quantum_ms,
		 job_sched_params->sched_params.preempt_timeout_us,
		 xe_sriov_sched_priority_to_string(job_sched_params->sched_params.priority),
		 job_sched_params->duration_ms, k, num_vfs + 1, job_timeout_ms);

	init_vf_ids(vf_ids, ARRAY_SIZE(vf_ids),
		    &(struct init_vf_ids_opts){ .shuffle = true,
						.shuffle_pf = true });

	/* init subm_set */
	subm_set_alloc_data(set, num_vfs + 1 /*PF*/);
	subm_set_init_sync_method(set, opts->sync_method);

	for (int n = 0; n < set->ndata; ++n) {
		int vf_fd =
			vf_ids[n] ?
				igt_sriov_open_vf_drm_device(pf_fd, vf_ids[n]) :
				drm_reopen_driver(pf_fd);

		igt_assert_fd(vf_fd);
		set->data[n].opts = opts;
		subm_init(&set->data[n].subm, vf_fd, vf_ids[n], 0,
			  *eci, k);
		subm_workload_init(&set->data[n].subm,
				   &(struct subm_work_desc){
					.duration_ms = job_sched_params->duration_ms,
					.preempt = (n < preemptible_end),
					.repeats = MIN_NUM_REPEATS });
		igt_stats_init_with_size(&set->data[n].stats.samples,
					 set->data[n].subm.work.repeats);
		set->data[n].stats.complete_ts = calloc(set->data[n].subm.work.repeats,
							sizeof(uint64_t));
		if (set->sync_method == SYNC_BARRIER)
			set->data[n].barrier = &set->barrier;
	}

	/* dispatch spinners, wait for results */
	subm_set_dispatch_and_wait_threads(set);
	subm_set_close_handles(set);

	/* verify results */
	for (int n = 0; n < set->ndata; ++n) {
		if (n < preemptible_end) {
			igt_assert_eq(0, set->data[n].stats.num_early_finish);
			igt_assert_eq(set->data[n].subm.work.repeats,
				      set->data[n].stats.samples.n_values);
		} else {
			igt_assert_eq(1, set->data[n].stats.num_early_finish);
		}
	}

	/* cleanup */
	subm_set_fini(set);
}

static struct job_sched_params
prepare_default_enabled_job_sched_params(int pf_fd,
					 const uint8_t *vf_ids,
					 size_t num_functions,
					 int job_timeout_ms)
{
	struct job_sched_params params = { };
	uint32_t min_exec_quantum_ms = UINT32_MAX;

	params.sched_params = (struct xe_sriov_sched_params) {
		.exec_quantum_ms = xe_sriov_admin_get_exec_quantum_ms(pf_fd, vf_ids[0]),
		.preempt_timeout_us = xe_sriov_admin_get_preempt_timeout_us(pf_fd, vf_ids[0]),
		.priority = xe_sriov_admin_get_sched_priority(pf_fd, vf_ids[0], NULL),
	};

	for (size_t n = 0; n < num_functions; ++n) {
		uint32_t exec_quantum_ms =
			xe_sriov_admin_get_exec_quantum_ms(pf_fd, vf_ids[n]);

		min_exec_quantum_ms = min(min_exec_quantum_ms,
					  exec_quantum_ms);
	}

	params.duration_ms = calculate_job_duration_ms(min_exec_quantum_ms);
	params.num_repeats = adjust_num_repeats(params.duration_ms, num_functions);

	igt_require_f(params.duration_ms +
		      (num_functions - 1) * params.sched_params.exec_quantum_ms <=
		      job_timeout_ms,
		      "Default scheduling eq=%ums across %zu functions exceeds job timeout=%dms\n",
		      params.sched_params.exec_quantum_ms, num_functions, job_timeout_ms);

	return params;
}

static void validate_default_enabled_sched_params(int pf_fd,
						  const uint8_t *vf_ids,
						  size_t num_functions)
{
	for (size_t n = 0; n < num_functions; ++n) {
		uint32_t exec_quantum_ms =
			xe_sriov_admin_get_exec_quantum_ms(pf_fd, vf_ids[n]);
		uint32_t preempt_timeout_us =
			xe_sriov_admin_get_preempt_timeout_us(pf_fd, vf_ids[n]);
		enum xe_sriov_sched_priority priority =
			xe_sriov_admin_get_sched_priority(pf_fd, vf_ids[n], NULL);

		igt_require_f(exec_quantum_ms > 0,
			      "%s exec_quantum_ms stayed at 0 after enabling VFs\n",
			      igt_sriov_func_str(vf_ids[n]));
		igt_require_f(preempt_timeout_us > 0,
			      "%s preempt_timeout_us stayed at 0 after enabling VFs\n",
			      igt_sriov_func_str(vf_ids[n]));
		igt_warn_on_f(priority != XE_SRIOV_SCHED_PRIORITY_LOW,
			      "%s expected LOW sched_priority after enabling VFs, got %s\n",
			      igt_sriov_func_str(vf_ids[n]),
			      xe_sriov_sched_priority_to_string(priority));
	}
}

static void ensure_enabled_vfs(int pf_fd, int num_vfs)
{
	unsigned int enabled_vfs = igt_sriov_get_enabled_vfs(pf_fd);

	if (enabled_vfs && enabled_vfs != (unsigned int)num_vfs) {
		xe_sriov_disable_vfs_restore_auto_provisioning(pf_fd);
		enabled_vfs = 0;
	}

	if (!enabled_vfs) {
		xe_sriov_require_default_scheduling_attributes(pf_fd);
		igt_sriov_enable_driver_autoprobe(pf_fd);
		igt_sriov_enable_vfs(pf_fd, num_vfs);
	}
}

/**
 * SUBTEST: default-enabled-fair-scheduling
 * Description:
 *   Check PF and enabled VFs keep fair scheduling using the driver-applied
 *   default scheduling settings established when VFs are enabled.
 */
static void default_enabled_fair_scheduling(int pf_fd, int num_vfs,
					    const struct subm_opts *opts,
					    bool verify_with_perf_counters,
					    const struct drm_xe_engine_class_instance *eci)
{
	uint8_t vf_ids[num_vfs + 1 /*PF*/];
	uint32_t job_timeout_ms = sysfs_get_job_timeout_ms(pf_fd, eci);
	struct job_sched_params job_sched_params;

	ensure_enabled_vfs(pf_fd, num_vfs);

	init_vf_ids(vf_ids, ARRAY_SIZE(vf_ids),
		    &(struct init_vf_ids_opts){ .shuffle = true,
						.shuffle_pf = true });
	validate_default_enabled_sched_params(pf_fd, vf_ids, ARRAY_SIZE(vf_ids));
	job_sched_params = prepare_default_enabled_job_sched_params(pf_fd, vf_ids,
								    ARRAY_SIZE(vf_ids),
								    job_timeout_ms);

	verify_applied_scheduling_behavior(pf_fd, vf_ids, ARRAY_SIZE(vf_ids), opts,
					   verify_with_perf_counters, eci,
					   &job_sched_params);
}

static bool skip_visited_gt(bool extended_scope, uint64_t *visited_gts,
			    unsigned short gt_id)
{
	if (extended_scope)
		return false;

	if (*visited_gts & (1ULL << gt_id))
		return true;

	*visited_gts |= (1ULL << gt_id);

	return false;
}

static struct subm_opts subm_opts = {
	.sync_method = SYNC_BARRIER,
	.outlier_treshold = 0.1,
	.inflight = 0,
};

static bool extended_scope;

static int subm_opts_handler(int opt, int opt_index, void *data)
{
	switch (opt) {
	case 'e':
		extended_scope = true;
		break;
	case 's':
		subm_opts.sync_method = atoi(optarg);
		igt_info("Sync method: %d\n", subm_opts.sync_method);
		break;
	case 'q':
		subm_opts.exec_quantum_ms = atoi(optarg);
		igt_info("Execution quantum ms: %u\n", subm_opts.exec_quantum_ms);
		break;
	case 'p':
		subm_opts.preempt_timeout_us = atoi(optarg);
		igt_info("Preempt timeout us: %u\n", subm_opts.preempt_timeout_us);
		break;
	case 't':
		subm_opts.outlier_treshold = atoi(optarg) / 100.0;
		igt_info("Outlier threshold: %.2f\n", subm_opts.outlier_treshold);
		break;
	case 'i': {
		int val = atoi(optarg);

		subm_opts.inflight = val > 0 ? val : 0;
		if (subm_opts.inflight)
			igt_info("In-flight submissions: %u\n", subm_opts.inflight);
		else
			igt_info("In-flight submissions: auto (0)\n");
		break;
	}
	default:
		return IGT_OPT_HANDLER_ERROR;
	}

	return IGT_OPT_HANDLER_SUCCESS;
}

static const struct option long_opts[] = {
	{ .name = "extended", .has_arg = false, .val = 'e', },
	{ .name = "sync", .has_arg = true, .val = 's', },
	{ .name = "threshold", .has_arg = true, .val = 't', },
	{ .name = "eq_ms", .has_arg = true, .val = 'q', },
	{ .name = "pt_us", .has_arg = true, .val = 'p', },
	{ .name = "inflight", .has_arg = true, .val = 'i', },
	{}
};

static const char help_str[] =
	"  --extended\tRun the extended test scope\n"
	"  --sync\tThreads synchronization method: 0 - none 1 - barrier (Default 1)\n"
	"  --threshold\tSample outlier threshold (Default 0.1)\n"
	"  --eq_ms\texec_quantum_ms\n"
	"  --pt_us\tpreempt_timeout_us\n"
	"  --inflight\tNumber of submissions kept in flight per VF (0=auto)\n";

int igt_main_args("", long_opts, help_str, subm_opts_handler, NULL)
{
	int pf_fd;
	bool autoprobe;
	bool has_perf_events;
	struct drm_xe_engine_class_instance *eci;
	unsigned short ecls;

	igt_fixture() {
		pf_fd = drm_open_driver(DRIVER_XE);
		igt_require(igt_sriov_is_pf(pf_fd));
		igt_require(igt_sriov_get_enabled_vfs(pf_fd) == 0);
		igt_require(xe_sriov_admin_is_present(pf_fd));
		igt_sriov_install_exit_handler(pf_fd,
					       xe_sriov_admin_exit_cleanup_restore_sched_defaults,
					       NULL);
		autoprobe = igt_sriov_is_driver_autoprobe_enabled(pf_fd);
		xe_sriov_require_default_scheduling_attributes(pf_fd);
		xe_perf_device(pf_fd, perf_device, sizeof(perf_device));
		has_perf_events = has_perf_event(perf_device, "engine-active-ticks") &&
				  has_perf_event(perf_device, "engine-total-ticks");
	}

	igt_describe("Check PF and VFs keep fair scheduling using driver defaults applied on VF enable");
	igt_subtest_with_dynamic("default-enabled-fair-scheduling") {
		if (extended_scope)
			for_each_sriov_num_vfs(pf_fd, vf)
				xe_for_each_engine(pf_fd, eci) {
					ecls = eci->engine_class;
					igt_dynamic_f("numvfs-%d-gt%u-%s%u", vf,
						      eci->gt_id,
						      xe_engine_class_short_string(ecls),
						      eci->engine_instance)
						default_enabled_fair_scheduling(pf_fd, vf,
										&subm_opts,
										has_perf_events,
										eci);
				}

		for_random_sriov_vf(pf_fd, vf) {
			uint64_t visited_gts = 0;

			xe_for_each_engine(pf_fd, eci) {
				if (skip_visited_gt(extended_scope, &visited_gts,
						    eci->gt_id))
					continue;

				igt_dynamic_f("numvfs-random-gt%u-%s%u",
					      eci->gt_id,
					      xe_engine_class_short_string(eci->engine_class),
					      eci->engine_instance)
					default_enabled_fair_scheduling(pf_fd, vf, &subm_opts,
									has_perf_events,
									eci);
			}
		}
	}

	igt_fixture() {
		xe_sriov_disable_vfs_restore_auto_provisioning(pf_fd);
	}

	for (enum xe_sriov_sched_priority priority = XE_SRIOV_SCHED_PRIORITY_LOW;
	     priority <= XE_SRIOV_SCHED_PRIORITY_NORMAL;
	     priority++) {
		igt_describe_f("Check VFs achieve equal throughput with %s priority provisioning applied before VF enable on each selected engine",
			       xe_sriov_sched_priority_to_string(priority));
		igt_subtest_with_dynamic_f("equal-throughput-%s-priority",
					   xe_sriov_sched_priority_to_string(priority)) {
			if (extended_scope)
				for_each_sriov_num_vfs(pf_fd, vf) {
					struct job_sched_params job_sched_params = { };

					xe_for_each_engine(pf_fd, eci) {
						ecls = eci->engine_class;
						igt_dynamic_f("numvfs-%d-gt%u-%s%u", vf,
							      eci->gt_id,
							      xe_engine_class_short_string(ecls),
							      eci->engine_instance)
							throughput_ratio(pf_fd, vf, &subm_opts,
									 has_perf_events,
									 &job_sched_params,
									 priority, eci);
					}
				}

			for_random_sriov_vf(pf_fd, vf) {
				struct job_sched_params job_sched_params = { };
				uint64_t visited_gts = 0;

				xe_for_each_engine(pf_fd, eci) {
					if (skip_visited_gt(extended_scope, &visited_gts,
							    eci->gt_id))
						continue;

					ecls = eci->engine_class;
					igt_dynamic_f("numvfs-random-gt%u-%s%u",
						      eci->gt_id,
						      xe_engine_class_short_string(ecls),
						      eci->engine_instance)
						throughput_ratio(pf_fd, vf, &subm_opts,
								 has_perf_events,
								 &job_sched_params,
								 priority, eci);
				}
			}
		}

		igt_fixture() {
			xe_sriov_disable_vfs_restore_auto_provisioning(pf_fd);
			__xe_sriov_admin_bulk_restore_sched_defaults(pf_fd);
		}
	}

	for (enum xe_sriov_sched_priority priority = XE_SRIOV_SCHED_PRIORITY_LOW;
	     priority <= XE_SRIOV_SCHED_PRIORITY_NORMAL;
	     priority++) {
		igt_describe("Check VFs experience engine reset due to preemption timeout on each selected engine");
		igt_subtest_with_dynamic_f("nonpreempt-engine-resets-%s-priority",
					   xe_sriov_sched_priority_to_string(priority)) {
			if (extended_scope)
				for_each_sriov_num_vfs(pf_fd, vf) {
					struct job_sched_params job_sched_params = { };

					xe_for_each_engine(pf_fd, eci) {
						ecls = eci->engine_class;
						igt_dynamic_f("numvfs-%d-gt%u-%s%u", vf,
							      eci->gt_id,
							      xe_engine_class_short_string(ecls),
							      eci->engine_instance)
							nonpreempt_engine_resets(pf_fd, vf,
										 &subm_opts,
										 &job_sched_params,
										 priority,
										 eci);
					}
				}

			for_random_sriov_vf(pf_fd, vf) {
				struct job_sched_params job_sched_params = { };
				uint64_t visited_gts = 0;

				xe_for_each_engine(pf_fd, eci) {
					if (skip_visited_gt(extended_scope, &visited_gts,
							    eci->gt_id))
						continue;

					ecls = eci->engine_class;
					igt_dynamic_f("numvfs-random-gt%u-%s%u",
						      eci->gt_id,
						      xe_engine_class_short_string(ecls),
						      eci->engine_instance)
						nonpreempt_engine_resets(pf_fd, vf, &subm_opts,
									 &job_sched_params,
									 priority, eci);
				}
			}
		}

		igt_fixture() {
			xe_sriov_disable_vfs_restore_auto_provisioning(pf_fd);
			__xe_sriov_admin_bulk_restore_sched_defaults(pf_fd);
		}
	}

	igt_fixture() {
		int ret;

		xe_sriov_disable_vfs_restore_auto_provisioning(pf_fd);
		ret = __xe_sriov_admin_bulk_restore_sched_defaults(pf_fd);
		/* abort to avoid execution of next tests with enabled VFs */
		igt_abort_on_f(igt_sriov_get_enabled_vfs(pf_fd) > 0,
			       "Failed to disable VF(s)");
		autoprobe ? igt_sriov_enable_driver_autoprobe(pf_fd) :
			    igt_sriov_disable_driver_autoprobe(pf_fd);
		igt_abort_on_f(autoprobe != igt_sriov_is_driver_autoprobe_enabled(pf_fd),
			       "Failed to restore sriov_drivers_autoprobe value\n");
		igt_abort_on_f(ret,
			       "Failed to restore scheduling params\n");
		igt_sriov_clear_exit_handler();
		drm_close_driver(pf_fd);
	}
}
