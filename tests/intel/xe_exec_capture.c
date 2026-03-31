// SPDX-License-Identifier: MIT
/*
 * Copyright © 2024 Intel Corporation
 */

/**
 * TEST: Basic tests for GuC based register capture
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: CMD submission
 * Functionality: Debug
 * Test category: functionality test
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <zstd.h>

#include "igt.h"
#include "igt_device.h"
#include "igt_os.h"
#include "igt_sriov_device.h"
#include "igt_sysfs.h"
#include "lib/igt_syncobj.h"
#include "lib/intel_reg.h"
#include "linux_scaffold.h"
#include "xe_drm.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_legacy.h"
#include "xe/xe_query.h"
#include "xe/xe_spin.h"

#define MAX_N_EXECQUEUES		16
#define CAPTURE_JOB_TIMEOUT		2000
#define JOB_TIMOUT_ENTRY		"job_timeout_ms"

#define BASE_ADDRESS			0x1a0000
#define ADDRESS_SHIFT			39
#define CID_ADDRESS_MASK		0x7F
/* Batch buffer element count, in number of dwords(u32) */
#define BATCH_DW_COUNT			16

#define MAX_TEMP_LEN			80
#define MAX_SYSFS_PATH_LEN		128
#define MAX_LINES			4096
/* Max line buffer size (includes last '\0') */
#define MAX_LINE_LEN			1024
#define MAIN_BUF_SIZE			(MAX_LINES * MAX_LINE_LEN * sizeof(char))
/*
 * Devcoredump might have long line this test don't care.
 * This buffer size used when load dump content
 */
#define LINE_BUF_SIZE			(64 * 1024)

#define DUMP_PATH			"/sys/class/drm/card%d/device/devcoredump/data"
#define START_TAG			"**** Job ****"

/* Optional Space */
#define SPC_O				"[ \t\\.]*"
/* Required Space */
#define SPC				"[ \t\\.]+"
/* Optional Non-Space */
#define NSPC_O				"([^ \t\\.]*)"
/* Required Non-Space */
#define NSPC				"([^ \t\\.]+)"
#define BEG				"^" SPC_O
#define REQ_FIELD			NSPC SPC
#define REQ_FIELD_LAST			NSPC SPC_O
#define OPT_FIELD			NSPC_O SPC_O
#define END				SPC_O "$"

#define REGEX_NON_SPACE_GROUPS	BEG REQ_FIELD REQ_FIELD_LAST OPT_FIELD OPT_FIELD OPT_FIELD END
#define REGEX_NON_SPACE_GROUPS_COUNT	6

#define INDEX_KEY			1
#define INDEX_VALUE			2
#define INDEX_ENGINE_PHYSICAL		2
#define INDEX_ENGINE_NAME		1
#define INDEX_ENGINE_INSTANCE		4
#define INDEX_VM_LENGTH			2
#define INDEX_VM_SIZE			3

/*
 * Binary sections of the dump are encoded with the kernel's
 * include/linux/ascii85.h: a zero dword becomes a single 'z', every other
 * dword becomes 5 characters in the '!'..'u' range.
 */
#define ASCII85_ZERO			'z'
#define ASCII85_MIN			'!'
#define ASCII85_MAX			'u'
#define ASCII85_GROUP_LEN		5

#define LENGTH_SUFFIX			"].length:"
#define DATA_SUFFIX			"].data:"
#define ERROR_SUFFIX			"].error:"
#define PROPERTIES_SUFFIX		"].properties:"
#define MAX_BLOB_NAME_LEN		64

#define CAPTURE_SPIN_ADDRESS		0x1a0000ull
#define CAPTURE_PAYLOAD_ADDRESS		0x100000000ull

/* Largest dump size the kernel could render before it learned to chunk (VLK-71080) */
#define DEVCOREDUMP_LEGACY_MAX_SIZE	((uint64_t)INT_MAX)

/*
 * The test payloads contain no zero dwords, so their ASCII85 size is exactly
 * 5/4 of their binary size. The decoder still supports zero dwords.
 */
#define ASCII85_ENCODED_SIZE(bytes)	((bytes) / 4 * 5)

/* Small, but already far past any line-oriented parser */
#define DECODE_PAYLOAD_SIZE		SZ_2M
#define DECODE_PAYLOAD_COUNT		2

#define LARGE_PAYLOAD_SIZE		SZ_64M
/* One extra buffer makes the encoded payload exceed the legacy cap */
#define LARGE_PAYLOAD_COUNT		\
	((int)(DEVCOREDUMP_LEGACY_MAX_SIZE / \
	       ASCII85_ENCODED_SIZE(LARGE_PAYLOAD_SIZE)) + 1)

static u64
xe_sysfs_get_job_timeout_ms(int fd, struct drm_xe_engine_class_instance *eci)
{
	int engine_fd = -1;
	u64 ret;

	engine_fd = xe_sysfs_engine_open(fd, eci->gt_id, eci->engine_class);
	ret = igt_sysfs_get_u64(engine_fd, JOB_TIMOUT_ENTRY);
	close(engine_fd);

	return ret;
}

