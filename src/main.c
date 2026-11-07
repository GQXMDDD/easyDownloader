/*
 * edwl - 命令行入口
 *
 * 选项习惯照着 wget 来（-O / -t / -T 都是老一套），另外补了 aria2 风格的
 * 并发与限速选项。原则：只认命令行，不读任何全局配置。
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <edwl/core/blacklist.h>
#include <edwl/core/checksum.h>
#include <edwl/core/config.h>
#include <edwl/core/downloader.h>
#include <edwl/core/http_client.h>
#include <edwl/platform/platform.h>
#include <edwl/util/log.h>
#include <edwl/util/url.h>
#include <edwl/edwl.h>

#define MAX_URLS 16

/* 哪些字段来自命令行（命令行优先于配置文件） */
#define SET_THREADS (1u << 0)
#define SET_RETRIES (1u << 1)
#define SET_TIMEOUT (1u << 2)
#define SET_CONNECT_TIMEOUT (1u << 3)
#define SET_MIN_SPLIT (1u << 4)
#define SET_MAX_SPEED (1u << 5)
#define SET_OUTPUT (1u << 6)
#define SET_QUIET (1u << 7)
#define SET_NOPROGRESS (1u << 8)
#define SET_NORESUME (1u << 9)
#define SET_NORANGE (1u << 10)
#define SET_PROXY (1u << 11)
#define SET_SHA256 (1u << 12)
#define SET_DEBUG (1u << 13)

struct cli {
	struct edwl_download_opts opts;
	const char *urls[MAX_URLS];
	size_t url_count;
	const char *proxy;
	int debug;
	const char *bad_text;
	const char *config_path;
	int no_config;
	int generate_config;
	int no_blacklist;
	int wait_exit;
	unsigned set;
};

const char *edwl_version(void)
{
	return EDWL_VERSION_STRING;
}

static void usage(void)
{
	static const char text[] =
	    "用法: edwl [选项] URL [镜像URL ...]\n"
	    "\n"
	    "同一个文件有多个地址时，把它们都写在后面即可，程序会把它们\n"
	    "当作镜像源；分片可能来自不同源，下载完成后可整体校验。\n"
	    "\n"
	    "输出:\n"
	    "  -O, --output FILE      输出文件名；FILE 以 / 结尾时视为目录\n"
	    "  -q, --quiet            只打印错误\n"
	    "      --no-progress      不画进度条\n"
	    "\n"
	    "并发:\n"
	    "  -j, --threads N        并发分片数（默认 8）\n"
	    "      --min-split-size SIZE   单片下限，默认 1M\n"
	    "      --max-speed RATE   全局限速，如 2M（0 表示不限）\n"
	    "\n"
	    "重试与超时:\n"
	    "  -t, --tries N          单片重试次数（默认 3）\n"
	    "  -T, --timeout SEC      低速判定时长（默认 30）\n"
	    "      --connect-timeout SEC   连接超时（默认 15）\n"
	    "\n"
	    "续传与校验:\n"
	    "  -c, --continue         断点续传（默认开启，仅为兼容 wget）\n"
	    "      --no-resume        忽略断点，从头下载\n"
	    "      --sha256 HEX       下载完成后校验整文件 SHA-256\n"
	    "      --no-range         禁用分片，走单连接\n"
	    "\n"
	    "网络:\n"
	    "      --proxy URL        使用指定代理\n"
	    "      --no-proxy         忽略环境变量里的代理，强制直连\n"
	    "\n"
	    "配置文件:\n"
	    "      --config FILE      指定配置文件（默认 ./edwl.ini）\n"
	    "      --no-config        不读配置文件\n"
	    "      --generate-config  生成默认配置模板后退出\n"
	    "      --no-blacklist     本次忽略黑名单\n"
	    "  -W, --wait             结束前等回车（双击运行时方便看结果）\n"
	    "\n"
	    "其他:\n"
	    "      --debug            打印调试日志\n"
	    "  -h, --help             显示本帮助\n"
	    "  -V, --version          显示版本\n";

	edwl_console_write(text);
}

/* ---------- 数值解析 ---------- */

