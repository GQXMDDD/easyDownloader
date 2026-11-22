#ifndef EDWL_CORE_GITHUB_RELEASE_H
#define EDWL_CORE_GITHUB_RELEASE_H

#include <stddef.h>

#include <edwl/core/checksum.h>
#include <edwl/util/url.h>

struct edwl_http;

struct edwl_github_release {
	char tag[128];
	char asset[256];
	char url[EDWL_URL_MAX];
	char sha256[EDWL_SHA256_HEX];
};

/* 解析并选择资产；同分资产会报错，asset 非 NULL 时按精确文件名选择。 */
int edwl_github_select_release(const char *json, const char *asset,
			       const char *os, const char *arch,
			       struct edwl_github_release *out, char *error,
			       size_t error_size);

/* latest 正式 Release → 当前平台资产及
 * SHA-256；找不到哈希则报错，不静默跳过校验。 */
int edwl_github_resolve_release(struct edwl_http *http, const char *repository,
				const char *asset, const char *expected_sha256,
				struct edwl_github_release *out, char *error,
				size_t error_size);

#endif
