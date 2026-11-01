/*
 * edwl - 日志
 *
 * 全部日志走 stderr，避免污染 -O - 时的 stdout 管道。
 */
#ifndef WGETX_UTIL_LOG_H
#define WGETX_UTIL_LOG_H

enum wgetx_log_level {
	WGETX_LOG_ERROR = 0,
	WGETX_LOG_WARN,
	WGETX_LOG_INFO,
	WGETX_LOG_DEBUG,
};

/* 设定全局阈值，低于阈值的消息被丢弃 */
void wgetx_log_init(int level);
void wgetx_log_shutdown(void);
int wgetx_log_threshold(void);

void wgetx_log_msg(int level, const char *fmt, ...);

const char *wgetx_log_level_name(int level);

#endif /* WGETX_UTIL_LOG_H */
