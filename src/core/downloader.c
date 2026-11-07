/*
 * edwl - 下载器主控实现
 *
 * 一次下载的完整流程：
 *   1. 探测（找可用源，拿总长 / 是否支持 Range / ETag）
 *   2. 切片（或读取断点元数据）
 *   3. 预分配输出文件，起线程池并发拉取
 *   4. 监控线程刷进度、定期落断点
 *   5. 收尾：确认无缺口、可选校验哈希、删断点文件
 *
 * 两条路径：
 *   并行（服务器认 Range，且允许分片）—— 每片一条连接，pwrite 定位写
 *   单流（不支持 Range / 用户禁用 / 总长未知）—— 一条连接顺序写
 *
 * 所有平台差异都在 platform/ 后面，本文件不出现任何系统头。
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <edwl/core/checksum.h>
#include <edwl/core/downloader.h>
#include <edwl/core/http_client.h>
#include <edwl/core/segment.h>
#include <edwl/core/source.h>
#include <edwl/core/thread_pool.h>
#include <edwl/platform/platform.h>
#include <edwl/util/log.h>
#include <edwl/util/url.h>
#include <edwl/edwl.h>

#define META_MAGIC "edwl-meta 1"
#define META_SUFFIX ".edwl"
#define DEFAULT_THREADS 8
#define DEFAULT_MAX_PIECES 512
#define TICK_MS 200
#define PLAIN_TICK_MS 2000
#define META_INTERVAL_MS 1000
#define BACKOFF_STEP_MS 300
#define BACKOFF_MAX_MS 3000
#define PATH_LEN 1024

struct dl_state {
	const struct edwl_download_opts *opts;
	struct edwl_sources *sources;
	struct edwl_segment_map map;
	struct edwl_mutex *mtx; /* 保护 map / single_done / 计数 */
	edwl_file_t file;

	uint64_t total;
	char output[PATH_LEN];
	char meta_path[PATH_LEN + 16];
	char etag[128];
	char last_modified[64];

	int using_map;
	uint64_t single_done;
	uint64_t written;
	uint64_t last_meta_ms;
	uint64_t start_ms;

	volatile int fatal; /* 遇到不可恢复的错误 */
	int interrupted;
	int resumed;
	char last_error[EDWL_HTTP_ERR_LEN];
};

struct monitor {
	struct dl_state *st;
	int stop;
	uint64_t start_ms;
	uint64_t last_ms;
	uint64_t last_done;
	uint64_t last_draw_ms;
	uint64_t speed;
};

struct seg_io {
	struct dl_state *st;
	struct edwl_segment *seg;
};

struct single_io {
	struct dl_state *st;
};

/* ---------- 默认值 ---------- */

void edwl_download_opts_default(struct edwl_download_opts *o)
{
	if (!o)
		return;
	memset(o, 0, sizeof(*o));
	o->threads = DEFAULT_THREADS;
	o->min_piece_size = 1024ULL * 1024ULL;
	o->max_piece_size = 64ULL * 1024ULL * 1024ULL;
	o->max_pieces = DEFAULT_MAX_PIECES;
	o->retries = 3;
	o->connect_timeout_ms = 15000;
	o->low_speed_time_ms = 30000;
	o->low_speed_limit = 1;
	o->max_speed = 0;
	o->allow_resume = 1;
	o->allow_range = 1;
	o->show_progress = 1;
	o->quiet = 0;
}

/* 调用方可能只填几个字段，其余补默认值 */
static void fill_defaults(struct edwl_download_opts *o)
{
	if (o->threads == 0)
		o->threads = DEFAULT_THREADS;
	if (o->threads > 64)
		o->threads = 64;
	if (o->min_piece_size == 0)
		o->min_piece_size = 1024ULL * 1024ULL;
	if (o->max_piece_size < o->min_piece_size)
		o->max_piece_size = 64ULL * 1024ULL * 1024ULL;
	if (o->max_pieces == 0)
		o->max_pieces = DEFAULT_MAX_PIECES;
	if (o->connect_timeout_ms == 0)
		o->connect_timeout_ms = 15000;
	if (o->low_speed_time_ms == 0)
		o->low_speed_time_ms = 30000;
	if (o->low_speed_limit == 0)
		o->low_speed_limit = 1;
}

static void set_last_error(struct dl_state *st, const char *text)
{
	if (!st || !text)
		return;
	snprintf(st->last_error, sizeof(st->last_error), "%.*s",
		 (int)(sizeof(st->last_error) - 1), text);
}

