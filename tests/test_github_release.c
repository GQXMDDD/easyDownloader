#include <stdio.h>
#include <string.h>

#include <edwl/core/github_release.h>
#include <edwl/core/http_client.h>
#include <edwl/util/buffer.h>

#include "test_util.h"

#define HASH "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
#define ASSET(name, digest)                                                    \
	"{\"name\":\"" name "\",\"browser_download_url\":"                     \
	"\"https://github.com/owner/repo/releases/download/v1/" name "\","     \
	"\"digest\":" digest ",\"state\":\"uploaded\"}"

struct edwl_http {
	int unused;
};

static const char *mock_json;
static const char *mock_checksums;
static const char *mock_other_checksums;
static unsigned mock_requests;

int edwl_http_get_text(struct edwl_http *http, const char *url,
		       const char *accept, size_t max_size,
		       struct edwl_buf *buf)
{
	const char *response;

	(void)http;
	(void)accept;
	mock_requests++;
	if (strcmp(url,
		   "https://api.github.com/repos/owner/repo/releases/latest") ==
	    0) {
		response = mock_json;
	} else {
		CHECK(strstr(url, "https://github.com/owner/repo/releases/"
				  "download/v1/") == url);
		response = mock_checksums;
		if (mock_other_checksums && strstr(url, ".sha256.txt"))
			response = mock_other_checksums;
	}
	if (!response)
		return EDWL_HTTP_ERR;
	CHECK(strlen(response) <= max_size);
	edwl_buf_reset(buf);
	return edwl_buf_append_str(buf, response) == 0 ? EDWL_HTTP_OK
						       : EDWL_HTTP_ERR;
}

const char *edwl_http_error(struct edwl_http *http)
{
	(void)http;
	return "mock metadata request failed";
}

static const char release_json[] =
    "{\"tag_name\":\"v1.0\",\"draft\":false,\"prerelease\":false,"
    "\"author\":{\"name\":\"nested name\",\"other\":[1,true,null]},"
    "\"body\":\"ignored \\\"name\\\": and Unicode "
    "\\u4e2d\\u6587\",\"assets\":[" ASSET(
	"app-linux-amd64.tar.gz",
	"null") "," ASSET("app-windows-arm64.zip",
			  "null") "," ASSET("app-windows-amd64.zip",
					    "\"sha256:" HASH
					    "\"") "," ASSET("SHA256SUMS",
							    "null") "]}";

static void test_asset_selection(void)
{
	struct edwl_github_release release;
	char error[256];

	CHECK_INT(edwl_github_select_release(release_json, NULL, "windows",
					     "x64", &release, error,
					     sizeof(error)),
		  0);
	CHECK_STR(release.tag, "v1.0");
	CHECK_STR(release.asset, "app-windows-amd64.zip");
	CHECK_STR(release.sha256, HASH);
	CHECK_INT(edwl_github_select_release(release_json, NULL, "linux", "x64",
					     &release, error, sizeof(error)),
		  0);
	CHECK_STR(release.asset, "app-linux-amd64.tar.gz");
	CHECK_STR(release.sha256, "");
	CHECK_INT(edwl_github_select_release(
		      release_json, "app-windows-arm64.zip", "windows", "x64",
		      &release, error, sizeof(error)),
		  0);
	CHECK_STR(release.asset, "app-windows-arm64.zip");
	CHECK_INT(edwl_github_select_release(release_json, "missing.zip",
					     "windows", "x64", &release, error,
					     sizeof(error)),
		  -1);
}

static void test_ambiguous_and_invalid(void)
{
	static const char ambiguous[] =
	    "{\"tag_name\":\"v1\",\"draft\":false,\"prerelease\":false,"
	    "\"assets\":[" ASSET("first-windows-x64.zip", "null") "," ASSET(
		"second-windows-x64.zip", "null") "]}";
	static const char *invalid[] = {
	    "{",
	    "{\"assets\":[}",
	    "{}",
	    "[]",
	    "{\"tag_name\":\"v1\",\"tag_name\":\"v2\",\"draft\":false,"
	    "\"prerelease\":false,\"assets\":[]}",
	    "{\"tag_name\":\"v1\",\"draft\":true,\"prerelease\":false,"
	    "\"assets\":[]}",
	    "{\"tag_name\":\"v1\",\"draft\":false,\"prerelease\":true,"
	    "\"assets\":[]}",
	    "{\"tag_name\":\"v1\",\"draft\":false,\"prerelease\":false,"
	    "\"assets\":[" ASSET("app.exe", "\"sha256:123\"") "]}",
	    "{\"tag_name\":\"v1\",\"draft\":false,\"prerelease\":false,"
	    "\"assets\":["
	    "{\"name\":\"app.exe\",\"browser_download_url\":"
	    "\"http://github.com/app.exe\"}]}",
	};
	struct edwl_github_release release;
	char error[256];
	size_t i;

	CHECK_INT(edwl_github_select_release(ambiguous, NULL, "windows", "x64",
					     &release, error, sizeof(error)),
		  -1);
	CHECK(strstr(error, "ambiguous") != NULL);
	for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
		CHECK_INT(edwl_github_select_release(invalid[i], NULL,
						     "windows", "x64", &release,
						     error, sizeof(error)),
			  -1);
}

