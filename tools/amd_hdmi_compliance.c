/*
 * Copyright 2019 Advanced Micro Devices, Inc.
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
 */

#include "igt.h"
#include "igt_sysfs.h"
#include <fcntl.h>
#include <signal.h>
#include <string.h>

/* Common test data */
typedef struct data {
	struct igt_fb pattern_fb_info;
	int fd;
	igt_display_t display;
	igt_plane_t *primary;
	igt_output_t *output;
	igt_crtc_t *crtc;
	bool use_virtual_connector;
	int timeout_seconds;
} data_t;

enum dc_pixel_encoding {
		PIXEL_ENCODING_UNKNOWN = 0,
		PIXEL_ENCODING_RGB,
		PIXEL_ENCODING_YCBCR444,
		PIXEL_ENCODING_YCBCR422,
		PIXEL_ENCODING_YCBCR420,
};

/* Video modes indexed by VIC */
static drmModeModeInfo test_modes[] = {
	[0] = { 25175,
		640, 656, 752, 800, 0,
		480, 489, 492, 525, 0,
		60, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_4_3,
		0x40, "640x480",	/* VIC 0 */
	},
	[1] = { 25175,
		640, 656, 752, 800, 0,
		480, 490, 492, 525, 0,
		60, DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_PIC_AR_4_3,
		0x40, "640x480",	/* VIC 1 */
	},
	[2] = { 27000,
		720, 736, 798, 858, 0,
		480, 489, 495, 525, 0,
		60, DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_PIC_AR_4_3,
		0x40, "720x480",	/* VIC 2 */
	},
	[3] = { 27000,
		720, 736, 798, 858, 0,
		480, 489, 495, 525, 0,
		60, DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_PIC_AR_16_9,
		0x40, "720x480",	/* VIC 3 */
	},
	[4] = { 74250,
		1280, 1390, 1430, 1650, 0,
		720, 725, 730, 750, 0,
		60, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_16_9,
		0x40, "1280x720",	/* VIC 4 */
	},
	[16] = { 148500,
		1920, 2008, 2052, 2200, 0,
		1080, 1084, 1089, 1125, 0,
		60, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_16_9,
		0x40, "1920x1080",	/* VIC 16 */
	},
	[17] = { 27000,
		720, 732, 796, 864, 0,
		576, 581, 586, 625, 0,
		60, DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_PIC_AR_4_3,
		0x40, "720x576",	/* VIC 17 */
	},
	[18] = { 27000,
		720, 732, 796, 864, 0,
		576, 581, 586, 625, 0,
		60, DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_PIC_AR_16_9,
		0x40, "720x576",	/* VIC 18 */
	},
	[19] = { 74250,
		1280, 1720, 1760, 1980, 0,
		720, 725, 730, 750, 0,
		50, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_16_9,
		0x40, "1280x720",	/* VIC 19 */
	},
	[31] = { 148500,
		1920, 2448, 2492, 2640, 0,
		1080, 1084, 1089, 1125, 0,
		50, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_16_9,
		0x40, "1920x1080",	/* VIC 31 */
	},
	[32] = { 74250,
		1920, 2558, 2602, 2750, 0,
		1080, 1084, 1089, 1125, 0,
		24, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_16_9,
		0x40, "1920x1080",	/* VIC 32 */
	},
	[33] = { 74250,
		1920, 2448, 2492, 2640, 0,
		1080, 1084, 1089, 1125, 0,
		25, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_16_9,
		0x40, "1920x1080",	/* VIC 33 */
	},
	[34] = { 74250,
		1920, 2008, 2052, 2200, 0,
		1080, 1084, 1089, 1125, 0,
		30, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_16_9,
		0x40, "1920x1080",	/* VIC 34 */
	},
	[41] = { 148500,
		1280, 1720, 1760, 1980, 0,
		720, 725, 730, 750, 0,
		100, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_16_9,
		0x40, "1280x720",	/* VIC 41 */
	},
	[42] = { 54000,
		720, 732, 796, 864, 0,
		576, 581, 586, 625, 0,
		100, DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_PIC_AR_4_3,
		0x40, "720x576",	/* VIC 42 */
	},
	[43] = { 54000,
		720, 732, 796, 864, 0,
		576, 581, 586, 625, 0,
		100, DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_PIC_AR_16_9,
		0x40, "720x576",	/* VIC 43 */
	},
	[47] = { 148500,
		1280, 1390, 1430, 1650, 0,
		720, 725, 730, 750, 0,
		120, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_16_9,
		0x40, "1280x720",	/* VIC 47 */
	},
	[48] = { 54000,
		720, 736, 798, 858, 0,
		480, 489, 495, 525, 0,
		120, DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_PIC_AR_4_3,
		0x40, "720x480",	/* VIC 48 */
	},
	[49] = { 54000,
		720, 736, 798, 858, 0,
		480, 489, 495, 525, 0,
		120, DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_PIC_AR_16_9,
		0x40, "720x480",	/* VIC 49 */
	},
	[52] = { 108000,
		720, 732, 796, 864, 0,
		576, 581, 586, 625, 0,
		200, DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_PIC_AR_4_3,
		0x40, "720x576",	/* VIC 52 */
	},
	[53] = { 108000,
		720, 732, 796, 864, 0,
		576, 581, 586, 625, 0,
		200, DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_PIC_AR_16_9,
		0x40, "720x576",	/* VIC 53 */
	},
	[56] = { 108000,
		720, 736, 798, 858, 0,
		480, 489, 495, 525, 0,
		240, DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_PIC_AR_4_3,
		0x40, "720x480",	/* VIC 56 */
	},
	[57] = { 108000,
		720, 736, 798, 858, 0,
		480, 489, 495, 525, 0,
		240, DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_PIC_AR_16_9,
		0x40, "720x480",	/* VIC 57 */
	},
	[60] = { 59400,
		1280, 3040, 3080, 3300, 0,
		720, 725, 730, 750, 0,
		24, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_16_9,
		0x40, "1280x720",	/* VIC 60 */
	},
	[61] = { 74250,
		1280, 3700, 3740, 3960, 0,
		720, 725, 730, 750, 0,
		25, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_16_9,
		0x40, "1280x720",	/* VIC 61 */
	},
	[62] = { 74250,
		1280, 3040, 3080, 3300, 0,
		720, 725, 730, 750, 0,
		30, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_16_9,
		0x40, "1280x720",	/* VIC 62 */
	},
	[63] = { 297000,
		1920, 2008, 2052, 2200, 0,
		1080, 1084, 1089, 1125, 0,
		120, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_16_9,
		0x40, "1920x1080",	/* VIC 63 */
	},
	[64] = { 297000,
		1920, 2448, 2492, 2640, 0,
		1080, 1084, 1089, 1125, 0,
		100, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_16_9,
		0x40, "1920x1080",	/* VIC 64 */
	},
	[65] = { 59400,
		1280, 3040, 3080, 3300, 0,
		720, 725, 730, 750, 0,
		24, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		0x40, "1280x720",	/* VIC 65 */
	},
	[66] = { 74250,
		1280, 3700, 3740, 3960, 0,
		720, 725, 730, 750, 0,
		25, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		0x40, "1280x720",	/* VIC 66 */
	},
	[67] = { 74250,
		1280, 3040, 3080, 3300, 0,
		720, 725, 730, 750, 0,
		30, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		0x40, "1280x720",	/* VIC 67 */
	},
	[68] = { 74250,
		1280, 1720, 1760, 1980, 0,
		720, 725, 730, 750, 0,
		50, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		0x40, "1280x720",	/* VIC 68 */
	},
	[69] = { 74250,
		1280, 1390, 1430, 1650, 0,
		720, 725, 730, 750, 0,
		60, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		0x40, "1280x720",	/* VIC 69 */
	},
	[70] = { 148500,
		1280, 1720, 1760, 1980, 0,
		720, 725, 730, 750, 0,
		100, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		0x40, "1280x720",	/* VIC 70 */
	},
	[71] = { 148500,
		1280, 1390, 1430, 1650, 0,
		720, 725, 730, 750, 0,
		120, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		0x40, "1280x720",	/* VIC 71 */
	},
	[72] = { 74250,
		1920, 2558, 2602, 2750, 0,
		1080, 1084, 1089, 1125, 0,
		24, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		0x40, "1920x1080",	/* VIC 72 */
	},
	[73] = { 74250,
		1920, 2448, 2492, 2640, 0,
		1080, 1084, 1089, 1125, 0,
		25, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		0x40, "1920x1080",	/* VIC 74 */
	},
	[74] = { 74250,
		1920, 2008, 2052, 2200, 0,
		1080, 1084, 1089, 1125, 0,
		30, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		0x40, "1920x1080",	/* VIC 74 */
	},
	[75] = { 148500,
		1920, 2448, 2492, 2640, 0,
		1080, 1084, 1089, 1125, 0,
		50, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		0x40, "1920x1080",	/* VIC 75 */
	},
	[76] = { 148500,
		1920, 2008, 2052, 2200, 0,
		1080, 1084, 1089, 1125, 0,
		60, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		0x40, "1920x1080",	/* VIC 76 */
	},
	[77] = { 297000,
		1920, 2448, 2492, 2640, 0,
		1080, 1084, 1089, 1125, 0,
		50, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		0x40, "1920x1080",	/* VIC 77 */
	},
	[78] = { 297000,
		1920, 2008, 2052, 2200, 0,
		1080, 1084, 1089, 1125, 0,
		50, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		0x40, "1920x1080",	/* VIC 78 */
	},
	[79] = { 59400,
		1680, 3040, 3080, 3300, 0,
		720, 725, 730, 750, 0,
		24, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		0x40, "1680x720",	/* VIC 79 */
	},
	[80] = { 59400,
		1680, 2908, 2948, 3168, 0,
		720, 725, 730, 750, 0,
		25, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		0x40, "1680x720",	/* VIC 80 */
	},
	[81] = { 59400,
		1680, 2380, 2420, 2640, 0,
		720, 725, 730, 750, 0,
		30, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		0x40, "1680x720",	/* VIC 81 */
	},
	[82] = { 82500,
		1680, 1940, 1980, 2200, 0,
		720, 725, 730, 750, 0,
		50, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		0x40, "1680x720",	/* VIC 82 */
	},
	[83] = { 99000,
		1680, 1940, 1980, 2200, 0,
		720, 725, 730, 750, 0,
		60, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		0x40, "1680x720",	/* VIC 83 */
	},
	[84] = { 165000,
		1680, 1740, 1780, 2000, 0,
		720, 725, 730, 825, 0,
		100, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		0x40, "1680x720",	/* VIC 84 */
	},
	[85] = { 198000,
		1680, 1740, 1780, 2000, 0,
		720, 725, 730, 825, 0,
		120, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		0x40, "1680x720",	/* VIC 85 */
	},
	[86] = { 99000,
		2560, 3558, 3602, 3750, 0,
		1080, 1084, 1089, 1100, 0,
		24, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		0x40, "2560x1080",	/* VIC 86 */
	},
	[87] = { 90000,
		2560, 3008, 3052, 3200, 0,
		1080, 1084, 1089, 1125, 0,
		25, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		0x40, "2560x1080",	/* VIC 87 */
	},
	[88] = { 118800,
		2560, 3328, 3372, 3520, 0,
		1080, 1084, 1089, 1125, 0,
		30, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		0x40, "2560x1080",	/* VIC 88 */
	},
	[89] = { 185625,
		2560, 3108, 3152, 3300, 0,
		1080, 1084, 1089, 1125, 0,
		50, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		0x40, "2560x1080",	/* VIC 89 */
	},
	[90] = { 198000,
		2560, 2808, 2852, 3000, 0,
		1080, 1084, 1089, 1100, 0,
		60, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		0x40, "2560x1080",	/* VIC 90 */
	},
	[91] = { 371250,
		2560, 2778, 2822, 2970, 0,
		1080, 1084, 1089, 1250, 0,
		100, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		0x40, "2560x1080",	/* VIC 91 */
	},
	[92] = { 495000,
		2560, 3108, 3152, 3300, 0,
		1080, 1084, 1089, 1250, 0,
		120, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		0x40, "2560x1080",	/* VIC 92 */
	},
	[93] = { 297000,
		3840, 5116, 5204, 5500, 0,
		2160, 2168, 2178, 2250, 0,
		24, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_16_9,
		0x40, "3840x2160",	/* VIC 93 */
	},
	[94] = { 297000,
		3840, 4896, 4984, 5280, 0,
		2160, 2168, 2178, 2250, 0,
		25, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_16_9,
		0x40, "3840x2160",	/* VIC 94 */
	},
	[95] = { 297000,
		3840, 4016, 4104, 4400, 0,
		2160, 2168, 2178, 2250, 0,
		30, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_16_9,
		0x40, "3840x2160",	/* VIC 95 */
	},
	[96] = { 594000,
		 3840, 4896, 4984, 5280, 0,
		 2160, 2168, 2178, 2250, 0,
		 50, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_16_9,
		 0x40, "3840x2160",	/* VIC 96 */
	},
	[97] = { 594000,
		 3840, 4016, 4104, 4400, 0,
		 2160, 2168, 2178, 2250, 0,
		 60, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_16_9,
		 0x40, "3840x2160",	/* VIC 97 */
	},
	[98] = { 297000,
		 4096, 5116, 5204, 5500, 0,
		 2160, 2168, 2178, 2250, 0,
		 24, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_256_135,
		 0x40, "4096x2160",	/* VIC 98 */
	},
	[99] = { 297000,
		 4096, 5064, 5152, 5280, 0,
		 2160, 2168, 2178, 2250, 0,
		 25, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_256_135,
		 0x40, "4096x2160",	/* VIC 99 */
	},
	[100] = { 297000,
		 4096, 4184, 4272, 4400, 0,
		 2160, 2168, 2178, 2250, 0,
		 30, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_256_135,
		 0x40, "4096x2160",	/* VIC 100 */
	},
	[101] = { 594000,
		  4096, 5064, 5152, 5280, 0,
		  2160, 2168, 2178, 2250, 0,
		  50, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_256_135,
		  0x40, "4096x2160",	/* VIC 101 */
	},
	[102] = { 594000,
		  4096, 4184, 4272, 4400, 0,
		  2160, 2168, 2178, 2250, 0,
		  60, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_256_135,
		  0x40, "4096x2160",	/* VIC 102 */
	},
	[103] = { 297000,
		  3840, 5116, 5204, 5500, 0,
		  2160, 2168, 2178, 2250, 0,
		  24, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		  0x40, "3840x2160",	/* VIC 103 */
	},
	[104] = { 297000,
		  3840, 4896, 4984, 5280, 0,
		  2160, 2168, 2178, 2250, 0,
		  25, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		  0x40, "3840x2160",	/* VIC 104 */
	},
	[105] = { 297000,
		  3840, 4016, 4104, 4400, 0,
		  2160, 2168, 2178, 2250, 0,
		  30, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		  0x40, "3840x2160",	/* VIC 105 */
	},
	[106] = { 594000,
		  3840, 4896, 4984, 5280, 0,
		  2160, 2168, 2178, 2250, 0,
		  50, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		  0x40, "3840x2160",	/* VIC 106 */
	},
	[107] = { 594000,
		  3840, 4016, 4104, 4400, 0,
		  2160, 2168, 2178, 2250, 0,
		  60, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		  0x40, "3840x2160",	/* VIC 107 */
	},
	[108] = { 90000,
		  1280, 2240, 2280, 2500, 0,
		  720, 725, 730, 750, 0,
		  48, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_16_9,
		  0x40, "1280x720",	/* VIC 108 */
	},
	[109] = { 90000,
		  1280, 2240, 2280, 2500, 0,
		  720, 725, 730, 750, 0,
		  48, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		  0x40, "1280x720",	/* VIC 109 */
	},
	[110] = { 99000,
		  1680, 2490, 2530, 2750, 0,
		  720, 725, 730, 750, 0,
		  48, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		  0x40, "1680x720",	/* VIC 110 */
	},
	[111] = { 148500,
		  1920, 2558, 2602, 2750, 0,
		  1080, 1084, 1089, 1125, 0,
		  48, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_16_9,
		  0x40, "1920x1080",	/* VIC 111 */
	},
	[112] = { 148500,
		  1920, 2558, 2602, 2750, 0,
		  1080, 1084, 1089, 1125, 0,
		  48, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		  0x40, "1920x1080",	/* VIC 112 */
	},
	[113] = { 198000,
		  2560, 3558, 3602, 3750, 0,
		  1080, 1084, 1089, 1100, 0,
		  48, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		  0x40, "2560x1080",	/* VIC 113 */
	},
	 [114] = { 594000,
		  3840, 5116, 5204, 5500, 0,
		  2160, 2168, 2178, 2250, 0,
		  48, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_16_9,
		  0x40, "3840x2160",	/* VIC 114 */
	},
	[115] = { 594000,
		  4096, 5116, 5204, 5500, 0,
		  2160, 2168, 2178, 2250, 0,
		  48, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_256_135,
		  0x40, "4096x2160",	/* VIC 115 */
	},
	[116] = { 594000,
		  3840, 5116, 5204, 5500, 0,
		  2160, 2168, 2178, 2250, 0,
		  48, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		  0x40, "3840x2160",	/* VIC 116 */
	},
	[117] = { 1188000,
		  3840, 4896, 4984, 5280, 0,
		  2160, 2168, 2178, 2250, 0,
		  100, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_16_9,
		  0x40, "3840x2160",	/* VIC 117 */
	},
	[118] = { 1188000,
		  3840, 4016, 4104, 4400, 0,
		  2160, 2168, 2178, 2250, 0,
		  120, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_16_9,
		  0x40, "3840x2160",	/* VIC 118 */
	},
	[119] = { 1188000,
		  3840, 4896, 4984, 5280, 0,
		  2160, 2168, 2178, 2250, 0,
		  100, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		  0x40, "3840x2160",	/* VIC 119 */
	},
	[120] = { 1188000,
		  3840, 4016, 4104, 4400, 0,
		  2160, 2168, 2178, 2250, 0,
		  120, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		  0x40, "3840x2160",	/* VIC 120 */
	},
	[121] = { 396000,
		  5120, 7116, 7204, 7500, 0,
		  2160, 2168, 2178, 2200, 0,
		  24, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		  0x40, "5120x2160",	/* VIC 121 */
	},
	[122] = { 396000,
		  5120, 6816, 6904, 7200, 0,
		  2160, 2168, 2178, 2200, 0,
		  25, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		  0x40, "5120x2160",	/* VIC 122 */
	},
	[123] = { 396000,
		  5120, 5784, 5872, 6000, 0,
		  2160, 2168, 2178, 2200, 0,
		  30, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		  0x40, "5120x2160",	/* VIC 123 */
	},
	[124] = { 742500,
		  5120, 5866, 5954, 6250, 0,
		  2160, 2168, 2178, 2475, 0,
		  48, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		  0x40, "5120x2160",	/* VIC 124 */
	},
	[125] = { 742500,
		  5120, 6216, 6304, 6600, 0,
		  2160, 2168, 2178, 2250, 0,
		  50, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		  0x40, "5120x2160",	/* VIC 125 */
	},
	[126] = { 742500,
		  5120, 5284, 5372, 5500, 0,
		  2160, 2168, 2178, 2250, 0,
		  60, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		  0x40, "5120x2160",	/* VIC 126 */
	},
	[127] = { 1485000,
		  5120, 6216, 6304, 6600, 0,
		  2160, 2168, 2178, 2250, 0,
		  100, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		  0x40, "5120x2160",	/* VIC 127 */
	},
	[193] = { 1485000,
		  5120, 5284, 5372, 5500, 0,
		  2160, 2168, 2178, 2250, 0,
		  120, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		  0x40, "5120x2160",	/* VIC 193 */
	},
	[194] = { 1188000,
		  7680, 10232, 10408, 11000, 0,
		  4320, 4336, 4356, 4500, 0,
		  24, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_16_9,
		  0x40, "7680x4320",	/* VIC 194 */
	},
	[195] = { 1188000,
		  7680, 10032, 10208, 10800, 0,
		  4320, 4336, 4356, 4400, 0,
		  25, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_16_9,
		  0x40, "7680x4320",	/* VIC 195 */
	},
	[196] = { 1188000,
		  7680, 8232, 8408, 9000, 0,
		  4320, 4336, 4356, 4400, 0,
		  30, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_16_9,
		  0x40, "7680x4320",	/* VIC 196 */
	},
	[197] = { 2376000,
		  7680, 10232, 10408, 11000, 0,
		  4320, 4336, 4356, 4500, 0,
		  48, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_16_9,
		  0x40, "7680x4320",	/* VIC 197 */
	},
	[198] = { 2376000,
		  7680, 10032, 10208, 10800, 0,
		  4320, 4336, 4356, 4400, 0,
		  50, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_16_9,
		  0x40, "7680x4320",	/* VIC 198 */
	},
	[199] = { 2376000,
		  7680, 8232, 8408, 9000, 0,
		  4320, 4336, 4356, 4400, 0,
		  60, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_16_9,
		  0x40, "7680x4320",	/* VIC 199 */
	},
	[200] = { 4752000,
		  7680, 9792, 9968, 10560, 0,
		  4320, 4336, 4356, 4500, 0,
		  100, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_16_9,
		  0x40, "7680x4320",	/* VIC 200 */
	},
	[201] = { 4752000,
		  7680, 8032, 8208, 8800, 0,
		  4320, 4336, 4356, 4500, 0,
		  120, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_16_9,
		  0x40, "7680x4320",	/* VIC 201 */
	},
	[202] = { 1188000,
		  7680, 10232, 10408, 11000, 0,
		  4320, 4336, 4356, 4500, 0,
		  24, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		  0x40, "7680x4320",	/* VIC 202 */
	},
	[203] = { 1188000,
		  7680, 10032, 10208, 10800, 0,
		  4320, 4336, 4356, 4400, 0,
		  25, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		  0x40, "7680x4320",	/* VIC 203 */
	},
	[204] = { 1188000,
		  7680, 8232, 8408, 9000, 0,
		  4320, 4336, 4356, 4400, 0,
		  30, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		  0x40, "7680x4320",	/* VIC 204 */
	},
	[205] = { 2376000,
		  7680, 10232, 10408, 11000, 0,
		  4320, 4336, 4356, 4500, 0,
		  48, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		  0x40, "7680x4320",	/* VIC 205 */
	},
	[206] = { 2376000,
		  7680, 10032, 10208, 10800, 0,
		  4320, 4336, 4356, 4400, 0,
		  50, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		  0x40, "7680x4320",	/* VIC 206 */
	},
	[207] = { 2376000,
		  7680, 8232, 8408, 9000, 0,
		  4320, 4336, 4356, 4400, 0,
		  60, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		  0x40, "7680x4320",	/* VIC 207 */
	},
	[208] = { 4752000,
		  7680, 9792, 9968, 10560, 0,
		  4320, 4336, 4356, 4500, 0,
		  100, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		  0x40, "7680x4320",	/* VIC 208 */
	},
	[209] = { 4752000,
		  7680, 8032, 8208, 8800, 0,
		  4320, 4336, 4356, 4500, 0,
		  120, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		  0x40, "7680x4320",	/* VIC 209 */
	},
	[210] = { 1485000,
		  10240, 11732, 11908, 12500, 0,
		  4320, 4336, 4356, 4950, 0,
		  24, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		  0x40, "10240x4320",	/* VIC 210 */
	},
	[211] = { 1485000,
		  10240, 12732, 12908, 13500, 0,
		  4320, 4336, 4356, 4400, 0,
		  25, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		  0x40, "10240x4320",	/* VIC 211 */
	},
	[212] = { 1485000,
		  10240, 10528, 10704, 11000, 0,
		  4320, 4336, 4356, 4500, 0,
		  30, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		  0x40, "10240x4320",	/* VIC 212 */
	},
	[213] = { 2970000,
		  10240, 11732, 11908, 12500, 0,
		  4320, 4336, 4356, 4950, 0,
		  48, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		  0x40, "10240x4320",	/* VIC 213 */
	},
	[214] = { 2970000,
		  10240, 12732, 12908, 13500, 0,
		  4320, 4336, 4356, 4400, 0,
		  50, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		  0x40, "10240x4320",	/* VIC 214 */
	},
	[215] = { 2970000,
		  10240, 10528, 10704, 11000, 0,
		  4320, 4336, 4356, 4500, 0,
		  60, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		  0x40, "10240x4320",	/* VIC 215 */
	},
	[216] = { 5940000,
		  10240, 12432, 12608, 13200, 0,
		  4320, 4336, 4356, 4500, 0,
		  100, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		  0x40, "10240x4320",	/* VIC 216 */
	},
	[217] = { 5940000,
		  10240, 10528, 10704, 11000, 0,
		  4320, 4336, 4356, 4500, 0,
		  120, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_64_27,
		  0x40, "10240x4320",	/* VIC 217 */
	},
	[218] = { 1188000,
		  4096, 4896, 4984, 5280, 0,
		  2160, 2168, 2178, 2250, 0,
		  100, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_256_135,
		  0x40, "4096x2160",	/* VIC 218 */
	},
	[219] = { 1188000,
		  4096, 4184, 4272, 4400, 0,
		  2160, 2168, 2178, 2250, 0,
		  120, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_PIC_AR_256_135,
		  0x40, "4096x2160",	/* VIC 219 */
	},
};

