/*
 * edwl - URL 工具：解析、取名、Range 头拼装
 *
 * 不追求 RFC 3986 全覆盖，只覆盖 http(s) 下载场景够用的部分。
 */
#ifndef WGETX_UTIL_URL_H
#define WGETX_UTIL_URL_H

#include <stddef.h>
#include <stdint.h>

#define WGETX_URL_MAX 2048

struct wgetx_url {
	char scheme[16];	  /* 小写，"http" / "https" */
	char host[256];		  /* 主机名或 IP，不含方括号 */
	int port;		  /* 0 表示按 scheme 取默认值 */
	char path[WGETX_URL_MAX]; /* 含查询串，至少是 "/" */
	char host_header[300];	  /* Host: 头的值，host[:port] */
};

/*
 * 解析绝对 URL。0 成功，-1 失败（缺 scheme、缺 host 等）。
 * 只接受 http/https。
 */
int wgetx_url_parse(const char *url, struct wgetx_url *out);

/* 按 scheme 取默认端口 */
int wgetx_url_default_port(const char *scheme);

/*
 * 由 URL 猜测本地文件名：取 path 最后一段，百分号解码，
 * 非法字符替换为 '_'，空则返回 "index.html"。0 成功，-1 失败。
 */
int wgetx_url_guess_filename(const char *url, char *out, size_t out_size);

/* 文件名消毒：去掉路径分隔符、Windows 保留字符、控制字符 */
int wgetx_url_sanitize_filename(const char *in, char *out, size_t out_size);

/* 拼 "bytes=begin-end"，用于 Range 头。0 成功，-1 参数非法（begin > end） */
int wgetx_url_format_range(uint64_t begin, uint64_t end, char *out,
			   size_t out_size);

/*
 * 解析 "Content-Range: bytes 0-1023/4096"。
 * total 允许为 '*'，此时 *total 置 0。0 成功，-1 失败。
 */
int wgetx_url_parse_content_range(const char *value, uint64_t *begin,
				  uint64_t *end, uint64_t *total);

/* 判断 URL 是否 http/https 绝对地址 */
int wgetx_url_is_absolute(const char *url);

#endif /* WGETX_UTIL_URL_H */
