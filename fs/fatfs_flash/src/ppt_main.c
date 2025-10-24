/*
 * Copyright (c) 2024 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <zephyr/logging/log.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>  // for k_msleep()

#include <ff.h>

#define DISK_NAME DT_PROP(DT_NODELABEL(test_disk), disk_name)

#define FATFS_MNTP	"/"DISK_NAME":"
#define TEST_FILE	FATFS_MNTP"/testfile.txt"
#define TEST_DIR	FATFS_MNTP"/testdir"
#define TEST_DIR_FILE	FATFS_MNTP"/testdir/testfile2.txt"

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(fs, CONFIG_FS_LOG_LEVEL);

/* FatFs work area */
static FATFS fat_fs;

/* mounting info */
static struct fs_mount_t fatfs_mnt = {
	.type = FS_FATFS,
	.mnt_point = FATFS_MNTP,
	.fs_data = &fat_fs,
};

int main(void)
{
	k_msleep(1000);
	LOG_INF("run fatfs example\n");

	int ret;
	struct fs_file_t file;
	char read_buff[128];
	ssize_t brw;
	const char test_str[] = "Hello, FatFS on Flash!";
	size_t sz = strlen(test_str);

	ret = fs_mount(&fatfs_mnt);
	fs_file_t_init(&file);
	ret = fs_open(&file, TEST_FILE, FS_O_CREATE | FS_O_RDWR);

	// read previous write data, make sure they're read from flash
	(void)fs_seek(&file, 0, FS_SEEK_SET);
	memset(read_buff, 0, 128);
	brw = fs_read(&file, read_buff, sz);	
	read_buff[brw] = 0;

	if (!strcmp(test_str, read_buff)) {
		LOG_INF("Data read successfully. [%s]. len:%d\n", read_buff, brw);
	} else {
		LOG_ERR("Data read:[%s]. len:%d\n", read_buff, brw);
	}

	// write
	ret = fs_write(&file, test_str, strlen(test_str));
	LOG_INF("write date [%s] to file %s successfully. len:%d\n", test_str, TEST_FILE, ret);

#if 0
	// read
	(void)fs_seek(&file, 0, FS_SEEK_SET);
	brw = fs_read(&file, read_buff, sz);

	read_buff[brw] = 0;

	if (!strcmp(test_str, read_buff)) {
		LOG_INF("Data read successfully. [%s]\n", read_buff);
	}
#endif

err_close:
	ret = fs_close(&file);

err_unmount:
	ret = fs_unmount(&fatfs_mnt);


err_ret:
	return 1;
}

