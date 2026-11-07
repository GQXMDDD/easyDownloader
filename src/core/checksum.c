/*
 * edwl - SHA-256 实现
 *
 * 为什么要自己写：项目约定只用 C + libcurl，不额外引 OpenSSL 之类。
 * SHA-256 的轮函数就八十行，自己写比拖一个加密库进来干净得多，
 * 静态链接出来也小。实现按 FIPS 180-4 的标准流程，测试向量见 tests/。
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include <edwl/core/checksum.h>
#include <edwl/platform/platform.h>

#define SHA256_BLOCK 64
#define SHA256_DIGEST 32

struct sha256 {
	uint32_t state[8];
	uint64_t total; /* 已喂入的字节数 */
	unsigned char buf[SHA256_BLOCK];
	size_t fill;
};

static const uint32_t sha256_k[64] = {
    0x428a2f98UL, 0x71374491UL, 0xb5c0fbcfUL, 0xe9b5dba5UL, 0x3956c25bUL,
    0x59f111f1UL, 0x923f82a4UL, 0xab1c5ed5UL, 0xd807aa98UL, 0x12835b01UL,
    0x243185beUL, 0x550c7dc3UL, 0x72be5d74UL, 0x80deb1feUL, 0x9bdc06a7UL,
    0xc19bf174UL, 0xe49b69c1UL, 0xefbe4786UL, 0x0fc19dc6UL, 0x240ca1ccUL,
    0x2de92c6fUL, 0x4a7484aaUL, 0x5cb0a9dcUL, 0x76f988daUL, 0x983e5152UL,
    0xa831c66dUL, 0xb00327c8UL, 0xbf597fc7UL, 0xc6e00bf3UL, 0xd5a79147UL,
    0x06ca6351UL, 0x14292967UL, 0x27b70a85UL, 0x2e1b2138UL, 0x4d2c6dfcUL,
    0x53380d13UL, 0x650a7354UL, 0x766a0abbUL, 0x81c2c92eUL, 0x92722c85UL,
    0xa2bfe8a1UL, 0xa81a664bUL, 0xc24b8b70UL, 0xc76c51a3UL, 0xd192e819UL,
    0xd6990624UL, 0xf40e3585UL, 0x106aa070UL, 0x19a4c116UL, 0x1e376c08UL,
    0x2748774cUL, 0x34b0bcb5UL, 0x391c0cb3UL, 0x4ed8aa4aUL, 0x5b9cca4fUL,
    0x682e6ff3UL, 0x748f82eeUL, 0x78a5636fUL, 0x84c87814UL, 0x8cc70208UL,
    0x90befffaUL, 0xa4506cebUL, 0xbef9a3f7UL, 0xc67178f2UL,
};

static uint32_t rotr32(uint32_t x, unsigned n)
{
	return (x >> n) | (x << (32 - n));
}

static void sha256_init(struct sha256 *s)
{
	s->state[0] = 0x6a09e667UL;
	s->state[1] = 0xbb67ae85UL;
	s->state[2] = 0x3c6ef372UL;
	s->state[3] = 0xa54ff53aUL;
	s->state[4] = 0x510e527fUL;
	s->state[5] = 0x9b05688cUL;
	s->state[6] = 0x1f83d9abUL;
	s->state[7] = 0x5be0cd19UL;
	s->total = 0;
	s->fill = 0;
}

static void sha256_transform(struct sha256 *s, const unsigned char *block)
{
	uint32_t w[64];
	uint32_t a, b, c, d, e, f, g, h;
	uint32_t x, y;
	int i;

	for (i = 0; i < 16; i++) {
		w[i] = ((uint32_t)block[i * 4] << 24) |
		       ((uint32_t)block[i * 4 + 1] << 16) |
		       ((uint32_t)block[i * 4 + 2] << 8) |
		       (uint32_t)block[i * 4 + 3];
	}
	for (i = 16; i < 64; i++) {
		uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^
			      (w[i - 15] >> 3);
		uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^
			      (w[i - 2] >> 10);

		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}

	a = s->state[0];
	b = s->state[1];
	c = s->state[2];
	d = s->state[3];
	e = s->state[4];
	f = s->state[5];
	g = s->state[6];
	h = s->state[7];

	for (i = 0; i < 64; i++) {
		uint32_t big_s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
		uint32_t choose = (e & f) ^ (~e & g);
		uint32_t big_s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
		uint32_t majority = (a & b) ^ (a & c) ^ (b & c);

		x = h + big_s1 + choose + sha256_k[i] + w[i];
		y = big_s0 + majority;
		h = g;
		g = f;
		f = e;
		e = d + x;
		d = c;
		c = b;
		b = a;
		a = x + y;
	}

	s->state[0] += a;
	s->state[1] += b;
	s->state[2] += c;
	s->state[3] += d;
	s->state[4] += e;
	s->state[5] += f;
	s->state[6] += g;
	s->state[7] += h;
}