static int parse_size(const char *s, uint64_t *out)
{
	char *end = NULL;
	unsigned long long v;
	const char *suffix;

	if (!s || !*s || !out)
		return -1;

	v = strtoull(s, &end, 10);
	if (end == s)
		return -1;

	suffix = end;
	if (*suffix) {
		int shift = -1;
		char c = (char)tolower((unsigned char)suffix[0]);

		if (c == 'k')
			shift = 10;
		else if (c == 'm')
			shift = 20;
		else if (c == 'g')
			shift = 30;
		else if (c == 't')
			shift = 40;
		else
			return -1;

		suffix++;
		if (*suffix == 'b' || *suffix == 'B') {
			suffix++;
		} else if ((*suffix == 'i' || *suffix == 'I') &&
			   (suffix[1] == 'b' || suffix[1] == 'B')) {
			suffix += 2;
		}
		if (*suffix)
			return -1;
		v <<= shift;
	}

	*out = (uint64_t)v;
	return 0;
}

static int parse_uint(const char *s, unsigned *out)
{
	char *end = NULL;
	unsigned long v;

	if (!s || !*s || !out)
		return -1;
	v = strtoul(s, &end, 10);
	if (end == s || *end != '\0' || v > 100000)
		return -1;
	*out = (unsigned)v;
	return 0;
}

/* ---------- 参数取值 ---------- */

/* 长选项取值：支持 "--opt=v"，也支持 "--opt v" */
static const char *long_value(int argc, char **argv, int *i, const char *arg,
			      size_t name_len)
{
	if (arg[name_len] == '=')
		return arg + name_len + 1;
	if (arg[name_len] != '\0')
		return NULL;
	if (*i + 1 >= argc)
		return NULL;
	(*i)++;
	return argv[*i];
}

/* 短选项取值：支持 "-j8"，也支持 "-j 8" */
static const char *short_value(int argc, char **argv, int *i, const char *arg)
{
	if (arg[2] != '\0')
		return arg + 2;
	if (*i + 1 >= argc)
		return NULL;
	(*i)++;
	return argv[*i];
}

static void add_url(struct cli *c, const char *url)
{
	if (c->url_count >= MAX_URLS)
		return;
	c->urls[c->url_count++] = url;
}

static void apply_numeric(struct cli *c, char which, const char *value)
{
	unsigned n = 0;
	uint64_t bytes = 0;

	switch (which) {
	case 'j':
		if (parse_uint(value, &n) == 0) {
			c->opts.threads = n;
			c->set |= SET_THREADS;
		}
		break;
	case 't':
		if (parse_uint(value, &n) == 0) {
			c->opts.retries = n;
			c->set |= SET_RETRIES;
		}
		break;
	case 'T':
		if (parse_uint(value, &n) == 0) {
			c->opts.low_speed_time_ms = n * 1000;
			c->set |= SET_TIMEOUT;
		}
		break;
	case 'C':
		if (parse_uint(value, &n) == 0) {
			c->opts.connect_timeout_ms = n * 1000;
			c->set |= SET_CONNECT_TIMEOUT;
		}
		break;
	case 's':
		if (parse_size(value, &bytes) == 0) {
			c->opts.min_piece_size = bytes;
			c->set |= SET_MIN_SPLIT;
		}
		break;
	case 'S':
		if (parse_size(value, &bytes) == 0) {
			c->opts.max_speed = bytes;
			c->set |= SET_MAX_SPEED;
		}
		break;
	default:
		break;
	}
}

/* 处理 "-j 8" / "--threads=8" 这一族；匹配上返回 1 */
static int match_numeric(int argc, char **argv, int *i, struct cli *c,
			 const char *a)
{
	struct option_map {
		const char *name;
		size_t len;
		char which;
		int short_opt;
	};
	static const struct option_map table[] = {
	    {"--threads", 9, 'j', 0},	      {"--tries", 7, 't', 0},
	    {"--timeout", 9, 'T', 0},	      {"--connect-timeout", 17, 'C', 0},
	    {"--min-split-size", 16, 's', 0}, {"--max-speed", 11, 'S', 0},
	};
	size_t k;

	for (k = 0; k < sizeof(table) / sizeof(table[0]); k++) {
		if (strncmp(a, table[k].name, table[k].len) != 0)
			continue;
		apply_numeric(c, table[k].which,
			      long_value(argc, argv, i, a, table[k].len));
		return 1;
	}

	if (a[0] == '-' && a[1] != '-' && strchr("jtT", a[1])) {
		apply_numeric(c, a[1], short_value(argc, argv, i, a));
		return 1;
	}
	return 0;
}

