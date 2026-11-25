#include "net/Http.h"

#include <algorithm>
#include <new>

std::string describe(const HttpResult& r) {
	switch (r.error) {
	case HttpError::None: return "OK";
	case HttpError::NoNetwork: return "The Wii isn't connected to the internet. Check your network settings.";
	case HttpError::Dns: return "Couldn't find the server. Check your internet connection and DNS settings.";
	case HttpError::Connect: return "Couldn't connect to the server.";
	case HttpError::Timeout: return "The server took too long to respond.";
	case HttpError::HttpStatus:
		return r.status > 0 ? "The server returned an error (HTTP " + std::to_string(r.status) + ")."
		                    : "The server returned an error.";
	case HttpError::TlsRequired: return "The server needs a secure (HTTPS) connection, which isn't supported yet.";
	case HttpError::TooLarge: return "The server's response was too large.";
	case HttpError::Parse: return "The server sent a response that couldn't be understood.";
	case HttpError::Io: return "The connection was interrupted or the data couldn't be saved.";
	case HttpError::Cancelled: return "Cancelled.";
	case HttpError::Redirects: return "The server redirected too many times.";
	}
	return "Unknown error.";
}

namespace {
constexpr uint64_t kMaxReserve = 1024 * 1024;   // never trust Content-Length for the up-front allocation
}

bool MemorySink::onStart(int, int64_t contentLength) {
	if (maxBody_ && contentLength > 0 && static_cast<uint64_t>(contentLength) > maxBody_) {
		failure_ = HttpError::TooLarge;
		return false;
	}
	if (contentLength > 0) {
		uint64_t want = std::min<uint64_t>(static_cast<uint64_t>(contentLength), kMaxReserve);
		if (maxBody_) want = std::min(want, maxBody_);
		try {
			body_.reserve(static_cast<size_t>(want));
		} catch (const std::bad_alloc&) {
			failure_ = HttpError::TooLarge;
			return false;
		}
	}
	return true;
}

bool MemorySink::onData(const uint8_t* data, size_t len) {
	if (maxBody_ && body_.size() + len > maxBody_) {
		failure_ = HttpError::TooLarge;
		return false;
	}
	try {
		body_.append(reinterpret_cast<const char*>(data), len);
	} catch (const std::bad_alloc&) {
		failure_ = HttpError::TooLarge;
		return false;
	}
	return true;
}

namespace {
constexpr size_t kFileBuffer = 64 * 1024;
}

FileSink::~FileSink() {
	if (!committed_) abort();
}

bool FileSink::onStart(int, int64_t) {
	file_ = std::fopen(tmpPath_.c_str(), "wb");
	if (!file_) return false;
	opened_ = true;
	buffer_.reset(new char[kFileBuffer]);
	std::setvbuf(file_, buffer_.get(), _IOFBF, kFileBuffer);
	return true;
}

bool FileSink::onData(const uint8_t* data, size_t len) {
	if (!file_) return false;
	if (len && std::fwrite(data, 1, len, file_) != len) {
		writeFailed_ = true;
		return false;
	}
	written_ += len;
	return true;
}

void FileSink::onFinish(bool ok) {
	// fclose flushes the buffer, so a full SD card can surface here rather than in onData.
	bool closed = true;
	if (file_) {
		bool flushed = std::fflush(file_) == 0;
		closed = std::fclose(file_) == 0 && flushed;
		file_ = nullptr;
	}
	finishedOk_ = ok && opened_ && closed && !writeFailed_;
}

void FileSink::closeFile() {
	if (file_) {
		std::fclose(file_);
		file_ = nullptr;
	}
}

bool FileSink::commit() {
	if (committed_ || !finishedOk_) return false;
	if (!commitFile(tmpPath_, path_, ops_)) return false;
	committed_ = true;
	return true;
}

void FileSink::abort() {
	closeFile();
	std::remove(tmpPath_.c_str());
	finishedOk_ = false;
}
