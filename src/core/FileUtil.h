#pragma once
// Plain-stdio file helpers shared by the HTTP sinks, the platform layer and the catalog cache. No SDL, so the host
// tests link it on its own.
//
// The Wii's libfat cannot rename over an existing file, so every "replace a file" here goes through a tmp file and a
// backup dance (see commitFile). A power cut at any point leaves the old file, the new file, or the old file as
// "<path>.bak" (which the cache's reader knows to look for), never a half-written file under the real name.
#include <cstddef>
#include <string>
#include <vector>

enum class ReadStatus { Ok, Missing, TooLarge, IoError };

// Filesystem calls the writers and the catalog cache make, replaceable so tests can force the failure paths. A null
// member means the real call (std::rename, std::remove, stat, readFileCapped).
struct FileOps {
	int (*rename)(const char* from, const char* to) = nullptr;
	int (*remove)(const char* path) = nullptr;
	bool (*exists)(const char* path) = nullptr;
	// Only the catalog cache's reads go through this one.
	ReadStatus (*read)(const char* path, std::string& out, size_t maxBytes) = nullptr;
};

// Moves the finished tmp file over `to`. Without `keepBackup` a plain rename is tried first and the backup dance only
// runs when that fails because the target exists (libfat). With `keepBackup` the old target is always parked as
// "<to>.bak" first and the backup is left in place afterwards: the catalog cache needs the old file until the whole
// set of files has been committed. If a step fails the previous target is restored where possible. Returns true when
// `to` now holds the new content.
bool commitFile(const std::string& tmp, const std::string& to, const FileOps& ops = {}, bool keepBackup = false);

// Writes `data` to "<path>.tmp" and commits it over `path`. The tmp is removed again on failure.
bool writeFileAtomic(const std::string& path, const std::string& data, const FileOps& ops = {}, bool keepBackup = false);

// Reads the whole file with one fread after fseek/ftell. A file larger than `maxBytes` is refused before anything is
// allocated (the SD card is untrusted input). `out` is empty unless the result is Ok.
ReadStatus readFileCapped(const std::string& path, std::string& out, size_t maxBytes);

// Creates `dir` and every missing parent. True if it exists afterwards.
bool makeDirs(const std::string& dir);

// The names (not paths) of the entries in `dir`, without "." and "..". False if the directory cannot be opened.
bool listDir(const std::string& dir, std::vector<std::string>& names);
