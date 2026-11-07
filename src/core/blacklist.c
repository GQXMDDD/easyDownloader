/*
 * edwl - 黑名单实现
 *
 * 匹配规则刻意保持可预测：
 *   精确模式（无 *）        → 只匹配同名主机
 *   *.example.com          → 匹配 example.com 本身和它的所有子域
 *   *.sub.example.com      → 同上
 *   其它含 * 的模式         → 按通配符逐字符匹配（* 可匹配空串）
 */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <edwl/core/blacklist.h>
#include <edwl/util/url.h>

#define EDWL_BL_MAX_RULES 128

struct edwl_bl_host {
	char pattern[256];
};

struct edwl_bl_ip {
	uint32_t network;
	unsigned bits;
};

struct edwl_blacklist {
	struct edwl_bl_host hosts[EDWL_BL_MAX_RULES];
	size_t host_count;
	struct edwl_bl_ip ips[EDWL_BL_MAX_RULES];
	size_t ip_count;
};

/* ---------- 工具 ---------- */

static void lower_copy(char *dst, size_t size, const char *src, size_t len)
{
	size_t i;

	if (size == 0)
		return;
	if (len >= size)
		len = size - 1;
	for (i = 0; i < len; i++)
		dst[i] = (char)tolower((unsigned char)src[i]);
	dst[len] = '\0';
}

static int ci_equal(const char *a, const char *b)
{
	if (!a || !b)
		return 0;
	while (*a && *b) {
		if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
			return 0;
		a++;
		b++;
	}
	return *a == '\0' && *b == '\0';
}

/* '*' 通配匹配（大小写不敏感、可回溯） */
static int wildcard_match(const char *pattern, const char *text)
{
	const char *star = NULL;
	const char *mark = NULL;

	while (*text) {
		if (*pattern == '*') {
			star = pattern++;
			mark = text;
			continue;
		}
		if (*pattern && tolower((unsigned char)*pattern) ==
				    tolower((unsigned char)*text)) {
			pattern++;
			text++;
			continue;
		}
		if (star) {
			pattern = star + 1;
			text = ++mark;
			continue;
		}
		return 0;
	}
	while (*pattern == '*')
		pattern++;
	return *pattern == '\0';
}

static int ends_with_ci(const char *text, const char *suffix)
{
	size_t lt = strlen(text);
	size_t ls = strlen(suffix);
	size_t i;

	if (ls > lt)
		return 0;
	text += lt - ls;
	for (i = 0; i < ls; i++) {
		if (tolower((unsigned char)text[i]) !=
		    tolower((unsigned char)suffix[i]))
			return 0;
	}
	return 1;
}

static int host_rule_match(const char *pattern, const char *host)
{
	/* "*.example.com"：匹配 example.com 本身与所有子域 */
	if (pattern[0] == '*' && pattern[1] == '.') {
		const char *base = pattern + 2;

		if (ci_equal(host, base))
			return 1;
		return ends_with_ci(host, pattern + 1); /* ".example.com" */
	}
	return wildcard_match(pattern, host);
}

/* ---------- IPv4 / CIDR ---------- */

static int parse_ipv4(const char *s, uint32_t *out)
{
	uint32_t value = 0;
	int part;

	for (part = 0; part < 4; part++) {
		int digits = 0;
		unsigned octet = 0;

		while (isdigit((unsigned char)*s)) {
			octet = octet * 10 + (unsigned)(*s - '0');
			s++;
			digits++;
			if (digits > 3)
				return -1;
		}
		if (digits == 0 || octet > 255)
			return -1;
		value = (value << 8) | octet;
		if (part < 3) {
			if (*s != '.')
				return -1;
			s++;
		}
	}
	if (*s != '\0')
		return -1;
	*out = value;
	return 0;
}

static int parse_cidr(const char *s, struct edwl_bl_ip *out)
{
	char buf[64];
	char *slash;
	uint32_t ip;
	unsigned bits;

	if (strlen(s) >= sizeof(buf))
		return -1;
	strcpy(buf, s);
	slash = strchr(buf, '/');
	if (!slash)
		return -1;
	*slash = '\0';
	if (parse_ipv4(buf, &ip) != 0)
		return -1;
	bits = (unsigned)strtoul(slash + 1, NULL, 10);
	if (bits > 32)
		return -1;

	out->bits = bits;
	out->network = bits == 0 ? 0 : (ip & (0xFFFFFFFFUL << (32 - bits)));
	return 0;
}

static int ip_in_cidr(uint32_t ip, const struct edwl_bl_ip *rule)
{
	uint32_t mask =
	    rule->bits == 0 ? 0 : (0xFFFFFFFFUL << (32 - rule->bits));

	return (ip & mask) == rule->network;
}

/* ---------- 建表 ---------- */