static uint32_t backoff_ms(unsigned attempt)
{
	uint64_t ms = (uint64_t)BACKOFF_STEP_MS << (attempt > 3 ? 3 : attempt);

	if (ms > BACKOFF_MAX_MS)
		ms = BACKOFF_MAX_MS;
	return (uint32_t)ms;
}

/* ---------- 输出路径 ---------- */

static int resolve_output(struct dl_state *st)
{
	char name[512];
	const char *out = st->opts->output;
	size_t len;

	if (out && out[0]) {
		len = strlen(out);
		if (out[len - 1] != '/' && out[len - 1] != '\\') {
			if (len >= sizeof(st->output))
				return -1;
			memcpy(st->output, out, len + 1);
		} else {
			if (edwl_url_guess_filename(st->opts->urls[0], name,
						    sizeof(name)) != 0)
				return -1;
			snprintf(st->output, sizeof(st->output), "%s%s", out,
				 name);
		}
	} else {
		if (edwl_url_guess_filename(st->opts->urls[0], name,
					    sizeof(name)) != 0)
			return -1;
		snprintf(st->output, sizeof(st->output), "%s", name);
	}

	snprintf(st->meta_path, sizeof(st->meta_path), "%s%s", st->output,
		 META_SUFFIX);
	return 0;
}

static int open_output(struct dl_state *st)
{
	uint64_t prealloc = st->total;

	if (edwl_file_open_write(st->output, &st->file) != 0) {
		char msg[PATH_LEN + 64];

		snprintf(msg, sizeof(msg), "无法打开输出文件: %s", st->output);
		set_last_error(st, msg);
		return -1;
	}
	if (edwl_file_truncate(st->file, prealloc) != 0) {
		set_last_error(st, "无法预分配输出文件大小");
		edwl_file_close(st->file);
		st->file = (edwl_file_t)-1;
		return -1;
	}
	return 0;
}

/* ---------- 断点元数据 ---------- */

struct meta_piece {
	uint64_t begin;
	uint64_t end;
	uint64_t pos;
};

static int write_meta_body(FILE *f, struct dl_state *st,
			   const struct edwl_segment *snap, size_t count)
{
	size_t i;

	if (fprintf(f, "%s\n", META_MAGIC) < 0)
		return -1;
	if (fprintf(f, "size %llu\n", (unsigned long long)st->total) < 0)
		return -1;
	if (fprintf(f, "etag %s\n", st->etag) < 0)
		return -1;
	if (fprintf(f, "mtime %s\n", st->last_modified) < 0)
		return -1;
	if (fprintf(f, "pieces %zu\n", count) < 0)
		return -1;

	for (i = 0; i < count; i++) {
		if (fprintf(f, "piece %llu %llu %llu\n",
			    (unsigned long long)snap[i].begin,
			    (unsigned long long)snap[i].end,
			    (unsigned long long)snap[i].pos) < 0)
			return -1;
	}
	return 0;
}

static int save_meta(struct dl_state *st)
{
	struct edwl_segment *snap = NULL;
	char tmp[PATH_LEN + 32];
	size_t count = 0;
	FILE *f = NULL;
	int rc = -1;

	if (!st->using_map || st->total == 0)
		return -1;

	edwl_mutex_lock(st->mtx);
	count = st->map.count;
	if (count > 0) {
		snap = malloc(count * sizeof(*snap));
		if (snap)
			memcpy(snap, st->map.segs, count * sizeof(*snap));
	}
	edwl_mutex_unlock(st->mtx);

	if (!snap)
		return -1;

	snprintf(tmp, sizeof(tmp), "%s.tmp", st->meta_path);
	f = fopen(tmp, "wb");
	if (!f)
		goto done;
	if (write_meta_body(f, st, snap, count) != 0)
		goto done;
	if (fclose(f) != 0) {
		f = NULL;
		goto done;
	}
	f = NULL;

	/* Windows 上 rename 不覆盖已存在文件，先删再换 */
	remove(st->meta_path);
	if (rename(tmp, st->meta_path) != 0) {
		remove(tmp);
		goto done;
	}
	rc = 0;

done:
	if (f)
		fclose(f);
	free(snap);
	return rc;
}

static int meta_pieces_valid(const struct meta_piece *pieces, size_t count,
			     uint64_t total)
{
	size_t i;

	if (count == 0 || pieces[0].begin != 0)
		return 0;
	if (pieces[count - 1].end != total - 1)
		return 0;

	for (i = 0; i < count; i++) {
		if (pieces[i].begin > pieces[i].end)
			return 0;
		if (pieces[i].pos < pieces[i].begin ||
		    pieces[i].pos > pieces[i].end + 1)
			return 0;
		if (i > 0 && pieces[i].begin != pieces[i - 1].end + 1)
			return 0;
	}
	return 1;
}

