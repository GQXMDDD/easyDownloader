/*
 * edwl - Win32 平台：文件随机读写
 *
 * 关于 OVERLAPPED 的一个重要事实（与原设计里的映射表不同，已实测修正）：
 * 只有以 FILE_FLAG_OVERLAPPED 打开的句柄，WriteFile/ReadFile 才会使用
 * OVERLAPPED 里的 Offset；普通同步句柄会忽略 Offset、改用内部文件指针，
 * 多线程共用一个句柄时必然互相踩。所以这里一律带 FILE_FLAG_OVERLAPPED，
 * 并在 ERROR_IO_PENDING 时 GetOverlappedResult 等完成——行为仍是同步阻塞，
 * 但偏移是每个调用独立的，等价于 pwrite/pread。
 */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include <windows.h>

#include <edwl/platform/platform.h>

static HANDLE as_handle(edwl_file_t fd)
{
	return (HANDLE)(uintptr_t)fd;
}

int edwl_file_open_write(const char *path, edwl_file_t *out)
{
	HANDLE h;

	if (!path || !out)
		return -1;

	h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
			NULL, OPEN_ALWAYS,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return -1;

	*out = (edwl_file_t)(intptr_t)(uintptr_t)h;
	return 0;
}

int edwl_file_open_read(const char *path, edwl_file_t *out)
{
	HANDLE h;

	if (!path || !out)
		return -1;

	h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
			NULL, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return -1;

	*out = (edwl_file_t)(intptr_t)(uintptr_t)h;
	return 0;
}

int edwl_file_truncate(edwl_file_t fd, uint64_t size)
{
	HANDLE h = as_handle(fd);
	LARGE_INTEGER li;

	if (fd < 0)
		return -1;

	li.QuadPart = (LONGLONG)size;
	if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN))
		return -1;
	if (!SetEndOfFile(h))
		return -1;
	return 0;
}

int edwl_file_size(edwl_file_t fd, uint64_t *out)
{
	HANDLE h = as_handle(fd);
	LARGE_INTEGER li;

	if (fd < 0 || !out)
		return -1;
	if (!GetFileSizeEx(h, &li))
		return -1;
	*out = (uint64_t)li.QuadPart;
	return 0;
}

static void set_offset(OVERLAPPED *ov, uint64_t offset)
{
	memset(ov, 0, sizeof(*ov));
	ov->Offset = (DWORD)(offset & 0xFFFFFFFFULL);
	ov->OffsetHigh = (DWORD)((offset >> 32) & 0xFFFFFFFFULL);
}

int64_t edwl_file_pwrite(edwl_file_t fd, const void *buf, size_t count,
			 uint64_t offset)
{
	HANDLE h = as_handle(fd);
	OVERLAPPED ov;
	DWORD written = 0;

	if (fd < 0 || count > 0x7FFFFFFFUL)
		return -1;

	set_offset(&ov, offset);
	if (!WriteFile(h, buf, (DWORD)count, &written, &ov)) {
		if (GetLastError() != ERROR_IO_PENDING)
			return -1;
		if (!GetOverlappedResult(h, &ov, &written, TRUE))
			return -1;
	}
	return (int64_t)written;
}

int64_t edwl_file_pread(edwl_file_t fd, void *buf, size_t count,
			uint64_t offset)
{
	HANDLE h = as_handle(fd);
	OVERLAPPED ov;
	DWORD got = 0;
	DWORD err;

	if (fd < 0 || count > 0x7FFFFFFFUL)
		return -1;

	set_offset(&ov, offset);
	if (!ReadFile(h, buf, (DWORD)count, &got, &ov)) {
		err = GetLastError();
		/*
		 * 带 FILE_FLAG_OVERLAPPED 的句柄读到文件尾时返回
		 * ERROR_HANDLE_EOF，而不是"成功读 0 字节"——必须当成 EOF 处理，
		 * 否则校验阶段永远读不到结尾（这是实测踩到的坑）。
		 */
		if (err == ERROR_HANDLE_EOF)
			return 0;
		if (err != ERROR_IO_PENDING)
			return -1;
		if (!GetOverlappedResult(h, &ov, &got, TRUE)) {
			err = GetLastError();
			if (err == ERROR_HANDLE_EOF)
				return 0;
			return -1;
		}
	}
	return (int64_t)got;
}

int edwl_file_close(edwl_file_t fd)
{
	if (fd < 0)
		return -1;
	return CloseHandle(as_handle(fd)) ? 0 : -1;
}
