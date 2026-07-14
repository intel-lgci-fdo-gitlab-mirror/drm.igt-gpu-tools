// SPDX-License-Identifier: MIT
/*
 * Copyright © 2017 Intel Corporation
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

#include <unistd.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <math.h>
#include <ctype.h>

#include "drm.h"
#include "drmtest.h"
#include "igt_device_scan.h"
#include "intel_chipset.h"
#include "intel_reg.h"
#include "ioctl_wrappers.h"

#include "intel_io.h"
#include "igt_aux.h"
#include "igt_rand.h"
#include "igt_perf.h"
#include "sw_sync.h"

#include "i915/gem_create.h"
#include "i915/gem_engine_topology.h"
#include "i915/gem_mman.h"

#include "igt_syncobj.h"
#include "igt_thread.h"
#include "intel_allocator.h"
#include "xe_drm.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_spin.h"

enum intel_engine_class {
	RCS,
	BCS,
	VCS,
	VECS,
	CCS,
	NUM_ENGINE_CLASSES,
};

_Static_assert(RCS == DRM_XE_ENGINE_CLASS_RENDER, "mismatch");
_Static_assert(BCS == DRM_XE_ENGINE_CLASS_COPY, "mismatch");
_Static_assert(VCS == DRM_XE_ENGINE_CLASS_VIDEO_DECODE, "mismatch");
_Static_assert(VECS == DRM_XE_ENGINE_CLASS_VIDEO_ENHANCE, "mismatch");
_Static_assert(CCS == DRM_XE_ENGINE_CLASS_COMPUTE, "mismatch");
_Static_assert((int)RCS == (int)I915_ENGINE_CLASS_RENDER, "mismatch");
_Static_assert((int)BCS == (int)I915_ENGINE_CLASS_COPY, "mismatch");
_Static_assert((int)VCS == (int)I915_ENGINE_CLASS_VIDEO, "mismatch");
_Static_assert((int)VECS == (int)I915_ENGINE_CLASS_VIDEO_ENHANCE, "mismatch");
_Static_assert((int)CCS == (int)I915_ENGINE_CLASS_COMPUTE, "mismatch");

static const char *intel_engine_class_string(uint16_t engine_class)
{
	switch (engine_class) {
	case RCS:
		return "RCS";
	case BCS:
		return "BCS";
	case VCS:
		return "VCS";
	case VECS:
		return "VECS";
	case CCS:
		return "CCS";
	default:
		igt_assert(0);
	}
}

struct duration {
	unsigned int min, max;
	bool unbound;
	uint32_t requested_ticks;
};

enum w_type {
	BATCH,
	SYNC,
	DELAY,
	PERIOD,
	THROTTLE,
	QD_THROTTLE,
	SW_FENCE,
	SW_FENCE_SIGNAL,
	CTX_PRIORITY,
	PREEMPTION,
	ENGINE_MAP,
	LOAD_BALANCE,
	BOND,
	TERMINATE,
	SSEU,
	WORKINGSET,
};

struct dep_entry {
	int target;
	bool write;
	int working_set; /* -1 = step dependecy, >= 0 working set id */
};

struct deps {
	int nr;
	bool submit_fence;
	struct dep_entry *list;
};

#define for_each_dep(__dep, __deps) \
	for (int __i = 0; __i < __deps.nr && \
	     (__dep = &__deps.list[__i]); ++__i)

struct w_arg {
	char *filename;
	char *desc;
	int prio;
	bool sseu;
};

#define INVALID_ID ((uint16_t)-2)
#define DEFAULT_ID ((uint16_t)-1)

typedef struct drm_xe_engine_class_instance intel_engine_t;

struct intel_engines {
	unsigned int nr_engines;
	intel_engine_t *engines;
};

struct bond {
	struct intel_engines mask;
	intel_engine_t master;
};

struct work_buffer_size {
	unsigned long size;
	unsigned long min;
	unsigned long max;
};

struct working_set {
	int id;
	bool shared;
	unsigned int nr;
	uint32_t *handles;
	struct work_buffer_size *sizes;
};

struct workload;

struct w_step {
	struct workload *wrk;

	/* Workload step metadata */
	enum w_type type;
	unsigned int context;
	intel_engine_t engine;
	unsigned int engine_idx;
	struct duration duration;
	struct deps data_deps;
	struct deps fence_deps;
	int emit_fence;
	union {
		int sync;
		int delay;
		int period;
		int target;
		int throttle;
		int priority;
		struct intel_engines engine_map;
		bool load_balance;
		struct bond bond;
		int sseu;
		struct working_set working_set;
	};

	/* Implementation details */
	unsigned int idx;
	struct igt_list_head rq_link;
	unsigned int request_idx;
	unsigned int preempt_us;

	union {
		struct {
			struct drm_i915_gem_execbuffer2 eb;
			struct drm_i915_gem_exec_object2 *obj;
			struct drm_i915_gem_relocation_entry reloc[3];
			uint32_t *bb_duration;
		} i915;
		struct {
			struct drm_xe_exec exec;
			struct {
				struct xe_spin spin;
				uint64_t vm_sync;
				uint64_t exec_sync;
			} *data;
			struct drm_xe_sync *syncs;
		} xe;
	};
	unsigned long bb_size;
	uint32_t bb_handle;
};

struct vm {
	uint32_t id;
	bool compute_mode;
	uint64_t ahnd;
};

struct xe_exec_queue {
	uint32_t id;
	unsigned int nr_hwes;
	struct drm_xe_engine_class_instance *hwe_list;
};

struct ctx {
	uint32_t id;
	int priority;
	struct intel_engines engine_map;
	unsigned int bond_count;
	struct bond *bonds;
	bool load_balance;
	uint64_t sseu;
	/* reference to vm */
	struct vm *vm;
	struct {
		/* exec queues */
		unsigned int nr_queues;
		struct xe_exec_queue *queue_list;
	} xe;
};

struct workload {
	unsigned int id;

	unsigned int nr_steps;
	struct w_step *steps;
	int prio;
	bool sseu;

	pthread_t thread;
	bool run;
	bool background;
	unsigned int repeat;
	unsigned int flags;
	bool print_stats;

	uint32_t bb_prng;
	uint32_t bo_prng;

	unsigned int nr_ctxs;
	struct ctx *ctx_list;

	unsigned int nr_vms;
	struct vm *vm_list;

	struct working_set **working_sets; /* array indexed by set id */
	int max_working_set_id;

	int sync_timeline;
	uint32_t sync_seqno;

	struct igt_list_head *requests;
	unsigned int *nrequest;
};

#define __for_each_ctx(__ctx, __wrk, __ctx_idx) \
	for (typeof((__wrk)->nr_ctxs) __ctx_idx = 0; __ctx_idx < (__wrk)->nr_ctxs && \
	     (__ctx = &(__wrk)->ctx_list[__ctx_idx]); ++__ctx_idx)

#define for_each_ctx(__ctx, __wrk) \
	__for_each_ctx(__ctx, __wrk, igt_unique(__ctx_idx))

/* igt_unique(idx) is same on both lines as macro when expanded comes out on one line */
#define for_each_w_step(__w_step, __wrk) \
	for (typeof(__wrk->nr_steps) igt_unique(idx) = ({__w_step = __wrk->steps; 0; }); \
	     igt_unique(idx) < __wrk->nr_steps; igt_unique(idx)++, __w_step++)

static unsigned int master_prng;

static int verbose = 1;
static int fd;
static bool is_xe;
static struct drm_i915_gem_context_param_sseu device_sseu = {
	.slice_mask = -1 /* Force read on first use. */
};

#define FLAG_SYNCEDCLIENTS	(1<<1)
#define FLAG_DEPSYNC		(1<<2)
#define FLAG_SSEU		(1<<3)
#define FLAG_VERIFY_COMPLETION	(1 << 4)

static void w_step_sync(struct w_step *w)
{
	if (is_xe)
		igt_assert(syncobj_wait(fd, &w->xe.syncs[0].handle, 1, INT64_MAX, 0, NULL));
	else
		gem_sync(fd, w->i915.obj[0].handle);
}

static int read_timestamp_frequency(int i915)
{
	int value = 0;
	drm_i915_getparam_t gp = {
		.value = &value,
		.param = I915_PARAM_CS_TIMESTAMP_FREQUENCY,
	};
	ioctl(i915, DRM_IOCTL_I915_GETPARAM, &gp);
	return value;
}

static uint64_t div64_u64_round_up(uint64_t x, uint64_t y)
{
	return (x + y - 1) / y;
}

static uint64_t ns_to_ctx_ticks(uint64_t ns)
{
	static long f;

	if (!f) {
		f = read_timestamp_frequency(fd);
		if (intel_gen(intel_get_drm_devid(fd)) == 11)
			f = 12500000; /* icl!!! are you feeling alright? */
	}

	return div64_u64_round_up(ns * f, NSEC_PER_SEC);
}

#define MI_STORE_DWORD_INDEX	MI_INSTR(0x21, 1)

static unsigned int offset_in_page(void *addr)
{
	return (uintptr_t)addr & 4095;
}

static void add_dep(struct deps *deps, struct dep_entry entry)
{
	deps->list = realloc(deps->list, sizeof(*deps->list) * (deps->nr + 1));
	igt_assert(deps->list);

	deps->list[deps->nr++] = entry;
}

static int
parse_working_set_deps(struct workload *wrk,
		       struct deps *deps,
		       struct dep_entry _entry,
		       char *str)
{
	/*
	 * 1 - target handle index in the specified working set.
	 * 2-4 - range
	 */
	struct dep_entry entry = _entry;
	char *s;

	s = index(str, '-');
	if (s) {
		int from, to;

		from = atoi(str);
		if (from < 0)
			return -1;

		to = atoi(++s);
		if (to <= 0)
			return -1;

		if (to <= from)
			return -1;

		for (entry.target = from; entry.target <= to; entry.target++)
			add_dep(deps, entry);
	} else {
		entry.target = atoi(str);
		if (entry.target < 0)
			return -1;

		add_dep(deps, entry);
	}

	return 0;
}

