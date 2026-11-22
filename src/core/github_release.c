#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <edwl/core/github_release.h>
#include <edwl/core/http_client.h>
#include <edwl/util/buffer.h>
#include <edwl/util/log.h>

#define RELEASE_TEXT_LIMIT (4 * 1024 * 1024)
#define RELEASE_ASSET_LIMIT 512

struct json_value {
	const char *begin;
	const char *end;
	char type;
};

struct release_asset {
	char name[256];
	char url[EDWL_URL_MAX];
	char sha256[EDWL_SHA256_HEX];
};

struct release_data {
	char tag[128];
	char *body;
	struct release_asset *assets;
	size_t count;
};

static int fail(char *error, size_t size, const char *message)
{
	if (error && size)
		snprintf(error, size, "%s", message);
	return -1;
}

static void json_space(const char **p)
{
	while (**p == ' ' || **p == '\t' || **p == '\r' || **p == '\n')
		(*p)++;
}

static int json_scan(const char **cursor, unsigned depth,
		     struct json_value *out);

/* 跳过 \uXXXX 的四个十六进制位；格式不对返回 NULL */
static const char *json_skip_u4(const char *p)
{
	unsigned i;

	for (i = 0; i < 4; i++) {
		if (!isxdigit((unsigned char)*p))
			return NULL;
		p++;
	}
	return p;
}

/* object 的键：字符串 + 冒号 */
static int json_key(const char **pp, unsigned depth)
{
	struct json_value child;

	if (json_scan(pp, depth, &child) != 0 || child.type != '"')
		return -1;
	json_space(pp);
	if (*(*pp)++ != ':')
		return -1;
	return 0;
}

static int json_scan(const char **cursor, unsigned depth,
		     struct json_value *out)
{
	const char *p = *cursor;
	char close;

	json_space(&p);
	if (depth > 32 || !*p)
		return -1;
	out->begin = p;
	out->type = *p;
	if (*p == '"') {
		p++;
		while (*p && *p != '"') {
			if ((unsigned char)*p < 32)
				return -1;
			if (*p++ != '\\')
				continue;
			if (*p == 'u') {
				p = json_skip_u4(p + 1);
				if (!p)
					return -1;
			} else {
				if (!*p || !strchr("\"\\/bfnrt", *p))
					return -1;
				p++;
			}
		}
		if (*p++ != '"')
			return -1;
	} else if (*p == '{' || *p == '[') {
		int object = *p == '{';

		close = object ? '}' : ']';
		p++;
		json_space(&p);
		if (*p != close) {
			for (;;) {
				struct json_value child;

				if (object && json_key(&p, depth + 1) != 0)
					return -1;
				if (json_scan(&p, depth + 1, &child) != 0)
					return -1;
				json_space(&p);
				if (*p == close)
					break;
				if (*p++ != ',')
					return -1;
			}
		}
		p++;
	} else if (*p == 't' || *p == 'f' || *p == 'n') {
		const char *word = *p == 't'   ? "true"
				   : *p == 'f' ? "false"
					       : "null";
		size_t len = strlen(word);

		if (strncmp(p, word, len) != 0)
			return -1;
		p += len;
	} else {
		if (*p == '-')
			p++;
		if (*p == '0') {
			p++;
		} else {
			if (*p < '1' || *p > '9')
				return -1;
			while (isdigit((unsigned char)*p))
				p++;
		}
		if (*p == '.') {
			p++;
			if (!isdigit((unsigned char)*p))
				return -1;
			while (isdigit((unsigned char)*p))
				p++;
		}
		if (*p == 'e' || *p == 'E') {
			p++;
			if (*p == '+' || *p == '-')
				p++;
			if (!isdigit((unsigned char)*p))
				return -1;
			while (isdigit((unsigned char)*p))
				p++;
		}
	}
	out->end = p;
	*cursor = p;
	return 0;
}

