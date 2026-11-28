// SPDX-License-Identifier: GPL-3.0-or-later
#include "install/Installer.h"

#include <algorithm>
#include <cstdio>

#include <SDL_mutex.h>
#include <SDL_timer.h>

#include "core/Log.h"
#include "install/Commit.h"
#include "install/Journal.h"
#include "install/Md5Sink.h"
#include "net/NetState.h"
#include "net/Worker.h"

namespace install {

namespace {

constexpr uint32_t kWaitSliceMs = 100; // how often the runner looks at cancel, progress and the net job while it waits

// The copy of the download's result that the net worker hands to the runner (nothing in it points into the job or the sink).
struct DownloadShared {
	struct Mutex {
		SDL_mutex* m = SDL_CreateMutex();
		SDL_cond* c = SDL_CreateCond();
		~Mutex() {
			if (c) SDL_DestroyCond(c);
			if (m) SDL_DestroyMutex(m);
		}
	} sync;
	bool done = false;      // guarded by sync.m
	HttpResult http;
	std::string digest;     // the MD5 of what arrived (32 hex), "" unless the transfer ended ok
	uint64_t bytes = 0;     // body bytes that went through the sink
	bool committed = false; // FileSink renamed download.zip.tmp to download.zip
};

struct Lock {
	explicit Lock(SDL_mutex* m) : m_(m) { SDL_LockMutex(m_); }
	~Lock() { SDL_UnlockMutex(m_); }
	Lock(const Lock&) = delete;
	Lock& operator=(const Lock&) = delete;
	SDL_mutex* m_;
};

TaskOutcome failed(InstallError e, Device d = Device::None) { return {JobState::Failed, installErrorText(e, d)}; }

uint32_t nowOf(const Installer::Config& c) { return c.nowMs ? c.nowMs() : static_cast<uint32_t>(SDL_GetTicks()); }

// What one download attempt came to.
// Rejected: Worker::submit() returned 0 (the net worker is not running or is stopping), so nothing was sent and the network was not asked.
enum class Attempt { Finished, Vanished, Rejected };

Attempt downloadOnce(const Installer::Config& c, const JobSpec& spec, RunCtx& ctx, const std::string& zipPath,
                     std::shared_ptr<DownloadShared>& sh) {
	// FileSink writes <zipPath>.tmp and commit() renames it; a retry opens (truncates) the tmp again, the previous attempt's sink
	// already removed it.
	auto file = std::make_unique<FileSink>(zipPath);
	file->setFileOps(c.fileOps);
	auto sink = std::make_unique<Md5Sink>(std::move(file), spec.archiveSize);

	HttpRequest req;
	req.url = spec.url;
	req.maxBody = 0;           // unlimited at the client: Md5Sink holds the body to archiveSize
	req.totalTimeoutMs = 0;    // the 10 s idle timer is the bound; archives are long
	req.idleTimeoutMs = 10000;
	req.connectTimeoutMs = 8000;
	req.maxRedirects = 0;      // archive URLs never redirect: a 3xx fails with Redirects and the target is never contacted

	sh = std::make_shared<DownloadShared>();
	std::shared_ptr<DownloadShared> shared = sh;
	auto onDone = [shared](HttpSink& s, const HttpResult& r, net::JobContext&) {
		// Net worker thread, the sink still alive. Commit the file here (a rename), then publish a copy.
		Md5Sink& m = static_cast<Md5Sink&>(s);
		bool committed = false;
		if (r.ok()) committed = static_cast<FileSink&>(m.inner()).commit();
		Lock l(shared->sync.m);
		shared->http = r;
		shared->digest = m.digestHex();
		shared->bytes = m.bytes();
		shared->committed = committed;
		shared->done = true;
		SDL_CondSignal(shared->sync.c);
	};
	const net::JobId nid = c.worker->submit(net::JobClass::Background, "install:" + spec.slug,
	                                        std::make_unique<net::HttpGetJob>(std::move(req), std::move(sink), std::move(onDone)));
	if (nid == 0) return Attempt::Rejected; // not a network result: no retry, and the network state is left alone

	bool cancelSent = false;
	uint32_t cancelAt = 0;
	for (;;) {
		{
			Lock l(sh->sync.m);
			if (!sh->done) SDL_CondWaitTimeout(sh->sync.c, sh->sync.m, kWaitSliceMs);
			if (sh->done) return Attempt::Finished;
		}
		uint64_t rec = 0;
		int64_t tot = -1;
		if (c.worker->progress(nid, rec, tot)) {
			ctx.cell.setProgress(rec, spec.archiveSize);
		} else {
			// The job left the worker without its onDone (the worker stopped and destroyed it). Look once more under the lock: onDone
			// runs before the entry is removed, so a finished job has set `done` already.
			Lock l(sh->sync.m);
			if (sh->done) return Attempt::Finished;
			return Attempt::Vanished;
		}
		if (ctx.cell.cancelled()) {
			if (!cancelSent) {
				cancelSent = true;
				cancelAt = nowOf(c);
				c.worker->cancel(nid);
			} else if (nowOf(c) - cancelAt > c.cancelWaitMs) {
				LOG("install %s: the net worker did not release the cancelled transfer within %u ms", spec.slug.c_str(), c.cancelWaitMs);
				return Attempt::Vanished;
			}
		}
	}
}

// Removes the job's staging dir (guarded: only under apps/OpenShop/staging) and then its start-phase journal, so the next job on this
// root is not blocked by journalBlocker. A staging dir that cannot be removed is left for the Cleanup job (a staging dir with no
// journal is abandoned); the journal goes either way.
void scrub(const Installer::Config& c, const std::string& root, int64_t jobDirId) {
	const std::string stage = stagingAbs(root, jobDirId);
	const RemoveResult rr = removeTree(stage, stagingBase(root), c.removeRoots, c.fileOps);
	if (!rr.ok) LOG("install: staging %s was not fully removed (%s); the Cleanup job retries it", stage.c_str(), rr.error.c_str());
	if (!removeJournal(root, c.fileOps)) LOG("install: the start journal under %s could not be removed", root.c_str());
}

// Scrubs on EVERY exit of runInstall from the moment the staging dir is claimed (success, failure, cancel, early return), so no path can
// forget it. Disarmed only by release(): the explicit hand-over to the commit (Increment 8a), after which the commit's own cleanup
// owns staging and the journal.
class ScrubGuard {
public:
	ScrubGuard(const Installer::Config& c, std::string root, int64_t id) : c_(c), root_(std::move(root)), id_(id) {}
	~ScrubGuard() {
		if (armed_) scrub(c_, root_, id_);
	}
	ScrubGuard(const ScrubGuard&) = delete;
	ScrubGuard& operator=(const ScrubGuard&) = delete;
	void release() { armed_ = false; } // Increment 8a: only at the commit hand-over, after enterCommit()
private:
	const Installer::Config& c_;
	std::string root_;
	int64_t id_;
	bool armed_ = true;
};

TaskOutcome runInstall(const Installer::Config& c, const JobSpec& spec, RunCtx& ctx) {
	ctx.cell.setStage(Stage::Checking);
	const std::optional<Device> dev = parseDeviceId(spec.device);
	const Device device = dev ? *dev : Device::None;
#if OPENSHOP_INSTALLER_TESTHOOKS
	PreflightEnv env = c.preflight;
#else
	PreflightEnv env; // release: the real checks only
#endif
	env.net = net::netState(nowOf(c));
	const PreflightResult pf = preflight(spec, device, env);
	if (!pf.ok()) {
		LOG("install %s: pre-flight refused at job start: %s", spec.slug.c_str(), pf.message.c_str());
		return {JobState::Failed, pf.message};
	}
	if (!c.worker) return failed(InstallError::Unavailable);
	const std::string& root = spec.targetRoot;

	// The staging dir: a fresh id (never one that exists), CLAIMED before the dir exists and before the journal is written.
	const int64_t id = freshJobId(root, ctx.id, c.fileOps);
	if (id < 0) return failed(InstallError::WriteFailed, device);
	ctx.claimStaging(root, id);
	ScrubGuard guard(c, root, id); // every return below removes the staging dir and then the start journal
	const std::string stage = stagingAbs(root, id);
	const std::string zipPath = stage + "/download.zip";

	if (!writeStartJournal(root, spec.device, id, JournalKind::Install, spec.slug, c.fileOps)) {
		return failed(InstallError::WriteFailed, device);
	}
	// (A crash between the journal write and makeDirs leaves a journal with no staging dir: Increment 6's recovery covers it.)
	if (!makeDirs(stage, c.fileOps)) {
		return failed(InstallError::WriteFailed, device);
	}
	LOG("install %s: job %u, staging %s, %s over plain http into download.zip", spec.slug.c_str(), ctx.id, stage.c_str(), spec.url.c_str());
#if OPENSHOP_INSTALLER_TESTHOOKS
	if (c.beforeDownload) c.beforeDownload(stage);
#endif

	// ---- download (net worker), with one automatic retry from zero ----
	ctx.cell.setStage(Stage::Downloading);
	ctx.cell.setProgress(0, spec.archiveSize);
	std::shared_ptr<DownloadShared> sh;
	for (int attempt = 0;; attempt++) {
		if (ctx.cell.cancelled()) return {JobState::Cancelled, {}};
		const Attempt a = downloadOnce(c, spec, ctx, zipPath, sh);
		if (a == Attempt::Rejected) return failed(InstallError::Unavailable);
		if (a == Attempt::Vanished) {
			if (ctx.cell.cancelled()) return {JobState::Cancelled, {}};
			return failed(InstallError::Unavailable);
		}
		HttpResult http;
		{
			Lock l(sh->sync.m);
			http = sh->http;
		}
		if (http.error == HttpError::NoNetwork) net::setNetState(net::NetState::Down, nowOf(c));
		else if (http.ok() || http.error == HttpError::HttpStatus || http.error == HttpError::Redirects || http.error == HttpError::TooLarge)
			net::setNetState(net::NetState::Up, nowOf(c)); // the server answered
		if (http.ok()) break;
		if (http.error == HttpError::Cancelled || ctx.cell.cancelled()) {
			return {JobState::Cancelled, {}};
		}
		LOG("install %s: download failed (%d, status %d): %s", spec.slug.c_str(), static_cast<int>(http.error), http.status, http.detail.c_str());
		if (attempt == 0 && isRetryable(http.error, http.status)) {
			LOG("install %s: one automatic retry from zero in %u ms", spec.slug.c_str(), c.retryDelayMs);
			ctx.cell.setProgress(0, spec.archiveSize);
			for (uint32_t t = 0; t < c.retryDelayMs && !ctx.cell.cancelled();) {
				const uint32_t step = std::min<uint32_t>(50, c.retryDelayMs - t);
				SDL_Delay(step);
				t += step;
			}
			continue;
		}
		return {JobState::Failed, downloadErrorText(http.error, http.status)};
	}

	// ---- verify (install worker): the byte count and the digest against what the catalog says ----
	ctx.cell.setStage(Stage::Verifying);
	std::string digest;
	uint64_t bytes = 0;
	bool committed = false;
	{
		Lock l(sh->sync.m);
		digest = sh->digest;
		bytes = sh->bytes;
		committed = sh->committed;
	}
	if (!committed) {
		LOG("install %s: the finished download could not be moved into place", spec.slug.c_str());
		return failed(InstallError::WriteFailed, device);
	}
	FileStat st;
	const Presence pres = presenceOf(zipPath, c.fileOps, &st);
	const bool onDisk = pres == Presence::Present && !st.isDir && st.size == spec.archiveSize;
	if (bytes != spec.archiveSize || !sameHex(digest, spec.md5) || !onDisk) {
		LOG("install %s: damaged download: %llu of %llu bytes, md5 %s, expected %s, on disk %s", spec.slug.c_str(), (unsigned long long)bytes,
		    (unsigned long long)spec.archiveSize, digest.c_str(), spec.md5.c_str(), onDisk ? "yes" : "no");
		// Delete the file (the guard removes the whole staging dir anyway; this is the explicit step the plan names).
		const int rm = c.fileOps.remove ? c.fileOps.remove(zipPath.c_str()) : std::remove(zipPath.c_str());
		if (rm != 0) LOG("install %s: could not delete the damaged download (the staging removal on return retries)", spec.slug.c_str());
		return failed(InstallError::Damaged);
	}
	LOG("install %s: download complete, %llu bytes, md5 matches the catalog's; extraction is not wired yet (Increment 8a): verified, extraction pending",
	    spec.slug.c_str(), (unsigned long long)bytes);
#if OPENSHOP_INSTALLER_TESTHOOKS
	if (c.onVerified) c.onVerified(zipPath);
#endif
	const bool wasCancelled = ctx.cell.cancelled(); // (Increment 8a replaces this tail: enterCommit(), then guard.release() at the hand-over)
	if (wasCancelled) return {JobState::Cancelled, {}};
	return {};
}

} // namespace

JobSpec makeInstallSpec(const AppEntry& app, const std::string& root, const std::string& device, bool flipMd5) {
	JobSpec s;
	s.slug = app.slug;
	s.name = app.name;
	s.version = app.version;
	s.url = app.archiveUrl;
	s.md5 = app.archiveMd5;
	if (flipMd5 && !s.md5.empty()) s.md5[0] = s.md5[0] == '0' ? '1' : '0'; // OPENSHOP_TEST_BAD_MD5: a hash the archive can't match
	s.archiveSize = app.archiveSize;
	s.uncompressedSize = app.uncompressedSize;
	s.packageType = static_cast<uint8_t>(app.packageType);
	s.dirs = app.subdirectories;
	s.targetRoot = root;
	s.device = device;
	return s;
}

PreflightResult queueTimeCheck(const JobSpec& spec, Device device, uint32_t nowMs, const PreflightEnv& env) {
	PreflightEnv e = env;
	e.net = net::netState(nowMs); // Unknown passes; only a known-down network refuses here
	return preflight(spec, device, e);
}

Installer::Installer(Config cfg) : cfg_(std::make_shared<const Config>(std::move(cfg))) {}

Runner Installer::runner() const {
	std::shared_ptr<const Config> c = cfg_;
	return [c](const JobSpec& spec, RunCtx& ctx) { return runInstall(*c, spec, ctx); };
}

} // namespace install