static void sha256_update(struct sha256 *s, const void *data, size_t len)
{
	const unsigned char *p = data;

	s->total += (uint64_t)len;

	if (s->fill > 0) {
		size_t take = SHA256_BLOCK - s->fill;

		if (take > len)
			take = len;
		memcpy(s->buf + s->fill, p, take);
		s->fill += take;
		p += take;
		len -= take;
		if (s->fill == SHA256_BLOCK) {
			sha256_transform(s, s->buf);
			s->fill = 0;
		}
	}

	while (len >= SHA256_BLOCK) {
		sha256_transform(s, p);
		p += SHA256_BLOCK;
		len -= SHA256_BLOCK;
	}

	if (len > 0) {
		memcpy(s->buf, p, len);
		s->fill = len;
	}
}

static void sha256_final(struct sha256 *s, unsigned char out[SHA256_DIGEST])
{
	uint64_t bits = s->total * 8ULL;
	unsigned char pad[72];
	size_t padlen;
	size_t i;

	pad[0] = 0x80;
	padlen = (s->fill < 56) ? (56 - s->fill) : (120 - s->fill);

	memset(pad + 1, 0, padlen - 1);
	for (i = 0; i < 8; i++)
		pad[padlen + i] = (unsigned char)(bits >> (56 - 8 * i));

	sha256_update(s, pad, padlen + 8);

	for (i = 0; i < 8; i++) {
		out[i * 4] = (unsigned char)(s->state[i] >> 24);
		out[i * 4 + 1] = (unsigned char)(s->state[i] >> 16);
		out[i * 4 + 2] = (unsigned char)(s->state[i] >> 8);
		out[i * 4 + 3] = (unsigned char)s->state[i];
	}
}

static void to_hex(const unsigned char *in, size_t len, char *out)
{
	static const char digits[] = "0123456789abcdef";
	size_t i;

	for (i = 0; i < len; i++) {
		out[i * 2] = digits[in[i] >> 4];
		out[i * 2 + 1] = digits[in[i] & 0x0f];
	}
	out[len * 2] = '\0';
}

void edwl_sha256_buf(const void *data, size_t len, char out[EDWL_SHA256_HEX])
{
	struct sha256 s;
	unsigned char digest[SHA256_DIGEST];

	sha256_init(&s);
	if (data && len > 0)
		sha256_update(&s, data, len);
	sha256_final(&s, digest);
	to_hex(digest, sizeof(digest), out);
}

int edwl_sha256_file(const char *path, char out[EDWL_SHA256_HEX])
{
	struct sha256 s;
	unsigned char digest[SHA256_DIGEST];
	unsigned char *chunk;
	edwl_file_t fd;
	uint64_t offset = 0;
	int rc = -1;

	if (!path || !out)
		return -1;
	if (edwl_file_open_read(path, &fd) != 0)
		return -1;

	chunk = malloc(SHA256_BLOCK * 16);
	if (!chunk) {
		edwl_file_close(fd);
		return -1;
	}

	sha256_init(&s);
	for (;;) {
		int64_t n =
		    edwl_file_pread(fd, chunk, SHA256_BLOCK * 16, offset);

		if (n < 0)
			goto out;
		if (n == 0)
			break;
		sha256_update(&s, chunk, (size_t)n);
		offset += (uint64_t)n;
	}

	sha256_final(&s, digest);
	to_hex(digest, sizeof(digest), out);
	rc = 0;

out:
	free(chunk);
	edwl_file_close(fd);
	return rc;
}

static int hex_equal_ci(const char *a, const char *b, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		if (tolower((unsigned char)a[i]) !=
		    tolower((unsigned char)b[i]))
			return 0;
	}
	return 1;
}

int edwl_hash_equals(const char *a, const char *b)
{
	size_t la, lb;

	if (!a || !b)
		return 0;

	/* 容忍末尾空白（从文件里粘贴哈希时常见） */
	la = strlen(a);
	while (la > 0 && (a[la - 1] == '\n' || a[la - 1] == '\r' ||
			  a[la - 1] == ' ' || a[la - 1] == '\t'))
		la--;
	lb = strlen(b);
	while (lb > 0 && (b[lb - 1] == '\n' || b[lb - 1] == '\r' ||
			  b[lb - 1] == ' ' || b[lb - 1] == '\t'))
		lb--;

	if (la != 64 || lb != 64)
		return 0;
	return hex_equal_ci(a, b, 64);
}

int edwl_hash_is_valid(const char *hex)
{
	size_t i;

	if (!hex || strlen(hex) != 64)
		return 0;
	for (i = 0; i < 64; i++) {
		char c = hex[i];

		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
		      (c >= 'A' && c <= 'F')))
			return 0;
	}
	return 1;
}
