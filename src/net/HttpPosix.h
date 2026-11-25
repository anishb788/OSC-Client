#pragma once
// POSIX (Linux/PC) implementation of Transport. The Wii uses libogc's net_* instead (HttpWii.cpp, Increment 7), so
// this header and HttpPosix.cpp compile to nothing there. No SDL.
//
// Keep-alive is not used: every transfer opens a fresh connection and sends "Connection: close" (see
// HttpParse::buildRequest). A per-host connection-reuse struct would sit between HttpTransfer and this class.
#ifndef __wii__
#include <memory>

#include "net/Transport.h"

class PosixTransport : public Transport {
public:
	PosixTransport() = default;
	~PosixTransport() override { close(); }
	PosixTransport(const PosixTransport&) = delete;
	PosixTransport& operator=(const PosixTransport&) = delete;

	uint64_t nowMs() override;
	HttpError resolve(const std::string& host, uint32_t& ipv4) override;
	HttpError beginConnect(uint32_t ipv4, int port) override;
	ConnectState checkConnected() override;
	IoResult send(const uint8_t* data, size_t len) override;
	IoResult recv(uint8_t* buf, size_t cap) override;
	void wait(bool forWrite, int ms) override;
	void close() override;

private:
	int fd_ = -1;
	bool connected_ = false;
};

std::unique_ptr<Transport> makePosixTransport();
#endif
