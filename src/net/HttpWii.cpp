#ifdef __wii__
#include "net/HttpWii.h"

#include <algorithm>
#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <network.h>
#include <ogc/lwp_watchdog.h>
#include <poll.h>

#include <SDL_thread.h>
#include <SDL_timer.h>
#include <SDL_version.h>

#include "core/Log.h"
#include "net/HttpParse.h"
#include "net/NetState.h"

// QUIRKS OBSERVED IN DOLPHIN (Increment 7; real hardware is still to be checked, see the epic notes)
//  - Dolphin's emulated network is a pass-through to the host: 127.0.0.1 on the Wii is the HOST's loopback, so a server
//    on the host's 127.0.0.1 or its LAN address both work, and the host resolver answers net_gethostbyname
//    ("localhost" resolves; a missing name fails with errno -7 in about 5 ms; real names take ~5 ms).
//  - net_init_async + net_get_status is ready in about 20 ms. net_init_async returns 0 when it started.
//  - A non-blocking net_connect returns -EINPROGRESS; net_poll(POLLOUT, 0 ms) then reports revents 0x08 (POLLOUT) on
//    success, and 0x68 (POLLOUT|POLLERR|POLLHUP) on a refused connection, where a second net_connect returns -111
//    (ECONNREFUSED) and, on success, 0. SO_ERROR is never consulted. A zero timeout returns at once, and wait()'s
//    50 ms slices behave (a cancel during a black-holed connect took effect within one slice).
//  - After net_deinit() a fresh net_init_async() works again (the retry path was exercised with a simulated failure).
//  Not seen anywhere yet: a real DHCP failure or an unplugged network (Dolphin always brings the network up), and
//  how long net_gethostbyname takes on a dead network on hardware (kDnsTimeoutMs is a guess from "about 30 s").

namespace {

// One net_poll never lasts longer than this, whatever the caller asks, so a cancel or a deadline is noticed within it
// (same bound as the POSIX backend).
constexpr int kMaxPollSliceMs = 50;
// IOS's O_NONBLOCK. net_fcntl hands the flags to IOS untranslated.
constexpr u32 kIosNonBlock = 0x04;
// libogc's net heap is small (about 32 KB) and net_send / net_recv allocate their length from it, so keep calls modest.
constexpr size_t kMaxSendChunk = 4096;
constexpr size_t kMaxRecvChunk = 8192;
// net_gethostbyname has no timeout of its own that we know of (about 30 s on a dead network). Give up on it earlier;
// the lookup keeps running on its orphaned thread.
constexpr int kDnsTimeoutMs = 15000;
constexpr int kNoMemBackoffMs = 5;
constexpr int kNoMemGiveUpMs = 10000;
constexpr size_t kDnsStackBytes = 32 * 1024;

// ---- DNS on a throwaway thread -------------------------------------------------------------------------------------
// net_gethostbyname cannot be interrupted, so the worker never calls it itself: a detached thread does, and the worker
// polls this slot. On timeout or abort the worker simply walks away. The thread owns a shared_ptr to the slot and
// touches nothing else of ours, so it may outlive the transport, the worker and even App::shutdown().
struct DnsSlot {
	std::string host;
	std::atomic<bool> done{false};
	std::atomic<bool> ok{false};
	std::atomic<uint32_t> ip{0};
};

// net_gethostbyname returns a pointer into one static buffer, so two lookups must never overlap. At most one slot is
// live; a lookup abandoned on timeout may still be running when the next resolve() starts, and that one waits for it
// (same host) or fails fast with Timeout (different host) instead of racing it. Worker thread only.
// A resolver that hangs inside IOS therefore blocks every other name until the app restarts: the orphan only ends when
// IOS answers, and there is no way to cancel an in-flight net_gethostbyname.
std::shared_ptr<DnsSlot> g_dns;

void lookup(DnsSlot& slot) {
	hostent* he = net_gethostbyname(slot.host.c_str());
	if (he && he->h_addrtype == AF_INET && he->h_length == 4 && he->h_addr_list && he->h_addr_list[0]) {
		// Copied out before any other net_* call: the buffer is static.
		const uint8_t* b = reinterpret_cast<const uint8_t*>(he->h_addr_list[0]);
		slot.ip.store((uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3], std::memory_order_relaxed);
		slot.ok.store(true, std::memory_order_relaxed);
	} else {
		LOG("net_gethostbyname(%s) failed: errno %d", slot.host.c_str(), errno);
	}
	slot.done.store(true, std::memory_order_release);
}

int dnsThreadMain(void* arg) {
	auto* owned = static_cast<std::shared_ptr<DnsSlot>*>(arg);
	std::shared_ptr<DnsSlot> slot = *owned;
	delete owned;
	lookup(*slot);
	return 0;
}

// ---- network bring-up ----------------------------------------------------------------------------------------------
enum class InitState { Idle, Running, Ready, Failed };
// Ready is sticky: nothing notices a network that drops later (cable pulled, access point gone), so M3's long downloads
// fail through their own idle timeouts and may need a re-init (see the epic notes).
InitState g_init = InitState::Idle;   // worker thread only (the main thread reads it in net::shutdown() after the join)
uint64_t g_initStartMs = 0;
uint64_t g_initFailedMs = 0;
// Failed because IOS reported an error (or net_init_async refused), so nothing is pending and net_deinit() is safe. A
// Failed state that came from our own timeout is NOT definite: IOS may still be working on it.
bool g_failedDefinite = false;

uint64_t nowMs() { return ticks_to_millisecs(gettime()); }

HttpError initFailed(const char* why, s32 code, bool definite) {
	LOG("net init failed (%s, code %d%s)", why, static_cast<int>(code), definite ? "" : ", IOS may still be working on it");
	g_init = InitState::Failed;
	g_initFailedMs = nowMs();
	g_failedDefinite = definite;
	net::setNetState(net::NetState::Down, SDL_GetTicks()); // pre-flight (M3 Increment 8); Down expires by itself (NetState.h)
	return HttpError::NoNetwork;
}

}  // namespace

