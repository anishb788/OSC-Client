// SPDX-License-Identifier: GPL-3.0-or-later
// install::Commit (M3 Increment 6): the ONLY code that renames or writes anything into "<dev>:/apps/" or another top-level
// folder. Input: "<root>/apps/OpenShop/staging/<jobId>/root/..." (ZipExtract's output) and the ZipExtract plan. No SDL; links
// libzip only through ZipExtract.h's types, jansson, core/FileUtil.cpp and the install records (Manifest, Journal, RecordIo).
//
// FLOW  commit(in)
//   1. PLAN (reads, plus renames INSIDE staging only; a failure changes nothing on the device): walk the staged tree and the
//      device, build the finished manifest (merged with the old one on update), dry-run it (dryRunMerge / manifestSaveProblem)
//      and the op list (journalSaveProblem). A plan that does not fit is refused here, "too many files to update safely".
//   2. Write the finished manifest to <staging>/manifest.json, then the commit-phase journal (fsynced) with step = -1.
//   3. Run the ops in order. The journal is NOT rewritten (step stays -1; review M4): recovery probes the real state.
//   4. Remove the journal, then the job's staging dir (a failed staging removal is only logged; Increment 7's Cleanup job
//      removes what is left).
//   An op that fails after step 2 rolls the job back in this process (rollBack), the same code recovery uses.
//
// OPS (see Journal.h for the exact shapes). Install: renameDir per app dir, mkdir / writeFile for other folders, writeManifest.
// Update adds parkDir (the existing apps/<real name> moves to <staging>/old/...) before the renameDir and restoreFile after it
// (the carry-over: everything in the old folder that the new archive does not contain, user saves and settings included; a
// whole subfolder is ONE restoreFile; folders that exist in both trees are entered). A name that is a FOLDER on one side and a
// FILE on the other is not "shipped over": the old entry is kept under "<name>.old" ("<name>.old2", ...) and listed in
// CommitResult::kept (user decision 2026-11-27); it stays UNTRACKED user data inside the app dir (not in the manifest), so
// "Everything in the app's folder" deletes it and "Only what OpenShop installed" keeps it. An existing outside file is parked
// (parkFile) before writeFile. Roll-forward = the remaining ops, then the staged manifest is copied to installed/<slug>.json.
//
// EVERY op is idempotent and probes the real state (does `path` exist, does `other` exist), so repeating the list after any cut
// does the right thing. A probe that FAILS (a stat error) is never "absent": the op fails, and the job is rolled back. Recovery (Recovery.h) uses rollForward when the staged manifest is intact and rollBack otherwise (or when
// a forward op cannot be done).
//
// CASE: an app dir that exists under another casing (device apps/WiiMC, archive apps/wiimc) is replaced under the EXISTING name:
// the staged dir is renamed to the real casing first (inside staging, in two steps so FAT sees a different name). Outside
// folders and the entries below them take the spelling that is on the device (staged entries are renamed to it inside staging), so
// a PC (ext4) run behaves like FAT.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/FileUtil.h"
#include "install/Journal.h"
#include "install/Manifest.h"
#include "install/ZipExtract.h"