static void xe_sysfs_set_job_timeout_ms(int fd, struct drm_xe_engine_class_instance *eci,
					u64 timeout)
{
	int engine_fd = -1;

	engine_fd = xe_sysfs_engine_open(fd, eci->gt_id, eci->engine_class);
	igt_sysfs_set_u64(engine_fd, JOB_TIMOUT_ENTRY, timeout);
	close(engine_fd);
}

static char *safe_strncpy(char *dst, const char *src, int n)
{
	char *s;

	igt_assert(n > 0);
	igt_assert(dst && src);

	s = strncpy(dst, src, n - 1);
	s[n - 1] = '\0';

	return s;
}

static const char *xe_engine_class_name(u32 engine_class)
{
	switch (engine_class) {
	case DRM_XE_ENGINE_CLASS_RENDER:
		return "rcs";
	case DRM_XE_ENGINE_CLASS_COPY:
		return "bcs";
	case DRM_XE_ENGINE_CLASS_VIDEO_DECODE:
		return "vcs";
	case DRM_XE_ENGINE_CLASS_VIDEO_ENHANCE:
		return "vecs";
	case DRM_XE_ENGINE_CLASS_COMPUTE:
		return "ccs";
	default:
		igt_warn("Engine class 0x%x unknown\n", engine_class);
		return "unknown";
	}
}

static char **alloc_lines_buffer(void)
{
	int i;
	char **lines = (char **)malloc(MAX_LINES * sizeof(char *));
	char *main_buf =  (char *)malloc(MAIN_BUF_SIZE);

	igt_assert_f(lines, "Out of memory.\n");
	igt_assert_f(main_buf, "Out of memory.\n");

	/* set string array pointers */
	for (i = 0; i < MAX_LINES; i++)
		lines[i] = main_buf + i * MAX_LINE_LEN;

	return lines;
}

static char *get_devcoredump_path(int card_id, char *buf)
{
	sprintf(buf, DUMP_PATH, card_id);
	return buf;
}

static int load_all(FILE *fd, char **lines, char *buf)
{
	int start_line = 0, i = 0;
	bool skip = true;

	memset(lines[0], 0, MAIN_BUF_SIZE);
	while (!feof(fd) && i < MAX_LINES) {
		/*
		 * Devcoredump might have long lines, load up to
		 * LINE_BUF_SIZE for a single line
		 */
		if (!fgets(buf, LINE_BUF_SIZE, fd))
			if (ferror(fd) != 0) {
				igt_warn("Failed to read devcoredump file, error: %d\n",
					 ferror(fd));
				break;
			}

		if (skip) {
			start_line++;
			/* Skip all lines before START_TAG */
			if (strncmp(START_TAG, buf, strlen(START_TAG)))
				continue;
			else
				skip = false;
		}

		/* Only save up to MAX_LINE_LEN to buffer */
		safe_strncpy(lines[i++], buf, MAX_LINE_LEN);
	}
	return start_line;
}

static int access_devcoredump(char *path, char **lines, char *line_buf)
{
	int start_line = -1;
	FILE *fd = fopen(path, "r");

	if (!fd)
		return false;

	igt_debug("Devcoredump found: %s\n", path);

	/* Clear memory before load file */
	if (lines)
		start_line = load_all(fd, lines, line_buf);

	fclose(fd);
	return start_line;
}

static bool rm_devcoredump(char *path)
{
	int fd = open(path, O_WRONLY);

	if (fd != -1) {
		igt_debug("Clearing devcoredump.\n");
		write(fd, "0", 1);
		close(fd);
		return true;
	}

	return false;
}

static bool devcoredump_exists(char *path)
{
	return access_devcoredump(path, NULL, NULL) != 0;
}

/**
 * struct payload_bo - A buffer object the test fills with known dword values
 * @handle: GEM handle
 * @addr: GPU virtual address the buffer is bound at, which is also the name the
 *	  dump gives it, i.e. "[<addr>].data:"
 * @size: buffer size in bytes
 * @payload_index: zero-based payload index, used to generate this buffer's
 *		   dword values
 * @seen: set once the dump has been found to carry this buffer intact
 */
struct payload_bo {
	uint32_t handle;
	uint64_t addr;
	uint64_t size;
	uint32_t payload_index;
	bool seen;
};

/* Keep each payload's dword values distinct and non-zero. */
#define PAYLOAD_INDEX_STRIDE		0x04000000u

/**
 * payload_dword - Return the expected dword at a payload offset
 * @payload_index: zero-based payload index
 * @dword_index: dword offset in the payload
 *
 * Returns: expected dword value
 */
static uint32_t payload_dword(uint32_t payload_index, uint64_t dword_index)
{
	return (payload_index + 1) * PAYLOAD_INDEX_STRIDE + (uint32_t)dword_index;
}

/**
 * struct ascii85_decoder - Position within one encoded blob
 * @pos: next character to consume
 * @bad: set once a group is found that no encoder could have produced
 * @truncated: set when the blob ends part way through a group
 */