static unsigned json_hex4(const char *p)
{
	unsigned value = 0;
	unsigned i;

	for (i = 0; i < 4; i++) {
		unsigned char c = (unsigned char)p[i];

		value =
		    value * 16 + (c <= '9' ? (unsigned)(c - '0')
					   : (unsigned)tolower(c) - 'a' + 10);
	}
	return value;
}

/* 解一个 \uXXXX（含代理对）；成功返回码点并前移指针，失败返回 -1 */
static long json_unicode_escape(const char **pp, const char *end)
{
	const char *p = *pp;
	unsigned code;
	unsigned low;

	code = json_hex4(p);
	p += 4;
	if (code >= 0xd800 && code <= 0xdbff) {
		if (end - p < 6 || p[0] != '\\' || p[1] != 'u')
			return -1;
		low = json_hex4(p + 2);
		if (low < 0xdc00 || low > 0xdfff)
			return -1;
		code = 0x10000 + (code - 0xd800) * 1024 + low - 0xdc00;
		p += 6;
	} else if (code >= 0xdc00 && code <= 0xdfff) {
		return -1;
	}
	*pp = p;
	return (long)code;
}

/* 把码点写成 UTF-8，返回字节数 */
static size_t utf8_encode(unsigned code, char out[4])
{
	if (code >= 0x10000) {
		out[0] = (char)(0xf0 | (code >> 18));
		out[1] = (char)(0x80 | ((code >> 12) & 63));
		out[2] = (char)(0x80 | ((code >> 6) & 63));
		out[3] = (char)(0x80 | (code & 63));
		return 4;
	}
	if (code >= 0x800) {
		out[0] = (char)(0xe0 | (code >> 12));
		out[1] = (char)(0x80 | ((code >> 6) & 63));
		out[2] = (char)(0x80 | (code & 63));
		return 3;
	}
	if (code >= 0x80) {
		out[0] = (char)(0xc0 | (code >> 6));
		out[1] = (char)(0x80 | (code & 63));
		return 2;
	}
	out[0] = (char)code;
	return 1;
}

/* 解一个反斜杠转义（调用时已吃掉反斜杠）；失败返回 0 */
static size_t decode_escape(const char **pp, const char *end, char out[4])
{
	unsigned code = (unsigned char)*(*pp)++;

	if (code != 'u') {
		if (code == 'n')
			code = '\n';
		else if (code == 'r')
			code = '\r';
		else if (code == 't')
			code = '\t';
		else if (code == 'b')
			code = '\b';
		else if (code == 'f')
			code = '\f';
		out[0] = (char)code;
		return 1;
	}
	{
		long cp = json_unicode_escape(pp, end);

		if (cp <= 0)
			return 0;
		return utf8_encode((unsigned)cp, out);
	}
}

static int json_string(struct json_value value, char *out, size_t size)
{
	const char *p = value.begin + 1;
	const char *end = value.end - 1;
	size_t used = 0;

	if (value.type != '"' || size == 0)
		return -1;
	while (p < end) {
		unsigned code = (unsigned char)*p++;
		char bytes[4];
		size_t n = 1;

		if (code == '\\') {
			n = decode_escape(&p, end, bytes);
			if (n == 0)
				return -1;
		} else {
			bytes[0] = (char)code;
		}
		if (n >= size - used)
			return -1;
		memcpy(out + used, bytes, n);
		used += n;
	}
	out[used] = '\0';
	return 0;
}

static int json_member(struct json_value object, const char *key,
		       struct json_value *out)
{
	const char *p = object.begin + 1;
	int found = 0;

	if (object.type != '{')
		return -1;
	json_space(&p);
	while (*p != '}') {
		struct json_value name;
		struct json_value value;
		char decoded[128];

		if (json_scan(&p, 0, &name) != 0 ||
		    json_string(name, decoded, sizeof(decoded)) != 0)
			return -1;
		json_space(&p);
		if (*p++ != ':' || json_scan(&p, 0, &value) != 0)
			return -1;
		if (strcmp(decoded, key) == 0) {
			if (found)
				return -1;
			*out = value;
			found = 1;
		}
		json_space(&p);
		if (*p == ',')
			p++;
	}
	return found;
}