static int install_meta_pieces(struct dl_state *st,
			       const struct meta_piece *pieces, size_t count)
{
	struct edwl_segment *segs;
	size_t i;

	segs = calloc(count, sizeof(*segs));
	if (!segs)
		return -1;

	for (i = 0; i < count; i++) {
		segs[i].index = (uint64_t)i;
		segs[i].begin = pieces[i].begin;
		segs[i].end = pieces[i].end;
		segs[i].pos = pieces[i].pos;
		segs[i].state = (pieces[i].pos > pieces[i].end)
				    ? EDWL_SEG_DONE
				    : EDWL_SEG_PENDING;
	}

	edwl_segment_map_free(&st->map);
	st->map.segs = segs;
	st->map.count = count;
	st->map.total = st->total;
	return 0;
}

static int parse_piece_line(const char *text, struct meta_piece *out)
{
	unsigned long long b = 0, e = 0, p = 0;

	if (sscanf(text, "%llu %llu %llu", &b, &e, &p) != 3)
		return -1;
	out->begin = (uint64_t)b;
	out->end = (uint64_t)e;
	out->pos = (uint64_t)p;
	return 0;
}

static int parse_meta(struct dl_state *st, FILE *f)
{
	struct meta_piece *pieces = NULL;
	char line[PATH_LEN];
	char etag[128] = "";
	char mtime[64] = "";
	uint64_t total = 0;
	uint64_t want = 0;
	size_t got = 0;
	int ok = 0;

	while (fgets(line, sizeof(line), f)) {
		int n = (int)strlen(line);

		while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
			line[--n] = '\0';

		if (strncmp(line, "size ", 5) == 0) {
			total = strtoull(line + 5, NULL, 10);
		} else if (strncmp(line, "etag ", 5) == 0) {
			snprintf(etag, sizeof(etag), "%.*s",
				 (int)(sizeof(etag) - 1), line + 5);
		} else if (strncmp(line, "mtime ", 6) == 0) {
			snprintf(mtime, sizeof(mtime), "%.*s",
				 (int)(sizeof(mtime) - 1), line + 6);
		} else if (strncmp(line, "pieces ", 7) == 0) {
			want = strtoull(line + 7, NULL, 10);
			if (want == 0 || want > 100000)
				goto out;
			pieces = calloc((size_t)want, sizeof(*pieces));
			if (!pieces)
				goto out;
		} else if (strncmp(line, "piece ", 6) == 0 && pieces &&
			   got < want) {
			struct meta_piece piece;

			if (parse_piece_line(line + 6, &piece) == 0)
				pieces[got++] = piece;
		}
	}

	if (total != st->total || got == 0 || got != want)
		goto out;
	/* 服务器上的东西换了，旧断点作废 */
	if (etag[0] && st->etag[0] && strcmp(etag, st->etag) != 0)
		goto out;
	if (mtime[0] && st->last_modified[0] &&
	    strcmp(mtime, st->last_modified) != 0)
		goto out;
	if (!meta_pieces_valid(pieces, got, st->total))
		goto out;
	if (install_meta_pieces(st, pieces, got) != 0)
		goto out;

	ok = 1;
out:
	free(pieces);
	return ok;
}

static int load_meta(struct dl_state *st)
{
	FILE *f;
	char line[64];
	int ok;

	f = fopen(st->meta_path, "rb");
	if (!f)
		return 0;
	if (!fgets(line, sizeof(line), f)) {
		fclose(f);
		return 0;
	}
	if (strncmp(line, META_MAGIC, strlen(META_MAGIC)) != 0) {
		fclose(f);
		return 0;
	}
	ok = parse_meta(st, f);
	fclose(f);
	return ok;
}

/* ---------- 落盘回调 ---------- */

static int seg_write(void *ctx, const void *data, size_t len, uint64_t offset)
{
	struct seg_io *io = ctx;
	struct dl_state *st = io->st;
	struct edwl_segment *seg = io->seg;
	int64_t n;

	if (offset < seg->begin || offset + len > seg->end + 1)
		return -1; /* 服务器越界给了多余字节 */

	n = edwl_file_pwrite(st->file, data, len, offset);
	if (n < 0 || (size_t)n != len)
		return -1;

	edwl_mutex_lock(st->mtx);
	if (offset + len > seg->pos)
		seg->pos = offset + len;
	edwl_mutex_unlock(st->mtx);
	return 0;
}

