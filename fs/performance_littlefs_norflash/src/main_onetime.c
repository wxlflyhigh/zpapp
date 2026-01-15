/*
 * LittleFS 性能测试代码
 * 针对 NOR Flash 特性优化
 */

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/printk.h>
#include <string.h>
#include <stdlib.h>

/* 测试配置 */
#define TEST_PARTITION        storage_partition  /* Flash 分区标签 */
#define TEST_MOUNT_POINT      "/lfs1"
#define TEST_BLOCK_SIZE       4096              /* 匹配 Flash 擦除块大小 */
#define TEST_FILE_SIZE        (64 * 1024)       /* 64KB 测试文件 */
#define TEST_ITERATIONS       100
#define TEST_SMALL_FILE_COUNT 100

/* 性能统计结构 */
struct lfs_perf_stats {
    uint64_t seq_write_time;
    uint64_t seq_read_time;
    uint64_t random_write_time;
    uint64_t random_read_time;
    uint32_t write_speed;
    uint32_t read_speed;
    uint32_t operations;
    uint32_t erase_cycles;
};

/* 测试状态 */
static struct lfs_perf_stats stats;
FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(storage);
static struct fs_mount_t lfs_storage_mnt = {
    .type = FS_LITTLEFS,
    .fs_data = &storage,
    .storage_dev = (void *)FIXED_PARTITION_ID(TEST_PARTITION),
    .mnt_point = TEST_MOUNT_POINT,
};

/* 生成测试数据（模式化，便于验证） */
static void generate_pattern_data(uint8_t *buf, size_t len, uint32_t seed) {
    for (size_t i = 0; i < len; i++) {
        buf[i] = (seed + i) & 0xFF;
    }
}

/* 验证数据完整性 */
static int verify_pattern_data(const uint8_t *buf, size_t len, uint32_t seed) {
    for (size_t i = 0; i < len; i++) {
        if (buf[i] != ((seed + i) & 0xFF)) {
            printk("数据验证失败 @ offset %zu: 预期 0x%02x, 实际 0x%02x\n",
                   i, (seed + i) & 0xFF, buf[i]);
            return -1;
        }
    }
    return 0;
}

