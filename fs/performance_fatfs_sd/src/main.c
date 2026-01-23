/*
 * Copyright (c) 2024 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ameba_soc.h"

#include <stdio.h>
#include <zephyr/logging/log.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>  // for k_msleep()
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

/* 测试配置 */
#define SYNC_AFTER_WRITE (1)
#define USE_SYSRAND (0)

// #define PRINT_SINGLE_WRITE_TIME (1)          // 打印每次 fs_write 的耗时
#define TEST_BLOCK_SIZE_MAX     (32*1024)       /* 测试块最大大小 */
#define TEST_FILE_NAME      "/SD:/test.dat"
#define TEST_ITERATIONS     (5)

#define CHECK_READ_DATA (0) //  是否检查读出数据的有效性
#define RW_DATA_PATTREN_BASE (0xA5)
static unsigned char grw_data_pattern = 0x50;

struct fs_test_config {
    uint32_t file_size_bytes;   // total file size
    uint32_t block_size_bytes;  // read/write size each time
    bool random_access;
    uint32_t rows;  // random matrix row
    uint32_t cols;  // random matrix columns

    uint32_t avg_write_speed;
    uint32_t avg_read_speed;
};

/* 性能统计结构体 */
struct perf_stats {
    struct fs_test_config *config;
    uint64_t write_time_ms;
    uint32_t write_time_us;
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
    // {8*1024*1024, 32*1024, 0},
    // {8*1024*1024, 32*1024, 1},

#if 0
    // 测试局部性
    // {256*1024,  128,     0},
    // {256*1024,  256,     0},
    // {256*1024,  512,     0},
    // {256*1024,  1*1024,  0},
    // {256*1024,  2*1024,  0},
    // {256*1024,  4*1024,  0},
    // {256*1024,  8*1024,  0},
    // {256*1024,  16*1024, 0},
    {256*1024,  32*1024, 0},

    /* rand access */
    {256*1024,  512,     1},
    {256*1024,  128,     1},
    // {256*1024,  256,     1},
    // {256*1024,  1*1024,  1},
    // {256*1024,  2*1024,  1},
    // {256*1024,  4*1024,  1},
    // {256*1024,  8*1024,  1},
    // {256*1024,  16*1024, 1},
    // {256*1024,  32*1024, 1},
#endif

#if 0
    {4*1024*1024,  4*1024,  0},
    {4*1024*1024,  8*1024,  0},
    {4*1024*1024,  16*1024, 0},
    {4*1024*1024,  32*1024, 0},

    /* rand access */
    {4*1024*1024,  4*1024,  1},
    {4*1024*1024,  8*1024,  1},
    {4*1024*1024,  16*1024, 1},
    {4*1024*1024,  32*1024, 1},
#endif

#if 0
    {16*1024*1024, 4*1024,  0},
    {16*1024*1024, 8*1024,  0},
    {16*1024*1024, 16*1024, 0},
    {16*1024*1024, 32*1024, 0},

    {16*1024*1024, 4*1024,  1},
    {16*1024*1024, 8*1024,  1},
    {16*1024*1024, 16*1024, 1},
    {16*1024*1024, 32*1024, 1},
#endif

#if 1
// 通用测试
    {8*1024*1024,  128,     0},
    {8*1024*1024,  256,     0},
    {8*1024*1024,  512,     0},
    {8*1024*1024,  1*1024,  0},
    {8*1024*1024,  2*1024,  0},
    {8*1024*1024,  4*1024,  0},
    {8*1024*1024,  8*1024,  0},
    {8*1024*1024,  16*1024, 0},
    {8*1024*1024,  32*1024, 0},