static void signal_handler(int signo)
{
	if (signo == SIGALRM)
		igt_info("Timeout and exit\n");
}

/* Common test setup. */
static void test_init(data_t *data, int conn_id)
{
	igt_display_t *display = &data->display;

	data->crtc = igt_first_crtc(&data->display);

	igt_display_reset(display);

	/* find a connected output */
	data->output = NULL;
	for (int i=0; i < data->display.n_outputs; ++i) {
		drmModeConnector *conn = data->display.outputs[i].config.connector;

		if ((data->use_virtual_connector &&
		     conn->connector_type == DRM_MODE_CONNECTOR_VIRTUAL) ||
		    (conn->connector_type == DRM_MODE_CONNECTOR_HDMIA &&
		     conn->connection == DRM_MODE_CONNECTED &&
		     (conn_id == 0 || conn->connector_id == conn_id))) {
			data->output = &data->display.outputs[i];
			break;
		}
	}

	igt_require_f(data->output, "No valid connectors found\n");

	data->primary =
		igt_crtc_get_plane_type(data->crtc, DRM_PLANE_TYPE_PRIMARY);

	igt_output_set_crtc(data->output,
			    data->crtc);

	if (data->timeout_seconds > 0) {
		struct sigaction sa;

		memset(&sa, 0, sizeof(struct sigaction));
		sa.sa_handler = signal_handler;
		/* without SA_RESTART so getchar() is not restarted on signal */
		sa.sa_flags = 0;

		if (sigaction(SIGALRM, &sa, NULL))
			igt_info("cannot set up timeout: %s\n", strerror(errno));
		else
			alarm(data->timeout_seconds);
	}
}

