// SPDX-License-Identifier: GPL-3.0-or-later
#include "install/Md5Sink.h"

#include <cctype>

namespace install {

std::string md5Hex(const unsigned char digest[16]) {
	static const char* const d = "0123456789abcdef";
	std::string s;
	s.reserve(32);
	for (int i = 0; i < 16; i++) {
		s += d[digest[i] >> 4];
		s += d[digest[i] & 15];
	}
	return s;
}

bool sameHex(const std::string& a, const std::string& b) {
	if (a.size() != b.size()) return false;
	for (size_t i = 0; i < a.size(); i++)
		if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
	return true;
}

Md5Sink::Md5Sink(std::unique_ptr<HttpSink> inner, uint64_t expectedSize) : inner_(std::move(inner)), expected_(expectedSize) {
	MD5_Init(&ctx_);
}

bool Md5Sink::onStart(int status, int64_t contentLength) {
	if (contentLength >= 0 && static_cast<uint64_t>(contentLength) != expected_) {
		failure_ = HttpError::TooLarge;
		return false;
	}
	if (!inner_ || !inner_->onStart(status, contentLength)) {
		failure_ = HttpError::Io;
		return false;
	}
	return true;
}

bool Md5Sink::onData(const uint8_t* data, size_t len) {
	if (bytes_ + len < bytes_ || bytes_ + len > expected_) {
		failure_ = HttpError::TooLarge;
		return false;
	}
	if (len && (!inner_ || !inner_->onData(data, len))) {
		failure_ = HttpError::Io;
		return false;
	}
	MD5_Update(&ctx_, data, static_cast<unsigned long>(len));
	bytes_ += len;
	return true;
}

void Md5Sink::onFinish(bool ok) {
	if (finished_) return;
	finished_ = true;
	if (ok) {
		unsigned char d[16];
		MD5_Final(d, &ctx_);
		digest_ = md5Hex(d);
	}
	if (inner_) inner_->onFinish(ok);
}

} // namespace install
