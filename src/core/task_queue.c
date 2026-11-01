/*
 * edwl - 线程安全任务队列实现
 *
 * 单链表 + 互斥量 + 条件变量。close 是"优雅收尾"：已入队的照常被取走，
 * 取空了才让 pop 返回失败，消费者据此退出。
 */
#include <stdlib.h>

#include <edwl/core/task_queue.h>
#include <edwl/platform/platform.h>

struct wgetx_tq_node {
	void *item;
	struct wgetx_tq_node *next;
};

struct wgetx_task_queue {
	struct wgetx_mutex *mtx;
	struct wgetx_cond *cond;
	struct wgetx_tq_node *head;
	struct wgetx_tq_node *tail;
	size_t count;
	int closed;
};

int wgetx_tq_create(struct wgetx_task_queue **out)
{
	struct wgetx_task_queue *q;

	if (!out)
		return -1;

	q = calloc(1, sizeof(*q));
	if (!q)
		return -1;

	if (wgetx_mutex_init(&q->mtx) != 0)
		goto fail_mutex;
	if (wgetx_cond_init(&q->cond) != 0)
		goto fail_cond;

	*out = q;
	return 0;

fail_cond:
	wgetx_mutex_destroy(q->mtx);
fail_mutex:
	free(q);
	return -1;
}

void wgetx_tq_destroy(struct wgetx_task_queue *q)
{
	struct wgetx_tq_node *node;

	if (!q)
		return;

	node = q->head;
	while (node) {
		struct wgetx_tq_node *next = node->next;

		free(node);
		node = next;
	}

	wgetx_cond_destroy(q->cond);
	wgetx_mutex_destroy(q->mtx);
	free(q);
}

int wgetx_tq_push(struct wgetx_task_queue *q, void *item)
{
	struct wgetx_tq_node *node;

	if (!q)
		return -1;

	node = calloc(1, sizeof(*node));
	if (!node)
		return -1;
	node->item = item;

	wgetx_mutex_lock(q->mtx);
	if (q->closed) {
		wgetx_mutex_unlock(q->mtx);
		free(node);
		return -1;
	}
	if (q->tail)
		q->tail->next = node;
	else
		q->head = node;
	q->tail = node;
	q->count++;
	wgetx_cond_signal(q->cond);
	wgetx_mutex_unlock(q->mtx);
	return 0;
}

int wgetx_tq_pop(struct wgetx_task_queue *q, void **item)
{
	struct wgetx_tq_node *node;

	if (!q || !item)
		return -1;

	wgetx_mutex_lock(q->mtx);
	while (q->count == 0 && !q->closed)
		wgetx_cond_wait(q->cond, q->mtx);

	/* 走到这里只可能是"已关闭且取空"，或者 cond 出错 */
	if (q->count == 0) {
		wgetx_mutex_unlock(q->mtx);
		return -1;
	}

	node = q->head;
	q->head = node->next;
	if (!q->head)
		q->tail = NULL;
	q->count--;
	wgetx_mutex_unlock(q->mtx);

	*item = node->item;
	free(node);
	return 0;
}

int wgetx_tq_try_pop(struct wgetx_task_queue *q, void **item)
{
	struct wgetx_tq_node *node;

	if (!q || !item)
		return -1;

	wgetx_mutex_lock(q->mtx);
	if (q->count == 0) {
		wgetx_mutex_unlock(q->mtx);
		return 0;
	}
	node = q->head;
	q->head = node->next;
	if (!q->head)
		q->tail = NULL;
	q->count--;
	wgetx_mutex_unlock(q->mtx);

	*item = node->item;
	free(node);
	return 1;
}

size_t wgetx_tq_size(struct wgetx_task_queue *q)
{
	size_t n;

	if (!q)
		return 0;
	wgetx_mutex_lock(q->mtx);
	n = q->count;
	wgetx_mutex_unlock(q->mtx);
	return n;
}

void wgetx_tq_close(struct wgetx_task_queue *q)
{
	if (!q)
		return;
	wgetx_mutex_lock(q->mtx);
	q->closed = 1;
	wgetx_cond_broadcast(q->cond);
	wgetx_mutex_unlock(q->mtx);
}

int wgetx_tq_closed(struct wgetx_task_queue *q)
{
	int closed;

	if (!q)
		return 1;
	wgetx_mutex_lock(q->mtx);
	closed = q->closed;
	wgetx_mutex_unlock(q->mtx);
	return closed;
}