/* Common test cleanup. */
static void test_fini(data_t *data)
{
	igt_display_reset(&data->display);
}

static void wait_for_keypress(void)
{
	int c;

	do {
		c = getchar();
	} while (c != '\n' && c != EOF);
}

static void force_pixel_format(data_t *data, int pixel_format, int conn_id)
{
	int fd, res;
	const char *entry_name;

	test_init(data, conn_id);

	fd = igt_debugfs_connector_dir(data->fd, data->output->name, O_RDONLY);
	igt_assert(fd >= 0);

	igt_info("Setting %d on connector id %d\n",
			pixel_format, data->output->config.connector->connector_id);

	switch (pixel_format) {
	case 1:
		entry_name = "force_rgb_output";
		break;
	case 2:
		entry_name = "force_yuv422_output";
		break;
	case 3:
		entry_name = "force_yuv444_output";
		break;
	case 4:
		entry_name = "force_yuv420_output";
		break;
	default:
		goto out;
	}

	igt_info("%s\n", entry_name);
	res = igt_sysfs_write(fd, entry_name, "1", 2);
	igt_info("res = %d\n", res);
	igt_require(res > 0);

out:
	close(fd);
	test_fini(data);
}

/* Set "max bpc" property of connector */
static void set_max_bpc(data_t *data, int max_bpc, int conn_id)
{
	igt_display_t *display = &data->display;
	igt_fb_t afb;

	test_init(data, conn_id);

	igt_info("Setting max bpc to %d on connector id %d\n",
		 max_bpc, data->output->config.connector->connector_id);
	igt_create_pattern_fb(data->fd, 1024, 1024, DRM_FORMAT_XRGB8888, 0, &afb);
	igt_plane_set_fb(data->primary, &afb);
	igt_output_set_prop_value(data->output, IGT_CONNECTOR_MAX_BPC, max_bpc);
	igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

	igt_remove_fb(data->fd, &afb);
	test_fini(data);
}

