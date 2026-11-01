/*
 * edwl - 分片模型（Piece / Segment）
 *
 * 一个文件被切成若干段，每段闭区间 [begin, end]。pos 表示"该段已落盘的
 * 下一个字节"，因此断点续传只要把 pos 存下来，重连后从 pos 继续请求。
 *
 * 本文件是纯数据结构 + 纯函数，不加锁；并发保护由 downloader 负责。
 */
#ifndef WGETX_CORE_SEGMENT_H
#define WGETX_CORE_SEGMENT_H

#include <stddef.h>
#include <stdint.h>

enum wgetx_seg_state {
	WGETX_SEG_PENDING = 0, /* 还没人领 */
	WGETX_SEG_ACTIVE,      /* 有 worker 正在拉 */
	WGETX_SEG_DONE,	       /* 拉完了 */
};

struct wgetx_segment {
	uint64_t index;
	uint64_t begin; /* 闭区间起点 */
	uint64_t end;	/* 闭区间终点 */
	uint64_t pos;	/* 下一个要写的偏移，范围 [begin, end+1] */
	int state;
	unsigned attempts; /* 已失败重试次数 */
};

struct wgetx_segment_map {
	struct wgetx_segment *segs;
	size_t count;
	uint64_t total; /* 文件总长度 */
};

/*
 * 按 total 与期望连接数算出单片大小。
 * 返回 0 表示参数非法（total 为 0 时返回 1，表示"不分片"）。
 */
uint64_t wgetx_segment_piece_size(uint64_t total, size_t conns,
				  uint64_t min_piece, uint64_t max_piece);

/*
 * 按 total 切分。piece_size 为 0 时自动算。
 * max_pieces 限制分片数上限（避免小文件切太碎）。
 * 0 成功，-1 失败（内存不足或 total 为 0）。
 */
int wgetx_segment_map_init(struct wgetx_segment_map *m, uint64_t total,
			   uint64_t piece_size, size_t max_pieces);

void wgetx_segment_map_free(struct wgetx_segment_map *m);

/* 该段还剩多少字节没落盘 */
uint64_t wgetx_segment_remaining(const struct wgetx_segment *s);

/* 该段是否已经写完 */
int wgetx_segment_complete(const struct wgetx_segment *s);

/* 全文件已落盘字节数 */
uint64_t wgetx_segment_map_done(const struct wgetx_segment_map *m);

/* 已完成的段数 */
size_t wgetx_segment_map_done_count(const struct wgetx_segment_map *m);

/* 全部完成返回 1 */
int wgetx_segment_map_all_done(const struct wgetx_segment_map *m);

/* 把状态为 ACTIVE 的段退回 PENDING（收尾、中断时用），pos 保留 */
void wgetx_segment_map_release_active(struct wgetx_segment_map *m);

#endif /* WGETX_CORE_SEGMENT_H */
