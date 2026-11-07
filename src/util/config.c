/*
 * edwl - 配置文件解析（inih）
 *
 * 依赖规则里的第二个例外：第三方头 <ini.h> 只允许出现在本文件里，
 * 和 http_client.c 里只允许出现 <curl/curl.h> 是一个道理。
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ini.h>

#include <edwl/core/config.h>
#include <edwl/util/log.h>

#define EDWL_CFG_MAX_MIRRORS 64

/* ---------- 小工具 ---------- */

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

static char *trim(char *s)
{
	char *end;

	while (*s == ' ' || *s == '\t')
		s++;
	end = s + strlen(s);
	while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
			   end[-1] == '\r' || end[-1] == '\n'))
		end--;
	*end = '\0';
	return s;
}

static void copy_str(char *dst, size_t size, const char *src)
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

static int parse_int_range(const char *value, int *out, int lo, int hi)
{
	char *end = NULL;
	long v;

	if (!value || !*value)
		return -1;
	v = strtol(value, &end, 10);
	if (end == value)
		return -1;
	if (*end != '\0')
		return -1;
	if (v < lo || v > hi)
		return -1;
	*out = (int)v;
	return 0;
}

static int parse_bool(const char *value)
{
	if (ci_equal(value, "on") || ci_equal(value, "yes") ||
	    ci_equal(value, "true") || ci_equal(value, "1"))
		return 1;
	return 0;
}

/* ---------- 解析回调 ---------- */

static int add_mirror(struct edwl_config *cfg, const char *name,
		      const char *value)
{
	struct edwl_mirror_map *node;
	size_t n = 0;
	struct edwl_mirror_map *it;

	for (it = cfg->mirrors; it; it = it->next)
		n++;
	if (n >= EDWL_CFG_MAX_MIRRORS)
		return 1; /* 超上限就忽略，不打断解析 */

	node = calloc(1, sizeof(*node));
	if (!node)
		return 0;
	copy_str(node->original_host, sizeof(node->original_host), name);
	copy_str(node->mirrors, sizeof(node->mirrors), value);
	node->next = cfg->mirrors;
	cfg->mirrors = node;
	return 1;
}

static int handle_general(struct edwl_config *cfg, const char *name,
			  const char *value)
{
	if (ci_equal(name, "Threads")) {
		parse_int_range(value, &cfg->threads, 1, 64);
	} else if (ci_equal(name, "DynamicThreads")) {
		cfg->dynamic_threads = parse_bool(value);
	} else if (ci_equal(name, "MaxSpeed")) {
		parse_int_range(value, &cfg->max_speed, 0, 1000000);
	} else if (ci_equal(name, "MinSpeed")) {
		parse_int_range(value, &cfg->min_speed, 0, 1000000);
	} else if (ci_equal(name, "Timeout")) {
		parse_int_range(value, &cfg->timeout, 1, 3600);
	} else if (ci_equal(name, "Retries")) {
		parse_int_range(value, &cfg->retries, 0, 100);
	} else if (ci_equal(name, "RetryBackoff")) {
		if (ci_equal(value, "fixed") || ci_equal(value, "exponential"))
			copy_str(cfg->retry_backoff, sizeof(cfg->retry_backoff),
				 value);
	} else if (ci_equal(name, "OutputDir")) {
		copy_str(cfg->output_dir, sizeof(cfg->output_dir), value);
	} else if (ci_equal(name, "ChunkSizeMB")) {
		parse_int_range(value, &cfg->chunk_size_mb, 1, 1024);
	} else if (ci_equal(name, "UserAgent")) {
		copy_str(cfg->user_agent, sizeof(cfg->user_agent), value);
	} else if (ci_equal(name, "ResumeFileExt")) {
		copy_str(cfg->resume_ext, sizeof(cfg->resume_ext), value);
	}
	return 1;
}

static int handle_logging(struct edwl_config *cfg, const char *name,
			  const char *value)
{
	if (ci_equal(name, "Enable")) {
		cfg->log_enable = parse_bool(value);
	} else if (ci_equal(name, "Level")) {
		if (ci_equal(value, "off") || ci_equal(value, "low") ||
		    ci_equal(value, "high") || ci_equal(value, "max"))
			copy_str(cfg->log_level, sizeof(cfg->log_level), value);
	} else if (ci_equal(name, "Output")) {
		if (ci_equal(value, "console") || ci_equal(value, "file") ||
		    ci_equal(value, "both"))
			copy_str(cfg->log_output, sizeof(cfg->log_output),
				 value);
	} else if (ci_equal(name, "FilePath")) {
		copy_str(cfg->log_filepath, sizeof(cfg->log_filepath), value);
	} else if (ci_equal(name, "Retention")) {
		copy_str(cfg->log_retention, sizeof(cfg->log_retention), value);
	} else if (ci_equal(name, "TimeFormat")) {
		copy_str(cfg->log_time_format, sizeof(cfg->log_time_format),
			 value);
	}
	return 1;
}

