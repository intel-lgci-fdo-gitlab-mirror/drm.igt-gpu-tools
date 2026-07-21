// SPDX-License-Identifier: MIT
/*
 * Copyright © 2024 Intel Corporation
 */

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "igt_core.h"
#include "intel_batchbuffer.h"
#include "intel_chipset.h"
#include "ioctl_wrappers.h"
#include "linux_scaffold.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_oa.h"
#include "xe/xe_oa_data.h"
#include "xe/xe_query.h"

#include "xe_perf_recorder_commands.h"

#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define OAG_MMIOTRIGGER 0xdb1c
#define OAMERT_MMIOTRIGGER 0x1453cc
#define OAM_MMIOTRIGGER_OFFSET 0x1d0
#define MEDIA_GT_GSI_OFFSET 0x380000
#define XE_OAM_SAG_BASE_ADJ (MEDIA_GT_GSI_OFFSET + 0x13000)
#define XE_OAM_SCMI_0_BASE_ADJ (MEDIA_GT_GSI_OFFSET + 0x14000)
#define XE_OAM_SCMI_1_BASE_ADJ (MEDIA_GT_GSI_OFFSET + 0x14800)
#define OAREPORT_REASON_MASK 0x3f
#define OAREPORT_REASON_SHIFT 19

struct circular_buffer {
	char   *data;
	size_t  allocated_size;
	size_t  size;
	size_t  beginpos;
	size_t  endpos;
};

struct chunk {
	char *data;
	size_t len;
};

static size_t
circular_available_size(const struct circular_buffer *buffer)
{
	assert(buffer->size <= buffer->allocated_size);
	return buffer->allocated_size - buffer->size;
}

static void
get_chunks(struct chunk *chunks, struct circular_buffer *buffer, bool write, size_t len)
{
	size_t offset = write ? buffer->endpos : buffer->beginpos;

	if (write)
		assert(circular_available_size(buffer) >= len);
	else
		assert(buffer->size >= len);

	chunks[0].data = &buffer->data[offset];

	if ((offset + len) > buffer->allocated_size) {
		chunks[0].len = buffer->allocated_size - offset;
		chunks[1].data = buffer->data;
		chunks[1].len = len - (buffer->allocated_size - offset);
	} else {
		chunks[0].len = len;
		chunks[1].data = NULL;
		chunks[1].len = 0;
	}
}

static ssize_t
circular_buffer_read(void *c, char *buf, size_t size)
{
	struct circular_buffer *buffer = c;
	struct chunk chunks[2];

	if (buffer->size < size)
		return -1;

	get_chunks(chunks, buffer, false, size);

	memcpy(buf, chunks[0].data, chunks[0].len);
	memcpy(buf + chunks[0].len, chunks[1].data, chunks[1].len);
	buffer->beginpos = (buffer->beginpos + size) % buffer->allocated_size;
	buffer->size -= size;

	return size;
}

static size_t
peek_item_size(struct circular_buffer *buffer)
{
	struct intel_xe_perf_record_header header;
	struct chunk chunks[2];

	if (!buffer->size)
		return 0;

	assert(buffer->size >= sizeof(header));

	get_chunks(chunks, buffer, false, sizeof(header));
	memcpy(&header, chunks[0].data, chunks[0].len);
	memcpy((char *) &header + chunks[0].len, chunks[1].data, chunks[1].len);

	return header.size;
}

static void
circular_shrink(struct circular_buffer *buffer, size_t size)
{
	size_t shrank = 0, item_size;

	assert(size <= buffer->allocated_size);

	while (shrank < size && buffer->size > (item_size = peek_item_size(buffer))) {
		assert(item_size > 0 && item_size <= buffer->allocated_size);

		buffer->beginpos = (buffer->beginpos + item_size) % buffer->allocated_size;
		buffer->size -= item_size;

		shrank += item_size;
	}
}

static ssize_t
circular_buffer_write(void *c, const char *buf, size_t _size)
{
	struct circular_buffer *buffer = c;
	size_t size = _size;

	while (size) {
		size_t avail = circular_available_size(buffer), item_size;
		struct chunk chunks[2];

		/* Make space in the buffer if there is too much data. */
		if (avail < size)
			circular_shrink(buffer, size - avail);

		item_size = MIN(circular_available_size(buffer), size);

		get_chunks(chunks, buffer, true, item_size);

		memcpy(chunks[0].data, buf, chunks[0].len);
		memcpy(chunks[1].data, buf + chunks[0].len, chunks[1].len);

		buf += item_size;
		size -= item_size;

		buffer->endpos = (buffer->endpos + item_size) % buffer->allocated_size;
		buffer->size += item_size;
	}

	return _size;
}

static int
circular_buffer_seek(void *c, off64_t *offset, int whence)
{
	return -1;
}

static int
circular_buffer_close(void *c)
{
	return 0;
}

cookie_io_functions_t circular_buffer_functions = {
	.read  = circular_buffer_read,
	.write = circular_buffer_write,
	.seek  = circular_buffer_seek,
	.close = circular_buffer_close,
};


static bool
read_file_uint64(const char *file, uint64_t *value)
{
	char buf[32];
	int fd, n;

	fd = open(file, 0);
	if (fd < 0)
		return false;
	n = read(fd, buf, sizeof (buf) - 1);
	close(fd);
	if (n < 0)
		return false;

	buf[n] = '\0';
	*value = strtoull(buf, 0, 0);

	return true;
}

static uint32_t
read_device_param(const char *stem, int id, const char *param)
{
	char *name;
	int ret = asprintf(&name, "/sys/class/drm/%s%u/device/%s", stem, id, param);
	uint64_t value;
	bool success;

	assert(ret != -1);

	success = read_file_uint64(name, &value);
	free(name);

	return success ? value : 0;
}