/* Override video mode with specified VIC. */
static void test_vic_mode(data_t *data, int vic, int conn_id)
{
	igt_display_t *display = &data->display;
	drmModeModeInfo *mode;
	igt_fb_t afb;

	test_init(data, conn_id);

	mode = &test_modes[vic];

	igt_info("Setting mode %s on connector id %d\n",
		 mode->name, data->output->config.connector->connector_id);
	igt_output_override_mode(data->output, mode);
	igt_create_pattern_fb(data->fd, mode->hdisplay, mode->vdisplay, DRM_FORMAT_XRGB8888, 0, &afb);
	igt_plane_set_fb(data->primary, &afb);
	igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

	if (data->timeout_seconds > 0)
		pause(); /* block until SIGALRM fires (works when detached) */
	else {
		igt_info("Press [Enter] to finish\n");
		wait_for_keypress();
	}

	test_fini(data);
}

/*
 * Toggle HDMI content type (No Data <-> Game) on a live pipe to exercise HDMI
 * ALLM (HF1-56 steps 14/15) without tearing down the link.
 *
 * A full atomic modeset is performed once, then each toggle is first attempted
 * as a property-only atomic commit (no DRM_MODE_ATOMIC_ALLOW_MODESET). If the
 * kernel rejects it, the change is retried with ALLOW_MODESET. The path taken
 * is reported:
 *   [seamless, no modeset] -> ALLM toggled in-band, link stays up
 *   [MODESET required]     -> content-type change blanks/retrains the link
 */
