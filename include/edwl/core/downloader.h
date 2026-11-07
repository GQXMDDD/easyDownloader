/*
 * edwl - 下载器主控
 *
 * 职责：探测 → 切片 → 起线程池 → 汇总进度 → 断点保存 → 校验。
 * 所有平台差异都在 platform/ 后面，本层只用标准 C。
 */
#ifndef EDWL_CORE_DOWNLOADER_H
#define EDWL_CORE_DOWNLOADER_H

#include <stddef.h>
#include <stdint.h>

struct edwl_blacklist;

struct edwl_download_opts {
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
	const char *user_agent;	     /* NULL 用内建 UA */
	const char *resume_ext;	     /* 断点文件后缀，默认 .edwl */
	const char *output_dir;	     /* 未指定输出路径时的默认目录 */
	const struct edwl_blacklist *blacklist; /* NULL = 不过滤 */
	int retry_backoff_fixed;		/* 1 = 固定间隔退避 */
	int dynamic_threads;			/* 1 = 动态并发 */
	int superspeed_enable;			/* 1 = 开启倍率加速 */
	int speedup_auto;			/* 1 = 倍率自动 */
	double speed_multiplier;		/* 固定倍率 */

	int allow_resume;  /* 1 允许断点续传（默认） */
	int allow_range;   /* 1 允许分片（默认）；0 走单连接 */
	int show_progress; /* 1 画进度（默认，非终端时自动退化） */
	int quiet;	   /* 1 只输出错误 */
};

struct edwl_download_result {
	uint64_t bytes;	 /* 最终文件大小 */
	uint64_t total;	 /* 服务器给的总长 */
	unsigned pieces; /* 分片数 */
	int resumed;	 /* 1 表示这次是续传 */
	int range_used;	 /* 1 表示用了分片 */
	int verified;	 /* 1 校验通过，0 未要求校验，-1 不匹配 */
	char sha256[65]; /* 实际算出的哈希 */

	/* 用时统计（毫秒） */
	uint64_t probe_ms;     /* 探测阶段 */
	uint64_t transfer_ms;  /* 传输阶段 */
	uint64_t elapsed_ms;   /* 探测 + 传输 */
	uint64_t speed_bps;    /* 平均速度（按传输耗时算） */
	uint64_t piece_min_us; /* 最快的一片（微秒） */
	uint64_t piece_max_us; /* 最慢的一片 */
	uint64_t piece_avg_us; /* 平均每片 */
};

/* 填默认值 */
void edwl_download_opts_default(struct edwl_download_opts *o);

/* 执行一次下载。返回 EDWL_EXIT_* 中的值 */
int edwl_download_run(const struct edwl_download_opts *o,
		      struct edwl_download_result *r);

#endif /* EDWL_CORE_DOWNLOADER_H */
