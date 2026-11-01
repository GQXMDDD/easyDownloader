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

struct wgetx_thread {
	HANDLE handle;
	wgetx_thread_fn fn;
	void *arg;
};

struct wgetx_mutex {
	CRITICAL_SECTION cs;
};

struct wgetx_cond {
	CONDITION_VARIABLE cv;
};

static unsigned __stdcall thread_entry(void *param)
{
	wgetx_thread *t = param;

	if (t->fn)
		t->fn(t->arg);
	return 0;
}

int wgetx_thread_create(wgetx_thread **out, wgetx_thread_fn fn, void *arg)
{
	wgetx_thread *t;
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

int wgetx_thread_join(wgetx_thread *t)
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

unsigned long wgetx_thread_current_id(void)
{
	return (unsigned long)GetCurrentThreadId();
}

int wgetx_mutex_init(wgetx_mutex **out)
{
	wgetx_mutex *m;

	if (!out)
		return -1;
	m = calloc(1, sizeof(*m));
	if (!m)
		return -1;
	InitializeCriticalSection(&m->cs);
	*out = m;
	return 0;
}

int wgetx_mutex_lock(wgetx_mutex *m)
{
	if (!m)
		return -1;
	EnterCriticalSection(&m->cs);
	return 0;
}

int wgetx_mutex_unlock(wgetx_mutex *m)
{
	if (!m)
		return -1;
	LeaveCriticalSection(&m->cs);
	return 0;
}

void wgetx_mutex_destroy(wgetx_mutex *m)
{
	if (!m)
		return;
	DeleteCriticalSection(&m->cs);
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
	InitializeConditionVariable(&c->cv);
	*out = c;
	return 0;
}

int wgetx_cond_wait(wgetx_cond *c, wgetx_mutex *m)
{
	if (!c || !m)
		return -1;
	if (!SleepConditionVariableCS(&c->cv, &m->cs, INFINITE))
		return -1;
	return 1;
}

int wgetx_cond_timedwait(wgetx_cond *c, wgetx_mutex *m, uint32_t ms)
{
	if (!c || !m)
		return -1;
	if (SleepConditionVariableCS(&c->cv, &m->cs, (DWORD)ms))
		return 1;
	if (GetLastError() == ERROR_TIMEOUT)
		return 0;
	return -1;
}

int wgetx_cond_signal(wgetx_cond *c)
{
	if (!c)
		return -1;
	WakeConditionVariable(&c->cv);
	return 0;
}

int wgetx_cond_broadcast(wgetx_cond *c)
{
	if (!c)
		return -1;
	WakeAllConditionVariable(&c->cv);
	return 0;
}

void wgetx_cond_destroy(wgetx_cond *c)
{
	if (!c)
		return;
	free(c);
}
