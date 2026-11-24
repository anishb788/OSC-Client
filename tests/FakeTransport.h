#pragma once
// A scripted, deterministic Transport for the HttpTransfer tests. Time is virtual: nothing here ever sleeps, wait()
// just moves the shared FakeClock forward, so timeout tests run instantly and two transports can share one clock.
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "net/Transport.h"

struct FakeClock {
	uint64_t now = 0;
};

// What the "server" does on one connection (one entry per connect, so a redirect consumes the next script).
struct FakeScript {
	std::string response;            // bytes the server sends
	size_t maxPerRecv = (size_t)-1;  // largest slice a single recv() returns
	bool trickle = false;            // data "arrives" only after a wait(): every recv is followed by WouldBlock
	bool closeAtEnd = true;          // false: the connection stays open (a stalled server) once the data is used up
	int connectPolls = 0;            // checkConnected() answers InProgress this many times first (-1 = forever)
	bool connectFails = false;
	size_t maxPerSend = (size_t)-1;  // largest slice send() accepts
	bool sendBlocks = false;         // every send() accepts one slice, then WouldBlock until a wait(write)
	bool sendZero = false;           // send() answers Ok with 0 bytes (a misbehaving transport)
	int maxWaitMs = 0;               // >0: an idle wait() returns after at most this long (spurious wake-ups)
};

class FakeTransport : public Transport {
public:
	FakeTransport(FakeClock& clock, std::vector<FakeScript> scripts) : clock_(clock), scripts_(std::move(scripts)) {}

	// Test knobs and observations.
	HttpError resolveError = HttpError::None;
	int resolveCalls = 0;
	std::vector<std::string> resolvedHosts;
	std::vector<int> connectPorts;
	std::vector<std::string> requests;   // the request bytes received, one per connection
	int connects = 0;
	bool open = false;
	int resolveCostMs = 0;
	// Server side of the live connection: add more bytes later (a stalled script) or hang up.
	void append(const std::string& more) { cur().response += more; }
	void hangUp() { cur().closeAtEnd = true; }

	uint64_t nowMs() override { return clock_.now; }

	HttpError resolve(const std::string& host, uint32_t& ipv4) override {
		resolveCalls++;
		resolvedHosts.push_back(host);
		clock_.now += resolveCostMs;
		ipv4 = 0x7f000001;
		return resolveError;
	}

	HttpError beginConnect(uint32_t, int port) override {
		if (connects >= (int)scripts_.size()) return HttpError::Connect;
		connectPorts.push_back(port);
		connects++;
		open = true;
		pos_ = 0;
		polls_ = 0;
		ready_ = !cur().trickle;
		writable_ = true;
		requests.emplace_back();
		return HttpError::None;
	}

	ConnectState checkConnected() override {
		if (cur().connectFails) return ConnectState::Failed;
		if (cur().connectPolls < 0 || polls_ < cur().connectPolls) {
			polls_++;
			return ConnectState::InProgress;
		}
		return ConnectState::Connected;
	}

	IoResult send(const uint8_t* data, size_t len) override {
		if (cur().sendZero) return {IoStatus::Ok, 0};
		if (cur().sendBlocks && !writable_) return {IoStatus::WouldBlock, 0};
		size_t n = std::min(len, cur().maxPerSend);
		requests.back().append(reinterpret_cast<const char*>(data), n);
		if (cur().sendBlocks) writable_ = false;
		return {IoStatus::Ok, n};
	}

	IoResult recv(uint8_t* buf, size_t cap) override {
		const std::string& r = cur().response;
		if (pos_ >= r.size()) return {cur().closeAtEnd ? IoStatus::Closed : IoStatus::WouldBlock, 0};
		if (!ready_) return {IoStatus::WouldBlock, 0};
		size_t n = std::min({cap, cur().maxPerRecv, r.size() - pos_});
		std::memcpy(buf, r.data() + pos_, n);
		pos_ += n;
		if (cur().trickle) ready_ = false;
		return {IoStatus::Ok, n};
	}

	void wait(bool forWrite, int ms) override {
		if (forWrite) {
			writable_ = true;
			clock_.now += 1;
			return;
		}
		// Trickled data shows up after a millisecond; with nothing to deliver the whole wait elapses.
		if (!ready_ && pos_ < cur().response.size()) {
			ready_ = true;
			clock_.now += 1;
		} else if (pos_ >= cur().response.size() || ready_) {
			int slice = ms > 0 ? ms : 1;
			if (cur().maxWaitMs > 0 && slice > cur().maxWaitMs) slice = cur().maxWaitMs;
			clock_.now += slice;
		}
	}

	void close() override { open = false; }

private:
	FakeScript& cur() {
		static FakeScript empty;
		return connects > 0 ? scripts_[connects - 1] : empty;
	}
	FakeClock& clock_;
	std::vector<FakeScript> scripts_;
	size_t pos_ = 0;
	int polls_ = 0;
	bool ready_ = true;
	bool writable_ = true;
};