/* 返回 0 继续，1 已处理（帮助/版本），-1 出错 */
static int parse_args(int argc, char **argv, struct cli *c)
{
	int i;

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];

		if (strcmp(a, "--") == 0) {
			for (i++; i < argc; i++)
				add_url(c, argv[i]);
			return 0;
		}
		if (a[0] != '-' || a[1] == '\0') {
			add_url(c, a);
			continue;
		}

		if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
			usage();
			return 1;
		}
		if (!strcmp(a, "-V") || !strcmp(a, "--version")) {
			char line[384];

			snprintf(line, sizeof(line),
				 "%s %s\n多线程分片下载器（纯 C 实现）\n内置 "
				 "HTTP 层: %s\n",
				 EDWL_PROGRAM_NAME, edwl_version(),
				 edwl_http_version());
			edwl_console_write(line);
			return 1;
		}
		if (!strcmp(a, "-q") || !strcmp(a, "--quiet")) {
			c->opts.quiet = 1;
			c->opts.show_progress = 0;
			c->set |= SET_QUIET;
			continue;
		}
		if (!strcmp(a, "--no-progress")) {
			c->opts.show_progress = 0;
			c->set |= SET_NOPROGRESS;
			continue;
		}
		if (!strcmp(a, "-c") || !strcmp(a, "--continue")) {
			c->opts.allow_resume = 1;
			continue;
		}
		if (!strcmp(a, "--no-resume")) {
			c->opts.allow_resume = 0;
			c->set |= SET_NORESUME;
			continue;
		}
		if (!strcmp(a, "--no-range")) {
			c->opts.allow_range = 0;
			c->set |= SET_NORANGE;
			continue;
		}
		if (!strcmp(a, "--no-proxy")) {
			c->proxy = "";
			c->set |= SET_PROXY;
			continue;
		}
		if (!strcmp(a, "--debug")) {
			c->debug = 1;
			c->set |= SET_DEBUG;
			continue;
		}
		if (!strcmp(a, "--no-config")) {
			c->no_config = 1;
			continue;
		}
		if (!strcmp(a, "--generate-config")) {
			c->generate_config = 1;
			continue;
		}
		if (!strcmp(a, "--no-blacklist")) {
			c->no_blacklist = 1;
			continue;
		}
		if (!strcmp(a, "-W") || !strcmp(a, "--wait")) {
			c->wait_exit = 1;
			continue;
		}
		if (!strncmp(a, "--config", 8)) {
			const char *v = long_value(argc, argv, &i, a, 8);

			if (!v) {
				c->bad_text = a;
				return -1;
			}
			c->config_path = v;
			continue;
		}
		if (!strcmp(a, "-O") || !strncmp(a, "-O", 2) ||
		    !strncmp(a, "--output", 8)) {
			const char *v = (a[1] == 'O')
					    ? short_value(argc, argv, &i, a)
					    : long_value(argc, argv, &i, a, 8);

			if (!v) {
				c->bad_text = a;
				return -1;
			}
			c->opts.output = v;
			c->set |= SET_OUTPUT;
			continue;
		}
		if (!strncmp(a, "--sha256", 8)) {
			const char *v = long_value(argc, argv, &i, a, 8);

			if (!v || !edwl_hash_is_valid(v)) {
				c->bad_text = a;
				return -1;
			}
			c->opts.expected_sha256 = v;
			c->set |= SET_SHA256;
			continue;
		}
		if (!strncmp(a, "--proxy", 7)) {
			c->proxy = long_value(argc, argv, &i, a, 7);
			if (!c->proxy) {
				c->bad_text = a;
				return -1;
			}
			c->set |= SET_PROXY;
			continue;
		}
		if (match_numeric(argc, argv, &i, c, a))
			continue;

		c->bad_text = a;
		return -1;
	}

	return 0;
}

/* ---------- 入口 ---------- */

static void report_bad_option(const struct cli *c)
{
	char line[512];

	snprintf(line, sizeof(line), "edwl: 无法识别的选项或非法取值: %s\n",
		 c->bad_text ? c->bad_text : "?");
	edwl_console_write_err(line);
	edwl_console_write_err("用 edwl --help 看用法\n");
}