static int
find_intel_render_node(void)
{
	for (int i = 128; i < (128 + 16); i++) {
		if (read_device_param("renderD", i, "vendor") == 0x8086)
			return i;
	}

	return -1;
}

static void
print_intel_devices(void)
{
	fprintf(stdout, "Available devices:\n");
	for (int i = 0; i < 128; i++) {
		if (read_device_param("card", i, "vendor") == 0x8086) {
			uint32_t devid = read_device_param("card", i, "device");
			const struct intel_device_info *devinfo =
				intel_get_device_info(devid);
			fprintf(stdout, "   %i: %s (0x%04hx)\n", i,
				devinfo ? devinfo->codename : "unknwon",
				devid);
		}
	}
}

static int
open_render_node(uint32_t *devid, int card)
{
	char *name;
	int ret;
	int fd;
	int render;

	if (card < 0) {
		render = find_intel_render_node();
		if (render < 0)
			return -1;
	} else {
		render = 128 + card;
	}

	ret = asprintf(&name, "/dev/dri/renderD%u", render);
	assert(ret != -1);

	*devid = read_device_param("renderD", render, "device");

	fd = open(name, O_RDWR);
	free(name);

	return fd;
}

static uint32_t
oa_exponent_for_period(uint64_t device_timestamp_frequency, double period)
{
	uint64_t period_ns = 1000 * 1000 * 1000 * period;
	uint64_t device_periods[32];

	for (uint32_t i = 0; i < ARRAY_SIZE(device_periods); i++)
		device_periods[i] = 1000000000ull * (1u << i) / device_timestamp_frequency;

	for (uint32_t i = 1; i < ARRAY_SIZE(device_periods); i++) {
		if (period_ns >= device_periods[i - 1] &&
		    period_ns < device_periods[i]) {
			if ((device_periods[i] - period_ns) >
			    (period_ns - device_periods[i - 1]))
				return i - 1;
			return i;
		}
	}

	return -1;
}

static int
perf_ioctl(int fd, unsigned long request, void *arg)
{
	int ret;

	do {
		ret = ioctl(fd, request, arg);
	} while (ret == -1 && (errno == EINTR || errno == EAGAIN));

	return ret;
}

static uint64_t
get_device_cs_timestamp_frequency(int drm_fd)
{
	return xe_gt_list(drm_fd)->gt_list[0].reference_clock;
}

static uint64_t
get_device_oa_timestamp_frequency(int drm_fd)
{
	struct drm_xe_query_oa_units *qoa = xe_oa_units(drm_fd);
	struct drm_xe_oa_unit *oau = (struct drm_xe_oa_unit *)&qoa->oa_units[0];

	return oau->oa_timestamp_freq;
}

struct recording_context {
	int drm_fd;
	int perf_fd;

	uint32_t devid;
	uint64_t oa_timestamp_frequency;
	uint64_t cs_timestamp_frequency;

	const struct intel_device_info *devinfo;

	struct intel_xe_topology_info *topology;
	uint32_t topology_size;

	struct intel_xe_perf *perf;
	struct intel_xe_perf_metric_set *metric_set;

	uint32_t oa_exponent;

	struct circular_buffer circular_buffer;
	FILE *output_stream;

	const char *command_fifo;
	int command_fifo_fd;

	int oa_unit_id;
	struct drm_xe_oa_unit *oa_unit;
	struct drm_xe_engine_class_instance *hwe;

	uint32_t vm;
	uint32_t exec_queue;
	struct intel_bb *ibb;
};

static uint32_t oa_unit_mmio_trigger_reg(struct recording_context *ctx)
{
	const struct drm_xe_oa_unit *oau = ctx->oa_unit;

	switch (oau->oa_unit_type) {
	case DRM_XE_OA_UNIT_TYPE_OAM: {
		struct drm_xe_query_oa_units *qoa = xe_oa_units(ctx->drm_fd);
		uint8_t *poau = (uint8_t *)&qoa->oa_units[0];
		int first_oam_id = -1;

		/* Find the first OAM unit, as in oa_unit_by_type() */
		for (int i = 0; i < qoa->num_oa_units; i++) {
			struct drm_xe_oa_unit *u = (struct drm_xe_oa_unit *)poau;

			if (u->oa_unit_type == DRM_XE_OA_UNIT_TYPE_OAM) {
				first_oam_id = u->oa_unit_id;
				break;
			}
			poau += sizeof(*u) + u->num_engines * sizeof(u->eci[0]);
		}

		assert(first_oam_id != -1);

		if (oau->oa_unit_id == first_oam_id)
			return XE_OAM_SCMI_0_BASE_ADJ + OAM_MMIOTRIGGER_OFFSET;
		return XE_OAM_SCMI_1_BASE_ADJ + OAM_MMIOTRIGGER_OFFSET;
	}
	case DRM_XE_OA_UNIT_TYPE_OAM_SAG:
		return XE_OAM_SAG_BASE_ADJ + OAM_MMIOTRIGGER_OFFSET;
	case DRM_XE_OA_UNIT_TYPE_MERT:
		return OAMERT_MMIOTRIGGER;
	case DRM_XE_OA_UNIT_TYPE_OAG:
		return OAG_MMIOTRIGGER;
	default:
		assert(0);
	}
}

static void set_fd_flags(int fd, int flags)
{
	int old = fcntl(fd, F_GETFL, 0);

	igt_assert_lte(0, old);
	igt_assert_eq(0, fcntl(fd, F_SETFL, old | flags));
}

