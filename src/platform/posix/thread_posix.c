/*
 * edwl - POSIX 平台：线程、互斥量、条件变量
 */
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

#include <edwl/platform/platform.h>

struct wgetx_thread {
	pthread_t tid;
};

struct wgetx_mutex {
	pthread_mutex_t lock;
};

struct wgetx_cond {
	pthread_cond_t cond;
};

int wgetx_thread_create(wgetx_thread **out, wgetx_thread_fn fn, void *arg)
{
	wgetx_thread *t;

	if (!out || !fn)
		return -1;

	t = calloc(1, sizeof(*t));
	if (!t)
		return -1;

	if (pthread_create(&t->tid, NULL, fn, arg) != 0) {
		free(t);
		return -1;
	}

	*out = t;
	return 0;
}

int wgetx_thread_join(wgetx_thread *t)
{
	int rc;

	if (!t)
		return -1;
	rc = pthread_join(t->tid, NULL);
	free(t);
	return rc == 0 ? 0 : -1;
}

unsigned long wgetx_thread_current_id(void)
{
	return (unsigned long)pthread_self();
}

int wgetx_mutex_init(wgetx_mutex **out)
{
	wgetx_mutex *m;

	if (!out)
		return -1;
	m = calloc(1, sizeof(*m));
	if (!m)
		return -1;
	if (pthread_mutex_init(&m->lock, NULL) != 0) {
		free(m);
		return -1;
	}
	*out = m;
	return 0;
}

int wgetx_mutex_lock(wgetx_mutex *m)
{
	if (!m)
		return -1;
	return pthread_mutex_lock(&m->lock) == 0 ? 0 : -1;
}

int wgetx_mutex_unlock(wgetx_mutex *m)
{
	if (!m)
		return -1;
	return pthread_mutex_unlock(&m->lock) == 0 ? 0 : -1;
}

void wgetx_mutex_destroy(wgetx_mutex *m)
{
	if (!m)
		return;
	pthread_mutex_destroy(&m->lock);
	free(m);
}

int wgetx_cond_init(wgetx_cond **out)
{
	wgetx_cond *c;

	if (!out)
		return -1;
	c = calloc(1, sizeof(*c));
	if (!c)
		return -1;
	if (pthread_cond_init(&c->cond, NULL) != 0) {
		free(c);
		return -1;
	}
	*out = c;
	return 0;
}

int wgetx_cond_wait(wgetx_cond *c, wgetx_mutex *m)
{
	int rc;

	if (!c || !m)
		return -1;
	rc = pthread_cond_wait(&c->cond, &m->lock);
	if (rc == 0)
		return 1;
	return -1;
}

int wgetx_cond_timedwait(wgetx_cond *c, wgetx_mutex *m, uint32_t ms)
{
	struct timespec deadline;
	uint64_t add_ns;
	int rc;

	if (!c || !m)
		return -1;

	clock_gettime(CLOCK_REALTIME, &deadline);
	add_ns = (uint64_t)ms * 1000000ULL;
	deadline.tv_sec += (time_t)(add_ns / 1000000000ULL);
	deadline.tv_nsec += (long)(add_ns % 1000000000ULL);
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_nsec -= 1000000000L;
		deadline.tv_sec += 1;
	}

	rc = pthread_cond_timedwait(&c->cond, &m->lock, &deadline);
	if (rc == 0)
		return 1;
	if (rc == ETIMEDOUT)
		return 0;
	return -1;
}

int wgetx_cond_signal(wgetx_cond *c)
{
	if (!c)
		return -1;
	return pthread_cond_signal(&c->cond) == 0 ? 0 : -1;
}

int wgetx_cond_broadcast(wgetx_cond *c)
{
	if (!c)
		return -1;
	return pthread_cond_broadcast(&c->cond) == 0 ? 0 : -1;
}

void wgetx_cond_destroy(wgetx_cond *c)
{
	if (!c)
		return;
	pthread_cond_destroy(&c->cond);
	free(c);
}