static void __attribute__((format(printf, 1, 2)))
wsim_err(const char *fmt, ...)
{
	va_list ap;

	if (!verbose)
		return;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

static int
parse_dependency(unsigned int nr_steps, struct w_step *w, char *str)
{
	struct dep_entry entry = { .working_set = -1 };
	bool submit_fence = false;
	char *s;

	switch (str[0]) {
	case '-':
		if (str[1] < '0' || str[1] > '9')
			return -1;

		entry.target = atoi(str);
		if (entry.target > 0 || ((int)nr_steps + entry.target) < 0)
			return -1;

		add_dep(&w->data_deps, entry);

		break;
	case 's':
		/* no submit fence in xe */
		if (is_xe) {
			wsim_err("Submit fences are not supported with xe\n");
			return -1;
		}
		submit_fence = true;
		/* Fall-through. */
	case 'f':
		/* xe supports multiple fences */
		if (!is_xe)
			/* Multiple fences not yet supported. */
			igt_assert_eq(w->fence_deps.nr, 0);

		entry.target = atoi(++str);
		if (entry.target > 0 || ((int)nr_steps + entry.target) < 0)
			return -1;

		add_dep(&w->fence_deps, entry);

		w->fence_deps.submit_fence = submit_fence;
		break;
	case 'w':
		entry.write = true;
		/* Fall-through. */
	case 'r':
		/*
		 * [rw]N-<str>
		 * r1-<str> or w2-<str>, where N is working set id.
		 */
		s = index(++str, '-');
		if (!s)
			return -1;

		entry.working_set = atoi(str);
		if (entry.working_set < 0)
			return -1;

		if (parse_working_set_deps(w->wrk, &w->data_deps, entry, ++s))
			return -1;

		break;
	default:
		return -1;
	};

	return 0;
}

static int
parse_dependencies(unsigned int nr_steps, struct w_step *w, char *_desc)
{
	char *desc = strdup(_desc);
	char *token, *tctx = NULL, *tstart = desc;
	int ret = 0;

	/*
	 * Skip when no dependencies to avoid having to detect
	 * non-sensical "0/0/..." below.
	 */
	if (!strcmp(_desc, "0"))
		goto out;

	igt_assert(desc);
	igt_assert(!w->data_deps.nr && w->data_deps.nr == w->fence_deps.nr);
	igt_assert(!w->data_deps.list &&
		   w->data_deps.list == w->fence_deps.list);

	while ((token = strtok_r(tstart, "/", &tctx)) != NULL) {
		tstart = NULL;

		ret = parse_dependency(nr_steps, w, token);
		if (ret)
			break;
	}

out:
	free(desc);

	return ret;
}

#define check_arg(cond, fmt, ...) \
{ \
	if (cond) { \
		wsim_err(fmt, __VA_ARGS__); \
		return NULL; \
	} \
}

/* engine_class[<engine_instance>-<gt_id>] */
static intel_engine_t str_to_engine(const char *str)
{
	intel_engine_t e = {INVALID_ID, DEFAULT_ID, DEFAULT_ID};
	size_t pos;

	if (!strcasecmp("DEFAULT", str)) {
		e.engine_class = DEFAULT_ID;
		return e;
	} else if (!strncasecmp("RCS", str, 3)) {
		e.engine_class = RCS;
		pos = 3;
	} else if (!strncasecmp("BCS", str, 3)) {
		e.engine_class = BCS;
		pos = 3;
	} else if (!strncasecmp("VCS", str, 3)) {
		e.engine_class = VCS;
		pos = 3;
	} else if (!strncasecmp("VECS", str, 4)) {
		e.engine_class = VECS;
		pos = 4;
	} else if (!strncasecmp("CCS", str, 3)) {
		e.engine_class = CCS;
		pos = 3;
	} else {
		return (intel_engine_t){INVALID_ID};
	}

	if (str[pos]) {
		char *s = strchr(&str[pos], '-');
		char *endptr = NULL;
		long id;

		if (!s || (s && *s != str[pos])) {
			id = strtol(&str[pos], &endptr, 10);
			if (endptr == &str[pos] || id < 1 || id >= INVALID_ID)
				return (intel_engine_t){INVALID_ID};
			e.engine_instance = id - 1;
		}

		if (s && *(++s)) {
			id = strtol(s, &endptr, 10);
			if (endptr == s || id < 0 || id >= INVALID_ID)
				return (intel_engine_t){INVALID_ID};
			e.gt_id = id;
		}

		if (endptr && endptr != (str + strlen(str)))
			return (intel_engine_t){INVALID_ID};
	}

	return e;
}

static struct i915_engine_class_instance
engine_to_i915_engine_class(const intel_engine_t *engine)
{
	return (struct i915_engine_class_instance){ engine->engine_class,
						    engine->engine_instance };
}

static unsigned int
engine_to_i915_legacy_ring(const intel_engine_t *engine)
{
	switch (engine->engine_class) {
	case DEFAULT_ID:
		return I915_EXEC_DEFAULT;
	case RCS:
		return I915_EXEC_RENDER;
	case BCS:
		return I915_EXEC_BLT;
	case VCS:
		if (engine->engine_instance == DEFAULT_ID)
			return I915_EXEC_BSD;
		else if (engine->engine_instance == 0)
			return I915_EXEC_BSD | I915_EXEC_BSD_RING1;
		else if (engine->engine_instance == 1)
			return I915_EXEC_BSD | I915_EXEC_BSD_RING2;
		break;
	case VECS:
		return I915_EXEC_VEBOX;
	};

	igt_assert(0);
}

static bool are_equal_engines(const intel_engine_t *e1,
			      const intel_engine_t *e2)
{
	return e1->engine_class == e2->engine_class &&
	       e1->engine_instance == e2->engine_instance &&
	       e1->gt_id == e2->gt_id;
}

static bool find_engine_in_map(const intel_engine_t *engine,
			       struct intel_engines *engines, unsigned int *idx)
{
	igt_assert(idx);
	for (unsigned int i = 0; i < engines->nr_engines; ++i)
		if (are_equal_engines(engine, &engines->engines[i])) {
			*idx = i;
			return true;
		}

	return false;
}

static struct intel_engines *query_engines(void)
{
	static struct intel_engines engines = {};

	if (engines.nr_engines)
		return &engines;

	if (is_xe) {
		struct drm_xe_engine_class_instance *hwe;

		engines.engines = calloc(xe_number_engines(fd), sizeof(intel_engine_t));
		igt_assert(engines.engines);
		engines.nr_engines = 0;
		xe_for_each_engine(fd, hwe)
			engines.engines[engines.nr_engines++] = *hwe;
		igt_assert(engines.nr_engines);
	} else {
		struct intel_engine_data ed = {};

		ed = intel_engine_list_of_physical(fd);
		igt_assert(ed.nengines);
		engines.nr_engines = ed.nengines;
		engines.engines = calloc(engines.nr_engines, sizeof(intel_engine_t));
		igt_assert(engines.engines);
		for (int i = 0; i < ed.nengines; ++i) {
			engines.engines[i].engine_class = ed.engines[i].class;
			engines.engines[i].engine_instance = ed.engines[i].instance;
			engines.engines[i].gt_id = DEFAULT_ID;
		}
	}

	return &engines;
}

static bool is_valid_engine(const intel_engine_t *engine)
{
	return engine->engine_class != INVALID_ID;
}

static bool is_default_engine(const intel_engine_t *engine)
{
	return engine->engine_class == DEFAULT_ID &&
	       engine->engine_instance == DEFAULT_ID &&
	       engine->gt_id == DEFAULT_ID;
}

static bool engine_matches_filter(const intel_engine_t *engine, const intel_engine_t *filter)
{
	return (filter->engine_class == DEFAULT_ID ||
		filter->engine_class == engine->engine_class) &&
	       (filter->engine_instance == DEFAULT_ID ||
		filter->engine_instance == engine->engine_instance) &&
	       (filter->gt_id == DEFAULT_ID ||
		filter->gt_id == engine->gt_id);
}

#define for_each_matching_engine(__engine, __filter, __engines) \
	for (unsigned int __i = 0; __i < (__engines)->nr_engines && \
	     ((__engine) = &(__engines)->engines[__i]); __i++) \
		for_if(engine_matches_filter((__engine), (__filter)))

static unsigned int
append_matching_engines(const intel_engine_t *filter, struct intel_engines *engines)
{
	unsigned int prev_nr_engines;
	struct intel_engines *all = query_engines();
	intel_engine_t *engine;

	igt_assert(engines);
	prev_nr_engines = engines->nr_engines;

	for_each_matching_engine(engine, filter, all) {
		engines->nr_engines++;
		engines->engines = realloc(engines->engines,
					   engines->nr_engines * sizeof(intel_engine_t));
		igt_assert(engines->engines);
		engines->engines[engines->nr_engines - 1] = *engine;
	}

	return engines->nr_engines - prev_nr_engines;
}

static intel_engine_t get_default_engine(void)
{
	struct intel_engines *all_engines = query_engines();
	const intel_engine_t filters[] = {
		{RCS, DEFAULT_ID, DEFAULT_ID},
		{CCS, DEFAULT_ID, DEFAULT_ID},
		{DEFAULT_ID, DEFAULT_ID, DEFAULT_ID},
		{INVALID_ID}
	}, *filter, *default_engine;

	for (filter = filters; is_valid_engine(filter); filter++)
		for_each_matching_engine(default_engine, filter, all_engines)
			return *default_engine;

	igt_assert(0);
}

static intel_engine_t resolve_to_physical_engine_(const intel_engine_t *engine)
{
	struct intel_engines *all_engines = query_engines();
	intel_engine_t *resolved;

	igt_assert(engine);
	if (is_default_engine(engine))
		return get_default_engine();

	for_each_matching_engine(resolved, engine, all_engines)
		return *resolved;

	return (intel_engine_t){INVALID_ID};
}

static void resolve_to_physical_engine(intel_engine_t *engine)
{
	*engine = resolve_to_physical_engine_(engine);
	igt_assert(is_valid_engine(engine));
}

static int parse_engine_map(struct w_step *step, const char *_str)
{
	char *token, *tctx = NULL, *tstart = (char *)_str;
	intel_engine_t engine;

	while ((token = strtok_r(tstart, "|", &tctx))) {
		tstart = NULL;

		engine = str_to_engine(token);
		if (!is_valid_engine(&engine) || is_default_engine(&engine))
			return -1;

		if (!append_matching_engines(&engine, &step->engine_map))
			return -1;
	}

	return 0;
}

static int parse_bond_engines(struct w_step *step, const char *_str)
{
	char *token, *tctx = NULL, *tstart = (char *)_str;
	intel_engine_t engine;

	while ((token = strtok_r(tstart, "|", &tctx))) {
		tstart = NULL;

		engine = str_to_engine(token);
		if (append_matching_engines(&engine, &step->bond.mask) != 1)
			return -1;
	}

	return 0;
}

static unsigned long parse_size(char *str)
{
	const unsigned int len = strlen(str);
	unsigned int mult = 1;
	long val;

	/* "1234567890[gGmMkK]" */

	if (len == 0)
		return 0;

	switch (str[len - 1]) {
	case 'g':
	case 'G':
		mult *= 1024;
		/* Fall-throuogh. */
	case 'm':
	case 'M':
		mult *= 1024;
		/* Fall-throuogh. */
	case 'k':
	case 'K':
		mult *= 1024;

		str[len - 1] = 0;
		/* Fall-throuogh. */

	case '0' ... '9':
		break;
	default:
		return 0; /* Unrecognized non-digit. */
	}

	val = atol(str);
	if (val <= 0)
		return 0;

	return val * mult;
}

static int add_buffers(struct working_set *set, char *str)
{
	/*
	 * 4096
	 * 4k
	 * 4m
	 * 4g
	 * 10n4k - 10 4k batches
	 * 4096-16k - random size in range
	 */
	struct work_buffer_size *sizes;
	unsigned long min_sz, max_sz;
	char *n, *max = NULL;
	int add, i;

	n = index(str, 'n');
	if (n) {
		*n = 0;
		add = atoi(str);
		if (add <= 0)
			return -1;
		str = ++n;
	} else {
		add = 1;
	}

	n = index(str, '-');
	if (n) {
		*n = 0;
		max = ++n;
	}

	min_sz = parse_size(str);
	if (!min_sz)
		return -1;

	if (max) {
		max_sz = parse_size(max);
		if (!max_sz)
			return -1;
	} else {
		max_sz = min_sz;
	}

	sizes = realloc(set->sizes, (set->nr + add) * sizeof(*sizes));
	if (!sizes)
		return -1;

	for (i = 0; i < add; i++) {
		struct work_buffer_size *sz = &sizes[set->nr + i];

		sz->min = min_sz;
		sz->max = max_sz;
		sz->size = 0;
	}

	set->nr += add;
	set->sizes = sizes;

	return 0;
}

static int parse_working_set(struct working_set *set, char *str)
{
	char *token, *tctx = NULL, *tstart = str;

	while ((token = strtok_r(tstart, "/", &tctx))) {
		tstart = NULL;

		if (add_buffers(set, token))
			return -1;
	}

	return 0;
}

static unsigned long
allocate_working_set(struct workload *wrk, struct working_set *set);

static long __duration(long dur, double scale)
{
	return round(scale * dur);
}

static int
parse_duration(unsigned int nr_steps, struct duration *dur, double scale_dur, char *field)
{
	char *sep = NULL;
	long tmpl;

	if (field[0] == '*') {
		if (intel_gen(intel_get_drm_devid(fd)) < 8) {
			wsim_err("Infinite batch at step %u needs Gen8+!\n", nr_steps);
			return -1;
		}
		dur->unbound = true;
	} else {
		tmpl = strtol(field, &sep, 10);
		if (tmpl <= 0 || tmpl == LONG_MIN || tmpl == LONG_MAX) {
			wsim_err("Invalid duration at step %u!\n", nr_steps);
			return -1;
		}

		dur->min = __duration(tmpl, scale_dur);

		if (sep && *sep == '-') {
			tmpl = strtol(sep + 1, NULL, 10);
			if (tmpl <= 0 || __duration(tmpl, scale_dur) <= dur->min ||
			    tmpl == LONG_MIN || tmpl == LONG_MAX) {
				wsim_err("Invalid maximum duration at step %u!\n", nr_steps);
				return -1;
			}

			dur->max = __duration(tmpl, scale_dur);
		} else {
			dur->max = dur->min;
		}
	}

	return 0;
}

#define int_field(_STEP_, _FIELD_, _COND_, _ERR_) \
	do { \
		field = strtok_r(fstart, ".", &fctx); \
		if (field) { \
			tmp = atoi(field); \
			check_arg(_COND_, _ERR_, nr_steps); \
			step.type = _STEP_; \
			step._FIELD_ = tmp; \
			goto add_step; \
		} \
	} while (0)

static struct workload *
parse_workload(struct w_arg *arg, unsigned int flags, double scale_dur,
	       double scale_time, struct workload *app_w)
{
	struct workload *wrk;
	unsigned int nr_steps = 0;
	char *desc = strdup(arg->desc);
	char *_token, *token, *tctx = NULL, *tstart = desc;
	char *field, *fctx = NULL, *fstart;
	struct w_step step, *w, *steps = NULL;
	unsigned int valid;
	int i, j, tmp;

	igt_assert(desc);

	while ((_token = strtok_r(tstart, ",", &tctx))) {
		tstart = NULL;
		token = strdup(_token);
		igt_assert(token);
		fstart = token;
		valid = 0;
		memset(&step, 0, sizeof(step));

		field = strtok_r(fstart, ".", &fctx);
		if (field) {
			fstart = NULL;

			/* line starting with # is a comment */
			if (field[0] == '#') {
				if (verbose > 3)
					printf("skipped line: %s\n", _token);
				free(token);
				continue;
			}

			if (!strcmp(field, "d")) {
				int_field(DELAY, delay, tmp <= 0,
					  "Invalid delay at step %u!\n");
			} else if (!strcmp(field, "p")) {
				int_field(PERIOD, period, tmp <= 0,
					  "Invalid period at step %u!\n");
			} else if (!strcmp(field, "P")) {
				unsigned int nr = 0;

				if (is_xe) {
					wsim_err("Priority step is not implemented with xe yet.\n");
					free(token);
					return NULL;
				}

				while ((field = strtok_r(fstart, ".", &fctx))) {
					tmp = atoi(field);
					check_arg(nr == 0 && tmp <= 0,
						  "Invalid context at step %u!\n",
						  nr_steps);
					check_arg(nr > 1,
						  "Invalid priority format at step %u!\n",
						  nr_steps);

					if (nr == 0)
						step.context = tmp;
					else
						step.priority = tmp;

					nr++;
				}

				step.type = CTX_PRIORITY;
				goto add_step;
			} else if (!strcmp(field, "s")) {
				int_field(SYNC, target,
					  tmp >= 0 || ((int)nr_steps + tmp) < 0,
					  "Invalid sync target at step %u!\n");
			} else if (!strcmp(field, "S")) {
				unsigned int nr = 0;

				if (is_xe) {
					wsim_err("SSEU step is not implemented with xe yet.\n");
					free(token);
					return NULL;
				}

				while ((field = strtok_r(fstart, ".", &fctx))) {
					tmp = atoi(field);
					check_arg(tmp <= 0 && nr == 0,
						  "Invalid context at step %u!\n",
						  nr_steps);
					check_arg(nr > 1,
						  "Invalid SSEU format at step %u!\n",
						  nr_steps);

					if (nr == 0)
						step.context = tmp;
					else if (nr == 1)
						step.sseu = tmp;

					nr++;
				}

				step.type = SSEU;
				goto add_step;
			} else if (!strcmp(field, "t")) {
				int_field(THROTTLE, throttle,
					  tmp < 0,
					  "Invalid throttle at step %u!\n");
			} else if (!strcmp(field, "q")) {
				int_field(QD_THROTTLE, throttle,
					  tmp < 0,
					  "Invalid qd throttle at step %u!\n");
			} else if (!strcmp(field, "a")) {
				int_field(SW_FENCE_SIGNAL, target,
					  tmp >= 0,
					  "Invalid sw fence signal at step %u!\n");
			} else if (!strcmp(field, "f")) {
				step.type = SW_FENCE;
				goto add_step;
			} else if (!strcmp(field, "M")) {
				unsigned int nr = 0;

				while ((field = strtok_r(fstart, ".", &fctx))) {
					tmp = atoi(field);
					check_arg(nr == 0 && tmp <= 0,
						  "Invalid context at step %u!\n",
						  nr_steps);
					check_arg(nr > 1,
						  "Invalid engine map format at step %u!\n",
						  nr_steps);

					if (nr == 0) {
						step.context = tmp;
					} else {
						tmp = parse_engine_map(&step,
								       field);
						check_arg(tmp < 0,
							  "Invalid engine map list at step %u!\n",
							  nr_steps);
					}

					nr++;
				}

				step.type = ENGINE_MAP;
				goto add_step;
			} else if (!strcmp(field, "T")) {
				int_field(TERMINATE, target,
					  tmp >= 0 || ((int)nr_steps + tmp) < 0,
					  "Invalid terminate target at step %u!\n");
			} else if (!strcmp(field, "X")) {
				unsigned int nr = 0;

				while ((field = strtok_r(fstart, ".", &fctx))) {
					tmp = atoi(field);
					check_arg(nr == 0 && tmp <= 0,
						  "Invalid context at step %u!\n",
						  nr_steps);
					check_arg(nr == 1 && tmp < 0,
						  "Invalid preemption period at step %u!\n",
						  nr_steps);
					check_arg(nr > 1,
						  "Invalid preemption format at step %u!\n",
						  nr_steps);

					if (nr == 0)
						step.context = tmp;
					else
						step.period = tmp;

					nr++;
				}

				step.type = PREEMPTION;
				goto add_step;
			} else if (!strcmp(field, "B")) {
				unsigned int nr = 0;

				while ((field = strtok_r(fstart, ".", &fctx))) {
					tmp = atoi(field);
					check_arg(nr == 0 && tmp <= 0,
						  "Invalid context at step %u!\n",
						  nr_steps);
					check_arg(nr > 0,
						  "Invalid load balance format at step %u!\n",
						  nr_steps);

					step.context = tmp;
					step.load_balance = true;

					nr++;
				}

				step.type = LOAD_BALANCE;
				goto add_step;
			} else if (!strcmp(field, "b")) {
				unsigned int nr = 0;

				if (is_xe) {
					wsim_err("Bonding is not implemented with xe yet.\n");
					free(token);
					return NULL;
				}

				while ((field = strtok_r(fstart, ".", &fctx))) {
					check_arg(nr > 2,
						  "Invalid bond format at step %u!\n",
						  nr_steps);

					if (nr == 0) {
						tmp = atoi(field);
						step.context = tmp;
						check_arg(tmp <= 0,
							  "Invalid context at step %u!\n",
							  nr_steps);
					} else if (nr == 1) {
						tmp = parse_bond_engines(&step, field);
						check_arg(tmp < 0,
							  "Invalid siblings list at step %u!\n",
							  nr_steps);
					} else if (nr == 2) {
						struct intel_engines engines;

						step.bond.master = str_to_engine(field);
						check_arg(append_matching_engines(&step.bond.master,
										  &engines) != 1,
							  "Invalid master engine at step %u!\n",
							  nr_steps);
						free(engines.engines);
					}

					nr++;
				}

				step.type = BOND;
				goto add_step;
			} else if (!strcmp(field, "w") || !strcmp(field, "W")) {
				unsigned int nr = 0;

				if (is_xe) {
					wsim_err("Working sets are not implemented with xe yet.\n");
					free(token);
					return NULL;
				}

				step.working_set.shared = field[0] == 'W';

				while ((field = strtok_r(fstart, ".", &fctx))) {
					tmp = atoi(field);
					if (nr == 0) {
						step.working_set.id = tmp;
					} else {
						tmp = parse_working_set(&step.working_set,
									field);
						check_arg(tmp < 0,
							  "Invalid working set at step %u!\n",
							  nr_steps);
					}

					nr++;
				}

				step.type = WORKINGSET;
				goto add_step;
			}

			if (!field) {
				if (verbose)
					fprintf(stderr,
						"Parse error at step %u!\n",
						nr_steps);
				return NULL;
			}

			tmp = atoi(field);
			check_arg(tmp < 0, "Invalid ctx id at step %u!\n",
				  nr_steps);
			step.context = tmp;

			valid++;
		}

		field = strtok_r(fstart, ".", &fctx);
		if (field) {
			fstart = NULL;

			step.engine = str_to_engine(field);
			check_arg(!is_valid_engine(&step.engine),
				  "Invalid engine id at step %u!\n", nr_steps);

			valid++;
		}

		field = strtok_r(fstart, ".", &fctx);
		if (field) {
			fstart = NULL;

			if (parse_duration(nr_steps, &step.duration, scale_dur, field))
				return NULL;

			valid++;
		}

		field = strtok_r(fstart, ".", &fctx);
		if (field) {
			fstart = NULL;

			tmp = parse_dependencies(nr_steps, &step, field);
			check_arg(tmp < 0,
				  "Invalid dependency at step %u!\n", nr_steps);

			valid++;
		}

		field = strtok_r(fstart, ".", &fctx);
		if (field) {
			fstart = NULL;

			check_arg(strlen(field) != 1 ||
				  (field[0] != '0' && field[0] != '1'),
				  "Invalid wait boolean at step %u!\n",
				  nr_steps);
			step.sync = field[0] - '0';

			valid++;
		}

		check_arg(valid != 5, "Invalid record at step %u!\n", nr_steps);

		step.type = BATCH;

add_step:
		if (step.type == DELAY)
			step.delay = __duration(step.delay, scale_time);

		step.idx = nr_steps++;
		step.rq_link.next = NULL;
		step.rq_link.prev = NULL;
		steps = realloc(steps, sizeof(step) * nr_steps);
		igt_assert(steps);

		memcpy(&steps[nr_steps - 1], &step, sizeof(step));

		free(token);
	}

	if (app_w) {
		steps = realloc(steps, sizeof(step) *
				(nr_steps + app_w->nr_steps));
		igt_assert(steps);

		memcpy(&steps[nr_steps], app_w->steps,
		       sizeof(step) * app_w->nr_steps);

		for (i = 0; i < app_w->nr_steps; i++)
			steps[nr_steps + i].idx += nr_steps;

		nr_steps += app_w->nr_steps;
	}

	wrk = malloc(sizeof(*wrk));
	igt_assert(wrk);

	wrk->nr_steps = nr_steps;
	wrk->steps = steps;
	wrk->prio = arg->prio;
	wrk->sseu = arg->sseu;
	wrk->max_working_set_id = -1;
	wrk->working_sets = NULL;
	wrk->bo_prng = (flags & FLAG_SYNCEDCLIENTS) ? master_prng : rand();

	free(desc);

	/*
	 * Tag all steps which need to emit a sync fence if another step is
	 * referencing them as a sync fence dependency.
	 */
	for (i = 0; i < nr_steps; i++) {
		struct dep_entry *dep;

		for_each_dep(dep, steps[i].fence_deps) {
			tmp = steps[i].idx + dep->target;
			check_arg(tmp < 0 || tmp >= i ||
				  (steps[tmp].type != BATCH &&
				   steps[tmp].type != SW_FENCE),
				  "Invalid dependency target %u!\n", i);
			steps[tmp].emit_fence = -1;
		}
	}

	/* Validate SW_FENCE_SIGNAL targets. */
	for (i = 0; i < nr_steps; i++) {
		if (steps[i].type == SW_FENCE_SIGNAL) {
			tmp = steps[i].idx + steps[i].target;
			check_arg(tmp < 0 || tmp >= i ||
				  steps[tmp].type != SW_FENCE,
				  "Invalid sw fence target %u!\n", i);
		}
	}

	/*
	 * Check no duplicate working set ids.
	 */
	for_each_w_step(w, wrk) {
		struct w_step *w2;

		if (w->type != WORKINGSET)
			continue;

		for_each_w_step(w2, wrk) {
			if (w->idx == w2->idx)
				continue;
			if (w2->type != WORKINGSET)
				continue;

			check_arg(w->working_set.id == w2->working_set.id,
				  "Duplicate working set id at %u!\n", j);
		}
	}

	/*
	 * Allocate shared working sets.
	 */
	for_each_w_step(w, wrk) {
		if (w->type == WORKINGSET && w->working_set.shared) {
			unsigned long total =
				allocate_working_set(wrk, &w->working_set);

			if (verbose > 1)
				printf("%u: %lu bytes in shared working set %u\n",
				       wrk->id, total, w->working_set.id);
		}
	}

	wrk->max_working_set_id = -1;
	for_each_w_step(w, wrk) {
		if (w->type == WORKINGSET &&
		    w->working_set.shared &&
		    w->working_set.id > wrk->max_working_set_id)
			wrk->max_working_set_id = w->working_set.id;
	}

	wrk->working_sets = calloc(wrk->max_working_set_id + 1,
				   sizeof(*wrk->working_sets));
	igt_assert(wrk->working_sets);

	for_each_w_step(w, wrk) {
		if (w->type == WORKINGSET && w->working_set.shared)
			wrk->working_sets[w->working_set.id] = &w->working_set;
	}

	return wrk;
}

static struct workload *
clone_workload(struct workload *_wrk)
{
	int nr_engines = query_engines()->nr_engines;
	struct workload *wrk;
	struct w_step *w;

	wrk = malloc(sizeof(*wrk));
	igt_assert(wrk);
	memset(wrk, 0, sizeof(*wrk));

	wrk->prio = _wrk->prio;
	wrk->sseu = _wrk->sseu;
	wrk->nr_steps = _wrk->nr_steps;
	wrk->steps = calloc(wrk->nr_steps, sizeof(struct w_step));
	igt_assert(wrk->steps);

	memcpy(wrk->steps, _wrk->steps, sizeof(struct w_step) * wrk->nr_steps);

	wrk->max_working_set_id = _wrk->max_working_set_id;
	if (wrk->max_working_set_id >= 0) {
		wrk->working_sets = calloc(wrk->max_working_set_id + 1,
					sizeof(*wrk->working_sets));
		igt_assert(wrk->working_sets);

		memcpy(wrk->working_sets,
		       _wrk->working_sets,
		       (wrk->max_working_set_id + 1) *
		       sizeof(*wrk->working_sets));
	}

	/* Check if we need a sw sync timeline. */
	for_each_w_step(w, wrk) {
		if (w->type == SW_FENCE) {
			wrk->sync_timeline = sw_sync_timeline_create();
			igt_assert(wrk->sync_timeline >= 0);
			break;
		}
	}

	wrk->requests = calloc(nr_engines, sizeof(*wrk->requests));
	igt_assert(wrk->requests);
	wrk->nrequest = calloc(nr_engines, sizeof(*wrk->nrequest));
	igt_assert(wrk->nrequest);
	while (--nr_engines >= 0)
		IGT_INIT_LIST_HEAD(&wrk->requests[nr_engines]);

	return wrk;
}

#define rounddown(x, y) (x - (x%y))
#ifndef PAGE_SIZE
#define PAGE_SIZE (4096)
#endif

static unsigned int get_duration(struct workload *wrk, struct w_step *w)
{
	struct duration *dur = &w->duration;

	if (dur->min == dur->max)
		return dur->min;
	else
		return dur->min + hars_petruska_f54_1_random(&wrk->bb_prng) %
		       (dur->max + 1 - dur->min);
}

static struct ctx *
__get_ctx(struct workload *wrk, const struct w_step *w)
{
	return &wrk->ctx_list[w->context];
}

static uint32_t mmio_base(int i915, const intel_engine_t *engine, int gen)
{
	char name[16];

	if (gen >= 11)
		return 0;

	switch (engine->engine_class) {
	default:
		return 0;

	case DEFAULT_ID:
	case RCS:
		snprintf(name, sizeof(name), "rcs%u", engine->engine_instance);
		break;
	case BCS:
		snprintf(name, sizeof(name), "bcs%u", engine->engine_instance);
		break;
	case VCS:
		snprintf(name, sizeof(name), "vcs%u", engine->engine_instance);
		break;
	case VECS:
		snprintf(name, sizeof(name), "vecs%u", engine->engine_instance);
		break;
	case CCS:
		snprintf(name, sizeof(name), "ccs%u", engine->engine_instance);
		break;
	}

	return gem_engine_mmio_base(i915, name);
}

static unsigned int create_bb(struct w_step *w, int self)
{
	const int gen = intel_gen(intel_get_drm_devid(fd));
	const uint32_t base = mmio_base(fd, &w->engine, gen);
#define CS_GPR(x) (base + 0x600 + 8 * (x))
#define TIMESTAMP (base + 0x3a8)
	const int use_64b = gen >= 8;
	enum { START_TS, NOW_TS };
	uint32_t *cs, *jmp;
	unsigned int r = 0;

	/* Loop until CTX_TIMESTAMP - initial > target ns */

	gem_set_domain(fd, w->bb_handle,
		       I915_GEM_DOMAIN_WC, I915_GEM_DOMAIN_WC);

	if (__gem_set_caching(fd, w->bb_handle, I915_CACHING_CACHED) == 0) {
		cs = gem_mmap__cpu(fd, w->bb_handle,
				   0, w->bb_size,
				   PROT_READ | PROT_WRITE);
	} else {
		cs = gem_mmap__device_coherent(fd,
					       w->bb_handle,
					       0, w->bb_size,
					       PROT_READ | PROT_WRITE);
	}

	/* Store initial 64b timestamp: start */
	*cs++ = MI_LOAD_REGISTER_IMM(1) | MI_CS_MMIO_DST;
	*cs++ = CS_GPR(START_TS) + 4;
	*cs++ = 0;
	*cs++ = MI_LOAD_REGISTER_REG | MI_CS_MMIO_DST | MI_CS_MMIO_SRC;
	*cs++ = TIMESTAMP;
	*cs++ = CS_GPR(START_TS);

	if (offset_in_page(cs) & 4)
		*cs++ = 0;
	jmp = cs;

	if (w->preempt_us) /* Not precise! */
		*cs++ = MI_ARB_CHECK;

	/* Store this 64b timestamp: now */
	*cs++ = MI_LOAD_REGISTER_IMM(1) | MI_CS_MMIO_DST;
	*cs++ = CS_GPR(NOW_TS) + 4;
	*cs++ = 0;
	*cs++ = MI_LOAD_REGISTER_REG | MI_CS_MMIO_DST | MI_CS_MMIO_SRC;
	*cs++ = TIMESTAMP;
	*cs++ = CS_GPR(NOW_TS);

	/* delta = now - start; inverted to match COND_BBE */
	*cs++ = MI_MATH(4);
	*cs++ = MI_MATH_LOAD(MI_MATH_REG_SRCA, MI_MATH_REG(NOW_TS));
	*cs++ = MI_MATH_LOAD(MI_MATH_REG_SRCB, MI_MATH_REG(START_TS));
	*cs++ = MI_MATH_SUB;
	*cs++ = MI_MATH_STOREINV(MI_MATH_REG(NOW_TS), MI_MATH_REG_ACCU);

	/* Save delta for indirect read by COND_BBE */
	*cs++ = MI_STORE_REGISTER_MEM_CMD | (1 + use_64b) | MI_CS_MMIO_DST;
	*cs++ = CS_GPR(NOW_TS);
	w->i915.reloc[r].presumed_offset = w->i915.obj[self].offset;
	w->i915.reloc[r].target_handle = self;
	w->i915.reloc[r].offset = offset_in_page(cs);
	w->i915.reloc[r].delta = 4000;
	*cs++ = w->i915.reloc[r].presumed_offset + w->i915.reloc[r].delta;
	*cs++ = 0;
	r++;

	/* Delay between SRM and COND_BBE to post the writes */
	for (int n = 0; n < 8; n++) {
		*cs++ = MI_STORE_DWORD_INDEX;
		*cs++ = 2048; /* offset into ppHWSP */
		*cs++ = 0;
	}

	/* Break if delta [time elapsed] > target ns (target filled in later) */
	*cs++ = MI_COND_BATCH_BUFFER_END | MI_DO_COMPARE | (1 + use_64b);
	w->i915.bb_duration = cs;
	*cs++ = 0;
	w->i915.reloc[r].presumed_offset = w->i915.obj[self].offset;
	w->i915.reloc[r].target_handle = self;
	w->i915.reloc[r].offset = offset_in_page(cs);
	w->i915.reloc[r].delta = 4000;
	*cs++ = w->i915.reloc[r].presumed_offset + w->i915.reloc[r].delta;
	*cs++ = 0;
	r++;

	/* Otherwise back to recalculating delta */
	*cs++ = MI_BATCH_BUFFER_START | 1 << 8 | use_64b;
	w->i915.reloc[r].presumed_offset = w->i915.obj[self].offset;
	w->i915.reloc[r].target_handle = self;
	w->i915.reloc[r].offset = offset_in_page(cs);
	w->i915.reloc[r].delta = offset_in_page(jmp);
	*cs++ = w->i915.reloc[r].presumed_offset + w->i915.reloc[r].delta;
	*cs++ = 0;
	r++;

	/* returns still mmapped for w->bb_duration to be filled in later */
	return r;
}

static void
eb_update_flags(struct workload *wrk, struct w_step *w)
{
	w->i915.eb.flags = w->engine_idx;
	w->i915.eb.flags |= I915_EXEC_HANDLE_LUT;
	w->i915.eb.flags |= I915_EXEC_NO_RELOC;

	igt_assert(w->emit_fence <= 0);
	if (w->emit_fence)
		w->i915.eb.flags |= I915_EXEC_FENCE_OUT;
}

static uint32_t
get_ctxid(struct workload *wrk, struct w_step *w)
{
	return wrk->ctx_list[w->context].id;
}

static struct xe_exec_queue *
xe_get_eq(struct workload *wrk, const struct w_step *w)
{
	struct ctx *ctx = __get_ctx(wrk, w);

	igt_assert_lt(w->engine_idx, ctx->xe.nr_queues);
	return &ctx->xe.queue_list[w->engine_idx];
}

static struct vm *
get_vm(struct workload *wrk, const struct w_step *w)
{
	return wrk->vm_list;
}

static uint32_t alloc_bo(int i915, unsigned long *size)
{
	uint32_t handle;
	uint64_t sz = *size;

	igt_assert_eq(__gem_create(i915, &sz, &handle), 0);
	igt_assert(sz <= ULONG_MAX);
	*size = sz;

	return handle;
}

static void
alloc_step_batch(struct workload *wrk, struct w_step *w)
{
	struct dep_entry *dep;
	unsigned int j = 0;
	unsigned int nr_obj = 2 + w->data_deps.nr;
	unsigned int objflags = 0;
	uint64_t addr;
	struct vm *vm = get_vm(wrk, w);

	addr = gem_aperture_size(fd) / 2;
	if (addr >> 32)
		objflags |= EXEC_OBJECT_SUPPORTS_48B_ADDRESS;

	if (vm->ahnd)
		objflags |= EXEC_OBJECT_PINNED;

	w->i915.obj = calloc(nr_obj, sizeof(*w->i915.obj));
	igt_assert(w->i915.obj);

	w->bb_size = PAGE_SIZE;
	w->i915.obj[j].handle = alloc_bo(fd, &w->bb_size);
	w->i915.obj[j].flags = EXEC_OBJECT_WRITE;
	if (vm->ahnd) {
		addr = get_offset(vm->ahnd, w->i915.obj[j].handle, w->bb_size, 0);
		w->i915.obj[j].offset = CANONICAL(addr);
		w->i915.obj[j].flags |= objflags;
	}

	j++;
	igt_assert(j < nr_obj);

	for_each_dep(dep, w->data_deps) {
		uint32_t dep_handle;
		uint64_t dep_size;

		if (dep->working_set == -1) {
			int dep_idx = w->idx + dep->target;

			igt_assert(dep->target <= 0);
			igt_assert(dep_idx >= 0 && dep_idx < w->idx);
			igt_assert(wrk->steps[dep_idx].type == BATCH);

			dep_handle = wrk->steps[dep_idx].i915.obj[0].handle;
			dep_size = w->bb_size;
		} else {
			struct working_set *set;

			igt_assert(dep->working_set <=
				   wrk->max_working_set_id);

			set = wrk->working_sets[dep->working_set];

			igt_assert(set->nr);
			igt_assert(dep->target < set->nr);
			igt_assert(set->sizes[dep->target].size);

			dep_handle = set->handles[dep->target];
			dep_size = set->sizes[dep->target].size;
		}

		w->i915.obj[j].flags = dep->write ? EXEC_OBJECT_WRITE : 0;
		w->i915.obj[j].handle = dep_handle;
		if (vm->ahnd) {
			addr = get_offset(vm->ahnd, w->i915.obj[j].handle, dep_size, 0);
			w->i915.obj[j].offset = CANONICAL(addr);
			w->i915.obj[j].flags |= objflags;
		}
		j++;
		igt_assert(j < nr_obj);
	}

	w->bb_handle = w->i915.obj[j].handle = alloc_bo(fd, &w->bb_size);
	if (vm->ahnd) {
		addr = get_offset(vm->ahnd, w->i915.obj[j].handle, w->bb_size, 0);
		w->i915.obj[j].offset = CANONICAL(addr);
		w->i915.obj[j].flags |= objflags;
	}
	w->i915.obj[j].relocation_count = create_bb(w, j);
	if (vm->ahnd) {
		w->i915.obj[j].relocation_count = 0;
	} else {
		igt_assert(w->i915.obj[j].relocation_count <= ARRAY_SIZE(w->i915.reloc));
		w->i915.obj[j].relocs_ptr = to_user_pointer(&w->i915.reloc);
	}

	w->i915.eb.buffers_ptr = to_user_pointer(w->i915.obj);
	w->i915.eb.buffer_count = j + 1;
	w->i915.eb.rsvd1 = get_ctxid(wrk, w);

	eb_update_flags(wrk, w);
#ifdef DEBUG
	printf("%u: %u:|", w->idx, w->i915.eb.buffer_count);
	for (i = 0; i <= j; i++)
		printf("%x|", w->i915.obj[i].handle);
	printf(" flags=%llx bb=%x[%u] ctx[%u]=%u\n",
		w->i915.eb.flags, w->bb_handle, j, w->context,
		get_ctxid(wrk, w));
#endif
}

static void
xe_alloc_step_batch(struct workload *wrk, struct w_step *w)
{
	struct vm *vm = get_vm(wrk, w);
	struct xe_exec_queue *eq = xe_get_eq(wrk, w);
	struct dep_entry *dep;
	int i;

	w->bb_size = xe_bb_size(fd, PAGE_SIZE);
	w->bb_handle = xe_bo_create(fd, vm->id, w->bb_size,
				    vram_if_possible(fd, eq->hwe_list[0].gt_id),
				    DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
	w->xe.data = xe_bo_map(fd, w->bb_handle, w->bb_size);
	w->xe.exec.address =
		intel_allocator_alloc_with_strategy(vm->ahnd, w->bb_handle, w->bb_size,
						    0, ALLOC_STRATEGY_LOW_TO_HIGH);
	xe_vm_bind_sync(fd, vm->id, w->bb_handle, 0, w->xe.exec.address, w->bb_size);
	w->duration.requested_ticks = xe_spin_nsec_to_ticks(fd, eq->hwe_list[0].gt_id,
							    1000LL * get_duration(wrk, w));
	xe_spin_init_opts(&w->xe.data->spin,
			  .addr = w->xe.exec.address,
			  .preempt = (w->preempt_us > 0),
			  .ctx_ticks = w->duration.requested_ticks);
	w->xe.exec.exec_queue_id = eq->id;
	w->xe.exec.num_batch_buffer = 1;
	/* always at least one out fence */
	w->xe.exec.num_syncs = 1;
	/* count syncs */
	for_each_dep(dep, w->data_deps) {
		int dep_idx = w->idx + dep->target;

		igt_assert(dep_idx >= 0 && dep_idx < w->idx);
		igt_assert(wrk->steps[dep_idx].type == BATCH);

		w->xe.exec.num_syncs++;
	}
	for_each_dep(dep, w->fence_deps) {
		int dep_idx = w->idx + dep->target;

		igt_assert(dep_idx >= 0 && dep_idx < w->idx);
		igt_assert(wrk->steps[dep_idx].type == SW_FENCE ||
			   wrk->steps[dep_idx].type == BATCH);

		w->xe.exec.num_syncs++;
	}
	w->xe.syncs = calloc(w->xe.exec.num_syncs, sizeof(*w->xe.syncs));
	/* fill syncs */
	i = 0;
	/* out fence */
	w->xe.syncs[i].handle = syncobj_create(fd, 0);
	w->xe.syncs[i].type = DRM_XE_SYNC_TYPE_SYNCOBJ;
	w->xe.syncs[i++].flags = DRM_XE_SYNC_FLAG_SIGNAL;
	/* in fence(s) */
	for_each_dep(dep, w->data_deps) {
		int dep_idx = w->idx + dep->target;

		igt_assert(wrk->steps[dep_idx].xe.syncs && wrk->steps[dep_idx].xe.syncs[0].handle);
		w->xe.syncs[i].handle = wrk->steps[dep_idx].xe.syncs[0].handle;
		w->xe.syncs[i++].type = DRM_XE_SYNC_TYPE_SYNCOBJ;
	}
	for_each_dep(dep, w->fence_deps) {
		int dep_idx = w->idx + dep->target;

		igt_assert(wrk->steps[dep_idx].xe.syncs && wrk->steps[dep_idx].xe.syncs[0].handle);
		w->xe.syncs[i].handle = wrk->steps[dep_idx].xe.syncs[0].handle;
		w->xe.syncs[i++].type = DRM_XE_SYNC_TYPE_SYNCOBJ;
	}
	w->xe.exec.syncs = to_user_pointer(w->xe.syncs);
}

static bool set_priority(uint32_t ctx_id, int prio)
{
	struct drm_i915_gem_context_param param = {
		.ctx_id = ctx_id,
		.param = I915_CONTEXT_PARAM_PRIORITY,
		.value = prio,
	};

	return __gem_context_set_param(fd, &param) == 0;
}

static bool set_persistence(uint32_t ctx_id, bool state)
{
	struct drm_i915_gem_context_param param = {
		.ctx_id = ctx_id,
		.param = I915_CONTEXT_PARAM_PERSISTENCE,
		.value = state,
	};

	return __gem_context_set_param(fd, &param) == 0;
}

static void __configure_context(uint32_t ctx_id, unsigned int prio)
{
	set_priority(ctx_id, prio);

	/* Mark as non-persistent if supported. */
	set_persistence(ctx_id, false);
}

static int __vm_destroy(int i915, uint32_t vm_id)
{
	struct drm_i915_gem_vm_control ctl = { .vm_id = vm_id };
	int err = 0;

	if (igt_ioctl(i915, DRM_IOCTL_I915_GEM_VM_DESTROY, &ctl)) {
		err = -errno;
		igt_assume(err);
	}

	errno = 0;
	return err;
}

static void vm_destroy(int i915, uint32_t vm_id)
{
	igt_assert_eq(__vm_destroy(i915, vm_id), 0);
}

static struct drm_i915_gem_context_param_sseu get_device_sseu(void)
{
	struct drm_i915_gem_context_param param = { };

	if (device_sseu.slice_mask == -1) {
		param.param = I915_CONTEXT_PARAM_SSEU;
		param.value = (uintptr_t)&device_sseu;

		gem_context_get_param(fd, &param);
	}

	return device_sseu;
}

static uint64_t
set_ctx_sseu(struct ctx *ctx, uint64_t slice_mask)
{
	struct drm_i915_gem_context_param_sseu sseu = get_device_sseu();
	struct drm_i915_gem_context_param param = { };

	if (slice_mask == -1)
		slice_mask = device_sseu.slice_mask;

	if (ctx->engine_map.nr_engines && ctx->load_balance) {
		sseu.flags = I915_CONTEXT_SSEU_FLAG_ENGINE_INDEX;
		sseu.engine.engine_class = I915_ENGINE_CLASS_INVALID;
		sseu.engine.engine_instance = 0;
	}

	sseu.slice_mask = slice_mask;

	param.ctx_id = ctx->id;
	param.param = I915_CONTEXT_PARAM_SSEU;
	param.size = sizeof(sseu);
	param.value = (uintptr_t)&sseu;

	gem_context_set_param(fd, &param);

	return slice_mask;
}

static size_t sizeof_load_balance(int count)
{
	return offsetof(struct i915_context_engines_load_balance,
			engines[count]);
}

static size_t sizeof_param_engines(int count)
{
	return offsetof(struct i915_context_param_engines,
			engines[count]);
}

static size_t sizeof_engines_bond(int count)
{
	return offsetof(struct i915_context_engines_bond,
			engines[count]);
}

static unsigned long
get_buffer_size(struct workload *wrk, const struct work_buffer_size *sz)
{
	if (sz->min == sz->max)
		return sz->min;
	else
		return sz->min + hars_petruska_f54_1_random(&wrk->bo_prng) %
		       (sz->max + 1 - sz->min);
}

static unsigned long
allocate_working_set(struct workload *wrk, struct working_set *set)
{
	unsigned long total = 0;
	unsigned int i;

	set->handles = calloc(set->nr, sizeof(*set->handles));
	igt_assert(set->handles);

	for (i = 0; i < set->nr; i++) {
		set->sizes[i].size = get_buffer_size(wrk, &set->sizes[i]);
		set->handles[i] = alloc_bo(fd, &set->sizes[i].size);
		total += set->sizes[i].size;
	}

	return total;
}

static bool
find_dep(struct dep_entry *deps, unsigned int nr, struct dep_entry dep)
{
	unsigned int i;

	for (i = 0; i < nr; i++) {
		if (deps[i].working_set == dep.working_set &&
		    deps[i].target == dep.target)
			return true;
	}

	return false;
}

static void measure_active_set(struct workload *wrk)
{
	unsigned long total = 0, batch_sizes = 0;
	struct dep_entry *dep, *deps = NULL;
	unsigned int nr = 0;
	struct w_step *w;

	if (verbose < 3)
		return;

	for_each_w_step(w, wrk) {
		if (w->type != BATCH)
			continue;

		batch_sizes += w->bb_size;

		if (is_xe)
			continue;

		for_each_dep(dep, w->data_deps) {
			struct dep_entry _dep = *dep;

			if (dep->working_set == -1 && dep->target < 0) {
				int idx = w->idx + dep->target;

				igt_assert(idx >= 0 && idx < w->idx);
				igt_assert(wrk->steps[idx].type == BATCH);

				_dep.target = wrk->steps[idx].i915.obj[0].handle;
			}

			if (!find_dep(deps, nr, _dep)) {
				if (dep->working_set == -1) {
					total += w->bb_size;
				} else {
					struct working_set *set;

					igt_assert(dep->working_set <=
						   wrk->max_working_set_id);

					set = wrk->working_sets[dep->working_set];
					igt_assert(set->nr);
					igt_assert(dep->target < set->nr);
					igt_assert(set->sizes[dep->target].size);

					total += set->sizes[dep->target].size;
				}

				deps = realloc(deps, (nr + 1) * sizeof(*deps));
				deps[nr++] = *dep;
			}
		}
	}

	free(deps);

	printf("%u: %lu bytes active working set in %u buffers. %lu in batch buffers.\n",
	       wrk->id, total, nr, batch_sizes);
}

#define alloca0(sz) ({ size_t sz__ = (sz); memset(alloca(sz__), 0, sz__); })

static void xe_vm_create_(struct vm *vm)
{
	uint32_t flags = 0;

	if (vm->compute_mode)
		flags |= DRM_XE_VM_CREATE_FLAG_LR_MODE;

	vm->id = xe_vm_create(fd, flags, 0);
}

static void xe_exec_queue_create_(struct ctx *ctx, struct xe_exec_queue *eq)
{
	struct drm_xe_exec_queue_create create = {
		.vm_id = ctx->vm->id,
		.width = 1,
		.num_placements = eq->nr_hwes,
		.instances = to_user_pointer(eq->hwe_list),
	};

	igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_EXEC_QUEUE_CREATE, &create), 0);

	eq->id = create.exec_queue_id;
}