static int json_field(struct json_value object, const char *key, char *out,
		      size_t size, int optional)
{
	struct json_value value;
	int found = json_member(object, key, &value);

	if (found < 0)
		return -1;
	if (found == 0 || value.type == 'n') {
		if (!optional)
			return -1;
		out[0] = '\0';
		return 0;
	}
	return json_string(value, out, size);
}

static void release_free(struct release_data *data)
{
	free(data->body);
	free(data->assets);
}

static int release_parse(const char *json, struct release_data *data,
			 char *error, size_t size)
{
	const char *p = json;
	struct json_value root;
	struct json_value array;
	struct json_value body;
	struct json_value flag;
	size_t scanned = 0;

	memset(data, 0, sizeof(*data));
	if (!json || json_scan(&p, 0, &root) != 0 || root.type != '{')
		return fail(error, size, "invalid GitHub Release JSON");
	json_space(&p);
	if (*p ||
	    json_field(root, "tag_name", data->tag, sizeof(data->tag), 0) !=
		0 ||
	    !data->tag[0] || json_member(root, "assets", &array) != 1 ||
	    array.type != '[')
		return fail(error, size,
			    "Release has no valid tag or asset list");
	if (json_member(root, "draft", &flag) != 1 || flag.type != 'f' ||
	    json_member(root, "prerelease", &flag) != 1 || flag.type != 'f')
		return fail(
		    error, size,
		    "latest Release must be published and non-prerelease");
	data->assets = calloc(RELEASE_ASSET_LIMIT, sizeof(*data->assets));
	if (!data->assets)
		return fail(error, size, "cannot allocate Release assets");
	p = array.begin + 1;
	json_space(&p);
	while (*p != ']') {
		struct json_value value;
		struct release_asset *asset;
		struct edwl_url parsed;
		char digest[128];
		char state[32];

		if (++scanned > RELEASE_ASSET_LIMIT ||
		    json_scan(&p, 0, &value) != 0)
			return fail(error, size,
				    "invalid or oversized Release asset list");
		asset = &data->assets[data->count];
		memset(asset, 0, sizeof(*asset));
		if (json_field(value, "name", asset->name, sizeof(asset->name),
			       0) != 0 ||
		    json_field(value, "browser_download_url", asset->url,
			       sizeof(asset->url), 0) != 0 ||
		    json_field(value, "digest", digest, sizeof(digest), 1) !=
			0 ||
		    json_field(value, "state", state, sizeof(state), 1) != 0 ||
		    !asset->name[0] ||
		    edwl_url_parse(asset->url, &parsed) != 0 ||
		    strcmp(parsed.scheme, "https") != 0 ||
		    strcmp(parsed.host, "github.com") != 0)
			return fail(error, size,
				    "invalid Release asset fields or "
				    "non-GitHub HTTPS URL");
		if (strncmp(digest, "sha256:", 7) == 0) {
			if (!edwl_hash_is_valid(digest + 7))
				return fail(
				    error, size,
				    "invalid SHA-256 digest in Release asset");
			memcpy(asset->sha256, digest + 7, EDWL_SHA256_HEX);
		}
		if (!state[0] || strcmp(state, "uploaded") == 0)
			data->count++;
		json_space(&p);
		if (*p == ',') {
			p++;
			json_space(&p);
		}
	}
	{
		int found = json_member(root, "body", &body);

		if (found < 0)
			return fail(error, size,
				    "invalid or duplicate Release body");
		if (found == 0 || body.type == 'n')
			return 0;
	}
	{
		size_t length = (size_t)(body.end - body.begin) + 1;

		data->body = malloc(length);
		if (!data->body || json_string(body, data->body, length) != 0)
			return fail(
			    error, size,
			    "invalid Release notes or allocation failure");
	}
	return 0;
}

