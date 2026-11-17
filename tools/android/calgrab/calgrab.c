// SPDX-License-Identifier: GPL-2.0
/*
 * calgrab.c (v1.0) - READ-ONLY: expose the camera module calibration that Samsung's fimc-is driver already read
 * from the EEPROM (its cal buffer in kernel RAM) as /sys/kernel/debug/calgrab/rom<id>, for the ISP emulator
 * (exynos-woa-s10 notes/s10-isp.md, 2026-11-17). Touches no hardware, writes nothing: it looks up the driver's own
 * getter fimc_is_sec_get_cal_buf(char **buf, int rom_id) (vendor/mcd/fimc-is-sec-define.c:74) and wraps the buffer
 * in a debugfs blob. Built against the exact kernel it is loaded into (ExtremeKernel ispdbg2 tree).
 *
 *   insmod calgrab.ko rom_id=1 size=16384      (front: rom_id 1, cal_size 16384 per the phone's dmesg)
 *   cat /sys/kernel/debug/calgrab/rom1 > /sdcard/s10cam/rom1_cal.bin
 *   rmmod calgrab
 */
#include <linux/module.h>
#include <linux/kallsyms.h>
#include <linux/debugfs.h>

static int rom_id = 1;
module_param(rom_id, int, 0444);
static uint size = 16384;
module_param(size, uint, 0444);

static struct dentry *dir;
static struct debugfs_blob_wrapper blob;

static int __init calgrab_init(void)
{
	int (*get_cal_buf)(char **buf, int rom_id);
	char *buf = NULL;
	char name[16];

	get_cal_buf = (void *)kallsyms_lookup_name("fimc_is_sec_get_cal_buf");
	if (!get_cal_buf) {
		pr_err("calgrab: fimc_is_sec_get_cal_buf not found\n");
		return -ENOENT;
	}
	get_cal_buf(&buf, rom_id);
	if (!buf) {
		pr_err("calgrab: no cal buffer for rom_id %d\n", rom_id);
		return -ENODATA;
	}
	blob.data = buf;
	blob.size = size;
	dir = debugfs_create_dir("calgrab", NULL);
	if (IS_ERR_OR_NULL(dir))
		return -ENODEV;
	snprintf(name, sizeof(name), "rom%d", rom_id);
	debugfs_create_blob(name, 0400, dir, &blob);
	pr_info("calgrab: rom_id %d buffer %p size %u -> /sys/kernel/debug/calgrab/%s\n", rom_id, buf, size, name);
	return 0;
}

static void __exit calgrab_exit(void)
{
	debugfs_remove_recursive(dir);
}

module_init(calgrab_init);
module_exit(calgrab_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("read-only view of the fimc-is camera calibration buffer");
