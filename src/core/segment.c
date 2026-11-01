/*
 * edwl - 分片模型实现
 *
 * 切法：先按"每个 worker 大约两片"估算片大小，再夹在 [min, max] 之间。
 * 两片而不是一片，是有意留一点余量给动态均衡：谁先干完谁再领一片。
 * 最后一片吃掉除不尽的那点尾巴，保证 [0, total-1] 完全覆盖、不重叠。
 */
#include <stdlib.h>
#include <string.h>

#include <edwl/core/segment.h>

uint64_t wgetx_segment_piece_size(uint64_t total, size_t conns,
				  uint64_t min_piece, uint64_t max_piece)
{
	uint64_t want;

	if (total == 0)
		return 1;
	if (conns == 0)
		conns = 1;
	if (min_piece == 0)
		min_piece = 1;
	if (max_piece < min_piece)
		max_piece = min_piece;

	want = total / ((uint64_t)conns * 2ULL);
	if (want < min_piece)
		want = min_piece;
	if (want > max_piece)
		want = max_piece;
	if (want > total)
		want = total;
	if (want == 0)
		want = 1;
	return want;
}

int wgetx_segment_map_init(struct wgetx_segment_map *m, uint64_t total,
			   uint64_t piece_size, size_t max_pieces)
{
	struct wgetx_segment *segs;
	uint64_t begin = 0;
	uint64_t span;
	size_t count;
	size_t i;

	if (!m || total == 0)
		return -1;

	m->segs = NULL;
	m->count = 0;
	m->total = total;

	if (piece_size == 0)
		piece_size = 1;

	count = (size_t)((total + piece_size - 1) / piece_size);
	if (count == 0)
		count = 1;
	if (max_pieces > 0 && count > max_pieces)
		count = max_pieces;

	/* 按实际片数重算跨度，避免除不尽时最后一片过大 */
	span = total / (uint64_t)count;
	if (span == 0)
		span = 1;

	segs = calloc(count, sizeof(*segs));
	if (!segs)
		return -1;

	for (i = 0; i < count; i++) {
		uint64_t end =
		    (i + 1 == count) ? (total - 1) : (begin + span - 1);

		if (end < begin)
			end = begin;
		segs[i].index = (uint64_t)i;
		segs[i].begin = begin;
		segs[i].end = end;
		segs[i].pos = begin;
		segs[i].state = WGETX_SEG_PENDING;
		segs[i].attempts = 0;
		begin = end + 1;
	}

	m->segs = segs;
	m->count = count;
	return 0;
}

void wgetx_segment_map_free(struct wgetx_segment_map *m)
{
	if (!m)
		return;
	free(m->segs);
	m->segs = NULL;
	m->count = 0;
	m->total = 0;
}

uint64_t wgetx_segment_remaining(const struct wgetx_segment *s)
{
	if (!s)
		return 0;
	if (s->pos > s->end)
		return 0;
	return s->end - s->pos + 1;
}

int wgetx_segment_complete(const struct wgetx_segment *s)
{
	if (!s)
		return 0;
	return s->pos > s->end;
}

uint64_t wgetx_segment_map_done(const struct wgetx_segment_map *m)
{
	uint64_t done = 0;
	size_t i;

	if (!m || !m->segs)
		return 0;
	for (i = 0; i < m->count; i++)
		done += m->segs[i].pos - m->segs[i].begin;
	return done;
}

size_t wgetx_segment_map_done_count(const struct wgetx_segment_map *m)
{
	size_t n = 0;
	size_t i;

	if (!m || !m->segs)
		return 0;
	for (i = 0; i < m->count; i++) {
		if (m->segs[i].state == WGETX_SEG_DONE)
			n++;
	}
	return n;
}

int wgetx_segment_map_all_done(const struct wgetx_segment_map *m)
{
	size_t i;

	if (!m || !m->segs)
		return 0;
	for (i = 0; i < m->count; i++) {
		if (!wgetx_segment_complete(&m->segs[i]))
			return 0;
	}
	return 1;
}

void wgetx_segment_map_release_active(struct wgetx_segment_map *m)
{
	size_t i;

	if (!m || !m->segs)
		return;
	for (i = 0; i < m->count; i++) {
		if (m->segs[i].state == WGETX_SEG_ACTIVE)
			m->segs[i].state = WGETX_SEG_PENDING;
	}
}