static void test_allm_toggle(data_t *data, int conn_id)
{
	igt_display_t *display = &data->display;
	drmModeModeInfo *mode;
	uint32_t connector_id, prop_id;
	uint64_t cur_ct = 0;
	igt_fb_t afb;
	char line[16];
	int game = 0;

	test_init(data, conn_id);

	connector_id = data->output->config.connector->connector_id;
	igt_require_f(kmstest_get_property(data->fd, connector_id,
					   DRM_MODE_OBJECT_CONNECTOR,
					   "content type", &prop_id, &cur_ct,
					   NULL),
		      "connector has no 'content type' property\n");

	/* Initial full atomic modeset with a test pattern. */
	mode = igt_output_get_mode(data->output);
	igt_info("Using connector id %d, mode %s\n", connector_id, mode->name);
	igt_create_pattern_fb(data->fd, mode->hdisplay, mode->vdisplay,
			      DRM_FORMAT_XRGB8888, 0, &afb);
	igt_plane_set_fb(data->primary, &afb);
	igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

	igt_info("\nMode set. content type = No Data (0).\n");
	igt_info("Press [Enter] to toggle Game <-> No Data, 'q'+[Enter] to quit.\n\n");

	while (fgets(line, sizeof(line), stdin)) {
		drmModeAtomicReq *req;
		uint64_t val;
		int ret;

		if (line[0] == 'q')
			break;

		game = !game;
		val = game ? DRM_MODE_CONTENT_TYPE_GAME :
			     DRM_MODE_CONTENT_TYPE_NO_DATA;

		req = drmModeAtomicAlloc();
		igt_assert(req);
		drmModeAtomicAddProperty(req, connector_id, prop_id, val);

		/* Try a seamless, property-only commit first. */
		ret = drmModeAtomicCommit(data->fd, req, 0, NULL);
		if (ret) {
			int nomodeset_err = errno;

			ret = drmModeAtomicCommit(data->fd, req,
						  DRM_MODE_ATOMIC_ALLOW_MODESET,
						  NULL);
			igt_info("content type = %-7s (%llu)  [MODESET required, link blanked] (nomodeset err: %s)\n",
				 game ? "Game" : "No Data",
				 (unsigned long long)val,
				 strerror(nomodeset_err));
		} else {
			igt_info("content type = %-7s (%llu)  [seamless, no modeset]\n",
				 game ? "Game" : "No Data",
				 (unsigned long long)val);
		}
		if (ret)
			igt_warn("commit failed: %s\n", strerror(errno));

		drmModeAtomicFree(req);
	}

	igt_remove_fb(data->fd, &afb);
	test_fini(data);
}

