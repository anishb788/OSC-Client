#pragma once
// Plain-stdio file helpers shared by the HTTP sinks, the platform layer and the catalog cache. No SDL, so the host
// tests link it on its own.
//
// The Wii's libfat cannot rename over an existing file, so every "replace a file" here goes through a tmp file and a
// backup dance (see commitFile). A power cut at any point leaves the old file, the new file, or the old file as
// "<path>.bak" (which the cache's reader knows to look for), never a half-written file under the real name.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <vector>

enum class ReadStatus { Ok, Missing, TooLarge, IoError };

// What `FileOps::stat` reports. `isLink` is only ever true on PC (the Wii has no symlinks).
struct FileStat {
	bool exists = false;
	bool isDir = false;
	bool isLink = false;
	uint64_t size = 0;
};

// Filesystem calls the writers, the catalog cache and the tree helpers make, replaceable so tests can force the failure
// paths. A null member means the real call (std::rename, std::remove, ::mkdir, ::rmdir, lstat, opendir/readdir,
// readFileCapped).
struct FileOps {
	int (*rename)(const char* from, const char* to) = nullptr;
	int (*remove)(const char* path) = nullptr;
	bool (*exists)(const char* path) = nullptr;
	// Only the catalog cache's reads go through this one.
	ReadStatus (*read)(const char* path, std::string& out, size_t maxBytes) = nullptr;
	// Used by the directory helpers below (makeDirs, listDir, removeTree, commitDir, ...). FileSink and the catalog
	// cache never touch these.
	int (*mkdir)(const char* path) = nullptr;
	int (*rmdir)(const char* path) = nullptr;
	// lstat semantics (never follows a symlink). 0 with `exists` false means "not there"; -1 is a real error.
	int (*stat)(const char* path, FileStat* out) = nullptr;
	// The entry names (no "." / "..") of a directory; false if it cannot be opened or read.
	bool (*list)(const std::string& dir, std::vector<std::string>& names) = nullptr;
	// Test hook only: the descriptor-based removeTree walk (PC, no stat/list/remove/rmdir injected) calls it after it has
	// listed `dir` and before it touches the entries, so a test can swap an entry for a symlink at the worst moment.
	void (*walkHook)(const std::string& dir) = nullptr;
	// writeFileAtomic only (tests): the fwrite of the data (a short count is a failed write), and the fsync before the rename
	// (false is a failed fsync).
	size_t (*write)(const void* data, size_t n, std::FILE* f) = nullptr;
	bool (*fsync)(std::FILE* f) = nullptr;
};

// Moves the finished tmp file over `to`. Without `keepBackup` a plain rename is tried first and the backup dance only
// runs when that fails because the target exists (libfat). With `keepBackup` the old target is always parked as
// "<to>.bak" first and the backup is left in place afterwards: the catalog cache needs the old file until the whole
// set of files has been committed. If a step fails the previous target is restored where possible. Returns true when
// `to` now holds the new content.
bool commitFile(const std::string& tmp, const std::string& to, const FileOps& ops = {}, bool keepBackup = false);

// Writes `data` to "<path>.tmp" and commits it over `path`. The tmp is removed again on failure. With `syncData` the tmp is
// fsynced (fsyncFile) before the rename, so after a power cut `path` is the old or the complete new content (the install
// records, Increment 5).
bool writeFileAtomic(const std::string& path, const std::string& data, const FileOps& ops = {}, bool keepBackup = false,
                     bool syncData = false);

// Reads the whole file with one fread after fseek/ftell. A file larger than `maxBytes` is refused before anything is
// allocated (the SD card is untrusted input). `out` is empty unless the result is Ok.
// Missing is only ENOENT/ENOTDIR from fopen; any other open failure is IoError.
ReadStatus readFileCapped(const std::string& path, std::string& out, size_t maxBytes);

// Three-way answer for "is it there": a failing stat is an ERROR, never "absent". Everything that deletes, skips or decides on
// absence (recovery, commit) must tell the two apart: reading a flaky card as "missing" would drop a journal or skip an undo.
enum class Presence { Present, Absent, Error };
// lstat semantics through `ops.stat` (or the real call). `out` (optional) gets the stat when the call answered.
Presence presenceOf(const std::string& path, const FileOps& ops = {}, FileStat* out = nullptr);

