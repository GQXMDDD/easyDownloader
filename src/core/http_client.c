/*
 * edwl - HTTP 抽象层实现（唯一 include <curl/curl.h> 的文件）
 *
 * 三个动作：
 *   probe      —— Range: bytes=0-0 探一下总长和是否认 Range
 *   get_range  —— 拉一个闭区间，按绝对偏移交给 sink 落盘
 *   get_all    —— 整文件顺序拉（服务器不支持 Range 时的回退）
 *
 * 一个 wgetx_http 对应一个 worker，内部就一个 CURL easy handle。
 * 顺带说明：HTTP 层不认识"分片"，它只知道 begin/end 和 offset。
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

#include <edwl/core/http_client.h>
#include <edwl/platform/platform.h>
#include <edwl/util/url.h>

#define WGETX_USER_AGENT                                                       \
	"edwl/"                                                                \
	"0.1.0"

struct wgetx_http {
	CURL *easy;
	char error[WGETX_HTTP_ERR_LEN];
	struct wgetx_http_opts opts;
};

static int curl_ready;
static char version_text[256];

/* ---------- 小工具 ---------- */

static void set_error(struct wgetx_http *h, const char *text)
{
	if (!h || !text)
		return;
	snprintf(h->error, sizeof(h->error), "%s", text);
}

static void copy_field(char *dst, size_t size, const char *src)
{
	size_t n;

	if (!dst || size == 0)
		return;
	if (!src) {
		dst[0] = '\0';
		return;
	}
	n = strlen(src);
	if (n >= size)
		n = size - 1;
	memcpy(dst, src, n);
	dst[n] = '\0';
}

static int ci_prefix(const char *s, const char *prefix)
{
	while (*prefix) {
		if (tolower((unsigned char)*s) !=
		    tolower((unsigned char)*prefix))
			return 0;
		s++;
		prefix++;
	}
	return 1;
}

static const char *skip_space(const char *s)
{
	while (*s == ' ' || *s == '\t')
		s++;
	return s;
}

static void strip_eol(char *s)
{
	size_t n = strlen(s);

	while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r'))
		s[--n] = '\0';
}

/* ---------- 生命周期 ---------- */

int wgetx_http_global_init(void)
{
	if (curl_ready)
		return 0;
	if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0)
		return -1;
	curl_ready = 1;
	snprintf(version_text, sizeof(version_text), "%s", curl_version());
	return 0;
}

void wgetx_http_global_fini(void)
{
	if (!curl_ready)
		return;
	curl_global_cleanup();
	curl_ready = 0;
}

const char *wgetx_http_version(void)
{
	/* 允许在 global_init 之前调用（--version 就是这种情况） */
	if (!version_text[0])
		snprintf(version_text, sizeof(version_text), "%s",
			 curl_version());
	return version_text;
}

const char *wgetx_http_error(struct wgetx_http *h)
{
	if (!h)
		return "no handle";
	return h->error[0] ? h->error : "unknown error";
}

int wgetx_http_open(struct wgetx_http **out, const struct wgetx_http_opts *opts)
{
	struct wgetx_http *h;

	if (!out)
		return -1;

	h = calloc(1, sizeof(*h));
	if (!h)
		return -1;

	if (opts)
		h->opts = *opts;

	/*
	 * easy handle 推迟到第一次请求再建：probe 与 worker 都是短命对象，
	 * 建早了只是白白占内存。
	 */
	*out = h;
	return 0;
}

void wgetx_http_close(struct wgetx_http *h)
{
	if (!h)
		return;
	if (h->easy)
		curl_easy_cleanup(h->easy);
	free(h);
}

static CURL *ensure_easy(struct wgetx_http *h)
{
	if (h->easy)
		return h->easy;
	h->easy = curl_easy_init();
	return h->easy;
}

static void setup_common(struct wgetx_http *h, const char *url,
			 struct curl_slist *hdrs)
{
	const struct wgetx_http_opts *o = &h->opts;
	CURL *e = h->easy;
	long connect_ms = 15000;
	long maxredir = 10;

	if (o->connect_timeout_ms)
		connect_ms = (long)o->connect_timeout_ms;
	if (o->max_redirects)
		maxredir = (long)o->max_redirects;

