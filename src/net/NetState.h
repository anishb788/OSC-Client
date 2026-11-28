// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// The last known state of the network connection, for pre-flight (M3 Increment 8). Header only, no SDL, no sockets, so the pure
// install code and the host tests can use it.
//
// WRITERS. The Wii's ensureInit (HttpWii.cpp) says Up when the network came up and Down when bring-up failed; the archive download
// (install::Installer) says Up after any answer from the server and Down after NoNetwork. Anything else (catalog, icons) may report
// too. Nobody has to: Unknown is the normal answer before the first transfer.
//
// DOWN EXPIRES. A "Down" nobody could clear (pre-flight refuses to start a transfer while it holds) would lock the user out until a
// restart, so Down counts as Unknown again after kDownTtlMs: the next job then tries (and the Wii's own init cool-down of 3 s keeps
// that cheap). Up never expires; it only changes when a transfer says otherwise.
#include <atomic>
#include <cstdint>

namespace net {

enum class NetState : uint8_t { Unknown = 0, Up = 1, Down = 2 };

constexpr uint32_t kDownTtlMs = 10000;

namespace detail {
inline std::atomic<uint8_t>& stateCell() {
	static std::atomic<uint8_t> s{0};
	return s;
}
inline std::atomic<uint32_t>& sinceCell() {
	static std::atomic<uint32_t> s{0};
	return s;
}
} // namespace detail

// `nowMs` is any monotonic millisecond clock (SDL_GetTicks); it is stored with Down so that Down can expire.
inline void setNetState(NetState s, uint32_t nowMs) {
	detail::sinceCell().store(nowMs, std::memory_order_relaxed);
	detail::stateCell().store(static_cast<uint8_t>(s), std::memory_order_release);
}
// The pure expiry rule (host-tested): Down older than kDownTtlMs reads as Unknown. Unsigned subtraction survives clock wrap.
inline NetState effectiveNetState(NetState stored, uint32_t sinceMs, uint32_t nowMs) {
	if (stored == NetState::Down && nowMs - sinceMs >= kDownTtlMs) return NetState::Unknown;
	return stored;
}
inline NetState netState(uint32_t nowMs) {
	const NetState s = static_cast<NetState>(detail::stateCell().load(std::memory_order_acquire));
	return effectiveNetState(s, detail::sinceCell().load(std::memory_order_relaxed), nowMs);
}

} // namespace net
