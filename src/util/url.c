/*
 * edwl - URL 解析与 Range 拼装
 *
 * 只处理绝对 http/https URL。要点：
 *  - 支持 userinfo（http://u:p@host/path）
 *  - 支持 IPv6 字面量（http://[::1]:8080/path），Host 头要补方括号
 *  - 片段标识符（#frag）一律丢弃，它不会被发到服务器
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <edwl/util/url.h>

static int scheme_copy(const char *url, size_t len, char out[16])
{
	size_t i;

	if (len == 0 || len >= 16)
		return -1;
	for (i = 0; i < len; i++) {
		unsigned char c = (unsigned char)url[i];

		if (!isalpha(c))
			return -1;
		out[i] = (char)tolower(c);
	}
	out[len] = '\0';
	return 0;
}

static int parse_port(const char *s, size_t len, int *out)
{
	size_t i;
	long value = 0;

	if (len == 0 || len > 5)
		return -1;
	for (i = 0; i < len; i++) {
		if (!isdigit((unsigned char)s[i]))
			return -1;
		value = value * 10 + (s[i] - '0');
	}
	if (value <= 0 || value > 65535)
		return -1;
	*out = (int)value;
	return 0;
}

int edwl_url_default_port(const char *scheme)
{
	if (!scheme)
		return 0;
	if (strcmp(scheme, "http") == 0)
		return 80;
	if (strcmp(scheme, "https") == 0)
		return 443;
	return 0;
}

static int fill_host(struct edwl_url *out, const char *begin, const char *end)
{
	const char *host_begin = begin;
	const char *host_end = end;
	const char *colon;
	const char *rb;
	size_t len;

	if (*begin == '[') {
		rb = memchr(begin, ']', (size_t)(end - begin));
		if (!rb)
			return -1;
		host_begin = begin + 1;
		host_end = rb;
		if (rb + 1 < end) {
			if (rb[1] != ':')
				return -1;
			if (parse_port(rb + 2, (size_t)(end - rb - 2),
				       &out->port) != 0)
				return -1;
		}
	} else {
		colon = memchr(begin, ':', (size_t)(end - begin));
		if (colon) {
			host_end = colon;
			if (parse_port(colon + 1, (size_t)(end - colon - 1),
				       &out->port) != 0)
				return -1;
		}
	}

	len = (size_t)(host_end - host_begin);
	if (len == 0 || len >= sizeof(out->host))
		return -1;
	memcpy(out->host, host_begin, len);
	out->host[len] = '\0';
	return 0;
}

static int fill_host_header(struct edwl_url *out)
{
	int is_v6 = strchr(out->host, ':') != NULL;
	int def_port = edwl_url_default_port(out->scheme);
	int same_port = (out->port == def_port);
	int n;

	if (is_v6) {
		if (same_port)
			n = snprintf(out->host_header, sizeof(out->host_header),
				     "[%s]", out->host);
		else
			n = snprintf(out->host_header, sizeof(out->host_header),
				     "[%s]:%d", out->host, out->port);
	} else {
		if (same_port)
			n = snprintf(out->host_header, sizeof(out->host_header),
				     "%s", out->host);
		else
			n = snprintf(out->host_header, sizeof(out->host_header),
				     "%s:%d", out->host, out->port);
	}
	if (n < 0 || (size_t)n >= sizeof(out->host_header))
		return -1;
	return 0;
}

static int fill_path(struct edwl_url *out, const char *start)
{
	size_t len = strcspn(start, "#");

	if (len >= sizeof(out->path))
		return -1;
	if (len > 0 && start[0] == '/') {
		memcpy(out->path, start, len);
		out->path[len] = '\0';
		return 0;
	}

	/* 没有路径段（"http://host" 或 "http://host?q"）：补 '/' */
	if (len + 1 >= sizeof(out->path))
		return -1;
	out->path[0] = '/';
	memcpy(out->path + 1, start, len);
	out->path[len + 1] = '\0';
	return 0;
}

int edwl_url_parse(const char *url, struct edwl_url *out)
{
	const char *sep;
	const char *authority;
	const char *authority_end;
	const char *cursor;
	const char *at = NULL;
	const char *host_begin;

	if (!url || !out)
		return -1;
	memset(out, 0, sizeof(*out));

	sep = strstr(url, "://");
	if (!sep)
		return -1;
	if (scheme_copy(url, (size_t)(sep - url), out->scheme) != 0)
		return -1;
	if (strcmp(out->scheme, "http") != 0 &&
	    strcmp(out->scheme, "https") != 0)
		return -1;

	authority = sep + 3;
	authority_end = authority + strcspn(authority, "/?#");
	if (authority_end == authority)
		return -1;

	for (cursor = authority; cursor < authority_end; cursor++) {
		if (*cursor == '@')
			at = cursor;
	}
	host_begin = at ? at + 1 : authority;
	if (host_begin >= authority_end)
		return -1;

	if (fill_host(out, host_begin, authority_end) != 0)
		return -1;
	if (out->port == 0)
		out->port = edwl_url_default_port(out->scheme);
	if (out->port <= 0)
		return -1;

	if (fill_host_header(out) != 0)
		return -1;
	if (fill_path(out, authority_end) != 0)
		return -1;

	return 0;
}

