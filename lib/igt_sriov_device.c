// SPDX-License-Identifier: MIT
/*
 * Copyright(c) 2023 Intel Corporation. All rights reserved.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pciaccess.h>

#include "drmtest.h"
#include "igt_core.h"
#include "igt_device.h"
#include "igt_sriov_device.h"
#include "igt_sysfs.h"
#include "intel_io.h"
#include "xe/xe_query.h"

/**
 * igt_sriov_is_pf - Check if device is PF
 * @device: device file descriptor
 *
 * Determines if a device is a Physical Function (PF) by verifying
 * the presence of the sriov_totalvfs attribute and ensuring its
 * read value is greater than zero.
 *
 * Return:
 * True if device is PF, false otherwise.
 */
bool igt_sriov_is_pf(int device)
{
	uint32_t value = 0;
	int sysfs;

	sysfs = igt_sysfs_open(device);
	igt_assert_fd(sysfs);

	__igt_sysfs_get_u32(sysfs, "device/sriov_totalvfs", &value);
	close(sysfs);

	return value > 0;
}

/**
 * igt_sriov_func_str - Return "pf" or "vf%u" label for a function number
 * @vf_num: 0 for PF, >0 for VF index
 *
 * Helper for constructing SR-IOV sysfs paths.
 *
 * Returns: "pf" for @vf_num == 0, otherwise "vf%u".
 */
const char *igt_sriov_func_str(unsigned int vf_num)
{
	static __thread char buf[16];

	return igt_sriov_function_name_lower(vf_num, buf, sizeof(buf));
}

static const char *sriov_function_name(unsigned int vf_num, bool uppercase,
				       char *buf, size_t size)
{
	int ret;

	igt_assert(buf && size);

	if (vf_num)
		ret = snprintf(buf, size, uppercase ? "VF%u" : "vf%u", vf_num);
	else
		ret = snprintf(buf, size, uppercase ? "PF" : "pf");

	igt_assert(ret >= 0 && (size_t)ret < size);

	return buf;
}

/**
 * igt_sriov_function_name_lower - Get SR-IOV function name in lowercase
 * @vf_num: VF number (1-based to identify single VF) or 0 for PF
 * @buf: buffer
 * @size: size of the buffer
 *
 * It formats the function name as "pf" when @vf_num is 0, or as "vf%u"
 * when @vf_num is greater than 0.
 *
 * Return: formatted function name.
 */
const char *igt_sriov_function_name_lower(unsigned int vf_num, char *buf,
					 size_t size)
{
	return sriov_function_name(vf_num, false, buf, size);
}

/**
 * igt_sriov_function_name_upper - Get SR-IOV function name in uppercase
 * @vf_num: VF number (1-based to identify single VF) or 0 for PF
 * @buf: buffer
 * @size: size of the buffer
 *
 * It formats the function name as "PF" when @vf_num is 0, or as "VF%u"
 * when @vf_num is greater than 0.
 *
 * Return: formatted function name.
 */
const char *igt_sriov_function_name_upper(unsigned int vf_num, char *buf,
					 size_t size)
{
	return sriov_function_name(vf_num, true, buf, size);
}

static bool __pf_attr_get_u32(int pf, const char *attr, uint32_t *value)
{
	int sysfs;
	bool ret;

	igt_assert(igt_sriov_is_pf(pf));

	sysfs = igt_sysfs_open(pf);
	igt_assert_fd(sysfs);

	ret = __igt_sysfs_get_u32(sysfs, attr, value);
	close(sysfs);

	return ret;
}

static uint32_t pf_attr_get_u32(int pf, const char *attr)
{
	uint32_t value;

	igt_assert_f(__pf_attr_get_u32(pf, attr, &value),
		     "Failed to read %s attribute (%s)\n", attr, strerror(errno));

	return value;
}

static bool __pf_attr_set_u32(int pf, const char *attr, uint32_t value)
{
	int sysfs;
	bool ret;

	igt_assert(igt_sriov_is_pf(pf));

	sysfs = igt_sysfs_open(pf);
	igt_assert_fd(sysfs);

	ret = __igt_sysfs_set_u32(sysfs, attr, value);
	close(sysfs);

	return ret;
}

static void pf_attr_set_u32(int pf, const char *attr, uint32_t value)
{
	igt_assert_f(__pf_attr_set_u32(pf, attr, value),
		     "Failed to write %u to %s attribute (%s)\n", value, attr, strerror(errno));
}

