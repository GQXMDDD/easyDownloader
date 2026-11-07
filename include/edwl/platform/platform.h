/*
 * edwl - 平台抽象层统一入口
 *
 * 规矩（见 README 的「依赖规则」一节）：
 * core/ 与 util/ 下的 .c 只允许 include 标准 C 头、项目自己的头，
 * 以及本文件。出现 <windows.h> / <unistd.h> / <pthread.h> / <curl/curl.h>
 * 都算违规（http_client.c 的 curl 头是唯一例外）。
 */
#ifndef EDWL_PLATFORM_H
#define EDWL_PLATFORM_H

#include "file_io.h"
#include "thread.h"
#include "time.h"
#include "console.h"

#endif /* EDWL_PLATFORM_H */