static int handle_blacklist(struct edwl_config *cfg, const char *name,
			    const char *value)
{
	if (ci_equal(name, "Enable")) {
		cfg->blacklist_enable = parse_bool(value);
	} else if (ci_equal(name, "Hosts")) {
		copy_str(cfg->blacklist_hosts, sizeof(cfg->blacklist_hosts),
			 value);
	} else if (ci_equal(name, "IPs")) {
		copy_str(cfg->blacklist_ips, sizeof(cfg->blacklist_ips), value);
	}
	return 1;
}

static int config_ini_handler(void *user, const char *section, const char *name,
			      const char *value)
{
	struct edwl_config *cfg = user;
	char *val = trim((char *)value);

	if (ci_equal(section, "General"))
		return handle_general(cfg, name, val);
	if (ci_equal(section, "OpenSpeedy")) {
		if (ci_equal(name, "Enable"))
			cfg->openspeedy_enable = parse_bool(val);
		else if (ci_equal(name, "SpeedMultiplier"))
			copy_str(cfg->openspeedy_multiplier,
				 sizeof(cfg->openspeedy_multiplier), val);
		return 1;
	}
	if (ci_equal(section, "Logging"))
		return handle_logging(cfg, name, val);
	if (ci_equal(section, "Blacklist"))
		return handle_blacklist(cfg, name, val);
	if (ci_equal(section, "Mirrors"))
		return add_mirror(cfg, name, val);

	/* 未知段/键：忽略，保持前向兼容 */
	return 1;
}

/* ---------- 对外接口 ---------- */

void edwl_config_init_defaults(struct edwl_config *cfg)
{
	if (!cfg)
		return;
	memset(cfg, 0, sizeof(*cfg));

	cfg->threads = 8;
	cfg->dynamic_threads = 1;
	cfg->max_speed = 0;
	cfg->min_speed = 100;
	cfg->timeout = 15;
	cfg->retries = 3;
	copy_str(cfg->retry_backoff, sizeof(cfg->retry_backoff), "exponential");
	copy_str(cfg->output_dir, sizeof(cfg->output_dir), ".");
	cfg->chunk_size_mb = 4;
	copy_str(cfg->user_agent, sizeof(cfg->user_agent),
		 "Mozilla/5.0 (Windows NT 10.0; Win64; x64) edwl/0.2.0");
	copy_str(cfg->resume_ext, sizeof(cfg->resume_ext), ".edwl");

	cfg->openspeedy_enable = 0;
	copy_str(cfg->openspeedy_multiplier, sizeof(cfg->openspeedy_multiplier),
		 "auto");

	cfg->log_enable = 1;
	/* 内置默认用 high：没有配置文件时也保留进度条；模板里写的是 low */
	copy_str(cfg->log_level, sizeof(cfg->log_level), "high");
	copy_str(cfg->log_output, sizeof(cfg->log_output), "console");
	copy_str(cfg->log_filepath, sizeof(cfg->log_filepath), "./edwl.log");
	copy_str(cfg->log_retention, sizeof(cfg->log_retention), "1000L");
	copy_str(cfg->log_time_format, sizeof(cfg->log_time_format),
		 "[%Y-%m-%d %H:%M:%S]");

	cfg->blacklist_enable = 1;
	cfg->blacklist_hosts[0] = '\0';
	cfg->blacklist_ips[0] = '\0';

	cfg->mirrors = NULL;
}

int edwl_config_load(struct edwl_config *cfg, const char *path)
{
	int rc;

	if (!cfg || !path)
		return -1;

	rc = ini_parse(path, config_ini_handler, cfg);
	if (rc == -1)
		return 0; /* 打不开：当作没有配置文件 */
	if (rc > 0) {
		edwl_log_msg(EDWL_LOG_WARN, "配置文件 %s 第 %d 行有语法问题",
			     path, rc);
		return -1;
	}
	return 1;
}

int edwl_config_parse_multiplier(const char *text, double *out, int *is_auto)
{
	char *end = NULL;
	double v;

	if (!text || !out || !is_auto)
		return -1;
	while (*text == ' ' || *text == '\t')
		text++;
	if (ci_equal(text, "auto")) {
		*is_auto = 1;
		*out = 0.0;
		return 0;
	}

	v = strtod(text, &end);
	if (end == text)
		return -1;
	while (*end == ' ' || *end == '\t')
		end++;
	if (*end != '\0')
		return -1;
	if (v < 0.25 || v > 8.0)
		return -1;

	*is_auto = 0;
	*out = v;
	return 0;
}