// Creates `dir` and every missing parent. True if it exists afterwards.
bool makeDirs(const std::string& dir, const FileOps& ops = {});

// The names (not paths) of the entries in `dir`, without "." and "..". False if the directory cannot be opened.
bool listDir(const std::string& dir, std::vector<std::string>& names, const FileOps& ops = {});

// ---------------------------------------------------------------------------------------------------------------------
// Directory helpers (Increment 2a). Every disk step of M3 (staging cleanup, commit, Delete) goes through these.

// True if `dir` exists, can be listed and has no entries. False for a missing path, a file, or a listing error.
bool isEmptyDir(const std::string& dir, const FileOps& ops = {});

// Removes `dir` only if it is empty (checked first). True if it was removed; false if it is not empty or on any error.
bool removeDirIfEmpty(const std::string& dir, const FileOps& ops = {});

// The real on-disk name of the entry in `dir` that equals `name` ignoring ASCII case (FAT keeps the case it was created
// with but matches without it). An exact match wins; otherwise the smallest name that matches. nullopt if there is none
// or the directory cannot be listed.
std::optional<std::string> findCaseInsensitive(const std::string& dir, const std::string& name, const FileOps& ops = {});

// Free bytes for unprivileged writers on the filesystem holding `root` (statvfs: f_bavail * f_frsize). nullopt if
// the path does not exist or statvfs fails.
std::optional<uint64_t> freeSpace(const std::string& root);

// Flushes the stdio buffer and asks the OS to put the file on the medium (fsync(fileno)). devkitPPC's libsysbase has
// fsync and libfat implements it (flushes the sector cache), so no #ifdef is needed. A system that answers ENOSYS or
// ENOTSUP is treated as "nothing to do": that is logged once and the call returns true. False on a real failure.
bool fsyncFile(std::FILE* f);

// ---- removeTree ------------------------------------------------------------------------------------------------------

constexpr int kRemoveMaxDepth = 32; // directory levels below the path that is removed

struct RemoveResult {
	bool ok = false;       // the whole tree is gone (or was never there)
	bool refused = false;  // a guard said no; nothing was touched
	bool existed = false;  // the path was there when we started
	size_t files = 0;      // files and symlinks removed
	size_t dirs = 0;       // directories removed (including `path`)
	size_t errors = 0;     // entries that could not be removed
	bool stopped = false;  // the `stop` poll answered true: the walk ended early, ok is false and the tree is partly gone
	std::string error;     // the first problem (why it was refused, or the first failing path and step)
};