static int seg_progress(void *ctx, uint64_t got)
{
	struct seg_io *io = ctx;
	struct dl_state *st = io->st;

	(void)got;
	if (edwl_interrupt_requested() || st->fatal)
		return 1;
	return 0;
}

static int single_write(void *ctx, const void *data, size_t len,
			uint64_t offset)
{
	struct single_io *io = ctx;
	struct dl_state *st = io->st;
	int64_t n;

	n = edwl_file_pwrite(st->file, data, len, offset);
	if (n < 0 || (size_t)n != len)
		return -1;

	edwl_mutex_lock(st->mtx);
	st->single_done = offset + len;
	edwl_mutex_unlock(st->mtx);
	return 0;
}

static int single_progress(void *ctx, uint64_t got)
{
	struct single_io *io = ctx;

	(void)got;
	if (edwl_interrupt_requested() || io->st->fatal)
		return 1;
	return 0;
}

/* ---------- 单次传输 ---------- */

static void build_http_opts(struct dl_state *st, struct edwl_http_opts *o,
			    unsigned divisor)
{
	memset(o, 0, sizeof(*o));
	o->connect_timeout_ms = st->opts->connect_timeout_ms;
	o->low_speed_time_ms = st->opts->low_speed_time_ms;
	o->low_speed_limit = st->opts->low_speed_limit;
	o->proxy = st->opts->proxy;

	/*
	 * 限速是 per-connection 的（libcurl 没有全局配额），
	 * 所以按并发数均分，近似成全局速率。
	 */
	if (st->opts->max_speed > 0) {
		if (divisor == 0)
			divisor = 1;
		o->max_recv_speed = st->opts->max_speed / divisor;
		if (o->max_recv_speed == 0)
			o->max_recv_speed = 1;
	}
}

static int fetch_attempt(struct dl_state *st, struct edwl_segment *seg)
{
	struct edwl_http *http = NULL;
	struct edwl_http_opts hopts;
	struct edwl_http_sink sink;
	struct seg_io io;
	const char *url;
	size_t src = 0;
	uint64_t begin;
	uint64_t got = 0;
	int ret;

	if (edwl_sources_pick(st->sources, &src) != 1) {
		set_last_error(st, "所有源都在冷却中");
		return EDWL_HTTP_ERR;
	}
	url = edwl_sources_url(st->sources, src);
	if (!url)
		return EDWL_HTTP_ERR;

	build_http_opts(st, &hopts, st->opts->threads);
	if (edwl_http_open(&http, &hopts) != 0) {
		set_last_error(st, "无法创建 HTTP 句柄");
		return EDWL_HTTP_ERR;
	}

	edwl_mutex_lock(st->mtx);
	begin = seg->pos;
	edwl_mutex_unlock(st->mtx);

	io.st = st;
	io.seg = seg;
	sink.write = seg_write;
	sink.progress = seg_progress;
	sink.ctx = &io;

	ret = edwl_http_get_range(http, url, begin, seg->end, &sink, &got);
	if (ret == EDWL_HTTP_OK) {
		edwl_sources_report_ok(st->sources, src);
	} else {
		edwl_sources_report_fail(st->sources, src);
		set_last_error(st, edwl_http_error(http));
		edwl_log_msg(EDWL_LOG_DEBUG, "分片 %llu [%llu-%llu] 失败: %s",
			     (unsigned long long)seg->index,
			     (unsigned long long)seg->begin,
			     (unsigned long long)seg->end,
			     edwl_http_error(http));
	}

	edwl_http_close(http);
	return ret;
}

static int segment_run(struct dl_state *st, struct edwl_segment *seg)
{
	unsigned attempt;

	for (attempt = 0; attempt <= st->opts->retries; attempt++) {
		int ret;

		if (edwl_interrupt_requested() || st->fatal)
			break;

		edwl_mutex_lock(st->mtx);
		if (seg->pos > seg->end) {
			seg->state = EDWL_SEG_DONE;
			edwl_mutex_unlock(st->mtx);
			return 0;
		}
		seg->state = EDWL_SEG_ACTIVE;
		edwl_mutex_unlock(st->mtx);

		ret = fetch_attempt(st, seg);
		if (ret == EDWL_HTTP_OK) {
			edwl_mutex_lock(st->mtx);
			seg->state = EDWL_SEG_DONE;
			edwl_mutex_unlock(st->mtx);
			return 0;
		}

		edwl_mutex_lock(st->mtx);
		seg->attempts++;
		edwl_mutex_unlock(st->mtx);

		if (ret == EDWL_HTTP_ERR_INTERRUPTED)
			break;
		if (ret == EDWL_HTTP_ERR_RANGE_IGNORED) {
			set_last_error(st, "服务器无视 Range，放弃分片模式");
			st->fatal = 1;
			break;
		}
		edwl_sleep_ms(backoff_ms(attempt));
	}

	edwl_mutex_lock(st->mtx);
	seg->state = EDWL_SEG_PENDING;
	edwl_mutex_unlock(st->mtx);
	return -1;
}

