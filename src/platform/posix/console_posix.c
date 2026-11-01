/*
 * edwl - POSIX 平台：控制台、进度、Ctrl-C
 *
 * 进度走 stderr（和 wget 一致），这样 stdout 重定向到文件时干净。
 * 有 ANSI 就原地覆盖；没有（重定向、dumb 终端）就一行一条。
 */
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <edwl/platform/platform.h>

#include "../console_fmt.h"

static int console_ansi;
static int console_tty;
static int progress_active;
static volatile sig_atomic_t interrupted;

static void on_sigint(int sig)
{
	(void)sig;
	interrupted = 1;
}

int wgetx_console_init(void)
{
	console_tty = isatty(STDERR_FILENO) ? 1 : 0;
	console_ansi = console_tty;
	progress_active = 0;
	return console_ansi;
}

void wgetx_console_shutdown(void)
{
	if (progress_active) {
		fputs("\r\033[K", stderr);
		fflush(stderr);
		progress_active = 0;
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
	struct winsize ws;
	int fd = console_tty ? STDERR_FILENO : STDOUT_FILENO;

	/* 重定向到文件时没有"宽度"概念，给一个足够宽的值免得进度行被截断 */
	if (!console_tty)
		return 120;

	if (ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
		return (int)ws.ws_col;
	return 80;
}

void wgetx_console_write(const char *text)
{
	if (!text)
		return;
	fputs(text, stdout);
	fflush(stdout);
}

void wgetx_console_write_err(const char *text)
{
	if (!text)
		return;
	if (progress_active && console_ansi) {
		fputs("\r\033[K", stderr);
		progress_active = 0;
	}
	fputs(text, stderr);
	fflush(stderr);
}

void wgetx_console_progress_draw(const struct wgetx_progress *p)
{
	char line[256];
	size_t len;

	if (!p)
		return;

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
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_sigint;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	if (sigaction(SIGINT, &sa, NULL) != 0)
		return -1;
	if (sigaction(SIGTERM, &sa, NULL) != 0)
		return -1;
	return 0;
}

int wgetx_interrupt_requested(void)
{
	return interrupted != 0;
}

void wgetx_interrupt_reset(void)
{
	interrupted = 0;
}
