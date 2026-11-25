#pragma once
// libogc (Wii) implementation of Transport (Increment 7); the POSIX one is HttpPosix.h. Everything here compiles to
// nothing on the PC. The same HttpTransfer, Worker scheduler and httpGet() run on top of it unchanged.
//
// libogc facts this file is built on (read from the headers and the disassembled libogc.a, not guessed):
//  - net_* return a negative errno (-EAGAIN, -EINPROGRESS, ...) and leave errno alone, except net_gethostbyname which
//    returns NULL and stores the negative code in errno.
//  - net_init() blocks (it sleeps on an LWP queue), but net_init_async() + net_get_status() can be polled, which is
//    what makes the network bring-up cancellable (see ensureInit).
//  - net_gethostbyname() blocks for the whole IOS lookup and cannot be interrupted; see WiiTransport::resolve.
//  - net_fcntl(F_SETFL) passes its flags straight to IOS, so the non-blocking flag is IOS's 0x04, not newlib's O_NONBLOCK.
#ifdef __wii__
#include <atomic>
#include <memory>
#include <string>

#include "net/Transport.h"

namespace net {

// Brings the network up on first use and returns None, NoNetwork when it cannot be (no configured connection, DHCP
// failure, `abort` raised, or the total bound passed) or Cancelled (the job's `cancel` flag rose). Only ever called
// from the network worker thread, so no locking. The poll loop checks both flags every 20 ms, so a stop or a cancel
// ends the wait within that (it does not interrupt a single net_* call). Other jobs wait while it runs, which is fine
// because no transfer can run before the network is up.
//  - A failed attempt is remembered: calls within kInitCooldownMs of it return NoNetwork at once, so a queue of jobs
//    does not each wait out its own timeout. After that:
//      * if IOS reported the failure, net_deinit() runs (a failed init can leave the IOS top-level socket open, and
//        net_init_async() then claims success) and init starts again;
//      * if our own 30 s bound expired, IOS may still be pending and net_deinit() could block for good, so the earlier
//        attempt is polled again instead.
//    Both rest on Dolphin only; confirm on hardware.
//  - If `abort` or `cancel` fires mid-init the IOS side keeps going; the next call picks the result up instead of
//    starting over.
HttpError ensureInit(const std::atomic<bool>* abort, const std::atomic<bool>* cancel = nullptr);

// App exit, after Worker::stop() joined the worker. Calls net_deinit() only when the network is up and no DNS lookup is
// live in IOS; otherwise it skips it, so exiting never blocks. Logs and returns which case it took (for the self-test
// log file). Main thread.
const char* shutdown();

constexpr int kInitTotalMs = 30000;
constexpr int kInitCooldownMs = 3000;

}  // namespace net

class WiiTransport : public Transport {
public:
	WiiTransport() = default;
	~WiiTransport() override { close(); }
	WiiTransport(const WiiTransport&) = delete;
	WiiTransport& operator=(const WiiTransport&) = delete;

	uint64_t nowMs() override;
	HttpError resolve(const std::string& host, uint32_t& ipv4) override;
	HttpError beginConnect(uint32_t ipv4, int port) override;
	ConnectState checkConnected() override;
	IoResult send(const uint8_t* data, size_t len) override;
	IoResult recv(uint8_t* buf, size_t cap) override;
	void wait(bool forWrite, int ms) override;
	void close() override;
	void setCancel(const std::atomic<bool>* c) override { cancel_ = c; }
	// Only raises a flag: init and DNS loops see it within one poll slice (<= 50 ms), net_poll is not woken.
	void abort() override { aborted_.store(true, std::memory_order_relaxed); }
	bool refusedOnSend() const override { return refused_; }

private:
	HttpError interrupted() const {
		if (aborted_.load(std::memory_order_relaxed)) return HttpError::NoNetwork;
		if (cancel_ && cancel_->load(std::memory_order_relaxed)) return HttpError::Cancelled;
		return HttpError::None;
	}
	IoResult noMemBackoff(const char* what);

	const std::atomic<bool>* cancel_ = nullptr;
	uint64_t nomemSinceMs_ = 0;
	int fd_ = -1;
	bool connected_ = false;
	bool sentAny_ = false;  // a send() has succeeded on this connection
	bool refused_ = false;  // see refusedOnSend()
	uint32_t ip_ = 0;
	int port_ = 0;
	std::atomic<bool> aborted_{false};
};

std::unique_ptr<Transport> makeWiiTransport();
#else
namespace net {
inline const char* shutdown() { return "no-op on the PC"; }
}
#endif
