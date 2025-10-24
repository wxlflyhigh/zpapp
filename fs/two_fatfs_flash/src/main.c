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
// #include "os_wrapper.h"

#if 1
#if defined(CONFIG_DISK_DRIVER_FLASH)
#define DISK_NAME DT_PROP(DT_NODELABEL(test_disk), disk_name)
#define DISK_NAME2 DT_PROP(DT_NODELABEL(test_dtwo), disk_name)
#else
#error "Failed to select DISK access type"
#endif
#else
#define DISK_NAME "NAND"
#endif

#define FATFS_MNTP	"/"DISK_NAME":"
#define TEST_FILE	FATFS_MNTP"/testfile.txt"
// #define TEST_DIR	FATFS_MNTP"/testdir"
// #define TEST_DIR_FILE	FATFS_MNTP"/testdir/test2.txt"

#define FATFS_MNTP2	"/"DISK_NAME2":"
#define TEST_FILE2	FATFS_MNTP2"/test2.txt"

#include <zephyr/logging/log.h>
// LOG_MODULE_DECLARE(fs, CONFIG_FS_LOG_LEVEL);
LOG_MODULE_REGISTER(fatmain);

/* FatFs work area */
static FATFS fat_fs;

/* mounting info */
static struct fs_mount_t fatfs_mnt = {
	.type = FS_FATFS,
	.mnt_point = FATFS_MNTP,
	.fs_data = &fat_fs,
};

static struct fs_mount_t fatfs_mnt2 = {
	.type = FS_FATFS,
	.mnt_point = FATFS_MNTP2,
	.fs_data = &fat_fs,
};

void fatfs_test(struct fs_mount_t* pfatfs_mnt, char* test_file, char* test_str)
{
	int ret;
	struct fs_file_t file;
	char read_buff[128];
	ssize_t brw;
	// const char test_str[] = "Hello, FatFS on Flash!";
	size_t sz = strlen(test_str);

	LOG_INF("start to mount fatfs");
	ret = fs_mount(pfatfs_mnt);
	if (ret < 0) {
		LOG_INF("FAT file system mounting failed, [%d]\n", ret);
		goto err_ret;
	} else {
		LOG_INF("FAT file system mounting successfully\n");
	}

	LOG_INF("start to fs_file_t_init");
	fs_file_t_init(&file);
	LOG_INF("start to fs_open");
	ret = fs_open(&file, test_file, FS_O_CREATE | FS_O_RDWR);
	if (ret != 0) {
		LOG_INF("Error opening file: %s, (%d)\n", test_file, ret);
		goto err_unmount;
	} else {
		LOG_INF("open [%s] successfully\n", test_file);
	}

	// write
	LOG_INF("start to fs_write");
	ret = fs_write(&file, test_str, strlen(test_str));
	if (ret < 0) {
		LOG_INF("Error writing to file: %d\n", ret);
	}

	if (ret < strlen(test_str)) {
		LOG_INF("Unable to complete write. Volume full.\n");
	}
	LOG_INF("write date [%s] to file %s successfully. len:%d\n", test_str, test_file, ret);

	// read
	(void)fs_seek(&file, 0, FS_SEEK_SET);
	brw = fs_read(&file, read_buff, sz);
	if (brw < 0) {
		LOG_INF("Failed reading file [%zd]\n", brw);
		fs_close(&file);
		goto err_close;
	}

	read_buff[brw] = 0;

	if (strcmp(test_str, read_buff)) {
		LOG_INF("Error - Data read does not match data written\n");
		LOG_INF("Data read:\"%s\"\n\n", read_buff);
	} else {
		LOG_INF("Data read successfully. [%s]\n", read_buff);
	}

err_close:
	ret = fs_close(&file);
	if (ret != 0) {
		LOG_INF("Error closing file: %d\n", ret);
	} else {
		LOG_INF("close %s successfully\n", test_file);
	}

err_unmount:
#if 1
	ret = fs_unmount(pfatfs_mnt);
	if (ret < 0) {
		LOG_INF("Error unmount FAT file system [%d]\n", ret);
	} else {
		LOG_INF("unmount FAT file system successfully\n");
	}
#endif

err_ret:
	return 1;
}


int main(void)
{
	k_msleep(1000);
	LOG_INF("run fatfs example\n");

	const char test_str[] = "Hello, FatFS on Flash!";
	const char test_str2[] = "Good, FatFS2 on Flash!";

	fatfs_test(&fatfs_mnt2, TEST_FILE2, test_str2);
	fatfs_test(&fatfs_mnt, TEST_FILE, test_str);

	return 1;
}