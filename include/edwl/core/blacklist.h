/*
 * edwl - 黑名单（防止误连到不该下载的主机/IP）
 *
 * 两种规则：
 *   Hosts：主机名/域名，支持 * 通配符；`_.example.com` 视同 `*.example.com`
 *   IPs  ：IPv4 网段（CIDR），只匹配 URL 里直接写 IP 的情况
 *
 * 匹配发生在"发起任何请求之前"，所以被拦下的目标一个字节都不会被请求。
 */
#ifndef EDWL_CORE_BLACKLIST_H
#define EDWL_CORE_BLACKLIST_H

#include <stddef.h>

struct edwl_blacklist;

/*
 * 从逗号分隔的字符串建表。hosts/ips 可以为 NULL 或空串。
 * 0 成功，-1 失败（内存不足）。
 */
int edwl_blacklist_create(const char *hosts, const char *ips,
			  struct edwl_blacklist **out);
void edwl_blacklist_destroy(struct edwl_blacklist *b);

size_t edwl_blacklist_host_count(const struct edwl_blacklist *b);
size_t edwl_blacklist_ip_count(const struct edwl_blacklist *b);

/* 主机名是否命中。1 = 命中，0 = 放行 */
int edwl_blacklist_hit_host(const struct edwl_blacklist *b, const char *host,
			    char *why, size_t why_size);

/* 解析 URL 并检查（含 IPv4 字面量比对 CIDR）。1 = 命中 */
int edwl_blacklist_hit_url(const struct edwl_blacklist *b, const char *url,
			   char *why, size_t why_size);

#endif /* EDWL_CORE_BLACKLIST_H */