static void allocate_contexts(unsigned int id, struct workload *wrk)
{
	int max_ctx = -1;
	struct w_step *w;

	/*
	 * Pre-scan workload steps to allocate context list storage.
	 */
	for_each_w_step(w, wrk) {
		int ctx = w->context + 1;
		int delta;

		w->wrk = wrk;

		if (ctx <= max_ctx)
			continue;

		delta = ctx + 1 - wrk->nr_ctxs;

		wrk->nr_ctxs += delta;
		wrk->ctx_list = realloc(wrk->ctx_list,
					wrk->nr_ctxs * sizeof(*wrk->ctx_list));
		memset(&wrk->ctx_list[wrk->nr_ctxs - delta], 0,
			delta * sizeof(*wrk->ctx_list));

		max_ctx = ctx;
	}
}

static int prepare_contexts(unsigned int id, struct workload *wrk)
{
	uint32_t share_vm = 0;
	struct w_step *w;
	struct ctx *ctx, *ctx2;
	unsigned int j;

	/*
	 * Transfer over engine map configuration from the workload step.
	 */
	__for_each_ctx(ctx, wrk, ctx_idx) {
		for_each_w_step(w, wrk) {
			if (w->context != ctx_idx)
				continue;

			if (w->type == ENGINE_MAP) {
				ctx->engine_map = w->engine_map;
			} else if (w->type == LOAD_BALANCE) {
				if (!ctx->engine_map.nr_engines) {
					wsim_err("Load balancing needs an engine map!\n");
					return 1;
				}
				if (intel_gen(intel_get_drm_devid(fd)) < 11) {
					wsim_err("Load balancing needs relative mmio support, gen11+!\n");
					return 1;
				}
				ctx->load_balance = w->load_balance;
			} else if (w->type == BOND) {
				if (!ctx->load_balance) {
					wsim_err("Engine bonds need load balancing engine map!\n");
					return 1;
				}
				ctx->bond_count++;
				ctx->bonds = realloc(ctx->bonds,
						     ctx->bond_count *
						     sizeof(struct bond));
				igt_assert(ctx->bonds);
				ctx->bonds[ctx->bond_count - 1] = w->bond;
			}
		}
	}

	/*
	 * Create and configure contexts.
	 */
	__for_each_ctx(ctx, wrk, ctx_idx) {
		struct drm_i915_gem_context_create_ext_setparam ext = {
			.base.name = I915_CONTEXT_CREATE_EXT_SETPARAM,
			.param.param = I915_CONTEXT_PARAM_VM,
		};
		struct drm_i915_gem_context_create_ext args = { };
		uint32_t ctx_id;

		igt_assert(!ctx->id);

		/* Find existing context to share ppgtt with. */
		if (!share_vm)
			for_each_ctx(ctx2, wrk) {
				struct drm_i915_gem_context_param param = {
					.param = I915_CONTEXT_PARAM_VM,
					.ctx_id = ctx2->id,
				};

				if (!param.ctx_id)
					continue;

				gem_context_get_param(fd, &param);
				igt_assert(param.value);
				share_vm = param.value;
				wrk->nr_vms = 1;
				wrk->vm_list = calloc(wrk->nr_vms, sizeof(struct vm));
				igt_assert(wrk->vm_list);
				wrk->vm_list->id = share_vm;
				wrk->vm_list->ahnd = intel_allocator_open(fd, share_vm,
									  INTEL_ALLOCATOR_RELOC);
				ctx2->vm = wrk->vm_list;
				break;
			}

		if (share_vm) {
			ext.param.value = share_vm;
			args.flags = I915_CONTEXT_CREATE_FLAGS_USE_EXTENSIONS;
			args.extensions = to_user_pointer(&ext);
		}

		drmIoctl(fd, DRM_IOCTL_I915_GEM_CONTEXT_CREATE_EXT, &args);
		igt_assert(args.ctx_id);

		ctx_id = args.ctx_id;
		ctx->id = ctx_id;
		ctx->sseu = device_sseu.slice_mask;
		ctx->vm = wrk->vm_list;

		__configure_context(ctx_id, wrk->prio);

		if (ctx->engine_map.nr_engines) {
			struct i915_context_param_engines *set_engines =
				alloca0(sizeof_param_engines(ctx->engine_map.nr_engines + 1));
			struct i915_context_engines_load_balance *load_balance =
				alloca0(sizeof_load_balance(ctx->engine_map.nr_engines));
			struct drm_i915_gem_context_param param = {
				.ctx_id = ctx_id,
				.param = I915_CONTEXT_PARAM_ENGINES,
				.size = sizeof_param_engines(ctx->engine_map.nr_engines + 1),
				.value = to_user_pointer(set_engines),
			};
			struct i915_context_engines_bond *last = NULL;

			/* update engine_idx and request_idx */
			for_each_w_step(w, wrk) {
				if (w->context != ctx_idx)
					continue;
				if (w->type == BATCH) {
					unsigned int map_idx = 0;

					if (find_engine_in_map(&w->engine, &ctx->engine_map,
							       &map_idx))
						/* 0 is virtual, map indexes are shifted by one */
						w->engine_idx = map_idx + 1;
					else
						igt_assert(ctx->load_balance);

					igt_assert(find_engine_in_map(&ctx->engine_map
										.engines[map_idx],
								      query_engines(),
								      &w->request_idx));
				}
			}

			if (ctx->load_balance) {
				set_engines->extensions =
					to_user_pointer(load_balance);

				load_balance->base.name =
					I915_CONTEXT_ENGINES_EXT_LOAD_BALANCE;
				load_balance->num_siblings =
					ctx->engine_map.nr_engines;

				for (j = 0; j < ctx->engine_map.nr_engines; j++)
					load_balance->engines[j] =
						engine_to_i915_engine_class(&ctx->engine_map
										.engines[j]);
			}

			/* Reserve slot for virtual engine. */
			set_engines->engines[0].engine_class =
				I915_ENGINE_CLASS_INVALID;
			set_engines->engines[0].engine_instance =
				I915_ENGINE_CLASS_INVALID_NONE;

			for (j = 1; j <= ctx->engine_map.nr_engines; j++)
				set_engines->engines[j] =
					engine_to_i915_engine_class(&ctx->engine_map
										.engines[j - 1]);

			last = NULL;
			for (j = 0; j < ctx->bond_count; j++) {
				struct intel_engines *mask = &ctx->bonds[j].mask;
				struct i915_context_engines_bond *bond =
					alloca0(sizeof_engines_bond(mask->nr_engines));
				unsigned int b, e;

				bond->base.next_extension = to_user_pointer(last);
				bond->base.name = I915_CONTEXT_ENGINES_EXT_BOND;

				bond->virtual_index = 0;
				bond->master = engine_to_i915_engine_class(&ctx->bonds[j].master);

				for (b = 0, e = 0; e < mask->nr_engines; e++) {
					unsigned int idx;

					igt_assert(find_engine_in_map(&mask->engines[e],
								      &ctx->engine_map,
								      &idx));

					bond->engines[b++] = set_engines->engines[1 + idx];
				}

				last = bond;
			}
			load_balance->base.next_extension = to_user_pointer(last);

			gem_context_set_param(fd, &param);
		} else {
			/* update engine_idx and request_idx */
			for_each_w_step(w, wrk) {
				if (w->context != ctx_idx)
					continue;
				if (w->type == BATCH) {
					w->engine_idx = engine_to_i915_legacy_ring(&w->engine);
					resolve_to_physical_engine(&w->engine);
					igt_assert(find_engine_in_map(&w->engine,
								      query_engines(),
								      &w->request_idx));
				}
			}
		}

		if (wrk->sseu) {
			/* Set to slice 0 only, one slice. */
			ctx->sseu = set_ctx_sseu(ctx, 1);
		}
	}

	if (share_vm)
		vm_destroy(fd, share_vm);

	return 0;
}

