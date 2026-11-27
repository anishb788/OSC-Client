// SPDX-License-Identifier: GPL-3.0-or-later
// The install journal (M3 Increment 5): "<root>/apps/OpenShop/installed/.journal", jansson, formatVersion 1. At most one per
// device (one job runs at a time). Increment 6's commit engine writes it and its startup recovery reads it. No SDL; links
// jansson, core/FileUtil.cpp, core/NameRules.cpp, install/Rules.cpp, install/Manifest.cpp and install/RecordIo.cpp.
//
// TWO PHASES
//   Start  (written before extraction): jobId, kind, slug, device, stagingDir. Recovery then always knows which staging to clean.
//   Commit (written and fsynced ONCE, before the first rename): the ordered ops plus `step` (always -1, see below).
//
// STEP IS INFORMATIONAL. Rewriting the journal is O(ops) and fsynced, so Commit writes the commit phase ONCE, before the first
// rename (step = -1), and never again (user decision 2026-11-27, review M4: a periodic rewrite only added fsyncs and a window in
// which the journal itself could be cut). `step` is therefore always -1 on disk; the field stays in the format ("at least
// ops[0..step] are done") for a later writer, and nothing may rely on it. Consequences, which Commit and recovery (Increment 6)
// honour:
//   - EVERY op is idempotent: repeating one that is already done (or half done) must be harmless;
//   - recovery never trusts `step`: it PROBES the real file system (does `path` exist, does `other` exist, is `other` a
//     half-copied tmp) for each op, from the first op on, and does or skips what is left. Rolling back walks the same ops in
//     reverse, probing the same way. A probe that cannot tell (a failing stat) is a failure of that op, never "absent".
//
// SIZE: kMaxJournalOps ops and kMaxJournalBytes bytes. A job whose plan would not fit (journalSaveProblem is not empty) is
// refused at planning time, before the first rename. parkFile and restoreFile may carry a whole subfolder as ONE plain rename
// (path is then a folder), so carry-over only recurses into folders that exist in BOTH the old and the new tree; a folder that
// exists on one side only is one op. mkdir may create folders below a new app dir too (apps/<dir>/sub).
//
// ON DISK
//   { "formatVersion": 1, "phase": "start"|"commit", "jobId": 7, "kind": "install"|"update", "slug": "...", "device": "sd"|"usb",
//     "stagingDir": "apps/OpenShop/staging/7",
//     "ops": [ { "op": "renameDir", "path": "apps/foo", "other": "apps/OpenShop/staging/7/root/apps/foo" }, ... ], "step": -1 }
// ("ops" and "step" only in the commit phase.) All paths are relative to the device root. The staging dir of job N is laid
// out like the device (the root/ tree is ZipExtract's output):
//   <staging>/root/<path>      what was extracted for <path>
//   <staging>/old/<path>       what was moved aside from <path> (parkDir, parkFile)
//   <staging>/manifest.json    the finished manifest of this install (written with writeRecord before the commit phase)
// Op meanings (path = the real place on the device, other = a place inside this job's staging dir, exactly as listed):
//   renameDir     other -> path        other = <staging>/root/<path>. A staged app dir becomes apps/<dir> (path is an app dir,
//                                      which must not exist; after a parkDir of it for an update)
//   parkDir       path -> other        other = <staging>/old/<path>. The existing app dir apps/<dir> is moved aside. Update only,
//                                      and a renameDir of the same path must follow
//   parkFile      path -> other        other = <staging>/old/<path>. An existing file (or folder) is moved aside before it is
//                                      overwritten
//   restoreFile   other -> path        other = <staging>/old/<path>. Carry-over of a parked file or whole subfolder back into a
//                                      new app dir. Update only; path lies below an app dir that was parked and renamed earlier.
//                                      Exception (user decision 2026-11-27): an old FOLDER that the new archive replaces by a FILE
//                                      of the same name (or an old file by a new folder) is kept under a new name: path = X.old
//                                      (X.old2, X.old3, ...) and other = <staging>/old/<X>. Nothing else may differ.
//   writeFile     other -> path        other = <staging>/root/<path>. A staged file outside apps/<dir> goes in; `preexisting`
//                                      says something was there, and then a parkFile of the same path comes earlier
//   mkdir         path                 a directory is created; `created` says it did not exist
//   writeManifest path, other          other = <staging>/manifest.json, path = the manifest path. Roll-forward COPIES the staged
//                                      manifest to installed/<slug>.json (user decision 2026-11-27: the finished manifest is on
//                                      disk before the first rename, so a cut after the renames can always be rolled forward).
//                                      Always exactly one, always the last op.
// writeFile, parkFile and restoreFile take FILE-shaped paths: at least two segments ("boot.elf" in the device root is refused).
// writeFile is for paths outside apps/ only (app dirs arrive whole with renameDir). mkdir may be a single segment ("config").
// An Install journal has no parkDir and no restoreFile (a new app dir has nothing to carry over).
// See FileUtil.h for the rule when a cut falls between parkDir and renameDir.
//
// UNTRUSTED: recovery acts on this file, so any problem makes the whole journal Corrupt: bad JSON, caps, wrong types (every
// flag too: unlike the manifest the journal has no lenient fields), a bad slug/device, a stagingDir that is not exactly
// staging/<jobId>, an op path that fails the Manifest path rules, an `other` that is not exactly the string listed above
// (so an op can never be bound to another job's staging or to a different path), a missing `preexisting` on writeFile, a
// step outside -1..ops-1, or ops that break the consistency rules above. Recovery must then leave it alone (and say so). A
// journal written for the other device than the one it is read from (loadJournal's `device`) is Corrupt too.
// Memory: the text is pre-scanned (nesting 8, kMaxJournalValues) before jansson sees it, as for manifests.
//
// CRASH SAFETY: same as Manifest.h (tmp, fsync, rename; the ".bak" is read when the file is missing; saves are validated by
// round trip first).
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/FileUtil.h"
#include "install/Manifest.h" // LoadStatus, the path rules

