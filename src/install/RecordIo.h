// SPDX-License-Identifier: GPL-3.0-or-later
// Shared plumbing of the install records (Manifest.cpp, Journal.cpp): crash-safe write, bounded read with the "<file>.bak"
// fallback, a nesting-depth pre-check (jansson recurses, the install worker's stack is small) and capped field readers.
// Everything read here is untrusted (the card can hold anything). No SDL; links jansson and core/FileUtil.cpp.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <jansson.h>

#include "core/FileUtil.h"

namespace install::rec {

// "<root>/apps/OpenShop/installed": manifests and the journal live here (the HBC check: only apps/OpenShop/ is touched, never
// a folder with a boot file).
std::string installedDir(const std::string& root);

enum class ReadResult { Ok, Missing, TooLarge, IoError };
struct Read {
	ReadResult result = ReadResult::Missing;
	std::string text;
	bool fromBackup = false; // "<path>.bak" was read because "<path>" was missing (a cut inside libfat's replace dance)
};
// Reads `path`; when it is missing, reads "<path>.bak". Larger than maxBytes is TooLarge, nothing allocated.
Read readRecord(const std::string& path, size_t maxBytes, const FileOps& ops = {});

// Creates the parent directory, then writeFileAtomic with fsync before the rename. Refuses text over maxBytes (the reader
// would refuse it). True when `path` now holds `text`.
bool writeRecord(const std::string& path, const std::string& text, size_t maxBytes, const FileOps& ops = {});

// Removes "<path>.bak", then "<path>.tmp", then "<path>" (best effort; that order, so a cut part-way leaves the real record
// and never a stale ".bak" that the loader would pick up). True if none is left.
bool removeRecord(const std::string& path, const FileOps& ops = {});

// Pre-scan of untrusted text, outside strings: true if the [] / {} nesting never exceeds maxDepth AND the number of values
// (every '[' or '{' plus every ',') stays within maxValues. jansson allocates ~40+ bytes per value, so this bounds the memory
// a hostile file can make the parser take (a 4 MB file of "[0,0,0,...]" would otherwise be ~2M values).
bool shapeOk(std::string_view text, int maxDepth, size_t maxValues);

struct JsonDeleter { void operator()(json_t* j) const { if (j) json_decref(j); } };
using Json = std::unique_ptr<json_t, JsonDeleter>;
// Pre-checks shape (nesting and value count; no copy of the text is made), then json_loadb rejecting duplicate keys. Null on
// any problem (error is set).
Json parseObject(std::string_view text, int maxDepth, size_t maxValues, std::string& error);

// A JSON string with the bytes of `s` (embedded NULs kept, so a reader that refuses NULs rejects the round trip). Null when
// `s` is not valid UTF-8: writers treat that as a failed serialisation, never as a silently dropped field.
json_t* jsonStr(const std::string& s);

// Capped field readers. Missing or wrong type or too long (or an embedded NUL) is false.
bool getString(json_t* obj, const char* key, size_t maxLen, std::string& out);
bool getInt(json_t* obj, const char* key, int64_t& out);
// Present and a boolean -> true and `out` set. Anything else leaves `out` alone and returns false.
bool getBool(json_t* obj, const char* key, bool& out);
// The array under `key`, or null when missing or not an array or larger than maxSize.
json_t* getArray(json_t* obj, const char* key, size_t maxSize);
// A JSON string value as std::string, with the same caps. False for a non-string.
bool asString(json_t* v, size_t maxLen, std::string& out);

} // namespace install::rec