	curl_easy_setopt(e, CURLOPT_URL, url);
	curl_easy_setopt(e, CURLOPT_HTTPHEADER, hdrs);
	curl_easy_setopt(e, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(e, CURLOPT_MAXREDIRS, maxredir);
	curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(e, CURLOPT_USERAGENT,
			 o->user_agent ? o->user_agent : WGETX_USER_AGENT);
	curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT_MS, connect_ms);
	curl_easy_setopt(e, CURLOPT_TCP_KEEPALIVE, 1L);

	if (o->low_speed_time_ms > 0) {
		long secs = (long)((o->low_speed_time_ms + 999) / 1000);

		curl_easy_setopt(
		    e, CURLOPT_LOW_SPEED_LIMIT,
		    (long)(o->low_speed_limit ? o->low_speed_limit : 1));
		curl_easy_setopt(e, CURLOPT_LOW_SPEED_TIME, secs);
	}
	if (o->max_recv_speed > 0) {
		curl_easy_setopt(e, CURLOPT_MAX_RECV_SPEED_LARGE,
				 (curl_off_t)o->max_recv_speed);
	}
	if (o->proxy) {
		/* "" 表示强制直连，覆盖环境变量里的代理 */
		curl_easy_setopt(e, CURLOPT_PROXY, o->proxy);
	}
}

/* ---------- 探测 ---------- */

struct probe_ctx {
	struct wgetx_http_info *info;
};

static size_t discard_write_cb(char *ptr, size_t size, size_t nmemb,
			       void *userdata)
{
	(void)ptr;
	(void)userdata;
	return size * nmemb;
}

static size_t probe_header_cb(char *buffer, size_t size, size_t nitems,
			      void *userdata)
{
	struct probe_ctx *ctx = userdata;
	struct wgetx_http_info *info = ctx->info;
	size_t len = size * nitems;
	char line[512];
	char *colon;
	const char *value;
	const char *p;

	if (len >= sizeof(line))
		len = sizeof(line) - 1;
	memcpy(line, buffer, len);
	line[len] = '\0';
	strip_eol(line);

	/* 换一次响应（含重定向）就清一次，只留最后一组的头 */
	if (ci_prefix(line, "HTTP/")) {
		long status = 0;

		p = strchr(line, ' ');
		if (p)
			status = strtol(p + 1, NULL, 10);
		memset(info, 0, sizeof(*info));
		info->status = status;
		return size * nitems;
	}

	colon = strchr(line, ':');
	if (!colon)
		return size * nitems;
	*colon = '\0';
	value = skip_space(colon + 1);

	if (ci_prefix(line, "Content-Length")) {
		/* 206 的 Content-Length 是分片长度，不是文件总长，忽略 */
		if (info->status != 206)
			info->content_length = strtoull(value, NULL, 10);
	} else if (ci_prefix(line, "Content-Range")) {
		uint64_t b = 0, e = 0, total = 0;

		if (wgetx_url_parse_content_range(value, &b, &e, &total) == 0) {
			if (total > 0)
				info->content_length = total;
			info->range_supported = 1;
		}
	} else if (ci_prefix(line, "Accept-Ranges")) {
		if (ci_prefix(value, "bytes"))
			info->range_supported = 1;
	} else if (ci_prefix(line, "ETag")) {
		copy_field(info->etag, sizeof(info->etag), value);
	} else if (ci_prefix(line, "Last-Modified")) {
		copy_field(info->last_modified, sizeof(info->last_modified),
			   value);
	} else if (ci_prefix(line, "Content-Type")) {
		copy_field(info->content_type, sizeof(info->content_type),
			   value);
	}

	return size * nitems;
}

int wgetx_http_probe(struct wgetx_http *h, const char *url,
		     struct wgetx_http_info *info)
{
	struct curl_slist *hdrs = NULL;
	struct probe_ctx ctx;
	CURLcode rc;
	char *effective = NULL;
	long code = 0;
	int ret = WGETX_HTTP_OK;

	if (!h || !url || !info)
		return WGETX_HTTP_ERR;

	memset(info, 0, sizeof(*info));
	memset(&ctx, 0, sizeof(ctx));
	ctx.info = info;

	if (ensure_easy(h) == NULL) {
		set_error(h, "cannot create curl handle");
		return WGETX_HTTP_ERR;
	}
	curl_easy_reset(h->easy);

