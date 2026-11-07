/*
 * edwl - 线程池实现
 *
 * outstanding 是"还欠多少活"的计数：submit 加一，任务体执行完减一。
 * 这样即便 worker 在任务内部又 submit 了重试任务，wait_idle() 也不会
 * 提前返回——它等的是计数归零，不是队列空。
 */
#include <stdlib.h>

#include <edwl/core/task_queue.h>
#include <edwl/core/thread_pool.h>
#include <edwl/platform/platform.h>

struct edwl_thread_pool {
	struct edwl_task_queue *queue;
	edwl_pool_job_fn fn;
	edwl_pool_cleanup_fn cleanup;
	void *ctx;

	struct edwl_thread **threads;
	size_t workers;

	struct edwl_mutex *mtx;
	struct edwl_cond *cond;
	size_t outstanding;
	int stopping;
};

static void *worker_main(void *arg)
{
	struct edwl_thread_pool *p = arg;
	void *job;

	while (edwl_tq_pop(p->queue, &job) == 0) {
		p->fn(p->ctx, job);
		edwl_mutex_lock(p->mtx);
		if (p->outstanding > 0)
			p->outstanding--;
		if (p->outstanding == 0)
			edwl_cond_broadcast(p->cond);
		edwl_mutex_unlock(p->mtx);
	}
	if (p->cleanup)
		p->cleanup(p->ctx);
	return NULL;
}

static void destroy_partial(struct edwl_thread_pool *p, size_t started)
{
	size_t i;

	if (p->queue)
		edwl_tq_close(p->queue);
	for (i = 0; i < started; i++)
		edwl_thread_join(p->threads[i]);
	free(p->threads);
	if (p->cond)
		edwl_cond_destroy(p->cond);
	if (p->mtx)
		edwl_mutex_destroy(p->mtx);
	if (p->queue)
		edwl_tq_destroy(p->queue);
	free(p);
}

int edwl_pool_create(struct edwl_thread_pool **out, size_t workers,
		     edwl_pool_job_fn fn, void *ctx,
		     edwl_pool_cleanup_fn cleanup)
{
	struct edwl_thread_pool *p;
	size_t i = 0;

	if (!out || !fn || workers == 0)
		return -1;

	p = calloc(1, sizeof(*p));
	if (!p)
		return -1;

	p->fn = fn;
	p->cleanup = cleanup;
	p->ctx = ctx;
	p->workers = workers;

	if (edwl_tq_create(&p->queue) != 0)
		goto fail;
	if (edwl_mutex_init(&p->mtx) != 0)
		goto fail;
	if (edwl_cond_init(&p->cond) != 0)
		goto fail;

	p->threads = calloc(workers, sizeof(*p->threads));
	if (!p->threads)
		goto fail;

	for (i = 0; i < workers; i++) {
		if (edwl_thread_create(&p->threads[i], worker_main, p) != 0)
			goto fail;
	}

	*out = p;
	return 0;

fail:
	destroy_partial(p, i);
	return -1;
}

int edwl_pool_submit(struct edwl_thread_pool *p, void *job)
{
	if (!p)
		return -1;

	edwl_mutex_lock(p->mtx);
	if (p->stopping) {
		edwl_mutex_unlock(p->mtx);
		return -1;
	}
	p->outstanding++;
	edwl_mutex_unlock(p->mtx);

	if (edwl_tq_push(p->queue, job) != 0) {
		edwl_mutex_lock(p->mtx);
		if (p->outstanding > 0)
			p->outstanding--;
		edwl_mutex_unlock(p->mtx);
		return -1;
	}
	return 0;
}

int edwl_pool_wait_idle(struct edwl_thread_pool *p)
{
	if (!p)
		return -1;

	edwl_mutex_lock(p->mtx);
	while (p->outstanding > 0)
		edwl_cond_wait(p->cond, p->mtx);
	edwl_mutex_unlock(p->mtx);
	return 0;
}

size_t edwl_pool_outstanding(struct edwl_thread_pool *p)
{
	size_t n;

	if (!p)
		return 0;
	edwl_mutex_lock(p->mtx);
	n = p->outstanding;
	edwl_mutex_unlock(p->mtx);
	return n;
}

size_t edwl_pool_workers(struct edwl_thread_pool *p)
{
	return p ? p->workers : 0;
}

void edwl_pool_destroy(struct edwl_thread_pool *p)
{
	if (!p)
		return;

	edwl_mutex_lock(p->mtx);
	p->stopping = 1;
	edwl_mutex_unlock(p->mtx);

	destroy_partial(p, p->workers);
}
