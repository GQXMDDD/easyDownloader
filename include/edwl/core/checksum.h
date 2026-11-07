/*
 * edwl - 哈希校验（SHA-256，纯 C 实现）
 *
 * 不引第三方加密库：项目只允许 C 和 libcurl，SHA-256 自己写五十行，
 * 省掉一个依赖也让静态链接更干净。
 *
 * 用途：多源下载后，整文件算一次哈希，确认拼起来的字节是对的。
 */
#ifndef EDWL_CORE_CHECKSUM_H
#define EDWL_CORE_CHECKSUM_H

#include <stddef.h>

#define EDWL_SHA256_HEX 65 /* 64 字符 + 结尾 0 */

/* 对内存块求 SHA-256，输出小写十六进制 */
void edwl_sha256_buf(const void *data, size_t len, char out[EDWL_SHA256_HEX]);

/* 对文件求 SHA-256（分块读，走平台 IO）。0 成功，-1 失败 */
int edwl_sha256_file(const char *path, char out[EDWL_SHA256_HEX]);

/* 忽略大小写比较两个十六进制串（允许右侧带空白）。1 相等 */
int edwl_hash_equals(const char *a, const char *b);

/* 是否是 64 位十六进制串（允许大小写） */
int edwl_hash_is_valid(const char *hex);

#endif /* EDWL_CORE_CHECKSUM_H */
