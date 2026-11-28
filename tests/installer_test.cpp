// SPDX-License-Identifier: GPL-3.0-or-later
// Host tests for the Install runner (M3 Increment 8): the download through the real net worker against loopback servers (no network
// needed), MD5 verification, retry rules, redirect refusal, cancel, space and network pre-flight at job start, a real ENOSPC
// (/dev/full) and the claim and journal contract, all inside real install::Jobs on real SDL threads (no SDL_Init).
#include <sys/stat.h>
#include <unistd.h>

#include <SDL_timer.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Check.h"
#include "LoopbackServer.h"
#include "catalog/OscApi.h"
#include "install/Installer.h"
#include "install/Journal.h"
#include "install/Md5Sink.h"
#include "install/Messages.h"
#include "net/NetState.h"
#include "net/Worker.h"
#include "third_party/md5/md5.h"

namespace fs = std::filesystem;
using namespace install;

struct Temp {
	std::string path;
	Temp() {
		char t[] = "/tmp/openshop-inst-XXXXXX";
		path = mkdtemp(t);
	}
	~Temp() { std::error_code ec; fs::remove_all(path, ec); }
};
static bool there(const std::string& p) { std::error_code ec; return fs::symlink_status(p, ec).type() != fs::file_type::not_found; }
static uint32_t now() { return SDL_GetTicks(); }

static std::string md5OfBody(const std::string& body) {
	MD5_CTX c;
	MD5_Init(&c);
	MD5_Update(&c, body.data(), body.size());
	unsigned char d[16];
	MD5_Final(d, &c);
	return md5Hex(d);
}

// Calls update() until `pred` holds or the timeout passes.
static bool pump(Jobs& j, const std::function<bool()>& pred, uint32_t timeoutMs = 15000) {
	const uint32_t t0 = now();
	for (;;) {
		j.update();
		if (pred()) return true;
		if (now() - t0 > timeoutMs) return false;
		SDL_Delay(2);
	}
}

struct Outcome {
	JobState state = JobState::Queued;
	std::string error;
	bool found = false;
};

// One harness: a root, a net worker, Jobs with the Install runner. The preflight is relaxed to loopback URLs the way a test may.
struct Rig {
	Temp tmp;
	std::string root;
	net::Worker worker{net::defaultTransportFactory(), /*lowPriority=*/false};
	Jobs jobs;
	std::unique_ptr<Installer> inst;
	std::vector<std::string> verified; // zip paths seen by onVerified (existence and size are checked inside the hook)
	std::vector<uint64_t> verifiedSizes;
	std::function<void(const std::string&)> before;
	std::optional<uint64_t> freeBytes; // when set, the injected free space

	explicit Rig(bool relaxed = true, bool startWorker = true, uint32_t retryDelayMs = 20) {
		root = tmp.path;
		net::setNetState(net::NetState::Unknown, now());
		if (startWorker) CHECK(worker.start());
		Jobs::Config jc;
		jc.removeRoots = {root};
		CHECK(jobs.start(jc));
		Installer::Config ic;
		ic.worker = &worker;
		ic.removeRoots = {root};
		ic.retryDelayMs = retryDelayMs;
		ic.cancelWaitMs = 2000;
		if (relaxed) {
			ic.preflight.refusal = [](const JobSpec&) { return InstallRefusal::None; };
			ic.preflight.urlAllowed = [](const std::string& u) { return u.rfind("http://127.0.0.1:", 0) == 0; };
		}
		ic.preflight.freeSpace = [this](const std::string& r) -> std::optional<uint64_t> {
			if (freeBytes) return freeBytes;
			return freeSpace(r);
		};
		ic.beforeDownload = [this](const std::string& d) { if (before) before(d); };
		ic.onVerified = [this](const std::string& z) {
			verified.push_back(z);
			std::error_code ec;
			verifiedSizes.push_back(fs::file_size(z, ec));
		};
		inst = std::make_unique<Installer>(std::move(ic));
		jobs.setRunner(JobKind::Install, inst->runner());
	}
	~Rig() {
		jobs.shutdown();
		worker.stop();
		worker.discardResults();
	}
	JobSpec spec(const std::string& url, const std::string& body, uint64_t size = 0) const {
		JobSpec s;
		s.slug = "testapp";
		s.name = "Test app";
		s.url = url;
		s.md5 = md5OfBody(body);
		s.archiveSize = size ? size : body.size();
		s.uncompressedSize = s.archiveSize * 2;
		s.targetRoot = root;
		s.device = "sd";
		return s;
	}
	bool anyStaging() const {
		const std::string d = root + "/apps/OpenShop/staging";
		if (!there(d)) return false;
		return !fs::is_empty(d);
	}
	bool journal() const { return there(root + "/apps/OpenShop/installed/.journal"); }
	Outcome run(const JobSpec& s, uint32_t timeoutMs = 15000) {
		Outcome o;
		const JobId id = jobs.add(JobKind::Install, s);
		CHECK(id != 0);
		CHECK(pump(jobs, [&] { return jobs.queue().activeCount() == 0; }, timeoutMs));
		for (const JobView& v : jobs.snapshot().history)
			if (v.id == id) {
				o.state = v.state;
				o.error = v.error;
				o.found = true;
			}
		return o;
	}
};

