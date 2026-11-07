/*
 * edwl - 线程池
 *
 * 固定 workers 个线程，从内部 task_queue 取活干；提交方压任务后可用
 * wait_idle() 等全部做完。所有任务都由同一个 job 函数处理，job 参数
 * 是不透明指针，解释权在提交方。
 *
 * 与 task_queue 的分工：task_queue 只管排队，thread_pool 管线程生命周期
 * 和"还有多少活没干完"的计数（含 worker 内部重新入队的重试任务）。
 */
#ifndef EDWL_CORE_THREAD_POOL_H
#define EDWL_CORE_THREAD_POOL_H

#include <stddef.h>

struct edwl_thread_pool;

typedef void (*edwl_pool_job_fn)(void *ctx, void *job);

/* 0 成功，-1 失败 */
int edwl_pool_create(struct edwl_thread_pool **out, size_t workers,
		     edwl_pool_job_fn fn, void *ctx);

/* 提交任务；池已停止返回 -1 */
int edwl_pool_submit(struct edwl_thread_pool *p, void *job);

/* 等到"已提交的任务全部执行完"（含任务内部再提交的）返回 */
int edwl_pool_wait_idle(struct edwl_thread_pool *p);

/* 未完成的任务数（排队 + 执行中） */
size_t edwl_pool_outstanding(struct edwl_thread_pool *p);

size_t edwl_pool_workers(struct edwl_thread_pool *p);

/* 关闭队列、join 所有线程并释放 */
void edwl_pool_destroy(struct edwl_thread_pool *p);

#endif /* EDWL_CORE_THREAD_POOL_H */