static int xe_prepare_contexts(unsigned int id, struct workload *wrk)
{
	struct xe_exec_queue *eq;
	struct w_step *w;
	struct ctx *ctx;
	unsigned int i;

	/* shortcut, create one vm */
	wrk->nr_vms = 1;
	wrk->vm_list = calloc(wrk->nr_vms, sizeof(struct vm));
	igt_assert(wrk->vm_list);
	wrk->vm_list->compute_mode = false;
	xe_vm_create_(wrk->vm_list);
	wrk->vm_list->ahnd = intel_allocator_open(fd, wrk->vm_list->id,
						  INTEL_ALLOCATOR_RELOC);

	__for_each_ctx(ctx, wrk, ctx_idx) {
		/* link with vm */
		ctx->vm = wrk->vm_list;
		for_each_w_step(w, wrk) {
			if (w->context != ctx_idx)
				continue;
			if (w->type == ENGINE_MAP) {
				ctx->engine_map = w->engine_map;
			} else if (w->type == LOAD_BALANCE) {
				if (!ctx->engine_map.nr_engines) {
					wsim_err("Load balancing needs an engine map!\n");
					return 1;
				}
				ctx->load_balance = w->load_balance;
			}
		}

		/* create exec queue for each referenced engine */
		if (ctx->engine_map.nr_engines) {
			ctx->xe.nr_queues = 1;
			ctx->xe.queue_list = calloc(ctx->xe.nr_queues, sizeof(*ctx->xe.queue_list));
			igt_assert(ctx->xe.queue_list);
			eq = &ctx->xe.queue_list[ctx->xe.nr_queues - 1];
			eq->nr_hwes = ctx->engine_map.nr_engines;
			eq->hwe_list = calloc(eq->nr_hwes, sizeof(*eq->hwe_list));
			for (i = 0; i < eq->nr_hwes; ++i) {
				eq->hwe_list[i] = ctx->engine_map.engines[i];

				/* check no mixing classes and no duplicates */
				for (int j = 0; j < i; ++j) {
					if (eq->hwe_list[j].engine_class !=
					    eq->hwe_list[i].engine_class) {
						free(eq->hwe_list);
						eq->nr_hwes = 0;
						wsim_err("Mixing of engine class not supported!\n");
						return 1;
					}

					if (eq->hwe_list[j].engine_instance ==
					    eq->hwe_list[i].engine_instance) {
						free(eq->hwe_list);
						eq->nr_hwes = 0;
						wsim_err("Duplicate engine entry!\n");
						return 1;
					}
				}

				if (verbose > 3)
					printf("%u ctx[%d] %s [%u:%u:%u]\n", id,
					       ctx_idx,
					       intel_engine_class_string(ctx->engine_map
										.engines[i]
										.engine_class),
					       eq->hwe_list[i].engine_class,
					       eq->hwe_list[i].engine_instance,
					       eq->hwe_list[i].gt_id);
			}

			xe_exec_queue_create_(ctx, eq);
		} else {
			/* create engine_map, update engine_idx */
			for_each_w_step(w, wrk) {
				if (w->context != ctx_idx)
					continue;
				if (w->type == BATCH) {
					resolve_to_physical_engine(&w->engine);
					if (!find_engine_in_map(&w->engine, &ctx->engine_map,
								&w->engine_idx)) {
						igt_assert(1 ==
							   append_matching_engines(&w->engine,
										   &ctx->engine_map)
							  );
						w->engine_idx = ctx->engine_map.nr_engines - 1;
					}
				}
			}

			/* skip not referenced context */
			if (!ctx->engine_map.nr_engines)
				continue;

			ctx->xe.nr_queues = ctx->engine_map.nr_engines;
			ctx->xe.queue_list = calloc(ctx->xe.nr_queues, sizeof(*ctx->xe.queue_list));

			for (i = 0; i < ctx->xe.nr_queues; i++) {
				eq = &ctx->xe.queue_list[i];
				eq->nr_hwes = 1;
				eq->hwe_list = calloc(1, sizeof(*eq->hwe_list));
				eq->hwe_list[0] = ctx->engine_map.engines[i];

				if (verbose > 3)
					printf("%u ctx[%d] %s [%d:%d:%d]\n",
					       id, ctx_idx,
					       intel_engine_class_string(ctx->engine_map
										.engines[i]
										.engine_class),
					       eq->hwe_list[0].engine_class,
					       eq->hwe_list[0].engine_instance,
					       eq->hwe_list[0].gt_id);

				xe_exec_queue_create_(ctx, eq);
			}
		}

		/* update request_idx */
		for_each_w_step(w, wrk) {
			if (w->context != ctx_idx)
				continue;
			if (w->type == BATCH) {
				igt_assert(find_engine_in_map(&ctx->engine_map
									.engines[w->engine_idx],
							      query_engines(),
							      &w->request_idx));
			}
		}
	}

	/* create syncobjs for SW_FENCE */
	for_each_w_step(w, wrk)
		if (w->type == SW_FENCE) {
			w->xe.syncs = calloc(1, sizeof(struct drm_xe_sync));
			w->xe.syncs[0].handle = syncobj_create(fd, 0);
			w->xe.syncs[0].type = DRM_XE_SYNC_TYPE_SYNCOBJ;
		}

	return 0;
}

