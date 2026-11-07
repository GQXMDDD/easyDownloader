/*
 * edwl - 动态字节缓冲区
 *
 * 只做一件事：自动扩容的连续内存块，URL 拼接、头部拼装用。
 * 不加锁，谁用谁自己保证串行。
 */
#ifndef EDWL_UTIL_BUFFER_H
#define EDWL_UTIL_BUFFER_H

#include <stddef.h>

struct edwl_buf {
	char *data;
	size_t len;
	size_t cap;
};

/* 初始化并预分配 cap 字节（cap 可为 0）。0 成功，-1 失败 */
int edwl_buf_init(struct edwl_buf *b, size_t cap);

/* 释放内部内存，结构体本身归零 */
void edwl_buf_free(struct edwl_buf *b);

/* 保证还能再写 need 字节（含结尾 0）。0 成功，-1 失败 */
int edwl_buf_reserve(struct edwl_buf *b, size_t need);

int edwl_buf_append(struct edwl_buf *b, const void *data, size_t len);
int edwl_buf_append_str(struct edwl_buf *b, const char *s);
int edwl_buf_appendf(struct edwl_buf *b, const char *fmt, ...);

/* 清空内容但保留分配，结尾补 0 */
void edwl_buf_reset(struct edwl_buf *b);

/* 返回以 0 结尾的内容指针（内容为空也有 0） */
char *edwl_buf_cstr(struct edwl_buf *b);

#endif /* EDWL_UTIL_BUFFER_H */