static void test_checksum_discovery(void)
{
	static const char with_digest[] =
	    "{\"tag_name\":\"v1\",\"draft\":false,\"prerelease\":false,"
	    "\"assets\":[" ASSET("app.exe", "\"sha256:" HASH "\"") "]}";
	static const char with_sidecar[] =
	    "{\"tag_name\":\"v1\",\"draft\":false,\"prerelease\":false,"
	    "\"assets\":[" ASSET("app.exe", "null") "," ASSET("app.exe.sha256",
							      "null") "]}";
	static const char with_manifest[] =
	    "{\"tag_name\":\"v1\",\"draft\":false,\"prerelease\":false,"
	    "\"assets\":[" ASSET("app.exe", "null") "," ASSET("checksums.txt",
							      "null") "]}";
	static const char with_notes[] =
	    "{\"tag_name\":\"v1\",\"draft\":false,\"prerelease\":false,"
	    "\"assets\":[" ASSET("app.exe",
				 "null") "],\"body\":\"app.exe  SHA-256: " HASH
					 "\\n\"}";
	static const char no_hash[] =
	    "{\"tag_name\":\"v1\",\"draft\":false,\"prerelease\":false,"
	    "\"assets\":[" ASSET("app.exe", "null") "]}";
	static const char two_sidecars[] =
	    "{\"tag_name\":\"v1\",\"draft\":false,\"prerelease\":false,"
	    "\"assets\":[" ASSET("app.exe", "null") "," ASSET(
		"app.exe.sha256", "null") "," ASSET("app.exe.sha256.txt",
						    "null") "]}";
	static const char different[] =
	    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
	struct edwl_http http = {0};
	struct edwl_github_release release;
	char error[256];

	mock_json = with_digest;
	mock_requests = 0;
	CHECK_INT(edwl_github_resolve_release(&http, "owner/repo.git",
					      "app.exe", NULL, &release, error,
					      sizeof(error)),
		  0);
	CHECK_STR(release.sha256, HASH);
	CHECK_INT(mock_requests, 1);
	mock_json = with_sidecar;
	mock_checksums = HASH "\n";
	mock_requests = 0;
	CHECK_INT(edwl_github_resolve_release(&http, "owner/repo", "app.exe",
					      NULL, &release, error,
					      sizeof(error)),
		  0);
	CHECK_STR(release.sha256, HASH);
	CHECK_INT(mock_requests, 2);
	mock_json = with_manifest;
	mock_checksums = HASH "  other.exe\n" HASH " *app.exe\n";
	CHECK_INT(edwl_github_resolve_release(&http, "owner/repo", "app.exe",
					      NULL, &release, error,
					      sizeof(error)),
		  0);
	CHECK_STR(release.sha256, HASH);
	mock_checksums = HASH "  other.exe\n";
	CHECK_INT(edwl_github_resolve_release(&http, "owner/repo", "app.exe",
					      NULL, &release, error,
					      sizeof(error)),
		  -1);
	mock_json = with_notes;
	CHECK_INT(edwl_github_resolve_release(&http, "owner/repo", "app.exe",
					      NULL, &release, error,
					      sizeof(error)),
		  0);
	CHECK_STR(release.sha256, HASH);
	mock_json = no_hash;
	CHECK_INT(edwl_github_resolve_release(&http, "owner/repo", "app.exe",
					      NULL, &release, error,
					      sizeof(error)),
		  -1);
	CHECK(strstr(error, "no SHA-256") != NULL);
	CHECK_INT(edwl_github_resolve_release(&http, "owner/repo", "app.exe",
					      different, &release, error,
					      sizeof(error)),
		  0);
	CHECK_STR(release.sha256, different);
	mock_json = two_sidecars;
	mock_checksums = HASH "\n";
	mock_other_checksums = different;
	CHECK_INT(edwl_github_resolve_release(&http, "owner/repo", "app.exe",
					      NULL, &release, error,
					      sizeof(error)),
		  -1);
	CHECK(strstr(error, "across") != NULL);
	mock_other_checksums = NULL;
	CHECK_INT(edwl_github_resolve_release(&http, "https://example.com/file",
					      NULL, NULL, &release, error,
					      sizeof(error)),
		  -1);
}

int main(void)
{
	test_asset_selection();
	test_ambiguous_and_invalid();
	test_checksum_discovery();
	return test_finish("test_github_release");
}