static void prepare_working_sets(unsigned int id, struct workload *wrk)
{
	struct working_set **sets;
	unsigned long total = 0;
	struct w_step *w;

	/*
	 * Allocate working sets.
	 */
	for_each_w_step(w, wrk) {
		if (w->type == WORKINGSET && !w->working_set.shared)
			total += allocate_working_set(wrk, &w->working_set);
	}

	if (verbose > 2)
		printf("%u: %lu bytes in working sets.\n", wrk->id, total);

	/*
	 * Map of working set ids.
	 */
	wrk->max_working_set_id = -1;
	for_each_w_step(w, wrk) {
		if (w->type == WORKINGSET &&
		    w->working_set.id > wrk->max_working_set_id)
			wrk->max_working_set_id = w->working_set.id;
	}

	sets = wrk->working_sets;
	wrk->working_sets = calloc(wrk->max_working_set_id + 1,
				   sizeof(*wrk->working_sets));
	igt_assert(wrk->working_sets);

	for_each_w_step(w, wrk) {
		struct working_set *set;

		if (w->type != WORKINGSET)
			continue;

		if (!w->working_set.shared) {
			set = &w->working_set;
		} else {
			igt_assert(sets);

			set = sets[w->working_set.id];
			igt_assert(set->shared);
			igt_assert(set->sizes);
		}

		wrk->working_sets[w->working_set.id] = set;
	}

	if (sets)
		free(sets);
}