struct ascii85_decoder {
	const char *pos;
	bool bad;
	bool truncated;
};

/**
 * ascii85_next_dword - Decode the next dword of a blob
 * @dec: decoder positioned at the start of a group
 * @out: filled with the decoded value
 *
 * Returns: false at the end of the blob, on the first malformed group or on a
 * group cut short, the last two reported through @dec->bad and @dec->truncated.
 */
static bool ascii85_next_dword(struct ascii85_decoder *dec, uint32_t *out)
{
	uint64_t acc = 0;
	int i;

	if (!*dec->pos || *dec->pos == '\n')
		return false;

	/* A zero dword is abbreviated to one character */
	if (*dec->pos == ASCII85_ZERO) {
		dec->pos++;
		*out = 0;
		return true;
	}

	/* Otherwise five base-85 digits, most significant first */
	for (i = 0; i < ASCII85_GROUP_LEN; i++) {
		if (!dec->pos[i] || dec->pos[i] == '\n') {
			dec->truncated = true;
			return false;
		}
		if (dec->pos[i] < ASCII85_MIN || dec->pos[i] > ASCII85_MAX) {
			dec->bad = true;
			return false;
		}
		acc = acc * 85 + (dec->pos[i] - ASCII85_MIN);
	}

	/* 85^5 exceeds 2^32, so not every group is encodable */
	if (acc > UINT32_MAX) {
		dec->bad = true;
		return false;
	}

	dec->pos += ASCII85_GROUP_LEN;
	*out = acc;

	return true;
}

/**
 * parse_blob_field - Split a "[<name>]<suffix> <value>" devcoredump line
 * @line: line to match, e.g. "[4e4800000].length: 0x4000000"
 * @suffix: field suffix, matched literally so that
 *	    "].replay_length:" does not look like "].length:"
 * @name: filled with the bracketed name on success
 * @name_size: size of @name
 * @value: set to the first character of the value on success
 *
 * Returns: true when @line carries @suffix, false otherwise.
 */
static bool
parse_blob_field(const char *line, const char *suffix, char *name, size_t name_size,
		 const char **value)
{
	const char *open = strchr(line, '[');
	const char *close;
	size_t len;

	if (!open)
		return false;

	open++;
	close = strchr(open, ']');
	if (!close || strncmp(close, suffix, strlen(suffix)))
		return false;

	len = close - open;
	if (len >= name_size)
		return false;

	memcpy(name, open, len);
	name[len] = '\0';

	*value = close + strlen(suffix);
	while (**value == ' ' || **value == '\t')
		(*value)++;

	return true;
}

/**
 * payload_by_name - Find the payload a blob name refers to
 * @name: bracketed blob name, e.g. "4e4800000" or "HWSP"
 * @payloads: buffers the test bound before hanging, or NULL
 * @n_payloads: number of entries in @payloads
 *
 * Returns: the matching payload, or NULL when @name is not one of them. The
 * dump also uses labels such as "HWSP" and "CTB" for driver-owned sections;
 * names that are not entirely hexadecimal never match, which keeps "CTB" from
 * resolving as the address 0xc.
 */
static struct payload_bo *
payload_by_name(const char *name, struct payload_bo *payloads, int n_payloads)
{
	uint64_t addr;
	char *end;
	int i;

	if (!payloads)
		return NULL;

	addr = strtoull(name, &end, 16);
	if (end == name || *end)
		return NULL;

	for (i = 0; i < n_payloads; i++)
		if (payloads[i].addr == addr)
			return &payloads[i];

	return NULL;
}

struct blob_length {
	uint64_t size;
	uint64_t compressed_size;
};

static uint64_t parse_blob_size(const char *name, const char **value)
{
	uint64_t size;
	char *end;

	*value += strspn(*value, " \t");
	igt_assert_f(isdigit((unsigned char)**value),
		     "[%s]: invalid blob size: %s\n", name, *value);

	errno = 0;
	size = strtoull(*value, &end, 0);
	igt_assert_f(!errno && end != *value,
		     "[%s]: invalid blob size: %s\n", name, *value);
	*value = end;

	return size;
}

static struct blob_length parse_blob_length(const char *name, const char *value)
{
	struct blob_length length = {0};

	length.size = parse_blob_size(name, &value);
	value += strspn(value, " \t\r\n");
	if (!*value)
		return length;

	if (!strncmp(value, "->", 2)) {
		value += 2;
		length.compressed_size = parse_blob_size(name, &value);
		igt_assert_f(length.compressed_size,
			     "[%s]: compressed blob has zero size\n", name);
		value += strspn(value, " \t");
		igt_assert_f(!strncmp(value, "Algo: ZSTD", strlen("Algo: ZSTD")),
			     "[%s]: unsupported compression metadata: %s\n", name, value);
		value += strlen("Algo: ZSTD");
	} else {
		igt_assert_f(!strncmp(value, "Algo: RAW", strlen("Algo: RAW")),
			     "[%s]: unsupported compression metadata: %s\n", name, value);
		value += strlen("Algo: RAW");
	}

