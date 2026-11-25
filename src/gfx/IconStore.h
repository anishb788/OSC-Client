#pragma once
// The on-SD icon cache (Increment 11): <cacheDir>/icons/<slug>.png, the raw PNG bytes as the server sent them. No SDL,
// no network, so the host tests drive it directly. Used from the network worker's jobs only, never from draw(): SD I/O
// is slow, especially on a Wii.
//
// CRASH SAFETY. Every icon is independent, so there is no manifest and no generation: a file is written to
// "<slug>.png.tmp" and moved over the real name by commitFile (the libfat-safe tmp / .bak dance). The reader only ever
// opens "<slug>.png", so it sees the old file, the new file, or (for the instant between the two renames on libfat, or
// after a power cut there) no file, which just means "fetch again". It never sees half a file. A file that is nonetheless
// bad (a bit-flipped sector, someone's edit) fails the PNG check in the job, which deletes it and refetches. The job
// validates the PNG BEFORE writing, so a network error page can never become a cached "icon".
//
// FRESHNESS: a cached icon is valid when its size equals the catalog's assets.icon.size (0 = unknown, anything goes).
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "core/FileUtil.h"

namespace iconstore {

inline constexpr const char* kSubdir = "icons";

// <cacheDir>/icons
std::string dirOf(const std::string& cacheDir);
// <cacheDir>/icons/<slug>.png, or "" if the slug is not a safe file name (isValidSlug).
std::string pathOf(const std::string& cacheDir, std::string_view slug);

enum class Read { Ok, Missing, BadSlug, SizeMismatch, TooLarge, IoError };
// Reads the cached icon. With expectedSize > 0 a file of another size is SizeMismatch (stale; `out` is empty). The
// read is capped at maxBytes before anything is allocated.
Read read(const std::string& cacheDir, std::string_view slug, uint64_t expectedSize, size_t maxBytes, std::string& out);

// Creates the directory if needed and writes the file atomically. False when the card is read-only or full.
bool write(const std::string& cacheDir, std::string_view slug, const std::string& bytes, const FileOps& ops = {});

// Deletes one cached icon (a corrupt one). True if it is gone afterwards.
bool remove(const std::string& cacheDir, std::string_view slug);

// Removes the icons of slugs that are not in `keep`, plus leftover .tmp and .bak files, a few at a time so the caller
// can slice it. The first step() only takes the snapshot of the directory (no classification, no deletion); each later
// step() deletes at most `perStep` files and classifies at most 32 * perStep names, so one step is always bounded.
// Anything not named "<valid slug>.png" (or one of those leftovers) is left alone, since the user may keep files there.
class Pruner {
public:
	// `keep` holds the slugs to keep, compared case-insensitively (FAT is).
	Pruner(std::string cacheDir, const std::set<std::string>& keep);
	// True while more work is left.
	bool step(size_t perStep);
	size_t removed() const { return removed_; }
	size_t scanned() const { return scanned_; }
	double snapshotMs() const { return snapshotMs_; }   // how long the listDir of the first step took

private:
	std::string dir_;
	std::set<std::string> keep_;
	std::vector<std::string> names_, todo_;   // the snapshot; the names chosen for deletion
	size_t at_ = 0, next_ = 0, removed_ = 0, scanned_ = 0;
	double snapshotMs_ = 0;
	bool started_ = false;
};

}  // namespace iconstore