static int prepare_workload(unsigned int id, struct workload *wrk)
{
	struct w_step *w;
	int ret = 0;

	wrk->id = id;
	wrk->bb_prng = (wrk->flags & FLAG_SYNCEDCLIENTS) ? master_prng : rand();
	wrk->bo_prng = (wrk->flags & FLAG_SYNCEDCLIENTS) ? master_prng : rand();
	wrk->run = true;

	allocate_contexts(id, wrk);

	if (is_xe)
		ret = xe_prepare_contexts(id, wrk);
	else
		ret = prepare_contexts(id, wrk);

	if (ret)
		return ret;

	/* Record default preemption. */
	for_each_w_step(w, wrk)
		if (w->type == BATCH)
			w->preempt_us = 100;

	/*
	 * Scan for contexts with modified preemption config and record their
	 * preemption period for the following steps belonging to the same
	 * context.
	 */
	for_each_w_step(w, wrk) {
		struct w_step *w2;

		if (w->type != PREEMPTION)
			continue;

		for (int j = w->idx + 1; j < wrk->nr_steps; j++) {
			w2 = &wrk->steps[j];

			if (w2->context != w->context)
				continue;
			else if (w2->type == PREEMPTION)
				break;
			else if (w2->type != BATCH)
				continue;

			w2->preempt_us = w->period;
		}
	}

	/*
	 * Scan for SSEU control steps.
	 */
	for_each_w_step(w, wrk) {
		if (w->type == SSEU) {
			get_device_sseu();
			break;
		}
	}

	prepare_working_sets(id, wrk);

	/*
	 * Allocate batch buffers.
	 */
	for_each_w_step(w, wrk) {
		if (w->type != BATCH)
			continue;

		if (is_xe)
			xe_alloc_step_batch(wrk, w);
		else
			alloc_step_batch(wrk, w);
	}

	measure_active_set(wrk);

	return ret;
}

