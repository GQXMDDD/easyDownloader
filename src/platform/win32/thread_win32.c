/*
 * edwl - Win32 平台：线程、互斥量、条件变量
 *
 * 线程用 _beginthreadex 而不是 CreateThread：前者会初始化 CRT，
 * 否则新线程里 malloc/errno 都有坑（原设计明确要求）。
 *
 * 互斥量用 CRITICAL_SECTION（轻量、无需内核对象），
 * 条件变量用 CONDITION_VARIABLE，配 SleepConditionVariableCS。
 */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include <windows.h>
#include <process.h>
#include <stdlib.h>
#include <string.h>

#include <edwl/platform/platform.h>

struct edwl_thread {
	HANDLE handle;
	edwl_thread_fn fn;
	void *arg;
};

struct edwl_mutex {
	CRITICAL_SECTION cs;
};

struct edwl_cond {
	CONDITION_VARIABLE cv;
};

static unsigned __stdcall thread_entry(void *param)
{
	edwl_thread *t = param;

	if (t->fn)
		t->fn(t->arg);
	return 0;
}

int edwl_thread_create(edwl_thread **out, edwl_thread_fn fn, void *arg)
{
	edwl_thread *t;
	uintptr_t handle;

	if (!out || !fn)
		return -1;

	t = calloc(1, sizeof(*t));
	if (!t)
		return -1;

	t->fn = fn;
	t->arg = arg;

	handle = _beginthreadex(NULL, 0, thread_entry, t, 0, NULL);
	if (handle == 0) {
		free(t);
		return -1;
	}

	t->handle = (HANDLE)handle;
	*out = t;
	return 0;
}

int edwl_thread_join(edwl_thread *t)
{
	int rc = 0;

	if (!t)
		return -1;
	if (WaitForSingleObject(t->handle, INFINITE) != WAIT_OBJECT_0)
		rc = -1;
	CloseHandle(t->handle);
	free(t);
	return rc;
}

unsigned long edwl_thread_current_id(void)
{
	return (unsigned long)GetCurrentThreadId();
}

int edwl_mutex_init(edwl_mutex **out)
{
	edwl_mutex *m;

	if (!out)
		return -1;
	m = calloc(1, sizeof(*m));
	if (!m)
		return -1;
	InitializeCriticalSection(&m->cs);
	*out = m;
	return 0;
}

int edwl_mutex_lock(edwl_mutex *m)
{
	if (!m)
		return -1;
	EnterCriticalSection(&m->cs);
	return 0;
}

int edwl_mutex_unlock(edwl_mutex *m)
{
	if (!m)
		return -1;
	LeaveCriticalSection(&m->cs);
	return 0;
}

void edwl_mutex_destroy(edwl_mutex *m)
{
	if (!m)
		return;
	DeleteCriticalSection(&m->cs);
	free(m);
}

int edwl_cond_init(edwl_cond **out)
{
	edwl_cond *c;

	if (!out)
		return -1;
	c = calloc(1, sizeof(*c));
	if (!c)
		return -1;
	InitializeConditionVariable(&c->cv);
	*out = c;
	return 0;
}

int edwl_cond_wait(edwl_cond *c, edwl_mutex *m)
{
	if (!c || !m)
		return -1;
	if (!SleepConditionVariableCS(&c->cv, &m->cs, INFINITE))
		return -1;
	return 1;
}

int edwl_cond_timedwait(edwl_cond *c, edwl_mutex *m, uint32_t ms)
{
	if (!c || !m)
		return -1;
	if (SleepConditionVariableCS(&c->cv, &m->cs, (DWORD)ms))
		return 1;
	if (GetLastError() == ERROR_TIMEOUT)
		return 0;
	return -1;
}

int edwl_cond_signal(edwl_cond *c)
{
	if (!c)
		return -1;
	WakeConditionVariable(&c->cv);
	return 0;
}

int edwl_cond_broadcast(edwl_cond *c)
{
	if (!c)
		return -1;
	WakeAllConditionVariable(&c->cv);
	return 0;
}

void edwl_cond_destroy(edwl_cond *c)
{
	if (!c)
		return;
	free(c);
}
