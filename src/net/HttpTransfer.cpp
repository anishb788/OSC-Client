#include "net/HttpTransfer.h"

#include <algorithm>
#include <cctype>

#include "core/Log.h"

namespace {
constexpr size_t kRecvBuf = 8 * 1024;
constexpr uint64_t kProgressIntervalMs = 100;
}

bool HostCache::find(const std::string& host, uint32_t& ipv4) const {
	for (const Entry& e : entries_) {
		if (e.host == host) {
			ipv4 = e.ipv4;
			return true;
		}
	}
	return false;
}

void HostCache::put(const std::string& host, uint32_t ipv4) {
	for (Entry& e : entries_) {
		if (e.host == host) {
			e.ipv4 = ipv4;
			return;
		}
	}
	if (entries_.size() < kMax) {
		entries_.push_back({host, ipv4});
	} else {
		entries_[next_] = {host, ipv4};   // oldest first
		next_ = (next_ + 1) % kMax;
	}
}

void HostCache::erase(const std::string& host) {
	for (size_t i = 0; i < entries_.size(); i++) {
		if (entries_[i].host == host) {
			entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
			if (next_ >= entries_.size()) next_ = 0;
			return;
		}
	}
}

HttpTransfer::HttpTransfer(const HttpRequest& req, HttpSink& sink, ProgressFn progress, CancelFlag cancel,
                           Transport& transport, HostCache* cache)
	: req_(req), sink_(sink), progress_(std::move(progress)), cancel_(cancel), tr_(transport), cache_(cache) {
	result_.finalUrl = req.url;
}

HttpTransfer::~HttpTransfer() {
	// The owner of the progress callback may already be gone.
	progress_ = nullptr;
	finish(HttpError::Cancelled, "abandoned");
}

void HttpTransfer::charge() {
	uint64_t now = tr_.nowMs();
	uint64_t delta = now > mark_ ? now - mark_ : 0;
	mark_ = now;
	if (phase_ == Phase::Connect) connectMs_ += delta;
	else if (phase_ == Phase::Send || phase_ == Phase::Receive) idleMs_ += delta;
}

bool HttpTransfer::timedOut(HttpError& err, std::string& detail) {
	err = HttpError::Timeout;
	if (req_.totalTimeoutMs > 0 && tr_.nowMs() - startMs_ >= static_cast<uint64_t>(req_.totalTimeoutMs)) {
		detail = "total timeout";
		return true;
	}
	if (phase_ == Phase::Connect && connectMs_ >= static_cast<uint64_t>(req_.connectTimeoutMs)) {
		detail = "connect timeout";
		return true;
	}
	if ((phase_ == Phase::Send || phase_ == Phase::Receive) && idleMs_ >= static_cast<uint64_t>(req_.idleTimeoutMs)) {
		detail = "idle timeout";
		return true;
	}
	return false;
}

// Waits no longer than what is left of the step, of the active timer and of the total timeout, so a wait can never
// overshoot a deadline by more than the transport's own granularity.
void HttpTransfer::waitFor(bool forWrite) {
	uint64_t now = tr_.nowMs();
	int64_t ms = static_cast<int64_t>(deadline_ > now ? deadline_ - now : 0);
	if (phase_ == Phase::Connect) {
		ms = std::min<int64_t>(ms, static_cast<int64_t>(req_.connectTimeoutMs) - static_cast<int64_t>(connectMs_));
	} else {
		ms = std::min<int64_t>(ms, static_cast<int64_t>(req_.idleTimeoutMs) - static_cast<int64_t>(idleMs_));
	}
	if (req_.totalTimeoutMs > 0) {
		int64_t left = static_cast<int64_t>(req_.totalTimeoutMs) - static_cast<int64_t>(now - startMs_);
		ms = std::min(ms, left);
	}
	tr_.wait(forWrite, static_cast<int>(std::max<int64_t>(ms, 1)));
}

void HttpTransfer::reportProgress(bool force) {
	if (!progress_ || !parser_) return;
	uint64_t bytes = parser_->bodyBytes();
	if (progressSent_ && bytes == lastProgressBytes_) return;
	uint64_t now = tr_.nowMs();
	if (!force && progressSent_ && now - lastProgressMs_ < kProgressIntervalMs) return;
	progressSent_ = true;
	lastProgressBytes_ = bytes;
	lastProgressMs_ = now;
	progress_(bytes, parser_->contentLength());
}

