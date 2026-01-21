/*
 * Copyright (c) 2024 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <zephyr/logging/log.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>  // for k_msleep()
#include <zephyr/random/random.h>
#include <zephyr/fs/fs_sys.h>
#include <zephyr/sys/printk.h>
#include <string.h>

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

/* 测试配置 */
#define TEST_BLOCK_SIZE_MAX     (32*1024)           /* 测试块最大大小 */
#define TEST_FILE_NAME      "/SD:/test.dat"
#define TEST_ITERATIONS     (1)

#define CHECK_READ_DATA (0) //  是否检查读出数据的有效性
#define RW_DATA_PATTREN (0xA5)
#define ENABLE_RANDOM_RW (0)

struct fs_test_config {
    uint32_t file_size_bytes;   // total file size
    uint32_t block_size_bytes;  // read/write size each time
    bool random_access;

    uint32_t avg_write_speed;
    uint32_t avg_read_speed;
};

/* 性能统计结构体 */
struct perf_stats {
    struct fs_test_config *config;
    uint64_t write_time_ms;
    uint64_t read_time_ms;
    uint32_t write_speed_kbps;
    uint32_t read_speed_kbps;
    uint32_t write_operations_completed;
    uint32_t read_operations_completed;
};

__attribute__((aligned(32))) 
static uint8_t buffer[TEST_BLOCK_SIZE_MAX];
#if CHECK_READ_DATA
__attribute__((aligned(32))) 
static uint8_t expected_buffer[TEST_BLOCK_SIZE_MAX];
#endif

/* 全局统计 */
static struct perf_stats stats[TEST_ITERATIONS];

static struct fs_test_config configs[] = {
    {1*1024*1024, 4*1024, 0},
    // {8*1024*1024, 16*1024, 1},
    // {8*1024*1024, 32*1024, 0},
    // {8*1024*1024, 32*1024, 1},

#if 0
    {4*1024*1024,  4*1024,  0},
    {4*1024*1024,  8*1024,  0},
    {4*1024*1024,  16*1024, 0},
    {4*1024*1024,  32*1024, 0},
                   
    {8*1024*1024,  4*1024,  0},
    {8*1024*1024,  8*1024,  0},
    {8*1024*1024,  16*1024, 0},
    {8*1024*1024,  32*1024, 0},

    {16*1024*1024, 4*1024,  0},
    {16*1024*1024, 8*1024,  0},
    {16*1024*1024, 16*1024, 0},
    {16*1024*1024, 32*1024, 0},
    
    /* rand access */
    {4*1024*1024,  4*1024,  1},
    {4*1024*1024,  8*1024,  1},
    {4*1024*1024,  16*1024, 1},
    {4*1024*1024,  32*1024, 1},

    {8*1024*1024,  4*1024,  1},
    {8*1024*1024,  8*1024,  1},
    {8*1024*1024,  16*1024, 1},
    {8*1024*1024,  32*1024, 1},

    {16*1024*1024, 4*1024,  1},
    {16*1024*1024, 8*1024,  1},
    {16*1024*1024, 16*1024, 1},
    {16*1024*1024, 32*1024, 1},
#endif

#if 0
    {8*1024*1024,  128,     0},
    {8*1024*1024,  256,     0},
    {8*1024*1024,  512,     0},
    {8*1024*1024,  1*1024,  0},
    {8*1024*1024,  2*1024,  0},

    {8*1024*1024,  128,     1},
    {8*1024*1024,  256,     1},
    {8*1024*1024,  512,     1},
    {8*1024*1024,  1*1024,  1},
    {8*1024*1024,  2*1024,  1},
#endif
};

/* FatFs work area */
static FATFS fat_fs;

/* mounting info */
static struct fs_mount_t fatfs_mnt = {
	.type = FS_FATFS,
	.mnt_point = FATFS_MNTP,
	.fs_data = &fat_fs,
};

/******************************************************************/
/* 生成测试数据 */
static void generate_test_data(uint8_t *buffer, size_t size, uint8_t pattern)
{
#if CHECK_READ_DATA
    for (size_t i = 0; i < size; i++) {
        buffer[i] = (pattern + i) & 0xFF;
    }
#else
    memset(buffer, pattern&0xFF, size);
#endif
}

