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
#define TEST_PARTITION        demo_storage_partition  /* Flash 分区标签 */
#define TEST_MOUNT_POINT      "/lfs1"

#define SYNC_AFTER_WRITE (1)

#define TEST_BLOCK_SIZE_MAX     (32*1024)           /* 测试块最大大小 */
#define TEST_FILE_NAME      "/lfs1/test.bin"
#define TEST_ITERATIONS     (10)

#define CHECK_READ_DATA (0) //  是否检查读出数据的有效性
#define RW_DATA_PATTREN_BASE (0xA0)
static unsigned char grw_data_pattern = 0xA0;

// 随机写的时候，速度可能小于1，而 printk 不支持打印浮点，所以放大显示
#define WRITE_SPEED_MULTIPLIER (100)

struct fs_test_config {
    uint32_t file_size_bytes;   // total file size
    uint32_t block_size_bytes;  // read/write size each time
    bool random_access;
    uint32_t rows;  // random matrix row
    uint32_t cols;  // random matrix columns

    uint32_t avg_write_speed;
    uint32_t avg_read_speed;
    uint32_t read_success_rate_x100;
    uint32_t write_success_rate_x100;
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
    uint32_t written_bytes;
    uint32_t read_bytes;
    bool read_success;  // true: 每次都读成功
    bool write_success; // true: 每次都写成功了
};

static uint8_t buffer[TEST_BLOCK_SIZE_MAX] __attribute__((aligned(4096)));
#if CHECK_READ_DATA
static uint8_t expected_buffer[TEST_BLOCK_SIZE_MAX];
#endif


static const uint32_t file_lengths[] = {
    4*1024,
    8*1024,
    16*1024,
    32*1024,
    64*1024,
    128*1024,
    };

static const uint32_t block_lengths[] = {
    128,
    256,
    512,
    1024,
    2*1024,
    4*1024,
    8*1024,
    16*1024
};

/* 全局统计 */
static struct perf_stats stats[TEST_ITERATIONS];

FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(storage);
static struct fs_mount_t lfs_storage_mnt = {
    .type = FS_LITTLEFS,
    .fs_data = &storage,
    .storage_dev = (void *)FIXED_PARTITION_ID(TEST_PARTITION),
    .mnt_point = TEST_MOUNT_POINT,
};

/******************************************************************/
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

    DCache_Clean((uint32_t)buffer, size);
}

/* 测试顺序写入 */
static int test_write(struct perf_stats *stat)
{
    int rc;
    struct fs_file_t file;
    int64_t start_time, end_time;
    uint64_t start_cycles, end_cycles;

    uint32_t block_size = stat->config->block_size_bytes;  // buffer_size
    uint32_t file_size = stat->config->file_size_bytes;
    uint32_t chunk_size = -1;    // chunk size read or write each time

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
    start_time   = k_uptime_get();
    start_cycles = k_cycle_get_64();
    
    /* 写入 */
    size_t total_written = 0;
    uint32_t offset = -1;
    int row_start = 0;
    int row = 0;
    int col  = 0;
    // DiagPrintf("\n");
    while (total_written < file_size) {
        if (stat->config->random_access) {
            offset = RandomPermutationsGet(row, col, stat->config->cols);
            offset = offset * block_size;
            // DiagPrintf("w [%d][%d] %u\n", row, col, offset);

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
            printk("Write failed: expected %d, written %d; at %d\n", chunk_size, rc, total_written);
            rc = -1;
            goto out;
        }

        total_written += rc;
        stat->write_operations_completed++;
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

#if CHECK_READ_DATA
        if (stat->write_operations_completed == 1) {
            printk("%u, %u, %u, %u\n", buffer[0], buffer[4], buffer[8], buffer[12]);
        }
#endif

    }

    if (stat->config->random_access) {
        printk("last write offset %d, cur_pos %d\n", offset, offset + chunk_size);
    } 

#if SYNC_AFTER_WRITE
    fs_sync(&file);
#endif

out:
    if (total_written == file_size) {
        stat->write_success = true;
    } else {
        stat->write_success = false;
    }
    stat->written_bytes = total_written;

    /* 结束计时 */
    end_time   = k_uptime_get();
    end_cycles = k_cycle_get_64();
    
    /* 记录耗时 */
    stat->write_time_cycles = end_cycles - start_cycles;
    stat->write_time_ms = end_time - start_time;

    rc = fs_close(&file);
    if (rc != 0) {
        printk("Error closing file: %d\n", rc);
    }

    return rc;
}