int edwl_url_is_absolute(const char *url)
{
	const char *sep;

	if (!url)
		return 0;
	sep = strstr(url, "://");
	if (!sep)
		return 0;
	return (size_t)(sep - url) < 8;
}

static int ci_equal(const char *a, const char *b)
{
	while (*a && *b) {
		if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
			return 0;
		a++;
		b++;
	}
	return *a == '\0' && *b == '\0';
}

static int github_repo_parts(const char *path, size_t len, int strip_git,
			     size_t *owner_len, size_t *repo_len)
{
	const char *slash;
	const char *repo;
	size_t i;

	if (len > 0 && path[len - 1] == '/')
		len--;
	slash = memchr(path, '/', len);
	if (!slash)
		return -1;
	*owner_len = (size_t)(slash - path);
	*repo_len = len - *owner_len - 1;
	repo = slash + 1;
	if (strip_git && *repo_len >= 4 &&
	    memcmp(repo + *repo_len - 4, ".git", 4) == 0)
		*repo_len -= 4;
	if (*owner_len == 0 || *owner_len > 39 || *repo_len == 0 ||
	    *repo_len > 100)
		return -1;
	if (!isalnum((unsigned char)path[0]) ||
	    !isalnum((unsigned char)path[*owner_len - 1]))
		return -1;
	for (i = 0; i < *owner_len; i++) {
		unsigned char c = (unsigned char)path[i];

		if (c > 127 || (!isalnum(c) && c != '-'))
			return -1;
	}
	for (i = 0; i < *repo_len; i++) {
		unsigned char c = (unsigned char)repo[i];

		if (c > 127 ||
		    (!isalnum(c) && c != '-' && c != '_' && c != '.'))
			return -1;
	}
	if ((*repo_len == 1 && repo[0] == '.') ||
	    (*repo_len == 2 && repo[0] == '.' && repo[1] == '.'))
		return -1;
	return 0;
}

int edwl_url_normalize_github(const char *input, char *out, size_t out_size)
{
	struct edwl_url parsed;
	const char *path = input;
	const char *sep;
	size_t len;
	size_t owner_len;
	size_t repo_len;
	int n;

	if (!input || !out || out_size == 0)
		return -1;
	out[0] = '\0';
	sep = strstr(input, "://");
	if (sep) {
		if (edwl_url_parse(input, &parsed) != 0)
			return -1;
		path = parsed.path + 1;
		len = strcspn(path, "?#");
		if (!ci_equal(parsed.host, "github.com") ||
		    parsed.port != edwl_url_default_port(parsed.scheme) ||
		    memchr(sep + 3, '@', strcspn(sep + 3, "/?#")) ||
		    github_repo_parts(path, len, 1, &owner_len, &repo_len) !=
			0) {
			n = snprintf(out, out_size, "%s", input);
			return n < 0 || (size_t)n >= out_size ? -1 : 0;
		}
	} else {
		if (strncmp(input, "github.com/", 11) == 0)
			path = input + 11;
		else if (strncmp(input, "git@github.com:", 15) == 0)
			path = input + 15;
		if (github_repo_parts(path, strlen(path), 1, &owner_len,
				      &repo_len) != 0)
			return -1;
	}

	/* HEAD 跟随仓库的默认分支，不假设它叫 main 或 master。 */
	n = snprintf(out, out_size,
		     "https://codeload.github.com/%.*s/%.*s/zip/HEAD",
		     (int)owner_len, path, (int)repo_len, path + owner_len + 1);
	return n < 0 || (size_t)n >= out_size ? -1 : 1;
}

int edwl_url_github_repo(const char *input, char *out, size_t out_size)
{
	char normalized[EDWL_URL_MAX];
	struct edwl_url parsed;
	size_t len;

	if (!out || out_size == 0 ||
	    edwl_url_normalize_github(input, normalized, sizeof(normalized)) !=
		1 ||
	    edwl_url_parse(normalized, &parsed) != 0)
		return -1;
	len = strlen(parsed.path) - strlen("/zip/HEAD") - 1;
	if (len >= out_size)
		return -1;
	memcpy(out, parsed.path + 1, len);
	out[len] = '\0';
	return 0;
}