namespace net {

HttpError ensureInit(const std::atomic<bool>* abort, const std::atomic<bool>* cancel) {
	if (g_init == InitState::Ready) return HttpError::None;
	auto interrupted = [&]() {
		if (abort && abort->load(std::memory_order_relaxed)) return HttpError::NoNetwork;
		if (cancel && cancel->load(std::memory_order_relaxed)) return HttpError::Cancelled;
		return HttpError::None;
	};
	uint64_t now = nowMs();
	if (g_init == InitState::Failed) {
		if (now - g_initFailedMs < static_cast<uint64_t>(kInitCooldownMs)) return HttpError::NoNetwork;
		if (g_failedDefinite) {
			// net_deinit() cannot be interrupted, so do not start it for a caller that is already giving up. NEEDS
			// HARDWARE CONFIRMATION that it returns promptly after a definite failure (Dolphin: it did).
			if (HttpError ie = interrupted(); ie != HttpError::None) return ie;
			// Clears a half-open top-level socket (see the header).
			net_deinit();
			g_init = InitState::Idle;
		} else {
			// Our own timeout: IOS may still be bringing the network up, and net_deinit() would wait for it (maybe
			// for good). Adopt the attempt instead: poll its status again with a fresh bound.
			LOG("net init: retrying after a timeout without net_deinit, polling the earlier attempt again");
			g_init = InitState::Running;
			g_initStartMs = now;
		}
	}
	if (g_init == InitState::Idle) {
		s32 r = net_init_async(nullptr, nullptr);
		// -EBUSY: a previous attempt is still running (we were aborted during it); just wait for that one.
		if (r < 0 && r != -EBUSY) return initFailed("net_init_async", r, true);
		g_init = InitState::Running;
		g_initStartMs = now;
		LOG("net init: started (net_init_async returned %d)", static_cast<int>(r));
	}
	for (;;) {
		s32 st = net_get_status();
		if (st == 0) {
			g_init = InitState::Ready;
			setNetState(NetState::Up, SDL_GetTicks());
			LOG("net init: ready after %llu ms", static_cast<unsigned long long>(nowMs() - g_initStartMs));
			return HttpError::None;
		}
		if (st != -EBUSY) return initFailed("net_get_status", st, true);
		// State stays Running, so the next caller adopts the attempt that is still going.
		if (HttpError ie = interrupted(); ie != HttpError::None) return ie;
		if (nowMs() - g_initStartMs >= static_cast<uint64_t>(kInitTotalMs)) {
			// Guessed bound, no hardware data yet. Leave the IOS side alone, see g_failedDefinite.
			return initFailed("timeout", st, false);
		}
		SDL_Delay(20);
	}
}

const char* shutdown() {
	// Called after Worker::stop() has joined the worker thread, so the globals are quiet.
	const char* what;
	if (g_init == InitState::Running) {
		what = "skipped net_deinit: the init is still running in IOS (it would block)";
	} else if (g_init == InitState::Failed && !g_failedDefinite) {
		what = "skipped net_deinit: the last init timed out and IOS may still be on it";
	} else if (g_init == InitState::Failed) {
		// A definite failure (IOS answered with an error) can still leave the top-level socket half open. Exiting to
		// the HBC with it open hung the reload on a red-bordered screen on real hardware (network off, init failed
		// with -24), while exits after a successful init + net_deinit were clean. Nothing is pending in IOS here, so
		// net_deinit returns promptly (the same call the cooldown retry makes).
		LOG("net shutdown: calling net_deinit after a failed init");
		net_deinit();
		g_init = InitState::Idle;
		what = "called net_deinit (the init had failed; closing what it left open)";
	} else if (g_init != InitState::Ready) {
		what = "nothing to do: the network was never brought up";
	} else if (g_dns && !g_dns->done.load(std::memory_order_acquire)) {
		what = "skipped net_deinit: a DNS lookup is still live in IOS";
	} else {
		LOG("net shutdown: calling net_deinit");
		net_deinit();
		g_init = InitState::Idle;
		what = "called net_deinit (network was up, no lookup live)";
	}
	LOG("net shutdown: %s", what);
	return what;
}

}  // namespace net

