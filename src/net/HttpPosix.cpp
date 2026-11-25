#ifndef __wii__
#include "net/HttpPosix.h"

#include <algorithm>
#include <cerrno>
#include <cstring>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

namespace {
// One poll never lasts longer than this, whatever the caller asks, so a cancel or a deadline is noticed within it.
constexpr int kMaxPollSliceMs = 50;
}

uint64_t PosixTransport::nowMs() {
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return static_cast<uint64_t>(ts.tv_sec) * 1000u + static_cast<uint64_t>(ts.tv_nsec) / 1000000u;
}

// getaddrinfo cannot be sliced or cancelled; HostCache keeps it to once per host.
HttpError PosixTransport::resolve(const std::string& host, uint32_t& ipv4) {
	addrinfo hints;
	std::memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	addrinfo* res = nullptr;
	int rc = getaddrinfo(host.c_str(), nullptr, &hints, &res);
	if (rc != 0 || !res) return HttpError::Dns;
	ipv4 = ntohl(reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr.s_addr);
	freeaddrinfo(res);
	return HttpError::None;
}

HttpError PosixTransport::beginConnect(uint32_t ipv4, int port) {
	close();
	fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd_ < 0) return HttpError::Connect;
	int one = 1;
	setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

	sockaddr_in sa;
	std::memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_port = htons(static_cast<uint16_t>(port));
	sa.sin_addr.s_addr = htonl(ipv4);
	if (::connect(fd_, reinterpret_cast<sockaddr*>(&sa), sizeof sa) == 0) {
		connected_ = true;
		return HttpError::None;
	}
	if (errno == EINPROGRESS || errno == EINTR) return HttpError::None;
	int e = errno;
	close();
	return (e == ENETUNREACH || e == ENETDOWN) ? HttpError::NoNetwork : HttpError::Connect;
}

Transport::ConnectState PosixTransport::checkConnected() {
	if (fd_ < 0) return ConnectState::Failed;
	if (connected_) return ConnectState::Connected;
	pollfd p{fd_, POLLOUT, 0};
	int rc = ::poll(&p, 1, 0);
	if (rc == 0 || (rc < 0 && errno == EINTR)) return ConnectState::InProgress;
	if (rc < 0) return ConnectState::Failed;
	int err = 0;
	socklen_t len = sizeof err;
	if (getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) return ConnectState::Failed;
	connected_ = true;
	return ConnectState::Connected;
}

// MSG_NOSIGNAL: a peer reset must surface as EPIPE, not kill the process with SIGPIPE.
Transport::IoResult PosixTransport::send(const uint8_t* data, size_t len) {
	if (fd_ < 0) return {IoStatus::Error, 0};
	if (len == 0) return {IoStatus::WouldBlock, 0};
	ssize_t n = ::send(fd_, data, len, MSG_NOSIGNAL);
	if (n > 0) return {IoStatus::Ok, static_cast<size_t>(n)};
	if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return {IoStatus::WouldBlock, 0};
	return {IoStatus::Error, 0};
}

Transport::IoResult PosixTransport::recv(uint8_t* buf, size_t cap) {
	if (fd_ < 0) return {IoStatus::Error, 0};
	if (cap == 0) return {IoStatus::WouldBlock, 0};
	ssize_t n = ::recv(fd_, buf, cap, 0);
	if (n > 0) return {IoStatus::Ok, static_cast<size_t>(n)};
	if (n == 0) return {IoStatus::Closed, 0};
	if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return {IoStatus::WouldBlock, 0};
	return {IoStatus::Error, 0};
}

void PosixTransport::wait(bool forWrite, int ms) {
	if (fd_ < 0) return;
	pollfd p{fd_, static_cast<short>(forWrite ? POLLOUT : POLLIN), 0};
	::poll(&p, 1, std::clamp(ms, 0, kMaxPollSliceMs));
}

void PosixTransport::close() {
	if (fd_ >= 0) ::close(fd_);
	fd_ = -1;
	connected_ = false;
}

std::unique_ptr<Transport> makePosixTransport() { return std::make_unique<PosixTransport>(); }
#endif
