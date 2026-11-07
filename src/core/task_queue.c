/*
 * edwl - 线程安全任务队列实现
 *
 * 单链表 + 互斥量 + 条件变量。close 是"优雅收尾"：已入队的照常被取走，
 * 取空了才让 pop 返回失败，消费者据此退出。
 */
#include <stdlib.h>

#include <edwl/core/task_queue.h>
#include <edwl/platform/platform.h>

struct edwl_tq_node {
	void *item;
	struct edwl_tq_node *next;
};

struct edwl_task_queue {
	struct edwl_mutex *mtx;
	struct edwl_cond *cond;
	struct edwl_tq_node *head;
	struct edwl_tq_node *tail;
	size_t count;
	int closed;
};

int edwl_tq_create(struct edwl_task_queue **out)
{
	struct edwl_task_queue *q;

	if (!out)
		return -1;

	q = calloc(1, sizeof(*q));
	if (!q)
		return -1;

	if (edwl_mutex_init(&q->mtx) != 0)
		goto fail_mutex;
	if (edwl_cond_init(&q->cond) != 0)
		goto fail_cond;

	*out = q;
	return 0;

fail_cond:
	edwl_mutex_destroy(q->mtx);
fail_mutex:
	free(q);
	return -1;
}

void edwl_tq_destroy(struct edwl_task_queue *q)
{
	struct edwl_tq_node *node;

	if (!q)
		return;

	node = q->head;
	while (node) {
		struct edwl_tq_node *next = node->next;

		free(node);
		node = next;
	}

	edwl_cond_destroy(q->cond);
	edwl_mutex_destroy(q->mtx);
	free(q);
}

int edwl_tq_push(struct edwl_task_queue *q, void *item)
{
	struct edwl_tq_node *node;

	if (!q)
		return -1;

	node = calloc(1, sizeof(*node));
	if (!node)
		return -1;
	node->item = item;

	edwl_mutex_lock(q->mtx);
	if (q->closed) {
		edwl_mutex_unlock(q->mtx);
		free(node);
		return -1;
	}
	if (q->tail)
		q->tail->next = node;
	else
		q->head = node;
	q->tail = node;
	q->count++;
	edwl_cond_signal(q->cond);
	edwl_mutex_unlock(q->mtx);
	return 0;
}

int edwl_tq_pop(struct edwl_task_queue *q, void **item)
{
	struct edwl_tq_node *node;

	if (!q || !item)
		return -1;

	edwl_mutex_lock(q->mtx);
	while (q->count == 0 && !q->closed)
		edwl_cond_wait(q->cond, q->mtx);

	/* 走到这里只可能是"已关闭且取空"，或者 cond 出错 */
	if (q->count == 0) {
		edwl_mutex_unlock(q->mtx);
		return -1;
	}

	node = q->head;
	q->head = node->next;
	if (!q->head)
		q->tail = NULL;
	q->count--;
	edwl_mutex_unlock(q->mtx);

	*item = node->item;
	free(node);
	return 0;
}

int edwl_tq_try_pop(struct edwl_task_queue *q, void **item)
{
	struct edwl_tq_node *node;

	if (!q || !item)
		return -1;

	edwl_mutex_lock(q->mtx);
	if (q->count == 0) {
		edwl_mutex_unlock(q->mtx);
		return 0;
	}
	node = q->head;
	q->head = node->next;
	if (!q->head)
		q->tail = NULL;
	q->count--;
	edwl_mutex_unlock(q->mtx);

	*item = node->item;
	free(node);
	return 1;
}

size_t edwl_tq_size(struct edwl_task_queue *q)
{
	size_t n;

	if (!q)
		return 0;
	edwl_mutex_lock(q->mtx);
	n = q->count;
	edwl_mutex_unlock(q->mtx);
	return n;
}

void edwl_tq_close(struct edwl_task_queue *q)
{
	if (!q)
		return;
	edwl_mutex_lock(q->mtx);
	q->closed = 1;
	edwl_cond_broadcast(q->cond);
	edwl_mutex_unlock(q->mtx);
}

int edwl_tq_closed(struct edwl_task_queue *q)
{
	int closed;

	if (!q)
		return 1;
	edwl_mutex_lock(q->mtx);
	closed = q->closed;
	edwl_mutex_unlock(q->mtx);
	return closed;
}
