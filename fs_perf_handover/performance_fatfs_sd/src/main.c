/*
 * Copyright (c) 2024 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ameba_soc.h"

#include <stdio.h>
#include <zephyr/logging/log.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/fs/fs_sys.h>
#include <zephyr/sys/printk.h>
#include <string.h>
#include <stdlib.h>

#include <ff.h>
#include "diag.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(fatfs_sd);

#if defined(CONFIG_DISK_DRIVER_SDMMC)
#define DISK_NAME "SD"
#else
#error "Failed to select DISK access type"
#endif

#define FATFS_MNTP	"/"DISK_NAME":"
#define TEST_FILE	FATFS_MNTP"/testfile.txt"

#define TEST_FILE_SIZE  (8*1024*1024)    //8 MB
#define TEST_BLOCK_SIZE (32*1024)       /* fs_read|write 单次读|写的大小 */
#define TEST_FILE_NAME  "/SD:/test.dat"
#define TEST_ITERATIONS (5)

#define CHECK_READ_DATA (0) //  是否检查读出数据的有效性
#define RW_DATA_PATTREN_BASE (0xA5)
static unsigned char grw_data_pattern = RW_DATA_PATTREN_BASE;

struct fs_test_config {
    bool random_access;
    uint32_t rows;  // random matrix rows
    uint32_t cols;  // random matrix columns
};

struct perf_stats {
    struct fs_test_config *config;
    uint64_t write_time_ms;
    uint64_t read_time_ms;
    uint32_t write_speed_kbps;
    uint32_t read_speed_kbps;
};

__attribute__((aligned(32))) 
static uint8_t buffer[TEST_BLOCK_SIZE];

#if CHECK_READ_DATA
__attribute__((aligned(32))) 
static uint8_t expected_buffer[TEST_BLOCK_SIZE];
#endif

static struct fs_test_config gtst_config;
static struct perf_stats stats[TEST_ITERATIONS];

/* FatFs work area */
static FATFS fat_fs;

/* mounting info */
static struct fs_mount_t fatfs_mnt = {
	.type = FS_FATFS,
	.mnt_point = FATFS_MNTP,
	.fs_data = &fat_fs,
};

#define RANDOM_COL_RANGE (1024)
#define RANDOM_ROW_RANGE (64)

static uint8_t randrows[RANDOM_ROW_RANGE];
static uint16_t randcols[RANDOM_COL_RANGE];

// 初始化随机排列
void RandomPermutationsInitialize(int M, int N) {
    if (M > RANDOM_ROW_RANGE || N > RANDOM_COL_RANGE) {
        printf("error: random row %d > %d, col %d > %d\n",
            M, RANDOM_ROW_RANGE, N, RANDOM_COL_RANGE);
        return;
    }

    for (int i = 0; i < M; i++) {
        randrows[i] = i;
    }

    for (int j = 0; j < N; j++) {
        randcols[j] = j;
    }
    
    for (int i = M - 1; i > 0; i--) {
        int j = sys_rand32_get() % (i + 1);
        int temp = randrows[i];
        randrows[i] = randrows[j];
        randrows[j] = temp;
    }
    
    for (int i = N - 1; i > 0; i--) {
        int j = sys_rand32_get() % (i + 1);
        int temp = randcols[i];
        randcols[i] = randcols[j];
        randcols[j] = temp;
    }

    DCache_Clean((uint32_t)randrows, sizeof(randrows[0])*M);
    DCache_Clean((uint32_t)randcols, sizeof(randcols[0])*N);
}

static uint32_t RandomPermutationsGet(uint32_t row, uint32_t col, uint32_t columns) {
    uint32_t value = randrows[row] * columns + randcols[col];
    return value;
}

static void generate_test_data(uint8_t *buffer, size_t size, uint8_t pattern)
{
    memset(buffer, pattern&0xFF, size);
    DCache_Clean((uint32_t)buffer, size);
}

static int test_write(struct perf_stats *stat)
{
    struct fs_file_t file;
    int rc;
    int64_t start_time, end_time;
    uint32_t block_size = TEST_BLOCK_SIZE;
    uint32_t file_size = TEST_FILE_SIZE;
    uint32_t chunk_size;

    fs_file_t_init(&file);
    rc = fs_open(&file, TEST_FILE_NAME, FS_O_CREATE | FS_O_WRITE);
    if (rc < 0) {
        printk("Failed to open file for writing: %d\n", rc);
        return rc;
    }

    generate_test_data(buffer, block_size, grw_data_pattern);

    start_time = k_uptime_get();

    size_t total_written = 0;
    uint32_t offset;
    int row_start = 0;
    int row = 0;
    int col  = 0;
    while (total_written < file_size) {
        if (stat->config->random_access) {
            offset = RandomPermutationsGet(row, col, stat->config->cols);
            offset = offset * block_size;

            rc = fs_seek(&file, offset, FS_SEEK_SET);
            if (rc < 0) {
                printk("Seek failed: %d, offset %d\n", rc, offset);
                break;
            }
        }

        chunk_size = (file_size - total_written < block_size) ? file_size - total_written : block_size;
        rc = fs_write(&file, buffer, chunk_size);
        if (rc < 0 ||  rc != chunk_size) {
            printk("Write failed: %d\n", rc);
            fs_close(&file);
            return rc;
        }

        total_written += rc;

        if (stat->config->random_access) {
            row = (row + 1)% stat->config->rows;
            col++;
            if (col == stat->config->cols) {
                row_start++;
                row = row_start;
                col = 0;
            }
        }
    }

    fs_sync(&file);

    end_time = k_uptime_get();

    stat->write_time_ms = (end_time - start_time);
    stat->write_speed_kbps = (int)(((float)file_size / 1024 * 1000) / stat->write_time_ms);

    rc = fs_close(&file);
    if (rc != 0) {
        DiagPrintf("Error closing file: %d\n", rc);
    }

    return 0;
}

