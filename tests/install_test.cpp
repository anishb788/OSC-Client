// SPDX-License-Identifier: GPL-3.0-or-later
// Host tests for the pure install modules. Build and run: make -f Makefile.pc test
// Increment 1: the vendored MD5 and the libzip link.
#include <cstring>
#include <string>
#include <vector>

#include <zip.h>

#include "Check.h"
#include "third_party/md5/md5.h"

static std::string hex(const unsigned char d[16]) {
	static const char* digits = "0123456789abcdef";
	std::string s;
	for (int i = 0; i < 16; i++) { s += digits[d[i] >> 4]; s += digits[d[i] & 15]; }
	return s;
}

static std::string md5Of(const void* data, unsigned long n) {
	MD5_CTX c;
	unsigned char d[16];
	MD5_Init(&c);
	MD5_Update(&c, data, n);
	MD5_Final(d, &c);
	return hex(d);
}

static std::string md5Of(const std::string& s) { return md5Of(s.data(), s.size()); }

static void testMd5Vectors() {
	// RFC 1321 appendix A.5.
	CHECK(md5Of("") == "d41d8cd98f00b204e9800998ecf8427e");
	CHECK(md5Of("a") == "0cc175b9c0f1b6a831c399e269772661");
	CHECK(md5Of("abc") == "900150983cd24fb0d6963f7d28e17f72");
	CHECK(md5Of("message digest") == "f96b697d7cb7938d525a2f31aaf161d0");
	CHECK(md5Of("abcdefghijklmnopqrstuvwxyz") == "c3fcd3d76192e4007dfb496cca67e13b");
	CHECK(md5Of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789") == "d174ab98d277d9f5a5611c2c9f419d9f");
	CHECK(md5Of("12345678901234567890123456789012345678901234567890123456789012345678901234567890") ==
	      "57edf4a22be3c955ac49da2e2107b67a");
	// Padding edge cases: 55, 56, 63, 64 and 65 bytes straddle the length-field boundary.
	CHECK(md5Of(std::string(55, 'a')) == "ef1772b6dff9a122358552954ad0df65");
	CHECK(md5Of(std::string(56, 'a')) == "3b0c8ac703f828b04c6c197006d17218");
	CHECK(md5Of(std::string(63, 'a')) == "b06521f39153d618550606be297466d5");
	CHECK(md5Of(std::string(64, 'a')) == "014842d480b571495a4a0363793f7367");
	CHECK(md5Of(std::string(65, 'a')) == "c743a45e0d2e6a95cb859adae0248435");
	// One million 'a' (the classic long vector).
	CHECK(md5Of(std::string(1000000, 'a')) == "7707d6ae4e027c70eea2a935c2296f21");
}

static void testMd5Chunked() {
	std::vector<unsigned char> buf(1 << 20);
	unsigned x = 12345;
	for (auto& b : buf) { x = x * 1664525u + 1013904223u; b = static_cast<unsigned char>(x >> 24); }
	const std::string whole = md5Of(buf.data(), buf.size());

	MD5_CTX c;
	unsigned char d[16];
	MD5_Init(&c);
	for (size_t off = 0; off < buf.size(); off += 65536) MD5_Update(&c, buf.data() + off, 65536);
	MD5_Final(d, &c);
	CHECK(hex(d) == whole);

	// Odd chunk sizes (never block-aligned) must agree too.
	MD5_Init(&c);
	size_t off = 0, step = 1;
	while (off < buf.size()) {
		size_t n = step < buf.size() - off ? step : buf.size() - off;
		MD5_Update(&c, buf.data() + off, n);
		off += n;
		step = step * 3 % 4099 + 1;
	}
	MD5_Final(d, &c);
	CHECK(hex(d) == whole);
}

static void testLibzipLinked() {
	const char* v = zip_libzip_version();
	CHECK(v != nullptr && v[0] >= '1' && v[0] <= '9');
	std::printf("libzip %s\n", v ? v : "(null)");
}

int main() {
	testMd5Vectors();
	testMd5Chunked();
	testLibzipLinked();
	return checkReport("install_test");
}
