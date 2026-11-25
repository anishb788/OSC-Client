#include "catalog/NetLoader.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <optional>

#include "catalog/CatalogCache.h"
#include "catalog/OscApi.h"
#include "core/Log.h"
#include "core/Version.h"
#include "net/Worker.h"

namespace {
using namespace net;

// Where the job is, for the status line. Written by the worker, read by the main thread.
enum Stage : int { kInfo = 0, kContents = 1, kFeatured = 2, kParsing = 3, kFinished = 4, kReadCache = 5 };

// Shared by the loader and the job (shared_ptr), so a job that outlives its loader (cancelled, then still finishing a
// parse) never writes into freed memory. The result fields are written by the worker before it posts `delivered`'s
// closure and read by the main thread only after that closure ran (the post is the hand-over).
struct LoadState {
	std::atomic<int> stage{kInfo};
	bool delivered = false; // main thread only
	bool ok = false;
	bool fromNetwork = true; // false when the catalog was read from the SD cache
	std::string error;
	Catalog catalog;
};

double msSince(std::chrono::steady_clock::time_point t) {
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

class LoadCatalogJob : public Job {
public:
	LoadCatalogJob(std::shared_ptr<LoadState> st, const NetLoaderConfig& cfg)
	    : st_(std::move(st)), base_(cfg.baseUrl), totalMs_(cfg.totalTimeoutMs), cfg_(cfg) {
		start_ = std::chrono::steady_clock::now();
		if (!cfg_.cacheDir.empty()) cache_ = std::make_unique<catcache::Cache>(cfg_.cacheDir, cfg_.fileOps);
		if (cfg_.appVersion.empty()) cfg_.appVersion = OPENSHOP_VERSION;
		// The status line says "Reading cache…" from the first frame, unless a fetch follows regardless (a Refresh): then
		// the manifest read is a detail and "Connecting…" is the honest text.
		if (cache_ && readsCacheFirst()) st_->stage.store(kReadCache);
	}

	// Cache and parse work runs inside one step() each (SD reads and writes and jansson cannot be sliced); each logs
	// its duration. The job is of class Catalog, so only Background downloads wait for it.
	JobStep step(JobContext& ctx) override {
		if (phase_ == Phase::Plan) return plan(ctx);
		if (phase_ == Phase::WriteCache) return writeCache();
		if (!xfer_) {
			if (ctx.cancelled()) return finish(ctx, false, "Cancelled.");
			startTransfer(ctx);
		}
		// Cancelled transfers are stepped too: the transfer sees the flag itself and ends through its normal path.
		if (xfer_->step(ctx.budget) == HttpTransfer::StepResult::Pending) return JobStep::Pending;
		return onTransferDone(ctx);
	}

private:
	enum class Phase { Plan, Net, WriteCache };
	static constexpr const char* kNoCacheText = "There is no saved catalog on the SD card, and the network is switched off.";

	// A Refresh (without --offline) always fetches, so nothing is shown as "Reading cache…" for it.
	bool readsCacheFirst() const { return !cfg_.refreshMode || cfg_.offline; }

	int64_t nowSec() const { return cfg_.now ? cfg_.now() : (int64_t)std::time(nullptr); }

	// Decides between the cache and the network (catcache::decideSource) and acts on it.
	JobStep plan(JobContext& ctx) {
		if (ctx.cancelled()) return finish(ctx, false, "Cancelled.");
		catcache::Source src = catcache::Source::Fetch;
		if (cache_) {
			if (readsCacheFirst()) st_->stage.store(kReadCache);
			const auto t0 = std::chrono::steady_clock::now();
			catcache::Meta m;
			const auto rs = cache_->readMeta(m);
			if (rs == catcache::Cache::Status::Ok) {
				meta_ = std::move(m);
			} else if (rs == catcache::Cache::Status::Corrupt) {
				LOG("Catalog cache: the manifest is damaged, deleting the cache");
				cache_->invalidate();
			} else if (rs == catcache::Cache::Status::Unreadable) {
				// An I/O error says nothing about the data: treated as missing this time, and nothing is deleted.
				LOG("Catalog cache: the manifest could not be read (I/O error), keeping the cache");
				unreadable_ = true;
			}
			catcache::Policy pol = policy();
			src = catcache::decideSource(meta_, nowSec(), pol);
			std::string have = rs == catcache::Cache::Status::Missing ? "none" : unreadable_ ? "unreadable" : "unusable";
			if (meta_)
				have = "generation " + std::to_string(meta_->generation) + ", age " + std::to_string(nowSec() - meta_->fetchedAt) +
				       " s, ttl " + std::to_string(pol.ttlSec) + " s";
			LOG("Catalog cache: %s, %s -> %s (manifest read in %.0f ms)", have.c_str(),
			    cfg_.refreshMode ? "refresh" : cfg_.offline ? "offline" : cfg_.ignoreTtl ? "forced" : "startup",
			    catcache::sourceName(src), msSince(t0));
		} else if (cfg_.offline) {
			src = catcache::Source::Fail;
		}
		switch (src) {
		case catcache::Source::UseCache:
			if (cfg_.offline) LOG("Catalog cache: offline, reading the cache only (no network)");
			else LOG("Catalog cache: cache fresh, no fetch");
			if (loadFromCache(/*offline=*/cfg_.offline)) return finish(ctx, true, {});
			if (cfg_.offline) {
				return finish(ctx, false, unreadable_ ? "The saved catalog on the SD card couldn't be read just now. Try again."
				                                      : "The saved catalog on the SD card couldn't be read. It was damaged, so it has been deleted.");
			}
			break; // a damaged fresh cache is a miss: fetch, with nothing to fall back to
		case catcache::Source::FetchWithFallback: fallback_ = true; break;
		case catcache::Source::Fetch: break;
		case catcache::Source::Fail:
			return finish(ctx, false, unreadable_ ? "The saved catalog on the SD card couldn't be read just now. Try again." : kNoCacheText);
		}
		phase_ = Phase::Net;
		st_->stage.store(kInfo);
		start_ = std::chrono::steady_clock::now();
		return JobStep::Pending;
	}

	catcache::Policy policy() const {
		catcache::Policy p;
		p.appVersion = cfg_.appVersion;
		p.baseUrl = base_;
		p.ttlSec = cfg_.ttlSec;
		p.offline = cfg_.offline;
		p.ignoreTtl = cfg_.ignoreTtl;
		p.refreshMode = cfg_.refreshMode;
		return p;
	}

	// Builds the catalog from the cache into st_->catalog. A cache that does not add up (hash, size, parse) is deleted
	// and counts as missing: the caller fetches, or reports the error. A read that failed with an I/O error is also a
	// miss, but the files stay (the card may be fine next time).
	bool loadFromCache(bool offline) {
		st_->stage.store(kReadCache);
		const auto t0 = std::chrono::steady_clock::now();
		catcache::Bodies b;
		if (!meta_) return false;
		const auto rs = cache_->readBodies(*meta_, b);
		if (rs == catcache::Cache::Status::Unreadable) {
			LOG("Catalog cache: a file could not be read (I/O error), not using the cache this time");
			unreadable_ = true;
			meta_.reset();
			return false;
		}
		if (rs != catcache::Cache::Status::Ok) return dropCache("a file is missing or does not match the manifest");
		size_t bytes = 0;
		for (const auto* body : {&b.information, &b.contents, &b.featured}) if (*body) bytes += (*body)->size();
		const double readMs = msSince(t0);
		oscapi::BuildResult r = oscapi::buildCatalog(b.information ? std::optional<std::string_view>(*b.information) : std::nullopt,
		                                             *b.contents, b.featured ? std::optional<std::string_view>(*b.featured) : std::nullopt);
		if (!r.ok()) return dropCache(r.error.message.c_str());
		LOG("Catalog cache: read %zu KB in %.0f ms, %d apps, parsed in %.1f ms, fetched at %lld", (bytes + 512) / 1024, readMs,
		    (int)r.catalog.apps.size(), r.stats.parseMs, (long long)meta_->fetchedAt);
		r.catalog.fetchedAt = meta_->fetchedAt;
		r.catalog.offline = offline;
		st_->fromNetwork = false;
		st_->catalog = std::move(r.catalog);
		return true;
	}

	bool dropCache(const char* why) {
		LOG("Catalog cache: unusable (%s), deleting it", why);
		cache_->invalidate();
		meta_.reset();
		return false;
	}

	// A fetch (or its parse) failed. A startup with a stale cache shows that cache, flagged offline; a Refresh never does.
	JobStep failNet(JobContext& ctx, const std::string& message) {
		if (cache_ && cfg_.refreshMode) {
			LOG("Catalog cache: the refresh failed, the stale cache is not used");
		} else if (cache_ && fallback_ && !ctx.cancelled()) {
			LOG("Catalog cache: the network failed (%s), using the stale cache", message.c_str());
			if (loadFromCache(/*offline=*/true)) return finish(ctx, true, {});
		}
		return finish(ctx, false, message);
	}

	JobStep writeCache() {
		catcache::Bodies b;
		b.information = std::move(info_);
		b.contents = std::move(contents_);
		b.featured = std::move(featured_);
		// Runs to the end even when the job was cancelled meanwhile (stop(), the loader destroyed): the cache then never
		// holds half a write, and the next write would clean up either way.
		cache_->write(b, base_, cfg_.appVersion, fetchedAt_);
		return JobStep::Done;
	}

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
			if (!soft) return failNet(ctx, describe(r));
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
		if (!b.ok()) return failNet(ctx, b.error.message);
		// time() can fail (-1). 0 means "unknown": never fresh, still good as a fallback.
		fetchedAt_ = std::max<int64_t>(0, nowSec());
		b.catalog.fetchedAt = fetchedAt_;
		st_->fromNetwork = true;
		st_->catalog = std::move(b.catalog);
		// The catalog goes to the main thread first; the cache write (slow on a Wii's SD card) follows in the next
		// step, so Home does not wait for it. The cache is only written after a successful parse.
		deliver(ctx, true, {});
		if (!cache_) return JobStep::Done;
		phase_ = Phase::WriteCache;
		return JobStep::Pending;
	}

	void deliver(JobContext& ctx, bool ok, std::string error) {
		st_->ok = ok;
		st_->error = std::move(error);
		st_->stage.store(kFinished);
		std::shared_ptr<LoadState> st = st_;
		ctx.post([st] { st->delivered = true; });
	}

	JobStep finish(JobContext& ctx, bool ok, std::string error) {
		deliver(ctx, ok, std::move(error));
		return JobStep::Done;
	}

	std::shared_ptr<LoadState> st_;
	std::string base_;
	int totalMs_;
	NetLoaderConfig cfg_;
	Phase phase_ = Phase::Plan;
	std::unique_ptr<catcache::Cache> cache_;
	std::optional<catcache::Meta> meta_; // the manifest read at the start, if it was usable
	bool unreadable_ = false;            // the cache could not be read (I/O error); it was left alone
	bool fallback_ = false;              // a failed fetch may use the stale cache
	int64_t fetchedAt_ = 0;
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
			if (st_->ok) return {LoadStatus::State::Done, {}, st_->fromNetwork};
			return {LoadStatus::State::Failed, st_->error};
		}
		uint64_t received = 0;
		int64_t total = -1;
		const int s = st_->stage.load();
		if (s == kReadCache) return {LoadStatus::State::Pending, "Reading cache…"};
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