enum xe_oa_report_header {
	HDR_32_BIT = 0,
	HDR_64_BIT,
};

struct xe_oa_format {
	uint32_t counter_select;
	int size;
	int oa_type;
	enum xe_oa_report_header header;
	uint16_t counter_size;
	uint16_t bc_report;
};

#define DRM_FMT(x) DRM_XE_OA_FMT_TYPE_##x

static const struct xe_oa_format oa_formats[] = {
	[XE_OA_FORMAT_C4_B8]			= { 7, 64,  DRM_FMT(OAG) },
	[XE_OA_FORMAT_A12]			= { 0, 64,  DRM_FMT(OAG) },
	[XE_OA_FORMAT_A12_B8_C8]		= { 2, 128, DRM_FMT(OAG) },
	[XE_OA_FORMAT_A32u40_A4u32_B8_C8]	= { 5, 256, DRM_FMT(OAG) },
	[XE_OAR_FORMAT_A32u40_A4u32_B8_C8]	= { 5, 256, DRM_FMT(OAR) },
	[XE_OA_FORMAT_A24u40_A14u32_B8_C8]	= { 5, 256, DRM_FMT(OAG) },
	[XE_OAC_FORMAT_A24u64_B8_C8]		= { 1, 320, DRM_FMT(OAC), HDR_64_BIT },
	[XE_OAC_FORMAT_A22u32_R2u32_B8_C8]	= { 2, 192, DRM_FMT(OAC), HDR_64_BIT },
	[XE_OAM_FORMAT_MPEC8u64_B8_C8]		= { 1, 192, DRM_FMT(OAM_MPEC), HDR_64_BIT },
	[XE_OAM_FORMAT_MPEC8u32_B8_C8]		= { 2, 128, DRM_FMT(OAM_MPEC), HDR_64_BIT },
	[XE_OA_FORMAT_PEC64u64]			= { 1, 576, DRM_FMT(PEC), HDR_64_BIT, 1, 0 },
	[XE_OA_FORMAT_PEC64u64_B8_C8]		= { 1, 640, DRM_FMT(PEC), HDR_64_BIT, 1, 1 },
	[XE_OA_FORMAT_PEC64u32]			= { 1, 320, DRM_FMT(PEC), HDR_64_BIT },
	[XE_OA_FORMAT_PEC32u64_G1]		= { 5, 320, DRM_FMT(PEC), HDR_64_BIT, 1, 0 },
	[XE_OA_FORMAT_PEC32u32_G1]		= { 5, 192, DRM_FMT(PEC), HDR_64_BIT },
	[XE_OA_FORMAT_PEC32u64_G2]		= { 6, 320, DRM_FMT(PEC), HDR_64_BIT, 1, 0 },
	[XE_OA_FORMAT_PEC32u32_G2]		= { 6, 192, DRM_FMT(PEC), HDR_64_BIT },
	[XE_OA_FORMAT_PEC36u64_G1_32_G2_4]	= { 3, 320, DRM_FMT(PEC), HDR_64_BIT, 1, 0 },
	[XE_OA_FORMAT_PEC36u64_G1_4_G2_32]	= { 4, 320, DRM_FMT(PEC), HDR_64_BIT, 1, 0 },
};

static uint64_t oa_format_fields(uint64_t name)
{
#define FIELD_PREP_ULL(_mask, _val) \
	(((_val) << (__builtin_ffsll(_mask) - 1)) & (_mask))

	struct xe_oa_format f = oa_formats[name];

	/* 0 format name is invalid */
	if (!name)
		memset(&f, 0xff, sizeof(f));

	return FIELD_PREP_ULL(DRM_XE_OA_FORMAT_MASK_FMT_TYPE, (u64)f.oa_type) |
		FIELD_PREP_ULL(DRM_XE_OA_FORMAT_MASK_COUNTER_SEL, (u64)f.counter_select) |
		FIELD_PREP_ULL(DRM_XE_OA_FORMAT_MASK_COUNTER_SIZE, (u64)f.counter_size) |
		FIELD_PREP_ULL(DRM_XE_OA_FORMAT_MASK_BC_REPORT, (u64)f.bc_report);
#undef FIELD_PREP_ULL
}
#define __ff oa_format_fields

static int
perf_open(struct recording_context *ctx)
{
	int stream_fd;

	uint64_t properties[] = {
		DRM_XE_OA_PROPERTY_OA_UNIT_ID, ctx->oa_unit->oa_unit_id,

		/* Include OA reports in samples */
		DRM_XE_OA_PROPERTY_SAMPLE_OA, true,

		/* OA unit configuration */
		DRM_XE_OA_PROPERTY_OA_METRIC_SET, ctx->metric_set->perf_oa_metrics_set,
		DRM_XE_OA_PROPERTY_OA_FORMAT, __ff(ctx->metric_set->perf_oa_format),
		DRM_XE_OA_PROPERTY_OA_PERIOD_EXPONENT, ctx->oa_exponent,
	};
	struct intel_xe_oa_open_prop param = {
		.num_properties = ARRAY_SIZE(properties) / 2,
		.properties_ptr = to_user_pointer(properties),
	};

	stream_fd = intel_xe_perf_ioctl(ctx->drm_fd, DRM_XE_OBSERVATION_OP_STREAM_OPEN, &param);
	if (stream_fd < 0) {
		errno = 0;
		goto exit;
	}

	set_fd_flags(stream_fd, O_CLOEXEC | O_NONBLOCK);
exit:
	return stream_fd;
}

static bool quit = false;

static void
sigint_handler(int val)
{
	quit = true;
}