/* 测试顺序读取 */
static int test_read(struct perf_stats *stat)
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
    generate_test_data(expected_buffer, block_size, grw_data_pattern);
#endif

    /* 开始计时 */
    start_time   = k_uptime_get();
    start_cycles = k_cycle_get_64();
    
    /* 读取并验证 */
    uint32_t offset;
    int row_start = 0;
    int row = 0;
    int col  = 0;
    size_t total_read = 0;
    while (total_read < file_size) {
        if (stat->config->random_access) {
            offset = RandomPermutationsGet(row, col, stat->config->cols);
            offset = offset * block_size;

            // DiagPrintf("r [%d] [%d] %u\n", row, col, offset);

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
            printk("Read failed: expected %d, read %d; at %d\n", chunk_size, rc, total_read);
            rc = -1;
            goto out;
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


        rc = 0;
    }

out:
    if (total_read == file_size) {
        stat->read_success = true;
    } else {
        stat->read_success = false;
    }
    stat->read_bytes = total_read;

    /* 结束计时 */
    end_time   = k_uptime_get();
    end_cycles = k_cycle_get_64();
    
    /* 计算耗时*/
    stat->read_time_cycles = end_cycles - start_cycles;
    stat->read_time_ms = (end_time - start_time);

    fs_close(&file);
    return rc;
}

/* 显示性能结果 */
static void display_performance_results(struct fs_test_config *config, struct perf_stats *stats)
{
    printk("\n====== LittleFS Performance Results ======\n");
    printk("file_size %d bytes, block_size %d bytes, random access %d. "
            "Average read speed %d KB/s. Average write speed %u.%.2u KB/s. "
            "ReadSuccessRate %u%%, WriteSuccessRate %u%%\n", 
        config->file_size_bytes, config->block_size_bytes, config->random_access, 
        config->avg_read_speed, config->avg_write_speed / WRITE_SPEED_MULTIPLIER, config->avg_write_speed % WRITE_SPEED_MULTIPLIER,
        config->read_success_rate_x100, config->write_success_rate_x100);

    for (int i = 0; i < TEST_ITERATIONS; i++) {
        struct perf_stats *stat = &stats[i];
        printk("[%d] WriteSuccess %d, Completed Operations %u; ReadSuccess %d, Completed Operations %u\n",
            i, stat->write_success, stat->write_operations_completed, stat->read_success, stat->read_operations_completed);
        printk("[%d] Sequential Write: %llu ms, %llu us, %u.%.2u KB/s\n", 
            i, stat->write_time_ms, stat->write_time_us,
            stat->write_speed_kbps / WRITE_SPEED_MULTIPLIER, stat->write_speed_kbps % WRITE_SPEED_MULTIPLIER);
        printk("[%d] Sequential Read:  %llu ms, %llu us, %u KB/s.\n", 
            i, stat->read_time_ms, stat->read_time_us, stat->read_speed_kbps);

    }
    printk("======================================\n\n");
}