HttpTransfer::StepResult HttpTransfer::finish(HttpError err, const std::string& detail) {
	if (phase_ == Phase::Done) return StepResult::Done;
	reportProgress(err == HttpError::None);
	tr_.close();
	if (cache_ && (err == HttpError::Connect || (err == HttpError::Timeout && phase_ == Phase::Connect) ||
	               (err == HttpError::Io && phase_ == Phase::Send))) {
		cache_->erase(url_.host);
	}
	if (parser_) {
		if (parser_->status() > 0) result_.status = parser_->status();
		result_.bytesReceived = parser_->bodyBytes();
	}
	result_.error = err;
	result_.detail = detail;
	result_.finalUrl = httpparse::toString(url_);
	if (url_.host.empty()) result_.finalUrl = req_.url;
	phase_ = Phase::Done;
	sink_.onFinish(err == HttpError::None);
	if (err != HttpError::None) LOG("http %s failed: %s (%s)", result_.finalUrl.c_str(), describe(result_).c_str(), detail.c_str());
	return StepResult::Done;
}

// Extra headers may carry credentials meant for the original host, so only harmless ones follow a cross-host redirect.
void HttpTransfer::dropForeignHeaders() {
	auto keep = [](const std::string& name) {
		std::string n = name;
		for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return n == "user-agent" || n == "accept";
	};
	req_.headers.erase(std::remove_if(req_.headers.begin(), req_.headers.end(),
	                                  [&](const std::pair<std::string, std::string>& h) { return !keep(h.first); }),
	                   req_.headers.end());
}

// The parser reached Done. Returns true when the transfer is over (finish() was called), false after a redirect.
bool HttpTransfer::onResponseDone() {
	if (parser_->isRedirect()) {
		if (redirects_ >= req_.maxRedirects) {
			finish(HttpError::Redirects, "too many redirects");
			return true;
		}
		redirects_++;
		std::string next;
		HttpError e = httpparse::resolveLocation(url_, parser_->location(), next);
		httpparse::Url target;
		// Through parseUrl so an https redirect target reports TlsRequired instead of being fetched as http.
		if (e == HttpError::None) e = httpparse::parseUrl(next, target);
		if (e != HttpError::None) {
			finish(e, "bad redirect target: " + parser_->location());
			return true;
		}
		result_.status = parser_->status();
		if (target.host != url_.host || target.port != url_.port) dropForeignHeaders();
		url_ = target;
		parser_.reset();
		tr_.close();
		phase_ = Phase::Resolve;
		return false;
	}
	finish(parser_->error(), parser_->detail());
	return true;
}

