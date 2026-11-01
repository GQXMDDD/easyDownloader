/*
 * edwl - 多源管理实现
 *
 * 健康度模型很简单，够用就行：
 *   - 失败一次，该源进入冷却
 *   - 冷却时长指数增长 1s → 2s → 4s ... 封顶 30s
 *   - 成功一次立刻清零
 * 挑源时轮转着来，跳过冷却中的；全都在冷却就返回失败，让调用方等。
 */
#include <stdlib.h>
#include <string.h>

#include <edwl/core/source.h>
#include <edwl/platform/platform.h>

#define WGETX_SRC_COOLDOWN_BASE_MS 1000ULL
#define WGETX_SRC_COOLDOWN_MAX_MS 30000ULL
#define WGETX_SRC_MAX_BACKOFF_SHIFT 5

struct wgetx_source_item {
	char *url;
	unsigned fails;
	uint64_t ready_at_ms;
};

struct wgetx_sources {
	struct wgetx_mutex *mtx;
	struct wgetx_source_item *items;
	size_t count;
	size_t cursor;
};

int wgetx_sources_create(struct wgetx_sources **out, const char *const *urls,
			 size_t count)
{
	struct wgetx_sources *s;
	size_t i;

	if (!out || !urls || count == 0)
		return -1;

	s = calloc(1, sizeof(*s));
	if (!s)
		return -1;

	s->items = calloc(count, sizeof(*s->items));
	if (!s->items)
		goto fail_items;

	if (wgetx_mutex_init(&s->mtx) != 0)
		goto fail_mutex;

	s->count = count;
	for (i = 0; i < count; i++) {
		size_t len = strlen(urls[i]);

		s->items[i].url = malloc(len + 1);
		if (!s->items[i].url)
			goto fail_copy;
		memcpy(s->items[i].url, urls[i], len + 1);
	}

	*out = s;
	return 0;

fail_copy:
	while (i > 0) {
		i--;
		free(s->items[i].url);
	}
	wgetx_mutex_destroy(s->mtx);
fail_mutex:
	free(s->items);
fail_items:
	free(s);
	return -1;
}

void wgetx_sources_destroy(struct wgetx_sources *s)
{
	size_t i;

	if (!s)
		return;
	for (i = 0; i < s->count; i++) {
		free(s->items[i].url);
	}
	wgetx_mutex_destroy(s->mtx);
	free(s->items);
	free(s);
}

size_t wgetx_sources_count(const struct wgetx_sources *s)
{
	return s ? s->count : 0;
}

const char *wgetx_sources_url(struct wgetx_sources *s, size_t index)
{
	if (!s || index >= s->count)
		return NULL;
	return s->items[index].url;
}

int wgetx_sources_pick(struct wgetx_sources *s, size_t *index)
{
	uint64_t now;
	size_t i;

	if (!s || !index)
		return 0;

	now = wgetx_now_ms();
	wgetx_mutex_lock(s->mtx);
	for (i = 0; i < s->count; i++) {
		size_t idx = (s->cursor + i) % s->count;

		if (s->items[idx].ready_at_ms <= now) {
			s->cursor = (idx + 1) % s->count;
			*index = idx;
			wgetx_mutex_unlock(s->mtx);
			return 1;
		}
	}
	wgetx_mutex_unlock(s->mtx);
	return 0;
}

void wgetx_sources_report_ok(struct wgetx_sources *s, size_t index)
{
	if (!s || index >= s->count)
		return;
	wgetx_mutex_lock(s->mtx);
	s->items[index].fails = 0;
	s->items[index].ready_at_ms = 0;
	wgetx_mutex_unlock(s->mtx);
}

void wgetx_sources_report_fail(struct wgetx_sources *s, size_t index)
{
	struct wgetx_source_item *it;
	unsigned shift;
	uint64_t cool;

	if (!s || index >= s->count)
		return;

	wgetx_mutex_lock(s->mtx);
	it = &s->items[index];
	if (it->fails < 1000)
		it->fails++;
	shift = it->fails > 0 ? it->fails - 1 : 0;
	if (shift > WGETX_SRC_MAX_BACKOFF_SHIFT)
		shift = WGETX_SRC_MAX_BACKOFF_SHIFT;
	cool = WGETX_SRC_COOLDOWN_BASE_MS << shift;
	if (cool > WGETX_SRC_COOLDOWN_MAX_MS)
		cool = WGETX_SRC_COOLDOWN_MAX_MS;
	it->ready_at_ms = wgetx_now_ms() + cool;
	wgetx_mutex_unlock(s->mtx);
}

uint64_t wgetx_sources_cooldown_left(struct wgetx_sources *s)
{
	uint64_t now, best = 0;
	size_t i;

	if (!s)
		return 0;

	now = wgetx_now_ms();
	wgetx_mutex_lock(s->mtx);
	for (i = 0; i < s->count; i++) {
		uint64_t left = s->items[i].ready_at_ms > now
				    ? s->items[i].ready_at_ms - now
				    : 0;

		if (left > 0 && (best == 0 || left < best))
			best = left;
	}
	wgetx_mutex_unlock(s->mtx);
	return best;
}

size_t wgetx_sources_healthy(struct wgetx_sources *s)
{
	size_t n = 0;
	size_t i;

	if (!s)
		return 0;
	wgetx_mutex_lock(s->mtx);
	for (i = 0; i < s->count; i++) {
		if (s->items[i].fails == 0)
			n++;
	}
	wgetx_mutex_unlock(s->mtx);
	return n;
}
