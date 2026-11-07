/*
 * edwl - 日志实现
 *
 * 一句话：级别过滤 + 可选文件 + 时间戳 + 两种清理策略（按行 / 按天）。
 * 所有输出都在一把互斥量里做，保证多 worker 并发写时行不会交错。
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <edwl/platform/platform.h>
#include <edwl/util/log.h>

#define EDWL_LOG_LINE_MAX 1024
#define EDWL_LOG_TRIM_LIMIT (32L * 1024L * 1024L) /* 超过就不做按行裁剪 */

struct log_state {
	int enabled;
	int threshold;
	int to_console;
	int to_file;
	FILE *file;
	char path[512]; /* 实际写入的路径（按天模式下带日期） */
	char dir[448];
	char stem[192];
	char ext[32];
	char time_fmt[64];
	int ret_kind; /* 0 = 按行，1 = 按天 */
	int ret_value;
	struct edwl_mutex *mtx;
};

static struct log_state g_log = {
    1,	EDWL_LOG_INFO,	       1, 0,	NULL, "", "", "",
    "", "[%Y-%m-%d %H:%M:%S]", 0, 1000, NULL};

/* ---------- 工具 ---------- */

static void split_path(const char *full)
{
	const char *slash = strrchr(full, '/');
	const char *bslash = strrchr(full, '\\');
	const char *sep = slash > bslash ? slash : bslash;
	const char *name;
	const char *dot;

	g_log.dir[0] = '\0';
	if (sep) {
		size_t n = (size_t)(sep - full);

		if (n >= sizeof(g_log.dir))
			n = sizeof(g_log.dir) - 1;
		memcpy(g_log.dir, full, n);
		g_log.dir[n] = '\0';
		name = sep + 1;
	} else {
		strcpy(g_log.dir, ".");
		name = full;
	}

	dot = strrchr(name, '.');
	if (dot && dot != name) {
		size_t n = (size_t)(dot - name);

		if (n >= sizeof(g_log.stem))
			n = sizeof(g_log.stem) - 1;
		memcpy(g_log.stem, name, n);
		g_log.stem[n] = '\0';
		strncpy(g_log.ext, dot, sizeof(g_log.ext) - 1);
		g_log.ext[sizeof(g_log.ext) - 1] = '\0';
	} else {
		strncpy(g_log.stem, name, sizeof(g_log.stem) - 1);
		g_log.stem[sizeof(g_log.stem) - 1] = '\0';
		g_log.ext[0] = '\0';
	}
}

static void join_path(char *out, size_t size, const char *dir, const char *name)
{
	size_t dl = strlen(dir);
	size_t nl = strlen(name);

	if (strcmp(dir, ".") == 0) {
		if (nl >= size)
			nl = size - 1;
		memcpy(out, name, nl);
		out[nl] = '\0';
		return;
	}
	if (dl + 1 + nl + 1 > size)
		nl = size > dl + 2 ? size - dl - 2 : 0;
	memcpy(out, dir, dl);
	out[dl] = '/';
	memcpy(out + dl + 1, name, nl);
	out[dl + 1 + nl] = '\0';
}

/* "1000L" → kind 0 / 1000；"30T" → kind 1 / 30 */
static int parse_retention(const char *text, int *kind, int *value)
{
	char *end = NULL;
	long v;

	if (!text || !*text)
		return -1;
	v = strtol(text, &end, 10);
	if (end == text || v <= 0)
		return -1;
	if (*end == 'L' || *end == 'l')
		*kind = 0;
	else if (*end == 'T' || *end == 't')
		*kind = 1;
	else
		return -1;
	if (end[1] != '\0')
		return -1;
	*value = (int)v;
	return 0;
}

/* ---------- 按行清理 ---------- */

static void trim_file_lines(const char *path, int max_lines)
{
	FILE *f;
	long size;
	char *buf;
	size_t got;
	size_t i;
	int total = 0;
	int skip;
	size_t start = 0;
	char tmp[560];

	f = fopen(path, "rb");
	if (!f)
		return;
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return;
	}
	size = ftell(f);
	if (size <= 0 || size > EDWL_LOG_TRIM_LIMIT) {
		fclose(f);
		return;
	}
	rewind(f);
	buf = malloc((size_t)size + 1);
	if (!buf) {
		fclose(f);
		return;
	}
	got = fread(buf, 1, (size_t)size, f);
	fclose(f);
	buf[got] = '\0';
	if (got != (size_t)size) {
		free(buf);
		return;
	}

	for (i = 0; i < got; i++) {
		if (buf[i] == '\n' && i + 1 < got)
			total++;
	}
	if (got > 0 && buf[got - 1] != '\n')
		total++;

	if (total <= max_lines) {
		free(buf);
		return;
	}

	/* 找到需要保留的第一行起始位置 */
	skip = total - max_lines;
	for (i = 0; i < got; i++) {
		if (buf[i] == '\n' && i + 1 < got) {
			if (skip == 0) {
				start = i + 1;
				break;
			}
			skip--;
		}
	}

	{
		FILE *out;

		snprintf(tmp, sizeof(tmp), "%s.tmp", path);
		out = fopen(tmp, "wb");
		if (!out) {
			free(buf);
			return;
		}
		fwrite(buf + start, 1, got - start, out);
		fclose(out);
	}

	remove(path);
	rename(tmp, path);
	free(buf);
}