static int percent_decode(const char *in, char *out, size_t out_size)
{
	size_t oi = 0;

	while (*in) {
		if (oi + 1 >= out_size)
			return -1;
		if (in[0] == '%' && isxdigit((unsigned char)in[1]) &&
		    isxdigit((unsigned char)in[2])) {
			char hex[3];

			hex[0] = in[1];
			hex[1] = in[2];
			hex[2] = '\0';
			out[oi++] = (char)strtol(hex, NULL, 16);
			in += 3;
			continue;
		}
		out[oi++] = *in++;
	}
	out[oi] = '\0';
	return 0;
}

int edwl_url_sanitize_filename(const char *in, char *out, size_t out_size)
{
	size_t i = 0;
	size_t oi = 0;

	if (!in || !out || out_size == 0)
		return -1;

	while (in[i] && oi + 1 < out_size) {
		unsigned char c = (unsigned char)in[i++];

		if (c < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' ||
		    c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
			out[oi++] = '_';
			continue;
		}
		out[oi++] = (char)c;
	}
	out[oi] = '\0';

	/* Windows 不接受结尾的点或空格 */
	while (oi > 0 && (out[oi - 1] == '.' || out[oi - 1] == ' '))
		out[--oi] = '\0';

	return 0;
}

int edwl_url_guess_filename(const char *url, char *out, size_t out_size)
{
	struct edwl_url parsed;
	const char *archive_suffix = NULL;
	char decoded[512];
	const char *path;
	const char *query;
	const char *last;
	size_t len;

	if (!out || out_size == 0)
		return -1;
	out[0] = '\0';

	if (edwl_url_parse(url, &parsed) != 0)
		return -1;

	/* 路径里带查询串的话，文件名只取 '?' 前面那段 */
	query = strchr(parsed.path, '?');
	len = query ? (size_t)(query - parsed.path) : strlen(parsed.path);
	if (ci_equal(parsed.host, "github.com"))
		archive_suffix = "/archive/HEAD.zip";
	else if (ci_equal(parsed.host, "codeload.github.com"))
		archive_suffix = "/zip/HEAD";
	if (archive_suffix && len > strlen(archive_suffix) + 1 &&
	    memcmp(parsed.path + len - strlen(archive_suffix), archive_suffix,
		   strlen(archive_suffix)) == 0) {
		size_t owner_len;
		size_t repo_len;

		if (github_repo_parts(parsed.path + 1,
				      len - strlen(archive_suffix) - 1, 0,
				      &owner_len, &repo_len) == 0) {
			int n =
			    snprintf(out, out_size, "%.*s.zip", (int)repo_len,
				     parsed.path + owner_len + 2);

			return n < 0 || (size_t)n >= out_size ? -1 : 0;
		}
	}
	if (len >= sizeof(decoded))
		return -1;
	memcpy(decoded, parsed.path, len);
	decoded[len] = '\0';

	last = strrchr(decoded, '/');
	path = last ? last + 1 : decoded;
	if (*path == '\0') {
		if (edwl_url_sanitize_filename("index.html", out, out_size) !=
		    0)
			return -1;
		return 0;
	}

	if (percent_decode(path, decoded, sizeof(decoded)) != 0)
		return -1;
	if (edwl_url_sanitize_filename(decoded, out, out_size) != 0)
		return -1;
	if (out[0] == '\0')
		return edwl_url_sanitize_filename("index.html", out, out_size);
	return 0;
}

int edwl_url_format_range(uint64_t begin, uint64_t end, char *out,
			  size_t out_size)
{
	int n;

	if (!out || out_size == 0)
		return -1;
	if (end < begin)
		return -1;
	n = snprintf(out, out_size, "bytes=%llu-%llu",
		     (unsigned long long)begin, (unsigned long long)end);
	if (n < 0 || (size_t)n >= out_size)
		return -1;
	return 0;
}

static const char *skip_spaces(const char *p)
{
	while (*p == ' ' || *p == '\t')
		p++;
	return p;
}

int edwl_url_parse_content_range(const char *value, uint64_t *begin,
				 uint64_t *end, uint64_t *total)
{
	const char *p;
	char *ep;

	if (!value || !begin || !end || !total)
		return -1;

	p = skip_spaces(value);
	if (strncmp(p, "bytes", 5) != 0)
		return -1;
	p = skip_spaces(p + 5);
	if (!isdigit((unsigned char)*p))
		return -1;

	*begin = strtoull(p, &ep, 10);
	if (ep == p)
		return -1;
	p = ep;
	if (*p != '-')
		return -1;
	p++;
	if (!isdigit((unsigned char)*p))
		return -1;
	*end = strtoull(p, &ep, 10);
	if (ep == p)
		return -1;
	p = ep;
	if (*p != '/')
		return -1;
	p++;

	if (*p == '*') {
		*total = 0;
		return 0;
	}
	if (!isdigit((unsigned char)*p))
		return -1;
	*total = strtoull(p, &ep, 10);
	if (ep == p)
		return -1;
	return 0;
}