	hdrs = curl_slist_append(hdrs, "Accept-Encoding: identity");
	hdrs = curl_slist_append(hdrs, "Range: bytes=0-0");
	if (!hdrs) {
		set_error(h, "out of memory");
		return WGETX_HTTP_ERR;
	}

	setup_common(h, url, hdrs);
	curl_easy_setopt(h->easy, CURLOPT_WRITEFUNCTION, discard_write_cb);
	curl_easy_setopt(h->easy, CURLOPT_WRITEDATA, NULL);
	curl_easy_setopt(h->easy, CURLOPT_HEADERFUNCTION, probe_header_cb);
	curl_easy_setopt(h->easy, CURLOPT_HEADERDATA, &ctx);

	rc = curl_easy_perform(h->easy);
	curl_slist_free_all(hdrs);

	if (rc != CURLE_OK) {
		char msg[WGETX_HTTP_ERR_LEN];

		snprintf(msg, sizeof(msg), "%s", curl_easy_strerror(rc));
		set_error(h, msg);
		return WGETX_HTTP_ERR;
	}

	curl_easy_getinfo(h->easy, CURLINFO_RESPONSE_CODE, &code);
	curl_easy_getinfo(h->easy, CURLINFO_EFFECTIVE_URL, &effective);
	if (effective)
		copy_field(info->effective_url, sizeof(info->effective_url),
			   effective);

	info->status = code;
	if (code == 206)
		info->range_supported = 1;
	else if (code == 200)
		info->range_supported = 0;

	if (code != 200 && code != 206) {
		char msg[WGETX_HTTP_ERR_LEN];

		snprintf(msg, sizeof(msg), "HTTP %ld", code);
		set_error(h, msg);
		ret = WGETX_HTTP_ERR;
	}

	return ret;
}

/* ---------- 传输 ---------- */

struct fetch_ctx {
	const struct wgetx_http_sink *sink;
	uint64_t base; /* 本段在文件里的起点 */
	uint64_t got;  /* 本次已写入字节 */
	int io_error;
	int stopped;
};

static size_t fetch_write_cb(char *ptr, size_t size, size_t nmemb,
			     void *userdata)
{
	struct fetch_ctx *ctx = userdata;
	size_t len = size * nmemb;

	if (len == 0)
		return 0;

	if (ctx->sink->write(ctx->sink->ctx, ptr, len, ctx->base + ctx->got) !=
	    0) {
		ctx->io_error = 1;
		return 0; /* 让 curl 以 CURLE_WRITE_ERROR 收场 */
	}

	ctx->got += len;
	return len;
}

static int fetch_progress_cb(void *clientp, curl_off_t dltotal,
			     curl_off_t dlnow, curl_off_t ultotal,
			     curl_off_t ulnow)
{
	struct fetch_ctx *ctx = clientp;

	(void)dltotal;
	(void)dlnow;
	(void)ultotal;
	(void)ulnow;

	if (wgetx_interrupt_requested()) {
		ctx->stopped = 1;
		return 1;
	}
	if (ctx->sink->progress &&
	    ctx->sink->progress(ctx->sink->ctx, ctx->got) != 0) {
		ctx->stopped = 1;
		return 1;
	}
	return 0;
}

static int classify(CURLcode rc, struct fetch_ctx *ctx, struct wgetx_http *h,
		    long code)
{
	if (rc == CURLE_ABORTED_BY_CALLBACK) {
		set_error(h, ctx->stopped ? "aborted" : "aborted by callback");
		return WGETX_HTTP_ERR_INTERRUPTED;
	}
	if (rc == CURLE_WRITE_ERROR) {
		set_error(h, ctx->io_error ? "local write failed"
					   : "transfer write error");
		return WGETX_HTTP_ERR_LOCAL_IO;
	}
	if (rc != CURLE_OK) {
		set_error(h, curl_easy_strerror(rc));
		return WGETX_HTTP_ERR;
	}
	if (code != 200 && code != 206) {
		char msg[WGETX_HTTP_ERR_LEN];

		snprintf(msg, sizeof(msg), "HTTP %ld", code);
		set_error(h, msg);
		return WGETX_HTTP_ERR;
	}
	return WGETX_HTTP_OK;
}