static void print_fs_status() {
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

/* 场景1：基础顺序读写测试 */
static int test_sequential_rw(void) {
    char filename[32];
    struct fs_file_t file;
    uint8_t *buffer;
    int rc;
    int64_t start_time, end_time;
    
    snprintf(filename, sizeof(filename), "%s/seq_test.bin", TEST_MOUNT_POINT);
    
    /* 分配对齐的缓冲区（重要！） */
    buffer = k_malloc(TEST_BLOCK_SIZE);
    if (!buffer) {
        printk("内存分配失败\n");
        return -ENOMEM;
    }
    
    fs_file_t_init(&file);
    
    /* === 顺序写入测试 === */
    printk("开始顺序写入测试...\n");
    rc = fs_open(&file, filename, FS_O_CREATE | FS_O_WRITE);
    if (rc < 0) {
        printk("无法创建文件: %d\n", rc);
        goto cleanup;
    }
    
    generate_pattern_data(buffer, TEST_BLOCK_SIZE, 0xAA);
    start_time = k_uptime_get();
    
    size_t total_written = 0;
    while (total_written < TEST_FILE_SIZE) {
        rc = fs_write(&file, buffer, TEST_BLOCK_SIZE);
        if (rc < 0) {
            printk("写入失败: %d\n", rc);
            break;
        }
        total_written += rc;
        stats.operations++;
    }
    
    end_time = k_uptime_get();
    stats.seq_write_time = (end_time - start_time);
    fs_close(&file);
    
    if (total_written < TEST_FILE_SIZE) {
        rc = -EIO;
        goto cleanup;
    }
    
    printk("顺序写入: %zu bytes, %llu ms\n", total_written, stats.seq_write_time);
    print_fs_status();

    /* === 顺序读取测试 === */
    printk("开始顺序读取测试...\n");
    rc = fs_open(&file, filename, FS_O_READ);
    if (rc < 0) {
        printk("无法打开文件: %d\n", rc);
        goto cleanup;
    }
    
    start_time = k_uptime_get();
    size_t total_read = 0;
    while (total_read < TEST_FILE_SIZE) {
        rc = fs_read(&file, buffer, TEST_BLOCK_SIZE);
        if (rc < 0) {
            printk("读取失败: %d\n", rc);
            break;
        }
        
        // 验证数据
        if (verify_pattern_data(buffer, rc, 0xAA) < 0) {
            rc = -EIO;
            break;
        }
        
        total_read += rc;
    }
    
    end_time = k_uptime_get();
    stats.seq_read_time = (end_time - start_time);
    fs_close(&file);
    
    printk("顺序读取: %zu bytes, %llu ms\n", total_read, stats.seq_read_time);
    print_fs_status();

    // 计算速度
    stats.write_speed = (TEST_FILE_SIZE / 1024 * 1000) / stats.seq_write_time;
    stats.read_speed  = (TEST_FILE_SIZE / 1024 * 1000) / stats.seq_read_time;
    
    rc = 0;
    
cleanup:
    fs_unlink(filename);
    k_free(buffer);
    return rc;
}

/* 场景2：随机访问测试（NOR Flash 关键测试） */
static int test_random_access(void) {
    char filename[32];
    struct fs_file_t file;
    uint8_t *write_buf, *read_buf;
    int rc;
    int64_t total_time = 0;
    const int iterations = 100;
    
    snprintf(filename, sizeof(filename), "%s/random_test.bin", TEST_MOUNT_POINT);
    
    write_buf = k_malloc(TEST_BLOCK_SIZE);
    read_buf = k_malloc(TEST_BLOCK_SIZE);
    if (!write_buf || !read_buf) {
        rc = -ENOMEM;
        goto cleanup;
    }
    
    /* 先创建一个基础文件 */
    fs_file_t_init(&file);
    rc = fs_open(&file, filename, FS_O_CREATE | FS_O_WRITE);
    if (rc < 0) {
        printk("无法创建文件: %d\n", rc);
        goto cleanup;
    }
    
    // 用零填充文件
    memset(write_buf, 0, TEST_BLOCK_SIZE);
    for (int i = 0; i < 16; i++) {
        fs_write(&file, write_buf, TEST_BLOCK_SIZE);
    }
    fs_close(&file);
    
    /* 随机访问测试 */
    printk("开始随机访问测试 (%d 次迭代)...\n", iterations);
    
    for (int i = 0; i < iterations; i++) {
        // 生成随机偏移（块对齐）
        off_t offset = (i * 997) % (16 * TEST_BLOCK_SIZE - TEST_BLOCK_SIZE);
        offset = (offset / TEST_BLOCK_SIZE) * TEST_BLOCK_SIZE;
        
        // 生成随机数据
        generate_pattern_data(write_buf, TEST_BLOCK_SIZE, i);
        
        // 打开文件进行读写
        rc = fs_open(&file, filename, FS_O_RDWR);
        if (rc < 0) {
            printk("无法打开文件: %d\n", rc);
            break;
        }
        
        int64_t start_time = k_uptime_get();
        
        // 定位并写入
        rc = fs_seek(&file, offset, FS_SEEK_SET);
        if (rc < 0) {
            printk("定位失败: %d\n", rc);
            fs_close(&file);
            break;
        }
        
        rc = fs_write(&file, write_buf, TEST_BLOCK_SIZE);
        if (rc < 0) {
            printk("随机写入失败: %d\n", rc);
            fs_close(&file);
            break;
        }
        
        // 重新定位并读取验证
        rc = fs_seek(&file, offset, FS_SEEK_SET);
        if (rc < 0) {
            printk("定位失败: %d\n", rc);
            fs_close(&file);
            break;
        }
        
        rc = fs_read(&file, read_buf, TEST_BLOCK_SIZE);
        if (rc < 0) {
            printk("随机读取失败: %d\n", rc);
            fs_close(&file);
            break;
        }
        
        // 验证数据
        if (memcmp(write_buf, read_buf, TEST_BLOCK_SIZE) != 0) {
            printk("数据不匹配 @ 迭代 %d\n", i);
            fs_close(&file);
            rc = -EIO;
            break;
        }
        
        int64_t end_time = k_uptime_get();
        total_time += (end_time - start_time);
        
        fs_close(&file);
        stats.operations++;
        
        if ((i + 1) % 20 == 0) {
            printk("  完成 %d/%d 次随机操作\n", i + 1, iterations);
        }
    }
    
    stats.random_write_time = total_time * 1000 / iterations; // 平均微秒
    
    printk("随机访问测试完成，平均 %llu us/操作\n", stats.random_write_time);
    
cleanup:
    fs_unlink(filename);
    k_free(write_buf);
    k_free(read_buf);
    return rc;
}

/* 场景3：小文件密集型操作 */
static int test_small_files(void) {
    char filename[32];
    struct fs_file_t file;
    uint8_t buffer[256];
    int rc;
    int64_t start_time, end_time;
    int success_count = 0;
    
    printk("开始小文件测试 (%d 个文件)...\n", TEST_SMALL_FILE_COUNT);
    
    start_time = k_uptime_get();
    
    /* 创建大量小文件 */
    for (int i = 0; i < TEST_SMALL_FILE_COUNT; i++) {
        snprintf(filename, sizeof(filename), "%s/small_%04d.txt", 
                 TEST_MOUNT_POINT, i);
        
        fs_file_t_init(&file);
        rc = fs_open(&file, filename, FS_O_CREATE | FS_O_WRITE);
        if (rc < 0) {
            printk("无法创建文件 %s: %d\n", filename, rc);
            continue;
        }
        
        // 写入少量数据
        snprintf((char *)buffer, sizeof(buffer), 
                "小文件测试 #%d, 时间戳: %lld\n", i, k_uptime_get());
        rc = fs_write(&file, buffer, strlen((char *)buffer));
        fs_close(&file);
        
        if (rc >= 0) {
            success_count++;
        }
        
        stats.operations++;
    }
    
    end_time = k_uptime_get();
    printk("创建 %d 个小文件耗时: %lld ms\n", 
           success_count, end_time - start_time);
    
    /* 读取并验证小文件 */
    start_time = k_uptime_get();
    int verify_count = 0;
    
    for (int i = 0; i < TEST_SMALL_FILE_COUNT; i++) {
        snprintf(filename, sizeof(filename), "%s/small_%04d.txt", 
                 TEST_MOUNT_POINT, i);
        
        fs_file_t_init(&file);
        rc = fs_open(&file, filename, FS_O_READ);
        if (rc < 0) {
            continue; // 文件可能创建失败
        }
        
        rc = fs_read(&file, buffer, sizeof(buffer));
        fs_close(&file);
        
        if (rc > 0) {
            verify_count++;
        }
    }
    
    end_time = k_uptime_get();
    printk("读取 %d 个小文件耗时: %lld ms\n", 
           verify_count, end_time - start_time);
    
    /* 删除小文件 */
    start_time = k_uptime_get();
    int delete_count = 0;
    
    for (int i = 0; i < TEST_SMALL_FILE_COUNT; i++) {
        snprintf(filename, sizeof(filename), "%s/small_%04d.txt", 
                 TEST_MOUNT_POINT, i);
        
        if (fs_unlink(filename) == 0) {
            delete_count++;
        }
    }
    
    end_time = k_uptime_get();
    printk("删除 %d 个小文件耗时: %lld ms\n", 
           delete_count, end_time - start_time);
    
    return 0;
}

/* 场景4：目录操作测试 */
static int test_directory_operations(void) {
    struct fs_dir_t dir;
    char dir_path[64];
    char subdir_path[64];
    int rc;
    int64_t start_time, end_time;
    
    printk("开始目录操作测试...\n");
    
    /* 测试1：创建和遍历目录 */
    start_time = k_uptime_get();
    
    // 创建嵌套目录
    for (int i = 0; i < 5; i++) {
        snprintf(dir_path, sizeof(dir_path), "%s/dir_level_%d", 
                 TEST_MOUNT_POINT, i);
        rc = fs_mkdir(dir_path);
        if (rc < 0 && rc != -EEXIST) {
            printk("创建目录失败 %s: %d\n", dir_path, rc);
        }
    }
    
    // 在最后一级目录创建文件
    snprintf(dir_path, sizeof(dir_path), "%s/dir_level_4", TEST_MOUNT_POINT);
    for (int i = 0; i < 10; i++) {
        snprintf(subdir_path, sizeof(subdir_path), 
                "%s/file_%d.txt", dir_path, i);
        struct fs_file_t file;
        fs_file_t_init(&file);
        rc = fs_open(&file, subdir_path, FS_O_CREATE | FS_O_WRITE);
        if (rc == 0) {
            fs_close(&file);
        }
    }
    
    /* 测试2：目录遍历性能 */
    fs_dir_t_init(&dir);
    rc = fs_opendir(&dir, TEST_MOUNT_POINT);
    if (rc < 0) {
        printk("无法打开目录: %d\n", rc);
        return rc;
    }
    
    int entry_count = 0;
    struct fs_dirent entry;
    
    while (1) {
        rc = fs_readdir(&dir, &entry);
        if (rc < 0 || entry.name[0] == 0) {
            break;
        }
        entry_count++;
    }
    
    fs_closedir(&dir);
    end_time = k_uptime_get();
    
    printk("目录遍历: %d 个条目, 耗时 %lld ms\n", 
           entry_count, end_time - start_time);
    
    /* 清理：删除测试目录 */
    for (int i = 4; i >= 0; i--) {
        snprintf(dir_path, sizeof(dir_path), "%s/dir_level_%d", 
                 TEST_MOUNT_POINT, i);
        
        // 先删除目录中的文件
        if (i == 4) {
            for (int j = 0; j < 10; j++) {
                snprintf(subdir_path, sizeof(subdir_path), 
                        "%s/file_%d.txt", dir_path, j);
                fs_unlink(subdir_path);
            }
        }
        
        fs_unlink(dir_path); // LittleFS 中删除目录使用 unlink
    }
    
    return 0;
}

/* 显示性能结果 */
static void display_performance_results(void) {
    printk("\n====== LittleFS 性能测试结果 ======\n");
    printk("存储设备: NOR Flash @ %s\n", TEST_MOUNT_POINT);
    printk("块大小: %d 字节\n", TEST_BLOCK_SIZE);
    printk("----------------------------------\n");
    printk("顺序写入: %llu us, %u KB/s\n", 
           stats.seq_write_time, stats.write_speed);
    printk("顺序读取: %llu us, %u KB/s\n", 
           stats.seq_read_time, stats.read_speed);
    printk("随机访问: %llu us/操作\n", stats.random_write_time);
    printk("总操作数: %u\n", stats.operations);
    printk("==================================\n");
}

/* 主测试函数 */
int main(void) {
    int rc;
    printk("\n***** LittleFS on NOR Flash Performance Test *****\n");
    print_fs_status();
    
    /* 运行测试场景 */
    printk("\n--- 场景1: 基础顺序读写 ---\n");
    rc = test_sequential_rw();
    if (rc != 0) {
        printk("顺序读写测试失败: %d\n", rc);
    }

#if 0
    printk("\n--- 场景2: 随机访问 ---\n");
    rc = test_random_access();
    if (rc != 0) {
        printk("随机访问测试失败: %d\n", rc);
    }
    
    printk("\n--- 场景3: 小文件操作 ---\n");
    rc = test_small_files();
    if (rc != 0) {
        printk("小文件测试失败: %d\n", rc);
    }
    
    printk("\n--- 场景4: 目录操作 ---\n");
    rc = test_directory_operations();
    if (rc != 0) {
        printk("目录操作测试失败: %d\n", rc);
    }
#endif

    /* 显示结果 */
    display_performance_results();

    return 0;
}