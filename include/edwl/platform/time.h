/*
 * edwl - 平台抽象层：单调时钟与休眠
 *
 * 只用单调时钟：系统时间被 NTP 调整时不会影响测速和超时判定。
 */
#ifndef WGETX_PLATFORM_TIME_H
#define WGETX_PLATFORM_TIME_H

#include <stdint.h>

/* 单调时钟，毫秒。只用于计算时间差，绝对值无意义 */
uint64_t wgetx_now_ms(void);

/* 单调时钟，微秒 */
uint64_t wgetx_now_us(void);

/* 休眠指定毫秒；被信号打断时提前返回，调用方自行重试 */
void wgetx_sleep_ms(uint32_t ms);

#endif /* WGETX_PLATFORM_TIME_H */
