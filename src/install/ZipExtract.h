// SPDX-License-Identifier: GPL-3.0-or-later
// ZipExtract (M3 Increment 4): validate a whole zip archive first, then extract it into <staging>/root as a resumable state
// machine. It never writes anywhere but below <staging>/root and never touches a device's apps/ folder (install::Commit does,
// Increment 6). No SDL and no libogc; links libzip, core/FileUtil.cpp and install/Rules.cpp (and core/NameRules.cpp).
//
// FLOW
//   ZipExtract zx(opts);
//   zx.open(zipPath)            validates the WHOLE central directory; not one byte is written. On success plan() is set.
//   zx.begin(stagingDir, free)  re-checks free space, creates <stagingDir>/root (which must not exist yet).
//   zx.confirmUndeclared()      only when plan().needsConfirmation(): the user agreed to the undeclared folders.
//   zx.step(budgetMs)           repeat until Done or Error. Any failure or cancel removes <stagingDir>/root.
// NeedsConfirmation is NOT terminal: step() returns it (and nothing is lost) until confirmUndeclared() or requestCancel() is
// called. The caller should check plan().needsConfirmation() before begin() and ask first, so the worker never waits on it.
// open() validates the central directory only; before it calls libzip it reads the end-of-central-directory record itself and
// refuses an archive whose entry count is over maxEntries, so libzip never loads an oversized directory.
//
// THREADING: open/begin/step run on the install worker. requestCancel(), confirmUndeclared() and the progress getters may be
// called from the main thread (atomics); everything else is for one thread at a time.
//
// MEMORY: one fixed 64 KB heap buffer (allocated in begin(); never on the stack, the worker's stack is small) and no
// whole-entry reads: peak memory does not depend on an entry's size. The central directory itself lives in libzip (about
// 100 bytes plus the name per entry, at most maxEntries entries). open() also keeps one folded-name key per written entry for
// the collision check (a std::string plus a flag, roughly 60 to 100 bytes with a typical name, so about 1 to 2 MB for 20,000
// entries; it is freed when open() returns) and begin() keeps a 24-byte record per written entry.
//
// WII NOTES: libzip reads the archive with fopen/fseeko/ftello (newlib has them, libfat implements lseek). If fseeko ever
// misbehaves on libfat, the fallback is zip_source_function_create over plain stdio fread/fseek; not needed so far.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "catalog/Catalog.h" // PackageType
#include "catalog/InstallLimits.h"
#include "core/FileUtil.h"

struct zip;
struct zip_file;

namespace install {

enum class ZipError {
	None,
	Open,           // the file is missing, not a zip, or its central directory is unreadable
	BadName,        // validateEntryName refused an entry name
	Collision,      // two entries (or a file and a directory) share a folded name
	Symlink,        // a symlink or other special file
	BadEntry,       // a malformed entry (a directory with data, a directory flagged on a file name)
	Encrypted,
	Method,         // not store (0) or deflate (8)
	TooManyEntries,
	TooBig,         // the declared total is over the cap
	FileTooBig,     // one file of 4 GiB or more (FAT32)
	NoBoot,         // no apps/<dir>/boot.dol|boot.elf that matches the package type
	Forbidden,      // under apps/OpenShop, under a system root, or in an app dir ownsDir refused
	NoSpace,        // begin(): not enough free space; `needed` says how much
	Io,             // a write, mkdir or fclose failed (ENOSPC included), or the staging root is not usable
	SizeMismatch,   // an entry's real data is longer or shorter than its declared size
	Corrupt,        // libzip could not read or verify (CRC) an entry
	Cancelled,
	BadState,       // a call out of order
};

const char* describe(ZipError e);

struct ZipFailure {
	ZipError code = ZipError::None;
	std::string detail; // human-readable, for the log
	std::string entry;  // the entry name involved, when there is one
	uint64_t needed = 0; // NoSpace: the bytes required (declared total + 1 MB)
};

struct ZipOptions {
	PackageType packageType = PackageType::Unknown; // Unknown accepts either boot.dol or boot.elf
	std::vector<std::string> subdirHint;            // install::filterSubdirHint output, e.g. "/WiiFlow"
	size_t maxEntries = kMaxEntries;                // injectable for tests
	uint64_t maxInstallBytes = kMaxInstallBytes;    // injectable for tests
	// The catalog's number, 0 when unknown. INFORMATIONAL ONLY: it is logged by open() and never used to accept or reject
	// anything. The limits are maxInstallBytes (on the declared total), each entry's own declared size (the real data may
	// not differ), and free space.
	uint64_t uncompressedSize = 0;
	// Gets the case-folded name of an apps/<dir>; false refuses the archive. Null accepts every dir.
	std::function<bool(const std::string& foldedDirName)> ownsDir;
	// FileUtil::removeTree's `roots` (platform::storageRoots() in the app). Cleanup is refused, and logged, when it is empty.
	std::vector<std::string> removeRoots;
	FileOps fileOps;
	// TEST HOOK: replaces fwrite for the extracted data (to inject ENOSPC). Null means std::fwrite.
	size_t (*testWriteFn)(const void* p, size_t size, size_t n, std::FILE* f) = nullptr;
};

struct ZipPlan {
	struct AppDir { std::string name; bool hasBoot = false; };       // the real spelling from the archive
	struct Folder { std::string name; bool needsConfirmation = false; }; // other top-level folders
	std::vector<AppDir> appDirs;
	std::vector<Folder> otherFolders;
	size_t skippedRootFiles = 0; // files at the archive root (and loose files directly in apps/): never written
	size_t skippedMacosx = 0;    // entries under __MACOSX/: never written
	size_t entryCount = 0;       // all entries in the archive
	size_t writtenEntries = 0;   // entries that will be written (files and directories)
	uint64_t declaredTotal = 0;  // sum of the declared sizes of the files that will be written
	bool needsConfirmation() const {
		for (const Folder& f : otherFolders) if (f.needsConfirmation) return true;
		return false;
	}
};

enum class StepResult { Running, Done, Error, NeedsConfirmation };

class ZipExtract {
public:
	explicit ZipExtract(ZipOptions opts = {});
	// An unfinished extraction removes its staging root. A failed cleanup is not retried here: callers check rootRemoved()
	// after a failure (Increment 6's recovery deals with a leftover <staging>/root).
	~ZipExtract();
	ZipExtract(const ZipExtract&) = delete;
	ZipExtract& operator=(const ZipExtract&) = delete;

