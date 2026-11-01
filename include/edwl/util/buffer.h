/*
 * edwl - 动态字节缓冲区
 *
 * 只做一件事：自动扩容的连续内存块，URL 拼接、头部拼装用。
 * 不加锁，谁用谁自己保证串行。
 */
#ifndef WGETX_UTIL_BUFFER_H
#define WGETX_UTIL_BUFFER_H

#include <stddef.h>

struct wgetx_buf {
	char *data;
	size_t len;
	size_t cap;
};

/* 初始化并预分配 cap 字节（cap 可为 0）。0 成功，-1 失败 */
int wgetx_buf_init(struct wgetx_buf *b, size_t cap);

/* 释放内部内存，结构体本身归零 */
void wgetx_buf_free(struct wgetx_buf *b);

/* 保证还能再写 need 字节（含结尾 0）。0 成功，-1 失败 */
int wgetx_buf_reserve(struct wgetx_buf *b, size_t need);

int wgetx_buf_append(struct wgetx_buf *b, const void *data, size_t len);
int wgetx_buf_append_str(struct wgetx_buf *b, const char *s);
int wgetx_buf_appendf(struct wgetx_buf *b, const char *fmt, ...);

/* 清空内容但保留分配，结尾补 0 */
void wgetx_buf_reset(struct wgetx_buf *b);

/* 返回以 0 结尾的内容指针（内容为空也有 0） */
char *wgetx_buf_cstr(struct wgetx_buf *b);

#endif /* WGETX_UTIL_BUFFER_H */
