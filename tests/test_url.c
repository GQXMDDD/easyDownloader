/*
 * edwl - URL 工具测试
 */
#include <edwl/util/url.h>

#include "test_util.h"

static void test_parse_basic(void)
{
	struct edwl_url u;

	CHECK_INT(edwl_url_parse("http://example.com/a/b.zip?x=1", &u), 0);
	CHECK_STR(u.scheme, "http");
	CHECK_STR(u.host, "example.com");
	CHECK_INT(u.port, 80);
	CHECK_STR(u.path, "/a/b.zip?x=1");
	CHECK_STR(u.host_header, "example.com");
}

static void test_parse_port_and_userinfo(void)
{
	struct edwl_url u;

	CHECK_INT(edwl_url_parse("https://user:pw@host.example:8443/p", &u), 0);
	CHECK_STR(u.scheme, "https");
	CHECK_STR(u.host, "host.example");
	CHECK_INT(u.port, 8443);
	CHECK_STR(u.host_header, "host.example:8443");
	CHECK_STR(u.path, "/p");
}

static void test_parse_ipv6(void)
{
	struct edwl_url u;

	CHECK_INT(edwl_url_parse("http://[::1]:8080/x", &u), 0);
	CHECK_STR(u.host, "::1");
	CHECK_INT(u.port, 8080);
	CHECK_STR(u.host_header, "[::1]:8080");
}

static void test_parse_empty_path(void)
{
	struct edwl_url u;

	CHECK_INT(edwl_url_parse("http://example.com", &u), 0);
	CHECK_STR(u.path, "/");
	CHECK_INT(u.port, 80);
}

static void test_parse_rejects(void)
{
	struct edwl_url u;

	CHECK_INT(edwl_url_parse("ftp://example.com/x", &u), -1);
	CHECK_INT(edwl_url_parse("example.com/x", &u), -1);
	CHECK_INT(edwl_url_parse("http://", &u), -1);
	CHECK_INT(edwl_url_parse("http://host:99999/", &u), -1);
}

static void test_filename(void)
{
	char name[256];

	CHECK_INT(
	    edwl_url_guess_filename("http://h/dir/file%20name.bin?token=1",
				    name, sizeof(name)),
	    0);
	CHECK_STR(name, "file name.bin");

	CHECK_INT(edwl_url_guess_filename("http://h/", name, sizeof(name)), 0);
	CHECK_STR(name, "index.html");
}

static void test_github_repository(void)
{
	static const char *inputs[] = {
	    "octocat/Hello-World.git",
	    "octocat/Hello-World",
	    "octocat/Hello-World.git/",
	    "github.com/octocat/Hello-World.git",
	    "git@github.com:octocat/Hello-World.git",
	    "https://github.com/octocat/Hello-World.git",
	    "https://GITHUB.com/octocat/Hello-World/?tab=readme#top",
	};
	static const char *unchanged[] = {
	    "https://example.com/owner/repo.git",
	    "https://github.com/owner/repo/releases/download/v1/app.zip?x=1",
	    "https://github.com/owner/repo/archive/refs/heads/main.zip",
	    "https://raw.githubusercontent.com/owner/repo/main/file.c",
	    "https://github.com/owner/repo/blob/main/file.c",
	    "https://github.com.evil.example/owner/repo.git",
	};
	static const char *invalid[] = {
	    "",
	    "owner",
	    "/repo.git",
	    "owner/.git",
	    "owner/../repo.git",
	    "owner/repo.git/extra",
	    "owner/repo.git?x=1",
	    "-owner/repo.git",
	    "owner/re po.git",
	    "owner\\repo.git",
	    "ftp://github.com/owner/repo.git",
	};
	char url[EDWL_URL_MAX];
	char name[128];
	size_t i;

	for (i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
		CHECK_INT(
		    edwl_url_normalize_github(inputs[i], url, sizeof(url)), 1);
		CHECK_STR(
		    url,
		    "https://codeload.github.com/octocat/Hello-World/zip/HEAD");
		CHECK_INT(edwl_url_guess_filename(url, name, sizeof(name)), 0);
		CHECK_STR(name, "Hello-World.zip");
	}
	for (i = 0; i < sizeof(unchanged) / sizeof(unchanged[0]); i++) {
		CHECK_INT(
		    edwl_url_normalize_github(unchanged[i], url, sizeof(url)),
		    0);
		CHECK_STR(url, unchanged[i]);
	}
	for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
		CHECK_INT(
		    edwl_url_normalize_github(invalid[i], url, sizeof(url)),
		    -1);
	CHECK_INT(edwl_url_normalize_github("owner/repo.git", url, 8), -1);
	CHECK_INT(edwl_url_guess_filename(
		      "https://github.com/owner/repo/archive/HEAD.zip", name,
		      sizeof(name)),
		  0);
	CHECK_STR(name, "repo.zip");
	CHECK_INT(edwl_url_github_repo("github.com/octocat/Hello-World.git",
				       name, sizeof(name)),
		  0);
	CHECK_STR(name, "octocat/Hello-World");
	CHECK_INT(edwl_url_github_repo("https://example.com/owner/repo.git",
				       name, sizeof(name)),
		  -1);
	CHECK_INT(
	    edwl_url_normalize_github("owner/repo.git.git", url, sizeof(url)),
	    1);
	CHECK_INT(edwl_url_guess_filename(url, name, sizeof(name)), 0);
	CHECK_STR(name, "repo.git.zip");
}

static void test_sanitize(void)
{
	char out[64];

	edwl_url_sanitize_filename("a:b*c?d", out, sizeof(out));
	CHECK_STR(out, "a_b_c_d");
}

static void test_range(void)
{
	char buf[64];

	CHECK_INT(edwl_url_format_range(0, 99, buf, sizeof(buf)), 0);
	CHECK_STR(buf, "bytes=0-99");
	CHECK_INT(edwl_url_format_range(10, 9, buf, sizeof(buf)), -1);
}

static void test_content_range(void)
{
	uint64_t b = 0, e = 0, total = 0;

	CHECK_INT(
	    edwl_url_parse_content_range("bytes 0-0/12345", &b, &e, &total), 0);
	CHECK_INT(b, 0);
	CHECK_INT(e, 0);
	CHECK_INT(total, 12345);

	CHECK_INT(
	    edwl_url_parse_content_range("bytes 100-199/*", &b, &e, &total), 0);
	CHECK_INT(b, 100);
	CHECK_INT(e, 199);
	CHECK_INT(total, 0);

	CHECK_INT(edwl_url_parse_content_range("garbage", &b, &e, &total), -1);
}

int main(void)
{
	test_parse_basic();
	test_parse_port_and_userinfo();
	test_parse_ipv6();
	test_parse_empty_path();
	test_parse_rejects();
	test_filename();
	test_github_repository();
	test_sanitize();
	test_range();
	test_content_range();
	return test_finish("test_url");
}
