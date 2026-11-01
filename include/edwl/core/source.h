/*
 * edwl - 多源管理
 *
 * 一组等价的 URL（镜像），带健康度：失败一次进入冷却，冷却时间指数增长；
 * 全部失败时退化为"按顺序重试"。选择是线程安全的。
 *
 * 设计意图：分片可以来自不同源，所以整体校验放在合并之后做（见 checksum）。
 */
#ifndef WGETX_CORE_SOURCE_H
#define WGETX_CORE_SOURCE_H

#include <stddef.h>
#include <stdint.h>

struct wgetx_sources;

/* urls 里的字符串会被复制。0 成功，-1 失败 */
int wgetx_sources_create(struct wgetx_sources **out, const char *const *urls,
			 size_t count);
void wgetx_sources_destroy(struct wgetx_sources *s);

size_t wgetx_sources_count(const struct wgetx_sources *s);

/*
 * 挑一个当前可用的源：优先按轮转，跳过冷却中的。
 * 全部在冷却时返回 0（调用方应等待或放弃）。index 返回源序号。
 */
int wgetx_sources_pick(struct wgetx_sources *s, size_t *index);

/* 取第 index 个源的 URL，越界返回 NULL */
const char *wgetx_sources_url(struct wgetx_sources *s, size_t index);

/* 某个源成功一次：清零失败计数与冷却 */
void wgetx_sources_report_ok(struct wgetx_sources *s, size_t index);

/* 某个源失败一次：失败计数 +1，进入冷却 */
void wgetx_sources_report_fail(struct wgetx_sources *s, size_t index);

/* 距最短冷却结束还有多少毫秒，没有冷却中的源返回 0 */
uint64_t wgetx_sources_cooldown_left(struct wgetx_sources *s);

/* 从未失败过的源数量，用于日志 */
size_t wgetx_sources_healthy(struct wgetx_sources *s);

#endif /* WGETX_CORE_SOURCE_H */
