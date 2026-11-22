/*
 * edwl - HTTP 抽象层实现（唯一 include <curl/curl.h> 的文件）
 *
 * 四个动作：
 *   probe      —— Range: bytes=0-0 探一下总长和是否认 Range
 *   get_range  —— 拉一个闭区间，按绝对偏移交给 sink 落盘
 *   get_all    —— 整文件顺序拉（服务器不支持 Range 时的回退）
 *   get_text   —— 有上限的 HTTPS 元数据/校验清单，逐跳检查黑名单
 *
 * 一个 edwl_http 对应一个 worker，内部就一个 CURL easy handle。
 * 顺带说明：HTTP 层不认识"分片"，它只知道 begin/end 和 offset。
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

#include <edwl/core/blacklist.h>
#include <edwl/core/http_client.h>
#include <edwl/platform/platform.h>
#include <edwl/util/buffer.h>
#include <edwl/util/url.h>

#define EDWL_USER_AGENT                                                        \
	"edwl/"                                                                \
	"0.1.0"

struct edwl_http {
	CURL *easy;
	char error[EDWL_HTTP_ERR_LEN];
	struct edwl_http_opts opts;
};

static int curl_ready;
static char version_text[256];

/* 每个 worker 线程一个复用句柄（C11 _Thread_local） */
static _Thread_local struct edwl_http *tls_http;

/* ---------- 小工具 ---------- */

static void set_error(struct edwl_http *h, const char *text)
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

int edwl_http_global_init(void)
{
	if (curl_ready)
		return 0;
	if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0)
		return -1;
	curl_ready = 1;
	snprintf(version_text, sizeof(version_text), "%s", curl_version());
	return 0;
}

void edwl_http_global_fini(void)
{
	if (!curl_ready)
		return;
	curl_global_cleanup();
	curl_ready = 0;
}

const char *edwl_http_version(void)
{
	/* 允许在 global_init 之前调用（--version 就是这种情况） */
	if (!version_text[0])
		snprintf(version_text, sizeof(version_text), "%s",
			 curl_version());
	return version_text;
}

const char *edwl_http_error(struct edwl_http *h)
{
	if (!h)
		return "no handle";
	return h->error[0] ? h->error : "unknown error";
}

int edwl_http_open(struct edwl_http **out, const struct edwl_http_opts *opts)
{
	struct edwl_http *h;

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

void edwl_http_close(struct edwl_http *h)
{
	if (!h)
		return;
	if (h->easy)
		curl_easy_cleanup(h->easy);
	free(h);
}

int edwl_http_thread_get(const struct edwl_http_opts *opts,
			 struct edwl_http **out)
{
	if (!out)
		return -1;
	if (tls_http) {
		*out = tls_http;
		return 0;
	}
	if (edwl_http_open(&tls_http, opts) != 0)
		return -1;
	*out = tls_http;
	return 0;
}

void edwl_http_thread_drop(void)
{
	edwl_http_thread_release();
}

void edwl_http_thread_release(void)
{
	if (!tls_http)
		return;
	edwl_http_close(tls_http);
	tls_http = NULL;
}

static CURL *ensure_easy(struct edwl_http *h)
{
	if (h->easy)
		return h->easy;
	h->easy = curl_easy_init();
	return h->easy;
}

static void setup_common(struct edwl_http *h, const char *url,
			 struct curl_slist *hdrs)
{
	const struct edwl_http_opts *o = &h->opts;
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
			 o->user_agent ? o->user_agent : EDWL_USER_AGENT);
	curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT_MS, connect_ms);
	curl_easy_setopt(e, CURLOPT_TCP_KEEPALIVE, 1L);
	curl_easy_setopt(e, CURLOPT_BUFFERSIZE, 256L * 1024L);
#if LIBCURL_VERSION_NUM >= 0x073600
	curl_easy_setopt(e, CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);
#endif

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
	if (o->verbose)
		curl_easy_setopt(e, CURLOPT_VERBOSE, 1L);
}

/* ---------- 探测 ---------- */

struct probe_ctx {
	struct edwl_http_info *info;
	int stopped_after_headers;
};

