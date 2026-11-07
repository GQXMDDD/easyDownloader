/*
 * edwl - HTTP 抽象层（libcurl 封装）
 *
 * 依赖规则里唯一的例外：<curl/curl.h> 只允许出现在 http_client.c。
 * 本头只暴露自定义结构体和回调，将来换 WinHTTP / nghttp2 只改一个 .c。
 *
 * 一个 edwl_http 就是一条连接上下文，**不是线程安全的**：
 * 每个 worker 线程各持一个。
 */
#ifndef EDWL_CORE_HTTP_CLIENT_H
#define EDWL_CORE_HTTP_CLIENT_H

#include <stddef.h>
#include <stdint.h>

/* 统一返回码 */
#define EDWL_HTTP_OK 0
#define EDWL_HTTP_ERR (-1)		 /* 传输/协议错误 */
#define EDWL_HTTP_ERR_RANGE_IGNORED (-2) /* 服务器无视 Range，回了 200 */
#define EDWL_HTTP_ERR_INTERRUPTED (-3)	 /* 用户中断或回调节流 */
#define EDWL_HTTP_ERR_LOCAL_IO (-4)	 /* 本地写盘失败 */
#define EDWL_HTTP_ERR_SHORT (-5)	 /* 连接提前结束，只拿到一部分 */

#define EDWL_HTTP_ERR_LEN 256

struct edwl_http;

struct edwl_http_opts {
	const char *user_agent;	     /* NULL 用内置 UA */
	unsigned connect_timeout_ms; /* 0 用默认 15000 */
	unsigned low_speed_time_ms;  /* 0 关闭低速判定 */
	uint64_t low_speed_limit;    /* 字节/秒，0 关闭 */
	uint64_t max_recv_speed;     /* 字节/秒，0 不限 */
	const char *proxy;	     /* NULL 跟随环境变量；"" 表示强制直连 */
	unsigned max_redirects;	     /* 0 用默认 10 */
	int verbose;		     /* 1 = libcurl 详细调试输出 */
};

struct edwl_http_info {
	long status;		  /* 最终响应码 */
	uint64_t content_length;  /* 响应里的文件总长，0 未知 */
	int range_supported;	  /* 服务器是否认 Range */
	char etag[128];		  /* 可能为空 */
	char last_modified[64];	  /* 可能为空 */
	char content_type[128];	  /* 可能为空 */
	char effective_url[2048]; /* 跟随跳转后的最终地址 */
};

/*
 * 落盘回调：offset 是文件绝对偏移，返回 0 成功，-1 失败。
 * progress 回调返回非 0 表示要求中止本次传输。
 */
struct edwl_http_sink {
	int (*write)(void *ctx, const void *data, size_t len, uint64_t offset);
	int (*progress)(void *ctx, uint64_t got);
	void *ctx;
};

/* 进程级初始化，必须在任何线程之前调用一次。0 成功 */
int edwl_http_global_init(void);
void edwl_http_global_fini(void);

/* libcurl 版本串，仅供 --version 展示 */
const char *edwl_http_version(void);

/* 取错误描述（最近一次操作） */
const char *edwl_http_error(struct edwl_http *h);

/* 0 成功，-1 失败 */
int edwl_http_open(struct edwl_http **out, const struct edwl_http_opts *opts);

/*
 * worker 线程内的连接复用：首次调用创建，之后复用同一个 easy handle
 * （curl 会保持 TCP/TLS 连接与 DNS 缓存，省掉每片一次握手）。
 * 拿到的句柄不要 edwl_http_close，线程退出时调 edwl_http_thread_release()。
 */
int edwl_http_thread_get(const struct edwl_http_opts *opts,
			 struct edwl_http **out);
void edwl_http_thread_release(void);
/* 本次传输出错时丢掉线程句柄，下一次强制新建连接（避免复用坏连接） */
void edwl_http_thread_drop(void);
void edwl_http_close(struct edwl_http *h);

/*
 * 探测：用 Range: bytes=0-0 发一个 GET，拿状态码、总长、是否支持 Range、
 * ETag/Last-Modified。服务器回 200 说明不支持 Range（但总长仍可取）。
 */
int edwl_http_probe(struct edwl_http *h, const char *url,
		    struct edwl_http_info *info);

/*
 * 拉取 [begin, end] 闭区间，逐块交给 sink->write，offset 从 begin 起递增。
 * got 返回本次实际写入的字节数。
 * 成功 0；服务器回 200（无视 Range）返回 EDWL_HTTP_ERR_RANGE_IGNORED；
 * 提前断开返回 EDWL_HTTP_ERR_SHORT；其余见返回码表。
 */
int edwl_http_get_range(struct edwl_http *h, const char *url, uint64_t begin,
			uint64_t end, const struct edwl_http_sink *sink,
			uint64_t *got);

/* 整文件下载（服务器不支持 Range 或用户禁用分片时的回退路径） */
int edwl_http_get_all(struct edwl_http *h, const char *url,
		      const struct edwl_http_sink *sink, uint64_t *got);

#endif /* EDWL_CORE_HTTP_CLIENT_H */
