/*
 * edwl - Win32 平台：单调时钟与休眠
 *
 * 用 QueryPerformanceCounter：GetTickCount 精度差且 49 天回绕，
 * 不能拿来算速度和超时。
 */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include <windows.h>

#include <edwl/platform/platform.h>

static LONGLONG qpc_freq;

static void ensure_freq(void)
{
	if (qpc_freq == 0) {
		LARGE_INTEGER f;

		QueryPerformanceFrequency(&f);
		qpc_freq = (f.QuadPart > 0) ? f.QuadPart : 1;
	}
}

uint64_t edwl_now_ms(void)
{
	LARGE_INTEGER c;

	ensure_freq();
	QueryPerformanceCounter(&c);
	return (uint64_t)(c.QuadPart / qpc_freq * 1000 +
			  (c.QuadPart % qpc_freq) * 1000 / qpc_freq);
}

uint64_t edwl_now_us(void)
{
	LARGE_INTEGER c;

	ensure_freq();
	QueryPerformanceCounter(&c);
	return (uint64_t)(c.QuadPart / qpc_freq * 1000000 +
			  (c.QuadPart % qpc_freq) * 1000000 / qpc_freq);
}

void edwl_sleep_ms(uint32_t ms)
{
	if (ms == 0)
		return;
	Sleep((DWORD)ms);
}
