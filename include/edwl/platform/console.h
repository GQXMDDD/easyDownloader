/*
 * edwl - 平台抽象层：控制台、进度输出、中断标志
 *
 * 控制台是本程序唯一碰"环境"的地方（开 ANSI、转发 Ctrl-C）。
 * 全部是用户态行为，不写注册表、不改系统设置。
 */
#ifndef WGETX_PLATFORM_CONSOLE_H
#define WGETX_PLATFORM_CONSOLE_H

#include <stddef.h>
#include <stdint.h>

/* 进度快照：由核心层填好数字，平台层负责画 */
struct wgetx_progress {
	uint64_t done;	    /* 已完成字节 */
	uint64_t total;	    /* 总字节，0 表示未知 */
	uint64_t speed_bps; /* 瞬时速度，字节/秒 */
	unsigned active;    /* 正在传输的分片数 */
	unsigned segs_done;
	unsigned segs_total;
	uint64_t elapsed_ms;
};

/* 初始化控制台（Windows 下尝试打开 VT 处理）。返回 1 表示可用 ANSI，0 表示退化
 */
int wgetx_console_init(void);

/* 恢复控制台状态（关闭 VT、去掉残留进度行） */
void wgetx_console_shutdown(void);

/* 当前是否可用 ANSI 转义序列 */
int wgetx_console_ansi(void);

/* 输出是否为终端；重定向到文件时应关掉进度刷新 */
int wgetx_console_tty(void);

/* 终端宽度（字符数），取不到时返回 80 */
int wgetx_console_width(void);

/* 原样写一段文本到 stdout / stderr */
void wgetx_console_write(const char *text);
void wgetx_console_write_err(const char *text);

/* 画一行进度（stderr，覆盖式）；非终端下退化为按行输出 */
void wgetx_console_progress_draw(const struct wgetx_progress *p);

/* 清掉当前进度行，给后续日志腾地方 */
void wgetx_console_progress_end(void);

/* 安装 Ctrl-C 处理器。0 成功，-1 失败（不支持则忽略） */
int wgetx_console_install_interrupt(void);

/* 是否收到了中断请求（核心层在回调里轮询它） */
int wgetx_interrupt_requested(void);

/* 清除中断标志，便于测试重复使用 */
void wgetx_interrupt_reset(void);

/* 把字节数格式化成 "12.4 MiB" 这样的短串（摘要输出用） */
void wgetx_console_format_size(uint64_t bytes, char *buf, size_t size);

#endif /* WGETX_PLATFORM_CONSOLE_H */