namespace install {

constexpr int kJournalFormat = 1;
constexpr size_t kMaxJournalBytes = 4u * 1024 * 1024;
constexpr size_t kMaxJournalOps = 20000;
// Values in the text: an op costs up to 6 ('{', four inner ',', the separating ','), plus slack.
constexpr size_t kMaxJournalValues = 6 * kMaxJournalOps + 1024;

enum class JournalPhase { Start, Commit };
enum class JournalKind { Install, Update };
enum class OpType { RenameDir, ParkDir, ParkFile, RestoreFile, WriteFile, Mkdir, WriteManifest };

struct JournalOp {
	OpType type = OpType::RenameDir;
	std::string path;
	std::string other;
	bool preexisting = true;  // WriteFile: the safe default; parseJournal requires the key on a writeFile
	bool created = false;     // Mkdir: false = "it was there" (never removed on rollback)
};

struct Journal {
	int formatVersion = kJournalFormat;
	JournalPhase phase = JournalPhase::Start;
	int64_t jobId = 0; // >= 0
	JournalKind kind = JournalKind::Install;
	std::string slug;
	std::string device; // "sd" | "usb"
	std::string stagingDir; // stagingRelPath(jobId)
	std::vector<JournalOp> ops; // Commit only
	int step = -1;              // Commit only: informational, always -1 on disk (see "STEP IS INFORMATIONAL")
};

const char* opName(OpType t);
std::string stagingRelPath(int64_t jobId); // "apps/OpenShop/staging/<jobId>"
std::string journalRelPath();              // "apps/OpenShop/installed/.journal"
// The exact `other` of the ops (stagingDir is stagingRelPath(jobId)).
std::string stagedRootPath(const std::string& stagingDir, const std::string& path); // <staging>/root/<path>
std::string stagedOldPath(const std::string& stagingDir, const std::string& path);  // <staging>/old/<path>
std::string stagedManifestPath(const std::string& stagingDir);                      // <staging>/manifest.json

std::string journalPath(const std::string& root);

// "" when the journal would be accepted by parseJournal, else the first problem (fields, op shapes, consistency).
std::string validateJournal(const Journal& j);
// "" when saveJournal would write `j`: validateJournal, serialisable (valid UTF-8), within kMaxJournalBytes, and
// parseJournal(journalToJson(j)) accepts it. Commit calls it on the full plan before its first rename.
std::string journalSaveProblem(const Journal& j);

// "" when a string is not valid UTF-8 (never a document with a field silently dropped).
std::string journalToJson(const Journal& j);

struct JournalParse {
	bool ok = false;
	Journal journal;
	std::string error;
};
JournalParse parseJournal(std::string_view text);

bool saveJournal(const std::string& root, const Journal& j, const FileOps& ops = {});

struct JournalLoad {
	LoadStatus status = LoadStatus::Missing; // Corrupt: leave the file alone
	Journal journal;
	std::string error;
	bool fromBackup = false;
};
// `device` is the device `root` is: a journal written for the other one is Corrupt.
JournalLoad loadJournal(const std::string& root, std::string_view device, const FileOps& ops = {});

// Removes the journal and its ".tmp"/".bak". True if none is left (a missing one counts).
bool removeJournal(const std::string& root, const FileOps& ops = {});

} // namespace install
