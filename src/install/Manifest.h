// SPDX-License-Identifier: GPL-3.0-or-later
// The per-app install manifest (M3 Increment 5): "<root>/apps/OpenShop/installed/<slug>.json", jansson, formatVersion 1.
// It records what one install wrote, so Delete (Increment 12) and Update (Increment 6) never have to guess. No SDL; links
// jansson, core/FileUtil.cpp, core/NameRules.cpp, install/Rules.cpp and install/RecordIo.cpp.
//
// ON DISK (all paths are relative to the device root, '/' separated, validated by validateEntryName):
//   { "formatVersion": 1, "slug": "wiimc", "device": "sd"|"usb", "version": "...", "installedAt": <unix seconds>,
//     "archiveMd5": "" | 32 lowercase hex,
//     "dirs":  [ { "path": "apps/WiiMC", "created": true, "adopted": false,
//                  "files": ["apps/WiiMC/boot.dol", ...], "createdSubdirs": ["apps/WiiMC/skins", ...] } ],
//     "files": [ { "path": "config/wiimc/x.ini", "preexisting": false } ],
//     "createdDirs": [ "config", "config/wiimc" ] }
// A dir's files and createdSubdirs are full paths that sit strictly below the dir's own path. Unknown fields are ignored.
// At least one dirs[] entry is required (an install always writes an app folder; a manifest without one could never be
// deleted or updated sensibly) and two entries may not name the same folder (compared with foldKeyUnicode); both make the
// file Corrupt. files[] and dirs[].files entries are FILES: at least two path segments ("boot.elf" at the root is refused);
// createdDirs[] and createdSubdirs[] may be a single segment ("config").
//
// UNTRUSTED INPUT: files over kMaxManifestBytes (4 MB) are refused unread; before jansson sees the text, nesting (8) and the
// number of values (kMaxManifestValues; jansson takes ~40+ bytes per value) are pre-scanned, so a file of "[0,0,0,..." cannot
// make the parser allocate hundreds of MB. Every array and string is capped, duplicate JSON keys are rejected, and wrong types
// on required fields, a bad slug, a bad device or an unknown formatVersion make the whole file "corrupt" (the app is then
// treated as "no manifest"). The three FLAGS (created, adopted, preexisting) are lenient: a wrong type reads as the safe value
// (created=false, adopted=false, preexisting=true: never remove what we cannot vouch for). A PATH that fails validation does not
// make the file corrupt: it is kept as data with `valid=false`, so Delete can skip it with a log line instead of acting on it
// (and still knows it was there). The queries below ignore invalid paths.
//
// IDENTITY: path comparisons (queries, merge, containment, duplicate dirs) use foldKeyUnicode, because FAT folds more than
// ASCII ("config/Caf\xc3\xa9" and "config/CAF\xc3\x89" are one file). Slugs are always ASCII and use foldKey.
//
// DEVICE: every manifest says which device it was written for. loadManifest, listManifests and loadJournal take the device
// the caller reads from and treat a record for the other one as Corrupt (a copied card or a swapped folder is foreign, and
// Delete must never act on it).
//
// CRASH SAFETY: saves go to "<file>.tmp", are fsynced, then renamed over the record (commitFile; on libfat that parks the old
// one as "<file>.bak" for a moment). A cut leaves the old record, the new one, or only the ".bak"; loadManifest reads the
// ".bak" when the record itself is missing, and listManifests does the same. Removal deletes ".bak", ".tmp", then the record,
// so a cut part-way never leaves an older ".bak" to be resurrected. saveManifest validates by round trip: the text it writes
// has been parsed back successfully first.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/FileUtil.h"