	// Opens and validates. False: failure() says why and nothing was written.
	bool open(const std::string& zipPath);
	const ZipPlan& plan() const { return plan_; }

	// `freeBytes` is the free space on the target device. Refuses with NoSpace (needed = declaredTotal + 1 MB) before
	// creating anything. Otherwise creates <stagingDir>/root, which must not exist.
	bool begin(const std::string& stagingDir, uint64_t freeBytes);

	// Allows step() to write the undeclared top-level folders.
	void confirmUndeclared() { confirmed_ = true; }
	// Checked between 64 KB chunks; the next step() cleans up and returns Error (Cancelled).
	void requestCancel() { cancel_ = true; }

	// Extracts for about budgetMs (at least one chunk of work per call). NeedsConfirmation is non-terminal: the next entry is
	// in an undeclared folder and confirmUndeclared() has not been called; nothing was lost (call confirmUndeclared() and
	// step() again, or requestCancel() + step() to drop the staging root instead).
	StepResult step(unsigned budgetMs);

	const ZipFailure& failure() const { return failure_; }
	uint64_t bytesWritten() const { return bytesDone_; }
	uint64_t totalBytes() const { return plan_.declaredTotal; }
	const std::string& root() const { return root_; } // <stagingDir>/root, set by begin()
	// After a failure or cancel: true when <stagingDir>/root is gone.
	bool rootRemoved() const { return rootRemoved_; }

private:
	enum class State { Idle, Opened, Extracting, Done, Failed };
	struct Item {
		uint32_t index = 0;
		uint64_t size = 0;
		bool isDir = false;
		bool undeclared = false;
	};
	bool fail(ZipError code, std::string detail, std::string entry = {});
	bool failOpen(ZipError code, std::string detail, std::string entry = {}); // fail() plus state_ = Failed, zip closed
	void closeCurrent();
	void cleanupRoot();
	StepResult stepFail(ZipError code, std::string detail, std::string entry = {});
	bool openEntry(const Item& it);

	ZipOptions opts_;
	ZipPlan plan_;
	ZipFailure failure_;
	State state_ = State::Idle;
	zip* zip_ = nullptr;
	std::vector<Item> items_;
	std::string stagingDir_, root_;
	std::unique_ptr<unsigned char[]> buf_;
	size_t next_ = 0;
	zip_file* zf_ = nullptr;
	std::FILE* out_ = nullptr;
	std::string curName_;
	uint64_t curWritten_ = 0, actualTotal_ = 0;
	std::atomic<uint64_t> bytesDone_{0};
	std::atomic<bool> cancel_{false};
	std::atomic<bool> confirmed_{false};
	bool rootCreated_ = false, rootRemoved_ = false;
};

} // namespace install