/* ---------- 按天清理 ---------- */

struct purge_ctx {
	int days;
};

static int is_digits(const char *s, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		if (s[i] < '0' || s[i] > '9')
			return 0;
	}
	return 1;
}

static int parse_date(const char *s, int *y, int *m, int *d)
{
	if (!is_digits(s, 4) || s[4] != '-' || !is_digits(s + 5, 2) ||
	    s[7] != '-' || !is_digits(s + 8, 2))
		return -1;
	*y = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 +
	     (s[3] - '0');
	*m = (s[5] - '0') * 10 + (s[6] - '0');
	*d = (s[8] - '0') * 10 + (s[9] - '0');
	if (*m < 1 || *m > 12 || *d < 1 || *d > 31)
		return -1;
	return 0;
}

static long days_since_epoch(int y, int m, int d)
{
	struct tm tmv;

	memset(&tmv, 0, sizeof(tmv));
	tmv.tm_year = y - 1900;
	tmv.tm_mon = m - 1;
	tmv.tm_mday = d;
	tmv.tm_hour = 12;
	return (long)(mktime(&tmv) / 86400L);
}

static int purge_cb(const char *name, void *user)
{
	struct purge_ctx *ctx = user;
	size_t stem_len = strlen(g_log.stem);
	size_t ext_len = strlen(g_log.ext);
	size_t name_len = strlen(name);
	const char *date;
	char full[560];
	int y, m, d;
	long age;

	/* 只处理 <stem>-YYYY-MM-DD<ext> 这种我们自己生成的文件 */
	if (name_len != stem_len + 1 + 10 + ext_len)
		return 0;
	if (memcmp(name, g_log.stem, stem_len) != 0)
		return 0;
	if (name[stem_len] != '-')
		return 0;
	date = name + stem_len + 1;
	if (ext_len > 0 &&
	    memcmp(name + stem_len + 11, g_log.ext, ext_len) != 0)
		return 0;
	if (parse_date(date, &y, &m, &d) != 0)
		return 0;

	{
		time_t now = time(NULL);
		struct tm *today_p = localtime(&now);
		struct tm today;

		if (!today_p)
			return 0;
		today = *today_p;
		age = days_since_epoch(today.tm_year + 1900, today.tm_mon + 1,
				       today.tm_mday) -
		      days_since_epoch(y, m, d);
	}
	if (age <= ctx->days)
		return 0;

	join_path(full, sizeof(full), g_log.dir, name);
	remove(full);
	return 0;
}

static void make_dated_path(char *out, size_t size)
{
	char stamp[32];
	time_t now = time(NULL);
	struct tm tmv;
	char name[320];

	{
		struct tm *p = localtime(&now);

		if (!p)
			return;
		tmv = *p;
	}
	strftime(stamp, sizeof(stamp), "%Y-%m-%d", &tmv);
	snprintf(name, sizeof(name), "%s-%s%s", g_log.stem, stamp, g_log.ext);
	join_path(out, size, g_log.dir, name);
}

/* ---------- 初始化 ---------- */

static int ensure_mutex(void)
{
	if (g_log.mtx)
		return 0;
	return edwl_mutex_init(&g_log.mtx);
}

