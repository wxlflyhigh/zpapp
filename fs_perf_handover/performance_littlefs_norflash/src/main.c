/*
 * LittleFS on NOR Flash 性能测试代码
 */
#include "ameba_soc.h"

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/printk.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/time_units.h>
#include <string.h>
#include <stdlib.h>

/* 测试配置 */
#define TEST_FILE_SIZE      (16*1024)
#define TEST_BLOCK_SIZE     (4*1024)           /* fs_read|write API 单次读|写的大小 */
#define TEST_FILE_NAME      "/lfs1/test.bin"
#define TEST_ITERATIONS     (10)

#define CHECK_READ_DATA (0)
#define RW_DATA_PATTREN_BASE (0xA5)
static unsigned char grw_data_pattern = RW_DATA_PATTREN_BASE;

struct fs_test_config {
    bool random_access;
    uint32_t rows;  // random matrix rows
    uint32_t cols;  // random matrix columns
};

struct perf_stats {
    struct fs_test_config *config;
    uint64_t read_time_us;
    uint64_t write_time_us;
    uint32_t write_speed_kbps;
    uint32_t read_speed_kbps;
};

static uint8_t buffer[TEST_BLOCK_SIZE] __attribute__((aligned(4096)));

#if CHECK_READ_DATA
static uint8_t expected_buffer[TEST_BLOCK_SIZE]  __attribute__((aligned(4096)));
#endif

static struct perf_stats stats[TEST_ITERATIONS];

/******************************************************************/
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
    int rc;
    struct fs_file_t file;
    int64_t start_time, end_time;

    uint32_t block_size = TEST_BLOCK_SIZE;
    uint32_t file_size = TEST_FILE_SIZE;
    uint32_t chunk_size = -1;

    fs_file_t_init(&file);
    rc = fs_open(&file, TEST_FILE_NAME, FS_O_CREATE | FS_O_WRITE);
    if (rc < 0) {
        printk("Failed to open file for writing: %d\n", rc);
        return rc;
    }
    
    generate_test_data(buffer, block_size, grw_data_pattern);
    
    start_time   = DTimestamp_Get();

    size_t total_written = 0;
    uint32_t offset = -1;
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
            printk("Write failed: expected %d, written %d; at %d\n", chunk_size, rc, total_written);
            rc = -1;
            goto out;
        }

        total_written += rc;
        rc = 0;

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

out:
    end_time   = DTimestamp_Get();

    stat->write_time_us = end_time - start_time;
    stat->write_speed_kbps = (int)(((float)total_written / 1024 * 1000000) / stat->write_time_us);

    rc = fs_close(&file);
    if (rc != 0) {
        printk("Error closing file: %d\n", rc);
    }

    return rc;
}

static int test_read(struct perf_stats *stat)
{
    int rc;
    struct fs_file_t file;
    uint64_t start_time, end_time;
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

    start_time = DTimestamp_Get();
    
    uint32_t offset;
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
            printk("Read failed: expected %d, read %d; at %d\n", chunk_size, rc, total_read);
            rc = -1;
            goto out;
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

        rc = 0;
    }

out:
    end_time = DTimestamp_Get();

    stat->read_time_us = end_time - start_time;
    stat->read_speed_kbps = (int)(((float)total_read / 1024 * 1000000) / stat->read_time_us);

    fs_close(&file);
    return rc;
}

int main(void) {
    int rc;
    printk("\n***** LittleFS on NOR Flash Performance Test *****\n");

    (void)fs_unlink(TEST_FILE_NAME);

    struct fs_test_config test_config;
    struct fs_test_config *config = &test_config;
    memset(&test_config, 0, sizeof(struct fs_test_config));

    /* 预生成 随机序列 */
    int blocks = TEST_FILE_SIZE/TEST_BLOCK_SIZE;
    if (blocks > RANDOM_COL_RANGE) {
        config->cols = RANDOM_COL_RANGE;
        config->rows = blocks/RANDOM_COL_RANGE;
    } else {
        config->rows = 1;
        config->cols = blocks;
    }

    RandomPermutationsInitialize(config->rows, config->cols);

    for (int random = 0; random < 2; random++) {
        test_config.random_access = random;

        memset(stats, 0, sizeof(stats[0]) * TEST_ITERATIONS);

        for (int i = 0; i < TEST_ITERATIONS; i++) {
            grw_data_pattern = RW_DATA_PATTREN_BASE + i;
            (void)fs_unlink(TEST_FILE_NAME);
            struct perf_stats *stat = &stats[i];
            stat->config = config;

            DCache_CleanInvalidate(0xFFFFFFFF, 0xFFFFFFFF);
            rc = test_write(stat);
            if (rc != 0) {
                printk("[%d] write test failed: %d\n", i, rc);
                // return rc;
            }

            DCache_CleanInvalidate(0xFFFFFFFF, 0xFFFFFFFF);
            rc = test_read(stat);
            if (rc != 0) {
                printk("[%d] read test failed: %d\n", i, rc);
                // return rc;
            }

            // printk("[%d] Write: %llu us, %u KB/s\n", i, stat->write_time_us, stat->write_speed_kbps);
            // printk("[%d] Read: %llu us, %u KB/s.\n",  i, stat->read_time_us, stat->read_speed_kbps);
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

        printk("file_size %d bytes, block_size %d bytes, random access %d. "
            "Average read speed %d KB/s. Average write speed %u KB/s.\r\n", 
            TEST_FILE_SIZE, TEST_BLOCK_SIZE, random, avg_read_speed, avg_write_speed);
    }

    printk("\n***** Finish LittleFS on NOR Flash Performance Test *****\n");

    return 0;
}