// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// install::Md5Sink (M3 Increment 8): an HttpSink that wraps another sink (a FileSink writing <staging>/download.zip.tmp, in the
// app), counts the body, hashes it with the vendored MD5 and enforces the size the catalog promised.
//
// CHECKSUM WORDING: archives come over plain http; MD5 detects corruption and truncation, not tampering. The catalog that carries
// the hash comes over plain http too, so an attacker who can change the archive can change the hash. The UI's only word for a
// mismatch is "damaged" (Messages.h).
//
// RULES (all end the transfer; the client then reports failure()):
//   * onStart: a Content-Length that is not exactly `expectedSize` -> TooLarge. (Unknown length, -1 as in a chunked answer, passes
//     and is held to the byte count below.) The inner sink's onStart returning false -> Io.
//   * onData: the first byte past `expectedSize` -> TooLarge (nothing of that slice is hashed or written); the inner sink refusing
//     data (a write error, ENOSPC) -> Io.
//   * onFinish(ok): the digest is finalised when ok; the inner sink always gets onFinish (FileSink closes or removes its tmp).
// A body SHORTER than expectedSize is not refused here (the transfer layer reports an early close as Io, and a chunked body that
// ends early ends "ok"); the caller's verify step compares bytes() and digest with what the catalog says and calls it damaged.
//
// Threading: every call runs on the net worker thread (the transfer's); the getters are read after onDone (same thread) or after the
// result was handed over.
#include <cstdint>
#include <memory>
#include <string>

#include "net/Http.h"
#include "third_party/md5/md5.h"

namespace install {

class Md5Sink : public HttpSink {
public:
	Md5Sink(std::unique_ptr<HttpSink> inner, uint64_t expectedSize);

	bool onStart(int status, int64_t contentLength) override;
	bool onData(const uint8_t* data, size_t len) override;
	void onFinish(bool ok) override;
	HttpError failure() const override { return failure_; }

	// The wrapped sink (the caller knows its type: Installer static_casts it to FileSink to commit).
	HttpSink& inner() { return *inner_; }
	uint64_t bytes() const { return bytes_; }
	bool finished() const { return finished_; }
	// 32 lowercase hex digits; "" until onFinish(true) ran.
	const std::string& digestHex() const { return digest_; }

private:
	std::unique_ptr<HttpSink> inner_;
	uint64_t expected_;
	uint64_t bytes_ = 0;
	MD5_CTX ctx_;
	HttpError failure_ = HttpError::Io;
	bool finished_ = false;
	std::string digest_;
};

// Lowercase hex of 16 digest bytes. Exposed for the tests.
std::string md5Hex(const unsigned char digest[16]);
// Case-insensitive comparison of two hex strings (the catalog's hash may be upper case).
bool sameHex(const std::string& a, const std::string& b);

} // namespace install
