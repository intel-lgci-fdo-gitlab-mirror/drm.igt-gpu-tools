/*
 * Copyright © 2019-2024 Intel Corporation
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

#include <errno.h>
#include <fcntl.h>
#ifdef ANDROID
#include "android/glib.h"
#else
#include <glib.h>
#endif
#include <libudev.h>
#include <string.h>
#include <signal.h>
#include <sys/ioctl.h>

#include "igt_core.h"
#include "igt_device_scan.h"
#include "igt_rc.h"

/**
 * SECTION:lsgpu
 * @short_description: lsgpu
 * @title: lsgpu
 * @include: lsgpu.c
 *
 * # lsgpu
 *
 * The devices can be scanned and displayed using 'lsgpu' tool. Tool also
 * displays properties and sysattrs (-p switch, means print detail) which
 * can be used during filter implementation.
 *
 * Tool can also be used to try out filters.
 * To select device use '-d' or '--device' argument like:
 *
 * |[<!-- language="plain" -->
 * ./lsgpu -d 'pci:vendor=Intel'
 * === Device filter list ===
 * [ 0]: pci:vendor=Intel

 * === Testing device open ===
 * subsystem   : pci
 * drm card    : /dev/dri/card0
 * drm render  : /dev/dri/renderD128
 * Device /dev/dri/card0 successfully opened
 * Device /dev/dri/renderD128 successfully opened
 * ]|
 *
 * NOTE: When using filters only the first matching device is printed.
 *
 * Additionally lsgpu tries to open the card and render nodes to verify
 * permissions. It also uses IGT variable search order:
 * - use --device first (it overrides IGT_DEVICE and .igtrc Common::Device
 *   settings)
 * - use IGT_DEVICE enviroment variable if no --device are passed
 * - use .igtrc Common::Device if no --device nor IGT_DEVICE are passed
 */

enum {
	OPT_PRINT_SIMPLE   = 's',
	OPT_PRINT_DETAIL   = 'p',
	OPT_NUMERIC        = 'n',
	OPT_CODENAME       = 'c',
	OPT_LIST_VENDORS   = 'v',
	OPT_LIST_FILTERS   = 'l',
	OPT_DEVICE         = 'd',
	OPT_HELP           = 'h',
	OPT_PCISCAN        = 'P',
	OPT_VERSION        = 'V',
};

static bool g_show_vendors;
static bool g_list_filters;
static bool g_help;
static bool g_pciscan;
static bool g_version;
static char *igt_device;

static const char *usage_str =
	"usage: lsgpu [options]\n\n"
	"Options:\n"
	"  -n, --numeric               Print vendor/device as hex\n"
	"  -c, --codename              Print codename instead pretty device name\n"
	"  -s, --print-simple          Print simple (legacy) device details\n"
	"  -p, --print-details         Print devices with details\n"
	"  -P, --pci-scan              Print pci display devices\n"
	"  -v, --list-vendors          List recognized vendors\n"
	"  -l, --list-filter-types     List registered device filters types\n"
	"  -d, --device filter         Device filter, can be given multiple times\n"
	"  -V, --version               Show version information and exit\n"
	"  -h, --help                  Show this help message and exit\n"
	"\nOptions valid for default print out mode only:\n"
	"      --drm                   Show DRM filters (default) for each device\n"
	"      --sysfs                 Show sysfs filters for each device\n"
	"      --pci                   Show PCI filters for each device\n";

static void show_version(void)
{
	printf("lsgpu version 1.0\n"
	       "Copyright © 2019-2024 Intel Corporation\n"
	       "This is free software; see the source for copying conditions.  There is NO\n"
	       "warranty; not even for MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.\n");
}

