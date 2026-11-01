/*
 * edwl - POSIX 平台：文件随机读写
 *
 * pwrite/pread 天然就是定位读写，且多线程共用一个 fd 是安全的
 * （每次调用自带偏移，不共享文件位置指针）。
 */
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <edwl/platform/platform.h>

int wgetx_file_open_write(const char *path, wgetx_file_t *out)
{
	int fd;

	if (!path || !out)
		return -1;

	/*
	 * 注意：这里刻意不带 O_TRUNC。断点续传要保留老内容，
	 * 最终长度由 wgetx_file_truncate() 统一设定。
	 */
	fd = open(path, O_WRONLY | O_CREAT, 0644);
	if (fd < 0)
		return -1;

	*out = (wgetx_file_t)fd;
	return 0;
}

int wgetx_file_open_read(const char *path, wgetx_file_t *out)
{
	int fd;

	if (!path || !out)
		return -1;
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	*out = (wgetx_file_t)fd;
	return 0;
}

int wgetx_file_truncate(wgetx_file_t fd, uint64_t size)
{
	if (fd < 0)
		return -1;
	if (ftruncate((int)fd, (off_t)size) != 0)
		return -1;
	return 0;
}

int wgetx_file_size(wgetx_file_t fd, uint64_t *out)
{
	struct stat st;

	if (fd < 0 || !out)
		return -1;
	if (fstat((int)fd, &st) != 0)
		return -1;
	*out = (uint64_t)st.st_size;
	return 0;
}

int64_t wgetx_file_pwrite(wgetx_file_t fd, const void *buf, size_t count,
			  uint64_t offset)
{
	ssize_t n;

	if (fd < 0)
		return -1;
	do {
		n = pwrite((int)fd, buf, count, (off_t)offset);
	} while (n < 0 && errno == EINTR);

	if (n < 0)
		return -1;
	return (int64_t)n;
}

int64_t wgetx_file_pread(wgetx_file_t fd, void *buf, size_t count,
			 uint64_t offset)
{
	ssize_t n;

	if (fd < 0)
		return -1;
	do {
		n = pread((int)fd, buf, count, (off_t)offset);
	} while (n < 0 && errno == EINTR);

	if (n < 0)
		return -1;
	return (int64_t)n;
}

int wgetx_file_close(wgetx_file_t fd)
{
	if (fd < 0)
		return -1;
	return close((int)fd) == 0 ? 0 : -1;
}
