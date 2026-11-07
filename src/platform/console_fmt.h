/*
 * edwl - 进度行文本格式化（平台无关，仅供平台层内部使用）
 *
 * 两个平台的 console 实现只差"怎么把字符写到终端"，格式化逻辑放这里，
 * 免得 posix/win32 各抄一份。
 */
#ifndef EDWL_CONSOLE_FMT_H
#define EDWL_CONSOLE_FMT_H

#include <stddef.h>

#include <edwl/platform/console.h>

/*
 * 把进度快照渲染成一行纯文本（不含 \r / \n）。
 * width 是终端可用列数。返回写入的字符数（不含结尾 0）。
 */
size_t edwl_console_format_progress(const struct edwl_progress *p, char *buf,
				    size_t size, int width);

#endif /* EDWL_CONSOLE_FMT_H */