static int test_read(struct perf_stats *stat)
{
    struct fs_file_t file;
    int rc;
    uint32_t offset;
    int64_t start_time, end_time;
    uint32_t block_size = TEST_BLOCK_SIZE;
    uint32_t file_size = TEST_FILE_SIZE;
    uint32_t chunk_size;

    fs_file_t_init(&file);
    rc = fs_open(&file, TEST_FILE_NAME, FS_O_READ);
    if (rc < 0) {
        printk("Failed to open file for reading: %d\n", rc);
        return rc;
    }

#if CHECK_READ_DATA
    generate_test_data(expected_buffer, block_size, grw_data_pattern);
#endif

    start_time = k_uptime_get();

    int row_start = 0;
    int row = 0;
    int col  = 0;
    size_t total_read = 0;
    while (total_read < file_size) {
        if (stat->config->random_access) {
            offset = RandomPermutationsGet(row, col, stat->config->cols);
            offset = offset * block_size;

            rc = fs_seek(&file, offset, FS_SEEK_SET);
            if (rc < 0) {
                printk("Seek failed: %d, offset %d\n", rc, offset);
                break;
            }
        }

        chunk_size = (file_size - total_read < block_size) ? file_size - total_read : block_size;
        rc = fs_read(&file, buffer, chunk_size);
        if (rc < 0 || rc != chunk_size) {
            printk("Read failed: %d. expected chunk_size=%d, total_read=%d\n", rc, chunk_size, total_read);
            fs_close(&file);
            return rc;
        }

        total_read += rc;

        if (stat->config->random_access) {
            row = (row + 1)% stat->config->rows;
            col++;
            if (col == stat->config->cols) {
                row_start++;
                row = row_start;
                col = 0;
            }
        }

#if CHECK_READ_DATA
        if (memcmp(buffer, expected_buffer, rc) != 0) {
            printk("ERROR: Data verification failed at offset %zu\n", total_read);
            fs_close(&file);
            return -1;
        }
#endif
    }

    end_time = k_uptime_get();
    
    stat->read_time_ms = (end_time - start_time);
    stat->read_speed_kbps = (int)(((float)file_size / 1024 * 1000) / stat->read_time_ms);

    fs_close(&file);
    return 0;
}

int main(void)
{
    int rc;
    
    printk("Starting FATFS performance test...\n");

	rc = fs_mount(&fatfs_mnt);
	if (rc < 0) {
		LOG_INF("FAT file system mounting failed, [%d]\n", rc);
		return rc;
	} else {
		LOG_INF("FAT file system mounting successfully\n");
	}

    /* 清理旧测试文件 */
    fs_unlink(TEST_FILE_NAME);

    memset(&gtst_config,0,sizeof(struct fs_test_config));

    /* 预生成 随机序列 */
    int blocks = TEST_FILE_SIZE/TEST_BLOCK_SIZE;
    if (blocks > RANDOM_COL_RANGE) {
        gtst_config.cols = RANDOM_COL_RANGE;
        gtst_config.rows = blocks/RANDOM_COL_RANGE;
    } else {
        gtst_config.rows = 1;
        gtst_config.cols = blocks;
    }

    RandomPermutationsInitialize(gtst_config.rows, gtst_config.cols);

    for (int random = 0; random < 2; random++) {
        struct fs_test_config *config = &gtst_config;
        config->random_access = random;

        memset(stats, 0, sizeof(stats[0]) * TEST_ITERATIONS);

        for (int i = 0; i < TEST_ITERATIONS; i++) {
            grw_data_pattern = RW_DATA_PATTREN_BASE + i;
            struct perf_stats *stat = &stats[i];
            stat->config = config;

            DCache_CleanInvalidate(0xFFFFFFFF, 0xFFFFFFFF);
            rc = test_write(stat);
            if (rc != 0) {
                printk("write test failed: %d\n", rc);
                return rc;
            }

            DCache_CleanInvalidate(0xFFFFFFFF, 0xFFFFFFFF);
            rc = test_read(stat);
            if (rc != 0) {
                printk("read test failed: %d\n", rc);
                return rc;
            }

            DiagPrintf("[%d] Write: %llu ms, %u KB/s. \n", i, stat->write_time_ms, stat->write_speed_kbps);
            DiagPrintf("[%d] Read: %llu ms, %u KB/s\n", i, stat->read_time_ms, stat->read_speed_kbps);
        }

        /* 计算均值 */
        uint32_t total_read_speed = 0;
        uint32_t total_write_speed = 0;
        for (int i = 0; i < TEST_ITERATIONS; i++) {
            struct perf_stats *stat = &stats[i];
            total_read_speed += stat->read_speed_kbps;
            total_write_speed += stat->write_speed_kbps;
        }
        uint32_t avg_read_speed = total_read_speed / TEST_ITERATIONS;
        uint32_t avg_write_speed = total_write_speed / TEST_ITERATIONS;

        DiagPrintf("file_size %d bytes, block_size %d bytes, random access %d. Average read speed %d KB/s. Average write speed %d KB/s\n", 
            TEST_FILE_SIZE, TEST_BLOCK_SIZE, random, avg_read_speed, avg_write_speed);
    }

    /* 清理测试文件 */
    fs_unlink(TEST_FILE_NAME);
    
    printk("FATFS performance test completed!\n");

    rc = fs_unmount(&fatfs_mnt);
    if (rc < 0) {
        LOG_INF("Error unmount FAT file system [%d]\n", rc);
    } else {
        LOG_INF("unmount FAT file system successfully\n");
    }

    return 0;
}