HttpTransfer::StepResult HttpTransfer::step(const StepBudget& budget) {
	if (phase_ == Phase::Done) return StepResult::Done;

	uint64_t now = tr_.nowMs();
	if (phase_ == Phase::Start) {
		startMs_ = now;
		HttpError e = httpparse::parseUrl(req_.url, url_);
		if (e != HttpError::None) return finish(e, "bad url: " + req_.url);
		phase_ = Phase::Resolve;
	}
	// The timers only see time from here to the end of this call, never the pause before it.
	mark_ = now;
	deadline_ = now + static_cast<uint64_t>(std::max(budget.maxMs, 0));
	size_t bytesLeft = std::max<size_t>(budget.maxBytes, 1);
	uint8_t buf[kRecvBuf];

	auto pending = [this] {
		charge();
		return StepResult::Pending;
	};

	for (;;) {
		if (cancel_ && cancel_->load(std::memory_order_relaxed)) return finish(HttpError::Cancelled, "cancelled");
		charge();
		HttpError terr;
		std::string tdetail;
		if (timedOut(terr, tdetail)) return finish(terr, tdetail);

		switch (phase_) {
		case Phase::Start:
		case Phase::Done:
			return StepResult::Done;

		case Phase::Resolve: {
			if (!cache_ || !cache_->find(url_.host, addr_)) {
				uint64_t t0 = tr_.nowMs();
				HttpError e = tr_.resolve(url_.host, addr_);
				LOG("dns %s: %s in %llu ms", url_.host.c_str(), e == HttpError::None ? "ok" : "failed",
				    static_cast<unsigned long long>(tr_.nowMs() - t0));
				if (e != HttpError::None) return finish(e, "resolve " + url_.host);
				if (cache_) cache_->put(url_.host, addr_);
			}
			HttpError e = tr_.beginConnect(addr_, url_.port);
			if (e != HttpError::None) return finish(e == HttpError::NoNetwork || e == HttpError::Cancelled ? e : HttpError::Connect, "connect start");
			// Neither the blocking DNS lookup nor the connect call counts against the connect timer.
			mark_ = tr_.nowMs();
			connectMs_ = 0;
			phase_ = Phase::Connect;
			break;
		}

		case Phase::Connect:
			switch (tr_.checkConnected()) {
			case Transport::ConnectState::Connected:
				wire_ = httpparse::buildRequest(url_, req_);
				sent_ = 0;
				idleMs_ = 0;
				phase_ = Phase::Send;
				break;
			case Transport::ConnectState::Failed:
				return finish(HttpError::Connect, "connect failed");
			case Transport::ConnectState::InProgress:
				if (tr_.nowMs() >= deadline_) return pending();
				waitFor(true);
				break;
			}
			break;

		case Phase::Send: {
			Transport::IoResult r = tr_.send(reinterpret_cast<const uint8_t*>(wire_.data()) + sent_, wire_.size() - sent_);
			if (r.status == Transport::IoStatus::Ok && r.bytes == 0) r.status = Transport::IoStatus::WouldBlock;
			if (r.status == Transport::IoStatus::Ok) {
				sent_ += r.bytes;
				idleMs_ = 0;
				if (sent_ >= wire_.size()) {
					parser_ = std::make_unique<httpparse::ResponseParser>(sink_, req_.maxBody);
					phase_ = Phase::Receive;
				}
			} else if (r.status == Transport::IoStatus::WouldBlock) {
				if (tr_.nowMs() >= deadline_) return pending();
				waitFor(true);
			} else {
				if (sent_ == 0 && tr_.refusedOnSend()) return finish(HttpError::Connect, "connection refused (on the first send)");
				return finish(HttpError::Io, "send failed");
			}
			break;
		}

		case Phase::Receive: {
			if (bytesLeft == 0) return pending();
			Transport::IoResult r = tr_.recv(buf, std::min(bytesLeft, sizeof buf));
			if (r.status == Transport::IoStatus::Ok && r.bytes == 0) r.status = Transport::IoStatus::WouldBlock;
			httpparse::ResponseParser::Status st = httpparse::ResponseParser::Status::NeedMore;
			if (r.status == Transport::IoStatus::Ok) {
				idleMs_ = 0;
				bytesLeft -= std::min(bytesLeft, r.bytes);
				st = parser_->feed(buf, r.bytes);
				if (st == httpparse::ResponseParser::Status::NeedMore) reportProgress(false);
				// Sink and progress time is the app's, not the network's, so it must not count as idle.
				mark_ = tr_.nowMs();
			} else if (r.status == Transport::IoStatus::WouldBlock) {
				if (tr_.nowMs() >= deadline_) return pending();
				waitFor(false);
				break;
			} else if (r.status == Transport::IoStatus::Closed) {
				st = parser_->finishEof();
			} else {
				return finish(HttpError::Io, "recv failed");
			}
			if (st == httpparse::ResponseParser::Status::Failed) return finish(parser_->error(), parser_->detail());
			if (st == httpparse::ResponseParser::Status::Done) {
				if (onResponseDone()) return StepResult::Done;
				break;
			}
			if (r.status == Transport::IoStatus::Ok && tr_.nowMs() >= deadline_) return pending();
			break;
		}
		}
	}
}

HttpResult httpGet(const HttpRequest& req, HttpSink& sink, ProgressFn progress, CancelFlag cancel, Transport& transport,
                   HostCache* cache) {
	HttpTransfer t(req, sink, std::move(progress), cancel, transport, cache);
	StepBudget budget;
	while (t.step(budget) == HttpTransfer::StepResult::Pending) {}
	return t.result();
}
