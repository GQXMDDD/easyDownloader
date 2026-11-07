/*
 * edwl - 日志
 *
 * 全部日志走 stderr，避免污染 -O - 时的 stdout 管道。
 */
#ifndef EDWL_UTIL_LOG_H
#define EDWL_UTIL_LOG_H

enum edwl_log_level {
	EDWL_LOG_ERROR = 0,
	EDWL_LOG_WARN,
	EDWL_LOG_INFO,
	EDWL_LOG_DEBUG,
};

/* 设定全局阈值，低于阈值的消息被丢弃 */
void edwl_log_init(int level);
void edwl_log_shutdown(void);
int edwl_log_threshold(void);

void edwl_log_msg(int level, const char *fmt, ...);

const char *edwl_log_level_name(int level);

#endif /* EDWL_UTIL_LOG_H */
