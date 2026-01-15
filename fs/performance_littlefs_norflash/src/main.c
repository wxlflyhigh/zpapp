/*
 * LittleFS on NOR Flash 性能测试代码
 */

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
#define TEST_PARTITION        storage_partition  /* Flash 分区标签 */
#define TEST_MOUNT_POINT      "/lfs1"

/* 测试配置 */
#define TEST_BLOCK_SIZE_MAX     (32*1024)           /* 测试块最大大小 */
#define TEST_FILE_NAME      "/lfs1/test.bin"
#define TEST_ITERATIONS     (1)

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
    uint64_t read_time_cycles;
    uint64_t write_time_cycles;
    uint32_t write_operations_completed;
    uint32_t read_operations_completed;

    uint64_t read_time_us;
    uint64_t write_time_us;
    uint32_t write_speed_kbps;
    uint32_t read_speed_kbps;
};

static uint8_t buffer[TEST_BLOCK_SIZE_MAX];
#if CHECK_READ_DATA
static uint8_t expected_buffer[TEST_BLOCK_SIZE_MAX];
#endif

/* 全局统计 */
static struct perf_stats stats[TEST_ITERATIONS];

static struct fs_test_config configs[] = {
    // {64*1024, 1*1024, 0},
    // {64*1024, 4*1024, 0},
    {64*1024, 1*1024, 1},
    // {64*1024, 4*1024, 1},

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
};

FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(storage);
static struct fs_mount_t lfs_storage_mnt = {
    .type = FS_LITTLEFS,
    .fs_data = &storage,
    .storage_dev = (void *)FIXED_PARTITION_ID(TEST_PARTITION),
    .mnt_point = TEST_MOUNT_POINT,
};

/******************************************************************/
/* 打印文件系统的使用情况 */
static void print_file_system_status() {
    struct fs_statvfs stats;
    int rc = fs_statvfs(lfs_storage_mnt.mnt_point, &stats);
    if (rc < 0) {
        LOG_PRINTK("FAIL: statvfs: %d\n", rc);
        return;
    }

    LOG_PRINTK("%s: bsize = %lu ; frsize = %lu ; blocks = %lu ; bfree = %lu;"
                "total size %lu KB, available size %lu KB, used %lu KB\n",
            lfs_storage_mnt.mnt_point,
            stats.f_bsize, stats.f_frsize, stats.f_blocks, stats.f_bfree,
           stats.f_frsize * stats.f_blocks / 1024,
           stats.f_frsize * stats.f_bfree / 1024,
           (stats.f_blocks - stats.f_bfree) *stats.f_frsize / 1024
           );
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
}

/* 测试顺序写入 */
static int test_sequential_write(struct perf_stats *stat)
{
    int rc;
    struct fs_file_t file;
    int64_t start_time, end_time;
    uint64_t start_cycles, end_cycles;

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
    generate_test_data(buffer, block_size, RW_DATA_PATTREN);
    
    /* 开始计时 */
    start_time   = k_uptime_get();
    start_cycles = k_cycle_get_64();
    
    /* 写入 */
    size_t total_written = 0;
    while (total_written < file_size) {
        if (stat->config->random_access) {
            uint32_t offset = sys_rand32_get() % (file_size/block_size) * block_size;
            rc = fs_seek(&file, offset, FS_SEEK_SET);
            if (rc < 0) {
                printk("Seek failed: %d, offset %d\n", rc, offset);
                break;
            }
            // printk("rand write at offset %u\n", offset);
        }

        chunk_size = (file_size - total_written < block_size) ? file_size - total_written : block_size;
        rc = fs_write(&file, buffer, chunk_size);
        if (rc < 0 ||  rc != chunk_size) {
            printk("Write failed: %d\n", rc);
            goto error;
        }

        total_written += rc;
        stat->write_operations_completed++;
    }
    
    /* 结束计时 */
    end_time   = k_uptime_get();
    end_cycles = k_cycle_get_64();
    
    /* 记录耗时 */
    stat->write_time_cycles = end_cycles - start_cycles;
    stat->write_time_ms = end_time - start_time;

error:
    rc = fs_close(&file);
    if (rc != 0) {
        printk("Error closing file: %d\n", rc);
    }

    return 0;
}