static bool
write_version(FILE *output, struct recording_context *ctx)
{
	struct intel_xe_perf_record_version version = {
		.version = INTEL_XE_PERF_RECORD_VERSION,
	};
	struct intel_xe_perf_record_header header = {
		.type = INTEL_XE_PERF_RECORD_TYPE_VERSION,
		.size = sizeof(header) + sizeof(version),
	};

	if (fwrite(&header, sizeof(header), 1, output) != 1)
		return false;

	if (fwrite(&version, sizeof(version), 1, output) != 1)
		return false;

	return true;
}

static bool
write_header(FILE *output, struct recording_context *ctx)
{
	struct intel_xe_perf_record_device_info info = {
		.timestamp_frequency = ctx->oa_timestamp_frequency,
		.device_id = ctx->perf->devinfo.devid,
		.device_revision = ctx->perf->devinfo.revision,
		.gt_min_frequency = ctx->perf->devinfo.gt_min_freq,
		.gt_max_frequency = ctx->perf->devinfo.gt_max_freq,
		.oa_format = ctx->metric_set->perf_oa_format,
		.engine_class = ctx->hwe->engine_class,
		.engine_instance = ctx->hwe->engine_instance,
	};
	struct intel_xe_perf_record_header header = {
		.type = INTEL_XE_PERF_RECORD_TYPE_DEVICE_INFO,
		.size = sizeof(header) + sizeof(info),
	};

	snprintf(info.metric_set_name, sizeof(info.metric_set_name),
		 "%s", ctx->metric_set->symbol_name);
	snprintf(info.metric_set_uuid, sizeof(info.metric_set_uuid),
		 "%s", ctx->metric_set->hw_config_guid);

	if (fwrite(&header, sizeof(header), 1, output) != 1)
		return false;

	if (fwrite(&info, sizeof(info), 1, output) != 1)
		return false;

	return true;
}

static struct intel_xe_topology_info *get_topology(struct recording_context *ctx)
{
	return xe_fill_topology_info(ctx->drm_fd, ctx->devid, &ctx->topology_size);
}

static bool
write_topology(FILE *output, struct recording_context *ctx)
{
	struct intel_xe_perf_record_header header = {
		.type = INTEL_XE_PERF_RECORD_TYPE_DEVICE_TOPOLOGY,
	};

	header.size = sizeof(header) + ctx->topology_size;
	if (fwrite(&header, sizeof(header), 1, output) != 1)
		return false;

	if (fwrite(ctx->topology, ctx->topology_size, 1, output) != 1)
		return false;

	return true;
}

static int get_stream_status(int perf_fd, u32 *oa_status)
{
	struct drm_xe_oa_stream_status status;
	int ret;

	ret = perf_ioctl(perf_fd, DRM_XE_OBSERVATION_IOCTL_STATUS, &status);
	if (ret)
		return ret;

	*oa_status = status.oa_status;
	return 0;
}

static bool write_stream_status(struct recording_context *ctx, FILE *output)
{
	u32 oa_status;

	if (!get_stream_status(ctx->perf_fd, &oa_status)) {
		struct intel_xe_perf_record_header header = { .size = sizeof(header) };

		if (oa_status & DRM_XE_OASTATUS_REPORT_LOST)
			header.type = INTEL_XE_PERF_RECORD_OA_TYPE_REPORT_LOST;
		else if (oa_status & DRM_XE_OASTATUS_BUFFER_OVERFLOW)
			header.type = INTEL_XE_PERF_RECORD_OA_TYPE_BUFFER_LOST;
		else
			return true;

		if (fwrite(&header, sizeof(header), 1, output) != 1)
			return false;
	}

	return true;
}

static uint32_t
report_reason(const uint32_t *report)
{
	return (report[0] >> OAREPORT_REASON_SHIFT) & OAREPORT_REASON_MASK;
}

static void
check_mmio_trigger_report(struct recording_context *ctx, const void *report)
{
	const struct xe_oa_format *fmt = &oa_formats[ctx->metric_set->perf_oa_format];
	const uint32_t *report_32 = report;
	uint64_t value;

	if (report_reason(report_32))
		return;

	value = (fmt->header == HDR_64_BIT) ? ((const uint64_t *)report)[2] : report_32[2];

	if (value == 0xc0ffee01 || value == 0xc0ffee02)
		fprintf(stdout, "Received trigger report with value 0x%" PRIx64 "\n", value);
}

static bool write_stream_data(struct recording_context *ctx,
			      char *data, ssize_t size, FILE *output)
{
	ssize_t format_size = oa_formats[ctx->metric_set->perf_oa_format].size;

	assert(!(size % format_size));

	for (int i = 0; i < size / format_size; i++) {
		struct intel_xe_perf_record_header header = {
			.type = INTEL_XE_PERF_RECORD_TYPE_SAMPLE,
			.size = sizeof(header) + format_size,
		};

		check_mmio_trigger_report(ctx, data);

		if (fwrite(&header, sizeof(header), 1, output) != 1)
			return false;

		if (fwrite(data, format_size, 1, output) != 1)
			return false;

		data += format_size;
	}

	return true;
}

static bool write_perf_data(FILE *output, struct recording_context *ctx)
{
	char data[4096];
	ssize_t len;
	bool ret;

	while (1) {
		len = read(ctx->perf_fd, data, sizeof(data));

		if (len < 0) {
			switch (errno) {
			case EIO:
				ret = write_stream_status(ctx, output);
				if (!ret)
					return ret;
				break;
			case EAGAIN:
			case EINTR:
				return true;
			default:
				/* Not expecting -EFAULT, -ENOSPC, -EINVAL */
				assert(0);
			}
		} else {
			ret = write_stream_data(ctx, data, len, output);
			if (!ret)
				return ret;
		}
	}

	/* Should not reach here */
	return false;
}

