
#include "ff.h"
#include <stdio.h>
#include <stdlib.h>

// diskio_linux.c 中写死了 pdrv 必须是0
// 对应 zephyr_fatfs_config.h 中的 FF_VOLUME_STRS，编号从0开始
#define MOUNT_PT "NAND:"
#define TESTFILE2 "test2.txt"


static int test_statvfs(void)
{
    FATFS fs;
    FIL fil;
    FRESULT fr;
    UINT bw;
    FILINFO fno;  // 文件信息结构体

    printf("FatFs Sample Application on Linux\n");

    /* 挂载文件系统 */
    fr = f_mount(&fs, MOUNT_PT, 1);
    if (fr != FR_OK) {
        printf("Mount failed: error %d\n", fr);
        return 1;
    }
    printf("File system mounted successfully.\n");

    /* 创建测试文件 */
    fr = f_open(&fil, TESTFILE2, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr == FR_OK) {
        f_write(&fil, "Test content", 12, &bw);
        f_close(&fil);
        printf("Test file created.\n");
    } else {
        printf("open Test file (%s) failed.\n", TESTFILE2);
    }

    /* 使用f_stat获取文件状态 */
    printf("\n=== 使用f_stat获取文件状态 ===\n");
    
    fr = f_stat(TESTFILE2, &fno);
    if (fr == FR_OK) {
        printf("文件状态信息：\n");
        printf("  文件大小: %u 字节\n", fno.fsize);
        printf("  最后修改日期: %u-%u-%u\n", 
               (fno.fdate >> 9) + 1980,  // 年
               (fno.fdate >> 5) & 15,      // 月  
               fno.fdate & 31);              // 日
        printf("  最后修改时间: %u:%u:%u\n",
               fno.ftime >> 11,             // 时
               (fno.ftime >> 5) & 63,      // 分
               (fno.ftime & 31) * 2);       // 秒
        printf("  文件属性: ");
        if (fno.fattrib & AM_DIR) printf("目录 ");
        if (fno.fattrib & AM_RDO) printf("只读 ");
        if (fno.fattrib & AM_HID) printf("隐藏 ");
        if (fno.fattrib & AM_SYS) printf("系统 ");
        if (fno.fattrib & AM_ARC) printf("存档 ");
        printf("\n");
        
        // 长文件名支持（如果启用）
        #if FF_USE_LFN
        printf("  长文件名: %s\n", fno.fname);
        #else
        printf("  文件名: %s\n", fno.fname);
        #endif
    } else {
        printf("获取文件状态失败: 错误代码 %d\n", fr);
    }

    /* 检查不存在的文件 */
    printf("\n=== 检查不存在文件 ===\n");
    fr = f_stat("nonexistent.txt", &fno);
    if (fr == FR_NO_FILE) {
        printf("文件不存在，符合预期\n");
    } else {
        printf("不符合预期， fr=%d\n", fr);
    }

    /* 获取目录状态 */
    printf("\n=== 获取目录状态 ===\n");
    fr = f_stat("/", &fno);
    if (fr == FR_OK && (fno.fattrib & AM_DIR)) {
        printf("根目录状态获取成功\n");
    } else {
        printf("根目录状态获取失败， fr=%d\n", fr);
    }

    /* 卸载文件系统 */
    f_mount(NULL, "", 0);
    printf("File system unmounted.\n");

	return 0;
}

int test_normal_flow() {
    FATFS fs;
    FIL fil;
    FRESULT fr;
    UINT bw;

    printf("FatFs Sample Application on Linux\n");

    /* 挂载文件系统 */
    fr = f_mount(&fs, MOUNT_PT, 1);
    if (fr != FR_OK) {
        printf("Mount failed: error %d\n", fr);
        return 1;
    }
    printf("File system mounted successfully.\n");

    /* 创建并写入文件 */
    fr = f_open(&fil, "hello.txt", FA_WRITE | FA_CREATE_ALWAYS);
    if (fr == FR_OK) {
        fr = f_write(&fil, "Hello, FatFs!\n", 14, &bw);
        f_close(&fil);
        if (fr == FR_OK && bw == 14) {
            printf("File 'hello.txt' created and written.\n");
        }
    }

    /* 读取文件内容 */
    fr = f_open(&fil, "hello.txt", FA_READ);
    if (fr == FR_OK) {
        char buffer[64];
        fr = f_read(&fil, buffer, sizeof(buffer), &bw);
        f_close(&fil);
        if (fr == FR_OK) {
            buffer[bw] = '\0';
            printf("File content: %s", buffer);
        }
    }

    /* 列出目录内容 */
    DIR dir;
    FILINFO fno;
    
    printf("\nDirectory listing:\n");
    fr = f_opendir(&dir, "/");
    if (fr == FR_OK) {
        while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
            printf("  %s\n", fno.fname);
        }
        f_closedir(&dir);
    }

    /* 卸载文件系统 */
    f_mount(NULL, "", 0);
    printf("File system unmounted.\n");

    return 0;
}

int main() {
    test_normal_flow();
    test_statvfs();
    return 0;
}