uint64_t WiiTransport::nowMs() { return ::nowMs(); }

HttpError WiiTransport::resolve(const std::string& host, uint32_t& ipv4) {
	if (HttpError ie = interrupted(); ie != HttpError::None) return ie;
	if (httpparse::parseDottedQuad(host, ipv4)) return HttpError::None;   // no IOS round trip for a literal address
	HttpError ie = net::ensureInit(&aborted_, cancel_);
	if (ie != HttpError::None) return ie;

	std::shared_ptr<DnsSlot> slot;
	if (g_dns && !g_dns->done.load(std::memory_order_acquire)) {
		if (g_dns->host != host) {
			LOG("dns %s: an abandoned lookup of %s is still running", host.c_str(), g_dns->host.c_str());
			return HttpError::Dns;
		}
		slot = g_dns;
	} else {
		slot = std::make_shared<DnsSlot>();
		slot->host = host;
		auto* handoff = new std::shared_ptr<DnsSlot>(slot);
#if SDL_VERSION_ATLEAST(2, 0, 9)
		SDL_Thread* t = SDL_CreateThreadWithStackSize(dnsThreadMain, "net-dns", kDnsStackBytes, handoff);
#else
		SDL_Thread* t = SDL_CreateThread(dnsThreadMain, "net-dns", handoff);
#endif
		if (!t) {
			// Rare: no thread. A plain blocking lookup still works, it just cannot be abandoned.
			LOG("dns: SDL_CreateThread failed (%s), resolving inline", SDL_GetError());
			delete handoff;
			lookup(*slot);
		} else {
			SDL_DetachThread(t);
			g_dns = slot;
		}
	}

	uint64_t t0 = nowMs();
	while (!slot->done.load(std::memory_order_acquire)) {
		// The slot stays orphaned in g_dns, see DnsSlot; the next resolve() adopts or waits for it.
		if (HttpError ie = interrupted(); ie != HttpError::None) return ie;
		if (nowMs() - t0 >= static_cast<uint64_t>(kDnsTimeoutMs)) {
			LOG("dns %s: gave up after %d ms (lookup abandoned)", host.c_str(), kDnsTimeoutMs);
			return HttpError::Timeout;
		}
		SDL_Delay(5);
	}
	if (g_dns == slot) g_dns.reset();
	if (!slot->ok.load(std::memory_order_relaxed)) return HttpError::Dns;
	ipv4 = slot->ip.load(std::memory_order_relaxed);
	return HttpError::None;
}