/*
 * 取下一个逗号分隔的片段：去首尾空白，拷进 out（可能为空串）。
 * 返回下一段的起始位置；已到末尾返回 NULL。
 */
static const char *next_field(const char *list, char *out, size_t out_size)
{
	const char *comma = strchr(list, ',');
	size_t len = comma ? (size_t)(comma - list) : strlen(list);
	const char *end = list + len;

	while (list < end && (*list == ' ' || *list == '\t'))
		list++;
	while (end > list && (end[-1] == ' ' || end[-1] == '\t' ||
			      end[-1] == '\r' || end[-1] == '\n'))
		end--;

	len = (size_t)(end - list);
	if (len >= out_size)
		len = out_size - 1;
	if (len > 0)
		memcpy(out, list, len);
	out[len] = '\0';

	return comma ? comma + 1 : NULL;
}

static void add_hosts(struct edwl_blacklist *b, const char *list)
{
	char field[300];

	if (!list)
		return;

	while (*list) {
		char normalized[256];

		list = next_field(list, field, sizeof(field));
		if (field[0] == '\0') {
			if (!list)
				break;
			continue;
		}
		if (b->host_count >= EDWL_BL_MAX_RULES)
			return;

		/* 文档里写的 _.example.com 等价于 *.example.com */
		if (field[0] == '_' && field[1] == '.') {
			normalized[0] = '*';
			lower_copy(normalized + 1, sizeof(normalized) - 1,
				   field + 1, strlen(field) - 1);
		} else {
			lower_copy(normalized, sizeof(normalized), field,
				   strlen(field));
		}
		{
			char *dst = b->hosts[b->host_count].pattern;
			size_t cap = sizeof(b->hosts[b->host_count].pattern);
			size_t len = strlen(normalized);

			if (len >= cap)
				len = cap - 1;
			memcpy(dst, normalized, len);
			dst[len] = '\0';
		}
		b->host_count++;

		if (!list)
			break;
	}
}

static void add_ips(struct edwl_blacklist *b, const char *list)
{
	char field[300];

	if (!list)
		return;

	while (*list) {
		struct edwl_bl_ip rule;

		list = next_field(list, field, sizeof(field));
		if (field[0] == '\0') {
			if (!list)
				break;
			continue;
		}
		if (b->ip_count >= EDWL_BL_MAX_RULES)
			return;
		if (parse_cidr(field, &rule) == 0)
			b->ips[b->ip_count++] = rule; /* 写错的行直接跳过 */

		if (!list)
			break;
	}
}

int edwl_blacklist_create(const char *hosts, const char *ips,
			  struct edwl_blacklist **out)
{
	struct edwl_blacklist *b;

	if (!out)
		return -1;
	b = calloc(1, sizeof(*b));
	if (!b)
		return -1;
	add_hosts(b, hosts);
	add_ips(b, ips);
	*out = b;
	return 0;
}

void edwl_blacklist_destroy(struct edwl_blacklist *b)
{
	free(b);
}

size_t edwl_blacklist_host_count(const struct edwl_blacklist *b)
{
	return b ? b->host_count : 0;
}

size_t edwl_blacklist_ip_count(const struct edwl_blacklist *b)
{
	return b ? b->ip_count : 0;
}

/* ---------- 匹配 ---------- */

int edwl_blacklist_hit_host(const struct edwl_blacklist *b, const char *host,
			    char *why, size_t why_size)
{
	size_t i;

	if (!b || !host || !*host)
		return 0;
	for (i = 0; i < b->host_count; i++) {
		if (host_rule_match(b->hosts[i].pattern, host)) {
			if (why && why_size)
				snprintf(why, why_size, "主机 %s 命中规则 %s",
					 host, b->hosts[i].pattern);
			return 1;
		}
	}
	return 0;
}

int edwl_blacklist_hit_url(const struct edwl_blacklist *b, const char *url,
			   char *why, size_t why_size)
{
	struct edwl_url parsed;
	uint32_t ip;
	size_t i;

	if (!b || !url)
		return 0;
	if (edwl_url_parse(url, &parsed) != 0)
		return 0; /* 解析不了的 URL 交给后面的流程报错 */

	if (edwl_blacklist_hit_host(b, parsed.host, why, why_size))
		return 1;

	/* 只有 URL 里直接写 IPv4 字面量时才可能命中网段规则 */
	if (parse_ipv4(parsed.host, &ip) != 0)
		return 0;
	for (i = 0; i < b->ip_count; i++) {
		if (ip_in_cidr(ip, &b->ips[i])) {
			if (why && why_size)
				snprintf(why, why_size,
					 "IP %s 命中网段规则 /%u", parsed.host,
					 b->ips[i].bits);
			return 1;
		}
	}
	return 0;
}