static int has_word(const char *name, const char *word)
{
	const char *p = name;
	size_t len = strlen(word);

	while ((p = strstr(p, word)) != NULL) {
		if ((p == name || !isalnum((unsigned char)p[-1])) &&
		    !isalnum((unsigned char)p[len]))
			return 1;
		p++;
	}
	return 0;
}

static int ends_with(const char *name, const char *suffix)
{
	size_t len = strlen(name);
	size_t suffix_len = strlen(suffix);

	return len >= suffix_len &&
	       strcmp(name + len - suffix_len, suffix) == 0;
}

static void lower_name(const char *name, char out[256])
{
	size_t i;

	for (i = 0; name[i] && i < 255; i++)
		out[i] = (char)tolower((unsigned char)name[i]);
	out[i] = '\0';
}

/* 合并新发现的哈希；与已有值冲突返回 -1 */
static int merge_discovered(char *discovered, const char *hash)
{
	if (discovered[0] && !edwl_hash_equals(discovered, hash))
		return -1;
	memcpy(discovered, hash, EDWL_SHA256_HEX);
	return 0;
}

static int checksum_kind(const char *name, const char *target)
{
	char lower[256];
	size_t len = strlen(target);

	lower_name(name, lower);
	if (strncmp(name, target, len) == 0 &&
	    (strcmp(lower + len, ".sha256") == 0 ||
	     strcmp(lower + len, ".sha256sum") == 0 ||
	     strcmp(lower + len, ".sha256.txt") == 0))
		return 1;
	if ((strstr(lower, "checksum") || strstr(lower, "sha256sum") ||
	     strcmp(lower, "sha256sum.txt") == 0 ||
	     strcmp(lower, "sha256.txt") == 0) &&
	    !ends_with(lower, ".sig") && !ends_with(lower, ".asc") &&
	    !ends_with(lower, ".minisig"))
		return 2;
	return 0;
}

static int asset_score(const char *name, const char *os, const char *arch)
{
	char lower[256];
	const char *asset_os = NULL;
	const char *asset_arch = NULL;
	int score = 0;

	lower_name(name, lower);
	if (strstr(lower, "sha256") || strstr(lower, "checksum") ||
	    has_word(lower, "source") || has_word(lower, "src") ||
	    has_word(lower, "symbols") || has_word(lower, "debug") ||
	    ends_with(lower, ".sig") || ends_with(lower, ".asc") ||
	    ends_with(lower, ".minisig") || ends_with(lower, ".txt"))
		return -1;
	if (has_word(lower, "windows") || has_word(lower, "win") ||
	    has_word(lower, "win32") || has_word(lower, "win64") ||
	    has_word(lower, "msvc") || has_word(lower, "mingw") ||
	    ends_with(lower, ".exe") || ends_with(lower, ".msi"))
		asset_os = "windows";
	else if (has_word(lower, "linux") || ends_with(lower, ".appimage") ||
		 ends_with(lower, ".deb") || ends_with(lower, ".rpm"))
		asset_os = "linux";
	else if (has_word(lower, "darwin") || has_word(lower, "macos") ||
		 has_word(lower, "mac") || has_word(lower, "osx") ||
		 ends_with(lower, ".dmg") || ends_with(lower, ".pkg"))
		asset_os = "macos";
	else if (has_word(lower, "android") || has_word(lower, "ios") ||
		 has_word(lower, "freebsd"))
		return -1;
	if (asset_os && strcmp(asset_os, os) != 0)
		return -1;
	if (!asset_os)
		return -1;
	if (asset_os)
		score += 100;
	if (has_word(lower, "amd64") || has_word(lower, "x86_64") ||
	    has_word(lower, "x86-64") || has_word(lower, "x64") ||
	    has_word(lower, "win64"))
		asset_arch = "x64";
	else if (has_word(lower, "arm64") || has_word(lower, "aarch64"))
		asset_arch = "arm64";
	else if (has_word(lower, "i386") || has_word(lower, "i686") ||
		 has_word(lower, "386") || has_word(lower, "x86") ||
		 has_word(lower, "win32"))
		asset_arch = "x86";
	else if (has_word(lower, "arm") || has_word(lower, "armv7") ||
		 has_word(lower, "armv7l") || has_word(lower, "armhf") ||
		 has_word(lower, "armel"))
		asset_arch = "arm";
	else if (has_word(lower, "ppc64le") || has_word(lower, "s390x") ||
		 has_word(lower, "riscv64") || has_word(lower, "mips64"))
		return -1;
	if (asset_arch && strcmp(asset_arch, arch) != 0)
		return -1;
	if (asset_arch)
		score += 50;
	if (ends_with(lower, ".zip"))
		score += 10;
	else if (ends_with(lower, ".7z"))
		score += 9;
	else if (ends_with(lower, ".exe") || ends_with(lower, ".appimage"))
		score += 8;
	else if (ends_with(lower, ".tar.gz") || ends_with(lower, ".tar.xz"))
		score += 7;
	else if (ends_with(lower, ".msi") || ends_with(lower, ".dmg") ||
		 ends_with(lower, ".pkg") || ends_with(lower, ".deb") ||
		 ends_with(lower, ".rpm"))
		score += 5;
	else
		return -1;
	if (has_word(lower, "portable"))
		score += 1;
	return score;
}