	value += strspn(value, " \t\r\n");
	igt_assert_f(!*value, "[%s]: unexpected blob length metadata: %s\n", name, value);

	return length;
}

/**
 * check_blob - Validate one ".length"/".data" pair of a devcoredump
 * @name: bracketed blob name, a hex VMA address for VM state sections
 * @length: original and compressed byte counts from the ".length" line
 * @data: encoded payload of the ".data" line
 * @payloads: buffers the test bound before hanging, or NULL
 * @n_payloads: number of entries in @payloads
 *
 * Checks ASCII85 decoding, optional Zstd decompression, and advertised sizes.
 * Test-created payloads are also checked against their expected contents.
 */
static void
check_blob(const char *name, const struct blob_length *length, const char *data,
	   struct payload_bo *payloads, int n_payloads)
{
	struct ascii85_decoder dec = { .pos = data };
	struct payload_bo *expected_payload;
	uint32_t *compressed = NULL;
	uint64_t encoded_length = length->size;
	uint64_t expected_dwords = 0;
	uint64_t dwords = 0;
	uint32_t val;

	expected_payload = payload_by_name(name, payloads, n_payloads);
	if (expected_payload) {
		igt_assert_f(length->size == expected_payload->size,
			     "[%s]: .length reports 0x%" PRIx64
			     " bytes, bound VMA is 0x%" PRIx64 " bytes\n",
			     name, length->size, expected_payload->size);
		expected_dwords = expected_payload->size / sizeof(uint32_t);
	}

	if (length->compressed_size) {
		igt_assert_f(length->compressed_size <= SIZE_MAX - (sizeof(uint32_t) - 1),
			     "[%s]: compressed size is too large\n", name);
		encoded_length = ALIGN(length->compressed_size, sizeof(uint32_t));
		compressed = malloc(encoded_length);
		igt_assert_f(compressed, "[%s]: failed to allocate compressed buffer\n", name);
	}

	/*
	 * Decode every blob for structural validation. Only test-created payloads
	 * need the additional content comparison.
	 */
	while (ascii85_next_dword(&dec, &val)) {
		if (compressed) {
			igt_assert_f(dwords < encoded_length / sizeof(uint32_t),
				     "[%s]: .data exceeds the compressed size\n", name);
			compressed[dwords] = val;
		} else if (expected_payload) {
			igt_assert_f(dwords < expected_dwords,
				     "[%s]: .data contains more dwords than the bound payload\n",
				     name);
			igt_assert_f(val == payload_dword(expected_payload->payload_index,
							  dwords),
				     "[%s]: decoded dword %" PRIu64
				     " differs from the content written to the buffer\n",
				     name, dwords);
		}
		dwords++;
	}

	igt_assert_f(!dec.truncated,
		     "[%s]: .data ends mid-ASCII85 group after %" PRIu64 " of %" PRIu64
		     " dwords, the dump was cut short\n",
		     name, dwords, encoded_length / sizeof(uint32_t));

	igt_assert_f(!dec.bad,
		     "[%s]: .data is not valid ASCII85 at dword %" PRIu64
		     " of %" PRIu64 " announced by .length\n",
		     name, dwords, encoded_length / sizeof(uint32_t));

	igt_assert_f(dwords * sizeof(uint32_t) == encoded_length,
		     "[%s]: expected 0x%" PRIx64 " ASCII85-decoded bytes but got"
		     " 0x%" PRIx64 " bytes\n",
		     name, encoded_length, dwords * sizeof(uint32_t));

	if (compressed) {
		uint32_t *decoded;
		size_t decoded_size;
		uint64_t i;

		igt_assert_f(length->size <= SIZE_MAX,
			     "[%s]: uncompressed size is too large\n", name);
		decoded = malloc(length->size ? length->size : 1);
		igt_assert_f(decoded, "[%s]: failed to allocate decompression buffer\n", name);

		/* Exclude the dword padding added for ASCII85 encoding. */
		decoded_size = ZSTD_decompress(decoded, length->size,
					      compressed, length->compressed_size);
		igt_assert_f(!ZSTD_isError(decoded_size),
			     "[%s]: Zstd decompression failed: %s\n",
			     name, ZSTD_getErrorName(decoded_size));
		igt_assert_f(decoded_size == length->size,
			     "[%s]: .length reports 0x%" PRIx64
			     " bytes but Zstd decompresses to 0x%zx bytes\n",
			     name, length->size, decoded_size);

		if (expected_payload)
			for (i = 0; i < expected_dwords; i++)
				igt_assert_f(decoded[i] ==
					     payload_dword(expected_payload->payload_index, i),
					     "[%s]: decoded dword %" PRIu64
					     " differs from the content written to the buffer\n",
					     name, i);

		free(decoded);
		free(compressed);
	}

	if (!expected_payload)
		return;

	expected_payload->seen = true;
	igt_debug("Cross-validated 0x%" PRIx64 " bytes of VMA [%s]\n", length->size, name);
}