static void pool_job(void *ctx, void *job)
{
	struct dl_state *st = ctx;
	size_t index = (size_t)(uintptr_t)job - 1;

	if (index >= st->map.count)
		return;
	segment_run(st, &st->map.segs[index]);
}

/* ---------- 进度 ---------- */

static void snapshot(struct dl_state *st, uint64_t *done, size_t *done_count)
{
	edwl_mutex_lock(st->mtx);
	if (st->using_map) {
		*done = edwl_segment_map_done(&st->map);
		*done_count = edwl_segment_map_done_count(&st->map);
	} else {
		*done = st->single_done;
		*done_count = 0;
	}
	edwl_mutex_unlock(st->mtx);
}

static void update_speed(struct monitor *mo, uint64_t done, uint64_t now)
{
	uint64_t dt = now - mo->last_ms;
	uint64_t delta = done > mo->last_done ? done - mo->last_done : 0;

	if (dt < TICK_MS)
		return;
	mo->speed = delta * 1000ULL / dt;
	mo->last_ms = now;
	mo->last_done = done;
}

static void draw_progress(struct monitor *mo, uint64_t done, size_t done_count,
			  uint64_t now)
{
	struct edwl_progress p;
	struct dl_state *st = mo->st;

	if (!st->opts->show_progress)
		return;
	if (!edwl_console_tty() && now - mo->last_draw_ms < PLAIN_TICK_MS)
		return;

	memset(&p, 0, sizeof(p));
	p.done = done;
	p.total = st->total;
	p.speed_bps = mo->speed;
	p.segs_total = st->using_map ? (unsigned)st->map.count : 0;
	p.segs_done = (unsigned)done_count;
	p.elapsed_ms = now - mo->start_ms;
	edwl_console_progress_draw(&p);
	mo->last_draw_ms = now;
}

static void *monitor_main(void *arg)
{
	struct monitor *mo = arg;
	struct dl_state *st = mo->st;

	for (;;) {
		uint64_t done = 0;
		uint64_t now;
		size_t done_count = 0;
		int stop;

		snapshot(st, &done, &done_count);
		now = edwl_now_ms();
		update_speed(mo, done, now);
		draw_progress(mo, done, done_count, now);

		if (edwl_interrupt_requested())
			st->interrupted = 1;

		if (st->using_map && st->total > 0 &&
		    now - st->last_meta_ms >= META_INTERVAL_MS) {
			st->last_meta_ms = now;
			save_meta(st);
		}

		edwl_mutex_lock(st->mtx);
		stop = mo->stop;
		edwl_mutex_unlock(st->mtx);
		if (stop)
			break;
		edwl_sleep_ms(TICK_MS);
	}
	return NULL;
}

static struct edwl_thread *monitor_start(struct monitor *mo,
					 struct dl_state *st)
{
	struct edwl_thread *t = NULL;
	size_t ignored = 0;

	memset(mo, 0, sizeof(*mo));
	mo->st = st;
	mo->start_ms = edwl_now_ms();
	mo->last_ms = mo->start_ms;
	mo->last_draw_ms = 0;
	snapshot(st, &mo->last_done, &ignored);

	if (edwl_thread_create(&t, monitor_main, mo) != 0)
		return NULL;
	return t;
}

static void monitor_stop(struct monitor *mo, struct edwl_thread *t)
{
	if (!t)
		return;

	edwl_mutex_lock(mo->st->mtx);
	mo->stop = 1;
	edwl_mutex_unlock(mo->st->mtx);

	edwl_thread_join(t);
	edwl_console_progress_end();
}

/* ---------- 探测 ---------- */