namespace install {

constexpr int kManifestFormat = 1;
constexpr size_t kMaxManifestBytes = 4u * 1024 * 1024;
// All paths in one manifest together (dir files + subdirs + outside files + created dirs). An archive holds at most
// kMaxEntries (20000) entries and an update merges with the old list, so twice that.
constexpr size_t kMaxManifestPaths = 40000;
// Values (arrays, objects, separators) in the text: a path in files[] costs up to 3 ('{', the inner ',', the ','), plus slack.
constexpr size_t kMaxManifestValues = 3 * kMaxManifestPaths + 1024;
constexpr size_t kMaxManifestDirs = 64;
constexpr size_t kMaxManifestVersionBytes = 256;
constexpr size_t kMaxManifestsPerDevice = 4096; // listManifests stops here (a hostile folder)
// listManifests also stops when the manifests it kept hold this many paths, or it has read this many bytes, in total.
constexpr size_t kMaxListedPaths = 200000;
constexpr size_t kMaxListedBytes = 16u * 1024 * 1024;

// A path inside an app dir (a written file or a created subfolder).
struct ManifestPath {
	std::string path;
	bool valid = true; // not stored; computed by validateManifest (on load, and by the caller before use)
};

// A file outside apps/<dir> that the install wrote.
struct ManifestFile {
	std::string path;
	bool preexisting = false; // something was there before and was overwritten: Delete must not remove it
	bool valid = true;
};

struct ManifestDir {
	std::string path; // "apps/<dir>", the real on-disk casing
	bool created = false; // OpenShop created the folder (false: it was there already)
	bool adopted = false; // created==false but it is the app's own folder (name == slug) updated by OpenShop (user decision 2026-11-26)
	std::vector<ManifestPath> files;
	std::vector<ManifestPath> createdSubdirs;
	bool valid = true;
};

struct Manifest {
	int formatVersion = kManifestFormat;
	std::string slug;
	std::string device; // "sd" | "usb"
	std::string version;
	int64_t installedAt = 0;
	std::string archiveMd5;
	std::vector<ManifestDir> dirs;
	std::vector<ManifestFile> files;
	std::vector<ManifestPath> createdDirs;
};

// ---- path rules (shared with Journal) ---------------------------------------------------------------------------------
// All take a root-relative path and are false for anything validateEntryName refuses, for a trailing '/', and for a path that
// is not already in its normalised form.
bool isAppDirPath(std::string_view p);    // "apps/<dir>": two segments, not apps/OpenShop, not __MACOSX
bool isOutsidePath(std::string_view p);   // top folder is not apps, a system root, OpenShop's own, or __MACOSX
bool isTargetPath(std::string_view p);    // outside, or inside apps/<dir>/ (3+ segments, not under apps/OpenShop)
// The same two, but for FILES: at least two segments (a file directly in the device root such as "boot.elf" is refused).
bool isOutsideFilePath(std::string_view p);
bool isTargetFilePath(std::string_view p);
bool pathStrictlyUnder(std::string_view p, std::string_view dir); // by whole components, foldKeyUnicode

// Recomputes every `valid` flag from the rules above (a dir's files and subdirs must also lie under the dir).
void validateManifest(Manifest& m);

// ---- JSON -------------------------------------------------------------------------------------------------------------
// "" when a string is not valid UTF-8 (never a document with a field silently dropped); parseManifest("") fails.
std::string manifestToJson(const Manifest& m);
// Total paths the manifest holds (dir files + subdirs, outside files, created dirs).
size_t manifestPathCount(const Manifest& m);
// "" when saveManifest would write `m`: caps, fields, serialisable, under kMaxManifestBytes and parseManifest(manifestToJson(m))
// accepts it (so what is saved can always be loaded). Otherwise the first problem. Commit calls it (through dryRunMerge) before
// its first rename, so a manifest that cannot be saved is found while nothing has changed yet.
std::string manifestSaveProblem(const Manifest& m);

struct ManifestParse {
	bool ok = false;
	Manifest manifest;
	std::string error;
};
// `text` is untrusted. On success the valid flags are set.
ManifestParse parseManifest(std::string_view text);

// ---- files ------------------------------------------------------------------------------------------------------------
std::string manifestRelPath(const std::string& slug); // "apps/OpenShop/installed/<slug>.json"
std::string manifestPath(const std::string& root, const std::string& slug);

// False (nothing written) when manifestSaveProblem is not empty. Creates the installed/ folder.
bool saveManifest(const std::string& root, const Manifest& m, const FileOps& ops = {});

enum class LoadStatus { Ok, Missing, Corrupt };
struct ManifestLoad {
	LoadStatus status = LoadStatus::Missing;
	Manifest manifest;
	std::string error;       // for Corrupt
	bool fromBackup = false; // read from "<slug>.json.bak"
	size_t bytes = 0;        // size of the text that was read (listManifests budgets with it)
};
// `device` ("sd" | "usb") is the device `root` is. Corrupt covers an unreadable or oversized file, bad JSON, a slug that doesn't
// match `slug` (case-insensitively), and a manifest written for another device. Callers treat Missing and Corrupt alike as "no
// manifest" and may log or show the second.
ManifestLoad loadManifest(const std::string& root, const std::string& slug, std::string_view device, const FileOps& ops = {});

// Removes the record and its ".tmp"/".bak". True if none is left (a missing one counts).
bool removeManifest(const std::string& root, const std::string& slug, const FileOps& ops = {});

struct ManifestList {
	std::vector<Manifest> manifests;
	std::vector<std::string> corrupt; // slugs (file names) of records that could not be used
	bool truncated = false;           // stopped early: more than kMaxManifestsPerDevice files, or kMaxListedPaths / kMaxListedBytes
	bool listed = false;              // the installed/ folder could be read (a missing folder is listed=false, no manifests)
};
// Every "<slug>.json" (or lone "<slug>.json.bak") in installed/, for the device `device`. Other names (".journal", ".tmp",
// junk) are ignored. The whole list is bounded: at most kMaxManifestsPerDevice records, kMaxListedPaths paths in the manifests
// kept and kMaxListedBytes read; past any of them it stops and sets `truncated` (the manifest that would cross the path cap is
// not kept). With `truncated` the ownership queries below may MISS a manifest: callers must then refuse anything destructive
// that relies on them (Delete of shared files or folders, "Everything" scope) and treat unknown owners as "shared".
ManifestList listManifests(const std::string& root, std::string_view device, const FileOps& ops = {});

// ---- queries over the manifests of one device ---------------------------------------------------------------------------
// Slugs (in list order) of the manifests that list `path` anywhere: dirs[].files, dirs[].createdSubdirs, files[], createdDirs[]
// (not dirs[].path: that is dirOwners). Compared with foldKeyUnicode; invalid paths never match. `exceptSlug` skips one
// manifest (the app being deleted or updated).
// Build a ManifestIndex once per scan (folded path -> slugs, O(total paths)) and query it any number of times; the vector
// overloads build a throw-away index per call and suit a single query.
struct ManifestIndex {
	std::unordered_map<std::string, std::vector<std::string>> paths; // folded path (files, subdirs, outside files, created dirs)
	std::unordered_map<std::string, std::vector<std::string>> dirs;  // folded dirs[].path
};
ManifestIndex buildManifestIndex(const std::vector<Manifest>& all);
std::vector<std::string> manifestsReferencing(const ManifestIndex& index, std::string_view path, std::string_view exceptSlug = {});
std::vector<std::string> manifestsReferencing(const std::vector<Manifest>& all, std::string_view path,
                                              std::string_view exceptSlug = {});
// Slugs of the manifests with a dirs[].path equal to `dir` ("apps/WiiMC" finds a manifest that lists "apps/wiimc").
std::vector<std::string> dirOwners(const ManifestIndex& index, std::string_view dir, std::string_view exceptSlug = {});
std::vector<std::string> dirOwners(const std::vector<Manifest>& all, std::string_view dir, std::string_view exceptSlug = {});

// ---- update merge (pure) ----------------------------------------------------------------------------------------------
// `neu` is what this install wrote; `old` is the previous manifest of the app, or null when OpenShop didn't install the folder.
//  - scalar fields (slug, device, version, installedAt, archiveMd5) come from `neu`;
//  - with `old`: everything the old manifest listed stays listed (files, subdirs, created dirs the update no longer ships are
//    NOT deleted by an update; Delete removes them), the lists are the union (new entries first, then the old extras, no
//    duplicates by foldKey), and for a path present in both `created`, `adopted` and `preexisting` keep the OLD value. A dir
//    only in `neu` keeps its own flags; a dir only in `old` is kept whole;
//  - without `old` (update over a folder OpenShop didn't install; user decisions 2026-11-25 and 2026-11-26): every dir gets
//    created=false, and adopted=true only for the app's own folder (last segment equal to the slug, case-insensitive). A
//    folder replaced through the "Replace..." question is not adopted. The lists are `neu`'s own (only what this update wrote;
//    the caller leaves pre-existing subfolders out of createdSubdirs).
//  - `exists` (optional, root-relative path -> is it still on the device): an OLD entry that `neu` does not have is kept only
//    when it is still there, so entries for files the user has since deleted do not pile up towards the caps. Entries of `neu`
//    are never dropped. Without it every old entry is kept.
// A fresh install does not call this: its manifest is `neu` as is.
using ExistsFn = std::function<bool(const std::string& relPath)>;
Manifest mergeForUpdate(const Manifest* old, const Manifest& neu, const ExistsFn& exists = {});
// Plan-time dry run: the merge above followed by manifestSaveProblem. "" means the merged manifest can be saved. Commit calls
// it BEFORE its first rename (an update whose merged manifest would not fit the caps is refused up front, never half done).
std::string dryRunMerge(const Manifest* old, const Manifest& neu, const ExistsFn& exists = {});

} // namespace install
