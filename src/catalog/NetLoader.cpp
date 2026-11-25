#include "catalog/NetLoader.h"

#include <atomic>
#include <chrono>
#include <ctime>
#include <optional>

#include "catalog/OscApi.h"
#include "core/Log.h"
#include "net/Worker.h"

namespace {
using namespace net;

// Where the job is, for the status line. Written by the worker, read by the main thread.
enum Stage : int { kInfo = 0, kContents = 1, kFeatured = 2, kParsing = 3, kFinished = 4 };

// Shared by the loader and the job (shared_ptr), so a job that outlives its loader (cancelled, then still finishing a
// parse) never writes into freed memory. The result fields are written by the worker before it posts `delivered`'s
// closure and read by the main thread only after that closure ran (the post is the hand-over).
struct LoadState {
	std::atomic<int> stage{kInfo};
	bool delivered = false; // main thread only
	bool ok = false;
	std::string error;
	Catalog catalog;
};

double msSince(std::chrono::steady_clock::time_point t) {
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

class LoadCatalogJob : public Job {
public:
	LoadCatalogJob(std::shared_ptr<LoadState> st, const NetLoaderConfig& cfg) : st_(std::move(st)), base_(cfg.baseUrl), totalMs_(cfg.totalTimeoutMs) {
		start_ = std::chrono::steady_clock::now();
	}

	JobStep step(JobContext& ctx) override {
		if (!xfer_) {
			if (ctx.cancelled()) return finish(ctx, false, "Cancelled.");
			startTransfer(ctx);
		}
		// Cancelled transfers are stepped too: the transfer sees the flag itself and ends through its normal path.
		if (xfer_->step(ctx.budget) == HttpTransfer::StepResult::Pending) return JobStep::Pending;
		return onTransferDone(ctx);
	}

private:
	static constexpr const char* kNames[3] = {"/information", "/contents", "/featured-app"};

	void startTransfer(JobContext& ctx) {
		static constexpr uint64_t kLimits[3] = {oscapi::kMaxInformationBytes, oscapi::kMaxContentsBytes, oscapi::kMaxFeaturedBytes};
		const int s = st_->stage.load();
		HttpRequest req;
		req.url = base_ + kNames[s];
		req.maxBody = kLimits[s]; // the same bound OscApi enforces, so an oversized body is never buffered
		req.totalTimeoutMs = totalMs_;
		req.headers.emplace_back("Accept", "application/json");
		sink_ = std::make_unique<MemorySink>(req.maxBody);
		JobProgress* prog = &ctx.progress;
		prog->set(0, -1);
		fetchStart_ = std::chrono::steady_clock::now();
		// The same Transport and HostCache carry all three requests (DNS runs once).
		xfer_ = std::make_unique<HttpTransfer>(req, *sink_,
		                                       [prog](uint64_t received, int64_t total) { prog->set(received, total); },
		                                       ctx.cancel, ctx.transport, &ctx.hosts);
	}

	JobStep onTransferDone(JobContext& ctx) {
		const int s = st_->stage.load();
		const HttpResult r = xfer_->result();
		const bool ok = r.ok() && sink_->finishedOk();
		LOG("Catalog: GET %s -> %s in %.0f ms, %llu bytes%s%s", kNames[s], ok ? "ok" : describe(r).c_str(),
		    msSince(fetchStart_), (unsigned long long)r.bytesReceived, r.detail.empty() ? "" : " | ", r.detail.c_str());
		std::string body;
		if (ok) body = std::move(sink_->body());
		xfer_.reset();
		sink_.reset();

		if (r.error == HttpError::Cancelled || ctx.cancelled()) return finish(ctx, false, "Cancelled.");
		if (!ok) {
			// /featured-app is always optional, and /information is optional when the server answered with something
			// unusable (an HTTP error status, an oversized or unparseable body): the fallbacks exist for that. Anything
			// else on /information is a transport problem (no network, DNS, connect, timeout, a dropped connection, too
			// many redirects) that would just repeat on /contents, each repeat costing up to a full timeout, so it is
			// fatal at once.
			const bool dataProblem = r.error == HttpError::HttpStatus || r.error == HttpError::TooLarge || r.error == HttpError::Parse;
			const bool soft = s == kFeatured || (s == kInfo && dataProblem);
			if (!soft) return finish(ctx, false, describe(r));
			LOG("Catalog: %s skipped (the fallback is used)", kNames[s]);
		} else if (s == kInfo) {
			info_ = std::move(body);
		} else if (s == kContents) {
			contents_ = std::move(body);
		} else {
			featured_ = std::move(body);
		}
		if (s == kFeatured) return parse(ctx);
		st_->stage.store(s + 1);
		return JobStep::Pending;
	}

	JobStep parse(JobContext& ctx) {
		st_->stage.store(kParsing);
		const double fetchMs = msSince(start_);
		oscapi::BuildResult b = oscapi::buildCatalog(info_ ? std::optional<std::string_view>(*info_) : std::nullopt,
		                                             *contents_, featured_ ? std::optional<std::string_view>(*featured_) : std::nullopt);
		LOG("Catalog: %d apps, fetched in %.0f ms, parsed in %.1f ms (peak about %zu KB)", (int)b.catalog.apps.size(), fetchMs,
		    b.stats.parseMs, b.stats.peakBytes() / 1024);
		if (!b.ok()) return finish(ctx, false, b.error.message);
		b.catalog.fetchedAt = (int64_t)std::time(nullptr);
		st_->catalog = std::move(b.catalog);
		return finish(ctx, true, {});
	}

	JobStep finish(JobContext& ctx, bool ok, std::string error) {
		st_->ok = ok;
		st_->error = std::move(error);
		st_->stage.store(kFinished);
		std::shared_ptr<LoadState> st = st_;
		ctx.post([st] { st->delivered = true; });
		return JobStep::Done;
	}

	std::shared_ptr<LoadState> st_;
	std::string base_;
	int totalMs_;
	std::chrono::steady_clock::time_point start_, fetchStart_;
	// sink_ before xfer_: an abandoned transfer's destructor still calls sink_->onFinish.
	std::unique_ptr<MemorySink> sink_;
	std::unique_ptr<HttpTransfer> xfer_;
	std::optional<std::string> info_, featured_, contents_;
};

class NetLoader : public CatalogLoader {
public:
	NetLoader(Worker& w, const NetLoaderConfig& cfg) : worker_(w), st_(std::make_shared<LoadState>()) {
		id_ = w.submit(JobClass::Catalog, "catalog", std::make_unique<LoadCatalogJob>(st_, cfg));
		if (!id_) LOG("ERROR: the catalog job was rejected (the net worker is not running)");
	}
	~NetLoader() override {
		if (id_ && !st_->delivered) worker_.cancel(id_);
	}

	LoadStatus poll() override {
		if (!id_) return {LoadStatus::State::Failed, "Couldn't start the network worker."};
		if (taken_) return {LoadStatus::State::Failed, "Internal error: the catalog was already handed over."};
		if (st_->delivered) {
			if (st_->ok) return {LoadStatus::State::Done, {}, /*fromNetwork=*/true};
			return {LoadStatus::State::Failed, st_->error};
		}
		uint64_t received = 0;
		int64_t total = -1;
		const int s = st_->stage.load();
		if (s == kContents && worker_.progress(id_, received, total) && received > 0)
			return {LoadStatus::State::Pending, "Downloading catalog… " + std::to_string((received + 512) / 1024) + " KB"};
		if (s == kFeatured) return {LoadStatus::State::Pending, "Downloading catalog…"};
		if (s >= kParsing) return {LoadStatus::State::Pending, "Reading the catalog…"};
		return {LoadStatus::State::Pending, "Connecting…"};
	}

	// Only valid once, after poll() said Done; a second call gets an empty catalog and poll() reports Failed from then on.
	Catalog take() override {
		if (taken_) LOG("ERROR: the catalog was taken twice");
		taken_ = true;
		return std::move(st_->catalog);
	}

private:
	bool taken_ = false;
	Worker& worker_;
	std::shared_ptr<LoadState> st_;
	JobId id_ = 0;
};

class FailedLoader : public CatalogLoader {
public:
	explicit FailedLoader(std::string text) : text_(std::move(text)) {}
	LoadStatus poll() override { return {LoadStatus::State::Failed, text_}; }
	Catalog take() override { return {}; }

private:
	std::string text_;
};
} // namespace

std::unique_ptr<CatalogLoader> makeNetworkLoader(net::Worker* worker, const NetLoaderConfig& cfg) {
	if (!worker || !worker->running()) {
		LOG("ERROR: no running net worker");
		return std::make_unique<FailedLoader>("Couldn't start the network worker.");
	}
	return std::make_unique<NetLoader>(*worker, cfg);
}

std::unique_ptr<CatalogLoader> makeFailedLoader(const std::string& text) { return std::make_unique<FailedLoader>(text); }