/**
 * validate_devcoredump - Check every ASCII85 blob of a devcoredump
 * @path: path of the devcoredump data file
 * @payloads: buffers the test bound before hanging, or NULL
 * @n_payloads: number of entries in @payloads
 *
 * Walks the whole dump, checks each blob's decoded size, and verifies that all
 * test-created payloads are present with the expected contents.
 */
static void
validate_devcoredump(const char *path, struct payload_bo *payloads, int n_payloads)
{
	char pending_name[MAX_BLOB_NAME_LEN] = {0};
	char name[MAX_BLOB_NAME_LEN];
	struct blob_length pending_length = {0};
	bool have_pending = false;
	bool read_error;
	size_t line_size = 0;
	char *line = NULL;
	const char *value;
	int n_blobs = 0, read_errno, i;
	FILE *f;

	f = fopen(path, "r");
	igt_assert_f(f, "Failed to open %s, errno=%d\n", path, errno);

	/* .data lines can be megabytes long, so let getline() grow the buffer. */
	while (getline(&line, &line_size, f) > 0) {
		if (parse_blob_field(line, LENGTH_SUFFIX, name, sizeof(name), &value)) {
			igt_assert_f(!have_pending,
				     "[%s].length has no matching .data, .error"
				     " or null_sparse .properties line\n",
				     pending_name);
			pending_length = parse_blob_length(name, value);
			memcpy(pending_name, name, strlen(name) + 1);
			have_pending = true;
			continue;
		}

		if (parse_blob_field(line, PROPERTIES_SUFFIX, name, sizeof(name), &value)) {
			/* NULL bindings have metadata but no captured data. */
			if (have_pending && !strcmp(name, pending_name) &&
			    strstr(value, "|null_sparse|")) {
				igt_assert_f(!payload_by_name(name, payloads, n_payloads),
					     "[%s]: expected payload is a NULL/sparse binding\n",
					     name);
				have_pending = false;
			}
			continue;
		}

		if (parse_blob_field(line, ERROR_SUFFIX, name, sizeof(name), &value)) {
			if (have_pending && !strcmp(name, pending_name))
				have_pending = false;

			igt_assert_f(!payload_by_name(name, payloads, n_payloads),
				     "[%s]: kernel could not snapshot the buffer: %s",
				     name, value);
			continue;
		}

		if (!parse_blob_field(line, DATA_SUFFIX, name, sizeof(name), &value))
			continue;

		igt_assert_f(have_pending && !strcmp(name, pending_name),
			     "[%s].data is not preceded by a matching .length line\n",
			     name);

		have_pending = false;
		n_blobs++;
		check_blob(name, &pending_length, value, payloads, n_payloads);
	}

	read_error = ferror(f);
	read_errno = read_error ? errno : 0;
	free(line);
	fclose(f);

	igt_assert_f(!read_error, "Failed to read %s, errno=%d\n", path, read_errno);
	igt_assert_f(!have_pending,
		     "[%s].length has no matching .data, .error"
		     " or null_sparse .properties line\n",
		     pending_name);

	igt_assert_f(n_blobs, "No ASCII85 blob found in %s\n", path);
	igt_debug("Validated %d ASCII85 blobs\n", n_blobs);

	for (i = 0; i < n_payloads; i++)
		igt_assert_f(payloads[i].seen,
			     "VMA 0x%" PRIx64 " missing from the devcoredump VM state\n",
			     payloads[i].addr);
}

static char
*get_coredump_item(regex_t *regex, char **lines, const char *tag, int tag_index, int target_index)
{
	int i;
	regmatch_t match[REGEX_NON_SPACE_GROUPS_COUNT];

	for (i = 0; i < MAX_LINES; i++) {
		char *line = lines[i];

		/* Skip lines without tag */
		if (!strstr(line, tag))
			continue;

		if ((regexec(regex, line, REGEX_NON_SPACE_GROUPS_COUNT, match, 0)) == 0) {
			char *key = NULL, *value = NULL;

			if (match[tag_index].rm_so >= 0) {
				key = &line[match[tag_index].rm_so];
				line[match[tag_index].rm_eo] = '\0';
			}
			if (match[target_index].rm_so >= 0) {
				value = &line[match[target_index].rm_so];
				line[match[target_index].rm_eo] = '\0';
			}
			if (key && value && strcmp(tag, key) == 0)
				return value;
			/* if key != tag,  keep searching and loop to next line */
		}
	}

	return NULL;
}

static uint64_t
compare_hex_value(const char *output)
{
	char result[64];
	uint64_t ret_val;
	char *src = (char *)output, *dst = result;

//example i/p : [1580001a0000].length: 0x10000
	while (*src) {
		if (*src == '[' || *src == ']') {
			src++;
			continue;
		}

		*dst = toupper((unsigned char)*src);
		dst++;
		src++;
	}
	*dst = '\0';
	ret_val = strtoull(result, NULL, 16);
	return ret_val;
}

