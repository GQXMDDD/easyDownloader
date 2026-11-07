/*
 * edwl - 平台抽象层：单调时钟与休眠
 *
 * 只用单调时钟：系统时间被 NTP 调整时不会影响测速和超时判定。
 */
#ifndef EDWL_PLATFORM_TIME_H
#define EDWL_PLATFORM_TIME_H

#include <stdint.h>

/* 单调时钟，毫秒。只用于计算时间差，绝对值无意义 */
uint64_t edwl_now_ms(void);

/* 单调时钟，微秒 */
uint64_t edwl_now_us(void);

/* 休眠指定毫秒；被信号打断时提前返回，调用方自行重试 */
void edwl_sleep_ms(uint32_t ms);

#endif /* EDWL_PLATFORM_TIME_H */
