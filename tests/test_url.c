/*
 * edwl - URL 工具测试
 */
#include <edwl/util/url.h>

#include "test_util.h"

static void test_parse_basic(void)
{
	struct wgetx_url u;

	CHECK_INT(wgetx_url_parse("http://example.com/a/b.zip?x=1", &u), 0);
	CHECK_STR(u.scheme, "http");
	CHECK_STR(u.host, "example.com");
	CHECK_INT(u.port, 80);
	CHECK_STR(u.path, "/a/b.zip?x=1");
	CHECK_STR(u.host_header, "example.com");
}

static void test_parse_port_and_userinfo(void)
{
	struct wgetx_url u;

	CHECK_INT(wgetx_url_parse("https://user:pw@host.example:8443/p", &u),
		  0);
	CHECK_STR(u.scheme, "https");
	CHECK_STR(u.host, "host.example");
	CHECK_INT(u.port, 8443);
	CHECK_STR(u.host_header, "host.example:8443");
	CHECK_STR(u.path, "/p");
}

static void test_parse_ipv6(void)
{
	struct wgetx_url u;

	CHECK_INT(wgetx_url_parse("http://[::1]:8080/x", &u), 0);
	CHECK_STR(u.host, "::1");
	CHECK_INT(u.port, 8080);
	CHECK_STR(u.host_header, "[::1]:8080");
}

static void test_parse_empty_path(void)
{
	struct wgetx_url u;

	CHECK_INT(wgetx_url_parse("http://example.com", &u), 0);
	CHECK_STR(u.path, "/");
	CHECK_INT(u.port, 80);
}

static void test_parse_rejects(void)
{
	struct wgetx_url u;

	CHECK_INT(wgetx_url_parse("ftp://example.com/x", &u), -1);
	CHECK_INT(wgetx_url_parse("example.com/x", &u), -1);
	CHECK_INT(wgetx_url_parse("http://", &u), -1);
	CHECK_INT(wgetx_url_parse("http://host:99999/", &u), -1);
}

static void test_filename(void)
{
	char name[256];

	CHECK_INT(
	    wgetx_url_guess_filename("http://h/dir/file%20name.bin?token=1",
				     name, sizeof(name)),
	    0);
	CHECK_STR(name, "file name.bin");

	CHECK_INT(wgetx_url_guess_filename("http://h/", name, sizeof(name)), 0);
	CHECK_STR(name, "index.html");
}

static void test_sanitize(void)
{
	char out[64];

	wgetx_url_sanitize_filename("a:b*c?d", out, sizeof(out));
	CHECK_STR(out, "a_b_c_d");
}

static void test_range(void)
{
	char buf[64];

	CHECK_INT(wgetx_url_format_range(0, 99, buf, sizeof(buf)), 0);
	CHECK_STR(buf, "bytes=0-99");
	CHECK_INT(wgetx_url_format_range(10, 9, buf, sizeof(buf)), -1);
}

static void test_content_range(void)
{
	uint64_t b = 0, e = 0, total = 0;

	CHECK_INT(
	    wgetx_url_parse_content_range("bytes 0-0/12345", &b, &e, &total),
	    0);
	CHECK_INT(b, 0);
	CHECK_INT(e, 0);
	CHECK_INT(total, 12345);

	CHECK_INT(
	    wgetx_url_parse_content_range("bytes 100-199/*", &b, &e, &total),
	    0);
	CHECK_INT(b, 100);
	CHECK_INT(e, 199);
	CHECK_INT(total, 0);

	CHECK_INT(wgetx_url_parse_content_range("garbage", &b, &e, &total), -1);
}

int main(void)
{
	test_parse_basic();
	test_parse_port_and_userinfo();
	test_parse_ipv6();
	test_parse_empty_path();
	test_parse_rejects();
	test_filename();
	test_sanitize();
	test_range();
	test_content_range();
	return test_finish("test_url");
}
