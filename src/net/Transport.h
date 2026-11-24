#pragma once
// The socket seam under HttpTransfer. Everything here is non-blocking except resolve(), so one thread can advance
// several transfers in turn (see the decision in HttpTransfer.h). Implementations: POSIX (Increment 6), libogc
// (Increment 7) and the scripted FakeTransport in tests/. No SDL, no libogc includes.
//
// One Transport object is one connection slot: a transfer owns (or is lent) its own instance, and reuses it across
// redirects by close() then beginConnect() again. Two transfers in flight need two Transport objects.
#include <cstddef>
#include <cstdint>
#include <string>

#include "net/Http.h"

class Transport {
public:
	virtual ~Transport() = default;

	enum class IoStatus { Ok, WouldBlock, Closed, Error };
	struct IoResult {
		IoStatus status = IoStatus::Ok;
		size_t bytes = 0;   // transferred when status == Ok; Ok always means bytes > 0 (report no progress as WouldBlock)
	};
	enum class ConnectState { InProgress, Connected, Failed };

	// Monotonic milliseconds. It lives on the transport so tests drive time deterministically (the fake advances it
	// inside wait()) and a real transport reads its platform clock. Must not go backwards.
	virtual uint64_t nowMs() = 0;

	// BLOCKING. The one phase a time slice cannot bound (getaddrinfo / net_gethostbyname). Returns Dns, NoNetwork or
	// None; the address is IPv4 in host byte order (the Wii has no IPv6).
	virtual HttpError resolve(const std::string& host, uint32_t& ipv4) = 0;

	// Starts a non-blocking connect. Anything but None is final for this attempt (reported as Connect unless it is
	// NoNetwork).
	virtual HttpError beginConnect(uint32_t ipv4, int port) = 0;
	virtual ConnectState checkConnected() = 0;

	// Never block. send/recv return Ok only with bytes > 0 (the transfer treats Ok/0 as WouldBlock). Closed from
	// recv() means an orderly shutdown by the peer.
	virtual IoResult send(const uint8_t* data, size_t len) = 0;
	virtual IoResult recv(uint8_t* buf, size_t cap) = 0;

	// Waits until the socket is readable (or writable when forWrite) or `ms` elapsed, whichever is first. May return
	// early or spuriously. Implementations bound each internal poll to ~50 ms and look at the transfer's cancel flag
	// via their own means, so a cancel is seen promptly (Increment 6).
	virtual void wait(bool forWrite, int ms) = 0;

	virtual void close() = 0;
};