static int probe_any(struct dl_state *st, struct edwl_http_info *info)
{
	struct edwl_http *http = NULL;
	struct edwl_http_opts hopts;
	unsigned max_attempts;
	unsigned attempt;

	build_http_opts(st, &hopts, 1);
	if (edwl_http_open(&http, &hopts) != 0) {
		set_last_error(st, "无法创建 HTTP 句柄");
		return -1;
	}

	/* 至少让每个源都试一次，再叠加重试预算 */
	max_attempts =
	    st->opts->retries + 1 + (unsigned)edwl_sources_count(st->sources);

	for (attempt = 0; attempt < max_attempts; attempt++) {
		size_t src = 0;
		const char *url;
		int ret;

		if (edwl_interrupt_requested()) {
			set_last_error(st, "已中断");
			break;
		}
		if (edwl_sources_pick(st->sources, &src) != 1) {
			if (!st->last_error[0])
				set_last_error(st, "所有源都在冷却中");
			break;
		}
		url = edwl_sources_url(st->sources, src);
		if (!url)
			break;

		ret = edwl_http_probe(http, url, info);
		if (ret == EDWL_HTTP_OK) {
			edwl_sources_report_ok(st->sources, src);
			edwl_http_close(http);
			return 0;
		}

		edwl_sources_report_fail(st->sources, src);
		set_last_error(st, edwl_http_error(http));
		edwl_log_msg(EDWL_LOG_WARN, "源 %zu 探测失败: %s", src,
			     edwl_http_error(http));
		/* 401/403/404 这类错误重试同一个源没意义，立刻换下一个源 */
		if (info->status == 401 || info->status == 403 ||
		    info->status == 404)
			continue;
		edwl_sleep_ms(backoff_ms(attempt));
	}

	edwl_http_close(http);
	return -1;
}

/* ---------- 并行模式 ---------- */

static int parallel_download(struct dl_state *st)
{
	struct edwl_thread_pool *pool = NULL;
	struct edwl_thread *mon = NULL;
	struct monitor mo;
	uint64_t piece;
	size_t threads;
	size_t i;
	int rc = EDWL_EXIT_OK;

	piece = edwl_segment_piece_size(st->total, st->opts->threads,
					st->opts->min_piece_size,
					st->opts->max_piece_size);
	if (edwl_segment_map_init(&st->map, st->total, piece,
				  st->opts->max_pieces) != 0) {
		set_last_error(st, "无法生成分片表");
		return EDWL_EXIT_ERROR;
	}
	st->using_map = 1;

	if (st->opts->allow_resume && load_meta(st)) {
		st->resumed = 1;
		edwl_log_msg(EDWL_LOG_INFO, "从断点恢复：%zu 片",
			     st->map.count);
	}

	if (open_output(st) != 0)
		goto fail;

	save_meta(st);

	threads = st->map.count < st->opts->threads ? st->map.count
						    : st->opts->threads;
	if (edwl_pool_create(&pool, threads, pool_job, st) != 0) {
		set_last_error(st, "无法创建线程池");
		goto fail;
	}

	mon = monitor_start(&mo, st);

	for (i = 0; i < st->map.count; i++) {
		if (edwl_segment_complete(&st->map.segs[i]))
			continue;
		if (edwl_pool_submit(pool, (void *)(uintptr_t)(i + 1)) != 0) {
			set_last_error(st, "任务提交失败");
			break;
		}
	}

	edwl_pool_wait_idle(pool);
	monitor_stop(&mo, mon);
	edwl_pool_destroy(pool);
	pool = NULL;

	edwl_mutex_lock(st->mtx);
	edwl_segment_map_release_active(&st->map);
	edwl_mutex_unlock(st->mtx);

	if (edwl_segment_map_all_done(&st->map)) {
		remove(st->meta_path);
		st->written = st->total;
	} else if (edwl_interrupt_requested() || st->interrupted) {
		save_meta(st);
		rc = EDWL_EXIT_INTERRUPTED;
	} else {
		save_meta(st);
		rc = EDWL_EXIT_ERROR;
	}

	edwl_file_close(st->file);
	st->file = (edwl_file_t)-1;
	return rc;

fail:
	if (pool)
		edwl_pool_destroy(pool);
	if (st->file >= 0) {
		edwl_file_close(st->file);
		st->file = (edwl_file_t)-1;
	}
	rc = EDWL_EXIT_ERROR;
	return rc;
}

/* ---------- 单流模式 ---------- */