/**
 * igt_sriov_vfs_supported - Check if VFs are supported
 * @pf: PF device file descriptor
 *
 * Determine VFs support by checking if value of sriov_totalvfs attribute
 * corresponding to @pf device is bigger than 0.
 *
 * Return:
 * True if VFs are supported, false otherwise.
 */
bool igt_sriov_vfs_supported(int pf)
{
	uint32_t totalvfs;

	if (!__pf_attr_get_u32(pf, "device/sriov_totalvfs", &totalvfs))
		return false;

	return totalvfs > 0;
}

/**
 * igt_sriov_get_totalvfs - Get maximum number of VFs that can be enabled
 * @pf: PF device file descriptor
 *
 * Maximum number of VFs that can be enabled is checked by reading
 * sriov_totalvfs attribute corresponding to @pf device.
 *
 * It asserts on failure.
 *
 * Return:
 * Maximum number of VFs that can be associated with given PF.
 */
unsigned int igt_sriov_get_total_vfs(int pf)
{
	return pf_attr_get_u32(pf, "device/sriov_totalvfs");
}

/**
 * igt_sriov_get_numvfs - Get number of enabled VFs
 * @pf: PF device file descriptor
 *
 * Number of enabled VFs is checked by reading sriov_numvfs attribute
 * corresponding to @pf device.
 *
 * It asserts on failure.
 *
 * Return:
 * Number of VFs enabled by given PF.
 */
unsigned int igt_sriov_get_enabled_vfs(int pf)
{
	return pf_attr_get_u32(pf, "device/sriov_numvfs");
}

/**
 * igt_sriov_enable_vfs - Enable VFs
 * @pf: PF device file descriptor
 * @num_vfs: Number of virtual functions to be enabled
 *
 * Enable VFs by writing @num_vfs to sriov_numvfs attribute corresponding to
 * @pf device.
 * It asserts on failure.
 */
void igt_sriov_enable_vfs(int pf, unsigned int num_vfs)
{
	int sysfs;
	int ret;

	igt_assert(igt_sriov_is_pf(pf));
	igt_assert(num_vfs > 0);

	sysfs = igt_sysfs_open(pf);
	igt_assert_fd(sysfs);

	igt_debug("Enabling %u VFs\n", num_vfs);
	ret = igt_sysfs_printf(sysfs, "device/sriov_numvfs", "%u", num_vfs);
	close(sysfs);

	igt_require_f(ret != -ENOENT, "SR-IOV not supported\n");
	igt_assert_f(ret > 0, "Failed to write %u to device/sriov_numvfs (%s)\n",
		     num_vfs, strerror(-ret));
}

/**
 * igt_sriov_disable_vfs - Disable VFs
 * @pf: PF device file descriptor
 *
 * Disable VFs by writing 0 to sriov_numvfs attribute corresponding to @pf
 * device.
 * It asserts on failure.
 */
void igt_sriov_disable_vfs(int pf)
{
	pf_attr_set_u32(pf, "device/sriov_numvfs", 0);
}

/**
 * igt_sriov_is_driver_autoprobe_enabled - Get VF driver autoprobe setting
 * @pf: PF device file descriptor
 *
 * Get current VF driver autoprobe setting by reading sriov_drivers_autoprobe
 * attribute corresponding to @pf device.
 *
 * It asserts on failure.
 *
 * Return:
 * True if autoprobe is enabled, false otherwise.
 */
bool igt_sriov_is_driver_autoprobe_enabled(int pf)
{
	return pf_attr_get_u32(pf, "device/sriov_drivers_autoprobe");
}

/**
 * igt_sriov_enable_driver_autoprobe - Enable VF driver autoprobe
 * @pf: PF device file descriptor
 *
 * Enable VF driver autoprobe setting by writing 1 to sriov_drivers_autoprobe
 * attribute corresponding to @pf device.
 *
 * If successful, kernel will automatically bind VFs to a compatible driver
 * immediately after they are enabled.
 * It asserts on failure.
 */
void igt_sriov_enable_driver_autoprobe(int pf)
{
	pf_attr_set_u32(pf,  "device/sriov_drivers_autoprobe", true);
}