/* 命令行没给的项，从配置文件补 */
static void apply_config(struct cli *c, const struct edwl_config *cfg)
{
	uint64_t bytes;

	if (!(c->set & SET_THREADS))
		c->opts.threads = (unsigned)cfg->threads;
	if (!(c->set & SET_RETRIES))
		c->opts.retries = (unsigned)cfg->retries;
	if (!(c->set & SET_CONNECT_TIMEOUT))
		c->opts.connect_timeout_ms = (unsigned)cfg->timeout * 1000;
	if (!(c->set & SET_MIN_SPLIT) && cfg->chunk_size_mb > 0) {
		bytes = (uint64_t)cfg->chunk_size_mb * 1024ULL * 1024ULL;
		c->opts.min_piece_size = bytes;
		if (c->opts.max_piece_size < bytes)
			c->opts.max_piece_size = bytes;
	}
	if (!(c->set & SET_MAX_SPEED))
		c->opts.max_speed = (uint64_t)cfg->max_speed * 1024ULL;
	if (cfg->min_speed > 0)
		c->opts.low_speed_limit = (uint64_t)cfg->min_speed * 1024ULL;

	c->opts.user_agent = cfg->user_agent;
	c->opts.resume_ext = cfg->resume_ext;
	c->opts.output_dir = cfg->output_dir;
	c->opts.retry_backoff_fixed =
	    (strcmp(cfg->retry_backoff, "fixed") == 0) ? 1 : 0;
	c->opts.dynamic_threads = cfg->dynamic_threads;

	/* [OpenSpeedy]：倍率加速（edwl 自己实现，不驱动外部程序） */
	c->opts.superspeed_enable = cfg->openspeedy_enable;
	if (cfg->openspeedy_enable) {
		double m = 1.0;
		int is_auto = 1;

		if (edwl_config_parse_multiplier(cfg->openspeedy_multiplier, &m,
						 &is_auto) != 0)
			edwl_log_msg(EDWL_LOG_WARN,
				     "SpeedMultiplier=%s 无法识别，改用 auto",
				     cfg->openspeedy_multiplier);
		c->opts.speedup_auto = is_auto;
		c->opts.speed_multiplier = is_auto ? 1.0 : m;
	}
}

static void build_log_opts(const struct edwl_config *cfg, const struct cli *c,
			   struct edwl_log_opts *lo)
{
	int level = edwl_log_level_from_name(cfg->log_level);

	if (level < 0)
		level = EDWL_LOG_INFO;

	memset(lo, 0, sizeof(*lo));
	lo->enabled = cfg->log_enable ? 1 : 0;
	lo->level = level;
	if (c->debug)
		lo->level = EDWL_LOG_DEBUG;
	if (c->opts.quiet) {
		lo->enabled = 1;
		lo->level = EDWL_LOG_ERROR;
	}
	lo->to_console = (strcmp(cfg->log_output, "file") != 0) ? 1 : 0;
	lo->to_file = (strcmp(cfg->log_output, "console") != 0) ? 1 : 0;
	lo->file_path = cfg->log_filepath;
	lo->time_format = cfg->log_time_format;
	lo->retention = cfg->log_retention;
}

/*
 * 按 [Mirrors] 展开出额外源：原始域名命中时，把镜像域名替换进去，
 * 路径与查询串保持原样（镜像值可以是 host 或 host/前缀）。
 */
/* 把一个镜像（host 或 host/前缀）拼成新 URL 并登记 */
static void add_mirror_url(const struct edwl_url *base, const char *spec,
			   size_t spec_len, const char **urls, char *owned[],
			   size_t *n, size_t max)
{
	char spec_buf[512];
	char *url;
	size_t need;

	if (spec_len == 0 || spec_len >= sizeof(spec_buf) || *n >= max)
		return;

	memcpy(spec_buf, spec, spec_len);
	spec_buf[spec_len] = '\0';
	need = strlen(base->scheme) + 3 + spec_len + strlen(base->path) + 1;
	url = malloc(need);
	if (!url)
		return;

	snprintf(url, need, "%s://%s%s", base->scheme, spec_buf, base->path);
	owned[*n] = url;
	urls[*n] = url;
	(*n)++;
}

static size_t expand_urls(const struct edwl_config *cfg, const struct cli *c,
			  const char **urls, char *owned[], size_t max)
{
	size_t i;
	size_t n = 0;

	for (i = 0; i < c->url_count && n < max; i++) {
		const char *u = c->urls[i];
		const struct edwl_mirror_map *m;
		struct edwl_url parsed;
		const char *p;

		urls[n++] = u;
		if (!cfg->mirrors)
			continue;
		if (edwl_url_parse(u, &parsed) != 0)
			continue;
		m = edwl_config_find_mirrors(cfg, parsed.host);
		if (!m)
			continue;

		p = m->mirrors;
		while (*p && n < max) {
			const char *comma = strchr(p, ',');
			size_t len = comma ? (size_t)(comma - p) : strlen(p);

			while (len > 0 && (*p == ' ' || *p == '\t')) {
				p++;
				len--;
			}
			while (len > 0 &&
			       (p[len - 1] == ' ' || p[len - 1] == '\t' ||
				p[len - 1] == '\r'))
				len--;
			add_mirror_url(&parsed, p, len, urls, owned, &n, max);
			if (!comma)
				break;
			p = comma + 1;
		}
	}
	return n;
}

