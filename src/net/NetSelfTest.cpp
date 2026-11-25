#include "net/NetSelfTest.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include <cstdarg>

#include "core/Log.h"

namespace net {

namespace {
std::FILE* g_ntlog = nullptr;
}

void nettestLogTo(const std::string& path) {
	if (g_ntlog) std::fclose(g_ntlog);
	g_ntlog = path.empty() ? nullptr : std::fopen(path.c_str(), "w");
	if (!path.empty() && !g_ntlog) LOG("[nettest] could not open %s for writing", path.c_str());
}

void nettestLog(const char* fmt, ...) {
	if (!g_ntlog) return;
	va_list ap;
	va_start(ap, fmt);
	std::vfprintf(g_ntlog, fmt, ap);
	va_end(ap);
	std::fputc('\n', g_ntlog);
	std::fflush(g_ntlog); // a crash or power-off should still leave everything logged so far
}

namespace {

const char* errName(HttpError e) {
	switch (e) {
	case HttpError::None: return "None";
	case HttpError::NoNetwork: return "NoNetwork";
	case HttpError::Dns: return "Dns";
	case HttpError::Connect: return "Connect";
	case HttpError::Timeout: return "Timeout";
	case HttpError::HttpStatus: return "HttpStatus";
	case HttpError::TlsRequired: return "TlsRequired";
	case HttpError::TooLarge: return "TooLarge";
	case HttpError::Parse: return "Parse";
	case HttpError::Io: return "Io";
	case HttpError::Cancelled: return "Cancelled";
	case HttpError::Redirects: return "Redirects";
	}
	return "?";
}

uint32_t crcStep(uint32_t crc, const uint8_t* p, size_t n) {
	// Bitwise CRC-32 (IEEE): slow but tiny, and the self-test is not performance-sensitive.
	for (size_t i = 0; i < n; i++) {
		crc ^= p[i];
		for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
	}
	return crc;
}

// tools/http_test_server.py: byte i = (i*167 + (i>>8)*13 + 5) & 0xff. 32-bit wraparound keeps the period at 65536.
uint8_t patternByte(uint32_t i) { return static_cast<uint8_t>((i * 167u + (i >> 8) * 13u + 5u) & 0xFFu); }

bool startsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

}  // namespace

struct NetSelfTest::Stats {
	std::string label;
	std::string url;
	bool checkPattern = false;
	// Written on the worker thread, read on the main thread only after `done` was posted back through drainResults().
	uint64_t bytes = 0;
	uint32_t crc = 0xFFFFFFFFu;
	int64_t firstBad = -1;
	std::string head;
	int status = 0;
	HttpResult result;
	bool done = false;   // main thread only
	uint32_t startMs = 0, endMs = 0;
};

namespace {
class StatsSink : public HttpSink {
public:
	struct S {
		bool checkPattern;
		uint64_t* bytes;
		uint32_t* crc;
		int64_t* firstBad;
		std::string* head;
		int* status;
	};
	explicit StatsSink(S s) : s_(s) {}
	bool onStart(int status, int64_t) override {
		*s_.status = status;
		return true;
	}
	bool onData(const uint8_t* d, size_t n) override {
		if (s_.head->size() < 40) {
			for (size_t i = 0; i < n && s_.head->size() < 40; i++) {
				char c = static_cast<char>(d[i]);
				s_.head->push_back(c >= 32 && c < 127 ? c : '.');
			}
		}
		*s_.crc = crcStep(*s_.crc, d, n);
		if (s_.checkPattern && *s_.firstBad < 0) {
			for (size_t i = 0; i < n; i++) {
				if (d[i] != patternByte(static_cast<uint32_t>(*s_.bytes + i))) {
					*s_.firstBad = static_cast<int64_t>(*s_.bytes + i);
					break;
				}
			}
		}
		*s_.bytes += n;
		return true;
	}
	void onFinish(bool) override {}

private:
	S s_;
};
}  // namespace

namespace {
bool isHttpUrl(const std::string& u) { return startsWith(u, "http://") || startsWith(u, "https://"); }

// Whole-string decimal, optionally negative (clamped to 0); false when it is not a number at all. Huge values saturate.
bool parseMs(const std::string& s, int& out) {
	size_t i = 0;
	bool neg = false;
	if (i < s.size() && s[i] == '-') neg = true, i++;
	if (i >= s.size()) return false;
	long v = 0;
	for (; i < s.size(); i++) {
		if (s[i] < '0' || s[i] > '9') return false;
		if (v < 1000000000L) v = v * 10 + (s[i] - '0');
	}
	out = neg ? 0 : static_cast<int>(std::min(v, 1000000000L));
	return true;
}
}  // namespace

std::vector<NetTestItem> parseNetTestScript(const std::string& script) {
	std::vector<NetTestItem> items;
	size_t pos = 0;
	while (pos <= script.size()) {
		size_t end = script.find(',', pos);
		if (end == std::string::npos) end = script.size();
		std::string tok = script.substr(pos, end - pos);
		pos = end + 1;
		if (tok.empty()) continue;
		NetTestItem it;
		if (startsWith(tok, "bg:")) {
			it.kind = NetTestItem::Kind::Bg;
			it.url = tok.substr(3);
			if (!isHttpUrl(it.url)) {
				NTLOG("[nettest] skipping bad bg item \"%s\"", tok.c_str());
				continue;
			}
		} else if (startsWith(tok, "cancel@")) {
			size_t colon = tok.find(':');
			it.kind = NetTestItem::Kind::Cancel;
			if (colon == std::string::npos || !parseMs(tok.substr(7, colon - 7), it.ms) || !isHttpUrl(tok.substr(colon + 1))) {
				NTLOG("[nettest] skipping malformed item \"%s\" (want cancel@<ms>:<http url>)", tok.c_str());
				continue;
			}
			it.url = tok.substr(colon + 1);
		} else if (startsWith(tok, "pause@")) {
			it.kind = NetTestItem::Kind::Pause;
			if (!parseMs(tok.substr(6), it.ms)) {
				NTLOG("[nettest] skipping malformed item \"%s\" (want pause@<ms>)", tok.c_str());
				continue;
			}
		} else if (tok == "waitbg") {
			it.kind = NetTestItem::Kind::WaitBg;
		} else if (isHttpUrl(tok)) {
			it.url = tok;
		} else {
			NTLOG("[nettest] skipping unknown item \"%s\"", tok.c_str());
			continue;
		}
		items.push_back(std::move(it));
	}
	return items;
}

NetSelfTest::NetSelfTest(Worker& worker, const std::string& script, bool quitWhenDone)
	: worker_(worker), quit_(quitWhenDone), items_(parseNetTestScript(script)) {
	NTLOG("[nettest] script: %zu item(s), quit when done: %s", items_.size(), quit_ ? "yes" : "no");
}

void NetSelfTest::startJob(const Item& it, bool background) {
	auto stats = std::make_shared<Stats>();
	stats->label = "#" + std::to_string(++index_);
	stats->url = it.url;
	// Only the server's own pattern bodies can be verified byte for byte.
	stats->checkPattern = it.url.find("/len/") != std::string::npos || it.url.find("/slow/") != std::string::npos;
	stats->startMs = nowMs_;

	HttpRequest req;
	req.url = it.url;
	req.totalTimeoutMs = background ? 0 : 60000;
	Stats* raw = stats.get();
	auto sink = std::make_unique<StatsSink>(StatsSink::S{stats->checkPattern, &raw->bytes, &raw->crc, &raw->firstBad, &raw->head, &raw->status});
	// `stats` is captured by the job and the posted closure, so it outlives both even if the app shuts down mid-job.
	auto done = [stats](HttpSink&, const HttpResult& r, JobContext& ctx) {
		stats->result = r;
		ctx.post([stats] { stats->done = true; });
	};
	JobId id = worker_.submit(background ? JobClass::Background : JobClass::Catalog, "nettest", std::make_unique<HttpGetJob>(req, std::move(sink), done));
	NTLOG("[nettest] %s %s %s%s -> job %llu", stats->label.c_str(), background ? "BG" : "GET", it.url.c_str(),
	    it.kind == Item::Kind::Cancel ? " (will cancel)" : "", static_cast<unsigned long long>(id));
	if (id == 0) {
		stats->result.error = HttpError::Io;
		stats->result.detail = "submit rejected";
		stats->done = true;
	}
	if (background) {
		bg_.emplace_back(id, stats);
	} else {
		current_ = stats;
		currentId_ = id;
		itemStartMs_ = nowMs_;
		cancelAtMs_ = it.kind == Item::Kind::Cancel ? it.ms : -1;
		maxFrameMs_ = 0;
		frames_ = 0;
	}
}

void NetSelfTest::startNext() {
	while (next_ < items_.size() && !current_ && !pausing_ && !waitingBg_) {
		const Item& it = items_[next_++];
		switch (it.kind) {
		case Item::Kind::Get:
		case Item::Kind::Cancel: startJob(it, false); break;
		case Item::Kind::Bg: startJob(it, true); break;
		case Item::Kind::Pause:
			pausing_ = true;
			pauseUntil_ = nowMs_ + static_cast<uint32_t>(std::max(0, it.ms));
			break;
		case Item::Kind::WaitBg: waitingBg_ = true; break;
		}
	}
}

void NetSelfTest::logProgress() {
	for (auto& [id, st] : bg_) {
		if (st->done) continue;
		uint64_t rec = 0;
		int64_t total = -1;
		if (worker_.progress(id, rec, total)) NTLOG("[nettest] %s bg progress %llu / %lld bytes (t=%u ms)", st->label.c_str(), static_cast<unsigned long long>(rec), static_cast<long long>(total), nowMs_);
	}
	if (current_ && !current_->done) {
		uint64_t rec = 0;
		int64_t total = -1;
		if (worker_.progress(currentId_, rec, total)) NTLOG("[nettest] %s progress %llu / %lld bytes (t=%u ms)", current_->label.c_str(), static_cast<unsigned long long>(rec), static_cast<long long>(total), nowMs_);
	}
}

bool NetSelfTest::tick(float dt) {
	if (finished_ && bg_.empty()) return quit_;
	nowMs_ += static_cast<uint32_t>(dt * 1000.0f + 0.5f);
	maxFrameMs_ = std::max(maxFrameMs_, dt * 1000.0f);
	frames_++;
	if (!begun_) {
		begun_ = true;
		NTLOG("[nettest] starting");
	}

	if (nowMs_ - lastProgressMs_ >= 1000) {
		lastProgressMs_ = nowMs_;
		logProgress();
	}

	auto report = [&](const std::shared_ptr<Stats>& st, bool background) {
		st->endMs = nowMs_;
		const HttpResult& r = st->result;
		char crc[16];
		std::snprintf(crc, sizeof crc, "%08x", static_cast<unsigned>(~st->crc));
		NTLOG("[nettest] %s DONE %s: %s status=%d http=%d bytes=%llu crc=%s pattern=%s time=%u ms detail=\"%s\" final=%s head=\"%s\"",
		    st->label.c_str(), background ? "bg" : "get", errName(r.error), r.status, st->status, static_cast<unsigned long long>(st->bytes), crc,
		    !st->checkPattern || st->bytes == 0 ? "n/a" : st->firstBad < 0 ? "ok" : "BAD", st->endMs - st->startMs, r.detail.c_str(), r.finalUrl.c_str(), st->head.c_str());
		if (!background) NTLOG("[nettest] %s ui: %d frames, max frame %.1f ms while it ran; %u ms since the last cancel/start", st->label.c_str(), frames_, maxFrameMs_, nowMs_ - itemStartMs_);
	};

	if (current_) {
		if (cancelAtMs_ >= 0 && nowMs_ - itemStartMs_ >= static_cast<uint32_t>(cancelAtMs_)) {
			NTLOG("[nettest] %s cancelling now (t=%u ms)", current_->label.c_str(), nowMs_);
			worker_.cancel(currentId_);
			cancelAtMs_ = -1;
			itemStartMs_ = nowMs_;   // so the log below measures cancel-to-finish
		}
		if (current_->done) {
			report(current_, false);
			current_.reset();
		}
	}
	if (pausing_ && static_cast<int32_t>(nowMs_ - pauseUntil_) >= 0) pausing_ = false;
	for (auto it = bg_.begin(); it != bg_.end();) {
		if (it->second->done) {
			report(it->second, true);
			it = bg_.erase(it);
		} else {
			++it;
		}
	}
	if (waitingBg_ && bg_.empty()) waitingBg_ = false;

	if (finished_) return quit_;
	startNext();
	if (next_ >= items_.size() && !current_ && !pausing_ && !waitingBg_) {
		finished_ = true;
		NTLOG("[nettest] script finished (%zu bg job(s) still running)", bg_.size());
		return quit_;
	}
	return false;
}

}  // namespace net
