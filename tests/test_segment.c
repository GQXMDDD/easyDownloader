/*
 * edwl - 分片模型测试
 */
#include <edwl/core/segment.h>

#include "test_util.h"

#define MIB (1024ULL * 1024ULL)

static void test_piece_size(void)
{
	uint64_t total = 100ULL * MIB;

	/* 8 路并发 → 目标每路两片 → 16 片 */
	CHECK_INT(edwl_segment_piece_size(total, 8, MIB, 64 * MIB), total / 16);
	/* 下限生效：20000/(8*2)=1250 小于下限 4096 */
	CHECK_INT(edwl_segment_piece_size(20000, 8, 4096, MIB), 4096);
	/* 上限生效 */
	CHECK_INT(edwl_segment_piece_size(1ULL << 40, 1, 1024, MIB), MIB);
	/* 总长为 0 时退化成 1 字节 */
	CHECK_INT(edwl_segment_piece_size(0, 8, 1, 2), 1);
}

static void test_split_even(void)
{
	struct edwl_segment_map m;
	size_t i;

	CHECK_INT(edwl_segment_map_init(&m, 1000, 100, 0), 0);
	CHECK_INT(m.count, 10);
	CHECK_INT(m.segs[0].begin, 0);
	CHECK_INT(m.segs[0].end, 99);
	CHECK_INT(m.segs[9].end, 999);

	for (i = 1; i < m.count; i++)
		CHECK_INT(m.segs[i].begin, m.segs[i - 1].end + 1);

	edwl_segment_map_free(&m);
}

static void test_split_uneven(void)
{
	struct edwl_segment_map m;

	/* 1000 / 300 = 3.33 → 4 片，每片 250，最后一片吃掉余数 */
	CHECK_INT(edwl_segment_map_init(&m, 1000, 300, 0), 0);
	CHECK_INT(m.count, 4);
	CHECK_INT(m.segs[0].end, 249);
	CHECK_INT(m.segs[3].begin, 750);
	CHECK_INT(m.segs[3].end, 999);
	edwl_segment_map_free(&m);
}

static void test_split_capped(void)
{
	struct edwl_segment_map m;

	CHECK_INT(edwl_segment_map_init(&m, 1000, 1, 5), 0);
	CHECK_INT(m.count, 5);
	CHECK_INT(m.segs[4].end, 999);
	edwl_segment_map_free(&m);
}

static void test_progress(void)
{
	struct edwl_segment_map m;

	CHECK_INT(edwl_segment_map_init(&m, 1000, 100, 0), 0);
	CHECK_INT(edwl_segment_map_done(&m), 0);
	CHECK(!edwl_segment_map_all_done(&m));

	/* 第一片写完 */
	m.segs[0].pos = 100;
	m.segs[0].state = EDWL_SEG_DONE;
	CHECK_INT(edwl_segment_map_done(&m), 100);
	CHECK_INT(edwl_segment_map_done_count(&m), 1);
	CHECK_INT(edwl_segment_remaining(&m.segs[0]), 0);
	CHECK(edwl_segment_complete(&m.segs[0]));

	/* 第二片写一半 */
	m.segs[1].pos = 150;
	CHECK_INT(edwl_segment_map_done(&m), 150);
	CHECK_INT(edwl_segment_remaining(&m.segs[1]), 50);

	edwl_segment_map_free(&m);
}

static void test_all_done(void)
{
	struct edwl_segment_map m;
	size_t i;

	CHECK_INT(edwl_segment_map_init(&m, 1000, 250, 0), 0);
	for (i = 0; i < m.count; i++) {
		m.segs[i].pos = m.segs[i].end + 1;
		m.segs[i].state = EDWL_SEG_DONE;
	}
	CHECK(edwl_segment_map_all_done(&m));
	CHECK_INT(edwl_segment_map_done(&m), 1000);
	CHECK_INT(edwl_segment_map_done_count(&m), (long long)m.count);
	edwl_segment_map_free(&m);
}

static void test_release_active(void)
{
	struct edwl_segment_map m;

	CHECK_INT(edwl_segment_map_init(&m, 100, 50, 0), 0);
	m.segs[0].state = EDWL_SEG_ACTIVE;
	m.segs[0].pos = 10;
	edwl_segment_map_release_active(&m);
	CHECK_INT(m.segs[0].state, EDWL_SEG_PENDING);
	CHECK_INT(m.segs[0].pos, 10);
	edwl_segment_map_free(&m);
}

int main(void)
{
	test_piece_size();
	test_split_even();
	test_split_uneven();
	test_split_capped();
	test_progress();
	test_all_done();
	test_release_active();
	return test_finish("test_segment");
}