static int run_transfer(struct wgetx_http *h, const char *url,
			struct curl_slist *hdrs, struct fetch_ctx *ctx,
			long *code_out)
{
	CURLcode rc;

	setup_common(h, url, hdrs);
	curl_easy_setopt(h->easy, CURLOPT_WRITEFUNCTION, fetch_write_cb);
	curl_easy_setopt(h->easy, CURLOPT_WRITEDATA, ctx);
	curl_easy_setopt(h->easy, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(h->easy, CURLOPT_XFERINFOFUNCTION, fetch_progress_cb);
	curl_easy_setopt(h->easy, CURLOPT_XFERINFODATA, ctx);

	rc = curl_easy_perform(h->easy);
	*code_out = 0;
	curl_easy_getinfo(h->easy, CURLINFO_RESPONSE_CODE, code_out);
	return classify(rc, ctx, h, *code_out);
}

int wgetx_http_get_range(struct wgetx_http *h, const char *url, uint64_t begin,
			 uint64_t end, const struct wgetx_http_sink *sink,
			 uint64_t *got)
{
	struct curl_slist *hdrs = NULL;
	struct fetch_ctx ctx;
	char spec[64];
	char header[96];
	long code = 0;
	uint64_t expect;
	int ret;

	if (!h || !url || !sink || !got || !sink->write)
		return WGETX_HTTP_ERR;
	if (end < begin)
		return WGETX_HTTP_ERR;

	if (wgetx_url_format_range(begin, end, spec, sizeof(spec)) != 0) {
		set_error(h, "bad range");
		return WGETX_HTTP_ERR;
	}
	snprintf(header, sizeof(header), "Range: %s", spec);

	if (ensure_easy(h) == NULL) {
		set_error(h, "cannot create curl handle");
		return WGETX_HTTP_ERR;
	}
	curl_easy_reset(h->easy);

	hdrs = curl_slist_append(hdrs, "Accept-Encoding: identity");
	hdrs = curl_slist_append(hdrs, header);
	if (!hdrs) {
		set_error(h, "out of memory");
		return WGETX_HTTP_ERR;
	}

	memset(&ctx, 0, sizeof(ctx));
	ctx.sink = sink;
	ctx.base = begin;

	ret = run_transfer(h, url, hdrs, &ctx, &code);
	curl_slist_free_all(hdrs);

	if (ret != WGETX_HTTP_OK)
		return ret;

	/* 服务器无视 Range，直接回了整个文件：绝不能按分片偏移落盘 */
	if (code == 200) {
		set_error(h, "server ignored Range");
		return WGETX_HTTP_ERR_RANGE_IGNORED;
	}

	*got = ctx.got;
	expect = end - begin + 1;
	if (ctx.got < expect) {
		char msg[WGETX_HTTP_ERR_LEN];

		snprintf(msg, sizeof(msg), "short read %llu/%llu",
			 (unsigned long long)ctx.got,
			 (unsigned long long)expect);
		set_error(h, msg);
		return WGETX_HTTP_ERR_SHORT;
	}

	return WGETX_HTTP_OK;
}

int wgetx_http_get_all(struct wgetx_http *h, const char *url,
		       const struct wgetx_http_sink *sink, uint64_t *got)
{
	struct curl_slist *hdrs = NULL;
	struct fetch_ctx ctx;
	long code = 0;
	int ret;

	if (!h || !url || !sink || !got || !sink->write)
		return WGETX_HTTP_ERR;

	if (ensure_easy(h) == NULL) {
		set_error(h, "cannot create curl handle");
		return WGETX_HTTP_ERR;
	}
	curl_easy_reset(h->easy);

	hdrs = curl_slist_append(hdrs, "Accept-Encoding: identity");
	if (!hdrs) {
		set_error(h, "out of memory");
		return WGETX_HTTP_ERR;
	}

	memset(&ctx, 0, sizeof(ctx));
	ctx.sink = sink;
	ctx.base = 0;

	ret = run_transfer(h, url, hdrs, &ctx, &code);
	curl_slist_free_all(hdrs);

	*got = ctx.got;
	if (ret != WGETX_HTTP_OK)
		return ret;
	if (code != 200) {
		char msg[WGETX_HTTP_ERR_LEN];

		snprintf(msg, sizeof(msg), "HTTP %ld", code);
		set_error(h, msg);
		return WGETX_HTTP_ERR;
	}
	return WGETX_HTTP_OK;
}