namespace {
sockaddr_in makeAddr(uint32_t ipv4, int port) {
	sockaddr_in sa;
	std::memset(&sa, 0, sizeof sa);
	sa.sin_len = 8;   // libogc's net_connect wants the IOS-sized 8, whatever sizeof says
	sa.sin_family = AF_INET;
	sa.sin_port = htons(static_cast<uint16_t>(port));
	sa.sin_addr.s_addr = htonl(ipv4);
	return sa;
}
}  // namespace

HttpError WiiTransport::beginConnect(uint32_t ipv4, int port) {
	close();
	if (HttpError ie = interrupted(); ie != HttpError::None) return ie;
	HttpError ie = net::ensureInit(&aborted_, cancel_);
	if (ie != HttpError::None) return ie;

	fd_ = net_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
	if (fd_ < 0) {
		LOG("net_socket failed: %d", fd_);
		int e = fd_;
		fd_ = -1;
		return e == -ENETDOWN ? HttpError::NoNetwork : HttpError::Connect;
	}
	// Without this net_connect / net_recv would block the worker, so failing to set it is fatal for this connection.
	s32 fr = net_fcntl(fd_, F_SETFL, kIosNonBlock);
	if (fr < 0) {
		LOG("net_fcntl(O_NONBLOCK) failed: %d", static_cast<int>(fr));
		close();
		return HttpError::Connect;
	}
	u32 one = 1;
	net_setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);   // best effort

	ip_ = ipv4;
	port_ = port;
	sockaddr_in sa = makeAddr(ipv4, port);
	s32 r = net_connect(fd_, reinterpret_cast<sockaddr*>(&sa), sizeof sa);
	if (r == 0 || r == -EISCONN) {
		connected_ = true;
		return HttpError::None;
	}
	// A non-blocking connect reports "started" as -EINPROGRESS; -EAGAIN and -EALREADY are the other spellings seen.
	if (r == -EINPROGRESS || r == -EAGAIN || r == -EALREADY) return HttpError::None;
	LOG("net_connect failed immediately: %d", static_cast<int>(r));
	close();
	return (r == -ENETUNREACH || r == -ENETDOWN) ? HttpError::NoNetwork : HttpError::Connect;
}

// SO_ERROR is not reliable on libogc, so success is confirmed the BSD way: once the socket polls writable, connect()
// again. 0 or -EISCONN means connected, -EINPROGRESS / -EALREADY means still going, anything else is the real error.
Transport::ConnectState WiiTransport::checkConnected() {
	if (fd_ < 0) return ConnectState::Failed;
	if (connected_) return ConnectState::Connected;
	pollsd p{fd_, POLLOUT, 0};
	s32 pr = net_poll(&p, 1, 0);
	if (pr == 0 || pr == -EAGAIN || pr == -EINTR) return ConnectState::InProgress;
	// A hard poll error says nothing about the connection, so do not fail it: the second net_connect below is the real
	// test and gives its own verdict (revents stays 0). wait() sleeps briefly meanwhile, so this does not spin.
	if (pr < 0) LOG("net_poll during connect failed: %d, asking net_connect directly", static_cast<int>(pr));
	sockaddr_in sa = makeAddr(ip_, port_);
	s32 r = net_connect(fd_, reinterpret_cast<sockaddr*>(&sa), sizeof sa);
	LOG("connect check: poll revents=0x%x, second net_connect=%d", static_cast<unsigned>(p.revents), static_cast<int>(r));
	if (r == 0 || r == -EISCONN) {
		// A socket can poll writable together with an error flag; trust the flag if connect() was not conclusive.
		connected_ = true;
		return ConnectState::Connected;
	}
	if (r == -EINPROGRESS || r == -EALREADY || r == -EAGAIN) {
		if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) return ConnectState::Failed;
		return ConnectState::InProgress;
	}
	return ConnectState::Failed;
}