static void
check_item_u64(regex_t *regex, char **lines, const char *tag, u64 addr_lo,
	       u64 addr_hi, int tag_index, int target_index)
{
	u64 result;
	char *output;

	igt_assert_f((output = get_coredump_item(regex, lines, tag, tag_index, target_index)),
		     "Target not found:%s\n", tag);

	result = compare_hex_value(output);
	igt_debug("Compare %s %s vs [0x%" PRIX64 "-0x%" PRIX64 "] result %" PRIX64 "\n", tag, output,
		  addr_lo, addr_hi, result);
	igt_assert_f((addr_lo <= result) && (result <= addr_hi),
		     "value %" PRIX64 " out of range[0x%" PRIX64 "-0x%" PRIX64 "]\n", result, addr_lo, addr_hi);
}

static void
check_item_str(regex_t *regex, char **lines, const char *tag, int tag_index, int target_index,
	       const char *target, bool up_to_target_len)
{
	char buf[MAX_TEMP_LEN] = {0};
	char *output;
	int code;

	igt_assert_f(output = get_coredump_item(regex, lines, tag, tag_index, target_index),
		     "Target not found:%s\n", tag);

	if (up_to_target_len) {
		igt_assert_f(strlen(target) < MAX_TEMP_LEN, "Target too long.\n");
		safe_strncpy(buf, output, MAX_TEMP_LEN);
		buf[strlen(target)] = 0;
		output = buf;
	}
	code = strncmp(output, target, strlen(target));
	igt_debug("From tag '%s' found %s vs %s\n", tag, output, target);
	igt_assert_f(code == 0, "Expected value:%s, received:%s\n", target, output);
}

/**
 * SUBTEST: reset
 * Description: Reset GuC, check devcoredump output values
 */
static void test_card(int fd)
{
	struct drm_xe_engine_class_instance *hwe;
	regex_t regex;
	int start_line;
	int engine_cid = rand();
	char **lines;
	char *single_line_buf =  (char *)malloc(LINE_BUF_SIZE);
	char temp[MAX_TEMP_LEN];
	char path[MAX_SYSFS_PATH_LEN];
	const bool is_vf_device = intel_is_vf_device(fd);

	igt_assert_f(single_line_buf, "Out of memory.\n");

	regcomp(&regex, REGEX_NON_SPACE_GROUPS, REG_EXTENDED | REG_NEWLINE);
	get_devcoredump_path(igt_device_get_card_index(fd), path);
	lines = alloc_lines_buffer();

	/* clear old devcoredump, if any */
	rm_devcoredump(path);

	xe_for_each_engine(fd, hwe) {
		/*
		 * To test devcoredump register data, the test batch address is
		 * used to compare with the dump, address bit 40 to 46 act as
		 * context id, which start with an random number, increased 1
		 * per engine. By this way, the address is unique for each
		 * engine, and start with an random number on each run.
		 */
		const u64 addr = BASE_ADDRESS | ((u64)(engine_cid++ % CID_ADDRESS_MASK) <<
						 ADDRESS_SHIFT);

		igt_debug("Running on engine class: %x instance: %x\n", hwe->engine_class,
			  hwe->engine_instance);

		xe_legacy_test_mode(fd, hwe, 1, 1, DRM_XE_VM_BIND_FLAG_DUMPABLE,
				    addr, true);

		/* Wait 1 sec for devcoredump complete */
		sleep(1);

		/* assert devcoredump created */
		igt_assert_f((start_line = access_devcoredump(path, lines, single_line_buf)) > 0,
			     "Devcoredump not exist, errno=%d.\n", errno);

		if (!is_vf_device) {
			sprintf(temp, "instance=%d", hwe->engine_instance);
			check_item_str(&regex, lines, "(physical),",
				       INDEX_ENGINE_PHYSICAL,
				       INDEX_ENGINE_INSTANCE, temp, false);
			check_item_str(&regex, lines, "(physical),",
				       INDEX_ENGINE_PHYSICAL, INDEX_ENGINE_NAME,
				       xe_engine_class_name(hwe->engine_class),
				       true);

			check_item_str(&regex, lines,
				       "Capture_source:", INDEX_KEY,
				       INDEX_VALUE, "GuC", false);

			check_item_u64(&regex, lines, "ACTHD:", addr,
				       addr + BATCH_DW_COUNT * sizeof(u32),
				       INDEX_KEY, INDEX_VALUE);
			check_item_u64(&regex, lines, "RING_BBADDR:", addr,
				       addr + BATCH_DW_COUNT * sizeof(u32),
				       INDEX_KEY, INDEX_VALUE);
		}
		check_item_u64(&regex, lines, "length:", addr,
			       addr + BATCH_DW_COUNT * sizeof(u32), INDEX_VALUE, INDEX_KEY);

		validate_devcoredump(path, NULL, 0);

		/* clear devcoredump */
		rm_devcoredump(path);
		sleep(1);
		/* Assert devcoredump removed */
		igt_assert_f(!access_devcoredump(path, NULL, NULL), "Devcoredump not removed\n");
	}
	/* Free lines buffer */
	free(lines);
	free(single_line_buf);
	regfree(&regex);
}

