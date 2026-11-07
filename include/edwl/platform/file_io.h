/*
 * edwl - 平台抽象层：文件随机读写
 *
 * 句柄是不透明的 intptr_t：POSIX 下装 fd，Win32 下装 HANDLE。
 * 核心层只通过本文件提供的函数操作它，不得解读其内容。
 */
#ifndef EDWL_PLATFORM_FILE_IO_H
#define EDWL_PLATFORM_FILE_IO_H

#include <stddef.h>
#include <stdint.h>

typedef intptr_t edwl_file_t;

/*
 * 打开/创建文件用于随机写。
 * 语义：文件不存在则创建；已存在则保持原内容（断点续传需要），
 * 长度由 edwl_file_truncate() 显式设定。
 * 返回 0 成功，-1 失败。
 */
int edwl_file_open_write(const char *path, edwl_file_t *out);

/* 以只读方式打开已存在的文件（完整性校验阶段用）。0 成功，-1 失败 */
int edwl_file_open_read(const char *path, edwl_file_t *out);

/* 把文件长度设为 size 字节（等价 ftruncate），预分配用。0 成功，-1 失败 */
int edwl_file_truncate(edwl_file_t fd, uint64_t size);

/* 读取当前文件长度。0 成功，-1 失败 */
int edwl_file_size(edwl_file_t fd, uint64_t *out);

/*
 * 定位写（等价 pwrite）：从 offset 处写 count 字节。
 * 返回实际写入字节数（可能小于 count），失败返回 -1。
 */
int64_t edwl_file_pwrite(edwl_file_t fd, const void *buf, size_t count,
			 uint64_t offset);

/* 定位读（等价 pread）：返回实际读到的字节数，0 表示已到文件尾，-1 失败 */
int64_t edwl_file_pread(edwl_file_t fd, void *buf, size_t count,
			uint64_t offset);

/* 关闭句柄。0 成功，-1 失败 */
int edwl_file_close(edwl_file_t fd);

/*
 * 列出目录下的普通文件名（不含路径），对每个名字回调一次；
 * 回调返回非 0 则提前停止。0 成功，-1 失败（目录不存在/无权限）。
 */
int edwl_dir_list(const char *dir, int (*fn)(const char *name, void *ctx),
		  void *ctx);

#endif /* EDWL_PLATFORM_FILE_IO_H */