/* 测试顺序写入 */
static int test_sequential_write_and_read(struct perf_stats *stat)
{
    struct fs_file_t file;
    int rc;
    int64_t start_time, end_time;
    uint32_t block_size = stat->config->block_size_bytes;  // buffer_size
    uint32_t file_size = stat->config->file_size_bytes;
    uint32_t chunk_size;    // chunk size read or write each time

    printf("read&write buffer %p\n", buffer);

    /* 打开文件用于写入 */
    fs_file_t_init(&file);

    rc = fs_open(&file, TEST_FILE_NAME, FS_O_CREATE | FS_O_WRITE);
    if (rc < 0) {
        printk("Failed to open file for writing: %d\n", rc);
        return rc;
    }
    
    /* 生成测试数据 */
    generate_test_data(buffer, block_size, RW_DATA_PATTREN);
    
    /* 开始计时 */
    start_time = k_uptime_get();
    
    /* 连续写入 */
    size_t total_written = 0;
    while (total_written < file_size) {
#if ENABLE_RANDOM_RW
        if (stat->config->random_access) {
            uint32_t offset = sys_rand32_get() % (file_size/block_size) * block_size;
            rc = fs_seek(&file, offset, FS_SEEK_SET);
            if (rc < 0) {
                printk("Seek failed: %d, offset %d\n", rc, offset);
                break;
            }
        }
#endif

        chunk_size = (file_size - total_written < block_size) ? file_size - total_written : block_size;
        rc = fs_write(&file, buffer, chunk_size);
        if (rc < 0 ||  rc != chunk_size) {
            printk("Write failed: %d\n", rc);
            fs_close(&file);
            return rc;
        }
        total_written += rc;
        stat->write_operations_completed++;
        // DiagPrintf("%d, %d. %d\n", chunk_size, total_written, stat->write_operations_completed);
    }
    
    /* 结束计时 */
    end_time = k_uptime_get();
    
    /* 计算性能 */
    stat->write_time_ms = (end_time - start_time);
    stat->write_speed_kbps = (int)(((float)file_size / 1024 * 1000) / stat->write_time_ms);

    rc = fs_close(&file);
    if (rc != 0) {
        DiagPrintf("Error closing file: %d\n", rc);
    } 

    /***********************************************************************/
    rc = fs_open(&file, TEST_FILE_NAME, FS_O_READ);
    if (rc < 0) {
        printk("Failed to open file for reading: %d\n", rc);
        return rc;
    }

#if CHECK_READ_DATA
    /* 生成预期数据 */
    generate_test_data(expected_buffer, block_size, RW_DATA_PATTREN);
#endif

    /* 开始计时 */
    start_time = k_uptime_get();
    
    /* 连续读取并验证 */
    size_t total_read = 0;
    while (total_read < file_size) {
#if ENABLE_RANDOM_RW
        if (stat->config->random_access) {
            uint32_t offset = sys_rand32_get() % (file_size/block_size) * block_size;
            rc = fs_seek(&file, offset, FS_SEEK_SET);
            if (rc < 0) {
                printk("Seek failed: %d, offset %d\n", rc, offset);
                break;
            }
        }
#endif

        chunk_size = (file_size - total_read < block_size) ? file_size - total_read : block_size;
        rc = fs_read(&file, buffer, chunk_size);
        if (rc < 0 || rc != chunk_size) {
            printk("Read failed: %d. expected chunk_size=%d, total_read=%d\n", rc, chunk_size, total_read);
            fs_close(&file);
            return rc;
        }

#if CHECK_READ_DATA
        /* 验证数据完整性 */
        if (memcmp(buffer, expected_buffer, rc) != 0) {
            printk("ERROR: Data verification failed at offset %zu\n", total_read);
            fs_close(&file);
            return -1;
        }
#endif

        total_read += rc;
        stat->read_operations_completed++;
    }
    
    /* 结束计时 */
    end_time = k_uptime_get();
    
    /* 计算性能 */
    stat->read_time_ms = (end_time - start_time);
    stat->read_speed_kbps = (int)(((float)file_size / 1024 * 1000) / stat->read_time_ms);
    
    fs_close(&file);
    return 0;
}

/* 显示性能结果 */
static void display_performance_results(struct fs_test_config *config, struct perf_stats *stats)
{
    DiagPrintf("\n====== FATFS Performance Results ======\n");
    DiagPrintf("file_size %d bytes, block_size %d bytes, random access %d. Average read speed %d KB/s. Average write speed %d KB/s\n", 
        config->file_size_bytes, config->block_size_bytes, config->random_access, 
        config->avg_read_speed, config->avg_write_speed);

    for (int i = 0; i < TEST_ITERATIONS; i++) {
        struct perf_stats *stat = &stats[i];
        DiagPrintf("[%d] Sequential Write: %llu ms, %u KB/s\n", 
            i, stat->write_time_ms, stat->write_speed_kbps);
        DiagPrintf("[%d] Sequential Read:  %llu ms, %u KB/s\n", 
            i, stat->read_time_ms, stat->read_speed_kbps);
        DiagPrintf("[%d] Completed Write Operations %u, Read Operations %u\n",
            i, stat->write_operations_completed, stat->read_operations_completed);
    }
    DiagPrintf("======================================\n");
}

/* 主测试函数 */
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

    int config_nums = (sizeof(configs) / sizeof(configs[0]));
    for (int c = 0; c < config_nums; c++) {
        struct fs_test_config *config = &configs[c];
        if (config->block_size_bytes > TEST_BLOCK_SIZE_MAX) {
            printk("ERROR: block_size %d exceeds %d\n", config->block_size_bytes, TEST_BLOCK_SIZE_MAX);
            continue;
        }
        printk("[%d:%d] file_size %d bytes, block_size %d bytes, random access %d\n", 
            c, config_nums,
            config->file_size_bytes, config->block_size_bytes, config->random_access);

        memset(stats, 0, sizeof(stats[0]) * TEST_ITERATIONS);
        for (int i = 0; i < TEST_ITERATIONS; i++) {
            struct perf_stats *stat = &stats[i];
            stat->config = config;

            /* 测试1: 顺序读写 */
            printk("Test 1: Sequential write test...\n");
            rc = test_sequential_write_and_read(stat);
            if (rc != 0) {
                printk("Sequential write test failed: %d\n", rc);
                return rc;
            }
        }

        /* 计算均值 */
        uint32_t total_read_speed = 0;
        uint32_t total_write_speed = 0;
        for (int i = 0; i < TEST_ITERATIONS; i++) {
            struct perf_stats *stat = &stats[i];
            total_read_speed += stat->read_speed_kbps;
            total_write_speed += stat->write_speed_kbps;
        }
        config->avg_read_speed = total_read_speed / TEST_ITERATIONS;
        config->avg_write_speed = total_write_speed / TEST_ITERATIONS;

        /* 显示结果 */
        display_performance_results(config, stats);

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
