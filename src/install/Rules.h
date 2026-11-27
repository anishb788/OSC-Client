// SPDX-License-Identifier: GPL-3.0-or-later
// Pure install rules: archive entry names, case folding, system and own roots, the subdirectory hint filter.
// No SDL, libogc or file access. Case folding is ASCII-only and locale-independent. Links with core/NameRules.cpp
// (isReservedDeviceName); catalog/Catalog.cpp (installRefusal) uses filterSubdirHint, so Rules.cpp must not depend on
// the catalog.
#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace install {

constexpr size_t kMaxEntryNameBytes = 240;
constexpr size_t kMaxEntrySegments = 16;

enum class NameReject {
	None,
	Empty,
	Backslash,
	Absolute,
	DrivePrefix,
	BadSegment,    // "." or ".." or an empty segment
	TrailingDotSpace,
	BadChar,       // FAT-illegal character, control character or DEL
	BadUnicode,    // valid UTF-8 that is unsafe in a file name: above U+FFFF, C1 control, invisible or bidi, fold look-alike
	LeadingSpace,  // a segment starting with a space
	BadUtf8,
	ReservedName,
	FatAlias,      // "~<digit>" in a segment (the FAT 8.3 alias pattern; see core/FileUtil.h)
	TooLong,
	TooDeep,
};

struct EntryName {
	bool ok = false;
	bool isDir = false;              // the name ended in one '/'
	std::string relPath;             // segments joined with '/', no trailing '/'
	std::vector<std::string> segments;
	NameReject reason = NameReject::None;
};

// Validates one zip entry name (untrusted bytes). A backslash is never turned into a separator: it rejects the name.
EntryName validateEntryName(std::string_view name);
const char* describe(NameReject r);

// ASCII case fold for FAT (case-insensitive) collision checks. Bytes above 127 are left alone.
std::string foldKey(std::string_view path);

// topSegment is one path segment (no slashes), compared case-insensitively.
// "apps", case-insensitive (a single segment).
bool isAppsRoot(std::string_view topSegment);
bool isSystemRoot(std::string_view topSegment);
// "apps/OpenShop" itself or anything below it; a leading and trailing '/' are ignored; case-insensitive.
bool isOwnRoot(std::string_view path);
bool isJunkRoot(std::string_view topSegment); // __MACOSX

// Drops the hints extraction would never write: "/", "/apps", anything under a system root, "/apps/OpenShop" (and below)
// and "/__MACOSX". Each drop is logged. The order of the kept entries is unchanged.
std::vector<std::string> filterSubdirHint(const std::vector<std::string>& subdirectories);

} // namespace install
