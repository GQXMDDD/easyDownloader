#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET test_socket;
typedef int test_socklen;
#define BAD_SOCKET INVALID_SOCKET
#define close_socket closesocket
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>
typedef int test_socket;
typedef socklen_t test_socklen;
#define BAD_SOCKET (-1)
#define close_socket close
#endif

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include <edwl/core/blacklist.h>
#include <edwl/core/checksum.h>
#include <edwl/core/downloader.h>
#include <edwl/core/http_client.h>
#include <edwl/edwl.h>
#include <edwl/platform/platform.h>

#include "test_util.h"

static unsigned char payload[2 * 1024 * 1024];

struct test_server {
	test_socket listener;
	struct edwl_thread *thread;
	struct edwl_mutex *mutex;
	struct edwl_cond *cond;
	atomic_int stop;
	unsigned port;
	unsigned redirects;
	unsigned expired_requests;
	int expire_link;
	int failed;
};

static int send_all(test_socket socket, const void *data, size_t len)
{
	const char *p = data;

	while (len > 0) {
#ifdef MSG_NOSIGNAL
		int n = (int)send(socket, p, len, MSG_NOSIGNAL);
#else
		int n = send(socket, p, (int)len, 0);
#endif
		if (n <= 0)
			return -1;
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

static void serve_request(struct test_server *server, test_socket socket)
{
	char request[4096];
	char path[128];
	char header[512];
	const char *range;
	size_t used = 0;
	size_t begin = 0;
	size_t end = sizeof(payload) - 1;
	unsigned long long b = 0;
	unsigned long long e = 0;
	int ranged = 0;
	int n;

	while (used + 1 < sizeof(request)) {
		n = recv(socket, request + used,
			 (int)(sizeof(request) - used - 1), 0);
		if (n <= 0)
			return;
		used += (size_t)n;
		request[used] = '\0';
		if (strstr(request, "\r\n\r\n"))
			break;
	}
	if (sscanf(request, "GET %127s", path) != 1) {
		server->failed = 1;
		return;
	}
	range = strstr(request, "\r\nRange: bytes=");
	if (range && sscanf(range + 15, "%llu-%llu", &b, &e) == 2 && b <= e &&
	    e < sizeof(payload)) {
		begin = (size_t)b;
		end = (size_t)e;
		ranged = 1;
	}

	if (strcmp(path, "/redirect") == 0) {
		const char *target;

		server->redirects++;
		target = server->expire_link && server->redirects == 1
			     ? "/expired"
			     : "/file.bin";
		n = snprintf(
		    header, sizeof(header),
		    "HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:%u%s\r\n"
		    "Content-Length: 0\r\nETag: wrong-redirect-tag\r\n"
		    "Connection: close\r\n\r\n",
		    server->port, target);
		(void)send_all(socket, header, (size_t)n);
		return;
	}
	if (strcmp(path, "/expired") == 0)
		server->expired_requests++;
	if (strcmp(path, "/denied") == 0 ||
	    (strcmp(path, "/expired") == 0 && !(ranged && b == 0 && e == 0))) {
		static const char error[] =
		    "HTTP/1.1 403 Forbidden\r\nContent-Length: 9\r\n"
		    "Connection: close\r\n\r\nforbidden";

		(void)send_all(socket, error, sizeof(error) - 1);
		return;
	}
	if (strcmp(path, "/slow") == 0 || strcmp(path, "/ignore") == 0) {
		ranged = 0;
		begin = 0;
		end = sizeof(payload) - 1;
	}
	if (ranged) {
		n = snprintf(
		    header, sizeof(header),
		    "HTTP/1.1 206 Partial Content\r\nContent-Length: %zu\r\n"
		    "Content-Range: bytes %zu-%zu/%zu\r\nETag: file-tag\r\n"
		    "Connection: close\r\n\r\n",
		    end - begin + 1, begin, end, sizeof(payload));
	} else {
		n = snprintf(header, sizeof(header),
			     "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n"
			     "ETag: file-tag\r\nConnection: close\r\n\r\n",
			     sizeof(payload));
	}
	if (send_all(socket, header, (size_t)n) != 0)
		return;
	if (strcmp(path, "/slow") == 0) {
		edwl_mutex_lock(server->mutex);
		if (!atomic_load(&server->stop))
			edwl_cond_timedwait(server->cond, server->mutex, 2000);
		edwl_mutex_unlock(server->mutex);
	}
	if (!atomic_load(&server->stop))
		(void)send_all(socket, payload + begin, end - begin + 1);
}

static void *server_main(void *arg)
{
	struct test_server *server = arg;

	while (!atomic_load(&server->stop)) {
		fd_set read_set;
		struct timeval timeout = {0, 100000};
		test_socket client;
		int rc;

		FD_ZERO(&read_set);
		FD_SET(server->listener, &read_set);
		rc = select((int)server->listener + 1, &read_set, NULL, NULL,
			    &timeout);
		if (rc == 0)
			continue;
		if (rc < 0) {
			server->failed = 1;
			break;
		}
		client = accept(server->listener, NULL, NULL);
		if (client == BAD_SOCKET) {
			server->failed = 1;
			break;
		}
		serve_request(server, client);
		close_socket(client);
	}
	return NULL;
}

static int server_start(struct test_server *server, int expire_link)
{
	struct sockaddr_in address;
	test_socklen len = sizeof(address);

	memset(server, 0, sizeof(*server));
	atomic_init(&server->stop, 0);
	server->expire_link = expire_link;
	server->listener = socket(AF_INET, SOCK_STREAM, 0);
	if (server->listener == BAD_SOCKET)
		return -1;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(server->listener, (struct sockaddr *)&address,
		 sizeof(address)) != 0 ||
	    listen(server->listener, 8) != 0 ||
	    getsockname(server->listener, (struct sockaddr *)&address, &len) !=
		0)
		goto fail;
	server->port = ntohs(address.sin_port);
	if (edwl_mutex_init(&server->mutex) != 0 ||
	    edwl_cond_init(&server->cond) != 0 ||
	    edwl_thread_create(&server->thread, server_main, server) != 0)
		goto fail;
	return 0;

fail:
	edwl_cond_destroy(server->cond);
	edwl_mutex_destroy(server->mutex);
	close_socket(server->listener);
	return -1;
}

static void server_stop(struct test_server *server)
{
	edwl_mutex_lock(server->mutex);
	atomic_store(&server->stop, 1);
	edwl_cond_signal(server->cond);
	edwl_mutex_unlock(server->mutex);
	CHECK_INT(edwl_thread_join(server->thread), 0);
	close_socket(server->listener);
	edwl_cond_destroy(server->cond);
	edwl_mutex_destroy(server->mutex);
	CHECK_INT(server->failed, 0);
}

static void make_url(struct test_server *server, const char *path, char *url,
		     size_t size)
{
	snprintf(url, size, "http://127.0.0.1:%u%s", server->port, path);
}

static void test_probe_stops_before_body(void)
{
	struct test_server server;
	struct edwl_http *http = NULL;
	struct edwl_http_opts opts = {0};
	struct edwl_http_info info;
	char url[256];
	uint64_t started;
	int rc = server_start(&server, 0);

	CHECK_INT(rc, 0);
	if (rc != 0)
		return;
	opts.proxy = "";
	opts.connect_timeout_ms = 2000;
	CHECK_INT(edwl_http_open(&http, &opts), 0);
	make_url(&server, "/slow", url, sizeof(url));
	started = edwl_now_ms();
	CHECK_INT(edwl_http_probe(http, url, &info), EDWL_HTTP_OK);
	CHECK(edwl_now_ms() - started < 1000);
	CHECK_INT(info.status, 200);
	CHECK_INT(info.content_length, sizeof(payload));
	CHECK_INT(info.range_supported, 0);
	edwl_http_close(http);
	server_stop(&server);
}

struct memory_sink {
	uint64_t bytes;
};

static int count_write(void *ctx, const void *data, size_t len, uint64_t offset)
{
	struct memory_sink *sink = ctx;

	if (offset > sizeof(payload) || len > sizeof(payload) - offset) {
		CHECK(0);
		return -1;
	}
	CHECK(memcmp(data, payload + offset, len) == 0);
	sink->bytes += len;
	return 0;
}

static void test_range_and_errors(void)
{
	struct test_server server;
	struct edwl_http *http = NULL;
	struct edwl_http_opts opts = {0};
	struct edwl_http_info info;
	struct memory_sink memory = {0};
	struct edwl_http_sink sink = {count_write, NULL, &memory};
	char url[256];
	char final_url[256];
	uint64_t got = 0;
	int rc = server_start(&server, 0);

	CHECK_INT(rc, 0);
	if (rc != 0)
		return;
	opts.proxy = "";
	opts.connect_timeout_ms = 2000;
	CHECK_INT(edwl_http_open(&http, &opts), 0);
	make_url(&server, "/redirect", url, sizeof(url));
	make_url(&server, "/file.bin", final_url, sizeof(final_url));
	CHECK_INT(edwl_http_probe(http, url, &info), EDWL_HTTP_OK);
	CHECK_INT(info.status, 206);
	CHECK_INT(info.content_length, sizeof(payload));
	CHECK_INT(info.range_supported, 1);
	CHECK_STR(info.etag, "file-tag");
	CHECK_STR(info.effective_url, final_url);
	CHECK_INT(edwl_http_get_range(http, final_url, 100, 199, &sink, &got),
		  EDWL_HTTP_OK);
	CHECK_INT(got, 100);
	CHECK_INT(memory.bytes, 100);
	make_url(&server, "/ignore", url, sizeof(url));
	memory.bytes = 0;
	CHECK_INT(edwl_http_get_range(http, url, 100, 199, &sink, &got),
		  EDWL_HTTP_ERR_RANGE_IGNORED);
	CHECK_INT(memory.bytes, 0);
	make_url(&server, "/denied", url, sizeof(url));
	CHECK_INT(edwl_http_get_range(http, url, 0, 99, &sink, &got),
		  EDWL_HTTP_ERR);
	CHECK_INT(memory.bytes, 0);
	CHECK_STR(edwl_http_error(http), "HTTP 403");
	CHECK_INT(edwl_http_probe(http, url, &info), EDWL_HTTP_ERR);
	CHECK_INT(info.status, 403);
	CHECK_INT(edwl_http_get_all(http, url, &sink, &got), EDWL_HTTP_ERR);
	CHECK_INT(memory.bytes, 0);
	edwl_http_close(http);
	server_stop(&server);
}

static void test_download_redirect(int expire_link, int use_range,
				   int wrong_hash)
{
	static const char output[] = "edwl_test_http.tmp";
	struct test_server server;
	struct edwl_download_opts opts;
	struct edwl_download_result result;
	struct edwl_blacklist *blacklist = NULL;
	char url[256];
	const char *urls[] = {url};
	char expected[EDWL_SHA256_HEX];
	char required[EDWL_SHA256_HEX];
	char actual[EDWL_SHA256_HEX];
	int rc = server_start(&server, expire_link);

	CHECK_INT(rc, 0);
	if (rc != 0)
		return;
	make_url(&server, "/redirect", url, sizeof(url));
	edwl_sha256_buf(payload, sizeof(payload), expected);
	memcpy(required, expected, sizeof(required));
	if (wrong_hash)
		required[0] = required[0] == '0' ? '1' : '0';
	edwl_download_opts_default(&opts);
	opts.urls = urls;
	opts.url_count = 1;
	opts.output = output;
	opts.proxy = "";
	opts.threads = 4;
	opts.min_piece_size = 64 * 1024;
	opts.max_piece_size = 256 * 1024;
	opts.expected_sha256 = required;
	opts.allow_range = use_range;
	opts.quiet = 1;
	opts.show_progress = 0;
	opts.allow_resume = 0;
	CHECK_INT(edwl_blacklist_create("blocked.invalid", "", &blacklist), 0);
	opts.blacklist = blacklist;
	remove(output);
	CHECK_INT(edwl_download_run(&opts, &result),
		  wrong_hash ? EDWL_EXIT_CHECKSUM : EDWL_EXIT_OK);
	CHECK_INT(result.bytes, sizeof(payload));
	CHECK_INT(result.range_used, use_range);
	CHECK_INT(result.verified, wrong_hash ? -1 : 1);
	CHECK_INT(edwl_sha256_file(output, actual), 0);
	CHECK_STR(actual, expected);
	edwl_blacklist_destroy(blacklist);
	server_stop(&server);
	if (expire_link) {
		CHECK(server.redirects >= 2);
		CHECK(server.expired_requests >= 2);
	} else {
		CHECK_INT(server.redirects, 1);
	}
	remove(output);
	remove("edwl_test_http.tmp.edwl");
}

int main(void)
{
	size_t i;
#ifdef _WIN32
	WSADATA data;

	if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
		return 1;
#endif
	for (i = 0; i < sizeof(payload); i++)
		payload[i] = (unsigned char)(i % 251);
	if (edwl_http_global_init() != 0)
		return 1;
	test_probe_stops_before_body();
	test_range_and_errors();
	edwl_http_global_fini();
	test_download_redirect(0, 1, 0);
	test_download_redirect(1, 1, 0);
	test_download_redirect(1, 0, 0);
	test_download_redirect(0, 0, 1);
#ifdef _WIN32
	WSACleanup();
#endif
	return test_finish("test_http");
}