static void test_device_open(struct igt_device_card *card)
{
	int fd;

	if (!card)
		return;

	fd = igt_open_card(card);
	if (fd >= 0) {
		printf("Device %s successfully opened\n", card->card);
		close(fd);
	} else {
		if (strlen(card->card))
			printf("Cannot open card %s device\n", card->card);
		else
			printf("Cannot open card device, empty name\n");
	}

	fd = igt_open_render(card);
	if (fd >= 0) {
		printf("Device %s successfully opened\n", card->render);
		close(fd);
	} else {
		if (strlen(card->render))
			printf("Cannot open render %s device\n", card->render);
		else
			printf("Cannot open render device, empty name\n");
	}
}

static void print_card(struct igt_device_card *card)
{
	if (!card)
		return;

	printf("subsystem   : %s\n", card->subsystem);
	printf("drm card    : %s\n", card->card);
	printf("drm render  : %s\n", card->render);
}

static char *get_device_from_rc(void)
{
	char *rc_device = NULL;
	GError *error = NULL;
	GKeyFile *key_file = igt_load_igtrc();

	if (key_file == NULL)
		return NULL;

	rc_device = g_key_file_get_string(key_file, "Common",
					  "Device", &error);

	g_clear_error(&error);

	return rc_device;
}

static int pciscan(void)
{
	struct udev *udev;
	struct udev_enumerate *enumerate;
	struct udev_list_entry *devices, *dev_list_entry;
	struct igt_device_card card;
	char pcistr[10];
	int ret;

	udev = udev_new();
	igt_assert(udev);

	enumerate = udev_enumerate_new(udev);
	igt_assert(enumerate);

	printf("Scanning pci subsystem\n");
	printf("----------------------\n");
	ret = udev_enumerate_add_match_subsystem(enumerate, "pci");
	igt_assert(!ret);

	ret = udev_enumerate_add_match_property(enumerate, "PCI_CLASS", "30000");
	igt_assert(!ret);
	ret = udev_enumerate_add_match_property(enumerate, "PCI_CLASS", "38000");
	igt_assert(!ret);

	ret = udev_enumerate_scan_devices(enumerate);
	igt_assert(!ret);

	devices = udev_enumerate_get_list_entry(enumerate);
	if (!devices) {
		printf("No pci devices with class 0x30000|0x38000 found\n");
		goto out;
	}

	udev_list_entry_foreach(dev_list_entry, devices) {
		const char *path;
		struct udev_device *udev_dev;
		struct udev_list_entry *entry;
		char *codename;

		path = udev_list_entry_get_name(dev_list_entry);
		udev_dev = udev_device_new_from_syspath(udev, path);
		printf("[%s]\n", path);

		strcpy(card.pci_slot_name, "-");
		entry = udev_device_get_properties_list_entry(udev_dev);
		while (entry) {
			const char *name = udev_list_entry_get_name(entry);
			const char *value = udev_list_entry_get_value(entry);

			entry = udev_list_entry_get_next(entry);
			if (!strcmp(name, "ID_VENDOR_FROM_DATABASE"))
				printf("  vendor [db]: %s\n", value);
			else if (!strcmp(name, "ID_MODEL_FROM_DATABASE"))
				printf("  model  [db]: %s\n", value);
			else if (!strcmp(name, "DRIVER"))
				printf("  driver     : %s\n", value);
			else if (!strcmp(name, "PCI_ID"))
				igt_assert_eq(sscanf(value, "%hx:%hx",
						     &card.pci_vendor, &card.pci_device), 2);
		}
		snprintf(pcistr, sizeof(pcistr), "%04x:%04x",
			 card.pci_vendor, card.pci_device);
		printf("  pci id     : %s\n", pcistr);
		codename = igt_device_get_pretty_name(&card, false);
		if (strcmp(pcistr, codename))
			printf("  codename   : %s\n", codename);
		free(codename);

		udev_device_unref(udev_dev);
	}

out:
	udev_enumerate_unref(enumerate);
	udev_unref(udev);

	return 0;
}