namespace install {

// ---- carry-over (pure) -----------------------------------------------------------------------------------------------
struct DirEntry {
	std::string name;
	bool isDir = false;
};
// Lists ONE directory level of a tree; `dirRel` is relative to the tree's top ("" is the top). False if it cannot be read.
using ListFn = std::function<bool(const std::string& dirRel, std::vector<DirEntry>& out)>;
enum class CarryFail { None, TooMany, TooDeep, Unreadable };
struct CarryResult {
	bool ok = false;
	std::vector<std::string> paths; // destination paths in the NEW tree, relative to the top, in order; a folder is one item
	// Old entries kept under another name (path in the old tree -> destination, both relative to the top). Each `to` is also in
	// `paths`.
	struct Renamed {
		std::string from, to;
	};
	std::vector<Renamed> renamed;
	std::string error;
	CarryFail fail = CarryFail::None;
};
// What to move back from the old tree into the new one: every entry (file or whole folder) of the old tree that the new tree
// does not contain. A folder present on both sides is entered; two files with one name: the shipped one wins (the old one is
// dropped with the old tree); a folder against a file of the same name (either way round): the OLD entry is kept under
// "<name>.old", ".old2", ... (the first name that neither tree has, in the same folder) and reported in `renamed`. Names compare with foldKeyUnicode. At most `maxItems` items and 32
// levels (more is an error: the update is refused, not truncated).
CarryResult carryOver(const ListFn& oldSide, const ListFn& newSide, size_t maxItems = kMaxJournalOps);

// ---- commit -------------------------------------------------------------------------------------------------------------
enum class CommitError {
	None,
	BadInput,   // a missing or malformed argument, or no staged tree
	Exists,     // an install, but apps/<dir> is already there
	Conflict,   // a file where a folder must go (or the reverse), outside apps/
	Unsafe,     // a special file, a symlink, nesting or a plan the records would refuse for another reason than size
	NameRefused, // an existing entry's name (or the name it would be kept under) cannot be recorded; `detail` names it
	TooMany,    // only counts: the manifest or the journal would not fit its caps ("too many files to update safely")
	Io,         // the plan could not be written (staged manifest, journal)
	OpFailed,   // an op failed after the first rename; see rolledBack
};

struct CommitInput {
	std::string root;   // the device root: "sd:", "usb:", "./sdcard", ...
	std::string device; // "sd" | "usb"
	int64_t jobId = 0;  // the staging dir is <root>/apps/OpenShop/staging/<jobId>
	std::string slug, version, archiveMd5;
	int64_t installedAt = 0;
	bool update = false;                  // Update: park an existing app dir and carry its other files over
	const Manifest* oldManifest = nullptr; // update of an app OpenShop installed; null: nothing recorded (carry everything)
	ZipPlan plan;                          // ZipExtract's plan: which app dirs and other folders were extracted
	std::vector<std::string> removeRoots;  // FileUtil::removeTree's roots (platform::storageRoots() in the app)
	FileOps fileOps;
	// Test hook: called after op `index` has run and before the journal is rewritten (a cut here leaves step behind).
	void (*afterOp)(int index) = nullptr;
	// Dev/measurement hook (the Dolphin commit probe): called at "walk" (tree walked, ops listed), "merge" (manifest merged),
	// "dryrun-manifest", "dryrun-journal", "journal" (written), "ops" (all ops ran) and "done".
	void (*onPhase)(const char* phase) = nullptr;
};

struct CommitResult {
	bool ok = false;
	CommitError error = CommitError::None;
	std::string detail;
	bool rolledBack = false;     // OpFailed: everything was put back
	bool journalLeft = false;    // OpFailed and the rollback failed too: the journal and staging stay for recovery
	bool stagingRemoved = false; // ok: the job's staging dir is gone
	int ops = 0;                 // number of ops in the plan
	// ok: old entries kept under another name (root-relative paths), for Increment 9's "we kept your old folder as ..." notice.
	struct Kept {
		std::string from, to; // e.g. "apps/Foo/data" -> "apps/Foo/data.old"
	};
	std::vector<Kept> kept;
};
CommitResult commit(const CommitInput& in);

// The start-phase journal (job start, before extraction). False when it could not be written.
bool writeStartJournal(const std::string& root, const std::string& device, int64_t jobId, JournalKind kind, const std::string& slug,
                       const FileOps& ops = {});

// The smallest job id >= `wanted` with no staging folder (recovery leaves finished and abandoned staging for Increment 7's
// Cleanup job, and ZipExtract/commit refuse a staging dir that is not new). -1 when that cannot be told (a failing stat).
int64_t freshJobId(const std::string& root, int64_t wanted, const FileOps& ops = {});

std::string stagingBase(const std::string& root);                       // <root>/apps/OpenShop/staging
std::string stagingAbs(const std::string& root, int64_t jobId);         // <root>/apps/OpenShop/staging/<jobId>

// ---- shared by recovery ---------------------------------------------------------------------------------------------
// Runs every op of `j` that is not done yet, probing the real state of each (step is not used: a rollback that was cut half way
// and then rolled forward must still work). Ends with the copy of the staged manifest. False (with `err`) when an op cannot be
// done: the caller then rolls back.
bool rollForward(const std::string& root, const Journal& j, const FileOps& ops, std::string& err);
// Undoes the ops in reverse order, probing each. False (with `err`) when something could not be put back.
bool rollBack(const std::string& root, const Journal& j, const FileOps& ops, std::string& err);
// True when <staging>/manifest.json is readable, parses, and is the manifest of j.slug on j.device.
bool stagedManifestOk(const std::string& root, const Journal& j, const FileOps& ops);

} // namespace install
