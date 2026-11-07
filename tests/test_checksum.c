/*
 * edwl - SHA-256 测试（FIPS 180-4 标准向量）
 */
#include <stdlib.h>
#include <string.h>

#include <edwl/core/checksum.h>

#include "test_util.h"

static void test_vectors(void)
{
	char hex[EDWL_SHA256_HEX];

	edwl_sha256_buf("", 0, hex);
	CHECK_STR(
	    hex,
	    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

	edwl_sha256_buf("abc", 3, hex);
	CHECK_STR(
	    hex,
	    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

	edwl_sha256_buf(
	    "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
	    hex);
	CHECK_STR(
	    hex,
	    "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

static void test_long_input(void)
{
	static const char expect[] =
	    "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0";
	char *buf;
	char hex[EDWL_SHA256_HEX];

	buf = malloc(1000000);
	if (!buf) {
		CHECK(0);
		return;
	}
	memset(buf, 'a', 1000000);
	edwl_sha256_buf(buf, 1000000, hex);
	CHECK_STR(hex, expect);
	free(buf);
}

static void test_hash_helpers(void)
{
	CHECK(edwl_hash_is_valid("ba7816bf8f01cfea414140de5dae2223b00361a39617"
				 "7a9cb410ff61f20015ad"));
	CHECK(!edwl_hash_is_valid("zz7816bf"));
	CHECK(!edwl_hash_is_valid(NULL));

	/* 大小写不敏感，且容忍结尾换行 */
	CHECK(edwl_hash_equals(
	    "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD",
	    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
	    "\n"));
	CHECK(!edwl_hash_equals(
	    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
	    "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd"
	    "0"));
}

int main(void)
{
	test_vectors();
	test_long_input();
	test_hash_helpers();
	return test_finish("test_checksum");
}