/**
 * hang_with_payloads - Hang @eci with dumpable buffers of known content
 * @fd: xe device file descriptor
 * @eci: engine to hang
 * @payloads: buffers to create, fill and bind; handles are returned to the
 *	      caller, which owns them
 * @n_payloads: number of entries in @payloads
 *
 * Submits a batch that spins forever, so the job timeout fires and the driver
 * writes a devcoredump. The payloads share the address space of that batch and
 * are bound DUMPABLE, which is what tells the driver to copy their contents
 * into the dump, giving us blobs whose expected bytes are known exactly.
 */
static void
hang_with_payloads(int fd, struct drm_xe_engine_class_instance *eci,
		   struct payload_bo *payloads, int n_payloads)
{
	struct drm_xe_sync sync = {
		.type = DRM_XE_SYNC_TYPE_SYNCOBJ,
		.flags = DRM_XE_SYNC_FLAG_SIGNAL,
	};
	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.num_syncs = 1,
		.syncs = to_user_pointer(&sync),
	};
	struct xe_spin_opts spin_opts = { .addr = CAPTURE_SPIN_ADDRESS };
	uint64_t spin_size = xe_bb_size(fd, sizeof(struct xe_spin));
	uint32_t vm = xe_vm_create(fd, 0, 0);
	uint32_t spin_bo, exec_queue, bind_syncobj, exec_syncobj;
	struct xe_spin *spin;
	int i;

	spin_bo = xe_bo_create(fd, vm, spin_size, vram_if_possible(fd, eci->gt_id),
			       DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
	spin = xe_bo_map(fd, spin_bo, spin_size);

	bind_syncobj = syncobj_create(fd, 0);
	sync.handle = bind_syncobj;

	__xe_vm_bind_assert(fd, vm, 0, spin_bo, 0, CAPTURE_SPIN_ADDRESS, spin_size,
			    DRM_XE_VM_BIND_OP_MAP, DRM_XE_VM_BIND_FLAG_DUMPABLE,
			    &sync, 1, 0, 0);
	igt_assert(syncobj_wait(fd, &bind_syncobj, 1, INT64_MAX, 0, NULL));

	for (i = 0; i < n_payloads; i++) {
		uint32_t *map;
		uint64_t j;

		/* Use system memory so the large capture does not require large VRAM. */
		payloads[i].handle = xe_bo_create(fd, vm, payloads[i].size,
						  system_memory(fd), 0);
		map = xe_bo_map(fd, payloads[i].handle, payloads[i].size);
		for (j = 0; j < payloads[i].size / sizeof(*map); j++)
			map[j] = payload_dword(payloads[i].payload_index, j);
		munmap(map, payloads[i].size);

		syncobj_reset(fd, &bind_syncobj, 1);
		__xe_vm_bind_assert(fd, vm, 0, payloads[i].handle, 0, payloads[i].addr,
				    payloads[i].size, DRM_XE_VM_BIND_OP_MAP,
				    DRM_XE_VM_BIND_FLAG_DUMPABLE, &sync, 1, 0, 0);
		igt_assert(syncobj_wait(fd, &bind_syncobj, 1, INT64_MAX, 0, NULL));
	}

	exec_queue = xe_exec_queue_create(fd, vm, eci, 0);
	exec_syncobj = syncobj_create(fd, 0);

	xe_spin_init(spin, &spin_opts);

	sync.handle = exec_syncobj;
	exec.exec_queue_id = exec_queue;
	exec.address = CAPTURE_SPIN_ADDRESS;
	xe_exec(fd, &exec);

	xe_spin_wait_started(spin);

	/* The job timeout resets the queue, which is what creates the dump */
	igt_assert(syncobj_wait(fd, &exec_syncobj, 1, INT64_MAX, 0, NULL));

	syncobj_destroy(fd, exec_syncobj);
	syncobj_destroy(fd, bind_syncobj);
	xe_exec_queue_destroy(fd, exec_queue);
	munmap(spin, spin_size);
	gem_close(fd, spin_bo);
	xe_vm_destroy(fd, vm);
}

/**
 * SUBTEST: decode-data
 * Description: Create payloads with known contents, hang an engine, and
 * 		verify the resulting coredump's blob lengths and payload
 * 		contents before removing it
 *
 * SUBTEST: decode-data-large
 * Description: Same as decode-data, but with enough data to exercise kernel's
 * 		coredump chunking and the legacy size limit
 */
