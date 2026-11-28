// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// install::Installer (M3 Increment 8): the Install job's runner, up to a verified download. Extraction and commit are Increment 8a;
// until then a job that verifies ends Done after cleaning its staging ("verified, extraction pending", logged), so the tree stays in
// a working state. No UI path queues it yet (the dev trigger `install=<slug>` does).
//
// WHO DOES WHAT (the Threading rule)
//   main thread    makeInstallSpec() copies the catalog entry into a JobSpec, queueTimeCheck() answers "may it be queued", Jobs::add()
//                  queues it. Nothing else here runs on the main thread.
//   install worker the runner (this file) runs on it as the RUNNING HEAD of the FIFO for the job's whole life, download phase
//                  included, so nothing else (the Cleanup job above all) runs there meanwhile. It holds the staging claim
//                  (RunCtx::claimStaging, made before the dir exists and before the start journal is written) and the journal check
//                  the wrapper made before calling it. It submits ONE Background HttpGetJob to the net worker and waits for it in
//                  slices of 100 ms (cancel, progress, "the net job vanished" checks); that wait is the install worker's only work
//                  meanwhile. It then verifies and cleans up, all disk work.
//   net worker     the HttpGetJob streams the body through Md5Sink into FileSink (<staging>/download.zip.tmp); its onDone (net worker
//                  thread) commits the FileSink (rename to download.zip) and hands a COPY of the result (digest, byte count,
//                  HttpResult) to the runner through a mutex-protected cell. No sockets are opened here and no net_* is called: only
//                  net::Worker's public API is used (submit, cancel, progress), which any thread may call.
//
// ONE CHANGE FROM THE PLAN: the plan says the result is "posted" to the main thread; here it goes straight to the waiting runner,
// because the runner must stay the job (the main thread would have to hand it back to the install worker, and the job could not
// keep the claim and the journal check as one continuous piece of code). The main thread still sees everything through the
// ProgressCell (stage, bytes done / total) and Jobs' snapshot.
//
// RETRY: one automatic retry from zero after NoNetwork, Dns, Connect, Timeout or a 5xx (Messages::isRetryable), after
// retryDelayMs. A damaged download (checksum or size mismatch) is never retried automatically; the user retries.
//
// COMMIT CONTRACT: this runner changes nothing outside <root>/apps/OpenShop/{staging,installed/.journal}. It never calls
// enterCommit(). Increment 8a calls it before its first change outside staging.
//
// CLEANUP: a scope guard (ScrubGuard in Installer.cpp), created right after the staging claim, removes the staging dir and then the
// start journal on EVERY exit: success, failure, cancel, early return. It is disarmed only by an explicit hand-over, guard.release(),
// which Increment 8a calls at the commit hand-over (after enterCommit(), when the commit's own cleanup takes over staging and journal).
// Nothing else may disarm it.
//
// NET WORKER REFUSAL: Worker::submit() returning 0 (not started or stopping) fails the job at once with InstallError::Unavailable: no
// retry, and the shared NetState is not touched (it is not a network result).
//
// TEST HOOKS: the Config members marked "Tests" exist only when OPENSHOP_INSTALLER_TESTHOOKS is 1 (OPENSHOP_TESTS, which Makefile.pc
// defines for the test binaries only, or OPENSHOP_DEVHOOKS), so a release build cannot weaken the allowlist re-check.
//
// CHECKSUM WORDING: archives come over plain http; MD5 detects corruption and truncation, not tampering.
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "catalog/Catalog.h"
#include "core/FileUtil.h"
#include "install/Jobs.h"
#include "install/Preflight.h"

namespace net { class Worker; }

#if (defined(OPENSHOP_TESTS) && OPENSHOP_TESTS) || (defined(OPENSHOP_DEVHOOKS) && OPENSHOP_DEVHOOKS)
#define OPENSHOP_INSTALLER_TESTHOOKS 1
#else
#define OPENSHOP_INSTALLER_TESTHOOKS 0
#endif

namespace install {

// Copies an entry into an Install JobSpec, like the UI will. `root` is App::appsRoot() ("" = no device), `device` its id
// ("sd" | "usb"). `flipMd5` is the dev hook OPENSHOP_TEST_BAD_MD5: the expected hash gets its first digit changed. Pure.
JobSpec makeInstallSpec(const AppEntry& app, const std::string& root, const std::string& device, bool flipMd5 = false);

class Installer {
public:
	struct Config {
		net::Worker* worker = nullptr;          // the app's net worker; App::shutdown joins the install worker (Jobs::shutdown) before the net worker stops
		std::vector<std::string> removeRoots;   // FileUtil::removeTree's roots (platform::storageRoots())
		FileOps fileOps;                        // tests
#if OPENSHOP_INSTALLER_TESTHOOKS
		PreflightEnv preflight;                 // Tests (refusal / urlAllowed / freeSpace); `net` is ignored, the runner reads net::netState
#endif
		std::function<uint32_t()> nowMs;        // default SDL_GetTicks
		uint32_t retryDelayMs = 1000;           // the pause before the automatic retry
		uint32_t cancelWaitMs = 5000;           // how long a cancelled job waits for the net worker to let go of the transfer
#if OPENSHOP_INSTALLER_TESTHOOKS
		// Tests: called on the install worker with the absolute staging dir after the start journal is written and before the
		// download is submitted (a test plants a symlink to /dev/full there to get a real ENOSPC).
		std::function<void(const std::string& stagingDir)> beforeDownload;
		// Tests: called with the absolute path of the verified download.zip just before the job cleans its staging.
		std::function<void(const std::string& zipPath)> onVerified;
#endif
	};
	explicit Installer(Config cfg);

	// Registers with Jobs::setRunner(JobKind::Install, ...). The runner owns a copy of the config and shared state only, so it stays
	// valid for a detached worker.
	Runner runner() const;

private:
	std::shared_ptr<const Config> cfg_;
};

// Queue-time pre-flight (main thread): the same decision the job makes again when it starts. `device` is the apps device.
PreflightResult queueTimeCheck(const JobSpec& spec, Device device, uint32_t nowMs, const PreflightEnv& env = {});

} // namespace install
