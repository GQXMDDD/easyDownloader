/*
 * edwl - 动态缓冲区实现
 *
 * 规则：data 永远以 0 结尾（容量上多吃一个字节），这样拼接时随时可以当
 * C 字符串用，不用反复算长度。
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <edwl/util/buffer.h>

int wgetx_buf_init(struct wgetx_buf *b, size_t cap)
{
	if (!b)
		return -1;

	b->data = NULL;
	b->len = 0;
	b->cap = 0;
	if (cap == 0)
		cap = 64;

	b->data = malloc(cap);
	if (!b->data)
		return -1;

	b->data[0] = '\0';
	b->cap = cap;
	return 0;
}

void wgetx_buf_free(struct wgetx_buf *b)
{
	if (!b)
		return;
	free(b->data);
	b->data = NULL;
	b->len = 0;
	b->cap = 0;
}

int wgetx_buf_reserve(struct wgetx_buf *b, size_t need)
{
	size_t want, cap_new;
	char *grown;

	if (!b || !b->data)
		return -1;
	want = b->len + need + 1;
	if (want <= b->cap)
		return 0;

	cap_new = b->cap;
	while (cap_new < want) {
		if (cap_new > (size_t)-1 / 2)
			return -1;
		cap_new *= 2;
	}

	grown = realloc(b->data, cap_new);
	if (!grown)
		return -1;

	b->data = grown;
	b->cap = cap_new;
	return 0;
}

int wgetx_buf_append(struct wgetx_buf *b, const void *data, size_t len)
{
	if (!b || (!data && len > 0))
		return -1;
	if (wgetx_buf_reserve(b, len) != 0)
		return -1;

	if (len > 0)
		memcpy(b->data + b->len, data, len);
	b->len += len;
	b->data[b->len] = '\0';
	return 0;
}

int wgetx_buf_append_str(struct wgetx_buf *b, const char *s)
{
	if (!s)
		return 0;
	return wgetx_buf_append(b, s, strlen(s));
}

int wgetx_buf_appendf(struct wgetx_buf *b, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (!b || !fmt)
		return -1;

	va_start(ap, fmt);
	n = vsnprintf(b->data + b->len, b->cap - b->len, fmt, ap);
	va_end(ap);
	if (n < 0)
		return -1;
	if ((size_t)n < b->cap - b->len) {
		b->len += (size_t)n;
		return 0;
	}

	/* 空间不够：按需要量扩一次再重来 */
	if (wgetx_buf_reserve(b, (size_t)n) != 0)
		return -1;

	va_start(ap, fmt);
	n = vsnprintf(b->data + b->len, b->cap - b->len, fmt, ap);
	va_end(ap);
	if (n < 0)
		return -1;

	b->len += (size_t)n;
	return 0;
}

void wgetx_buf_reset(struct wgetx_buf *b)
{
	if (!b || !b->data)
		return;
	b->len = 0;
	b->data[0] = '\0';
}

char *wgetx_buf_cstr(struct wgetx_buf *b)
{
	if (!b || !b->data)
		return NULL;
	b->data[b->len] = '\0';
	return b->data;
}