static double elapsed(const struct timespec *start, const struct timespec *end)
{
	return (end->tv_sec - start->tv_sec) +
	       (end->tv_nsec - start->tv_nsec) / 1e9;
}

static int elapsed_us(const struct timespec *start, const struct timespec *end)
{
	return elapsed(start, end) * 1e6;
}

static void
update_bb_start(struct workload *wrk, struct w_step *w)
{
	uint32_t ticks;

	/* ticks is inverted for MI_DO_COMPARE (less-than comparison) */
	ticks = 0;
	if (!w->duration.unbound)
		ticks = ~ns_to_ctx_ticks(1000LL * get_duration(wrk, w));

	*w->i915.bb_duration = ticks;
}

static void w_sync_to(struct workload *wrk, struct w_step *w, int target)
{
	if (target < 0)
		target = wrk->nr_steps + target;

	igt_assert(target < wrk->nr_steps);

	while (wrk->steps[target].type != BATCH) {
		if (--target < 0)
			target = wrk->nr_steps + target;
	}

	igt_assert(target < wrk->nr_steps);
	igt_assert(wrk->steps[target].type == BATCH);

	w_step_sync(&wrk->steps[target]);
}

static void do_xe_exec(struct workload *wrk, struct w_step *w)
{
	struct xe_exec_queue *eq = xe_get_eq(wrk, w);

	igt_assert(w->emit_fence <= 0);
	if (w->emit_fence == -1)
		syncobj_reset(fd, &w->xe.syncs[0].handle, 1);

	/* update duration if random */
	if (w->duration.max != w->duration.min) {
		w->duration.requested_ticks = xe_spin_nsec_to_ticks(fd, eq->hwe_list[0].gt_id,
								    1000LL * get_duration(wrk, w));
		xe_spin_init_opts(&w->xe.data->spin,
				  .addr = w->xe.exec.address,
				  .preempt = (w->preempt_us > 0),
				  .ctx_ticks = w->duration.requested_ticks);
	}
	xe_exec(fd, &w->xe.exec);
}

static void xe_w_step_sync_and_verify(struct w_step *w)
{
	if (!is_xe || w->type != BATCH)
		return;

	w_step_sync(w);

	if (!w->duration.unbound) {
		igt_assert(w->duration.requested_ticks && w->xe.data->spin.ticks_delta);
		igt_assert_lte(w->duration.requested_ticks, ~w->xe.data->spin.ticks_delta);
	}
}

static void
do_eb(struct workload *wrk, struct w_step *w)
{
	struct dep_entry *dep;
	unsigned int i;

	eb_update_flags(wrk, w);
	update_bb_start(wrk, w);

	for_each_dep(dep, w->fence_deps) {
		int tgt = w->idx + dep->target;

		/* TODO: fence merging needed to support multiple inputs */
		igt_assert(i == 0);
		igt_assert(tgt >= 0 && tgt < w->idx);
		igt_assert(wrk->steps[tgt].emit_fence > 0);

		if (w->fence_deps.submit_fence)
			w->i915.eb.flags |= I915_EXEC_FENCE_SUBMIT;
		else
			w->i915.eb.flags |= I915_EXEC_FENCE_IN;

		w->i915.eb.rsvd2 = wrk->steps[tgt].emit_fence;
	}

	if (w->i915.eb.flags & I915_EXEC_FENCE_OUT)
		gem_execbuf_wr(fd, &w->i915.eb);
	else
		gem_execbuf(fd, &w->i915.eb);

	if (w->i915.eb.flags & I915_EXEC_FENCE_OUT) {
		w->emit_fence = w->i915.eb.rsvd2 >> 32;
		igt_assert(w->emit_fence > 0);
	}
}

static void sync_deps(struct workload *wrk, struct w_step *w)
{
	unsigned int i;

	for (i = 0; i < w->data_deps.nr; i++) {
		struct dep_entry *entry = &w->data_deps.list[i];
		int dep_idx;

		if (entry->working_set == -1)
			continue;

		igt_assert(entry->target <= 0);

		if (!entry->target)
			continue;

		dep_idx = w->idx + entry->target;

		igt_assert(dep_idx >= 0 && dep_idx < w->idx);
		igt_assert(wrk->steps[dep_idx].type == BATCH);

		w_step_sync(&wrk->steps[dep_idx]);
	}
}

static void *run_workload(void *data)
{
	struct workload *wrk = (struct workload *)data;
	struct timespec t_start, t_end, repeat_start;
	struct w_step *w;
	int throttle = -1;
	int qd_throttle = -1;
	int count, missed = 0;
	unsigned long time_tot = 0, time_min = ULONG_MAX, time_max = 0;

	clock_gettime(CLOCK_MONOTONIC, &t_start);

	for (count = 0; wrk->run && (wrk->background || count < wrk->repeat);
	     count++) {
		unsigned int cur_seqno = wrk->sync_seqno;

		clock_gettime(CLOCK_MONOTONIC, &repeat_start);

		for_each_w_step(w, wrk) {
			int do_sleep = 0;

			if (!wrk->run)
				break;

			if (w->type == DELAY) {
				do_sleep = w->delay;
			} else if (w->type == PERIOD) {
				struct timespec now;
				int elapsed;

				clock_gettime(CLOCK_MONOTONIC, &now);
				elapsed = elapsed_us(&repeat_start, &now);
				do_sleep = w->period - elapsed;
				time_tot += elapsed;
				if (elapsed < time_min)
					time_min = elapsed;
				if (elapsed > time_max)
					time_max = elapsed;
				if (do_sleep < 0) {
					missed++;
					if (verbose > 2)
						printf("%u: Dropped period @ %u/%u (%dus late)!\n",
						       wrk->id, count, w->idx, do_sleep);
					continue;
				}
			} else if (w->type == SYNC) {
				unsigned int s_idx = w->idx + w->target;

				igt_assert(s_idx >= 0 && s_idx < w->idx);
				igt_assert(wrk->steps[s_idx].type == BATCH);
				w_step_sync(&wrk->steps[s_idx]);
				continue;
			} else if (w->type == THROTTLE) {
				throttle = w->throttle;
				continue;
			} else if (w->type == QD_THROTTLE) {
				qd_throttle = w->throttle;
				continue;
			} else if (w->type == SW_FENCE) {
				igt_assert(w->emit_fence < 0);
				w->emit_fence =
					sw_sync_timeline_create_fence(wrk->sync_timeline,
								      cur_seqno + w->idx);
				igt_assert(w->emit_fence > 0);
				if (is_xe)
					/* Convert sync file to syncobj */
					syncobj_import_sync_file(fd, w->xe.syncs[0].handle,
								 w->emit_fence);
				continue;
			} else if (w->type == SW_FENCE_SIGNAL) {
				int tgt = w->idx + w->target;
				int inc;

				igt_assert(tgt >= 0 && tgt < w->idx);
				igt_assert(wrk->steps[tgt].type == SW_FENCE);
				cur_seqno += wrk->steps[tgt].idx;
				inc = cur_seqno - wrk->sync_seqno;
				sw_sync_timeline_inc(wrk->sync_timeline, inc);
				continue;
			} else if (w->type == CTX_PRIORITY) {
				if (w->priority != wrk->ctx_list[w->context].priority) {
					struct drm_i915_gem_context_param param = {
						.ctx_id = wrk->ctx_list[w->context].id,
						.param = I915_CONTEXT_PARAM_PRIORITY,
						.value = w->priority,
					};

					gem_context_set_param(fd, &param);
					wrk->ctx_list[w->context].priority =
								    w->priority;
				}
				continue;
			} else if (w->type == TERMINATE) {
				unsigned int t_idx = w->idx + w->target;

				igt_assert(t_idx >= 0 && t_idx < w->idx);
				igt_assert(wrk->steps[t_idx].type == BATCH);
				igt_assert(wrk->steps[t_idx].duration.unbound);

				if (is_xe)
					xe_spin_end(&wrk->steps[t_idx].xe.data->spin);
				else
					*wrk->steps[t_idx].i915.bb_duration = 0xffffffff;
				__sync_synchronize();
				continue;
			} else if (w->type == SSEU) {
				if (w->sseu != wrk->ctx_list[w->context * 2].sseu) {
					wrk->ctx_list[w->context * 2].sseu =
						set_ctx_sseu(&wrk->ctx_list[w->context * 2],
							     w->sseu);
				}
				continue;
			} else if (w->type == PREEMPTION ||
				   w->type == ENGINE_MAP ||
				   w->type == LOAD_BALANCE ||
				   w->type == BOND ||
				   w->type == WORKINGSET) {
				   /* No action for these at execution time. */
				continue;
			}

			if (do_sleep || w->type == PERIOD) {
				usleep(do_sleep);
				continue;
			}

			igt_assert(w->type == BATCH);

			if (wrk->flags & FLAG_DEPSYNC)
				sync_deps(wrk, w);

			if (throttle > 0)
				w_sync_to(wrk, w, w->idx - throttle);

			if (is_xe)
				do_xe_exec(wrk, w);
			else
				do_eb(wrk, w);

			if (w->rq_link.next) {
				igt_list_del(&w->rq_link);
				wrk->nrequest[w->request_idx]--;
			}
			igt_list_add_tail(&w->rq_link, &wrk->requests[w->request_idx]);
			wrk->nrequest[w->request_idx]++;

			if (!wrk->run)
				break;

			if (w->sync)
				w_step_sync(w);

			if (qd_throttle > 0) {
				while (wrk->nrequest[w->request_idx] > qd_throttle) {
					struct w_step *s;

					s = igt_list_first_entry(&wrk->requests[w->request_idx],
								 s, rq_link);

					w_step_sync(s);

					igt_list_del(&s->rq_link);
					wrk->nrequest[w->request_idx]--;
				}
			}
		}

		if (wrk->sync_timeline) {
			int inc;

			inc = wrk->nr_steps - (cur_seqno - wrk->sync_seqno);
			sw_sync_timeline_inc(wrk->sync_timeline, inc);
			wrk->sync_seqno += wrk->nr_steps;
		}

		/* Cleanup all fences instantiated in this iteration. */
		for_each_w_step(w, wrk) {
			if (!wrk->run)
				break;

			if (w->emit_fence > 0) {
				if (is_xe) {
					igt_assert(w->type == SW_FENCE);
					syncobj_reset(fd, &w->xe.syncs[0].handle, 1);
				}
				close(w->emit_fence);
				w->emit_fence = -1;
			}

			if (wrk->flags & FLAG_VERIFY_COMPLETION)
				xe_w_step_sync_and_verify(w);
		}
	}

	for (int i = query_engines()->nr_engines; --i >= 0;) {
		if (!wrk->nrequest[i])
			continue;

		w = igt_list_last_entry(&wrk->requests[i], w, rq_link);
		w_step_sync(w);
	}

	if (is_xe) {
		for_each_w_step(w, wrk) {
			if (w->type == BATCH) {
				w_step_sync(w);
				syncobj_destroy(fd, w->xe.syncs[0].handle);
				free(w->xe.syncs);
				xe_vm_unbind_sync(fd, get_vm(wrk, w)->id, 0, w->xe.exec.address,
						  w->bb_size);
				gem_munmap(w->xe.data, w->bb_size);
				gem_close(fd, w->bb_handle);
			} else if (w->type == SW_FENCE) {
				syncobj_destroy(fd, w->xe.syncs[0].handle);
				free(w->xe.syncs);
			}
		}
	}

	clock_gettime(CLOCK_MONOTONIC, &t_end);

	if (wrk->print_stats) {
		double t = elapsed(&t_start, &t_end);

		printf("%c%u: %.3fs elapsed (%d cycles, %.3f workloads/s).",
		       wrk->background ? ' ' : '*', wrk->id,
		       t, count, count / t);
		if (time_tot)
			printf(" Time avg/min/max=%lu/%lu/%luus; %u missed.",
			       time_tot / count, time_min, time_max, missed);
		putchar('\n');
	}

	return NULL;
}