int edwl_main(int argc, char **argv)
{
	struct cli c;
	struct edwl_config cfg;
	struct edwl_blacklist *bl = NULL;
	struct edwl_download_result result;
	struct edwl_log_opts lopts;
	const char *urls[MAX_URLS];
	char *owned[MAX_URLS];
	size_t url_count;
	size_t i;
	int parsed;
	int rc;

	memset(&c, 0, sizeof(c));
	memset(owned, 0, sizeof(owned));
	edwl_download_opts_default(&c.opts);

	/*
	 * 先初始化控制台再解析参数：--help / --version 也会输出中文，
	 * 必须在控制台进入可正确显示状态之后再打印。
	 */
	edwl_console_init();

	parsed = parse_args(argc, argv, &c);
	if (parsed == 1) {
		edwl_console_shutdown();
		return EDWL_EXIT_OK;
	}
	if (parsed < 0) {
		report_bad_option(&c);
		edwl_console_shutdown();
		return EDWL_EXIT_USAGE;
	}

	edwl_config_init_defaults(&cfg);
	if (c.generate_config) {
		const char *path = c.config_path ? c.config_path : "edwl.ini";
		char line[512];

		if (edwl_config_generate(path) != 0) {
			snprintf(line, sizeof(line), "edwl: 生成配置失败: %s\n",
				 path);
			edwl_console_write_err(line);
			edwl_console_shutdown();
			return EDWL_EXIT_ERROR;
		}
		snprintf(line, sizeof(line), "已生成配置模板: %s\n", path);
		edwl_console_write(line);
		edwl_console_shutdown();
		return EDWL_EXIT_OK;
	}

	if (!c.no_config) {
		const char *path = c.config_path ? c.config_path : "edwl.ini";
		int load_rc = edwl_config_load(&cfg, path);

		if (load_rc < 0) {
			char line[512];

			snprintf(line, sizeof(line),
				 "edwl: 配置文件解析失败: %s\n", path);
			edwl_console_write_err(line);
			edwl_config_free(&cfg);
			edwl_console_shutdown();
			return EDWL_EXIT_USAGE;
		}
		if (load_rc == 0 && c.config_path) {
			char line[512];

			snprintf(line, sizeof(line),
				 "edwl: 指定的配置文件不存在: "
				 "%s（使用内置默认值）\n",
				 path);
			edwl_console_write_err(line);
		}
	}

	build_log_opts(&cfg, &c, &lopts);
	if (edwl_log_init(&lopts) != 0 && lopts.to_file)
		edwl_log_msg(EDWL_LOG_WARN, "日志文件不可用，已退回终端输出");

	if (c.url_count == 0) {
		usage();
		edwl_log_shutdown();
		edwl_console_shutdown();
		edwl_config_free(&cfg);
		return EDWL_EXIT_USAGE;
	}

	apply_config(&c, &cfg);

	if (cfg.blacklist_enable && !c.no_blacklist &&
	    (cfg.blacklist_hosts[0] || cfg.blacklist_ips[0])) {
		if (edwl_blacklist_create(cfg.blacklist_hosts,
					  cfg.blacklist_ips, &bl) != 0) {
			edwl_log_msg(EDWL_LOG_ERROR, "黑名单初始化失败");
			edwl_log_shutdown();
			edwl_console_shutdown();
			edwl_config_free(&cfg);
			return EDWL_EXIT_ERROR;
		}
	}

	url_count = expand_urls(&cfg, &c, urls, owned, MAX_URLS);

	c.opts.urls = urls;
	c.opts.url_count = url_count;
	c.opts.proxy = c.proxy;
	c.opts.blacklist = bl;

	rc = edwl_download_run(&c.opts, &result);

	if (c.wait_exit) {
		edwl_console_write("\n按回车键退出...\n");
		fflush(stdout);
		(void)getchar();
	}

	for (i = 0; i < MAX_URLS; i++)
		free(owned[i]);
	edwl_blacklist_destroy(bl);
	edwl_config_free(&cfg);
	edwl_console_shutdown();
	edwl_log_shutdown();
	return rc;
}

int main(int argc, char **argv)
{
	return edwl_main(argc, argv);
}
