/*
 * edwl - 测试用的小工具
 *
 * 不引第三方测试框架：几个宏足够，测试程序自己返回失败数。
 */
#ifndef EDWL_TEST_UTIL_H
#define EDWL_TEST_UTIL_H

#include <stdio.h>
#include <string.h>

static int test_failures;

#define CHECK(cond)                                                            \
	do {                                                                   \
		if (!(cond)) {                                                 \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,          \
				__LINE__, #cond);                              \
			test_failures++;                                       \
		}                                                              \
	} while (0)

#define CHECK_INT(actual, expect)                                              \
	do {                                                                   \
		long long a_ = (long long)(actual);                            \
		long long e_ = (long long)(expect);                            \
		if (a_ != e_) {                                                \
			fprintf(stderr, "FAIL %s:%d: %s = %lld, 期望 %lld\n",  \
				__FILE__, __LINE__, #actual, a_, e_);          \
			test_failures++;                                       \
		}                                                              \
	} while (0)

#define CHECK_STR(actual, expect)                                              \
	do {                                                                   \
		const char *a_ = (actual);                                     \
		const char *e_ = (expect);                                     \
		if (!a_ || !e_ || strcmp(a_, e_) != 0) {                       \
			fprintf(stderr, "FAIL %s:%d: \"%s\" != \"%s\"\n",      \
				__FILE__, __LINE__, a_ ? a_ : "(null)",        \
				e_ ? e_ : "(null)");                           \
			test_failures++;                                       \
		}                                                              \
	} while (0)

static int test_finish(const char *name)
{
	if (test_failures == 0) {
		printf("ok - %s\n", name);
		return 0;
	}
	printf("not ok - %s (%d 处失败)\n", name, test_failures);
	return 1;
}

#endif /* EDWL_TEST_UTIL_H */