/**
 * igt_sriov_disable_driver_autoprobe - Disable VF driver autoprobe
 * @pf: PF device file descriptor
 *
 * Disable VF driver autoprobe setting by writing 0 to sriov_drivers_autoprobe
 * attribute corresponding to @pf device.
 *
 * During VFs enabling driver won't be bound to VFs.
 * It asserts on failure.
 */
void igt_sriov_disable_driver_autoprobe(int pf)
{
	pf_attr_set_u32(pf,  "device/sriov_drivers_autoprobe", false);
}

/**
 * igt_sriov_open_vf_drm_device - Open VF DRM device node
 * @pf: PF device file descriptor
 * @vf_num: VF number (1-based to identify single VF)
 *
 * Open DRM device node for given VF.
 *
 * Return:
 * VF file descriptor or -1 on error.
 */
int igt_sriov_open_vf_drm_device(int pf, unsigned int vf_num)
{
	char dir_path[PATH_MAX], path[256], dev_name[16];
	DIR *dir;
	struct dirent *de;
	bool found = false;
	int fd;

	if (!vf_num)
		return -1;

	if (!igt_sysfs_path(pf, path, sizeof(path)))
		return -1;
	/* vf_num is 1-based, but virtfn is 0-based */
	snprintf(dir_path, sizeof(dir_path), "%s/device/virtfn%u/drm", path, vf_num - 1);

	dir = opendir(dir_path);
	if (!dir)
		return -1;
	while ((de = readdir(dir))) {
		unsigned int card_num;

		if (sscanf(de->d_name, "card%d", &card_num) == 1) {
			snprintf(dev_name, sizeof(dev_name), "/dev/dri/card%u", card_num);
			found = true;
			break;
		}
	}
	closedir(dir);

	if (!found)
		return -1;

	fd = __drm_open_device(dev_name, DRIVER_ANY);
	if (fd >= 0 && is_xe_device(fd))
		xe_device_get(fd);

	return fd;
}

/**
 * igt_sriov_is_vf_drm_driver_probed - Check if VF DRM driver is probed
 * @pf: PF device file descriptor
 * @vf_num: VF number (1-based to identify single VF)
 *
 * Verify if DRM driver is bound to VF device. Probe check is based on
 * existence of the DRM subsystem attribute in sysfs.
 *
 * Returns:
 * True if VF has DRM driver loaded, false if not.
 */
bool igt_sriov_is_vf_drm_driver_probed(int pf, unsigned int vf_num)
{
	char path[PATH_MAX];
	int sysfs;
	bool ret;

	igt_assert(vf_num > 0);

	sysfs = igt_sysfs_open(pf);
	igt_assert_fd(sysfs);
	/* vf_num is 1-based, but virtfn is 0-based */
	snprintf(path, sizeof(path), "device/virtfn%u/drm", vf_num - 1);
	ret = igt_sysfs_has_attr(sysfs, path);
	close(sysfs);

	return ret;
}

/*
 * __igt_sriov_get_vf_pci_slot_alloc:
 * @pf_sysfs: sysfs directory file descriptor
 * @vf_num: VF number (1-based)
 *
 * Resolve symbolic link from virtfnX to obtain the PCI slot address.
 * Returns a dynamically allocated string containing the PCI slot address,
 * or NULL if the link cannot be resolved.
 * The caller is responsible for freeing the returned memory.
 */
static char *__igt_sriov_get_vf_pci_slot_alloc(int pf_sysfs, unsigned int vf_num)
{
	char dir_path[PATH_MAX];
	char path[PATH_MAX];
	char *pci_slot_addr;
	int len;

	/* Adjust for 0-based index as vf_num is 1-based */
	if (vf_num)
		snprintf(dir_path, sizeof(dir_path), "device/virtfn%u",
			 vf_num - 1);
	else
		snprintf(dir_path, sizeof(dir_path), "device");

	len = readlinkat(pf_sysfs, dir_path, path, sizeof(path));
	if (len <= 0)
		return NULL;

	path[len] = '\0';
	pci_slot_addr = strrchr(path, '/') + 1;

	return pci_slot_addr ? strdup(pci_slot_addr) : NULL;
}

/**
 * igt_sriov_get_vf_pci_slot_alloc:
 * @pf: PF device file descriptor
 * @vf_num: VF number (1-based); 0 means PF
 *
 * Resolves the symlink under the PF sysfs directory (device/virtfnX) to obtain
 * the PCI slot address of the VF (or PF if @vf_num is 0).
 *
 * Returns: newly allocated PCI slot address string (e.g. "0000:3b:00.4"),
 * or NULL on failure. Caller must free().
 */
