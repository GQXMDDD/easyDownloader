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

#define EDWL_SRC_COOLDOWN_BASE_MS 1000ULL
#define EDWL_SRC_COOLDOWN_MAX_MS 30000ULL
#define EDWL_SRC_MAX_BACKOFF_SHIFT 5

struct edwl_source_item {
	char *url;
	unsigned fails;
	uint64_t ready_at_ms;
};

struct edwl_sources {
	struct edwl_mutex *mtx;
	struct edwl_source_item *items;
	size_t count;
	size_t cursor;
};

int edwl_sources_create(struct edwl_sources **out, const char *const *urls,
			size_t count)
{
	struct edwl_sources *s;
	size_t i;

	if (!out || !urls || count == 0)
		return -1;

	s = calloc(1, sizeof(*s));
	if (!s)
		return -1;

	s->items = calloc(count, sizeof(*s->items));
	if (!s->items)
		goto fail_items;

	if (edwl_mutex_init(&s->mtx) != 0)
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
	edwl_mutex_destroy(s->mtx);
fail_mutex:
	free(s->items);
fail_items:
	free(s);
	return -1;
}

void edwl_sources_destroy(struct edwl_sources *s)
{
	size_t i;

	if (!s)
		return;
	for (i = 0; i < s->count; i++) {
		free(s->items[i].url);
	}
	edwl_mutex_destroy(s->mtx);
	free(s->items);
	free(s);
}

size_t edwl_sources_count(const struct edwl_sources *s)
{
	return s ? s->count : 0;
}

const char *edwl_sources_url(struct edwl_sources *s, size_t index)
{
	if (!s || index >= s->count)
		return NULL;
	return s->items[index].url;
}

int edwl_sources_pick(struct edwl_sources *s, size_t *index)
{
	uint64_t now;
	size_t i;

	if (!s || !index)
		return 0;

	now = edwl_now_ms();
	edwl_mutex_lock(s->mtx);
	for (i = 0; i < s->count; i++) {
		size_t idx = (s->cursor + i) % s->count;

		if (s->items[idx].ready_at_ms <= now) {
			s->cursor = (idx + 1) % s->count;
			*index = idx;
			edwl_mutex_unlock(s->mtx);
			return 1;
		}
	}
	edwl_mutex_unlock(s->mtx);
	return 0;
}

void edwl_sources_report_ok(struct edwl_sources *s, size_t index)
{
	if (!s || index >= s->count)
		return;
	edwl_mutex_lock(s->mtx);
	s->items[index].fails = 0;
	s->items[index].ready_at_ms = 0;
	edwl_mutex_unlock(s->mtx);
}

void edwl_sources_report_fail(struct edwl_sources *s, size_t index)
{
	struct edwl_source_item *it;
	unsigned shift;
	uint64_t cool;

	if (!s || index >= s->count)
		return;

	edwl_mutex_lock(s->mtx);
	it = &s->items[index];
	if (it->fails < 1000)
		it->fails++;
	shift = it->fails > 0 ? it->fails - 1 : 0;
	if (shift > EDWL_SRC_MAX_BACKOFF_SHIFT)
		shift = EDWL_SRC_MAX_BACKOFF_SHIFT;
	cool = EDWL_SRC_COOLDOWN_BASE_MS << shift;
	if (cool > EDWL_SRC_COOLDOWN_MAX_MS)
		cool = EDWL_SRC_COOLDOWN_MAX_MS;
	it->ready_at_ms = edwl_now_ms() + cool;
	edwl_mutex_unlock(s->mtx);
}

uint64_t edwl_sources_cooldown_left(struct edwl_sources *s)
{
	uint64_t now, best = 0;
	size_t i;

	if (!s)
		return 0;

	now = edwl_now_ms();
	edwl_mutex_lock(s->mtx);
	for (i = 0; i < s->count; i++) {
		uint64_t left = s->items[i].ready_at_ms > now
				    ? s->items[i].ready_at_ms - now
				    : 0;

		if (left > 0 && (best == 0 || left < best))
			best = left;
	}
	edwl_mutex_unlock(s->mtx);
	return best;
}

size_t edwl_sources_healthy(struct edwl_sources *s)
{
	size_t n = 0;
	size_t i;

	if (!s)
		return 0;
	edwl_mutex_lock(s->mtx);
	for (i = 0; i < s->count; i++) {
		if (s->items[i].fails == 0)
			n++;
	}
	edwl_mutex_unlock(s->mtx);
	return n;
}
