#include <stdio.h>
#include <string.h>
#include "lsmalloc.h"

int main() {
    // 初始化分配器属性
    lsmem_attr_t attr;
    if (lsmem_attr_init(&attr) != LSMALLOC_SUCCESS) {
        printf("Failed to initialize allocator attributes\n");
        return 1;
    }

    // 设置总大小为 1GB
    attr.total_size = 1024 * 1024 * 1024;
    // 设置CXL设备路径（使用/dev/null作为测试）
    attr.cxl_dev_path = "/dev/shm/cxl";

    // 创建分配器
    lsmem_t allocator = lsmem_create(&attr);
    if (!allocator) {
        printf("Failed to create allocator\n");
        return 1;
    }

    // 分配内存
    const char* test_str = "Hello, Log-Structured Memory!";
    size_t str_len = strlen(test_str) + 1;
    SHMRef ref = lsmem_malloc(allocator, str_len);
    if (ref == 0) {
        printf("Failed to allocate memory\n");
        lsmem_destroy(allocator);
        return 1;
    }

    // 写入数据
    if (lsmem_write(allocator, ref, 0, test_str, str_len) != LSMALLOC_SUCCESS) {
        printf("Failed to write data\n");
        lsmem_destroy(allocator);
        return 1;
    }

    // 读取数据
    void* read_ptr = lsmem_read(allocator, ref, 0, str_len);
    if (!read_ptr) {
        printf("Failed to read data\n");
        lsmem_destroy(allocator);
        return 1;
    }

    // 验证数据
    if (strcmp((char*)read_ptr, test_str) != 0) {
        printf("Data verification failed\n");
        lsmem_destroy(allocator);
        return 1;
    }

    printf("Successfully read: %s\n", (char*)read_ptr);

    // 获取剩余空间
    size_t free_space = lsmem_free_space(allocator);
    printf("Free space: %zu bytes\n", free_space);

    // 执行垃圾回收
    if (lsmem_gc(allocator) != LSMALLOC_SUCCESS) {
        printf("Failed to perform garbage collection\n");
        lsmem_destroy(allocator);
        return 1;
    }

    // 释放内存
    if (lsmem_free(allocator, ref) != LSMALLOC_SUCCESS) {
        printf("Failed to free memory\n");
        lsmem_destroy(allocator);
        return 1;
    }

    // 销毁分配器
    if (lsmem_destroy(allocator) != LSMALLOC_SUCCESS) {
        printf("Failed to destroy allocator\n");
        return 1;
    }

    printf("All operations completed successfully\n");
    return 0;
} 