// libogc's small net heap can be full for a moment (other sockets, the DNS thread's buffers), and net_send / net_recv
// then answer -ENOMEM without having done anything. That is transient, so report "try again" after a short sleep. A
// heap that stays full is cut off after kNoMemGiveUpMs (also bounded by the transfer's idle timeout, which keeps
// counting while this returns WouldBlock).
Transport::IoResult WiiTransport::noMemBackoff(const char* what) {
	uint64_t now = nowMs();
	if (nomemSinceMs_ == 0) nomemSinceMs_ = now ? now : 1;
	if (now - nomemSinceMs_ >= static_cast<uint64_t>(kNoMemGiveUpMs)) {
		LOG("%s: -ENOMEM for %d ms, giving up", what, kNoMemGiveUpMs);
		return {IoStatus::Error, 0};
	}
	SDL_Delay(kNoMemBackoffMs);
	return {IoStatus::WouldBlock, 0};
}

Transport::IoResult WiiTransport::send(const uint8_t* data, size_t len) {
	if (fd_ < 0) return {IoStatus::Error, 0};
	if (len == 0) return {IoStatus::WouldBlock, 0};
	s32 n = net_send(fd_, data, static_cast<s32>(std::min(len, kMaxSendChunk)), 0);
	if (n > 0) {
		nomemSinceMs_ = 0;
		sentAny_ = true;
		return {IoStatus::Ok, static_cast<size_t>(n)};
	}
	if (n == 0 || n == -EAGAIN || n == -EINTR) return {IoStatus::WouldBlock, 0};
	if (n == -ENOMEM) return noMemBackoff("net_send");
	// Hardware: a connect to a closed port "succeeds" and the first send fails with -ENOTCONN (-128).
	refused_ = !sentAny_ && (n == -ENOTCONN || n == -ECONNREFUSED || n == -ECONNRESET);
	LOG("net_send failed: %d%s", static_cast<int>(n), refused_ ? " (the connection was refused)" : "");
	return {IoStatus::Error, 0};
}

Transport::IoResult WiiTransport::recv(uint8_t* buf, size_t cap) {
	if (fd_ < 0) return {IoStatus::Error, 0};
	if (cap == 0) return {IoStatus::WouldBlock, 0};
	s32 n = net_recv(fd_, buf, static_cast<s32>(std::min(cap, kMaxRecvChunk)), 0);
	if (n > 0) {
		nomemSinceMs_ = 0;
		return {IoStatus::Ok, static_cast<size_t>(n)};
	}
	if (n == 0) return {IoStatus::Closed, 0};
	if (n == -EAGAIN || n == -EINTR) return {IoStatus::WouldBlock, 0};
	if (n == -ENOMEM) return noMemBackoff("net_recv");
	LOG("net_recv failed: %d", static_cast<int>(n));
	return {IoStatus::Error, 0};
}

void WiiTransport::wait(bool forWrite, int ms) {
	if (fd_ < 0 || interrupted() != HttpError::None) return;
	// A net_poll under way is not woken by abort() or a cancel; the caller's loop sees the flag after this slice ends.
	int slice = std::clamp(ms, 0, kMaxPollSliceMs);
	pollsd p{fd_, static_cast<u32>(forWrite ? POLLOUT : POLLIN), 0};
	s32 r = net_poll(&p, 1, slice);
	// A hard poll error can come back instantly, so degrade to a short sleep instead of a CPU-spinning wait loop.
	if (r < 0 && r != -EAGAIN && r != -EINTR && slice > 0) SDL_Delay(std::min(slice, 10));
}

void WiiTransport::close() {
	if (fd_ >= 0) net_close(fd_);
	fd_ = -1;
	connected_ = false;
	nomemSinceMs_ = 0;
}

std::unique_ptr<Transport> makeWiiTransport() { return std::make_unique<WiiTransport>(); }
#endif
