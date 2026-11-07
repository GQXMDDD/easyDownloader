/*
 * edwl - 公共 API
 *
 * 对外只有两个入口：edwl_version() 与 edwl_main()。
 * 想当库用的话，直接拿 core/downloader.h 的 edwl_download_run()。
 *
 * 项目统一用 edwl 这个代号：目录 include/edwl，符号前缀 edwl_，本头文件即
 * edwl.h。
 */
#ifndef EDWL_EDWL_H
#define EDWL_EDWL_H

#include <stddef.h>
#include <stdint.h>

#define EDWL_PROGRAM_NAME "edwl"
#define EDWL_VERSION_STRING "0.2.0"
#define EDWL_VERSION_MAJOR 0
#define EDWL_VERSION_MINOR 2
#define EDWL_VERSION_PATCH 0

/* 退出码：与 wget 习惯保持一致 */
#define EDWL_EXIT_OK 0
#define EDWL_EXIT_ERROR 1
#define EDWL_EXIT_USAGE 2
#define EDWL_EXIT_CHECKSUM 3
#define EDWL_EXIT_INTERRUPTED 130

/* 版本字符串 */
const char *edwl_version(void);

/* 命令行入口，等价于运行 edwl。返回进程退出码 */
int edwl_main(int argc, char **argv);

#endif /* EDWL_EDWL_H */
