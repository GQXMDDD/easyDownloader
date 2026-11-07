/*
 * edwl - 日志
 *
 * 级别（对应配置文件 [Logging] Level）：
 *   off   = 0  什么都不输出
 *   low   = 3  只输出错误、警告和结果摘要
 *   high  = 5  再加进度/分片信息
 *   max   = 6  再加 HTTP 层的调试细节
 *
 * 输出（对应 [Logging] Output）：console / file / both。
 * console 时：结果走 stdout（方便重定向），其余走 stderr。
 *
 * 初始化之前也能安全调用（默认 console + info
 * 级别），这样"配置文件里有语法错误" 这类早期问题也能报出来。
 */
#ifndef EDWL_UTIL_LOG_H
#define EDWL_UTIL_LOG_H

#include <stddef.h>

enum edwl_log_level {
	EDWL_LOG_OFF = 0,
	EDWL_LOG_ERROR = 1,
	EDWL_LOG_WARN = 2,
	EDWL_LOG_RESULT = 3,
	EDWL_LOG_INFO = 4,
	EDWL_LOG_PROGRESS = 5,
	EDWL_LOG_DEBUG = 6
};

struct edwl_log_opts {
	int enabled; /* 0 = 全关 */
	int level;   /* 阈值，EDWL_LOG_* */
	int to_console;
	int to_file;
	const char *file_path;	 /* 仅 to_file 时用 */
	const char *time_format; /* strftime 格式，NULL 用内置 */
	const char *retention;	 /* "1000L" 按行 / "30T" 按天 */
};

/* 0 成功；-1 表示日志文件打不开（console 仍然可用） */
int edwl_log_init(const struct edwl_log_opts *opts);
void edwl_log_shutdown(void);

/* "off"/"low"/"high"/"max" → 阈值；不认识的返回 -1 */
int edwl_log_level_from_name(const char *name);
const char *edwl_log_level_name(int level);

/* 当前阈值 */
int edwl_log_threshold(void);

/* 该级别会不会被输出（进度条等 UI 用它判断） */
int edwl_log_allows(int level);

void edwl_log_msg(int level, const char *fmt, ...);

#endif /* EDWL_UTIL_LOG_H */