// Removes `path` and everything below it, deepest first, one entry at a time. Nothing outside `path` is touched, and a
// symlink (PC) is unlinked, never followed. It REFUSES (refused=true, nothing touched) unless all of this holds:
//  - `path`, `mustBeUnder` and `roots` are non-empty, and contain no NUL or backslash;
//  - every `roots` entry is sane: not empty, not "/" (or anything that normalises to the filesystem root), no ".."
//    segment and no "apps" or "OpenShop" component. One bad entry refuses the whole call. Callers should go through
//    platform::removeTree, which passes platform::storageRoots() so a bad vector cannot be built by hand;
//  - neither `path` nor `mustBeUnder` has a ".." segment, or a "." segment other than a leading "./" on a plain
//    relative path (the PC roots are written "./sdcard");
//  - on a device path ("sd:", "usb:") the character after the colon is "/" ("sd:foo" is refused, because the Wii would
//    resolve it against the device's current directory while the guards read it as "sd:/foo"). The bare "sd:" is a root
//    and refused as such. The path that is executed is the normalised one, "<device>:/" + components, so the string
//    that was checked is the string that is used;
//  - on a device path no component ends in "." or a space (FAT drops them: "OpenShop." is "OpenShop") or contains
//    "~<digit>" (the 8.3 alias, "OPENSH~1" is "OpenShop"). The guards assume callers never produce such names (manifest
//    dirs and archive entry names are checked to the same rule), and refuse them as a second line of defence;
//  - `path` is strictly below `mustBeUnder` after lexical normalisation (pathroot: trailing and doubled slashes
//    dropped), compared by whole components, so "apps/foo2" is not under "apps/foo";
//  - both lie on one of `roots`; `path` is not a root, not "<root>/apps" and not "<root>/apps/OpenShop"; `mustBeUnder`
//    is not a root itself; and a path inside "<root>/apps/OpenShop" needs `mustBeUnder` at or below
//    "<root>/apps/OpenShop/staging" (so staging cleanup works and "installed/", the journal and the rest cannot be
//    reached, whatever the caller says).
// Case: on a device path ("sd:", "usb:", FAT) the "strictly under" and root tests ignore ASCII case. On plain (PC)
// paths they are case-sensitive. The "apps", "OpenShop" and "staging" names are always matched ignoring case, so
// "APPS/openshop" is refused everywhere.
// More than kRemoveMaxDepth levels of directories are an error (that branch is left), not a crash. A missing `path` is
// ok with existed=false.
//
// Which walk runs: on PC with the real calls (no stat/list/remove/rmdir injected) the walk is by descriptor: the parent
// of `path` is reached from the root by openat(O_NOFOLLOW) one component at a time (the root itself is trusted and may
// be a symlink), every directory is opened with O_NOFOLLOW|O_DIRECTORY, entries are checked with
// fstatat(AT_SYMLINK_NOFOLLOW) and removed with unlinkat, so a directory swapped for a symlink while we walk is
// unlinked, never entered. A symlinked directory below the root is therefore an error, not a detour. With injected
// FileOps (tests of the failure paths), and on the Wii (no symlinks; libfat has no *at calls), the walk is by path with
// lstat semantics.
//
// `stop` (optional, M3 Increment 7: a cancelled Cleanup job) is polled before every directory entry that is about to be removed,
// on every level. When it answers true the walk ends there: RemoveResult::stopped is set, ok is false, errors does not count it,
// and the tree is left partly deleted (the directories on the way down are NOT removed, so what remains is an ordinary, smaller
// tree). That state is safe to retry: removeTree on the same path later finishes the job.
RemoveResult removeTree(const std::string& path, const std::string& mustBeUnder, const std::vector<std::string>& roots,
                        const FileOps& ops = {}, const std::function<bool()>& stop = nullptr);

// ---- commitDir -------------------------------------------------------------------------------------------------------

enum class CommitDirStep {
	NotStarted, // failed before anything moved (staged is still staged, target untouched)
	Renamed,    // target did not exist; staged was renamed to target. ok
	Replaced,   // old target is now at park and staged is at target. ok
	RolledBack, // staged -> target failed; the old target was put back
	Stuck,      // staged -> target failed and the old target could NOT be put back: it is at park
};

struct CommitDirResult {
	bool ok = false;
	CommitDirStep step = CommitDirStep::NotStarted;
};

// Three primitives, so that Increment 6 can journal every rename on its own, and a convenience wrapper over them. The
// missing parents of the destination are created (a device root such as "sd:" counts as existing).
//
// parkDir:       renames the existing directory `target` to `park`. `park` must not exist (libfat can't rename over
//                it, and it may be someone's data) and must not lie inside `target`.
// renameStaged:  renames the directory `staged` (lstat: a symlink or a file is refused) to `target`, which must not
//                exist.
// restoreParked: renames `park` back to `target`, which must not exist.
//
// RECOVERY RULE for a cut between the two renames of a replace (the journal says "parked" but not "placed"): if `park`
// exists, `target` is missing and `staged` exists, recovery either finishes (renameStaged(staged, target)) or restores
// (restoreParked(park, target)). Finishing is preferred when the staged tree is complete. If `target` exists and `park`
// exists the replace is done; `park` is only left-over to be removed (removeTree). If `park` is missing and `target`
// exists nothing was parked.
bool parkDir(const std::string& target, const std::string& park, const FileOps& ops = {});
bool renameStaged(const std::string& staged, const std::string& target, const FileOps& ops = {});
bool restoreParked(const std::string& park, const std::string& target, const FileOps& ops = {});

// Puts the directory `staged` at `target`: renameStaged when `target` does not exist, otherwise parkDir, renameStaged and,
// if that second rename fails, restoreParked. `step` says how far it got, for the journal.
CommitDirResult commitDir(const std::string& staged, const std::string& target, const std::string& park,
                          const FileOps& ops = {});
