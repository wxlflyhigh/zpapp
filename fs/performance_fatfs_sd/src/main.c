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
#define TEST_ITERATIONS     (5)

#define CHECK_READ_DATA (0) //  是否检查读出数据的有效性
#define RW_DATA_PATTREN (0xA5)

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

static uint8_t buffer[TEST_BLOCK_SIZE_MAX];
#if CHECK_READ_DATA
static uint8_t expected_buffer[TEST_BLOCK_SIZE_MAX];
#endif

/* 全局统计 */
static struct perf_stats stats[TEST_ITERATIONS];

static struct fs_test_config configs[] = {
    // {8*1024*1024, 16*1024, 0},
    // {8*1024*1024, 16*1024, 1},
    // {8*1024*1024, 32*1024, 0},
    // {8*1024*1024, 32*1024, 1},

#if 1
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

#if 1
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
static int test_sequential_write(struct perf_stats *stat)
{
    struct fs_file_t file;
    int rc;
    int64_t start_time, end_time;
    uint32_t block_size = stat->config->block_size_bytes;  // buffer_size
    uint32_t file_size = stat->config->file_size_bytes;
    uint32_t chunk_size;    // chunk size read or write each time
    // DiagPrintf("block_size %d, file_size %d\n", block_size, file_size);

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
        if (stat->config->random_access) {
            uint32_t offset = sys_rand32_get() % (file_size/block_size) * block_size;
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
    } else {
        // DiagPrintf("close %s successfully\n", TEST_FILE_NAME);
    }
    return 0;
}

/* 测试顺序读取 */
static int test_sequential_read(struct perf_stats *stat)
{
    struct fs_file_t file;
    int rc;
    int64_t start_time, end_time;
    uint32_t block_size = stat->config->block_size_bytes;  // buffer_size
    uint32_t file_size = stat->config->file_size_bytes;
    uint32_t chunk_size;    // chunk size read or write each time
    
    /* 打开文件用于读取 */
    fs_file_t_init(&file);
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
        if (stat->config->random_access) {
            uint32_t offset = sys_rand32_get() % (file_size/block_size) * block_size;
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

#if 0
/* 测试随机访问 */
static int test_random_access(void)
{
    struct fs_file_t file;
    uint8_t buffer[512];
    int rc;
    int64_t start_time;
    uint32_t iterations = 1000;  /* 随机访问1000次 */
    
    fs_file_t_init(&file);
    rc = fs_open(&file, TEST_FILE_NAME, FS_O_RDWR);
    if (rc < 0) {
        printk("Failed to open file for random access: %d\n", rc);
        return rc;
    }
    
    start_time = k_uptime_get();
    
    for (uint32_t i = 0; i < iterations; i++) {
        /* 随机位置 */
        off_t offset = (i * 997) % (TEST_FILE_SIZE - sizeof(buffer));
        
        /* 定位 */
        rc = fs_seek(&file, offset, FS_SEEK_SET);
        if (rc < 0) {
            printk("Seek failed: %d\n", rc);
            break;
        }
        
        /* 随机读取 */
        rc = fs_read(&file, buffer, sizeof(buffer));
        if (rc < 0) {
            printk("Random read failed: %d\n", rc);
            break;
        }
        
        /* 随机写入 */
        rc = fs_seek(&file, offset, FS_SEEK_SET);
        if (rc < 0) {
            printk("Seek failed: %d\n", rc);
            break;
        }
        
        generate_test_data(buffer, sizeof(buffer), i & 0xFF);
        rc = fs_write(&file, buffer, sizeof(buffer));
        if (rc < 0) {
            printk("Random write failed: %d\n", rc);
            break;
        }
    }
    
    fs_close(&file);
    
    int64_t time_us = (k_uptime_get() - start_time) * 1000;
    printk("Random access: %d operations in %lld us, %lld us/op\n",
           iterations, time_us, time_us / iterations);
    
    return 0;
}

/* 测试小文件操作 */
static int test_small_files(void)
{
    char filename[32];
    struct fs_file_t file;
    uint8_t buffer[128];
    int rc;
    int64_t start_time;
    uint32_t file_count = 100;
    
    /* 创建多个小文件 */
    start_time = k_uptime_get();
    
    for (uint32_t i = 0; i < file_count; i++) {
        snprintf(filename, sizeof(filename), "/SD:/small_%04d.dat", i);
        
        fs_file_t_init(&file);
        rc = fs_open(&file, filename, FS_O_CREATE | FS_O_WRITE);
        if (rc < 0) {
            printk("Failed to create file %s: %d\n", filename, rc);
            continue;
        }
        
        generate_test_data(buffer, sizeof(buffer), i & 0xFF);
        rc = fs_write(&file, buffer, sizeof(buffer));
        fs_close(&file);
        
        if (rc < 0) {
            printk("Failed to write file %s: %d\n", filename, rc);
        }
    }
    
    int64_t create_time = (k_uptime_get() - start_time) * 1000;
    
    /* 读取小文件 */
    start_time = k_uptime_get();
    
    for (uint32_t i = 0; i < file_count; i++) {
        snprintf(filename, sizeof(filename), "/SD:/small_%04d.dat", i);
        
        fs_file_t_init(&file);
        rc = fs_open(&file, filename, FS_O_READ);
        if (rc < 0) {
            printk("Failed to open file %s: %d\n", filename, rc);
            continue;
        }
        
        rc = fs_read(&file, buffer, sizeof(buffer));
        fs_close(&file);
        
        if (rc < 0) {
            printk("Failed to read file %s: %d\n", filename, rc);
        }
    }
    
    int64_t read_time = (k_uptime_get() - start_time) * 1000;
    
    printk("Small files: Create %d files in %lld us, Read in %lld us\n",
           file_count, create_time, read_time);
    
    /* 清理小文件 */
    for (uint32_t i = 0; i < file_count; i++) {
        snprintf(filename, sizeof(filename), "/SD:/small_%04d.dat", i);
        fs_unlink(filename);
    }
    
    return 0;
}
#endif

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

            /* 测试1: 顺序写入 */
            printk("Test 1: Sequential write test...\n");
            rc = test_sequential_write(stat);
            if (rc != 0) {
                printk("Sequential write test failed: %d\n", rc);
                return rc;
            }

            /* 测试2: 顺序读取 */
            printk("Test 2: Sequential read test...\n");
            rc = test_sequential_read(stat);
            if (rc != 0) {
                printk("Sequential read test failed: %d\n", rc);
                return rc;
            }

    #if 0
            /* 测试3: 随机访问 */
            printk("Test 3: Random access test...\n");
            rc = test_random_access();
            if (rc != 0) {
                printk("Random access test failed: %d\n", rc);
            }
            
            /* 测试4: 小文件操作 */
            printk("Test 4: Small files test...\n");
            rc = test_small_files();
            if (rc != 0) {
                printk("Small files test failed: %d\n", rc);
            }
    #endif
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
