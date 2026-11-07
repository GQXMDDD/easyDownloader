/*
 * edwl - 平台文件 IO 测试
 *
 * 重点验证"多线程共用一个句柄时，定位写/读是不是真的按偏移走"。
 * 这个测试在 Windows 上尤其有意义：OVERLAPPED 的 Offset 只有句柄带
 * FILE_FLAG_OVERLAPPED 时才会被尊重（见 file_io_win32.c 的说明）。
 *
 * 注意：写句柄按 POSIX 语义只给了写权限，要读就另外开一个只读句柄。
 */
#include <string.h>

#include <edwl/platform/platform.h>

#include "test_util.h"

#define TEST_FILE "edwl_test_io.tmp"

static void test_random_write_read(void)
{
	edwl_file_t w = (edwl_file_t)-1;
	edwl_file_t r = (edwl_file_t)-1;
	char buf[16];
	uint64_t size = 0;

	remove(TEST_FILE);

	CHECK_INT(edwl_file_open_write(TEST_FILE, &w), 0);
	CHECK(w >= 0);
	CHECK_INT(edwl_file_truncate(w, 100), 0);

	CHECK_INT(edwl_file_pwrite(w, "AAAA", 4, 0), 4);
	CHECK_INT(edwl_file_pwrite(w, "BBBB", 4, 50), 4);

	CHECK_INT(edwl_file_size(w, &size), 0);
	CHECK_INT(size, 100);
	CHECK_INT(edwl_file_close(w), 0);

	CHECK_INT(edwl_file_open_read(TEST_FILE, &r), 0);

	memset(buf, 0, sizeof(buf));
	CHECK_INT(edwl_file_pread(r, buf, 4, 0), 4);
	CHECK_STR(buf, "AAAA");

	memset(buf, 0, sizeof(buf));
	CHECK_INT(edwl_file_pread(r, buf, 4, 50), 4);
	CHECK_STR(buf, "BBBB");

	/* 没写过的地方应该还是 0 */
	memset(buf, 1, sizeof(buf));
	CHECK_INT(edwl_file_pread(r, buf, 4, 10), 4);
	CHECK_INT(buf[0], 0);

	/* 正好读到文件尾 / 跨过文件尾，都不能算错误 */
	memset(buf, 0, sizeof(buf));
	CHECK_INT(edwl_file_pread(r, buf, 4, 100), 0);
	memset(buf, 0, sizeof(buf));
	CHECK_INT(edwl_file_pread(r, buf, 4, 98), 2);

	/* 只读句柄写入必须失败 */
	CHECK(edwl_file_pwrite(r, "XX", 2, 0) < 0);

	CHECK_INT(edwl_file_close(r), 0);
}

static void test_missing_file(void)
{
	edwl_file_t fd = (edwl_file_t)-1;

	CHECK_INT(edwl_file_open_read("edwl_no_such_file.tmp", &fd), -1);
}

int main(void)
{
	test_random_write_read();
	test_missing_file();
	remove(TEST_FILE);
	return test_finish("test_file_io");
}