/* 主测试函数 */
int main(void) {
    int rc;
    printk("\n***** LittleFS on NOR Flash Performance Test *****\n");
    uint64_t cycles_per_sec = sys_clock_hw_cycles_per_sec();
    printk("cycles_per_sec=%llu\n", cycles_per_sec);

    print_file_system_status();

    // 清理测试文件，确保测试环境重置
    (void)fs_unlink(TEST_FILE_NAME);

    print_file_system_status();

    int total_cases = 2*ARRAY_SIZE(block_lengths)*ARRAY_SIZE(file_lengths);
    int case_number = 0;
    struct fs_test_config test_config;
    for (int random = 0; random < 2; random++) {
        for (size_t block = 0; block < ARRAY_SIZE(block_lengths); block++) {
            for (size_t flen = 0; flen < ARRAY_SIZE(file_lengths); flen++) {
                // 设置测试参数
                memset(&test_config, 0, sizeof(struct fs_test_config));
                test_config.file_size_bytes =  file_lengths[flen];
                test_config.block_size_bytes = block_lengths[block];
                test_config.random_access = random;
                case_number++;

                struct fs_test_config *config = &test_config;
                if ((config->block_size_bytes > config->file_size_bytes) 
                    // || (config->random_access && config->file_size_bytes > 16*1024)
                    ) {
                    printk("skip: [%d:%d] file %u bytes, block %u bytes, random access %d\n",
                        case_number, total_cases,
                        config->file_size_bytes, config->block_size_bytes, config->random_access);
                    continue;
                }

                if (config->block_size_bytes > TEST_BLOCK_SIZE_MAX) {
                    printk("ERROR: block_size %d exceeds %d\n", config->block_size_bytes, TEST_BLOCK_SIZE_MAX);
                    continue;
                }

                printk("test: [%d:%d] file %d bytes, block %d bytes, random access %d\n", 
                    case_number, total_cases,
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
                    grw_data_pattern = RW_DATA_PATTREN_BASE + i;
                    (void)fs_unlink(TEST_FILE_NAME);
                    struct perf_stats *stat = &stats[i];
                    stat->config = config;

                    printk("iteration: %d:%d\n", i, TEST_ITERATIONS);
                    /* 测试1: 顺序写入 */
                    // print_file_system_status();
                    printk("Test 1: write test...\n");
                    DCache_CleanInvalidate(0xFFFFFFFF, 0xFFFFFFFF);
                    rc = test_write(stat);
                    if (rc != 0) {
                        printk("[%d] Sequential write test failed: %d\n", i, rc);
                        // return rc;
                    }

                    /* 测试2: 顺序读取 */
                    // print_file_system_status();
                    printk("Test 2: read test...\n");
                    DCache_CleanInvalidate(0xFFFFFFFF, 0xFFFFFFFF);
                    rc = test_read(stat);
                    if (rc != 0) {
                        printk("[%d] Sequential read test failed: %d\n", i, rc);
                        // return rc;
                    }
                }

                /* 计算均值, 只有成功的 iteration 参与均值计算，
                    以防在 pos=0 处失败时，read_bytes或written_bytes 为0，导致计算的速度为0*/
                uint32_t total_read_speed = 0;
                uint32_t total_write_speed = 0;
                uint32_t read_success_times = 0;
                uint32_t write_success_times = 0;
                for (int i = 0; i < TEST_ITERATIONS; i++) {
                    struct perf_stats *stat = &stats[i];

                    stat->read_time_us = (stat->read_time_cycles * 1000000ULL) / cycles_per_sec;
                    stat->write_time_us = (stat->write_time_cycles * 1000000ULL) / cycles_per_sec;
                    stat->read_speed_kbps = (int)(((float)stat->read_bytes / 1024 * cycles_per_sec) / stat->read_time_cycles);
                    stat->write_speed_kbps = (int)(((float)stat->written_bytes / 1024 * cycles_per_sec * WRITE_SPEED_MULTIPLIER) / stat->write_time_cycles);

                    if (stat->read_success) {
                        read_success_times++;
                        total_read_speed += stat->read_speed_kbps;
                    }
                    if (stat->write_success) {
                        write_success_times++;
                        total_write_speed += stat->write_speed_kbps;
                    }
                }

                if (read_success_times > 0) {
                    config->avg_read_speed = total_read_speed / read_success_times;
                } else {
                    config->avg_read_speed = -1;
                }

                if (write_success_times > 0) {
                    config->avg_write_speed = total_write_speed / write_success_times;
                } else {
                    config->avg_write_speed = -1;
                }

                config->read_success_rate_x100  = read_success_times * 100 / TEST_ITERATIONS;
                config->write_success_rate_x100 = write_success_times * 100 / TEST_ITERATIONS;

                /* 显示结果 */
                display_performance_results(config, stats);
            }
        }
    }

    printk("\n***** Finish LittleFS on NOR Flash Performance Test *****\n");

    return 0;
}