/*
 * edwl - POSIX 平台：单调时钟与休眠
 */
#include <errno.h>
#include <time.h>

#include <edwl/platform/platform.h>

uint64_t edwl_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000ULL +
	       (uint64_t)(ts.tv_nsec / 1000000L);
}

uint64_t edwl_now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ULL +
	       (uint64_t)(ts.tv_nsec / 1000L);
}

void edwl_sleep_ms(uint32_t ms)
{
	struct timespec req, rem;

	if (ms == 0)
		return;

	req.tv_sec = (time_t)(ms / 1000);
	req.tv_nsec = (long)(ms % 1000) * 1000000L;

	while (nanosleep(&req, &rem) != 0 && errno == EINTR)
		req = rem;
}