static int release_select(const struct release_data *data, const char *name,
			  const char *os, const char *arch, size_t *index,
			  char *error, size_t size)
{
	size_t i;
	int best = -1;
	int ties = 0;

	for (i = 0; i < data->count; i++) {
		int score =
		    name ? (strcmp(name, data->assets[i].name) == 0 ? 1 : -1)
			 : asset_score(data->assets[i].name, os, arch);

		if (score > best) {
			best = score;
			*index = i;
			ties = 1;
		} else if (score >= 0 && score == best) {
			ties++;
		}
	}
	if (best < 0)
		return fail(
		    error, size,
		    name ? "requested asset not found in latest Release"
			 : "no matching platform asset; specify --asset NAME");
	if (ties > 1) {
		edwl_log_msg(EDWL_LOG_ERROR,
			     "Release 资产无法唯一匹配，请用 --asset 指定：");
		for (i = 0; i < data->count; i++)
			if (asset_score(data->assets[i].name, os, arch) == best)
				edwl_log_msg(EDWL_LOG_ERROR, "  %s",
					     data->assets[i].name);
		return fail(error, size,
			    "ambiguous Release assets; specify --asset NAME");
	}
	return 0;
}

static void release_result(const struct release_data *data, size_t index,
			   struct edwl_github_release *out)
{
	memset(out, 0, sizeof(*out));
	memcpy(out->tag, data->tag, sizeof(out->tag));
	memcpy(out->asset, data->assets[index].name, sizeof(out->asset));
	memcpy(out->url, data->assets[index].url, sizeof(out->url));
	memcpy(out->sha256, data->assets[index].sha256, sizeof(out->sha256));
}

int edwl_github_select_release(const char *json, const char *asset,
			       const char *os, const char *arch,
			       struct edwl_github_release *out, char *error,
			       size_t error_size)
{
	struct release_data data;
	size_t index = 0;
	int rc;

	if (!json || !os || !arch || !out)
		return fail(error, error_size,
			    "invalid Release selection arguments");
	if (strlen(json) > RELEASE_TEXT_LIMIT)
		return fail(error, error_size,
			    "Release JSON exceeds size limit");
	rc = release_parse(json, &data, error, error_size);
	if (rc == 0)
		rc = release_select(&data, asset, os, arch, &index, error,
				    error_size);
	if (rc == 0)
		release_result(&data, index, out);
	release_free(&data);
	return rc;
}

static const char *current_os(void)
{
#ifdef _WIN32
	return "windows";
#elif defined(__APPLE__)
	return "macos";
#elif defined(__linux__)
	return "linux";
#else
	return "unknown";
#endif
}

static const char *current_arch(void)
{
#if defined(__x86_64__) || defined(_M_X64)
	return "x64";
#elif defined(__aarch64__) || defined(_M_ARM64)
	return "arm64";
#elif defined(__i386__) || defined(_M_IX86)
	return "x86";
#elif defined(__arm__) || defined(_M_ARM)
	return "arm";
#else
	return "unknown";
#endif
}