int edwl_config_generate(const char *path)
{
	static const char tmpl[] =
	    "; ==========================================\n"
	    "; edwl 配置文件 (edwl.ini)\n"
	    ";\n"
	    "; 提示：命令行参数（如 -j 16）的优先级永远高于此文件。\n"
	    "; 可以用 `edwl --generate-config` 随时重新导出这份模板。\n"
	    "; ==========================================\n"
	    "\n"
	    "[General]\n"
	    "\n"
	    "; 静态线程数（开启动态线程时作为上限）\n"
	    "Threads = 8\n"
	    "\n"
	    "; 动态线程：根据网络带宽与服务器响应自动增减并发（on/off）\n"
	    "DynamicThreads = on\n"
	    "\n"
	    "; 速度上限（KB/s，0 表示不限）\n"
	    "MaxSpeed = 0\n"
	    "\n"
	    "; 速度下限（KB/s）：低于它并持续 Timeout 秒就放弃\n"
	    "MinSpeed = 100\n"
	    "\n"
	    "; 连接超时（秒）\n"
	    "Timeout = 15\n"
	    "\n"
	    "; 单个分片的最大重试次数\n"
	    "Retries = 3\n"
	    "\n"
	    "; 失败重试退避：exponential（指数退避）或 fixed（固定间隔）\n"
	    "RetryBackoff = exponential\n"
	    "\n"
	    "; 默认输出目录（留空或 . 表示当前目录）\n"
	    "OutputDir = .\n"
	    "\n"
	    "; 分片大小（MB），建议 2-8\n"
	    "ChunkSizeMB = 4\n"
	    "\n"
	    "; User-Agent\n"
	    "UserAgent = Mozilla/5.0 (Windows NT 10.0; Win64; x64) edwl/0.2.0\n"
	    "\n"
	    "; 断点续传元数据文件后缀\n"
	    "ResumeFileExt = .edwl\n"
	    "\n"
	    "[OpenSpeedy]\n"
	    "\n"
	    "; 内置倍率加速（思路参考 OpenSpeedy "
	    "的倍率概念，实现是自己写的）：\n"
	    ";   固定倍率 → 并发 = Threads × 倍率（上限 64），分片同时切小\n"
	    ";   auto     → 从 Threads 起跑，按实测吞吐自动加/减，上限 3 倍\n"
	    "Enable = off\n"
	    "SpeedMultiplier = auto\n"
	    "\n"
	    "[Logging]\n"
	    "\n"
	    "; 日志总开关（on/off）\n"
	    "Enable = on\n"
	    "\n"
	    "; 级别：off(无输出) / low(错误和结果) / high(含分片进度) / max(含 "
	    "HTTP 调试)\n"
	    "Level = low\n"
	    "\n"
	    "; 输出：console(仅终端) / file(仅文件) / both(终端+文件)\n"
	    "Output = both\n"
	    "\n"
	    "; 日志文件（Output 含 file 时生效）\n"
	    "FilePath = ./edwl.log\n"
	    "\n"
	    "; 清理策略：1000L 保留最近 1000 行；30T 保留最近 30 "
	    "天（按天分文件）\n"
	    "Retention = 1000L\n"
	    "\n"
	    "; 时间戳格式（strftime）\n"
	    "TimeFormat = [%Y-%m-%d %H:%M:%S]\n"
	    "\n"
	    "[Blacklist]\n"
	    "\n"
	    "; 黑名单总开关（on/off）\n"
	    "Enable = on\n"
	    "\n"
	    "; 忽略的主机名（支持 * 通配符，逗号分隔；_.example.com 等价于 "
	    "*.example.com）\n"
	    "Hosts = localhost, 127.0.0.1\n"
	    "\n"
	    "; 忽略的 IP 段（CIDR，逗号分隔）\n"
	    "IPs = 192.168.1.0/24, 10.0.0.0/8\n"
	    "\n"
	    "; ==========================================\n"
	    "; 镜像源映射（多源聚合加速）\n"
	    "; 格式：原始域名 = 镜像1, 镜像2\n"
	    "; 不要写 http:// 或 https:// 前缀，直接写域名（可带路径前缀）\n"
	    "; ==========================================\n"
	    "\n"
	    "[Mirrors]\n"
	    "github.com = githubmirror1.com, mirror2.com\n"
	    "raw.githubusercontent.com = "
	    "ghproxy.com/raw.githubusercontent.com\n";
	FILE *f;
	int rc;

	if (!path)
		return -1;
	f = fopen(path, "wb");
	if (!f)
		return -1;
	rc = fputs(tmpl, f) < 0 ? -1 : 0;
	if (fclose(f) != 0)
		rc = -1;
	return rc;
}

void edwl_config_free(struct edwl_config *cfg)
{
	struct edwl_mirror_map *it;

	if (!cfg)
		return;
	it = cfg->mirrors;
	while (it) {
		struct edwl_mirror_map *next = it->next;

		free(it);
		it = next;
	}
	cfg->mirrors = NULL;
}

const struct edwl_mirror_map *
edwl_config_find_mirrors(const struct edwl_config *cfg, const char *host)
{
	const struct edwl_mirror_map *it;

	if (!cfg || !host)
		return NULL;
	for (it = cfg->mirrors; it; it = it->next) {
		if (ci_equal(it->original_host, host))
			return it;
	}
	return NULL;
}
