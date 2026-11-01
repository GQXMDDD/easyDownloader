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

struct wgetx_thread_pool {
	struct wgetx_task_queue *queue;
	wgetx_pool_job_fn fn;
	void *ctx;

	struct wgetx_thread **threads;
	size_t workers;

	struct wgetx_mutex *mtx;
	struct wgetx_cond *cond;
	size_t outstanding;
	int stopping;
};

static void *worker_main(void *arg)
{
	struct wgetx_thread_pool *p = arg;
	void *job;

	while (wgetx_tq_pop(p->queue, &job) == 0) {
		p->fn(p->ctx, job);
		wgetx_mutex_lock(p->mtx);
		if (p->outstanding > 0)
			p->outstanding--;
		if (p->outstanding == 0)
			wgetx_cond_broadcast(p->cond);
		wgetx_mutex_unlock(p->mtx);
	}
	return NULL;
}

static void destroy_partial(struct wgetx_thread_pool *p, size_t started)
{
	size_t i;

	if (p->queue)
		wgetx_tq_close(p->queue);
	for (i = 0; i < started; i++)
		wgetx_thread_join(p->threads[i]);
	free(p->threads);
	if (p->cond)
		wgetx_cond_destroy(p->cond);
	if (p->mtx)
		wgetx_mutex_destroy(p->mtx);
	if (p->queue)
		wgetx_tq_destroy(p->queue);
	free(p);
}

int wgetx_pool_create(struct wgetx_thread_pool **out, size_t workers,
		      wgetx_pool_job_fn fn, void *ctx)
{
	struct wgetx_thread_pool *p;
	size_t i = 0;

	if (!out || !fn || workers == 0)
		return -1;

	p = calloc(1, sizeof(*p));
	if (!p)
		return -1;

	p->fn = fn;
	p->ctx = ctx;
	p->workers = workers;

	if (wgetx_tq_create(&p->queue) != 0)
		goto fail;
	if (wgetx_mutex_init(&p->mtx) != 0)
		goto fail;
	if (wgetx_cond_init(&p->cond) != 0)
		goto fail;

	p->threads = calloc(workers, sizeof(*p->threads));
	if (!p->threads)
		goto fail;

	for (i = 0; i < workers; i++) {
		if (wgetx_thread_create(&p->threads[i], worker_main, p) != 0)
			goto fail;
	}

	*out = p;
	return 0;

fail:
	destroy_partial(p, i);
	return -1;
}

int wgetx_pool_submit(struct wgetx_thread_pool *p, void *job)
{
	if (!p)
		return -1;

	wgetx_mutex_lock(p->mtx);
	if (p->stopping) {
		wgetx_mutex_unlock(p->mtx);
		return -1;
	}
	p->outstanding++;
	wgetx_mutex_unlock(p->mtx);

	if (wgetx_tq_push(p->queue, job) != 0) {
		wgetx_mutex_lock(p->mtx);
		if (p->outstanding > 0)
			p->outstanding--;
		wgetx_mutex_unlock(p->mtx);
		return -1;
	}
	return 0;
}

int wgetx_pool_wait_idle(struct wgetx_thread_pool *p)
{
	if (!p)
		return -1;

	wgetx_mutex_lock(p->mtx);
	while (p->outstanding > 0)
		wgetx_cond_wait(p->cond, p->mtx);
	wgetx_mutex_unlock(p->mtx);
	return 0;
}

size_t wgetx_pool_outstanding(struct wgetx_thread_pool *p)
{
	size_t n;

	if (!p)
		return 0;
	wgetx_mutex_lock(p->mtx);
	n = p->outstanding;
	wgetx_mutex_unlock(p->mtx);
	return n;
}

size_t wgetx_pool_workers(struct wgetx_thread_pool *p)
{
	return p ? p->workers : 0;
}

void wgetx_pool_destroy(struct wgetx_thread_pool *p)
{
	if (!p)
		return;

	wgetx_mutex_lock(p->mtx);
	p->stopping = 1;
	wgetx_mutex_unlock(p->mtx);

	destroy_partial(p, p->workers);
}