static clock_t correlation_clock_id = CLOCK_MONOTONIC;

static const char *
get_correlation_clock_name(clock_t clock_id)
{
	switch (clock_id) {
	case CLOCK_BOOTTIME:      return "bootime";
	case CLOCK_MONOTONIC:     return "monotonic";
	case CLOCK_MONOTONIC_RAW: return "monotonic_raw";
	default:                  return "*unknown*";
	}
}

static int query_engine_cycles(int fd, struct drm_xe_query_engine_cycles *ts)
{
	struct drm_xe_device_query query = {
		.extensions = 0,
		.query = DRM_XE_DEVICE_QUERY_ENGINE_CYCLES,
		.size = sizeof(*ts),
		.data = (uintptr_t)ts,
	};

	return perf_ioctl(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query);
}

static bool get_correlation_timestamps(struct recording_context *ctx,
				       struct intel_xe_perf_record_timestamp_correlation *corr)
{
	struct drm_xe_query_engine_cycles ts = {};

	ts.eci = *ctx->hwe;
	ts.clockid = correlation_clock_id;

	if (query_engine_cycles(ctx->drm_fd, &ts))
		return false;

	corr->cpu_timestamp = ts.cpu_timestamp + ts.cpu_delta / 2;
	corr->gpu_timestamp = ts.engine_cycles;

	return true;
}

static bool
write_saved_correlation_timestamps(FILE *output,
				   const struct intel_xe_perf_record_timestamp_correlation *corr)
{
	struct intel_xe_perf_record_header header = {
		.type = INTEL_XE_PERF_RECORD_TYPE_TIMESTAMP_CORRELATION,
		.size = sizeof(header) + sizeof(*corr),
	};

	if (fwrite(&header, sizeof(header), 1, output) != 1)
		return false;

	if (fwrite(corr, sizeof(*corr), 1, output) != 1)
		return false;

	return true;
}

static bool
write_correlation_timestamps(struct recording_context *ctx, FILE *output)
{
	struct intel_xe_perf_record_timestamp_correlation corr;

	if (!get_correlation_timestamps(ctx, &corr))
		return false;

	return write_saved_correlation_timestamps(output, &corr);
}

static void emit_mmio_triggered_report(struct intel_bb *ibb, uint32_t reg, uint32_t value)
{
	intel_bb_out(ibb, MI_LOAD_REGISTER_IMM(1));
	intel_bb_out(ibb, reg);
	intel_bb_out(ibb, value);
}

static void emit_oa_trigger(struct recording_context *ctx, uint32_t value)
{
	emit_mmio_triggered_report(ctx->ibb, oa_unit_mmio_trigger_reg(ctx), value);

	intel_bb_flush_render(ctx->ibb);
	intel_bb_sync(ctx->ibb);
	intel_bb_reset(ctx->ibb, false);
}

static void
read_command_file(struct recording_context *ctx)
{
	struct recorder_command_base header;
	ssize_t ret = read(ctx->command_fifo_fd, &header, sizeof(header));

	if (ret < 0)
		return;

	switch (header.command) {
	case RECORDER_COMMAND_DUMP: {
		uint32_t len = header.size - sizeof(header), offset = 0;
		uint8_t *dump = malloc(len);
		FILE *file;

		emit_oa_trigger(ctx, 0xc0ffee02);
		write_perf_data(ctx->output_stream, ctx);

		while (offset < len &&
		       ((ret = read(ctx->command_fifo_fd,
				    (void *) dump + offset, len - offset)) > 0
			|| errno == EAGAIN)) {
			if (ret > 0)
				offset += ret;
		}

		fprintf(stdout, "Writing circular buffer to %s\n", dump);

		file = fopen((const char *) dump, "w+");
		if (file) {
			struct chunk chunks[2];

			fflush(ctx->output_stream);
			get_chunks(chunks, &ctx->circular_buffer,
				   false, ctx->circular_buffer.size);

			if (!write_version(file, ctx) ||
			    !write_header(file, ctx) ||
			    !write_topology(file, ctx) ||
			    fwrite(chunks[0].data, chunks[0].len, 1, file) != 1 ||
			    (chunks[1].len > 0 &&
			     fwrite(chunks[1].data, chunks[1].len, 1, file) != 1) ||
			    !write_correlation_timestamps(ctx, file)) {
				fprintf(stderr, "Unable to write circular buffer data in file '%s'\n",
					dump);
			}
			fclose(file);
		} else
			fprintf(stderr, "Unable to write dump file '%s'\n", dump);

		free(dump);
		break;
	}
	case RECORDER_COMMAND_QUIT:
		quit = true;
		break;
	default:
		fprintf(stderr, "Unknown command 0x%x\n", header.command);
		break;
	}
}

static void
print_metric_sets(const struct intel_xe_perf *perf)
{
	struct intel_xe_perf_metric_set *metric_set;
	uint32_t longest_name = 0;

	igt_list_for_each_entry(metric_set, &perf->metric_sets, link) {
		longest_name = MAX(longest_name, strlen(metric_set->symbol_name));
	}

	igt_list_for_each_entry(metric_set, &perf->metric_sets, link) {
		fprintf(stdout, "%s:%*s%s\n",
			metric_set->symbol_name,
			(int) (longest_name - strlen(metric_set->symbol_name) + 1), " ",
			metric_set->name);
	}
}

