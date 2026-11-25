#pragma once
// The socket seam under HttpTransfer. Everything here is non-blocking except resolve(), so one thread can advance
// several transfers in turn (see the decision in HttpTransfer.h). Implementations: POSIX (Increment 6), libogc
// (Increment 7) and the scripted FakeTransport in tests/. No SDL, no libogc includes.
//
// One Transport object is one connection slot: a transfer owns (or is lent) its own instance, and reuses it across
// redirects by close() then beginConnect() again. Two transfers in flight need two Transport objects.
#include <atomic>
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

	// BLOCKING. The one phase a time slice cannot bound (getaddrinfo / net_gethostbyname). Returns Dns, NoNetwork,
	// Timeout (a backend that bounds the lookup itself), Cancelled (a backend that watches setCancel()'s flag, so a
	// cancelled job does not sit out the lookup) or None; the address is IPv4 in host byte order (the Wii has no IPv6).
	virtual HttpError resolve(const std::string& host, uint32_t& ipv4) = 0;

	// Starts a non-blocking connect. Anything but None is final for this attempt (reported as Connect unless it is
	// NoNetwork or Cancelled).
	virtual HttpError beginConnect(uint32_t ipv4, int port) = 0;
	virtual ConnectState checkConnected() = 0;

	// Never block. send/recv return Ok only with bytes > 0 (the transfer treats Ok/0 as WouldBlock). Closed from
	// recv() means an orderly shutdown by the peer.
	virtual IoResult send(const uint8_t* data, size_t len) = 0;
	virtual IoResult recv(uint8_t* buf, size_t cap) = 0;

	// Waits until the socket is readable (or writable when forWrite) or `ms` elapsed, whichever is first. May return
	// early or spuriously. Implementations bound each internal poll to ~50 ms: a cancel or abort() is noticed by the
	// caller's loop after at most one such slice, not instantly (a poll in progress is not woken).
	virtual void wait(bool forWrite, int ms) = 0;

	virtual void close() = 0;

	// OPTIONAL. The job's cancel flag (owned by the Worker, valid for this transport's whole life; set once, before the
	// first call, on the worker thread). A backend with a long non-sliceable phase (libogc's net init and DNS poll
	// loops) polls it next to abort() and returns promptly (Cancelled, or NoNetwork for abort). The default ignores it:
	// HttpTransfer already checks the flag between steps.
	virtual void setCancel(const std::atomic<bool>*) {}

	// OPTIONAL, THREAD-SAFE: the only Transport call that may come from another thread. Called by Worker::stop() so a
	// backend whose poll loops cannot be interrupted (libogc's net init / DNS wait) gives up and lets the worker
	// thread be joined. It only raises a flag: a loop that is mid-poll sees it within one <= 50 ms slice, a call
	// that is blocked inside the OS (getaddrinfo on the PC) is not woken. After abort() every call fails fast
	// (NoNetwork / Error); it is never undone. The default does nothing (POSIX waits are already sliced).
	virtual void abort() {}

	// OPTIONAL. True when the last send() failed because the connection was never really made (the peer refused it).
	// On a real Wii, connecting to a closed port reports success and only the first net_send fails (-ENOTCONN), so
	// HttpTransfer asks this when the very first send fails, to report Connect instead of Io.
	virtual bool refusedOnSend() const { return false; }
};