static const std::string kDamaged = "The download was damaged. Please try again.";

static void testSuccess() {
	LoopbackServer srv;
	Rig rig;
	const std::string body = patternBody(300000);
	const Outcome o = rig.run(rig.spec(srv.url("/len/300000"), body));
	CHECK(o.found && o.state == JobState::Done && o.error.empty());
	CHECK(rig.verified.size() == 1);
	CHECK(rig.verifiedSizes.size() == 1 && rig.verifiedSizes[0] == 300000); // the committed download.zip, complete, when it was verified
	CHECK(!rig.anyStaging());   // the job's staging is gone afterwards
	CHECK(!rig.journal());      // and so is its start journal: the next job is not blocked
	CHECK(srv.requests().size() == 1);
	CHECK(net::netState(now()) == net::NetState::Up);
	// The request carries the app's User-Agent (OSC API guidelines) and no other identity.
	const std::string req = srv.requests()[0];
	CHECK(req.find("User-Agent: OpenShop/") != std::string::npos);
}

static void testBadMd5AndSizes() {
	LoopbackServer srv;
	Rig rig;
	const std::string body = patternBody(100000);
	JobSpec s = rig.spec(srv.url("/len/100000"), body);
	s.md5[0] = s.md5[0] == '0' ? '1' : '0';       // a hash the file can't match (what OPENSHOP_TEST_BAD_MD5 does)
	Outcome o = rig.run(s);
	CHECK(o.state == JobState::Failed && o.error == kDamaged);
	CHECK(!rig.anyStaging() && !rig.journal());
	CHECK(rig.verified.empty());

	// Content-Length differs from the catalog's size -> TooLarge at the headers, no retry, nothing written.
	const size_t before = srv.requests().size();
	o = rig.run(rig.spec(srv.url("/len/100000"), body, 100001));
	CHECK(o.state == JobState::Failed && o.error == downloadErrorText(HttpError::TooLarge));
	CHECK(srv.requests().size() == before + 1);
	CHECK(!rig.anyStaging() && !rig.journal());

	// A chunked body (no Content-Length) one byte longer than the catalog says -> TooLarge at the first byte past the size.
	o = rig.run(rig.spec(srv.url("/chunked/100000/4096"), body, 99999));
	CHECK(o.state == JobState::Failed && o.error == downloadErrorText(HttpError::TooLarge));
	CHECK(!rig.anyStaging() && !rig.journal());

	// A chunked body that ends SHORT of the catalog's size ends "ok" at the transfer layer; verify calls it damaged.
	o = rig.run(rig.spec(srv.url("/chunked/100000/4096"), body, 100010));
	CHECK(o.state == JobState::Failed && o.error == kDamaged);
	CHECK(!rig.anyStaging() && !rig.journal());
	CHECK(rig.verified.empty());

	// Right size, right bytes: still fine after all those failures (no journal or staging left to block it).
	o = rig.run(rig.spec(srv.url("/len/100000"), body));
	CHECK(o.state == JobState::Done);
	CHECK(rig.verified.size() == 1);
}