int main(int argc, char *argv[])
{
	static struct option long_options[] = {
		{"drm",               no_argument,       NULL, 0},
		{"sysfs",             no_argument,       NULL, 1},
		{"pci",               no_argument,       NULL, 2},
		{"numeric",           no_argument,       NULL, OPT_NUMERIC},
		{"codename",          no_argument,       NULL, OPT_CODENAME},
		{"print-simple",      no_argument,       NULL, OPT_PRINT_SIMPLE},
		{"print-detail",      no_argument,       NULL, OPT_PRINT_DETAIL},
		{"pci-scan",          no_argument,       NULL, OPT_PCISCAN},
		{"list-vendors",      no_argument,       NULL, OPT_LIST_VENDORS},
		{"list-filter-types", no_argument,       NULL, OPT_LIST_FILTERS},
		{"device",            required_argument, NULL, OPT_DEVICE},
		{"help",              no_argument,       NULL, OPT_HELP},
		{"version",           no_argument,       NULL, OPT_VERSION},
		{0, 0, 0, 0}
	};
	int c, ret = 0, index = 0;
	char *env_device = NULL, *opt_device = NULL, *rc_device = NULL;
	struct igt_devices_print_format fmt = {
			.type = IGT_PRINT_USER,
	};

	while ((c = getopt_long(argc, argv, "ncspvld:hPV",
				long_options, &index)) != -1) {
		switch(c) {

		case OPT_NUMERIC:
			fmt.numeric = true;
			break;
		case OPT_CODENAME:
			fmt.codename = true;
			break;
		case OPT_PRINT_SIMPLE:
			fmt.type = IGT_PRINT_SIMPLE;
			break;
		case OPT_PRINT_DETAIL:
			fmt.type = IGT_PRINT_DETAIL;
			break;
		case OPT_LIST_VENDORS:
			g_show_vendors = true;
			break;
		case OPT_LIST_FILTERS:
			g_list_filters = true;
			break;
		case OPT_DEVICE:
			opt_device = strdup(optarg);
			break;
		case OPT_HELP:
			g_help = true;
			break;
		case OPT_PCISCAN:
			g_pciscan = true;
			break;
		case OPT_VERSION:
			g_version = true;
			break;
		case 0:
			fmt.option = IGT_PRINT_DRM;
			break;
		case 1:
			fmt.option = IGT_PRINT_SYSFS;
			break;
		case 2:
			fmt.option = IGT_PRINT_PCI;
			break;
		}
	}

	if (g_pciscan)
		return pciscan();

	if (g_help) {
		printf("%s\n", usage_str);
		exit(0);
	}

	if (g_version) {
		show_version();
		exit(0);
	}

	if (g_show_vendors) {
		igt_devices_print_vendors();
		return 0;
	}

	if (g_list_filters) {
		igt_device_print_filter_types();
		return 0;
	}

	env_device = getenv("IGT_DEVICE");
	rc_device = get_device_from_rc();

	if (opt_device != NULL) {
		igt_device = opt_device;
		printf("Notice: Using filter supplied via --device\n");
	}
	else if (env_device != NULL) {
		igt_device = env_device;
		printf("Notice: Using filter from IGT_DEVICE env variable\n");
	}
	else if (rc_device != NULL) {
		igt_device = rc_device;
		printf("Notice: Using filter from .igtrc\n");
	}

	igt_devices_scan_all_attrs();

	if (igt_device != NULL) {
		struct igt_device_card *cards;
		int matched;

		printf("=== Device filter ===\n");
		printf("%s\n\n", igt_device);

		printf("=== Testing device open ===\n");

		matched = igt_device_card_match_all(igt_device, &cards);
		if (!matched) {
			printf("No device found for the filter\n\n");
			ret = -1;
			goto out;
		}

		for (int i = 0; i < matched; i++) {
			printf("Device detail:\n");
			print_card(&cards[i]);
			test_device_open(&cards[i]);
			if (fmt.type == IGT_PRINT_DETAIL) {
				printf("\n");
				igt_devices_print(&fmt);
			}
			printf("-------------------------------------------\n");
		}
		free(cards);
	} else {
		igt_devices_print(&fmt);
	}
out:
	igt_devices_free();
	free(rc_device);
	free(opt_device);

	return ret;
}