    {8*1024*1024,  128,     1},
    {8*1024*1024,  256,     1},
    {8*1024*1024,  512,     1},
    {8*1024*1024,  1*1024,  1},
    {8*1024*1024,  2*1024,  1},
    {8*1024*1024,  4*1024,  1},
    {8*1024*1024,  8*1024,  1},
    {8*1024*1024,  16*1024, 1},
    {8*1024*1024,  32*1024, 1},
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
#define RANDOM_COL_RANGE (1024)
#define RANDOM_ROW_RANGE (64)

// 全局变量：随机排列
uint8_t randrows[RANDOM_ROW_RANGE];  // 行的随机排列
uint16_t randcols[RANDOM_COL_RANGE];  // 列的随机排列

// 初始化随机排列
void RandomPermutationsInitialize(int M, int N) {
    if (M > RANDOM_ROW_RANGE || N > RANDOM_COL_RANGE) {
        printf("error: random row %d > %d, col %d > %d\n",
            M, RANDOM_ROW_RANGE, N, RANDOM_COL_RANGE);
        return;
    }

    // 初始化randrows数组为0到M-1
    for (int i = 0; i < M; i++) {
        randrows[i] = i;
    }
    
    // 初始化randcols数组为0到N-1
    for (int j = 0; j < N; j++) {
        randcols[j] = j;
    }
    
    // 打乱randrows数组 (Fisher-Yates洗牌算法)
    for (int i = M - 1; i > 0; i--) {
        int j = sys_rand32_get() % (i + 1);
        // 交换randrows[i]和randrows[j]
        int temp = randrows[i];
        randrows[i] = randrows[j];
        randrows[j] = temp;
    }
    
    // 打乱randcols数组 (Fisher-Yates洗牌算法)
    for (int i = N - 1; i > 0; i--) {
        int j = sys_rand32_get() % (i + 1);
        // 交换randcols[i]和randcols[j]
        int temp = randcols[i];
        randcols[i] = randcols[j];
        randcols[j] = temp;
    }

#if 0
    // 打印随机排列
    printf("rand rows [%d]: ", M);
    for (int i = 0; i < M; i++) {
        printf("%d ", randrows[i]);
    }
    printf("\n");
    
    printf("rand columns [%d]: ", N);
    for (int j = 0; j < N; j++) {
        printf("%d ", randcols[j]);
    }
    printf("\n\n");
#endif

    DCache_Clean((uint32_t)randrows, sizeof(randrows[0])*M);
    DCache_Clean((uint32_t)randcols, sizeof(randcols[0])*N);
}

static uint32_t RandomPermutationsGet(uint32_t row, uint32_t col, uint32_t columns) {
    uint32_t value = randrows[row] * columns + randcols[col];
    return value;
}

/******************************************************************/
static uint32_t last_offset = 0;
uint32_t get_random(uint32_t file_size, uint32_t block_size) {
    uint32_t offset;
    do {
#if 1
    // 重复率高
        offset = sys_rand32_get() % (file_size/block_size) * block_size;
#endif

#if 0
    // 重复率高
        offset = sys_rand32_get() % file_size;
        offset = offset / block_size *block_size;
#endif

#if 0
        // 重复率高
        offset = _rand() % file_size;
        offset = offset / block_size *block_size;
#endif
    } while(offset == last_offset);

    last_offset = offset;
    return offset;
}

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

    DCache_Clean((uint32_t)buffer, size);
}

