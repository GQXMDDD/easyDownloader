/*
 * edwl - 公共 API
 *
 * 对外只有两个入口：wgetx_version() 与 wgetx_main()。
 * 想当库用的话，直接拿 core/downloader.h 的 wgetx_download_run()。
 *
 * 项目代号 edwl（别名 wgetx），所以目录叫 include/edwl、符号前缀叫 wgetx_。
 */
#ifndef WGETX_WGETX_H
#define WGETX_WGETX_H

#include <stddef.h>
#include <stdint.h>

#define WGETX_PROGRAM_NAME "edwl"
#define WGETX_VERSION_STRING "0.1.0"
#define WGETX_VERSION_MAJOR 0
#define WGETX_VERSION_MINOR 1
#define WGETX_VERSION_PATCH 0

/* 退出码：与 wget 习惯保持一致 */
#define WGETX_EXIT_OK 0
#define WGETX_EXIT_ERROR 1
#define WGETX_EXIT_USAGE 2
#define WGETX_EXIT_CHECKSUM 3
#define WGETX_EXIT_INTERRUPTED 130

/* 版本字符串 */
const char *wgetx_version(void);

/* 命令行入口，等价于运行 edwl。返回进程退出码 */
int wgetx_main(int argc, char **argv);

#endif /* WGETX_WGETX_H */
