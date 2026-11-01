/*
 * edwl - 线程安全任务队列（FIFO）
 *
 * 存的是 void *，谁压谁负责解释。close 之后不再接受 push，
 * 阻塞在 pop 上的消费者会立刻返回失败。
 */
#ifndef WGETX_CORE_TASK_QUEUE_H
#define WGETX_CORE_TASK_QUEUE_H

#include <stddef.h>

struct wgetx_task_queue;

/* 0 成功，-1 失败 */
int wgetx_tq_create(struct wgetx_task_queue **out);
void wgetx_tq_destroy(struct wgetx_task_queue *q);

/* 入队。队列已关闭或内存不足返回 -1 */
int wgetx_tq_push(struct wgetx_task_queue *q, void *item);

/* 阻塞取一个；队列关闭且为空时返回 -1 */
int wgetx_tq_pop(struct wgetx_task_queue *q, void **item);

/* 非阻塞取一个；空返回 0，取到返回 1，失败 -1 */
int wgetx_tq_try_pop(struct wgetx_task_queue *q, void **item);

size_t wgetx_tq_size(struct wgetx_task_queue *q);

/* 关闭：唤醒所有等待者，之后 push 失败 */
void wgetx_tq_close(struct wgetx_task_queue *q);

int wgetx_tq_closed(struct wgetx_task_queue *q);

#endif /* WGETX_CORE_TASK_QUEUE_H */