static void testRedirectNeverFollowed() {
	LoopbackServer a, b;
	Rig rig;
	const std::string body = patternBody(5000);
	// A answers 302 to B (another http host). The download must fail with Redirects and B must never be contacted.
	a.addRawRoute("/api/x.zip", "HTTP/1.1 302 Found\r\nLocation: " + b.url("/len/5000") + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
	const Outcome o = rig.run(rig.spec(a.url("/api/x.zip"), body));
	CHECK(o.state == JobState::Failed && o.error == "The shop's server sent an unexpected answer.");
	CHECK(a.requests().size() == 1);   // Redirects does not retry
	CHECK(b.requests().empty());       // the second host was never contacted
	CHECK(!rig.anyStaging() && !rig.journal());
	// The same for a redirect to https: nothing is followed and no TLS is attempted.
	a.addRawRoute("/api/y.zip", "HTTP/1.1 301 Moved\r\nLocation: https://example.org/y.zip\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
	const Outcome o2 = rig.run(rig.spec(a.url("/api/y.zip"), body));
	CHECK(o2.state == JobState::Failed && o2.error == "The shop's server sent an unexpected answer.");
}

static void testRetryRules() {
	LoopbackServer srv;
	Rig rig;
	const std::string body = patternBody(2000);
	srv.addRoute("/e503", "busy", 503);
	srv.addRoute("/e404", "gone", 404);
	// 5xx: one automatic retry from zero, then the failure with the status.
	Outcome o = rig.run(rig.spec(srv.url("/e503"), body));
	CHECK(o.state == JobState::Failed && o.error == downloadErrorText(HttpError::HttpStatus, 503));
	CHECK(srv.requests().size() == 2);
	// 4xx: no retry.
	o = rig.run(rig.spec(srv.url("/e404"), body));
	CHECK(o.state == JobState::Failed && o.error == downloadErrorText(HttpError::HttpStatus, 404));
	CHECK(srv.requests().size() == 3);
	CHECK(!rig.anyStaging() && !rig.journal());

	// Connection refused: Connect is retryable, so two attempts, then the failure.
	int deadPort = 0;
	{
		LoopbackServer gone;
		deadPort = gone.port();
	}
	const uint32_t t0 = now();
	o = rig.run(rig.spec("http://127.0.0.1:" + std::to_string(deadPort) + "/len/2000", body));
	CHECK(o.state == JobState::Failed && o.error == downloadErrorText(HttpError::Connect));
	CHECK(now() - t0 < 8000);
	CHECK(!rig.anyStaging() && !rig.journal());

	// The pure table.
	CHECK(isRetryable(HttpError::NoNetwork, 0) && isRetryable(HttpError::Dns, 0) && isRetryable(HttpError::Connect, 0) &&
	      isRetryable(HttpError::Timeout, 0) && isRetryable(HttpError::HttpStatus, 500) && isRetryable(HttpError::HttpStatus, 599));
	CHECK(!isRetryable(HttpError::HttpStatus, 404) && !isRetryable(HttpError::HttpStatus, 302) && !isRetryable(HttpError::Io, 0) &&
	      !isRetryable(HttpError::TooLarge, 0) && !isRetryable(HttpError::Redirects, 0) && !isRetryable(HttpError::Cancelled, 0) &&
	      !isRetryable(HttpError::Parse, 0) && !isRetryable(HttpError::TlsRequired, 0));
}

static void testPreflightAtStart() {
	LoopbackServer srv;
	const std::string body = patternBody(3000);
	{
		// A non-OSC URL is refused before anything is submitted or created (AC 24): the real allowlist, no overrides.
		Rig rig(/*relaxed=*/false);
		const Outcome o = rig.run(rig.spec(srv.url("/len/3000"), body));
		CHECK(o.state == JobState::Failed && !o.error.empty());
		CHECK(srv.requests().empty());
		CHECK(!there(rig.root + "/apps"));   // nothing was created on the device
		// The queue-time check says the same.
		const PreflightResult pr = queueTimeCheck(rig.spec(srv.url("/len/3000"), body), Device::Sd, now());
		CHECK(!pr.ok());
	}
	{
		Rig rig;
		// Too little free space: refused before the download, with the amount needed.
		const JobSpec s = rig.spec(srv.url("/len/3000"), body);
		rig.freeBytes = 1000;
		const Outcome o = rig.run(s);
		CHECK(o.state == JobState::Failed);
		CHECK(o.error == installErrorText(InstallError::NoSpace, Device::Sd, spaceNeeded(3000, 6000), 1000));
		CHECK(o.error.find("MB") != std::string::npos && o.error.find("SD card") != std::string::npos);
		CHECK(srv.requests().empty());
		CHECK(!there(rig.root + "/apps"));
		rig.freeBytes = spaceNeeded(3000, 6000); // exactly enough passes
		CHECK(rig.run(s).state == JobState::Done);
		rig.freeBytes.reset();
	}
	{
		// A network that is known to be down fails at job start, with no request.
		Rig rig;
		const size_t before = srv.requests().size();
		// The rig resets the state at construction; set Down now (within the 10 s the state holds).
		net::setNetState(net::NetState::Down, now());
		const Outcome o = rig.run(rig.spec(srv.url("/len/3000"), body));
		CHECK(o.state == JobState::Failed && o.error == installErrorText(InstallError::NetworkDown));
		CHECK(srv.requests().size() == before);
		net::setNetState(net::NetState::Unknown, now());
		CHECK(rig.run(rig.spec(srv.url("/len/3000"), body)).state == JobState::Done);
	}
	{
		// No apps device: the job refuses with the plain text.
		Rig rig;
		JobSpec s = rig.spec(srv.url("/len/3000"), body);
		s.targetRoot.clear();
		s.device = "none";
		const Outcome o = rig.run(s);
		CHECK(o.state == JobState::Failed && o.error == "No SD card or USB drive found.");
	}
}

// A real ENOSPC: the tmp file is a symlink to /dev/full, so the first flush of the 64 KB buffer fails with ENOSPC.
static void testEnospc() {
	if (!there("/dev/full")) {
		std::printf("installer_test: /dev/full missing, ENOSPC test skipped\n");
		return;
	}
	LoopbackServer srv;
	Rig rig;
	const std::string body = patternBody(400000);
	rig.before = [](const std::string& stage) {
		std::error_code ec;
		fs::create_symlink("/dev/full", stage + "/download.zip.tmp", ec);
	};
	const Outcome o = rig.run(rig.spec(srv.url("/len/400000"), body));
	CHECK(o.state == JobState::Failed && o.error == downloadErrorText(HttpError::Io));
	CHECK(srv.requests().size() == 1);   // Io is not retried
	CHECK(!rig.anyStaging() && !rig.journal());   // no .tmp (the symlink) and no staging left
	CHECK(rig.verified.empty());
	CHECK(fs::exists("/dev/full"));      // the symlink target was not touched
	rig.before = nullptr;
	CHECK(rig.run(rig.spec(srv.url("/len/400000"), body)).state == JobState::Done); // the next job works
}

// Cancel during the download: Cancelled, the staging and the journal are gone, and while it ran the claim and the journal held.
static void testCancelAndClaim() {
	LoopbackServer srv;
	Rig rig;
	srv.addStallRoute("/stall.zip", 100000);
	const JobSpec s = rig.spec(srv.url("/stall.zip"), patternBody(100000));
	const JobId id = rig.jobs.add(JobKind::Install, s);
	CHECK(id != 0);
	CHECK(pump(rig.jobs, [&] { return !rig.jobs.snapshot().line.empty() && rig.jobs.snapshot().line[0].state == JobState::Downloading; }));
	// While downloading: the staging dir and the start journal are on the card.
	CHECK(pump(rig.jobs, [&] { return rig.anyStaging() && rig.journal() && srv.requests().size() == 1; }));
	// A Cleanup job queued now waits behind the Install job (it is the running head of the FIFO) and cannot touch its staging.
	const JobId cl = rig.jobs.queueCleanup(rig.root, "sd");
	CHECK(cl != 0);
	for (int i = 0; i < 20; i++) {
		rig.jobs.update();
		SDL_Delay(10);
	}
	CHECK(rig.anyStaging() && rig.journal());
	const JobEntry* ce = rig.jobs.queue().find(cl);
	CHECK(ce && ce->state == JobState::Queued);
	// Cancel: the transfer is cancelled, the job ends Cancelled and cleans up.
	const uint32_t t0 = now();
	CHECK(rig.jobs.cancel(id));
	CHECK(rig.jobs.queue().find(id) && rig.jobs.queue().find(id)->state == JobState::Cancelled);
	CHECK(pump(rig.jobs, [&] { return rig.jobs.queue().activeCount() == 0; }));
	CHECK(now() - t0 < 3000);
	CHECK(!rig.anyStaging() && !rig.journal());
	for (const JobView& v : rig.jobs.snapshot().history) CHECK(v.id != id); // no history entry for a cancelled job
	CHECK(rig.verified.empty());
}

// The net worker's Catalog class and icons keep working while the archive downloads, and the download's progress advances.
static void testOthersWhileDownloading() {
	LoopbackServer srv;
	Rig rig;
	const std::string body = patternBody(120000);
	const JobId id = rig.jobs.add(JobKind::Install, rig.spec(srv.url("/slow/120000/60000"), body));
	CHECK(id != 0);
	CHECK(pump(rig.jobs, [&] { return !rig.jobs.snapshot().line.empty() && rig.jobs.snapshot().line[0].state == JobState::Downloading; }));
	// A Catalog-class fetch submitted now must finish long before the ~2 s download does.
	std::atomic<bool> fetched{false};
	HttpRequest req;
	req.url = srv.url("/len/50");
	req.totalTimeoutMs = 5000;
	const net::JobId nid = rig.worker.submit(net::JobClass::Catalog, "test:catalog",
	    std::make_unique<net::HttpGetJob>(req, std::make_unique<MemorySink>(), [&fetched](HttpSink& s, const HttpResult& r, net::JobContext&) {
		    if (r.ok() && static_cast<MemorySink&>(s).body().size() == 50) fetched = true;
	    }));
	CHECK(nid != 0);
	std::vector<uint64_t> samples;
	const uint32_t t0 = now();
	bool fetchedBeforeEnd = false;
	pump(rig.jobs, [&] {
		const JobSnapshot& sn = rig.jobs.snapshot();
		if (!sn.line.empty() && sn.line[0].state == JobState::Downloading && sn.line[0].total == 120000) {
			if (samples.empty() || samples.back() != sn.line[0].done) samples.push_back(sn.line[0].done);
		}
		if (fetched && sn.line.size() == 1 && sn.line[0].state == JobState::Downloading) fetchedBeforeEnd = true;
		return rig.jobs.queue().activeCount() == 0;
	});
	CHECK(now() - t0 < 12000);
	CHECK(fetchedBeforeEnd);
	size_t distinct = 0;
	for (size_t i = 1; i < samples.size(); i++) distinct += samples[i] > samples[i - 1];
	CHECK(distinct >= 2);   // progress kept advancing
	bool rising = true;
	for (size_t i = 1; i < samples.size(); i++) rising = rising && samples[i] >= samples[i - 1];
	CHECK(rising);
	bool done = false;
	for (const JobView& v : rig.jobs.snapshot().history) done = done || (v.id == id && v.state == JobState::Done);
	CHECK(done);
}

// A net worker that is not running: submit() returns 0, the job fails at once (no retry, the NetState untouched) instead of hanging.
static void testWorkerNotRunning() {
	LoopbackServer srv;
	Rig rig(/*relaxed=*/true, /*startWorker=*/false);
	net::setNetState(net::NetState::Up, now());
	const Outcome o = rig.run(rig.spec(srv.url("/len/100"), patternBody(100)), 5000);
	CHECK(o.state == JobState::Failed && o.error == installErrorText(InstallError::Unavailable));
	CHECK(o.error == "Downloads aren't available right now.");
	CHECK(srv.requests().empty());
	CHECK(!rig.anyStaging() && !rig.journal());
	CHECK(net::netState(now()) == net::NetState::Up); // a rejected submit is not a network result
	net::setNetState(net::NetState::Unknown, now());
}

// App::shutdown's order while a download is stalled: beginShutdown, cancelAll, jobs.shutdown. Nothing is left behind.
static void testShutdownDuringDownload() {
	LoopbackServer srv;
	Rig rig;
	srv.addStallRoute("/stall.zip", 100000);
	const JobId id = rig.jobs.add(JobKind::Install, rig.spec(srv.url("/stall.zip"), patternBody(100000)));
	CHECK(id != 0);
	CHECK(pump(rig.jobs, [&] { return rig.anyStaging() && rig.journal() && srv.requests().size() == 1; }));
	const uint32_t t0 = now();
	rig.jobs.beginShutdown();
	rig.worker.cancelAll();
	rig.jobs.shutdown();
	CHECK(now() - t0 < 5000);
	CHECK(!rig.anyStaging() && !rig.journal());
}

// The net worker stops first (the "net job vanished" path): the job fails as unavailable and cleans up.
static void testWorkerStoppedDuringDownload() {
	LoopbackServer srv;
	Rig rig;
	srv.addStallRoute("/stall.zip", 100000);
	const JobId id = rig.jobs.add(JobKind::Install, rig.spec(srv.url("/stall.zip"), patternBody(100000)));
	CHECK(id != 0);
	CHECK(pump(rig.jobs, [&] { return rig.anyStaging() && rig.journal() && srv.requests().size() == 1; }));
	rig.worker.stop();
	CHECK(pump(rig.jobs, [&] { return rig.jobs.queue().activeCount() == 0; }, 8000));
	// The stopped worker cancels the transfer, so the job ends Cancelled (no history entry); it must never end Done.
	for (const JobView& v : rig.jobs.snapshot().history) CHECK(v.id != id || v.state == JobState::Failed);
	CHECK(rig.verified.empty());
	CHECK(!rig.anyStaging() && !rig.journal());
}

// Bodies that do not match the size: a connection that closes early, and close-delimited short and long bodies.
static void testShortAndLongBodies() {
	LoopbackServer srv;
	Rig rig;
	const std::string b1000 = patternBody(1000);
	// Content-Length 1000, connection closes after 500: Io, no retry, nothing left.
	srv.addRawRoute("/short.zip", "HTTP/1.1 200 OK\r\nContent-Length: 1000\r\nConnection: close\r\n\r\n" + patternBody(500));
	Outcome o = rig.run(rig.spec(srv.url("/short.zip"), b1000));
	CHECK(o.state == JobState::Failed && o.error == downloadErrorText(HttpError::Io));
	CHECK(srv.requests().size() == 1);
	CHECK(!rig.anyStaging() && !rig.journal());
	// No Content-Length, the connection closes after 500 of 1000: the transfer ends ok, verify calls it damaged.
	srv.addRawRoute("/nocl.zip", "HTTP/1.0 200 OK\r\nConnection: close\r\n\r\n" + patternBody(500));
	o = rig.run(rig.spec(srv.url("/nocl.zip"), b1000));
	CHECK(o.state == JobState::Failed && o.error == kDamaged);
	CHECK(!rig.anyStaging() && !rig.journal());
	// No Content-Length and 1001 bytes of 1000: TooLarge at the first byte past the size.
	srv.addRawRoute("/long.zip", "HTTP/1.0 200 OK\r\nConnection: close\r\n\r\n" + patternBody(1001));
	o = rig.run(rig.spec(srv.url("/long.zip"), b1000));
	CHECK(o.state == JobState::Failed && o.error == downloadErrorText(HttpError::TooLarge));
	CHECK(!rig.anyStaging() && !rig.journal());
	CHECK(rig.verified.empty());
}

// Cancel while the job waits out the retry delay: no second request, nothing left.
static void testCancelDuringRetryDelay() {
	LoopbackServer srv;
	Rig rig(/*relaxed=*/true, /*startWorker=*/true, /*retryDelayMs=*/3000);
	srv.addRoute("/e503", "busy", 503);
	const JobId id = rig.jobs.add(JobKind::Install, rig.spec(srv.url("/e503"), patternBody(10)));
	CHECK(id != 0);
	CHECK(pump(rig.jobs, [&] { return srv.requests().size() >= 1; }));
	SDL_Delay(100); // the runner is in the retry delay now
	const uint32_t t0 = now();
	CHECK(rig.jobs.cancel(id));
	CHECK(pump(rig.jobs, [&] { return rig.jobs.queue().activeCount() == 0; }));
	CHECK(now() - t0 < 2500);
	CHECK(srv.requests().size() == 1);
	CHECK(!rig.anyStaging() && !rig.journal());
}

static void testSpecAndState() {
	AppEntry a;
	a.slug = "mame4wii";
	a.name = "MAME4Wii";
	a.version = "1.0";
	a.archiveUrl = "http://hbb1.oscwii.org/api/contents/mame4wii/mame4wii.zip";
	a.archiveMd5 = "e996304d3eb08046ff9aeb76996fa93a";
	a.archiveSize = 78687769;
	a.uncompressedSize = 1798841;
	a.packageType = PackageType::Dol;
	a.subdirectories = {"/mame", "/apps/mame4wii"};
	JobSpec s = makeInstallSpec(a, "sd:", "sd");
	CHECK(s.slug == "mame4wii" && s.url == a.archiveUrl && s.md5 == a.archiveMd5 && s.archiveSize == 78687769 &&
	      s.uncompressedSize == 1798841 && s.targetRoot == "sd:" && s.device == "sd" && s.dirs.size() == 2 &&
	      s.packageType == static_cast<uint8_t>(PackageType::Dol));
	CHECK(a.installable() && refusalOf(s) == InstallRefusal::None);
	const JobSpec bad = makeInstallSpec(a, "sd:", "sd", /*flipMd5=*/true);
	CHECK(bad.md5 != a.archiveMd5 && bad.md5.size() == 32 && bad.md5.substr(1) == a.archiveMd5.substr(1));
	// The queue-time check on the real entry: passes with enough space (injected), fails without.
	PreflightEnv env;
	env.freeSpace = [](const std::string&) -> std::optional<uint64_t> { return spaceNeeded(78687769, 1798841); };
	CHECK(queueTimeCheck(s, Device::Sd, now(), env).ok());
	env.freeSpace = [](const std::string&) -> std::optional<uint64_t> { return spaceNeeded(78687769, 1798841) - 1; };
	CHECK(!queueTimeCheck(s, Device::Sd, now(), env).ok());
	// A foreign URL on the real allowlist is refused.
	s.url = "http://example.org/api/contents/mame4wii/mame4wii.zip";
	CHECK(!queueTimeCheck(s, Device::Sd, now(), env).ok());
}

int main() {
	testSpecAndState();
	testSuccess();
	testBadMd5AndSizes();
	testRedirectNeverFollowed();
	testRetryRules();
	testPreflightAtStart();
	testEnospc();
	testCancelAndClaim();
	testOthersWhileDownloading();
	testWorkerNotRunning();
	testShutdownDuringDownload();
	testWorkerStoppedDuringDownload();
	testShortAndLongBodies();
	testCancelDuringRetryDelay();
	return checkReport("installer_test");
}
