// SPDX-License-Identifier: GPL-3.0-or-later
#include "install/Commit.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <optional>
#include <set>

#include "core/Log.h"
#include "core/NameRules.h"
#include "core/PathRoot.h"
#include "install/RecordIo.h"
#include "install/Rules.h"

namespace install {
namespace {

// ---- small filesystem helpers (all through FileOps, so the tests can cut or fail any call) -----------------------------

std::string joinRoot(const std::string& root, const std::string& rel) { return root + "/" + rel; }

// 0 = answered (st.exists says whether it is there), -1 = a real error (never read that as "absent").
int statOf(const FileOps& ops, const std::string& p, FileStat& st) { return presenceOf(p, ops, &st) == Presence::Error ? -1 : 0; }
Presence presence(const FileOps& ops, const std::string& p) { return presenceOf(p, ops); }
bool isRealDirAt(const FileOps& ops, const std::string& p) {
	FileStat st;
	return presenceOf(p, ops, &st) == Presence::Present && st.isDir && !st.isLink;
}

std::string parentOf(const std::string& p) {
	const size_t s = p.rfind('/');
	return s == std::string::npos ? std::string() : p.substr(0, s);
}

// A plain rename of a file or a whole folder. `from` must be there and `to` must not (libfat cannot rename over anything, and
// what is at `to` may be someone's data).
bool moveEntry(const FileOps& ops, const std::string& from, const std::string& to) {
	if (presence(ops, to) != Presence::Absent) return false; // there, or cannot tell: never rename onto it
	const std::string parent = parentOf(to);
	if (!parent.empty() && parent.back() != ':' && !makeDirs(parent, ops)) return false;
	const int rc = ops.rename ? ops.rename(from.c_str(), to.c_str()) : std::rename(from.c_str(), to.c_str());
	return rc == 0;
}

bool listEntries(const FileOps& ops, const std::string& absDir, std::vector<DirEntry>& out) {
	out.clear();
	std::vector<std::string> names;
	if (!listDir(absDir, names, ops)) return false;
	std::sort(names.begin(), names.end());
	for (const std::string& n : names) {
		FileStat st;
		if (statOf(ops, absDir + "/" + n, st) != 0 || !st.exists) return false;
		out.push_back({n, st.isDir && !st.isLink});
	}
	return true;
}

// ---- the staged tree ----------------------------------------------------------------------------------------------------
struct Entry {
	std::string rel; // relative to the walked dir
	bool isDir = false;
};
constexpr size_t kMaxWalkEntries = kMaxManifestPaths;

// Preorder (a dir before its children), sorted by name. Symlinks and other odd entries make it fail with `unsafe`.
bool walkTree(const FileOps& ops, const std::string& absDir, const std::string& rel, int depth, std::vector<Entry>& out,
              std::string& err, bool& unsafe) {
	if (depth > kRemoveMaxDepth) { err = "folders nested too deeply"; unsafe = true; return false; }
	std::vector<std::string> names;
	if (!listDir(absDir, names, ops)) { err = "cannot read " + absDir; return false; }
	std::sort(names.begin(), names.end());
	for (const std::string& n : names) {
		FileStat st;
		if (statOf(ops, absDir + "/" + n, st) != 0 || !st.exists) { err = "cannot stat " + absDir + "/" + n; return false; }
		if (st.isLink) { err = "a symlink in the staged tree: " + absDir + "/" + n; unsafe = true; return false; }
		if (out.size() >= kMaxWalkEntries) { err = "too many files"; unsafe = true; return false; }
		const std::string r = rel.empty() ? n : rel + "/" + n;
		out.push_back({r, st.isDir});
		if (st.isDir && !walkTree(ops, absDir + "/" + n, r, depth + 1, out, err, unsafe)) return false;
	}
	return true;
}

// ---- carry-over ---------------------------------------------------------------------------------------------------------
// The name an old entry is kept under when the new archive has a different KIND of entry with its name (user decision
// 2026-11-27): "<name>.old", then ".old2", ".old3", ... The first one that no entry of the new folder, the old folder or an
// earlier choice has, compared with foldKeyUnicode. Deterministic (the old names are visited in sorted order).
std::string keptName(const std::string& name, std::set<std::string>& taken) {
	for (int n = 1;; n++) {
		const std::string cand = name + (n == 1 ? std::string(".old") : ".old" + std::to_string(n));
		if (taken.insert(foldKeyUnicode(cand)).second) return cand;
	}
}

bool carryRec(const ListFn& o, const ListFn& n, const std::string& oldRel, const std::string& newRel, int depth, size_t maxItems,
              CarryResult& r) {
	if (depth > kRemoveMaxDepth) { r.error = "folders nested too deeply"; r.fail = CarryFail::TooDeep; return false; }
	std::vector<DirEntry> oe, ne;
	if (!o(oldRel, oe)) { r.error = "cannot read the old folder " + oldRel; r.fail = CarryFail::Unreadable; return false; }
	if (!n(newRel, ne)) { r.error = "cannot read the new folder " + newRel; r.fail = CarryFail::Unreadable; return false; }
	std::map<std::string, DirEntry> newByKey;
	std::set<std::string> taken;
	for (const DirEntry& e : ne) { newByKey[foldKeyUnicode(e.name)] = e; taken.insert(foldKeyUnicode(e.name)); }
	for (const DirEntry& e : oe) taken.insert(foldKeyUnicode(e.name));
	std::sort(oe.begin(), oe.end(), [](const DirEntry& a, const DirEntry& b) { return a.name < b.name; });
	auto join = [](const std::string& dir, const std::string& name) { return dir.empty() ? name : dir + "/" + name; };
	for (const DirEntry& e : oe) {
		const auto it = newByKey.find(foldKeyUnicode(e.name));
		auto add = [&](const std::string& dest) {
			if (r.paths.size() >= maxItems) { r.error = "too many items to carry over"; r.fail = CarryFail::TooMany; return false; }
			r.paths.push_back(dest);
			return true;
		};
		if (it == newByKey.end()) {
			if (!add(join(newRel, e.name))) return false;
		} else if (e.isDir && it->second.isDir) {
			if (!carryRec(o, n, join(oldRel, e.name), join(newRel, it->second.name), depth + 1, maxItems, r)) return false;
		} else if (e.isDir != it->second.isDir) {
			// A folder against a file of the same name: the old one is the user's data, so it is kept under a new name.
			const std::string to = join(newRel, keptName(e.name, taken));
			if (!add(to)) return false;
			r.renamed.push_back({join(oldRel, e.name), to});
		}
		// two files with one name: the new one is the shipped one; the old one goes with the old tree
	}
	return true;
}

// ---- executing and undoing ops ------------------------------------------------------------------------------------------
bool copyStagedManifest(const std::string& root, const Journal& j, const FileOps& ops, std::string& err) {
	const rec::Read rd = rec::readRecord(joinRoot(root, stagedManifestPath(j.stagingDir)), kMaxManifestBytes, ops);
	if (rd.result != rec::ReadResult::Ok) { err = "the staged manifest cannot be read"; return false; }
	const ManifestParse p = parseManifest(rd.text);
	if (!p.ok || foldKey(p.manifest.slug) != foldKey(j.slug) || p.manifest.device != j.device) {
		err = "the staged manifest is not valid";
		return false;
	}
	if (!rec::writeRecord(manifestPath(root, j.slug), rd.text, kMaxManifestBytes, ops)) {
		err = "cannot write installed/" + j.slug + ".json";
		return false;
	}
	return true;
}

// What is at the two places of an op. A failing stat is an ERROR (the op fails), never "absent": absent means "skip", and
// skipping on a flaky card could leave the old folder parked or the new one half placed.
struct Probe {
	bool pE = false, oE = false;
};
bool probe(const FileOps& ops, const std::string& P, const std::string& O, Probe& out, std::string& err) {
	const Presence p = presenceOf(P, ops);
	const Presence o = O.empty() ? Presence::Absent : presenceOf(O, ops);
	if (p == Presence::Error || o == Presence::Error) {
		err = "cannot tell what is at " + (p == Presence::Error ? P : O);
		return false;
	}
	out.pE = p == Presence::Present;
	out.oE = o == Presence::Present;
	return true;
}

// The renameDir that follows a parkDir of `path`.
const JournalOp* renameOf(const Journal& j, const std::string& path) {
	for (const JournalOp& op : j.ops)
		if (op.type == OpType::RenameDir && foldKeyUnicode(op.path) == foldKeyUnicode(path)) return &op;
	return nullptr;
}

bool forwardOp(const std::string& root, const Journal& j, const JournalOp& op, const FileOps& ops, std::string& err) {
	const std::string P = joinRoot(root, op.path);
	const std::string O = op.other.empty() ? std::string() : joinRoot(root, op.other);
	auto bad = [&](const char* why) {
		err = std::string(opName(op.type)) + " " + op.path + ": " + why;
		return false;
	};
	if (op.type == OpType::WriteManifest) return copyStagedManifest(root, j, ops, err);
	Probe pr;
	std::string perr;
	if (!probe(ops, P, O, pr, perr)) { err = std::string(opName(op.type)) + " " + op.path + ": " + perr; return false; }
	const bool pE = pr.pE, oE = pr.oE;
	switch (op.type) {
	case OpType::ParkDir: {
		if (!pE) return true; // parked already (or nothing was there)
		if (oE) return true;  // both exist: the replace is done and `other` is left over (FileUtil.h recovery rule)
		// Moving the old app dir away only makes sense while its replacement is still staged (a crafted or damaged journal
		// must not be able to take an app away and put nothing in its place).
		const JournalOp* rn = renameOf(j, op.path);
		if (!rn) return bad("no renameDir follows");
		const Presence st = presenceOf(joinRoot(root, rn->other), ops);
		if (st == Presence::Error) return bad("cannot tell whether the staged replacement is there");
		if (st != Presence::Present) return bad("the staged replacement folder is gone");
		return parkDir(P, O, ops) || bad("cannot move the old folder aside");
	}
	case OpType::ParkFile:
		if (!pE || oE) return true;
		return moveEntry(ops, P, O) || bad("cannot move the old file aside");
	case OpType::RenameDir:
		if (pE && !oE) return true;
		if (!pE && oE) return renameStaged(O, P, ops) || bad("cannot move the new folder into place");
		return bad(pE ? "both the staged and the final folder exist" : "the staged folder is gone");
	case OpType::WriteFile:
		if (pE && !oE) return true;
		if (!pE && oE) return moveEntry(ops, O, P) || bad("cannot move the new file into place");
		return bad(pE ? "both the staged and the final file exist" : "the staged file is gone");
	case OpType::RestoreFile:
		if (pE && !oE) return true;
		if (!pE && oE) return moveEntry(ops, O, P) || bad("cannot carry the old file over");
		// In neither tree: something removed it (or a stat lied). Not a "skip": the update is rolled back, saves intact.
		return bad(pE ? "exists in both trees" : "is in neither tree");
	case OpType::Mkdir:
		if (pE) return isRealDirAt(ops, P) || bad("a file is in the way");
		return makeDirs(P, ops) || bad("cannot create the folder");
	case OpType::WriteManifest: break;
	}
	return bad("unknown op");
}

// True when the app dir that `restorePath` lies under was parked (its parkDir op's `other` exists). A restoreFile can only have
// run after that; without the check "path exists, other is missing" would also describe a restoreFile that never ran while the
// OLD folder is still in place, and undoing it would move the user's own files out of it. `parked` is computed once per
// rollBack (not per op: that was O(ops) stats for every restoreFile).
bool parkedAbove(const std::vector<std::string>& parked, const std::string& restorePath) {
	for (const std::string& d : parked)
		if (pathStrictlyUnder(restorePath, d)) return true;
	return false;
}

// True when the parent of `O` (inside this job's staging) is a real folder. renameStaged/moveEntry would CREATE a missing
// parent, so an undo that moves something INTO staging first checks that the staged tree it came from is really there; a
// journal that names an app dir with no staged tree must not be able to move that app into a freshly made staging folder.
bool stagedParentThere(const FileOps& ops, const std::string& O, bool& error) {
	error = false;
	FileStat st;
	const Presence p = presenceOf(parentOf(O), ops, &st);
	if (p == Presence::Error) { error = true; return false; }
	return p == Presence::Present && st.isDir && !st.isLink;
}

bool backwardOp(const std::string& root, const std::vector<std::string>& parked, const JournalOp& op, const FileOps& ops, std::string& err) {
	if (op.type == OpType::WriteManifest) return true; // never copied before a rollback (it is the last op and a cut there rolls forward)
	const std::string P = joinRoot(root, op.path);
	const std::string O = op.other.empty() ? std::string() : joinRoot(root, op.other);
	auto bad = [&](const std::string& why) {
		err = std::string("undo ") + opName(op.type) + " " + op.path + ": " + why;
		return false;
	};
	Probe pr;
	std::string perr;
	if (!probe(ops, P, O, pr, perr)) return bad(perr);
	const bool pE = pr.pE, oE = pr.oE;
	switch (op.type) {
	case OpType::Mkdir:
		if (op.created && pE && isRealDirAt(ops, P) && !removeDirIfEmpty(P, ops)) LOG("commit: kept non-empty folder %s", op.path.c_str());
		return true;
	case OpType::WriteFile:
	case OpType::RenameDir:
		if (pE && !oE) {
			bool chkErr = false;
			if (!stagedParentThere(ops, O, chkErr)) {
				if (chkErr) return bad("cannot check the staged tree");
				LOG("commit: not undoing %s %s: its staged tree is not there", opName(op.type), op.path.c_str());
				return true;
			}
			if (op.type == OpType::WriteFile) return moveEntry(ops, P, O) || bad("cannot move it back");
			return renameStaged(P, O, ops) || bad("cannot move the new folder back");
		}
		return true;
	case OpType::RestoreFile:
		if (pE && !oE && parkedAbove(parked, op.path)) return moveEntry(ops, P, O) || bad("cannot move it back");
		return true;
	case OpType::ParkFile:
		if (!pE && oE) return moveEntry(ops, O, P) || bad("cannot put the old file back");
		return true;
	case OpType::ParkDir:
		if (!pE && oE) return restoreParked(O, P, ops) || bad("cannot put the old folder back");
		return true;
	case OpType::WriteManifest: break;
	}
	return true;
}

bool removeStaging(const std::string& root, int64_t jobId, const std::vector<std::string>& removeRoots, const FileOps& ops) {
	const RemoveResult r = removeTree(stagingAbs(root, jobId), stagingBase(root), removeRoots, ops);
	if (!r.ok) LOG("commit: staging %lld not fully removed: %s", (long long)jobId, r.error.c_str());
	return r.ok;
}

// Renames `from` to `to` inside `dir` (they differ only by case): two steps, so FAT sees two different names.
bool renameStagedCase(const FileOps& ops, const std::string& dir, const std::string& from, const std::string& to) {
	const std::string tmp = dir + "/.openshop-rename";
	return moveEntry(ops, dir + "/" + from, tmp) && moveEntry(ops, tmp, dir + "/" + to);
}

// Gives the staged entries the spelling of the entries that are on the device already (FAT keeps the case it was created with
// and matches without it; ext4 would put "config" next to "Config"), so a PC test run and the Wii do the same. Only staging is
// renamed. `stagedDir` and `deviceDir` are absolute; a device dir that is not there needs nothing. False with `err` on a real
// error (a failing list or stat).
bool conformCase(const FileOps& ops, const std::string& stagedDir, const std::string& deviceDir, int depth, std::string& err) {
	if (depth > kRemoveMaxDepth) { err = "folders nested too deeply"; return false; }
	FileStat dst;
	const Presence dp = presenceOf(deviceDir, ops, &dst);
	if (dp == Presence::Error) { err = "cannot stat " + deviceDir; return false; }
	if (dp == Presence::Absent || !dst.isDir) return true;
	std::vector<std::string> staged, dev;
	if (!listDir(stagedDir, staged, ops)) { err = "cannot read " + stagedDir; return false; }
	if (!listDir(deviceDir, dev, ops)) { err = "cannot read " + deviceDir; return false; }
	std::sort(staged.begin(), staged.end());
	std::sort(dev.begin(), dev.end());
	for (const std::string& s : staged) {
		std::string real = s;
		if (!std::binary_search(dev.begin(), dev.end(), s)) {
			for (const std::string& d : dev)
				if (pathroot::equalsNoCase(d, s)) { real = d; break; }
			if (real != s) {
				if (std::binary_search(staged.begin(), staged.end(), real)) { err = "two staged names differ only by case: " + s; return false; }
				if (!renameStagedCase(ops, stagedDir, s, real)) { err = "cannot rename the staged " + s + " to " + real; return false; }
			}
		}
		FileStat a, b;
		if (presenceOf(stagedDir + "/" + real, ops, &a) == Presence::Error || presenceOf(deviceDir + "/" + real, ops, &b) == Presence::Error) {
			err = "cannot stat " + real;
			return false;
		}
		if (a.exists && b.exists && a.isDir && b.isDir && !a.isLink && !b.isLink &&
		    !conformCase(ops, stagedDir + "/" + real, deviceDir + "/" + real, depth + 1, err))
			return false;
	}
	return true;
}

bool underFold(const std::string& rel, const std::string& dir) { return foldKeyUnicode(rel) == foldKeyUnicode(dir) || pathStrictlyUnder(rel, dir); }

} // namespace

std::string stagingBase(const std::string& root) { return joinRoot(root, "apps/OpenShop/staging"); }
std::string stagingAbs(const std::string& root, int64_t jobId) { return joinRoot(root, stagingRelPath(jobId)); }

int64_t freshJobId(const std::string& root, int64_t wanted, const FileOps& ops) {
	const int64_t first = wanted < 0 ? 0 : wanted;
	for (int64_t id = first; id < first + 1000000; id++) {
		const Presence p = presenceOf(stagingAbs(root, id), ops);
		if (p == Presence::Error) return -1;
		if (p == Presence::Absent) return id;
	}
	return -1;
}

CarryResult carryOver(const ListFn& oldSide, const ListFn& newSide, size_t maxItems) {
	CarryResult r;
	r.ok = carryRec(oldSide, newSide, "", "", 0, maxItems, r);
	if (!r.ok) { r.paths.clear(); r.renamed.clear(); }
	return r;
}

bool stagedManifestOk(const std::string& root, const Journal& j, const FileOps& ops) {
	const rec::Read rd = rec::readRecord(joinRoot(root, stagedManifestPath(j.stagingDir)), kMaxManifestBytes, ops);
	if (rd.result != rec::ReadResult::Ok) return false;
	const ManifestParse p = parseManifest(rd.text);
	return p.ok && foldKey(p.manifest.slug) == foldKey(j.slug) && p.manifest.device == j.device;
}

bool rollForward(const std::string& root, const Journal& j, const FileOps& ops, std::string& err) {
	for (const JournalOp& op : j.ops)
		if (!forwardOp(root, j, op, ops, err)) return false;
	return true;
}

bool rollBack(const std::string& root, const Journal& j, const FileOps& ops, std::string& err) {
	std::vector<std::string> parked; // app dirs whose old tree is in staging: computed ONCE (not per restoreFile)
	for (const JournalOp& op : j.ops) {
		if (op.type != OpType::ParkDir) continue;
		const Presence p = presenceOf(joinRoot(root, op.other), ops);
		if (p == Presence::Error) { err = "undo parkDir " + op.path + ": cannot tell whether the old folder is parked"; return false; }
		if (p == Presence::Present) parked.push_back(op.path);
	}
	for (size_t i = j.ops.size(); i-- > 0;)
		if (!backwardOp(root, parked, j.ops[i], ops, err)) return false;
	return true;
}

bool writeStartJournal(const std::string& root, const std::string& device, int64_t jobId, JournalKind kind, const std::string& slug,
                       const FileOps& ops) {
	Journal j;
	j.phase = JournalPhase::Start;
	j.jobId = jobId;
	j.kind = kind;
	j.slug = slug;
	j.device = device;
	j.stagingDir = stagingRelPath(jobId);
	return saveJournal(root, j, ops);
}

CommitResult commit(const CommitInput& in) {
	CommitResult r;
	const FileOps& ops = in.fileOps;
	auto fail = [&](CommitError e, const std::string& d) {
		r.ok = false;
		r.error = e;
		r.detail = d;
		LOG("commit: refused (%d): %s", (int)e, d.c_str());
		return r;
	};
	auto phase = [&](const char* name) { if (in.onPhase) in.onPhase(name); };
	if (in.root.empty() || (in.device != "sd" && in.device != "usb") || !isValidSlug(in.slug) || in.jobId < 0 || in.plan.appDirs.empty())
		return fail(CommitError::BadInput, "bad arguments");
	const std::string stage = stagingAbs(in.root, in.jobId);
	const std::string stageRel = stagingRelPath(in.jobId);
	const std::string stRoot = stage + "/root";
	if (!isRealDirAt(ops, stRoot)) return fail(CommitError::BadInput, "no staged tree at " + stRoot);
	for (const char* leftover : {"/old", "/manifest.json"}) {
		const Presence lp = presenceOf(stage + leftover, ops);
		if (lp == Presence::Error) return fail(CommitError::Io, std::string("cannot check ") + stage + leftover);
		if (lp == Presence::Present)
			return fail(CommitError::BadInput, "the staging dir is not clean (old/ or manifest.json exists): a leftover of an earlier job with this id");
	}

	// ---- plan -------------------------------------------------------------------------------------------------------
	Journal j;
	j.phase = JournalPhase::Commit;
	j.jobId = in.jobId;
	j.kind = in.update ? JournalKind::Update : JournalKind::Install;
	j.slug = in.slug;
	j.device = in.device;
	j.stagingDir = stageRel;
	Manifest neu;
	neu.slug = in.slug;
	neu.device = in.device;
	neu.version = in.version;
	neu.installedAt = in.installedAt;
	neu.archiveMd5 = in.archiveMd5;
	std::string statProblem;           // the first path whose stat failed while the plan was made: the update is refused
	std::vector<std::string> keptFrom; // root-relative old entries that are moved to another name (not at their old path afterwards)
	std::vector<CommitResult::Kept> kept;
	// "Is it on the device AFTER the commit", as far as the old tree goes: an old entry that is kept under another name is no
	// longer at its path (nor is anything inside a kept folder).
	auto exists = [&](const std::string& rel) {
		for (const std::string& k : keptFrom)
			if (underFold(rel, k)) return false;
		const Presence p = presenceOf(joinRoot(in.root, rel), ops);
		if (p == Presence::Error) {
			if (statProblem.empty()) statProblem = rel;
			return true; // keeps the entry; the plan is refused below
		}
		return p == Presence::Present;
	};
	auto walkFail = [&](const std::string& err, bool unsafe) {
		return fail(unsafe ? CommitError::Unsafe : CommitError::Io, err);
	};

	std::vector<std::string> appNames;
	if (!listDir(joinRoot(in.root, "apps"), appNames, ops)) return fail(CommitError::Io, "cannot read the apps folder");
	std::vector<std::string> sortedApps = appNames;
	std::sort(sortedApps.begin(), sortedApps.end());
	std::set<std::string> createdAppDirs; // fold keys of the app dirs this commit creates
	std::set<std::string> seenDirs;
	for (const ZipPlan::AppDir& ad : in.plan.appDirs) {
		const std::string D = ad.name;
		if (!isAppDirPath("apps/" + D)) return fail(CommitError::Unsafe, "bad app folder name " + D);
		if (!seenDirs.insert(foldKeyUnicode(D)).second) return fail(CommitError::Conflict, "two app folders named " + D);
		if (!isRealDirAt(ops, stRoot + "/apps/" + D)) return fail(CommitError::BadInput, "apps/" + D + " is not in the staged tree");
		// The device's spelling of this app dir, from the listing that was checked above (a failing listing must never read as
		// "not there": findCaseInsensitive answers nullopt for both).
		std::optional<std::string> real;
		for (const std::string& n : sortedApps)
			if (n == D) { real = n; break; }
		if (!real)
			for (const std::string& n : sortedApps)
				if (pathroot::equalsNoCase(n, D)) { real = n; break; }
		const bool existing = real.has_value();
		std::string T = D;
		if (existing) {
			T = *real;
			if (!isRealDirAt(ops, joinRoot(in.root, "apps/" + T))) return fail(CommitError::Conflict, "apps/" + T + " is not a folder");
			if (!in.update) return fail(CommitError::Exists, "apps/" + T + " already exists");
			if (T != D && !renameStagedCase(ops, stRoot + "/apps", D, T))
				return fail(CommitError::Io, "cannot rename the staged folder to " + T);
		}
		const std::string rel = "apps/" + T;
		if (!existing) createdAppDirs.insert(foldKeyUnicode(rel));

		// The carry-over first: it decides which old entries are kept under another name, and that decides what `exists` says.
		std::vector<std::string> carried;
		std::map<std::string, std::string> sourceOf; // carried path (relative to the app dir) -> old path (relative to the app dir)
		if (existing) {
			const std::string oldAbs = joinRoot(in.root, rel), newAbs = stRoot + "/" + rel;
			const CarryResult cr = carryOver(
			    [&](const std::string& d, std::vector<DirEntry>& out) { return listEntries(ops, d.empty() ? oldAbs : oldAbs + "/" + d, out); },
			    [&](const std::string& d, std::vector<DirEntry>& out) { return listEntries(ops, d.empty() ? newAbs : newAbs + "/" + d, out); });
			if (!cr.ok)
				return fail(cr.fail == CarryFail::TooMany ? CommitError::TooMany : cr.fail == CarryFail::TooDeep ? CommitError::Unsafe : CommitError::Io,
				            "cannot carry the old files over: " + cr.error);
			carried = cr.paths;
			for (const CarryResult::Renamed& rn : cr.renamed) {
				sourceOf[rn.to] = rn.from;
				keptFrom.push_back(rel + "/" + rn.from);
				kept.push_back({rel + "/" + rn.from, rel + "/" + rn.to});
				LOG("commit: the old %s/%s is kept as %s (the new archive has an entry of another kind with that name)", rel.c_str(),
				    rn.from.c_str(), rn.to.c_str());
			}
			// Names the records would refuse (an odd old file name, or a kept name that got too long) are refused up front:
			// the parked old tree is deleted afterwards, so nothing may be dropped.
			for (const std::string& p : carried) {
				const auto it = sourceOf.find(p);
				if (!isTargetFilePath(rel + "/" + p) || (it != sourceOf.end() && !isTargetFilePath(rel + "/" + it->second)))
					return fail(CommitError::NameRefused, "the existing entry " + rel + "/" + (it != sourceOf.end() ? it->second : p) +
					                                          " has a name that cannot be kept or recorded; rename or remove it and try again");
			}
		}

		std::vector<Entry> tree;
		std::string err;
		bool unsafe = false;
		if (!walkTree(ops, stRoot + "/" + rel, "", 0, tree, err, unsafe)) return walkFail(err, unsafe);

		ManifestDir md;
		md.path = rel;
		md.created = !existing;
		for (const Entry& e : tree) {
			if (!e.isDir) md.files.push_back({rel + "/" + e.rel, true});
			else if (!existing || !exists(rel + "/" + e.rel)) md.createdSubdirs.push_back({rel + "/" + e.rel, true});
		}
		neu.dirs.push_back(std::move(md));

		if (existing) {
			JournalOp pk;
			pk.type = OpType::ParkDir;
			pk.path = rel;
			pk.other = stagedOldPath(stageRel, rel);
			j.ops.push_back(pk);
		}
		JournalOp rn;
		rn.type = OpType::RenameDir;
		rn.path = rel;
		rn.other = stagedRootPath(stageRel, rel);
		j.ops.push_back(rn);
		for (const std::string& p : carried) {
			const auto it = sourceOf.find(p);
			JournalOp rf;
			rf.type = OpType::RestoreFile;
			rf.path = rel + "/" + p;
			rf.other = stagedOldPath(stageRel, rel + "/" + (it != sourceOf.end() ? it->second : p));
			j.ops.push_back(std::move(rf));
		}
		if (j.ops.size() > kMaxJournalOps)
			return fail(CommitError::TooMany, "too many files to update safely (more than " + std::to_string(kMaxJournalOps) + " steps)");
	}

	for (const ZipPlan::Folder& f : in.plan.otherFolders) {
		std::string top = f.name;
		if (!isOutsidePath(top)) return fail(CommitError::Unsafe, "bad folder name " + top);
		// The device's spelling wins (FAT matches case-insensitively; so must the PC): rename the staged entries to it first.
		{
			std::vector<std::string> rootNames;
			if (!listDir(in.root, rootNames, ops)) return fail(CommitError::Io, "cannot read the device root");
			std::sort(rootNames.begin(), rootNames.end());
			if (!std::binary_search(rootNames.begin(), rootNames.end(), top)) {
				for (const std::string& d : rootNames)
					if (pathroot::equalsNoCase(d, top)) {
						const Presence sp = presenceOf(stRoot + "/" + top, ops);
						if (sp == Presence::Error) return fail(CommitError::Io, "cannot stat the staged " + top);
						if (sp == Presence::Present && !renameStagedCase(ops, stRoot, top, d))
							return fail(CommitError::Io, "cannot rename the staged " + top + " to " + d);
						top = d;
						break;
					}
			}
			if (!isOutsidePath(top)) return fail(CommitError::Unsafe, "bad folder name " + top);
		}
		FileStat st;
		if (presenceOf(stRoot + "/" + top, ops, &st) == Presence::Error) return fail(CommitError::Io, "cannot stat the staged " + top);
		if (!st.exists) { LOG("commit: %s was not extracted (not confirmed?), skipped", top.c_str()); continue; }
		if (!st.isDir || st.isLink) return fail(CommitError::Unsafe, "the staged " + top + " is not a folder");
		std::string cerr;
		if (!conformCase(ops, stRoot + "/" + top, joinRoot(in.root, top), 1, cerr)) return fail(CommitError::Io, cerr);
		std::vector<Entry> tree{{top, true}};
		std::string err;
		bool unsafe = false;
		if (!walkTree(ops, stRoot + "/" + top, top, 1, tree, err, unsafe)) return walkFail(err, unsafe);
		for (const Entry& e : tree) {
			FileStat dst;
			if (presenceOf(joinRoot(in.root, e.rel), ops, &dst) == Presence::Error) return fail(CommitError::Io, "cannot stat " + e.rel);
			if (e.isDir) {
				if (dst.exists && !dst.isDir) return fail(CommitError::Conflict, e.rel + " is a file on the device");
				if (dst.exists) continue;
				JournalOp mk;
				mk.type = OpType::Mkdir;
				mk.path = e.rel;
				mk.created = true;
				j.ops.push_back(mk);
				neu.createdDirs.push_back({e.rel, true});
				continue;
			}
			if (dst.exists && dst.isDir) return fail(CommitError::Conflict, e.rel + " is a folder on the device");
			if (dst.exists) {
				JournalOp pk;
				pk.type = OpType::ParkFile;
				pk.path = e.rel;
				pk.other = stagedOldPath(stageRel, e.rel);
				j.ops.push_back(pk);
			}
			JournalOp wf;
			wf.type = OpType::WriteFile;
			wf.path = e.rel;
			wf.other = stagedRootPath(stageRel, e.rel);
			wf.preexisting = dst.exists;
			j.ops.push_back(wf);
			neu.files.push_back({e.rel, dst.exists, true});
			if (j.ops.size() > kMaxJournalOps)
				return fail(CommitError::TooMany, "too many files to update safely (more than " + std::to_string(kMaxJournalOps) + " steps)");
		}
	}

	JournalOp wm;
	wm.type = OpType::WriteManifest;
	wm.path = manifestRelPath(in.slug);
	wm.other = stagedManifestPath(stageRel);
	j.ops.push_back(wm);
	r.ops = (int)j.ops.size();
	phase("walk");

	// Dry runs: nothing has changed on the device yet, so a plan that does not fit is simply refused. A problem that is not a
	// size problem (a name the records refuse) is Unsafe, not "too many".
	auto refuseProblem = [&](const char* what, const std::string& p) {
		const bool size = p.find("over the cap") != std::string::npos || p.find("too many") != std::string::npos;
		return fail(size ? CommitError::TooMany : CommitError::Unsafe,
		            size ? "too many files to update safely (" + std::string(what) + ": " + p + ")" : std::string("the ") + what + " cannot be recorded: " + p);
	};
	Manifest finalM = in.update ? mergeForUpdate(in.oldManifest, neu, exists) : neu;
	if (in.update && !in.oldManifest) {
		// No old manifest: mergeForUpdate records every dir as one that was already there. The dirs this commit created are ours.
		for (ManifestDir& d : finalM.dirs)
			if (createdAppDirs.count(foldKeyUnicode(d.path))) { d.created = true; d.adopted = false; }
	}
	validateManifest(finalM);
	phase("merge");
	if (!statProblem.empty()) return fail(CommitError::Io, "cannot check " + statProblem);
	std::string problem = manifestSaveProblem(finalM);
	if (!problem.empty()) return refuseProblem("manifest", problem);
	phase("dryrun-manifest");
	problem = journalSaveProblem(j);
	if (!problem.empty()) return refuseProblem("journal", problem);
	phase("dryrun-journal");

	// ---- write the plan ---------------------------------------------------------------------------------------------
	if (!rec::writeRecord(joinRoot(in.root, stagedManifestPath(stageRel)), manifestToJson(finalM), kMaxManifestBytes, ops))
		return fail(CommitError::Io, "cannot write the staged manifest");
	// The journal is written ONCE, here, before the first rename (review M4: no periodic rewrite; recovery probes the real state).
	if (!saveJournal(in.root, j, ops)) return fail(CommitError::Io, "cannot write the journal");
	phase("journal");

	// ---- execute ----------------------------------------------------------------------------------------------------
	for (size_t i = 0; i < j.ops.size(); i++) {
		std::string err;
		if (!forwardOp(in.root, j, j.ops[i], ops, err)) {
			LOG("commit: op %zu failed (%s), rolling back", i, err.c_str());
			r.error = CommitError::OpFailed;
			r.detail = err;
			std::string rerr;
			if (rollBack(in.root, j, ops, rerr)) {
				r.rolledBack = true;
				if (removeJournal(in.root, ops)) removeStaging(in.root, in.jobId, in.removeRoots, ops);
				else r.journalLeft = true;
			} else {
				LOG("commit: rollback failed (%s); the journal stays for recovery", rerr.c_str());
				r.journalLeft = true;
				r.detail += "; rollback: " + rerr;
			}
			return r;
		}
		if (in.afterOp) in.afterOp((int)i);
	}
	phase("ops");

	// ---- finish -----------------------------------------------------------------------------------------------------
	r.ok = true;
	r.kept = std::move(kept);
	if (!removeJournal(in.root, ops)) {
		LOG("commit: the journal could not be removed; recovery will find everything done");
		return r;
	}
	r.stagingRemoved = removeStaging(in.root, in.jobId, in.removeRoots, ops);
	phase("done");
	return r;
}

} // namespace install
