/*
 * edwl - 日志实现
 *
 * 日志统一走 stderr：进度行也在 stderr 上，平台层会在打印前把进度行清掉，
 * 这样日志不会和进度条糊在一起。
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <edwl/platform/platform.h>
#include <edwl/util/log.h>

static int log_threshold = EDWL_LOG_INFO;

void edwl_log_init(int level)
{
	if (level < EDWL_LOG_ERROR)
		level = EDWL_LOG_ERROR;
	if (level > EDWL_LOG_DEBUG)
		level = EDWL_LOG_DEBUG;
	log_threshold = level;
}

void edwl_log_shutdown(void)
{
	log_threshold = EDWL_LOG_ERROR;
}

int edwl_log_threshold(void)
{
	return log_threshold;
}

const char *edwl_log_level_name(int level)
{
	switch (level) {
	case EDWL_LOG_ERROR:
		return "error";
	case EDWL_LOG_WARN:
		return "warn";
	case EDWL_LOG_INFO:
		return "info";
	case EDWL_LOG_DEBUG:
		return "debug";
	default:
		return "log";
	}
}

void edwl_log_msg(int level, const char *fmt, ...)
{
	char body[512];
	char line[600];
	va_list ap;
	int n;

	if (level > log_threshold || !fmt)
		return;

	va_start(ap, fmt);
	vsnprintf(body, sizeof(body), fmt, ap);
	va_end(ap);

	n = snprintf(line, sizeof(line), "edwl: %s: %s\n",
		     edwl_log_level_name(level), body);
	if (n < 0)
		return;
	if ((size_t)n >= sizeof(line))
		line[sizeof(line) - 2] = '\n';

	edwl_console_write_err(line);
}