static int probe_progress_cb(void *clientp, curl_off_t dltotal,
			     curl_off_t dlnow, curl_off_t ultotal,
			     curl_off_t ulnow)
{
	(void)clientp;
	(void)dltotal;
	(void)dlnow;
	(void)ultotal;
	(void)ulnow;
	return edwl_interrupt_requested();
}

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
	struct edwl_http_info *info = ctx->info;
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

	/* 只探响应头，避免不认 Range 的源把整文件传完再丢弃。 */
	if (line[0] == '\0' && info->status >= 200 && info->status != 301 &&
	    info->status != 302 && info->status != 303 && info->status != 307 &&
	    info->status != 308) {
		ctx->stopped_after_headers = 1;
		return 0;
	}

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

		if (edwl_url_parse_content_range(value, &b, &e, &total) == 0) {
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

int edwl_http_probe(struct edwl_http *h, const char *url,
		    struct edwl_http_info *info)
{
	struct curl_slist *hdrs = NULL;
	struct probe_ctx ctx;
	CURLcode rc;
	char *effective = NULL;
	long code = 0;
	int ret = EDWL_HTTP_OK;

	if (!h || !url || !info)
		return EDWL_HTTP_ERR;

	memset(info, 0, sizeof(*info));
	memset(&ctx, 0, sizeof(ctx));
	ctx.info = info;

	if (ensure_easy(h) == NULL) {
		set_error(h, "cannot create curl handle");
		return EDWL_HTTP_ERR;
	}
	curl_easy_reset(h->easy);

	hdrs = curl_slist_append(hdrs, "Accept-Encoding: identity");
	hdrs = curl_slist_append(hdrs, "Range: bytes=0-0");
	if (!hdrs) {
		set_error(h, "out of memory");
		return EDWL_HTTP_ERR;
	}

	setup_common(h, url, hdrs);
	curl_easy_setopt(h->easy, CURLOPT_WRITEFUNCTION, discard_write_cb);
	curl_easy_setopt(h->easy, CURLOPT_WRITEDATA, NULL);
	curl_easy_setopt(h->easy, CURLOPT_HEADERFUNCTION, probe_header_cb);
	curl_easy_setopt(h->easy, CURLOPT_HEADERDATA, &ctx);
	curl_easy_setopt(h->easy, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(h->easy, CURLOPT_XFERINFOFUNCTION, probe_progress_cb);

	rc = curl_easy_perform(h->easy);
	curl_slist_free_all(hdrs);

	curl_easy_getinfo(h->easy, CURLINFO_RESPONSE_CODE, &code);
	curl_easy_getinfo(h->easy, CURLINFO_EFFECTIVE_URL, &effective);
	if (effective && strlen(effective) >= sizeof(info->effective_url)) {
		set_error(h, "final URL exceeds size limit");
		return EDWL_HTTP_ERR;
	}
	if (effective)
		copy_field(info->effective_url, sizeof(info->effective_url),
			   effective);

	info->status = code;
	if (rc != CURLE_OK &&
	    !(rc == CURLE_WRITE_ERROR && ctx.stopped_after_headers)) {
		set_error(h, curl_easy_strerror(rc));
		return rc == CURLE_ABORTED_BY_CALLBACK
			   ? EDWL_HTTP_ERR_INTERRUPTED
			   : EDWL_HTTP_ERR;
	}
	if (code == 206)
		info->range_supported = 1;
	else if (code == 200)
		info->range_supported = 0;

	if (code != 200 && code != 206) {
		char msg[EDWL_HTTP_ERR_LEN];

		snprintf(msg, sizeof(msg), "HTTP %ld", code);
		set_error(h, msg);
		ret = EDWL_HTTP_ERR;
	}

	return ret;
}

/* ---------- 传输 ---------- */

struct fetch_ctx {
	const struct edwl_http_sink *sink;
	uint64_t base; /* 本段在文件里的起点 */
	uint64_t got;  /* 本次已写入字节 */
	long status;
	int range_request;
	int manual_redirects;
	int io_error;
	int stopped;
};

static size_t fetch_header_cb(char *buffer, size_t size, size_t nitems,
			      void *userdata)
{
	struct fetch_ctx *ctx = userdata;
	size_t len = size * nitems;
	char line[64];
	const char *p;

	if (len >= sizeof(line))
		len = sizeof(line) - 1;
	memcpy(line, buffer, len);
	line[len] = '\0';
	if (ci_prefix(line, "HTTP/")) {
		p = strchr(line, ' ');
		ctx->status = p ? strtol(p + 1, NULL, 10) : 0;
	}
	return size * nitems;
}

static size_t fetch_write_cb(char *ptr, size_t size, size_t nmemb,
			     void *userdata)
{
	struct fetch_ctx *ctx = userdata;
	size_t len = size * nmemb;

	if (len == 0)
		return 0;
	/* 错误页和无视 Range 的整文件不能推进分片断点。 */
	if (ctx->status != (ctx->range_request ? 206 : 200))
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

	if (edwl_interrupt_requested()) {
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

static int classify(CURLcode rc, struct fetch_ctx *ctx, struct edwl_http *h,
		    long code)
{
	if (rc == CURLE_ABORTED_BY_CALLBACK) {
		set_error(h, ctx->stopped ? "aborted" : "aborted by callback");
		return EDWL_HTTP_ERR_INTERRUPTED;
	}
	if (ctx->range_request && code == 200) {
		set_error(h, "server ignored Range");
		return EDWL_HTTP_ERR_RANGE_IGNORED;
	}
	if (code >= 200 && code != (ctx->range_request ? 206 : 200)) {
		char msg[EDWL_HTTP_ERR_LEN];

		snprintf(msg, sizeof(msg), "HTTP %ld", code);
		set_error(h, msg);
		return EDWL_HTTP_ERR;
	}
	if (rc == CURLE_WRITE_ERROR) {
		set_error(h, ctx->io_error ? "local write failed"
					   : "transfer write error");
		return EDWL_HTTP_ERR_LOCAL_IO;
	}
	if (rc != CURLE_OK) {
		set_error(h, curl_easy_strerror(rc));
		return EDWL_HTTP_ERR;
	}
	if (code != (ctx->range_request ? 206 : 200)) {
		char msg[EDWL_HTTP_ERR_LEN];

		snprintf(msg, sizeof(msg), "HTTP %ld", code);
		set_error(h, msg);
		return EDWL_HTTP_ERR;
	}
	return EDWL_HTTP_OK;
}

static int run_transfer(struct edwl_http *h, const char *url,
			struct curl_slist *hdrs, struct fetch_ctx *ctx,
			long *code_out)
{
	CURLcode rc;

	setup_common(h, url, hdrs);
	if (ctx->manual_redirects)
		curl_easy_setopt(h->easy, CURLOPT_FOLLOWLOCATION, 0L);
	curl_easy_setopt(h->easy, CURLOPT_WRITEFUNCTION, fetch_write_cb);
	curl_easy_setopt(h->easy, CURLOPT_WRITEDATA, ctx);
	curl_easy_setopt(h->easy, CURLOPT_HEADERFUNCTION, fetch_header_cb);
	curl_easy_setopt(h->easy, CURLOPT_HEADERDATA, ctx);
	curl_easy_setopt(h->easy, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(h->easy, CURLOPT_XFERINFOFUNCTION, fetch_progress_cb);
	curl_easy_setopt(h->easy, CURLOPT_XFERINFODATA, ctx);

	rc = curl_easy_perform(h->easy);
	*code_out = 0;
	curl_easy_getinfo(h->easy, CURLINFO_RESPONSE_CODE, code_out);
	return classify(rc, ctx, h, *code_out);
}

int edwl_http_get_range(struct edwl_http *h, const char *url, uint64_t begin,
			uint64_t end, const struct edwl_http_sink *sink,
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
		return EDWL_HTTP_ERR;
	if (end < begin)
		return EDWL_HTTP_ERR;

	if (edwl_url_format_range(begin, end, spec, sizeof(spec)) != 0) {
		set_error(h, "bad range");
		return EDWL_HTTP_ERR;
	}
	snprintf(header, sizeof(header), "Range: %s", spec);

	if (ensure_easy(h) == NULL) {
		set_error(h, "cannot create curl handle");
		return EDWL_HTTP_ERR;
	}
	curl_easy_reset(h->easy);

	hdrs = curl_slist_append(hdrs, "Accept-Encoding: identity");
	hdrs = curl_slist_append(hdrs, header);
	if (!hdrs) {
		set_error(h, "out of memory");
		return EDWL_HTTP_ERR;
	}

	memset(&ctx, 0, sizeof(ctx));
	ctx.sink = sink;
	ctx.base = begin;
	ctx.range_request = 1;

	ret = run_transfer(h, url, hdrs, &ctx, &code);
	curl_slist_free_all(hdrs);
	*got = ctx.got;

	if (ret != EDWL_HTTP_OK)
		return ret;

	expect = end - begin + 1;
	if (ctx.got < expect) {
		char msg[EDWL_HTTP_ERR_LEN];

		snprintf(msg, sizeof(msg), "short read %llu/%llu",
			 (unsigned long long)ctx.got,
			 (unsigned long long)expect);
		set_error(h, msg);
		return EDWL_HTTP_ERR_SHORT;
	}

	return EDWL_HTTP_OK;
}

int edwl_http_get_all(struct edwl_http *h, const char *url,
		      const struct edwl_http_sink *sink, uint64_t *got)
{
	struct curl_slist *hdrs = NULL;
	struct fetch_ctx ctx;
	long code = 0;
	int ret;

	if (!h || !url || !sink || !got || !sink->write)
		return EDWL_HTTP_ERR;

	if (ensure_easy(h) == NULL) {
		set_error(h, "cannot create curl handle");
		return EDWL_HTTP_ERR;
	}
	curl_easy_reset(h->easy);

	hdrs = curl_slist_append(hdrs, "Accept-Encoding: identity");
	if (!hdrs) {
		set_error(h, "out of memory");
		return EDWL_HTTP_ERR;
	}

	memset(&ctx, 0, sizeof(ctx));
	ctx.sink = sink;
	ctx.base = 0;

	ret = run_transfer(h, url, hdrs, &ctx, &code);
	curl_slist_free_all(hdrs);

	*got = ctx.got;
	if (ret != EDWL_HTTP_OK)
		return ret;
	if (code != 200) {
		char msg[EDWL_HTTP_ERR_LEN];

		snprintf(msg, sizeof(msg), "HTTP %ld", code);
		set_error(h, msg);
		return EDWL_HTTP_ERR;
	}
	return EDWL_HTTP_OK;
}

struct text_sink {
	struct edwl_buf *buf;
	size_t max_size;
	const char *error;
};

static int text_write(void *ctx, const void *data, size_t len, uint64_t offset)
{
	struct text_sink *text = ctx;

	if (len > text->max_size - text->buf->len) {
		text->error = "text response exceeds size limit";
		return -1;
	}
	if (memchr(data, '\0', len)) {
		text->error = "text response contains NUL bytes";
		return -1;
	}
	if (offset != text->buf->len ||
	    edwl_buf_append(text->buf, data, len) != 0) {
		text->error = "cannot buffer text response";
		return -1;
	}
	return 0;
}

int edwl_http_get_text(struct edwl_http *h, const char *url, const char *accept,
		       size_t max_size, struct edwl_buf *buf)
{
	char target[EDWL_URL_MAX];
	char accept_header[256];
	unsigned redirects = 0;
	unsigned max_redirects;
	struct curl_slist *hdrs = NULL;
	int ret = EDWL_HTTP_ERR;

	if (!h || !url || !buf || !buf->data || max_size == 0)
		return EDWL_HTTP_ERR;
	if (strlen(url) >= sizeof(target)) {
		set_error(h, "URL exceeds size limit");
		return EDWL_HTTP_ERR;
	}
	copy_field(target, sizeof(target), url);
	max_redirects = h->opts.max_redirects ? h->opts.max_redirects : 10;
	if (ensure_easy(h) == NULL) {
		set_error(h, "cannot create curl handle");
		return EDWL_HTTP_ERR;
	}
	hdrs = curl_slist_append(NULL, "Accept-Encoding: identity");
	if (!hdrs) {
		set_error(h, "out of memory");
		goto out;
	}
	if (accept) {
		struct curl_slist *next;
		int n = snprintf(accept_header, sizeof(accept_header),
				 "Accept: %s", accept);

		if (n < 0 || (size_t)n >= sizeof(accept_header)) {
			set_error(h, "Accept header exceeds size limit");
			goto out;
		}
		next = curl_slist_append(hdrs, accept_header);
		if (!next) {
			set_error(h, "out of memory");
			goto out;
		}
		hdrs = next;
	}
	for (;;) {
		struct edwl_url parsed;
		struct text_sink text = {buf, max_size, NULL};
		struct edwl_http_sink sink = {text_write, NULL, &text};
		struct fetch_ctx ctx = {0};
		long status = 0;
		char *next_url = NULL;
		char why[256];

		if (edwl_url_parse(target, &parsed) != 0 ||
		    strcmp(parsed.scheme, "https") != 0) {
			set_error(h, "metadata requires an HTTPS URL");
			ret = EDWL_HTTP_ERR;
			break;
		}
		if (h->opts.blacklist &&
		    edwl_blacklist_hit_url(h->opts.blacklist, target, why,
					   sizeof(why))) {
			set_error(h, why);
			ret = EDWL_HTTP_ERR;
			break;
		}
		curl_easy_reset(h->easy);
		edwl_buf_reset(buf);
		ctx.sink = &sink;
		ctx.manual_redirects = 1;
		ret = run_transfer(h, target, hdrs, &ctx, &status);
		if (text.error) {
			set_error(h, text.error);
			ret = EDWL_HTTP_ERR;
			break;
		}
		if (ret == EDWL_HTTP_ERR_INTERRUPTED)
			break;
		if (status != 301 && status != 302 && status != 303 &&
		    status != 307 && status != 308) {
			if (ret == EDWL_HTTP_OK && status != 200) {
				set_error(h, "metadata requires a complete "
					     "HTTP 200 response");
				ret = EDWL_HTTP_ERR;
			}
			break;
		}
		if (++redirects > max_redirects) {
			set_error(h, "too many metadata redirects");
			ret = EDWL_HTTP_ERR;
			break;
		}
		curl_easy_getinfo(h->easy, CURLINFO_REDIRECT_URL, &next_url);
		if (!next_url || strlen(next_url) >= sizeof(target)) {
			set_error(h, "missing or oversized redirect URL");
			ret = EDWL_HTTP_ERR;
			break;
		}
		copy_field(target, sizeof(target), next_url);
	}
out:
	curl_slist_free_all(hdrs);
	return ret;
}
