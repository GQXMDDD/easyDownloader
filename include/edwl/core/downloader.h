/*
 * edwl - 下载器主控
 *
 * 职责：探测 → 切片 → 起线程池 → 汇总进度 → 断点保存 → 校验。
 * 所有平台差异都在 platform/ 后面，本层只用标准 C。
 */
#ifndef WGETX_CORE_DOWNLOADER_H
#define WGETX_CORE_DOWNLOADER_H

#include <stddef.h>
#include <stdint.h>

struct wgetx_download_opts {
	const char *output;	 /* 输出路径；NULL 则由 URL 推断 */
	const char *const *urls; /* 至少一个，多个视为镜像 */
	size_t url_count;

	unsigned threads;	 /* 并发分片数，默认 8 */
	uint64_t min_piece_size; /* 单片下限，默认 1 MiB */
	uint64_t max_piece_size; /* 单片上限，默认 64 MiB */
	size_t max_pieces;

	unsigned retries;	     /* 单片重试次数，默认 3 */
	unsigned connect_timeout_ms; /* 默认 15000 */
	unsigned low_speed_time_ms;  /* 默认 30000，0 关闭 */
	uint64_t low_speed_limit;    /* 默认 1 字节/秒 */
	uint64_t max_speed;	     /* 全局限速，0 不限 */

	const char *expected_sha256; /* NULL 表示不校验 */
	const char *proxy;	     /* NULL 跟随环境变量；"" 强制直连 */

	int allow_resume;  /* 1 允许断点续传（默认） */
	int allow_range;   /* 1 允许分片（默认）；0 走单连接 */
	int show_progress; /* 1 画进度（默认，非终端时自动退化） */
	int quiet;	   /* 1 只输出错误 */
};

struct wgetx_download_result {
	uint64_t bytes;	 /* 最终文件大小 */
	uint64_t total;	 /* 服务器给的总长 */
	unsigned pieces; /* 分片数 */
	int resumed;	 /* 1 表示这次是续传 */
	int range_used;	 /* 1 表示用了分片 */
	int verified;	 /* 1 校验通过，0 未要求校验，-1 不匹配 */
	char sha256[65]; /* 实际算出的哈希 */
};

/* 填默认值 */
void wgetx_download_opts_default(struct wgetx_download_opts *o);

/* 执行一次下载。返回 WGETX_EXIT_* 中的值 */
int wgetx_download_run(const struct wgetx_download_opts *o,
		       struct wgetx_download_result *r);

#endif /* WGETX_CORE_DOWNLOADER_H */