static void fini_workload(struct workload *wrk)
{
	free(wrk->steps);
	free(wrk);
}

static void print_help(void)
{
	puts(
"Usage: gem_wsim [OPTIONS]\n"
"\n"
"Runs a simulated workload on the GPU.\n"
"Options:\n"
"  -h                This text.\n"
"  -q                Be quiet - do not output anything to stdout.\n"
"  -I <n>            Initial randomness seed.\n"
"  -p <n>            Context priority to use for the following workload on the\n"
"                    command line.\n"
"  -w <desc|path>    Filename or a workload descriptor.\n"
"                    Can be given multiple times.\n"
"  -W <desc|path>    Filename or a master workload descriptor.\n"
"                    Only one master workload can be optinally specified in which\n"
"                    case all other workloads become background ones and run as\n"
"                    long as the master.\n"
"  -a <desc|path>    Append a workload to all other workloads.\n"
"  -r <n>            How many times to emit the workload.\n"
"  -c <n>            Fork N clients emitting the workload simultaneously.\n"
"  -s                Turn on small SSEU config for the next workload on the\n"
"                    command line. Subsequent -s switches it off.\n"
"  -S                Synchronize the sequence of random batch durations between\n"
"                    clients.\n"
"  -d                Sync between data dependencies in userspace.\n"
"  -f <scale>        Scale factor for batch durations.\n"
"  -F <scale>        Scale factor for delays.\n"
"  -L                List GPUs.\n"
"  -l                List physical engines.\n"
"  -D <gpu>          One of the GPUs from -L.\n"
	);
}

static char *load_workload_descriptor(char *filename)
{
	struct stat sbuf;
	char *buf;
	int infd, ret, i;
	ssize_t len;
	bool in_comment = false;

	ret = stat(filename, &sbuf);
	if (ret || !S_ISREG(sbuf.st_mode))
		return filename;

	igt_assert(sbuf.st_size < 1024 * 1024); /* Just so. */
	buf = malloc(sbuf.st_size);
	igt_assert(buf);

	infd = open(filename, O_RDONLY);
	igt_assert(infd >= 0);
	len = read(infd, buf, sbuf.st_size);
	igt_assert(len == sbuf.st_size);
	close(infd);

	for (i = 0; i < len; i++) {
		/*
		 * Lines starting with '#' are skipped.
		 * If command line step separator (',') is encountered after '#'
		 * it is replaced with ';' to not break parsing.
		 */
		if (buf[i] == '#')
			in_comment = true;
		else if (buf[i] == '\n') {
			buf[i] = ',';
			in_comment = false;
		} else if (in_comment && buf[i] == ',')
			buf[i] = ';';
	}

	len--;
	while (buf[len] == ',')
		buf[len--] = 0;

	return buf;
}

static struct w_arg *
add_workload_arg(struct w_arg *w_args, unsigned int nr_args, char *w_arg,
		 int prio, bool sseu)
{
	w_args = realloc(w_args, sizeof(*w_args) * nr_args);
	igt_assert(w_args);
	w_args[nr_args - 1] = (struct w_arg) { w_arg, NULL, prio, sseu };

	return w_args;
}

static void list_engines(void)
{
	struct intel_engines *engines = query_engines();
	int engine_class_count[NUM_ENGINE_CLASSES] = {};
	unsigned int i;

	for (i = 0; i < engines->nr_engines; ++i) {
		igt_assert_lt(engines->engines[i].engine_class, NUM_ENGINE_CLASSES);
		engine_class_count[engines->engines[i].engine_class]++;
	}

	for (i = 0; i < engines->nr_engines; ++i) {
		if (engine_class_count[engines->engines[i].engine_class] > 1)
			printf("%s%u",
			       intel_engine_class_string(engines->engines[i].engine_class),
			       engines->engines[i].engine_instance + 1);
		else
			printf("%s",
			       intel_engine_class_string(engines->engines[i].engine_class));

		if (is_xe && engines->engines[i].gt_id)
			printf("-%u", engines->engines[i].gt_id);

		if (verbose > 3)
			printf(" [%d:%d:%d]", engines->engines[i].engine_class,
			       engines->engines[i].engine_instance,
			       engines->engines[i].gt_id);
		printf("\n");
	}
}

int main(int argc, char **argv)
{
	struct igt_device_card card = { };
	bool list_devices_arg = false;
	bool list_engines_arg = false;
	unsigned int repeat = 1;
	unsigned int clients = 1;
	unsigned int flags = 0;
	struct timespec t_start, t_end;
	struct workload **w, **wrk = NULL;
	struct workload *app_w = NULL;
	unsigned int nr_w_args = 0;
	int master_workload = -1;
	char *append_workload_arg = NULL;
	struct w_arg *w_args = NULL;
	int exitcode = EXIT_FAILURE;
	char *device_arg = NULL;
	double scale_time = 1.0f;
	double scale_dur = 1.0f;
	int prio = 0;
	double t;
	int i, c, ret;
	char *drm_dev;

	master_prng = time(NULL);

	while ((c = getopt(argc, argv,
			   "LlhqvVsSdc:r:w:W:a:p:I:f:F:D:")) != -1) {
		switch (c) {
		case 'L':
			list_devices_arg = true;
			break;
		case 'l':
			list_engines_arg = true;
			break;
		case 'D':
			device_arg = strdup(optarg);
			break;
		case 'W':
			if (master_workload >= 0) {
				wsim_err("Only one master workload can be given!\n");
				goto err;
			}
			master_workload = nr_w_args;
			/* Fall through */
		case 'w':
			w_args = add_workload_arg(w_args, ++nr_w_args, optarg,
						  prio, flags & FLAG_SSEU);
			break;
		case 'p':
			prio = atoi(optarg);
			break;
		case 'a':
			if (append_workload_arg) {
				wsim_err("Only one append workload can be given!\n");
				goto err;
			}
			append_workload_arg = optarg;
			break;
		case 'c':
			clients = strtol(optarg, NULL, 0);
			break;

		case 'r':
			repeat = strtol(optarg, NULL, 0);
			break;
		case 'q':
			verbose = 0;
			break;
		case 'v':
			verbose++;
			break;
		case 'V':
			flags |= FLAG_VERIFY_COMPLETION;
			break;
		case 'S':
			flags |= FLAG_SYNCEDCLIENTS;
			break;
		case 's':
			flags ^= FLAG_SSEU;
			break;
		case 'd':
			flags |= FLAG_DEPSYNC;
			break;
		case 'I':
			master_prng = strtol(optarg, NULL, 0);
			break;
		case 'f':
			scale_dur = atof(optarg);
			break;
		case 'F':
			scale_time = atof(optarg);
			break;
		case 'h':
			print_help();
			goto out;
		default:
			goto err;
		}
	}

	igt_devices_scan();

	if (list_devices_arg) {
		struct igt_devices_print_format fmt = {
			.type = IGT_PRINT_USER,
			.option = IGT_PRINT_DRM,
		};

		igt_devices_print(&fmt);
		return EXIT_SUCCESS;
	}

	if (device_arg) {
		ret = igt_device_card_match(device_arg, &card);
		if (!ret) {
			wsim_err("Requested device %s not found!\n",
				 device_arg);
			free(device_arg);
			return EXIT_FAILURE;
		}
		free(device_arg);
	} else {
		ret = igt_device_find_first_i915_discrete_card(&card);
		if (!ret)
			ret = igt_device_find_integrated_card(&card);
		if (!ret)
			ret = igt_device_find_first_xe_discrete_card(&card);
		if (!ret)
			ret = igt_device_find_xe_integrated_card(&card);
		if (!ret) {
			wsim_err("No device filter specified and no intel devices found!\n");
			return EXIT_FAILURE;
		}
	}

	if (strlen(card.card)) {
		drm_dev = card.card;
	} else if (strlen(card.render)) {
		drm_dev = card.render;
	} else {
		wsim_err("Failed to detect device!\n");
		return EXIT_FAILURE;
	}

	fd = open(drm_dev, O_RDWR);
	if (fd < 0) {
		wsim_err("Failed to open '%s'! (%s)\n",
			 drm_dev, strerror(errno));
		return EXIT_FAILURE;
	}
	if (verbose > 1)
		printf("Using device %s\n", drm_dev);

	is_xe = is_xe_device(fd);
	if (is_xe)
		xe_device_get(fd);

	if (list_engines_arg) {
		list_engines();
		goto out;
	}

	if (!nr_w_args) {
		wsim_err("No workload descriptor(s)!\n");
		goto err;
	}

	if (nr_w_args > 1 && clients > 1) {
		wsim_err("Cloned clients cannot be combined with multiple workloads!\n");
		goto err;
	}

	if (append_workload_arg) {
		append_workload_arg = load_workload_descriptor(append_workload_arg);
		if (!append_workload_arg) {
			wsim_err("Failed to load append workload descriptor!\n");
			goto err;
		}
	}

	if (append_workload_arg) {
		struct w_arg arg = { NULL, append_workload_arg, 0 };

		app_w = parse_workload(&arg, flags, scale_dur, scale_time,
				       NULL);
		if (!app_w) {
			wsim_err("Failed to parse append workload!\n");
			goto err;
		}
	}

	wrk = calloc(nr_w_args, sizeof(*wrk));
	igt_assert(wrk);

	for (i = 0; i < nr_w_args; i++) {
		w_args[i].desc = load_workload_descriptor(w_args[i].filename);

		if (!w_args[i].desc) {
			wsim_err("Failed to load workload descriptor %u!\n", i);
			goto err;
		}

		wrk[i] = parse_workload(&w_args[i], flags, scale_dur,
					scale_time, app_w);
		if (!wrk[i]) {
			wsim_err("Failed to parse workload %u!\n", i);
			goto err;
		}
	}

	if (nr_w_args > 1)
		clients = nr_w_args;

	if (verbose > 1) {
		printf("Random seed is %u.\n", master_prng);
		printf("%u client%s.\n", clients, clients > 1 ? "s" : "");
	}

	srand(master_prng);
	master_prng = rand();

	if (master_workload >= 0 && clients == 1)
		master_workload = -1;

	w = calloc(clients, sizeof(struct workload *));
	igt_assert(w);

	for (i = 0; i < clients; i++) {
		w[i] = clone_workload(wrk[nr_w_args > 1 ? i : 0]);

		w[i]->flags = flags;
		w[i]->repeat = repeat;
		w[i]->background = master_workload >= 0 && i != master_workload;
		w[i]->print_stats = verbose > 1 ||
				    (verbose > 0 && master_workload == i);

		if (prepare_workload(i, w[i])) {
			wsim_err("Failed to prepare workload %u!\n", i);
			goto err;
		}
	}

	clock_gettime(CLOCK_MONOTONIC, &t_start);

	for (i = 0; i < clients; i++) {
		ret = pthread_create(&w[i]->thread, NULL, run_workload, w[i]);
		igt_assert_eq(ret, 0);
	}

	if (master_workload >= 0) {
		ret = pthread_join(w[master_workload]->thread, NULL);
		igt_assert_eq(ret, 0);

		for (i = 0; i < clients; i++)
			w[i]->run = false;
	}

	for (i = 0; i < clients; i++) {
		if (master_workload != i) {
			ret = pthread_join(w[i]->thread, NULL);
			igt_assert_eq(ret, 0);
		}
	}

	clock_gettime(CLOCK_MONOTONIC, &t_end);

	t = elapsed(&t_start, &t_end);
	if (verbose)
		printf("%.3fs elapsed (%.3f workloads/s)\n",
		       t, clients * repeat / t);

	for (i = 0; i < clients; i++)
		fini_workload(w[i]);
	free(w);
	for (i = 0; i < nr_w_args; i++)
		fini_workload(wrk[i]);
	free(w_args);

out:
	exitcode = EXIT_SUCCESS;
err:
	if (is_xe)
		xe_device_put(fd);

	igt_thread_assert_no_failures();

	return exitcode;
}