char *igt_sriov_get_vf_pci_slot_alloc(int pf, unsigned int vf_num)
{
	char *pci_slot;
	int sysfs;

	sysfs = igt_sysfs_open(pf);
	igt_assert_fd(sysfs);

	pci_slot = __igt_sriov_get_vf_pci_slot_alloc(sysfs, vf_num);
	close(sysfs);

	return pci_slot;
}

static bool __igt_sriov_bind_vf_drm_driver(int pf, unsigned int vf_num, bool bind)
{
	char *pci_slot;
	int sysfs;
	bool ret;

	igt_assert(vf_num > 0);

	sysfs = igt_sysfs_open(pf);
	igt_assert_fd(sysfs);

	pci_slot = __igt_sriov_get_vf_pci_slot_alloc(sysfs, vf_num);
	igt_assert(pci_slot);

	igt_debug("vf_num: %u, pci_slot: %s\n", vf_num, pci_slot);
	ret = igt_sysfs_set(sysfs, bind ? "device/driver/bind" : "device/driver/unbind", pci_slot);

	free(pci_slot);
	close(sysfs);

	return ret;
}

/**
 * igt_sriov_bind_vf_drm_driver - Bind DRM driver to VF
 * @pf: PF device file descriptor
 * @vf_num: VF number (1-based to identify single VF)
 *
 * Bind the DRM driver to given VF.
 * It asserts on failure.
 */
void igt_sriov_bind_vf_drm_driver(int pf, unsigned int vf_num)
{
	igt_assert(__igt_sriov_bind_vf_drm_driver(pf, vf_num, true));
}

/**
 * igt_sriov_unbind_vf_drm_driver - Unbind DRM driver from VF
 * @pf: PF device file descriptor
 * @vf_num: VF number (1-based to identify single VF)
 *
 * Unbind the DRM driver from given VF.
 * It asserts on failure.
 */
void igt_sriov_unbind_vf_drm_driver(int pf, unsigned int vf_num)
{
	igt_assert(__igt_sriov_bind_vf_drm_driver(pf, vf_num, false));
}

/**
 * igt_sriov_device_sysfs_open:
 * @pf: PF device file descriptor
 * @vf_num: VF number (1-based to identify single VF) or 0 for PF
 *
 * Open the sysfs directory corresponding to SR-IOV device.
 *
 * Returns:
 * The SR-IOV device sysfs directory fd, -1 on failure.
 */
int igt_sriov_device_sysfs_open(int pf, unsigned int vf_num)
{
	char path[PATH_MAX];
	int sysfs, fd;

	sysfs = igt_sysfs_open(pf);
	if (sysfs < 0)
		return -1;

	if (!vf_num)
		snprintf(path, sizeof(path), "device");
	else
		/* vf_num is 1-based, but virtfn is 0-based */
		snprintf(path, sizeof(path), "device/virtfn%u", vf_num - 1);

	fd = openat(sysfs, path, O_DIRECTORY | O_RDONLY);
	close(sysfs);

	return fd;
}

/**
 * igt_sriov_device_reset_exists:
 * @pf: PF device file descriptor
 * @vf_num: VF number (1-based to identify single VF) or 0 for PF
 *
 * Check if reset attribute exists for a given SR-IOV device.
 *
 * Returns:
 * True if reset attribute exists, false otherwise.
 */
bool igt_sriov_device_reset_exists(int pf, unsigned int vf_num)
{
	int sysfs;
	bool reset_exists;

	sysfs = igt_sriov_device_sysfs_open(pf, vf_num);
	if (sysfs < 0)
		return false;

	reset_exists = igt_sysfs_has_attr(sysfs, "reset");
	close(sysfs);

	return reset_exists;
}

/**
 * igt_sriov_device_reset:
 * @pf: PF device file descriptor
 * @vf_num: VF number (1-based to identify single VF) or 0 for PF
 *
 * Trigger FLR on a given VF.
 *
 * Returns:
 * True on success, false on failure.
 */