static int single_download(struct dl_state *st)
{
	struct edwl_http *http = NULL;
	struct edwl_http_opts hopts;
	struct edwl_http_sink sink;
	struct edwl_thread *mon = NULL;
	struct monitor mo;
	struct single_io io;
	unsigned attempt;
	int done = 0;
	int rc = EDWL_EXIT_OK;

	if (open_output(st) != 0)
		return EDWL_EXIT_ERROR;

	build_http_opts(st, &hopts, 1);
	if (edwl_http_open(&http, &hopts) != 0) {
		set_last_error(st, "无法创建 HTTP 句柄");
		goto fail;
	}

	io.st = st;
	sink.write = single_write;
	sink.progress = single_progress;
	sink.ctx = &io;

	mon = monitor_start(&mo, st);

	for (attempt = 0; attempt <= st->opts->retries && !done; attempt++) {
		size_t src = 0;
		const char *url;
		uint64_t got = 0;
		int ret;

		if (edwl_interrupt_requested() || st->fatal)
			break;
		if (edwl_sources_pick(st->sources, &src) != 1) {
			if (!st->last_error[0])
				set_last_error(st, "所有源都在冷却中");
			break;
		}
		url = edwl_sources_url(st->sources, src);
		if (!url)
			break;

		/* 重来就把之前那半截清掉，避免尾部残留 */
		edwl_file_truncate(st->file, st->total);
		edwl_mutex_lock(st->mtx);
		st->single_done = 0;
		edwl_mutex_unlock(st->mtx);

		ret = edwl_http_get_all(http, url, &sink, &got);
		if (ret == EDWL_HTTP_OK) {
			edwl_sources_report_ok(st->sources, src);
			done = 1;
			break;
		}

		edwl_sources_report_fail(st->sources, src);
		set_last_error(st, edwl_http_error(http));
		if (ret == EDWL_HTTP_ERR_INTERRUPTED)
			break;
		edwl_log_msg(EDWL_LOG_WARN, "单流下载失败: %s",
			     edwl_http_error(http));
		edwl_sleep_ms(backoff_ms(attempt));
	}

	monitor_stop(&mo, mon);
	edwl_http_close(http);

	if (!done) {
		if (edwl_interrupt_requested())
			rc = EDWL_EXIT_INTERRUPTED;
		else
			rc = EDWL_EXIT_ERROR;
	} else if (edwl_file_size(st->file, &st->written) != 0) {
		st->written = st->single_done;
	}

	edwl_file_close(st->file);
	st->file = (edwl_file_t)-1;
	return rc;

fail:
	if (mon)
		monitor_stop(&mo, mon);
	if (http)
		edwl_http_close(http);
	if (st->file >= 0) {
		edwl_file_close(st->file);
		st->file = (edwl_file_t)-1;
	}
	return EDWL_EXIT_ERROR;
}

/* ---------- 摘要 ---------- */

static void print_summary(struct dl_state *st, struct edwl_download_result *r)
{
	char size_text[32];
	char speed_text[48];
	char line[PATH_LEN + 192];
	uint64_t elapsed;

	if (!r || st->opts->quiet)
		return;

	elapsed = edwl_now_ms() - st->start_ms;
	edwl_console_format_size(r->bytes, size_text, sizeof(size_text));

	edwl_console_progress_end();
	snprintf(line, sizeof(line), "已保存: %s\n", st->output);
	edwl_console_write(line);

	if (elapsed > 0 && r->bytes > 0) {
		uint64_t bps = r->bytes * 1000ULL / elapsed;
		char rate[32];

		edwl_console_format_size(bps, rate, sizeof(rate));
		snprintf(speed_text, sizeof(speed_text), "%s/s", rate);
	} else {
		snprintf(speed_text, sizeof(speed_text), "--");
	}

	snprintf(line, sizeof(line),
		 "大小: %s   用时: %llu.%01llu 秒   平均: %s\n", size_text,
		 (unsigned long long)(elapsed / 1000),
		 (unsigned long long)((elapsed % 1000) / 100), speed_text);
	edwl_console_write(line);

	if (r->range_used) {
		snprintf(line, sizeof(line), "分片: %u 片, %u 路并发%s\n",
			 r->pieces, st->opts->threads,
			 r->resumed ? "（断点续传）" : "");
		edwl_console_write(line);
	} else {
		snprintf(line, sizeof(line),
			 "单连接下载（服务器不支持 Range 或已禁用分片）\n");
		edwl_console_write(line);
	}

	if (r->verified == 1) {
		snprintf(line, sizeof(line), "SHA-256: %s（校验通过）\n",
			 r->sha256);
		edwl_console_write(line);
	} else if (r->verified == -1) {
		snprintf(line, sizeof(line), "SHA-256: %s（不匹配，期望 %s）\n",
			 r->sha256, st->opts->expected_sha256);
		edwl_console_write(line);
	}
}

/* ---------- 入口 ---------- */

int edwl_download_run(const struct edwl_download_opts *opts,
		      struct edwl_download_result *result)
{
	struct edwl_download_opts local;
	struct edwl_http_info info;
	struct dl_state st;
	int can_range = 0;
	int rc = EDWL_EXIT_OK;

	if (result)
		memset(result, 0, sizeof(*result));
	if (!opts || !opts->urls || opts->url_count == 0)
		return EDWL_EXIT_USAGE;

