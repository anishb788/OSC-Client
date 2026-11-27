// SPDX-License-Identifier: GPL-3.0-or-later
// See CommitProbe.h. DEBUGTOOLS builds only.
#include "platform/CommitProbe.h"

#ifdef OPENSHOP_DEBUGTOOLS
#include <SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/FileUtil.h"
#include "core/Log.h"
#include "core/StackProbe.h"
#include "install/Commit.h"
#include "install/Manifest.h"
#include "install/RecordIo.h"
#include "platform/Platform.h"

namespace platform {
namespace {

constexpr size_t kDefaultStackKb = 64; // the plan's proposal for the install worker
constexpr int kDefaultFiles = 5000;
constexpr int kMinFiles = 10, kMaxFiles = 19500;
constexpr int kShared = 50;                    // shared folders s0..s49
constexpr int64_t kProbeJob = 990001;          // the probe's own staging job id
constexpr const char* kSlug = "osprobe";
constexpr const char* kMarker = ".osprobe-marker";
constexpr size_t kPaintSkip = 2048;            // bytes below the worker's entry left alone (the paint helper's own frames)
constexpr size_t kStackReserve = 8 * 1024;     // never paint the last 8 KB of the requested stack

struct Job {
	std::string mode;
	std::string root; // the SD root ("sd:" on the Wii, ./sdcard on the PC)
	int files = kDefaultFiles;
	size_t stackBytes = kDefaultStackKb * 1024;
	bool ran = false;
	// stack
	uintptr_t entry = 0, paintLo = 0;
	size_t painted = 0, highWater = 0;
	// heap samples
	size_t heapBefore = 0, heapPeak = 0, heapAfter = 0;
	const char* peakAt = "before";
	uint32_t phaseStart = 0, runStart = 0;
};
Job* gJob = nullptr; // the hooks are plain function pointers, so the running job is reachable through this

// ---- paths ---------------------------------------------------------------------------------------------------------------
std::string appsDir(const Job& j) { return j.root + "/apps/" + kSlug; }
std::string stagingDirOf(const Job& j) { return install::stagingAbs(j.root, kProbeJob); }
std::string flushDir(const Job& j) { return j.root + "/apps/OpenShop/probe"; }

bool present(const std::string& p) { return presenceOf(p) == Presence::Present; }

bool writeSmall(const std::string& path, const char* data, size_t n) {
	std::FILE* f = std::fopen(path.c_str(), "wb");
	if (!f) return false;
	const bool ok = n == 0 || std::fwrite(data, 1, n, f) == n;
	return std::fclose(f) == 0 && ok;
}

// ---- cleanup (shared by every exit path) -----------------------------------------------------------------------------------
// Removes only what the probe creates: apps/osprobe (under <root>/apps), its staging job (under staging), its manifest and nothing else.
bool cleanCap(const Job& j) {
	bool ok = true;
	const RemoveResult a = platform::removeTree(appsDir(j), j.root + "/apps");
	if (!a.ok) { LOG("commitprobe: could not remove %s: %s", appsDir(j).c_str(), a.error.c_str()); ok = false; }
	const RemoveResult s = platform::removeTree(stagingDirOf(j), install::stagingBase(j.root));
	if (!s.ok) { LOG("commitprobe: could not remove %s: %s", stagingDirOf(j).c_str(), s.error.c_str()); ok = false; }
	if (!install::rec::removeRecord(install::manifestPath(j.root, kSlug))) { LOG("commitprobe: could not remove the probe manifest"); ok = false; }
	return ok;
}

// ---- the cap probe --------------------------------------------------------------------------------------------------------
void sample(const char* where) {
	if (!gJob) return;
	const size_t now = platform::heapInUseBytes();
	const uint32_t t = SDL_GetTicks();
	if (now > gJob->heapPeak) { gJob->heapPeak = now; gJob->peakAt = where; }
	LOG("commitprobe: phase %-16s +%6u ms (total %6u ms)  heap in use %zu KB (peak so far %zu KB)", where, (unsigned)(t - gJob->phaseStart),
	    (unsigned)(t - gJob->runStart), now / 1024, gJob->heapPeak / 1024);
	gJob->phaseStart = t;
}
void onPhase(const char* phase) { sample(phase); }

bool makeFiles(const std::string& dir, const char* prefix, int count, int from = 0) {
	for (int i = from; i < from + count; i++) {
		const std::string p = dir + "/" + prefix + std::to_string(i);
		if (!writeSmall(p, "x", 1)) { LOG("commitprobe: cannot write %s", p.c_str()); return false; }
	}
	return true;
}

bool buildTrees(const Job& j, install::Manifest& old, install::ZipPlan& plan) {
	const uint32_t t0 = SDL_GetTicks();
	const std::string app = appsDir(j);
	const std::string neu = stagingDirOf(j) + "/root/apps/" + kSlug;
	const int N = j.files;
	const int per = (N + kShared - 1) / kShared;
	if (!makeDirs(app + "/gone") || !makeDirs(neu)) return false;
	if (!writeSmall(app + "/" + kMarker, "osprobe", 7) || !writeSmall(app + "/boot.dol", "old", 3) || !writeSmall(neu + "/boot.dol", "new", 3)) return false;
	// the staging marker, so a leftover of a cut run is recognised as ours
	if (!writeSmall(stagingDirOf(j) + "/" + kMarker, "osprobe", 7)) return false;
	old = install::Manifest{};
	old.slug = kSlug;
	old.device = "sd";
	old.version = "1";
	old.installedAt = 1700000000;
	install::ManifestDir md;
	md.path = std::string("apps/") + kSlug;
	md.created = true;
	md.files.push_back({md.path + "/boot.dol", true});
	// gone/: N files the old install shipped and the new archive does not (one carried folder, N manifest paths)
	if (!makeFiles(app + "/gone", "g", N)) return false;
	md.createdSubdirs.push_back({md.path + "/gone", true});
	for (int i = 0; i < N; i++) md.files.push_back({md.path + "/gone/g" + std::to_string(i), true});
	LOG("commitprobe: built gone/ (%d files) in %u ms", N, (unsigned)(SDL_GetTicks() - t0));
	int made = 0;
	for (int k = 0; k < kShared; k++) {
		const std::string s = "s" + std::to_string(k);
		const int n = std::min(per, N - made);
		if (n <= 0) break;
		if (!makeDirs(app + "/" + s) || !makeDirs(neu + "/" + s)) return false;
		if (!writeSmall(app + "/" + s + "/shipped", "o", 1) || !writeSmall(neu + "/" + s + "/shipped", "n", 1)) return false;
		if (!makeFiles(app + "/" + s, "u", n, made)) return false;   // user files: one restoreFile each
		if (!makeFiles(neu + "/" + s, "n", n, made)) return false;   // shipped by the new archive: manifest paths
		md.createdSubdirs.push_back({md.path + "/" + s, true});
		md.files.push_back({md.path + "/" + s + "/shipped", true});
		made += n;
		if (k % 10 == 9) LOG("commitprobe: built %d of %d shared folders (%u ms)", k + 1, kShared, (unsigned)(SDL_GetTicks() - t0));
	}
	old.dirs.push_back(std::move(md));
	install::validateManifest(old);
	const std::string why = install::manifestSaveProblem(old);
	if (!why.empty()) { LOG("commitprobe: the synthetic old manifest does not fit: %s", why.c_str()); return false; }
	if (!install::saveManifest(j.root, old)) { LOG("commitprobe: cannot write the old manifest"); return false; }
	plan = install::ZipPlan{};
	plan.appDirs.push_back({kSlug, true});
	LOG("commitprobe: built the trees: %d user files, %d new files, %d old-manifest files in %u ms (3 x N file creations)", N, N, N,
	    (unsigned)(SDL_GetTicks() - t0));
	return true;
}

// Refuses when something that is not the probe's own is where the probe would write.
bool safeToRun(const Job& j) {
	const std::string app = appsDir(j), st = stagingDirOf(j);
	const std::string jp = install::journalPath(j.root);
	auto ours = [&](const std::string& dir) { return present(dir + "/" + kMarker); };
	if (present(app) && !ours(app)) { LOG("commitprobe: REFUSED: %s exists and is not the probe's (no %s)", app.c_str(), kMarker); return false; }
	if (present(st) && !ours(st)) { LOG("commitprobe: REFUSED: %s exists and is not the probe's (no %s)", st.c_str(), kMarker); return false; }
	if (present(jp) || present(jp + ".bak")) { LOG("commitprobe: REFUSED: an install journal exists (%s); recovery must deal with it first", jp.c_str()); return false; }
	const std::string mp = install::manifestPath(j.root, kSlug);
	if (present(mp) && !present(app + "/" + kMarker)) { LOG("commitprobe: REFUSED: %s exists and is not the probe's", mp.c_str()); return false; }
	return true;
}

void runCap(Job& j) {
	if (!safeToRun(j)) return;
	if (present(appsDir(j)) || present(stagingDirOf(j)) || present(install::manifestPath(j.root, kSlug))) {
		LOG("commitprobe: removing the probe's own leftover of an earlier run");
		if (!cleanCap(j)) return;
	}
	j.ran = true;
	// folders the probe makes only because they are missing; they go again when empty
	const std::string installedDir = install::rec::installedDir(j.root), stagingBase = install::stagingBase(j.root);
	const bool hadInstalled = present(installedDir), hadStaging = present(stagingBase);
	install::Manifest old;
	install::ZipPlan plan;
	bool built = false;
	if (!makeDirs(install::stagingBase(j.root))) LOG("commitprobe: cannot create the staging folder");
	else built = buildTrees(j, old, plan);
	if (built) {
		install::CommitInput in;
		in.root = j.root;
		in.device = "sd";
		in.jobId = kProbeJob;
		in.slug = kSlug;
		in.version = "2";
		in.installedAt = 1700000100;
		in.update = true;
		in.oldManifest = &old;
		in.plan = plan;
		for (const StorageRoot& r : storageRoots()) in.removeRoots.push_back(r.root);
		in.onPhase = onPhase;
		// the numbers start here
		gJob = &j;
		j.heapBefore = platform::heapInUseBytes();
		j.heapPeak = j.heapBefore;
		j.runStart = j.phaseStart = SDL_GetTicks();
		LOG("commitprobe: commit starts: heap in use %zu KB", j.heapBefore / 1024);
		const install::CommitResult r = install::commit(in);
		const uint32_t total = SDL_GetTicks() - j.runStart;
		j.heapAfter = platform::heapInUseBytes();
		gJob = nullptr;
		LOG("commitprobe: commit %s: ops=%d error=%d rolledBack=%d journalLeft=%d stagingRemoved=%d kept=%zu detail='%s' total %u ms",
		    r.ok ? "OK" : "FAILED", r.ops, (int)r.error, (int)r.rolledBack, (int)r.journalLeft, (int)r.stagingRemoved, r.kept.size(),
		    r.detail.c_str(), (unsigned)total);
		if (r.ok) {
			std::vector<std::string> names;
			listDir(appsDir(j) + "/s0", names);
			LOG("commitprobe: after the commit apps/osprobe/s0 holds %zu entries (shipped + new + carried user files)", names.size());
		}
	}
	const uint32_t c0 = SDL_GetTicks();
	const bool clean = cleanCap(j);
	const bool gone = !present(appsDir(j)) && !present(stagingDirOf(j)) && !present(install::manifestPath(j.root, kSlug)) &&
	                  !present(install::journalPath(j.root));
	if (!hadStaging) removeDirIfEmpty(stagingBase);
	if (!hadInstalled) removeDirIfEmpty(installedDir);
	LOG("commitprobe: removed the probe's files in %u ms", (unsigned)(SDL_GetTicks() - c0));
	if (clean && gone) LOG("commitprobe: cleaned");
	else LOG("commitprobe: NOT CLEAN: leftovers remain (apps/osprobe or staging/%lld); the next run removes them", (long long)kProbeJob);
}

// ---- the flush probe -------------------------------------------------------------------------------------------------------
std::string marker(char which, uint32_t salt) {
	char b[48];
	std::snprintf(b, sizeof b, "OSPROBE-%c-%08X", which, (unsigned)salt);
	return b;
}
// ~1 KB of the marker repeated, so a raw scan finds it at any sector alignment.
std::string body(const std::string& m) {
	std::string s;
	while (s.size() < 1000) s += m + "\n";
	return s;
}

void runFlush(Job& j) {
	const std::string dir = flushDir(j);
	if (present(dir)) {
		std::vector<std::string> names;
		listDir(dir, names);
		for (const std::string& n : names)
			if (n != "A.txt" && n != "B.txt" && n != "C.txt" && n != "C.tmp" && n != "A.txt.bak" && n != "A.txt.tmp") {
				LOG("commitprobe: REFUSED: %s exists and holds %s, which is not the probe's", dir.c_str(), n.c_str());
				return;
			}
	}
	j.ran = true;
	const uint32_t salt = (uint32_t)SDL_GetTicks() * 2654435761u ^ (uint32_t)std::rand() ^ 0x5EED0000u;
	const std::string ma = marker('A', salt), mb = marker('B', salt), mc = marker('C', salt);
	const std::string ba = body(ma), bb = body(mb), bc = body(mc);
	bool ok = makeDirs(dir);
	ok = ok && install::rec::writeRecord(dir + "/A.txt", ba, 1u << 20); // writeFileAtomic with fsync: the journal path
	{                                                                    // B: no fsync
		std::FILE* f = ok ? std::fopen((dir + "/B.txt").c_str(), "wb") : nullptr;
		ok = f && std::fwrite(bb.data(), 1, bb.size(), f) == bb.size();
		if (f) ok = std::fclose(f) == 0 && ok;
	}
	{                                                                    // C: fsynced tmp, then rename
		std::FILE* f = ok ? std::fopen((dir + "/C.tmp").c_str(), "wb") : nullptr;
		ok = f && std::fwrite(bc.data(), 1, bc.size(), f) == bc.size() && fsyncFile(f);
		if (f) ok = std::fclose(f) == 0 && ok;
		ok = ok && std::rename((dir + "/C.tmp").c_str(), (dir + "/C.txt").c_str()) == 0;
	}
	if (!ok) LOG("commitprobe: writing the probe files failed");
	else {
		LOG("commitprobe: flush ready: A.txt (%zu bytes, writeRecord = fwrite+fsync+rename) marker %s", ba.size(), ma.c_str());
		LOG("commitprobe: flush ready: B.txt (%zu bytes, fwrite+fclose, NO fsync) marker %s", bb.size(), mb.c_str());
		LOG("commitprobe: flush ready: C.txt (%zu bytes, fsynced C.tmp renamed to C.txt) marker %s", bc.size(), mc.c_str());
		LOG("commitprobe: flush ready in %s; snapshot the SD image now (sleeping 90 s)", dir.c_str());
		for (int s = 10; s <= 90; s += 10) {
			SDL_Delay(10000);
			LOG("commitprobe: flush waiting, %d of 90 s", s);
		}
	}
	bool clean = true;
	for (const char* n : {"A.txt", "A.txt.bak", "A.txt.tmp", "B.txt", "C.txt", "C.tmp"})
		if (present(dir + "/" + n) && std::remove((dir + "/" + n).c_str()) != 0) clean = false;
	if (present(dir) && !removeDirIfEmpty(dir)) clean = false;
	if (clean && !present(dir)) LOG("commitprobe: cleaned");
	else LOG("commitprobe: NOT CLEAN: %s remains", dir.c_str());
}

// ---- the worker --------------------------------------------------------------------------------------------------------------
int worker(void* arg) {
	Job& j = *static_cast<Job*>(arg);
	unsigned char entryMarker = 0;
	j.entry = (uintptr_t)&entryMarker;
	// Paint the unused part of this thread's own stack below the entry (never the last kStackReserve bytes of the requested size)
	// so the high-water can be measured afterwards. If the SDL port did not honour the size this could damage the heap, as the
	// plan's stack probe notes; it is a dev probe.
	j.painted = j.stackBytes > kPaintSkip + kStackReserve ? j.stackBytes - kPaintSkip - kStackReserve : 0;
	unsigned char* base = (unsigned char*)(j.entry - kPaintSkip - j.painted);
	j.paintLo = (uintptr_t)base;
	stackprobe::paint(base, j.painted);
	if (j.mode == "cap") runCap(j);
	else runFlush(j);
	j.highWater = stackprobe::highWater(base, j.painted);
	return 0;
}
} // namespace

bool commitProbe(const std::string& mode, int files, int stackKb) {
	if (mode != "cap" && mode != "flush") {
		LOG("commitprobe: unknown mode '%s' (cap or flush); nothing runs", mode.c_str());
		return false;
	}
	Job j;
	j.mode = mode;
	for (const StorageRoot& r : storageRoots())
		if (r.id == "sd" && r.mounted && r.writable) j.root = r.root;
	if (j.root.empty()) {
		LOG("commitprobe: REFUSED: no writable SD root");
		return false;
	}
	j.files = files < 0 ? kDefaultFiles : std::clamp(files, kMinFiles, kMaxFiles);
	j.stackBytes = (stackKb < 0 ? kDefaultStackKb : (size_t)std::max(16, stackKb)) * 1024;
	LOG("commitprobe: mode %s on %s, worker stack %zu bytes%s", mode.c_str(), j.root.c_str(), j.stackBytes,
	    mode == "cap" ? (", N=" + std::to_string(j.files)).c_str() : "");
	SDL_Thread* t = SDL_CreateThreadWithStackSize(worker, "commitprobe", j.stackBytes, &j);
	if (!t) {
		LOG("commitprobe: SDL_CreateThreadWithStackSize failed: %s", SDL_GetError());
		return false;
	}
	SDL_WaitThread(t, nullptr);
	if (j.mode == "cap" && j.ran) {
		const size_t used = j.highWater;
		LOG("commitprobe RESULT stack: requested %zu bytes; painted %zu bytes (%zu..%zu below the entry sp); high-water %zu bytes of them, i.e. "
		    "the commit reached about %zu bytes below the thread's entry%s",
		    j.stackBytes, j.painted, kPaintSkip, kPaintSkip + j.painted, used, used ? used + kPaintSkip : (size_t)0,
		    used >= j.painted ? " = the WHOLE painted region: the stack probably overflowed" : "");
		LOG("commitprobe RESULT heap: before %zu KB, peak %zu KB (at phase '%s'), after %zu KB; peak - before = %zu KB",
		    j.heapBefore / 1024, j.heapPeak / 1024, j.peakAt, j.heapAfter / 1024, (j.heapPeak - std::min(j.heapPeak, j.heapBefore)) / 1024);
	}
	LOG("commitprobe: finished (%s)", j.ran ? "ran" : "did not run");
	return j.ran;
}

} // namespace platform
#endif
