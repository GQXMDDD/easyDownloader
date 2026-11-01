/*
 * edwl - Win32 平台：控制台、进度、Ctrl-C
 *
 * 中文控制在两个地方，都只影响本进程/本控制台窗口，退出即还原：
 *  1. 给控制台打开 ENABLE_VIRTUAL_TERMINAL_PROCESSING（失败就退化为纯文本）
 *  2. SetConsoleCtrlHandler 接管 Ctrl-C，用来保存断点
 *
 * 编码问题的处理方式是"绕开代码页"而不是改代码页：
 *   - 输出目标是控制台 → UTF-8 转 UTF-16 后走 WriteConsoleW，
 *     不管当前 chcp 是 936 还是 65001 都能正确显示中文；
 *   - 输出是重定向（文件/管道）→ 原样写 UTF-8 字节，
 *     下游按 UTF-8 读即可（这也是 Unix 工具的通用约定）。
 * 这样既不会把控制台代码页改成 65001（那会影响同窗口的其他程序），
 * 也不需要用户先 chcp 65001。
 */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <edwl/platform/platform.h>

#include "../console_fmt.h"

static int console_ansi;
static int console_tty;
static int progress_active;
static DWORD saved_out_mode;
static DWORD saved_err_mode;
static int have_saved_mode;
static volatile LONG interrupted;

static BOOL WINAPI ctrl_handler(DWORD type)
{
	if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
		InterlockedExchange(&interrupted, 1);
		return TRUE;
	}
	return FALSE;
}

static int handle_is_console(HANDLE h)
{
	DWORD mode;

	return h != INVALID_HANDLE_VALUE && h != NULL &&
	       GetConsoleMode(h, &mode);
}

/* 把 UTF-8 文本按宽字符写进控制台，绕开代码页转换 */
static void write_console_wide(HANDLE h, const char *utf8)
{
	WCHAR small[512];
	WCHAR *wide = small;
	DWORD written = 0;
	int need;

	need = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
	if (need <= 1)
		return;
	if (need > (int)(sizeof(small) / sizeof(small[0]))) {
		wide = malloc(sizeof(WCHAR) * (size_t)need);
		if (!wide)
			return;
	}
	if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, need) == need)
		WriteConsoleW(h, wide, (DWORD)(need - 1), &written, NULL);
	if (wide != small)
		free(wide);
}

static void write_text(DWORD std_handle, FILE *stream, const char *text)
{
	HANDLE h = GetStdHandle(std_handle);

	if (handle_is_console(h)) {
		write_console_wide(h, text);
		return;
	}
	/* 重定向：写原始 UTF-8 字节 */
	fputs(text, stream);
	fflush(stream);
}

static int enable_vt(DWORD std_handle, DWORD *saved)
{
	HANDLE h = GetStdHandle(std_handle);
	DWORD mode;

	if (!handle_is_console(h))
		return 0;
	if (!GetConsoleMode(h, &mode))
		return 0;
	if (!SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING |
				   ENABLE_PROCESSED_OUTPUT))
		return 0;
	*saved = mode;
	return 1;
}

int wgetx_console_init(void)
{
	int ok_out, ok_err;

	console_tty = handle_is_console(GetStdHandle(STD_ERROR_HANDLE)) ? 1 : 0;
	progress_active = 0;

	ok_out = enable_vt(STD_OUTPUT_HANDLE, &saved_out_mode);
	ok_err = enable_vt(STD_ERROR_HANDLE, &saved_err_mode);
	have_saved_mode = 1;

	console_ansi = (ok_out || ok_err) ? 1 : 0;
	if (!console_tty)
		console_ansi = 0;
	return console_ansi;
}

void wgetx_console_shutdown(void)
{
	HANDLE h;

	if (progress_active) {
		fputs("\r\033[K", stderr);
		fflush(stderr);
		progress_active = 0;
	}

	if (have_saved_mode) {
		h = GetStdHandle(STD_OUTPUT_HANDLE);
		if (handle_is_console(h))
			SetConsoleMode(h, saved_out_mode);
		h = GetStdHandle(STD_ERROR_HANDLE);
		if (handle_is_console(h))
			SetConsoleMode(h, saved_err_mode);
		have_saved_mode = 0;
	}
	console_ansi = 0;
}

int wgetx_console_ansi(void)
{
	return console_ansi;
}

int wgetx_console_tty(void)
{
	return console_tty;
}

int wgetx_console_width(void)
{
	HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
	CONSOLE_SCREEN_BUFFER_INFO info;
	int width;

	/* 重定向到文件时没有"宽度"概念，给一个足够宽的值免得进度行被截断 */
	if (!console_tty)
		return 120;

	if (!handle_is_console(h))
		return 80;
	if (!GetConsoleScreenBufferInfo(h, &info))
		return 80;

	width = (int)(info.srWindow.Right - info.srWindow.Left + 1);
	if (width <= 0)
		return 80;
	return width;
}

void wgetx_console_write(const char *text)
{
	if (!text)
		return;
	write_text(STD_OUTPUT_HANDLE, stdout, text);
}

void wgetx_console_write_err(const char *text)
{
	if (!text)
		return;
	if (progress_active && console_ansi) {
		fputs("\r\033[K", stderr);
		fflush(stderr);
		progress_active = 0;
	}
	write_text(STD_ERROR_HANDLE, stderr, text);
}

void wgetx_console_progress_draw(const struct wgetx_progress *p)
{
	char line[256];
	size_t len;

	if (!p)
		return;

	/* 进度行全是 ASCII，直接写字节即可，不受代码页影响 */
	len = wgetx_console_format_progress(p, line, sizeof(line),
					    wgetx_console_width());
	if (len == 0)
		return;

	if (console_ansi) {
		fprintf(stderr, "\r\033[K%s", line);
		progress_active = 1;
	} else {
		fprintf(stderr, "%s\n", line);
	}
	fflush(stderr);
}

void wgetx_console_progress_end(void)
{
	if (!progress_active)
		return;
	fputs("\r\033[K", stderr);
	fflush(stderr);
	progress_active = 0;
}

int wgetx_console_install_interrupt(void)
{
	if (!SetConsoleCtrlHandler(ctrl_handler, TRUE))
		return -1;
	return 0;
}

int wgetx_interrupt_requested(void)
{
	return interrupted != 0;
}

void wgetx_interrupt_reset(void)
{
	InterlockedExchange(&interrupted, 0);
}