const char *optstr = "hvt:i:b:y:e:a";
static void usage(const char *name)
{
	igt_info("Usage: %s options\n", name);
	igt_info("-h		Show help\n");
	igt_info("-t vic	Select video mode based on VIC\n");
	igt_info("-v		Test on 'Virtual' connector as well, for debugging.\n");
	igt_info("-i conn_id	Use connector by ID\n");
	igt_info("-b 6|8|10|12|16	Set 6|8|10|12|16 bpc\n");
	igt_info("-y 1|2|3|4	Set RGB|YUV422|YUV444|YUV420\n");
	igt_info("-e seconds    number of seconds to display test pattern and exit\n");
	igt_info("-a            Toggle HDMI content type (ALLM) Game <-> No Data on a live pipe\n");
	igt_info("NOTE: if -i is not specified, first connected HDMI connector will be used for -t, -b, -y and -a\n");
}

int main(int argc, char **argv)
{
	data_t data;
	int c;
	int vic = 0;
	int conn_id = 0;
	int max_bpc = 0;
	int pixel_format = PIXEL_ENCODING_UNKNOWN;
	bool allm = false;

	memset(&data, 0, sizeof(data));

	while((c = getopt(argc, argv, optstr)) != -1) {
		switch(c) {
		case 't':
			vic = atoi(optarg);
			break;
		case 'i':
			conn_id = atoi(optarg);
			break;
		case 'v':
			data.use_virtual_connector = true;
			break;
		case 'b':
			max_bpc = atoi(optarg);
			break;
		case 'y':
			pixel_format = atoi(optarg);
			break;
		case 'e':
			data.timeout_seconds = atoi(optarg);
			break;
		case 'a':
			allm = true;
			break;
		default:
		case 'h':
			usage(argv[0]);
			exit(EXIT_SUCCESS);
		}
	}

	data.fd = drm_open_driver_master(DRIVER_ANY);
	kmstest_set_vt_graphics_mode();

	igt_display_require(&data.display, data.fd);
	igt_require(data.display.is_atomic);
	igt_display_require_output(&data.display);

	if (pixel_format >= PIXEL_ENCODING_RGB &&
		pixel_format <= PIXEL_ENCODING_YCBCR420)
		force_pixel_format(&data, pixel_format, conn_id);

	if (max_bpc)
		set_max_bpc(&data, max_bpc, conn_id);

	if (allm) {
		test_allm_toggle(&data, conn_id);
	} else if (vic >= 0) {
		if (vic > ARRAY_SIZE(test_modes) || !test_modes[vic].name[0])
			igt_warn("VIC %d is not supported\n", vic);
		else
			test_vic_mode(&data, vic, conn_id);
	}

	igt_display_fini(&data.display);
}