static void
print_metric_set_counters(const struct intel_xe_perf_metric_set *metric_set)
{
	uint32_t longest_name = 0;

	for (uint32_t i = 0; i < metric_set->n_counters; i++) {
		longest_name = MAX(longest_name, strlen(metric_set->counters[i].name));
	}

	fprintf(stdout, "%s (%s):\n", metric_set->symbol_name, metric_set->name);
	for (uint32_t i = 0; i < metric_set->n_counters; i++) {
		struct intel_xe_perf_logical_counter *counter = &metric_set->counters[i];

		fprintf(stdout, "  %s:%*s%s\n",
			counter->name,
			(int)(longest_name - strlen(counter->name) + 1), " ",
			counter->desc);
	}
}

static void
print_metric_sets_counters(struct intel_xe_perf *perf)
{
	struct intel_xe_perf_metric_set *metric_set;

	igt_list_for_each_entry(metric_set, &perf->metric_sets, link)
		print_metric_set_counters(metric_set);
}

static void
usage(const char *name)
{
	fprintf(stdout,
		"Usage: %s [options]\n"
		"Recording tool for xe-oa\n"
		"\n"
		"     --help,               -h          Print this screen\n"
		"     --device,             -d <value>  Device to use\n"
		"                                       (value=list to list devices\n"
		"                                        value=1 to use /dev/dri/card1)\n"
		"     --correlation-period, -c <value>  Time period of timestamp correlation in seconds\n"
		"                                       (default = 1.0)\n"
		"     --perf-period,        -p <value>  Time period of xe-oa reports in seconds\n"
		"                                       (default = 0.001)\n"
		"     --metric,             -m <value>  xe-oa metric to sample with (use value=list to list all metrics)\n"
		"     --counters,           -C          List counters for a given metric and exit\n"
		"     --size,               -s <value>  Size of circular buffer to use in kilobytes\n"
		"                                       If specified, a maximum amount of <value> data will\n"
		"                                       be recorded.\n"
		"     --command-fifo,       -f <path>   Path to a command fifo, implies circular buffer\n"
		"                                       (To use with xe-perf-control)\n"
		"     --output,             -o <path>   Output file (default = xe_perf.record)\n"
		"     --cpu-clock,          -k <path>   Cpu clock to use for correlations\n"
		"                                       Values: boot, mono, mono_raw (default = mono)\n"
		"     --oa-unit-id          -u <value>  OA unit id for the capture.\n",
		name);
}

static void
teardown_recording_context(struct recording_context *ctx)
{
	if (ctx->ibb)
		intel_bb_destroy(ctx->ibb);
	if (ctx->exec_queue)
		xe_exec_queue_destroy(ctx->drm_fd, ctx->exec_queue);
	if (ctx->vm)
		xe_vm_destroy(ctx->drm_fd, ctx->vm);

	if (ctx->topology)
		free(ctx->topology);

	if (ctx->perf)
		intel_xe_perf_free(ctx->perf);

	if (ctx->command_fifo)
		unlink(ctx->command_fifo);
	if (ctx->command_fifo_fd != -1)
		close(ctx->command_fifo_fd);

	if (ctx->output_stream)
		fclose(ctx->output_stream);

	free(ctx->circular_buffer.data);

	if (ctx->perf_fd != -1)
		close(ctx->perf_fd);
	if (ctx->drm_fd != -1)
		close(ctx->drm_fd);
}

static int assign_hwe(int fd, struct recording_context *ctx)
{
	struct drm_xe_query_oa_units *qoa = xe_oa_units(fd);
	struct drm_xe_oa_unit *oau;
	int oam_sag_gt_id = 1;
	uint8_t *poau;

	if (ctx->oa_unit->num_engines) {
		/* Just use the first hwe attached to the oa_unit */
		ctx->hwe = &ctx->oa_unit->eci[0];
		return 0;
	}

	assert(ctx->oa_unit->oa_unit_type == DRM_XE_OA_UNIT_TYPE_OAM_SAG);

	/* Use first hwe attached to a different oa_unit on the same gt */
	poau = (uint8_t *)&qoa->oa_units[0];
	for (int i = 0; i < qoa->num_oa_units; i++) {
		oau = (struct drm_xe_oa_unit *)poau;

		if (!oau->num_engines)
			goto next;

		if (oau->eci[0].gt_id == oam_sag_gt_id) {
			ctx->hwe = &oau->eci[0];
			return 0;
		}
next:
		poau += sizeof(*oau) + oau->num_engines * sizeof(oau->eci[0]);
	}

	return -1;
}

static int assign_oa_unit(int fd, struct recording_context *ctx)
{
	struct drm_xe_query_oa_units *qoa = xe_oa_units(fd);
	struct drm_xe_oa_unit *oau;
	uint8_t *poau;

	poau = (uint8_t *)&qoa->oa_units[0];
	for (int i = 0; i < qoa->num_oa_units; i++) {
		oau = (struct drm_xe_oa_unit *)poau;

		if (oau->oa_unit_id == ctx->oa_unit_id) {
			ctx->oa_unit = oau;
			return assign_hwe(fd, ctx);
		}

		poau += sizeof(*oau) + oau->num_engines * sizeof(oau->eci[0]);
	}

	return -1;
}

static void init_mmio_trigger_ctx(struct recording_context *ctx)
{
	ctx->vm = xe_vm_create(ctx->drm_fd, 0, 0);
	ctx->exec_queue = xe_exec_queue_create(ctx->drm_fd, ctx->vm, ctx->hwe, 0);
	ctx->ibb = intel_bb_create_with_context(ctx->drm_fd, ctx->exec_queue, ctx->vm,
						NULL, BATCH_SZ);
}