int edwl_github_resolve_release(struct edwl_http *http, const char *repository,
				const char *asset, const char *expected_sha256,
				struct edwl_github_release *out, char *error,
				size_t error_size)
{
	struct edwl_buf text;
	struct release_data data = {0};
	char repo[144];
	char api[256];
	size_t index = 0;
	size_t i;
	char discovered[EDWL_SHA256_HEX] = "";
	int rc = -1;
	int kind;

	if (!http || !out ||
	    edwl_url_github_repo(repository, repo, sizeof(repo)) != 0)
		return fail(
		    error, error_size,
		    "--release requires a GitHub owner/repo repository");
	if (edwl_buf_init(&text, 4096) != 0)
		return fail(error, error_size,
			    "cannot allocate Release metadata buffer");
	snprintf(api, sizeof(api),
		 "https://api.github.com/repos/%s/releases/latest", repo);
	if (edwl_http_get_text(http, api, "application/vnd.github+json",
			       RELEASE_TEXT_LIMIT, &text) != EDWL_HTTP_OK) {
		fail(error, error_size, edwl_http_error(http));
		goto out;
	}
	if (release_parse(text.data, &data, error, error_size) != 0 ||
	    release_select(&data, asset, current_os(), current_arch(), &index,
			   error, error_size) != 0)
		goto out;
	release_result(&data, index, out);
	edwl_log_msg(EDWL_LOG_INFO, "最新 Release: %s，资产: %s", out->tag,
		     out->asset);
	if (expected_sha256) {
		if (!edwl_hash_is_valid(expected_sha256)) {
			fail(error, error_size, "invalid explicit SHA-256");
			goto out;
		}
		memcpy(out->sha256, expected_sha256, EDWL_SHA256_HEX);
		rc = 0;
		goto out;
	}
	if (out->sha256[0]) {
		edwl_log_msg(EDWL_LOG_INFO,
			     "SHA-256 来源: GitHub asset digest");
		rc = 0;
		goto out;
	}
	for (kind = 1; kind <= 2; kind++) {
		for (i = 0; i < data.count; i++) {
			int found;

			if (checksum_kind(data.assets[i].name, out->asset) !=
			    kind)
				continue;
			if (edwl_http_get_text(http, data.assets[i].url,
					       "text/plain", RELEASE_TEXT_LIMIT,
					       &text) != EDWL_HTTP_OK) {
				fail(error, error_size, edwl_http_error(http));
				goto out;
			}
			found = edwl_sha256_manifest(text.data, out->asset,
						     kind == 1, out->sha256);
			if (found < 0) {
				fail(error, error_size,
				     "invalid or conflicting hashes in Release "
				     "checksum manifest");
				goto out;
			}
			if (found != 1)
				continue;
			if (merge_discovered(discovered, out->sha256) != 0) {
				fail(error, error_size,
				     "conflicting hashes across "
				     "Release checksum files");
				goto out;
			}
			edwl_log_msg(EDWL_LOG_INFO, "SHA-256 来源: %s",
				     data.assets[i].name);
		}
		if (discovered[0]) {
			memcpy(out->sha256, discovered, sizeof(discovered));
			rc = 0;
			goto out;
		}
	}
	if (data.body) {
		int found =
		    edwl_sha256_manifest(data.body, out->asset, 0, out->sha256);

		if (found < 0) {
			fail(error, error_size,
			     "invalid or conflicting hashes in Release notes");
			goto out;
		}
		if (found == 1) {
			edwl_log_msg(EDWL_LOG_INFO,
				     "SHA-256 来源: Release 说明");
			rc = 0;
			goto out;
		}
	}
	fail(error, error_size,
	     "no SHA-256 found for selected asset; provide --sha256 HEX");
out:
	release_free(&data);
	edwl_buf_free(&text);
	return rc;
}