/* 测试顺序读取 */
static int test_sequential_read(struct perf_stats *stat)
{
    int rc;
    struct fs_file_t file;
    int64_t start_time, end_time;
    uint64_t start_cycles, end_cycles;
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
    start_time   = k_uptime_get();
    start_cycles = k_cycle_get_64();
    
    /* 读取并验证 */
    size_t total_read = 0;
    while (total_read < file_size) {
        if (stat->config->random_access) {
            uint32_t offset = sys_rand32_get() % (file_size/block_size) * block_size;
            rc = fs_seek(&file, offset, FS_SEEK_SET);
            if (rc < 0) {
                printk("Seek failed: %d, offset %d\n", rc, offset);
                break;
            }
            // printk("rand read at offset %u\n", offset);
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
    end_time   = k_uptime_get();
    end_cycles = k_cycle_get_64();
    
    /* 计算耗时*/
    stat->read_time_cycles = end_cycles - start_cycles;
    stat->read_time_ms = (end_time - start_time);

    fs_close(&file);
    return 0;
}

/* 显示性能结果 */
static void display_performance_results(struct fs_test_config *config, struct perf_stats *stats)
{
    printk("\n====== LittleFS Performance Results ======\n");
    printk("file_size %d bytes, block_size %d bytes, random access %d. Average read speed %d KB/s. Average write speed %d KB/s\n", 
        config->file_size_bytes, config->block_size_bytes, config->random_access, 
        config->avg_read_speed, config->avg_write_speed);

    for (int i = 0; i < TEST_ITERATIONS; i++) {
        struct perf_stats *stat = &stats[i];
        printk("[%d] Sequential Write: %llu ms, %llu us, %u KB/s\n", 
            i, stat->write_time_ms, stat->write_time_us, stat->write_speed_kbps);
        printk("[%d] Sequential Read:  %llu ms, %llu us, %u KB/s.\n", 
            i, stat->read_time_ms, stat->read_time_us, stat->read_speed_kbps);
        printk("[%d] Completed Write Operations %u, Read Operations %u\n",
            i, stat->write_operations_completed, stat->read_operations_completed);
    }
    printk("======================================\n");
}

/* 主测试函数 */
int main(void) {
    int rc;
    printk("\n***** LittleFS on NOR Flash Performance Test *****\n");

    uint64_t cycles_per_sec = sys_clock_hw_cycles_per_sec();
    printk("cycles_per_sec=%llu", cycles_per_sec);
    print_file_system_status();

    int config_nums = (sizeof(configs) / sizeof(configs[0]));
    for (int c = 0; c < config_nums; c++) {
        struct fs_test_config *config = &configs[c];
        if (config->block_size_bytes > TEST_BLOCK_SIZE_MAX) {
            printk("ERROR: block_size %d exceeds %d\n", config->block_size_bytes, TEST_BLOCK_SIZE_MAX);
            continue;
        }
        printk("test [%d:%d] file_size %d bytes, block_size %d bytes, random access %d\n", 
            c, config_nums,
            config->file_size_bytes, config->block_size_bytes, config->random_access);

        memset(stats, 0, sizeof(stats[0]) * TEST_ITERATIONS);
        for (int i = 0; i < TEST_ITERATIONS; i++) {
            struct perf_stats *stat = &stats[i];
            stat->config = config;

            /* 测试1: 顺序写入 */
            printk("Test 1: Sequential write test...\n");
            print_file_system_status();
            rc = test_sequential_write(stat);
            if (rc != 0) {
                printk("Sequential write test failed: %d\n", rc);
                return rc;
            }

            /* 测试2: 顺序读取 */
            printk("Test 2: Sequential read test...\n");
            print_file_system_status();
            rc = test_sequential_read(stat);
            if (rc != 0) {
                printk("Sequential read test failed: %d\n", rc);
                return rc;
            }

        }

        /* 计算均值 */
        uint32_t total_read_speed = 0;
        uint32_t total_write_speed = 0;
        for (int i = 0; i < TEST_ITERATIONS; i++) {
            struct perf_stats *stat = &stats[i];

            stat->read_time_us = (stat->read_time_cycles * 1000000ULL) / cycles_per_sec;
            stat->write_time_us = (stat->write_time_cycles * 1000000ULL) / cycles_per_sec;
            stat->read_speed_kbps = (int)(((float)config->file_size_bytes / 1024 * cycles_per_sec) / stat->read_time_cycles);
            stat->write_speed_kbps = (int)(((float)config->file_size_bytes / 1024 * cycles_per_sec) / stat->write_time_cycles);

            total_read_speed += stat->read_speed_kbps;
            total_write_speed += stat->write_speed_kbps;
        }
        config->avg_read_speed = total_read_speed / TEST_ITERATIONS;
        config->avg_write_speed = total_write_speed / TEST_ITERATIONS;

        /* 显示结果 */
        display_performance_results(config, stats);

    }

    return 0;
}