	local = *opts;
	fill_defaults(&local);

	memset(&st, 0, sizeof(st));
	st.opts = &local;
	st.file = (edwl_file_t)-1;
	st.start_ms = edwl_now_ms();

	if (edwl_http_global_init() != 0) {
		edwl_log_msg(EDWL_LOG_ERROR, "libcurl 初始化失败");
		return EDWL_EXIT_ERROR;
	}
	if (edwl_mutex_init(&st.mtx) != 0) {
		edwl_log_msg(EDWL_LOG_ERROR, "互斥量初始化失败");
		edwl_http_global_fini();
		return EDWL_EXIT_ERROR;
	}
	if (edwl_sources_create(&st.sources, local.urls, local.url_count) !=
	    0) {
		edwl_log_msg(EDWL_LOG_ERROR, "源列表初始化失败");
		edwl_mutex_destroy(st.mtx);
		edwl_http_global_fini();
		return EDWL_EXIT_ERROR;
	}
	if (edwl_console_install_interrupt() != 0)
		edwl_log_msg(EDWL_LOG_DEBUG, "未能安装 Ctrl-C 处理器");

	if (resolve_output(&st) != 0) {
		edwl_log_msg(EDWL_LOG_ERROR, "无法确定输出文件名");
		rc = EDWL_EXIT_ERROR;
		goto out;
	}

	if (probe_any(&st, &info) != 0) {
		edwl_log_msg(EDWL_LOG_ERROR, "无法连接任何源: %s",
			     st.last_error);
		rc = edwl_interrupt_requested() ? EDWL_EXIT_INTERRUPTED
						: EDWL_EXIT_ERROR;
		goto out;
	}

	st.total = info.content_length;
	snprintf(st.etag, sizeof(st.etag), "%s", info.etag);
	snprintf(st.last_modified, sizeof(st.last_modified), "%s",
		 info.last_modified);

	if (!local.quiet) {
		char size_text[32];
		char line[256];

		if (st.total > 0)
			edwl_console_format_size(st.total, size_text,
						 sizeof(size_text));
		else
			snprintf(size_text, sizeof(size_text), "未知");
		snprintf(line, sizeof(line),
			 "目标: %s\n大小: %s\n源: %zu 个，服务器%s Range\n",
			 st.output, size_text, local.url_count,
			 info.range_supported ? "支持" : "不支持");
		edwl_console_write(line);
	}

	can_range = local.allow_range && info.range_supported && st.total > 0 &&
		    st.total >= local.min_piece_size && local.threads > 1;
	if (can_range)
		rc = parallel_download(&st);
	else
		rc = single_download(&st);
	if (result)
		result->range_used = can_range && rc == EDWL_EXIT_OK;

	if (rc == EDWL_EXIT_OK && result) {
		result->total = st.total;
		result->bytes = st.written ? st.written : st.total;
		result->pieces = (unsigned)st.map.count;
		result->resumed = st.resumed;
	}

	if (rc == EDWL_EXIT_OK && local.expected_sha256) {
		char actual[EDWL_SHA256_HEX];

		if (edwl_sha256_file(st.output, actual) != 0) {
			edwl_log_msg(EDWL_LOG_ERROR, "无法读取文件做校验");
			rc = EDWL_EXIT_ERROR;
		} else if (edwl_hash_equals(actual, local.expected_sha256)) {
			if (result) {
				result->verified = 1;
				snprintf(result->sha256, sizeof(result->sha256),
					 "%s", actual);
			}
		} else {
			edwl_log_msg(EDWL_LOG_ERROR, "SHA-256 不匹配: 实际 %s",
				     actual);
			if (result) {
				result->verified = -1;
				snprintf(result->sha256, sizeof(result->sha256),
					 "%s", actual);
			}
			rc = EDWL_EXIT_CHECKSUM;
		}
	}

	if (rc == EDWL_EXIT_INTERRUPTED) {
		edwl_log_msg(EDWL_LOG_INFO, "已中断，断点保存在 %s",
			     st.meta_path);
	} else if (rc != EDWL_EXIT_OK && rc != EDWL_EXIT_CHECKSUM &&
		   st.last_error[0]) {
		edwl_log_msg(EDWL_LOG_ERROR, "%s", st.last_error);
	}

	print_summary(&st, rc == EDWL_EXIT_OK ? result : NULL);

out:
	edwl_segment_map_free(&st.map);
	edwl_sources_destroy(st.sources);
	edwl_mutex_destroy(st.mtx);
	edwl_http_global_fini();
	return rc;
}