int
main(int argc, char *argv[])
{
	const struct option long_options[] = {
		{"help",		no_argument, 0, 'h'},
		{"device",		required_argument, 0, 'd'},
		{"correlation-period",	required_argument, 0, 'c'},
		{"perf-period",		required_argument, 0, 'p'},
		{"metric",		required_argument, 0, 'm'},
		{"counters",		no_argument, 0, 'C'},
		{"output",		required_argument, 0, 'o'},
		{"size",		required_argument, 0, 's'},
		{"command-fifo",	required_argument, 0, 'f'},
		{"cpu-clock",		required_argument, 0, 'k'},
		{"oa-unit-id",		required_argument, 0, 'u'},
		{0, 0, 0, 0}
	};
	const struct {
		clock_t id;
		const char *name;
	} clock_names[] = {
		{ CLOCK_BOOTTIME,	"boot" },
		{ CLOCK_MONOTONIC,	"mono" },
		{ CLOCK_MONOTONIC_RAW,	"mono_raw" },
	};
	double corr_period = 1.0, perf_period = 0.001;
	const char *metric_name = NULL, *output_file = "xe_perf.record";
	struct intel_xe_perf_metric_set *metric_set;
	struct intel_xe_perf_record_timestamp_correlation initial_correlation;
	struct timespec now;
	uint64_t corr_period_ns, poll_time_ns;
	uint32_t circular_size = 0;
	int opt, dev_node_id = -1;
	bool list_counters = false;
	FILE *output = NULL;
	struct recording_context ctx = {
		.drm_fd = -1,
		.perf_fd = -1,

		.command_fifo = XE_PERF_RECORD_FIFO_PATH,
		.command_fifo_fd = -1,

		.oa_unit_id = 0,
	};

	while ((opt = getopt_long(argc, argv, "hc:d:p:m:Co:s:f:k:P:u:", long_options, NULL)) != -1) {
		switch (opt) {
		case 'h':
			usage(argv[0]);
			return EXIT_SUCCESS;
		case 'c':
			corr_period = atof(optarg);
			break;
		case 'd':
			if (!strcmp(optarg, "list"))
				dev_node_id = -2;
			else
				dev_node_id = atoi(optarg);
			break;
		case 'p':
			perf_period = atof(optarg);
			break;
		case 'm':
			metric_name = optarg;
			break;
		case 'C':
			list_counters = true;
			break;
		case 'o':
			output_file = optarg;
			break;
		case 's':
			circular_size = MAX(8, atoi(optarg)) * 1024;
			break;
		case 'f':
			ctx.command_fifo = optarg;
			circular_size = 8 * 1024 * 1024;
			break;
		case 'k': {
			bool found = false;
			for (uint32_t i = 0; i < ARRAY_SIZE(clock_names); i++) {
				if (!strcmp(clock_names[i].name, optarg)) {
					correlation_clock_id = clock_names[i].id;
					found = true;
					break;
				}
			}
			if (!found) {
				fprintf(stderr, "Unknown clock name '%s'\n", optarg);
				return EXIT_FAILURE;
			}
			break;
		}
		case 'u':
			ctx.oa_unit_id = atoi(optarg);
			break;
		default:
			fprintf(stderr, "Internal error: "
				"unexpected getopt value: %d\n", opt);
			usage(argv[0]);
			return EXIT_FAILURE;
		}
	}

	if (dev_node_id == -2) {
		print_intel_devices();
		return EXIT_SUCCESS;
	}

	ctx.drm_fd = open_render_node(&ctx.devid, dev_node_id);
	if (ctx.drm_fd < 0) {
		fprintf(stderr, "Unable to open device.\n");
		return EXIT_FAILURE;
	}

	xe_device_get(ctx.drm_fd);

	ctx.devinfo = intel_get_device_info(ctx.devid);
	if (!ctx.devinfo) {
		fprintf(stderr, "No device info found.\n");
		goto fail;
	}

	if (assign_oa_unit(ctx.drm_fd, &ctx) < 0) {
		fprintf(stderr, "assign_oa_unit failed\n");
		goto fail;
	}

	fprintf(stdout, "Device name=%s gen=%i id=0x%x oa_unit=%i gt=%i\n",
		ctx.devinfo->codename, ctx.devinfo->graphics_ver, ctx.devid,
		ctx.oa_unit->oa_unit_id, ctx.hwe->gt_id);

	ctx.topology = get_topology(&ctx);
	if (!ctx.topology) {
		fprintf(stderr, "Unable to retrieve GPU topology\n");
		goto fail;
	}

	ctx.perf = intel_xe_perf_for_fd(ctx.drm_fd, ctx.hwe->gt_id);
	if (!ctx.perf) {
		fprintf(stderr, "No perf data found.\n");
		goto fail;
	}

	intel_xe_perf_load_perf_configs(ctx.perf, ctx.drm_fd);

	if (metric_name) {
		if (!strcmp(metric_name, "list")) {
			print_metric_sets(ctx.perf);
			return EXIT_SUCCESS;
		}

		igt_list_for_each_entry(metric_set, &ctx.perf->metric_sets, link) {
			if (!strcasecmp(metric_set->symbol_name, metric_name)) {
				ctx.metric_set = metric_set;
				break;
			}
		}
	}

	if (list_counters) {
		if (!ctx.metric_set)
			print_metric_sets_counters(ctx.perf);
		else
			print_metric_set_counters(ctx.metric_set);
		teardown_recording_context(&ctx);
		return EXIT_SUCCESS;
	}

	if (!ctx.metric_set) {
		if (!metric_name)
			fprintf(stderr, "No metric set specified.\n");
		else
			fprintf(stderr, "Unknown metric set '%s'.\n", metric_name);
		print_metric_sets(ctx.perf);
		goto fail;
	}

	intel_xe_perf_load_perf_configs(ctx.perf, ctx.drm_fd);

	ctx.oa_timestamp_frequency = get_device_oa_timestamp_frequency(ctx.drm_fd);
	ctx.cs_timestamp_frequency = get_device_cs_timestamp_frequency(ctx.drm_fd);

	signal(SIGINT, sigint_handler);

	if (ctx.command_fifo) {
		if (mkfifo(ctx.command_fifo,
			   S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH) != 0) {
			fprintf(stderr, "Unable to create command fifo '%s': %s\n",
				ctx.command_fifo, strerror(errno));
			goto fail;
		}

		ctx.command_fifo_fd = open(ctx.command_fifo, O_RDWR);
		if (ctx.command_fifo_fd < 0) {
			fprintf(stderr, "Unable to open command fifo '%s': %s\n",
				ctx.command_fifo, strerror(errno));
			goto fail;
		}
	}

	if (circular_size) {
		ctx.circular_buffer.allocated_size = circular_size;
		ctx.circular_buffer.data = malloc(circular_size);
		if (!ctx.circular_buffer.data) {
			fprintf(stderr, "Unable to allocate circular buffer\n");
			goto fail;
		}

		ctx.output_stream = fopencookie(&ctx.circular_buffer, "w+",
						circular_buffer_functions);
		if (!ctx.output_stream) {
			fprintf(stderr, "Unable to create circular buffer\n");
			goto fail;
		}

		if (!get_correlation_timestamps(&ctx, &initial_correlation)) {
			fprintf(stderr, "Unable to correlation timestamps\n");
			goto fail;
		}

		write_correlation_timestamps(&ctx, ctx.output_stream);
		fprintf(stdout,
			"Recoding in internal circular buffer.\n"
			"Use xe-perf-control to snapshot into file.\n");
	} else {
		output = fopen(output_file, "w+");
		if (!output) {
			fprintf(stderr, "Unable to open output file '%s'\n",
				output_file);
			goto fail;
		}

		if (!write_version(output, &ctx) ||
		    !write_header(output, &ctx) ||
		    !write_topology(output, &ctx) ||
		    !write_correlation_timestamps(&ctx, output)) {
			fprintf(stderr, "Unable to write header in file '%s'\n",
				output_file);
			goto fail;
		}

		ctx.output_stream = output;
		fprintf(stdout, "Writing recoding to %s\n", output_file);
	}

	if (ctx.metric_set->perf_oa_metrics_set == 0) {
		fprintf(stderr,
			"Unable to load performance configuration, consider running:\n"
			"   sysctl dev.xe.observation_paranoid=0\n");
		goto fail;
	}

	fprintf(stdout, "Using correlation clock: %s\n",
		get_correlation_clock_name(correlation_clock_id));

	ctx.oa_exponent = oa_exponent_for_period(ctx.oa_timestamp_frequency, perf_period);
	fprintf(stdout, "Opening perf stream with metric_id=%"PRIu64" oa_exponent=%u oa_format=%u\n",
		ctx.metric_set->perf_oa_metrics_set, ctx.oa_exponent,
		ctx.metric_set->perf_oa_format);

	ctx.perf_fd = perf_open(&ctx);
	if (ctx.perf_fd < 0) {
		fprintf(stderr, "Unable to open xe oa stream: %s\n",
			strerror(errno));
		goto fail;
	}

	init_mmio_trigger_ctx(&ctx);
	emit_oa_trigger(&ctx, 0xc0ffee01);

	corr_period_ns = corr_period * 1000000000ul;
	poll_time_ns = corr_period_ns;

	while (!quit) {
		struct pollfd pollfd[2] = {
			{         ctx.perf_fd, POLLIN, 0 },
			{ ctx.command_fifo_fd, POLLIN, 0 },
		};
		uint64_t elapsed_ns;
		int ret;

		igt_gettime(&now);
		ret = poll(pollfd, ctx.command_fifo_fd != -1 ? 2 : 1, poll_time_ns / 1000000);
		if (ret < 0 && errno != EINTR) {
			fprintf(stderr, "Failed to poll xe-oa stream: %s\n",
				strerror(errno));
			break;
		}

		if (ret > 0) {
			if (pollfd[0].revents & POLLIN) {
				if (!write_perf_data(ctx.output_stream, &ctx)) {
					fprintf(stderr, "Failed to write xe-oa data: %s\n",
						strerror(errno));
					break;
				}
			}

			if (pollfd[1].revents & POLLIN) {
				read_command_file(&ctx);
			}
		}

		elapsed_ns = igt_nsec_elapsed(&now);
		if (elapsed_ns > poll_time_ns) {
			poll_time_ns = corr_period_ns;
			if (!write_correlation_timestamps(&ctx, ctx.output_stream)) {
				fprintf(stderr,
					"Failed to write xe timestamp correlation data: %s\n",
					strerror(errno));
				break;
			}
		} else {
			poll_time_ns -= elapsed_ns;
		}
	}

	/* Emit second OA trigger for the case where circular buffer is not used */
	emit_oa_trigger(&ctx, 0xc0ffee02);
	fprintf(stdout, "Exiting...\n");

	if (!write_perf_data(ctx.output_stream, &ctx)) {
		fprintf(stderr, "Failed to write xe-oa data: %s\n",
			strerror(errno));
	}

	if (!write_correlation_timestamps(&ctx, ctx.output_stream)) {
		fprintf(stderr,
			"Failed to write final xe timestamp correlation data: %s\n",
			strerror(errno));
	}

	teardown_recording_context(&ctx);

	return EXIT_SUCCESS;

 fail:
	teardown_recording_context(&ctx);

	return EXIT_FAILURE;
}