int edwl_log_init(const struct edwl_log_opts *opts)
{
	struct purge_ctx ctx;

	if (ensure_mutex() != 0)
		return -1;

	g_log.file = NULL;
	g_log.enabled = opts ? opts->enabled : 1;
	g_log.to_console = opts ? opts->to_console : 1;
	g_log.to_file = opts ? opts->to_file : 0;
	g_log.threshold = opts ? opts->level : EDWL_LOG_INFO;
	if (!g_log.enabled) {
		g_log.threshold = EDWL_LOG_OFF;
		g_log.to_console = 0;
		g_log.to_file = 0;
		return 0;
	}
	if (g_log.threshold < EDWL_LOG_OFF)
		g_log.threshold = EDWL_LOG_INFO;
	if (g_log.threshold > EDWL_LOG_DEBUG)
		g_log.threshold = EDWL_LOG_DEBUG;

	if (opts && opts->time_format && opts->time_format[0])
		snprintf(g_log.time_fmt, sizeof(g_log.time_fmt), "%s",
			 opts->time_format);
	else
		snprintf(g_log.time_fmt, sizeof(g_log.time_fmt),
			 "[%%Y-%%m-%%d %%H:%%M:%%S]");

	if (!g_log.to_file || !opts || !opts->file_path ||
	    !opts->file_path[0]) {
		g_log.to_file = 0;
		return 0;
	}

	split_path(opts->file_path);
	g_log.ret_kind = 0;
	g_log.ret_value = 1000;
	if (parse_retention(opts->retention, &g_log.ret_kind,
			    &g_log.ret_value) != 0) {
		g_log.ret_kind = 0;
		g_log.ret_value = 1000;
	}

	if (g_log.ret_kind == 0) {
		snprintf(g_log.path, sizeof(g_log.path), "%s", opts->file_path);
		trim_file_lines(g_log.path, g_log.ret_value);
	} else {
		make_dated_path(g_log.path, sizeof(g_log.path));
		ctx.days = g_log.ret_value;
		edwl_dir_list(g_log.dir, purge_cb, &ctx);
	}

	g_log.file = fopen(g_log.path, "ab");
	if (!g_log.file) {
		g_log.to_file = 0;
		edwl_log_msg(EDWL_LOG_WARN,
			     "日志文件打不开：%s（改为只输出到终端）",
			     g_log.path);
		return -1;
	}
	return 0;
}

void edwl_log_shutdown(void)
{
	if (g_log.file) {
		fclose(g_log.file);
		g_log.file = NULL;
	}
	if (g_log.mtx) {
		edwl_mutex_destroy(g_log.mtx);
		g_log.mtx = NULL;
	}
	memset(&g_log, 0, sizeof(g_log));
}

/* ---------- 输出 ---------- */

int edwl_log_level_from_name(const char *name)
{
	if (!name)
		return -1;
	if (strcmp(name, "off") == 0)
		return EDWL_LOG_OFF;
	if (strcmp(name, "low") == 0)
		return EDWL_LOG_RESULT;
	if (strcmp(name, "high") == 0)
		return EDWL_LOG_PROGRESS;
	if (strcmp(name, "max") == 0)
		return EDWL_LOG_DEBUG;
	if (strcmp(name, "error") == 0)
		return EDWL_LOG_ERROR;
	if (strcmp(name, "warn") == 0)
		return EDWL_LOG_WARN;
	if (strcmp(name, "info") == 0)
		return EDWL_LOG_INFO;
	if (strcmp(name, "debug") == 0)
		return EDWL_LOG_DEBUG;
	return -1;
}

const char *edwl_log_level_name(int level)
{
	switch (level) {
	case EDWL_LOG_ERROR:
		return "error";
	case EDWL_LOG_WARN:
		return "warn";
	case EDWL_LOG_RESULT:
		return "result";
	case EDWL_LOG_INFO:
		return "info";
	case EDWL_LOG_PROGRESS:
		return "progress";
	case EDWL_LOG_DEBUG:
		return "debug";
	default:
		return "log";
	}
}

int edwl_log_threshold(void)
{
	return g_log.threshold;
}

int edwl_log_allows(int level)
{
	if (level <= EDWL_LOG_OFF || level > g_log.threshold)
		return 0;
	return 1;
}

static void format_stamp(char *out, size_t size)
{
	time_t now = time(NULL);
	struct tm tmv;

	if (!g_log.time_fmt[0])
		return;
	{
		struct tm *p = localtime(&now);

		if (!p)
			return;
		tmv = *p;
	}
	if (strftime(out, size, g_log.time_fmt, &tmv) == 0)
		out[0] = '\0';
}

void edwl_log_msg(int level, const char *fmt, ...)
{
	char body[EDWL_LOG_LINE_MAX];
	char line[EDWL_LOG_LINE_MAX + 128];
	char stamp[64] = "";
	va_list ap;
	int n;

	if (!fmt || level <= EDWL_LOG_OFF || level > g_log.threshold)
		return;

	va_start(ap, fmt);
	vsnprintf(body, sizeof(body), fmt, ap);
	va_end(ap);

	if (g_log.enabled && g_log.to_file)
		format_stamp(stamp, sizeof(stamp));

	n = snprintf(line, sizeof(line), "%s[%s] %s\n", stamp,
		     edwl_log_level_name(level), body);
	if (n < 0)
		return;
	if ((size_t)n >= sizeof(line))
		line[sizeof(line) - 2] = '\n';

	if (g_log.mtx)
		edwl_mutex_lock(g_log.mtx);

	if (g_log.enabled && g_log.to_console) {
		if (level == EDWL_LOG_RESULT)
			edwl_console_write(line);
		else
			edwl_console_write_err(line);
	}
	if (g_log.enabled && g_log.to_file && g_log.file) {
		fputs(line, g_log.file);
		fflush(g_log.file);
	}

	if (g_log.mtx)
		edwl_mutex_unlock(g_log.mtx);
}
