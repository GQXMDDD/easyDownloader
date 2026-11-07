/*
 * edwl - 配置文件测试
 */
#include <stdio.h>
#include <string.h>

#include <edwl/core/config.h>

#include "test_util.h"

static const char *TMP_INI = "edwl_test_config.ini";

static void write_file(const char *path, const char *text)
{
	FILE *f = fopen(path, "wb");

	if (!f) {
		CHECK(0);
		return;
	}
	fputs(text, f);
	fclose(f);
}

static void test_defaults(void)
{
	struct edwl_config cfg;

	edwl_config_init_defaults(&cfg);
	CHECK_INT(cfg.threads, 8);
	CHECK_INT(cfg.dynamic_threads, 1);
	CHECK_INT(cfg.max_speed, 0);
	CHECK_INT(cfg.timeout, 15);
	CHECK_INT(cfg.retries, 3);
	CHECK_STR(cfg.retry_backoff, "exponential");
	CHECK_STR(cfg.resume_ext, ".edwl");
	CHECK_INT(cfg.blacklist_enable, 1);
	CHECK_STR(cfg.blacklist_hosts, "");
	CHECK(!cfg.mirrors);
	edwl_config_free(&cfg);
}

static void test_custom(void)
{
	struct edwl_config cfg;
	const struct edwl_mirror_map *m;
	static const char ini[] =
	    "[General]\n"
	    "threads = 4\n"
	    "RetryBackoff = fixed\n"
	    "ChunkSizeMB = 2\n"
	    "ResumeFileExt = .part\n"
	    "Timeout = 7\n"
	    "\n"
	    "[Logging]\n"
	    "Level = high\n"
	    "Output = console\n"
	    "Retention = 30T\n"
	    "\n"
	    "[Blacklist]\n"
	    "Enable = off\n"
	    "Hosts = *.example.com, localhost\n"
	    "IPs = 192.168.1.0/24\n"
	    "\n"
	    "[Mirrors]\n"
	    "github.com = a.com, b.com\n"
	    "raw.githubusercontent.com = ghproxy.com/raw\n";

	write_file(TMP_INI, ini);
	edwl_config_init_defaults(&cfg);
	CHECK_INT(edwl_config_load(&cfg, TMP_INI), 1);

	CHECK_INT(cfg.threads, 4);
	CHECK_STR(cfg.retry_backoff, "fixed");
	CHECK_INT(cfg.chunk_size_mb, 2);
	CHECK_STR(cfg.resume_ext, ".part");
	CHECK_INT(cfg.timeout, 7);
	CHECK_STR(cfg.log_level, "high");
	CHECK_STR(cfg.log_output, "console");
	CHECK_STR(cfg.log_retention, "30T");
	CHECK_INT(cfg.blacklist_enable, 0);
	CHECK_STR(cfg.blacklist_hosts, "*.example.com, localhost");
	CHECK_STR(cfg.blacklist_ips, "192.168.1.0/24");

	m = edwl_config_find_mirrors(&cfg, "GitHub.com");
	CHECK(m != NULL);
	if (m)
		CHECK_STR(m->mirrors, "a.com, b.com");
	CHECK(!edwl_config_find_mirrors(&cfg, "gitlab.com"));

	edwl_config_free(&cfg);
	remove(TMP_INI);
}

static void test_missing_file(void)
{
	struct edwl_config cfg;

	edwl_config_init_defaults(&cfg);
	CHECK_INT(edwl_config_load(&cfg, "edwl_no_such_config.ini"), 0);
	CHECK_INT(cfg.threads, 8);
	edwl_config_free(&cfg);
}

static void test_generate(void)
{
	struct edwl_config cfg;
	const struct edwl_mirror_map *m;
	int rc;

	remove(TMP_INI);
	rc = edwl_config_generate(TMP_INI);
	CHECK_INT(rc, 0);

	edwl_config_init_defaults(&cfg);
	CHECK_INT(edwl_config_load(&cfg, TMP_INI), 1);
	/* 模板里的值应当能被正确读回 */
	CHECK_INT(cfg.threads, 8);
	CHECK_STR(cfg.log_level, "high");
	CHECK_INT(cfg.blacklist_enable, 1);
	CHECK(strstr(cfg.blacklist_hosts, "localhost") != NULL);
	m = edwl_config_find_mirrors(&cfg, "github.com");
	CHECK(m != NULL);

	edwl_config_free(&cfg);
	remove(TMP_INI);
}

static void test_multiplier(void)
{
	double m = 0;
	int is_auto = 0;

	CHECK_INT(edwl_config_parse_multiplier("auto", &m, &is_auto), 0);
	CHECK_INT(is_auto, 1);
	CHECK_INT(edwl_config_parse_multiplier("2.5", &m, &is_auto), 0);
	CHECK_INT(is_auto, 0);
	CHECK(m > 2.49 && m < 2.51);
	CHECK_INT(edwl_config_parse_multiplier(" 3 ", &m, &is_auto), 0);
	CHECK(m > 2.99 && m < 3.01);
	CHECK_INT(edwl_config_parse_multiplier("abc", &m, &is_auto), -1);
	CHECK_INT(edwl_config_parse_multiplier("0.1", &m, &is_auto), -1);
	CHECK_INT(edwl_config_parse_multiplier("9", &m, &is_auto), -1);
}

int main(void)
{
	test_multiplier();
	test_defaults();
	test_custom();
	test_missing_file();
	test_generate();
	return test_finish("test_config");
}
