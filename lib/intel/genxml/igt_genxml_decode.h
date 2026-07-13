/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2026 Intel Corporation
 *
 * Batch buffer annotated decode using genxml-generated decode headers.
 *
 * Usage:
 *   igt_genxml_decode_batch(fp, devid, batch_ptr, batch_dwords);
 *
 * This dispatches to the appropriate per-gen decode function based on
 * the device ID. Each gen's decode header provides command identification
 * and field-level annotation.
 */

#ifndef IGT_GENXML_DECODE_H
#define IGT_GENXML_DECODE_H

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "intel_chipset.h"

#include "gen90_decode.h"
#include "gen110_decode.h"
#include "gen120_decode.h"
#include "gen125_decode.h"
#include "xe2_decode.h"
#include "xe3_decode.h"
#include "xe3p_decode.h"

static inline unsigned
igt_genxml_decode_command(FILE *fp, uint32_t devid, uint32_t offset,
			  const uint32_t *dw, unsigned remaining)
{
	unsigned gen = intel_gen(devid);
	unsigned len = 0;

#define TRY_DECODE(fn) \
	do { \
		if (!len) \
			len = fn(fp, offset, dw, remaining); \
	} while (0)

	if (gen >= 35) {
		TRY_DECODE(gfx35_decode_command);
		TRY_DECODE(gfx30_decode_command);
		TRY_DECODE(gfx20_decode_command);
		TRY_DECODE(gfx125_decode_command);
		TRY_DECODE(gfx12_decode_command);
		TRY_DECODE(gfx11_decode_command);
		TRY_DECODE(gfx9_decode_command);
	} else if (gen >= 30) {
		TRY_DECODE(gfx30_decode_command);
		TRY_DECODE(gfx20_decode_command);
		TRY_DECODE(gfx125_decode_command);
		TRY_DECODE(gfx12_decode_command);
		TRY_DECODE(gfx11_decode_command);
		TRY_DECODE(gfx9_decode_command);
	} else if (gen >= 20) {
		TRY_DECODE(gfx20_decode_command);
		TRY_DECODE(gfx125_decode_command);
		TRY_DECODE(gfx12_decode_command);
		TRY_DECODE(gfx11_decode_command);
		TRY_DECODE(gfx9_decode_command);
	} else if (HAS_4TILE(devid) || gen > 12) {
		TRY_DECODE(gfx125_decode_command);
		TRY_DECODE(gfx12_decode_command);
		TRY_DECODE(gfx11_decode_command);
		TRY_DECODE(gfx9_decode_command);
	} else if (gen >= 12) {
		TRY_DECODE(gfx12_decode_command);
		TRY_DECODE(gfx11_decode_command);
		TRY_DECODE(gfx9_decode_command);
	} else if (gen >= 11) {
		TRY_DECODE(gfx11_decode_command);
		TRY_DECODE(gfx9_decode_command);
	} else {
		TRY_DECODE(gfx9_decode_command);
	}

#undef TRY_DECODE

	return len;
}

/*
 * igt_genxml_decode_batch - walk and annotate a batch buffer.
 *
 * @fp:            output file
 * @devid:         PCI device ID (used to select the right gen decoder)
 * @batch:         pointer to batch buffer dwords
 * @batch_dwords:  number of dwords in the batch
 */
static inline void
igt_genxml_decode_batch(FILE *fp, uint32_t devid,
			const uint32_t *batch, unsigned batch_dwords)
{
	unsigned offset = 0;

	while (offset < batch_dwords * 4) {
		const uint32_t *dw = &batch[offset / 4];
		uint32_t cmd = dw[0];
		unsigned len;

		if (cmd == 0x05000000) {
			fprintf(fp, "[0x%04x] 0x%08x  MI_BATCH_BUFFER_END\n", offset, cmd);
			break;
		}

		if (cmd == 0) {
			fprintf(fp, "[0x%04x] 0x%08x  MI_NOOP\n", offset, cmd);
			offset += 4;
			continue;
		}

		len = igt_genxml_decode_command(fp, devid, offset, dw,
						batch_dwords - offset / 4);
		if (!len) {
			fprintf(fp, "[0x%04x] 0x%08x  UNKNOWN\n", offset, cmd);
			len = 1;
		}

		offset += len * 4;
	}
}

typedef void (*igt_genxml_decode_line_fn)(const char *line, void *userdata);

/*
 * igt_genxml_decode_batch_lines - decode a batch and replay one line at a time.
 *
 * @devid:         PCI device ID (used to select the right gen decoder)
 * @batch:         pointer to batch buffer dwords
 * @batch_dwords:  number of dwords in the batch
 * @emit_line:     callback invoked once per decoded line, without trailing '\n'
 * @userdata:      opaque pointer passed through to @emit_line
 *
 * Returns false only if the temporary memstream could not be created.
 */
static inline bool
igt_genxml_decode_batch_lines(uint32_t devid,
			      const uint32_t *batch,
			      unsigned batch_dwords,
			      igt_genxml_decode_line_fn emit_line,
			      void *userdata)
{
	char *buf = NULL;
	char *line;
	char *next;
	size_t len = 0;
	FILE *fp;

	if (!emit_line)
		return false;

	fp = open_memstream(&buf, &len);
	if (!fp)
		return false;

	igt_genxml_decode_batch(fp, devid, batch, batch_dwords);
	fclose(fp);

	for (line = buf; line && *line; line = next) {
		next = strchr(line, '\n');
		if (next)
			*next++ = '\0';

		emit_line(line, userdata);
	}

	free(buf);
	return true;
}

#endif /* IGT_GENXML_DECODE_H */
