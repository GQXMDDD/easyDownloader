/*
 * edwl - 黑名单测试
 */
#include <edwl/core/blacklist.h>

#include "test_util.h"

static void test_hosts(void)
{
	struct edwl_blacklist *b = NULL;
	char why[128];

	CHECK_INT(edwl_blacklist_create(
		      "localhost, *.baidu.com, 127.0.0.1, _.sina.com.cn",
		      "192.168.1.0/24, 10.0.0.0/8", &b),
		  0);
	CHECK_INT(edwl_blacklist_host_count(b), 4);
	CHECK_INT(edwl_blacklist_ip_count(b), 2);

	CHECK_INT(edwl_blacklist_hit_host(b, "localhost", why, sizeof(why)), 1);
	CHECK(why[0] != '\0');
	/* 精确模式不匹配子域 */
	CHECK_INT(edwl_blacklist_hit_host(b, "sub.localhost", why, sizeof(why)),
		  0);

	/* *.baidu.com：本身和所有子域都算 */
	CHECK_INT(edwl_blacklist_hit_host(b, "baidu.com", why, sizeof(why)), 1);
	CHECK_INT(edwl_blacklist_hit_host(b, "www.baidu.com", why, sizeof(why)),
		  1);
	CHECK_INT(edwl_blacklist_hit_host(b, "a.b.baidu.com", why, sizeof(why)),
		  1);
	/* 不能误伤"看着像"的域名 */
	CHECK_INT(edwl_blacklist_hit_host(b, "abbaidu.com", why, sizeof(why)),
		  0);

	/* _.sina.com.cn 视同 *.sina.com.cn（大小写不敏感） */
	CHECK_INT(edwl_blacklist_hit_host(b, "Sina.com.cn", why, sizeof(why)),
		  1);
	CHECK_INT(edwl_blacklist_hit_host(b, "x.SINA.com.cn", why, sizeof(why)),
		  1);

	edwl_blacklist_destroy(b);
}

static void test_ips(void)
{
	struct edwl_blacklist *b = NULL;
	char why[128];

	CHECK_INT(edwl_blacklist_create("localhost",
					"192.168.1.0/24, 10.0.0.0/8", &b),
		  0);

	CHECK_INT(edwl_blacklist_hit_url(b, "http://192.168.1.5/a.bin", why,
					 sizeof(why)),
		  1);
	CHECK(why[0] != '\0');
	CHECK_INT(
	    edwl_blacklist_hit_url(b, "http://10.20.30.40/x", why, sizeof(why)),
	    1);
	CHECK_INT(
	    edwl_blacklist_hit_url(b, "http://8.8.8.8/x", why, sizeof(why)), 0);
	CHECK_INT(edwl_blacklist_hit_url(b, "https://93.184.216.34/a", why,
					 sizeof(why)),
		  0);

	/* 主机名同时受 hosts 规则约束 */
	CHECK_INT(edwl_blacklist_hit_url(b, "http://localhost:8080/x", why,
					 sizeof(why)),
		  1);

	edwl_blacklist_destroy(b);
}

static void test_disabled_or_empty(void)
{
	struct edwl_blacklist *b = NULL;
	char why[64];

	CHECK_INT(edwl_blacklist_create("", "", &b), 0);
	CHECK_INT(edwl_blacklist_host_count(b), 0);
	CHECK_INT(edwl_blacklist_ip_count(b), 0);
	CHECK_INT(
	    edwl_blacklist_hit_url(b, "http://example.com/a", why, sizeof(why)),
	    0);
	edwl_blacklist_destroy(b);

	/* NULL 也要安全 */
	CHECK_INT(edwl_blacklist_hit_url(NULL, "http://example.com/a", why,
					 sizeof(why)),
		  0);
}

static void test_bad_input(void)
{
	struct edwl_blacklist *b = NULL;

	/* 写错的 CIDR 直接跳过，不影响其它规则 */
	CHECK_INT(
	    edwl_blacklist_create("", "999.1.1.1/24, 10.0.0.0/99, ok", &b), 0);
	CHECK_INT(edwl_blacklist_ip_count(b), 0);

	edwl_blacklist_destroy(b);
}

int main(void)
{
	test_hosts();
	test_ips();
	test_disabled_or_empty();
	test_bad_input();
	return test_finish("test_blacklist");
}