/* 测试顺序写入 */
static int test_write(struct perf_stats *stat)
{
    struct fs_file_t file;
    int rc;
    int64_t start_time, end_time;
    uint32_t start_time_us, end_time_us;
    uint32_t block_size = stat->config->block_size_bytes;  // buffer_size
    uint32_t file_size = stat->config->file_size_bytes;
    uint32_t chunk_size;    // chunk size read or write each time

    /* 打开文件用于写入 */
    fs_file_t_init(&file);
    rc = fs_open(&file, TEST_FILE_NAME, FS_O_CREATE | FS_O_WRITE);
    if (rc < 0) {
        printk("Failed to open file for writing: %d\n", rc);
        return rc;
    }
    
    /* 生成测试数据 */
    generate_test_data(buffer, block_size, grw_data_pattern);
    
    /* 开始计时 */
    start_time = k_uptime_get();
    start_time_us = DTimestamp_Get();

    /* 写入 */
    size_t total_written = 0;
    uint32_t offset;
    int row_start = 0;
    int row = 0;
    int col  = 0;
    DiagPrintf("\n");
    while (total_written < file_size) {
        if (stat->config->random_access) {
#if USE_SYSRAND
            offset = get_random(file_size, block_size);
#else
            offset = RandomPermutationsGet(row, col, stat->config->cols);
            offset = offset * block_size;
#endif
            // DiagPrintf("w [%d][%d] %u\n", row, col, offset);
            rc = fs_seek(&file, offset, FS_SEEK_SET);
            if (rc < 0) {
                printk("Seek failed: %d, offset %d\n", rc, offset);
                break;
            }
        }

#if PRINT_SINGLE_WRITE_TIME
        uint32_t t1 = DTimestamp_Get();
#endif

        chunk_size = (file_size - total_written < block_size) ? file_size - total_written : block_size;
        rc = fs_write(&file, buffer, chunk_size);
        if (rc < 0 ||  rc != chunk_size) {
            printk("Write failed: %d\n", rc);
            fs_close(&file);
            return rc;
        }
#if PRINT_SINGLE_WRITE_TIME
        uint32_t t2 = DTimestamp_Get();
        DiagPrintf("write %d us\n", t2-t1);
#endif

        total_written += rc;
        stat->write_operations_completed++;

        if (stat->config->random_access) {
            row = (row + 1)% stat->config->rows;
            col++;
            if (col == stat->config->cols) {
                row_start++;
                row = row_start;
                col = 0;
            }
        }
        // DiagPrintf("%d, %d. %d\n", chunk_size, total_written, stat->write_operations_completed);

#if CHECK_READ_DATA
        if (stat->write_operations_completed == 1) {
            printk("%u, %u, %u, %u\n", buffer[0], buffer[4], buffer[8], buffer[12]);
        }
#endif
    }

    #if SYNC_AFTER_WRITE
        fs_sync(&file);
    #endif

    /* 结束计时 */
    end_time = k_uptime_get();
    end_time_us = DTimestamp_Get();
    
    /* 计算性能 */
    stat->write_time_us = end_time_us - start_time_us;
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
static int test_read(struct perf_stats *stat)
{
    struct fs_file_t file;
    int rc;
    uint32_t offset;
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
    generate_test_data(expected_buffer, block_size, grw_data_pattern);
#endif

    /* 开始计时 */
    start_time = k_uptime_get();
    
    /* 读取并验证 */
    int row_start = 0;
    int row = 0;
    int col  = 0;
    size_t total_read = 0;
    while (total_read < file_size) {
        if (stat->config->random_access) {
#if USE_SYSRAND
            offset = get_random(file_size, block_size);
#else
            offset = RandomPermutationsGet(row, col, stat->config->cols);
            offset = offset * block_size;
#endif
            // DiagPrintf("r [%d] [%d] %u\n", row, col, offset);
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
        stat->read_operations_completed++;

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
        if (stat->read_operations_completed == 1) {
            printk("%u, %u, %u, %u\n", buffer[0], buffer[4], buffer[8], buffer[12]);
        }

        /* 验证数据完整性 */
        if (memcmp(buffer, expected_buffer, rc) != 0) {
            printk("ERROR: Data verification failed at offset %zu\n", total_read);
            fs_close(&file);
            return -1;
        }
#endif
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
        DiagPrintf("[%d] Write: Operations %u, %llu ms, %u us, %u KB/s. \n",
            i, stat->write_operations_completed, stat->write_time_ms, stat->write_time_us, stat->write_speed_kbps);
        DiagPrintf("[%d] Read:  Operations %u, %llu ms, %u KB/s\n",
            i, stat->read_operations_completed, stat->read_time_ms, stat->read_speed_kbps);
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
    LOG_INF("wr buffer  %p\n", buffer);

    int config_nums = (sizeof(configs) / sizeof(configs[0]));
    for (int c = 0; c < config_nums; c++) {
        struct fs_test_config *config = &configs[c];
        if (config->block_size_bytes > TEST_BLOCK_SIZE_MAX) {
            printk("ERROR: block_size %d exceeds %d\n", config->block_size_bytes, TEST_BLOCK_SIZE_MAX);
            continue;
        }
        printk("\n\n[%d:%d] file_size %d bytes, block_size %d bytes, random access %d\n", 
            c, config_nums,
            config->file_size_bytes, config->block_size_bytes, config->random_access);

        /* 预生成 随机序列 */
        int blocks = config->file_size_bytes/config->block_size_bytes;
        if (blocks > RANDOM_COL_RANGE) {
            config->cols = RANDOM_COL_RANGE;
            config->rows = blocks/RANDOM_COL_RANGE;
        } else {
            config->rows = 1;
            config->cols = blocks;
        }
        if (config->rows * config->cols != blocks || config->rows > RANDOM_ROW_RANGE) {
            printk("ERROR: rows %d, cols %d, blocks %d\n", config->rows, config->cols, blocks);
            continue;
        }

        RandomPermutationsInitialize(config->rows, config->cols);

        memset(stats, 0, sizeof(stats[0]) * TEST_ITERATIONS);

        for (int i = 0; i < TEST_ITERATIONS; i++) {
            grw_data_pattern = RW_DATA_PATTREN_BASE + i*config_nums + c;
            struct perf_stats *stat = &stats[i];
            stat->config = config;

            /* 测试1: 顺序写入 */
            printk("Test 1: write test... [%d]\n", grw_data_pattern);
            rc = test_write(stat);
            if (rc != 0) {
                printk("write test failed: %d\n", rc);
                return rc;
            }

#if 1
            /* 测试2: 顺序读取 */
            printk("Test 2: read test...\n");
            rc = test_read(stat);
            if (rc != 0) {
                printk("read test failed: %d\n", rc);
                return rc;
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