static void
test_capture_payload(int fd, struct drm_xe_engine_class_instance *eci,
		     int n_payloads, uint64_t payload_size)
{
	char path[MAX_SYSFS_PATH_LEN];
	struct payload_bo *payloads;
	int i;

	/* Every buffer needs its own slice of the 32-bit value space */
	igt_assert(payload_size / sizeof(uint32_t) <= PAYLOAD_INDEX_STRIDE &&
		   n_payloads <= UINT32_MAX / PAYLOAD_INDEX_STRIDE);

	payloads = calloc(n_payloads, sizeof(*payloads));
	igt_assert(payloads);

	for (i = 0; i < n_payloads; i++) {
		payloads[i].addr = CAPTURE_PAYLOAD_ADDRESS + i * payload_size;
		payloads[i].size = payload_size;
		payloads[i].payload_index = i;
	}

	get_devcoredump_path(igt_device_get_card_index(fd), path);
	rm_devcoredump(path);

	/* Wait for the previous coredump to disappear before creating another. */
	igt_assert_f(igt_wait(!devcoredump_exists(path), 5000, 100),
		     "Devcoredump not removed, errno=%d.\n", errno);

	hang_with_payloads(fd, eci, payloads, n_payloads);

	/*
	 * Close our BO handles; the snapshot holds BO references until it copies
	 * their contents into kernel-owned buffers for the coredump.
	 */
	for (i = 0; i < n_payloads; i++)
		gem_close(fd, payloads[i].handle);

	igt_assert_f(igt_wait(devcoredump_exists(path), 5000, 100),
		     "Devcoredump not created, a previous dump may still be held\n");

	validate_devcoredump(path, payloads, n_payloads);

	free(payloads);

	rm_devcoredump(path);

	/* Wait for the dump to disappear so its large buffers can be released. */
	igt_assert_f(igt_wait(!devcoredump_exists(path), 5000, 100),
		     "Devcoredump not removed, errno=%d.\n", errno);
}

/**
 * require_capture_memory - Skip unless the machine can hold the whole capture
 * @n_payloads: number of buffers the subtest binds
 * @payload_size: size of each buffer
 *
 * Upper bound on the footprint: the payloads, the snapshot copy the kernel
 * takes of them, the chunk the kernel renders and the longest line getline()
 * has to buffer. Not all of them peak at the same instant. The rendering buffer
 * is sized for the unfixed kernel, which is the one this has to survive.
 */
static void require_capture_memory(int n_payloads, uint64_t payload_size)
{
	uint64_t payload = (uint64_t)n_payloads * payload_size;
	uint64_t encoded = ASCII85_ENCODED_SIZE(payload);
	uint64_t rendered = encoded < DEVCOREDUMP_LEGACY_MAX_SIZE ?
			    encoded : DEVCOREDUMP_LEGACY_MAX_SIZE;

	igt_require_memory(1, 2 * payload + rendered +
			   ASCII85_ENCODED_SIZE(payload_size), CHECK_RAM);
}

int igt_main()
{
	int xe;
	/*
	 * The payload tests exercise VM capture and need one engine to trigger it.
	 * The reset subtest separately iterates over every available engine.
	 */
	struct drm_xe_engine_class_instance *hwe, *hang_hwe = NULL;
	u64 timeouts[DRM_XE_ENGINE_CLASS_VM_BIND] = {0};

	igt_fixture() {
		xe = drm_open_driver(DRIVER_XE);
		xe_for_each_engine(xe, hwe) {
			/* Skip kernel only classes */
			if (hwe->engine_class >= DRM_XE_ENGINE_CLASS_VM_BIND)
				continue;
			if (!hang_hwe)
				hang_hwe = hwe;
			/* Skip classes already set */
			if (timeouts[hwe->engine_class])
				continue;
			/* Save original timeout value */
			timeouts[hwe->engine_class] = xe_sysfs_get_job_timeout_ms(xe, hwe);
			/* Reduce timeout value to speedup test */
			xe_sysfs_set_job_timeout_ms(xe, hwe, CAPTURE_JOB_TIMEOUT);

			igt_debug("Reduced %s class timeout from %" PRIu64 " to %d\n",
				  xe_engine_class_name(hwe->engine_class),
				  timeouts[hwe->engine_class], CAPTURE_JOB_TIMEOUT);
		}
	}

	igt_subtest("reset")
		test_card(xe);

	igt_subtest("decode-data") {
		igt_require(hang_hwe);
		test_capture_payload(xe, hang_hwe, DECODE_PAYLOAD_COUNT,
				     DECODE_PAYLOAD_SIZE);
	}

	igt_subtest("decode-data-large") {
		igt_require(hang_hwe);
		require_capture_memory(LARGE_PAYLOAD_COUNT, LARGE_PAYLOAD_SIZE);
		test_capture_payload(xe, hang_hwe, LARGE_PAYLOAD_COUNT,
				     LARGE_PAYLOAD_SIZE);
	}

	igt_fixture() {
		xe_for_each_engine(xe, hwe) {
			u64 store, timeout;

			/* Skip kernel only classes */
			if (hwe->engine_class >= DRM_XE_ENGINE_CLASS_VM_BIND)
				continue;

			timeout = timeouts[hwe->engine_class];
			/* Skip classes already set */
			if (!timeout)
				continue;

			/* Restore original timeout value */
			xe_sysfs_set_job_timeout_ms(xe, hwe, timeout);

			/* Assert successful restore */
			store = xe_sysfs_get_job_timeout_ms(xe, hwe);
			igt_abort_on_f(timeout != store, "job_timeout_ms not restored!\n");

			igt_debug("Restored %s class timeout to %" PRIu64 "\n",
				  xe_engine_class_name(hwe->engine_class),
				  timeouts[hwe->engine_class]);

			timeouts[hwe->engine_class] = 0;
		}

		drm_close_driver(xe);
	}
}
