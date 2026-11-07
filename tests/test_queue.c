/*
 * edwl - 任务队列测试
 */
#include <edwl/core/task_queue.h>

#include "test_util.h"

static void test_fifo(void)
{
	struct edwl_task_queue *q = NULL;
	void *item = NULL;
	int a = 1, b = 2, c = 3;

	CHECK_INT(edwl_tq_create(&q), 0);
	CHECK_INT(edwl_tq_push(q, &a), 0);
	CHECK_INT(edwl_tq_push(q, &b), 0);
	CHECK_INT(edwl_tq_push(q, &c), 0);
	CHECK_INT(edwl_tq_size(q), 3);

	CHECK_INT(edwl_tq_pop(q, &item), 0);
	CHECK(item == &a);
	CHECK_INT(edwl_tq_pop(q, &item), 0);
	CHECK(item == &b);
	CHECK_INT(edwl_tq_pop(q, &item), 0);
	CHECK(item == &c);
	CHECK_INT(edwl_tq_size(q), 0);

	edwl_tq_destroy(q);
}

static void test_try_pop_empty(void)
{
	struct edwl_task_queue *q = NULL;
	void *item = NULL;

	CHECK_INT(edwl_tq_create(&q), 0);
	CHECK_INT(edwl_tq_try_pop(q, &item), 0);

	CHECK_INT(edwl_tq_push(q, &item), 0);
	CHECK_INT(edwl_tq_try_pop(q, &item), 1);
	CHECK_INT(edwl_tq_try_pop(q, &item), 0);

	edwl_tq_destroy(q);
}

static void test_close(void)
{
	struct edwl_task_queue *q = NULL;
	void *item = NULL;
	int a = 1, b = 2;

	CHECK_INT(edwl_tq_create(&q), 0);
	CHECK_INT(edwl_tq_push(q, &a), 0);
	edwl_tq_close(q);
	CHECK_INT(edwl_tq_closed(q), 1);

	/* 关闭后仍能取走已排队的，取空后 pop 失败 */
	CHECK_INT(edwl_tq_pop(q, &item), 0);
	CHECK(item == &a);
	CHECK_INT(edwl_tq_pop(q, &item), -1);

	/* 关闭后 push 失败 */
	CHECK_INT(edwl_tq_push(q, &b), -1);

	edwl_tq_destroy(q);
}

int main(void)
{
	test_fifo();
	test_try_pop_empty();
	test_close();
	return test_finish("test_queue");
}
