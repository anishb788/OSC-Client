// SPDX-License-Identifier: GPL-3.0-or-later
// See ClaimProbe.h. DEBUGTOOLS builds only.
#include "platform/ClaimProbe.h"

#ifdef OPENSHOP_DEBUGTOOLS
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "core/FileUtil.h"
#include "core/Log.h"
#include "install/Commit.h"
#include "install/Jobs.h"
#include "platform/Platform.h"

namespace platform {
namespace {

constexpr int64_t kFirstId = 990101; // far above anything a real job takes; freshJobId moves on if it is taken
constexpr const char* kMarker = ".claimprobe-marker";

// Shared by the three jobs. They run one after the other on the one install worker (FIFO), so no lock is needed.
struct State {
	std::string root;
	int64_t claimedId = -1, abandonedId = -1;
	bool refused = false, keptOk = false, removedOk = false;
	bool madeBase = false; // staging/ did not exist before the probe, so the probe removes it again if it is empty
	std::string why;
};

bool writeSmall(const std::string& path, const std::string& text) {
	std::FILE* f = std::fopen(path.c_str(), "wb");
	if (!f) return false;
	const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
	return std::fclose(f) == 0 && ok;
}

// staging/<id>/<marker> and staging/<id>/root/apps/claimprobe/{a,b,c}.txt
bool createStaged(const std::string& root, int64_t id) {
	const std::string d = install::stagingAbs(root, id);
	if (!makeDirs(d + "/root/apps/claimprobe")) return false;
	bool ok = writeSmall(d + "/" + kMarker, "claimprobe\n");
	for (const char* n : {"a.txt", "b.txt", "c.txt"}) ok = writeSmall(d + "/root/apps/claimprobe/" + n, std::string("file ") + n + "\n") && ok;
	return ok;
}
bool intact(const std::string& root, int64_t id) {
	const std::string d = install::stagingAbs(root, id);
	for (const char* n : {"/root/apps/claimprobe/a.txt", "/root/apps/claimprobe/b.txt", "/root/apps/claimprobe/c.txt"})
		if (presenceOf(d + n) != Presence::Present) return false;
	return presenceOf(d + "/" + kMarker) == Presence::Present;
}
bool absent(const std::string& root, int64_t id) { return id < 0 || presenceOf(install::stagingAbs(root, id)) == Presence::Absent; }

install::TaskOutcome partA(const std::shared_ptr<State>& st, install::RunCtx& ctx) {
	const std::string& root = st->root;
	const std::string blocker = install::journalBlocker(root, "sd");
	if (!blocker.empty()) {
		LOG("claimprobe: REFUSED: a journal exists on %s (or it cannot be told apart from one): %s", root.c_str(), blocker.c_str());
		st->refused = true;
		return {};
	}
	const std::string base = install::stagingBase(root);
	const Presence bp = presenceOf(base);
	std::vector<std::string> names;
	if (bp == Presence::Error || (bp == Presence::Present && !listDir(base, names))) {
		LOG("claimprobe: REFUSED: cannot look at %s", base.c_str());
		st->refused = true;
		return {};
	}
	st->madeBase = (bp == Presence::Absent);
	if (!names.empty()) {
		LOG("claimprobe: REFUSED: %s holds %zu dir(s) the probe did not create (first: %s); skipping", base.c_str(), names.size(), names[0].c_str());
		st->refused = true;
		return {};
	}
	const int64_t claimed = install::freshJobId(root, kFirstId);
	if (claimed < 0) {
		LOG("claimprobe: REFUSED: freshJobId failed");
		st->refused = true;
		return {};
	}
	st->claimedId = claimed;
	ctx.claimStaging(root, claimed); // BEFORE the dir exists (the contract in Jobs.h)
	if (!createStaged(root, claimed)) {
		st->why = "could not create the claimed dir";
		return {};
	}
	const int64_t abandoned = install::freshJobId(root, claimed + 1);
	st->abandonedId = abandoned;
	if (abandoned < 0 || !createStaged(root, abandoned)) {
		st->why = "could not create the abandoned dir";
		return {};
	}
	LOG("claimprobe: staging/%lld is claimed by this job (held), staging/%lld is abandoned; running the Cleanup routine now", (long long)claimed,
	    (long long)abandoned);
	const install::TaskOutcome oc = ctx.cleanup(root, "sd"); // while the claim is held
	st->keptOk = intact(root, claimed);
	st->removedOk = absent(root, abandoned);
	LOG("claimprobe: claimed dir kept=%d abandoned dir removed=%d", st->keptOk ? 1 : 0, st->removedOk ? 1 : 0);
	if (oc.state != install::JobState::Done) LOG("claimprobe: the Cleanup routine ended %s %s", install::stateName(oc.state), oc.error.c_str());
	return {}; // the claim ends here
}

install::TaskOutcome partC(const std::shared_ptr<State>& st) {
	if (st->refused) {
		LOG("claimprobe: no result (refused, see above)");
		return {};
	}
	const std::string& root = st->root;
	const bool claimedGone = absent(root, st->claimedId);
	LOG("claimprobe: after the normal Cleanup job: claimed dir removed=%d", claimedGone ? 1 : 0);
	std::string why = st->why;
	if (why.empty() && !st->keptOk) why = "the claimed dir did not survive the Cleanup routine while it was claimed";
	if (why.empty() && !st->removedOk) why = "the abandoned dir survived the Cleanup routine";
	if (why.empty() && !claimedGone) why = "the claimed dir was still there after the claim was released and Cleanup ran";
	if (why.empty()) LOG("claimprobe: RESULT PASS");
	else LOG("claimprobe: RESULT FAIL %s", why.c_str());
	// Whatever is left of what the probe made goes, also after a failure.
	bool clean = true;
	for (const int64_t id : {st->claimedId, st->abandonedId}) {
		if (id < 0 || absent(root, id)) continue;
		const RemoveResult r = platform::removeTree(install::stagingAbs(root, id), install::stagingBase(root));
		if (!r.ok) LOG("claimprobe: could not remove staging/%lld: %s", (long long)id, r.error.c_str());
		if (!absent(root, id)) clean = false;
	}
	// Only the folder the probe itself created, and only when nothing else is in it (removeDirIfEmpty refuses otherwise).
	if (clean && st->madeBase && !removeDirIfEmpty(install::stagingBase(root))) {
		LOG("claimprobe: could not remove the empty %s it created", install::stagingBase(root).c_str());
		clean = false;
	}
	if (clean) LOG("claimprobe: cleaned");
	else LOG("claimprobe: NOT CLEAN: leftovers remain under %s (the next Cleanup job removes them)", install::stagingBase(root).c_str());
	return {};
}
} // namespace

bool claimProbe(install::Jobs& jobs) {
	std::string root;
	for (const StorageRoot& r : storageRoots())
		if (r.id == "sd" && r.mounted && r.writable) root = r.root;
	if (root.empty()) {
		LOG("claimprobe: REFUSED: no writable SD root");
		return false;
	}
	if (!jobs.running()) {
		LOG("claimprobe: REFUSED: the job system is not running");
		return false;
	}
	auto st = std::make_shared<State>();
	st->root = root;
	install::JobSpec s;
	s.targetRoot = root;
	s.device = "sd";
	s.name = "Claim probe A";
	const install::JobId a = jobs.addWithRunner(install::JobKind::Sleep, s, [st](const install::JobSpec&, install::RunCtx& ctx) { return partA(st, ctx); });
	const install::JobId b = a ? jobs.queueCleanup(root, "sd") : 0;
	s.name = "Claim probe C";
	const install::JobId c = b ? jobs.addWithRunner(install::JobKind::Sleep, s, [st](const install::JobSpec&, install::RunCtx&) { return partC(st); }) : 0;
	if (!a || !b || !c) {
		LOG("claimprobe: REFUSED: the jobs could not be queued");
		return false;
	}
	LOG("claimprobe: queued jobs %u (hold a claim and run Cleanup), %u (a normal Cleanup job) and %u (verify and clean up) on %s", a, b, c, root.c_str());
	return true;
}
} // namespace platform
#endif
