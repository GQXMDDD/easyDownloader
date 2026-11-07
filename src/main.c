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

#include <edwl/core/checksum.h>
#include <edwl/core/downloader.h>
#include <edwl/core/http_client.h>
#include <edwl/platform/platform.h>
#include <edwl/util/log.h>
#include <edwl/edwl.h>

#define MAX_URLS 16

struct cli {
	struct edwl_download_opts opts;
	const char *urls[MAX_URLS];
	size_t url_count;
	const char *proxy;
	int debug;
	const char *bad_text;
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
		if (parse_uint(value, &n) == 0)
			c->opts.threads = n;
		break;
	case 't':
		if (parse_uint(value, &n) == 0)
			c->opts.retries = n;
		break;
	case 'T':
		if (parse_uint(value, &n) == 0)
			c->opts.low_speed_time_ms = n * 1000;
		break;
	case 'C':
		if (parse_uint(value, &n) == 0)
			c->opts.connect_timeout_ms = n * 1000;
		break;
	case 's':
		if (parse_size(value, &bytes) == 0)
			c->opts.min_piece_size = bytes;
		break;
	case 'S':
		if (parse_size(value, &bytes) == 0)
			c->opts.max_speed = bytes;
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
			continue;
		}
		if (!strcmp(a, "--no-progress")) {
			c->opts.show_progress = 0;
			continue;
		}
		if (!strcmp(a, "-c") || !strcmp(a, "--continue")) {
			c->opts.allow_resume = 1;
			continue;
		}
		if (!strcmp(a, "--no-resume")) {
			c->opts.allow_resume = 0;
			continue;
		}
		if (!strcmp(a, "--no-range")) {
			c->opts.allow_range = 0;
			continue;
		}
		if (!strcmp(a, "--no-proxy")) {
			c->proxy = "";
			continue;
		}
		if (!strcmp(a, "--debug")) {
			c->debug = 1;
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
			continue;
		}
		if (!strncmp(a, "--sha256", 8)) {
			const char *v = long_value(argc, argv, &i, a, 8);

			if (!v || !edwl_hash_is_valid(v)) {
				c->bad_text = a;
				return -1;
			}
			c->opts.expected_sha256 = v;
			continue;
		}
		if (!strncmp(a, "--proxy", 7)) {
			c->proxy = long_value(argc, argv, &i, a, 7);
			if (!c->proxy) {
				c->bad_text = a;
				return -1;
			}
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

int edwl_main(int argc, char **argv)
{
	struct cli c;
	struct edwl_download_result result;
	int parsed;
	int rc;

	memset(&c, 0, sizeof(c));
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

	if (c.opts.quiet)
		edwl_log_init(EDWL_LOG_ERROR);
	else if (c.debug)
		edwl_log_init(EDWL_LOG_DEBUG);
	else
		edwl_log_init(EDWL_LOG_INFO);

	if (c.url_count == 0) {
		usage();
		edwl_console_shutdown();
		return EDWL_EXIT_USAGE;
	}

	c.opts.urls = c.urls;
	c.opts.url_count = c.url_count;
	c.opts.proxy = c.proxy;

	rc = edwl_download_run(&c.opts, &result);

	edwl_console_shutdown();
	edwl_log_shutdown();
	return rc;
}

int main(int argc, char **argv)
{
	return edwl_main(argc, argv);
}