bool igt_sriov_device_reset(int pf, unsigned int vf_num)
{
	int sysfs;
	bool ret;

	sysfs = igt_sriov_device_sysfs_open(pf, vf_num);
	if (sysfs < 0)
		return false;

	igt_debug("Initiating FLR on VF%d\n", vf_num);
	ret = igt_sysfs_set(sysfs, "reset", "1");
	close(sysfs);

	return ret;
}

/**
 * intel_is_vf_device - Check if device is VF
 * @device: device file descriptor
 *
 * Determines if a device is a Virtual Function (VF)
 * by reading VF_CAPABILITY_REGISTER. If the least
 * significant bit is set the device is VF.
 *
 * Return:
 * True if device is VF, false otherwise.
 */
bool intel_is_vf_device(int fd)
{
#define VF_CAP_REG		0x1901f8
	struct intel_mmio_data mmio_data;
	uint32_t value;

	intel_register_access_init(&mmio_data, igt_device_get_pci_device(fd), false);
	value = intel_register_read(&mmio_data, VF_CAP_REG);
	intel_register_access_fini(&mmio_data);
	igt_require((value & ~1) == 0);

	return (value & 1) != 0;
}

struct sriov_exit_handler_ctx {
	int pf;
	bool autoprobe;
	igt_sriov_exit_cleanup_fn cleanup_fn;
	void *cleanup_data;
};

static struct sriov_exit_handler_ctx sriov_exit_ctx = {
	.pf = -1,
};

static void sriov_exit_handler_release_fd(void)
{
	if (sriov_exit_ctx.pf < 0)
		return;

	__drm_close_driver(sriov_exit_ctx.pf);
	sriov_exit_ctx.pf = -1;
}

static void sriov_exit_handler(int sig)
{
	if (sriov_exit_ctx.pf < 0)
		return;

	igt_sriov_disable_vfs(sriov_exit_ctx.pf);

	if (sriov_exit_ctx.cleanup_fn)
		sriov_exit_ctx.cleanup_fn(sriov_exit_ctx.pf, sig,
					  sriov_exit_ctx.cleanup_data);

	if (sriov_exit_ctx.autoprobe)
		igt_sriov_enable_driver_autoprobe(sriov_exit_ctx.pf);
	else
		igt_sriov_disable_driver_autoprobe(sriov_exit_ctx.pf);
}

/**
 * igt_sriov_install_exit_handler - Install best-effort SR-IOV cleanup handler
 * @pf: PF device file descriptor
 * @cleanup_fn: Optional callback invoked after VF disable
 * @cleanup_data: Opaque callback data
 *
 * Registers a process-exit cleanup routine for SR-IOV tests. The handler runs
 * from normal and signal-triggered exits (when possible), disables VFs,
 * invokes @cleanup_fn if provided (passing signal number, or 0 on normal
 * exit), and restores the original autoprobe setting captured at
 * installation time. Tests that perform full explicit teardown should call
 * igt_sriov_clear_exit_handler() once that teardown succeeds. An internal
 * duplicate of @pf is kept so cleanup remains usable even if the caller closes
 * the original DRM fd before process exit.
 */
void igt_sriov_install_exit_handler(int pf,
				    igt_sriov_exit_cleanup_fn cleanup_fn,
				    void *cleanup_data)
{
	int pf_dup;

	if (!igt_sriov_is_pf(pf))
		return;

	pf_dup = dup(pf);
	igt_assert_f(pf_dup >= 0, "Failed to duplicate PF fd (%s)\n",
		     strerror(errno));

	sriov_exit_handler_release_fd();

	sriov_exit_ctx.pf = pf_dup;
	sriov_exit_ctx.autoprobe = igt_sriov_is_driver_autoprobe_enabled(pf_dup);
	sriov_exit_ctx.cleanup_fn = cleanup_fn;
	sriov_exit_ctx.cleanup_data = cleanup_data;

	igt_install_exit_handler(sriov_exit_handler);
}

/**
 * igt_sriov_clear_exit_handler - Disable SR-IOV exit cleanup context
 *
 * Clears the active SR-IOV cleanup context, used when a test has completed
 * explicit teardown and no longer needs best-effort cleanup on process exit.
 */
void igt_sriov_clear_exit_handler(void)
{
	sriov_exit_handler_release_fd();
	sriov_exit_ctx.autoprobe = false;
	sriov_exit_ctx.cleanup_fn = NULL;
	sriov_exit_ctx.cleanup_data = NULL;
}
