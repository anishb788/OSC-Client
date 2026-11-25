#pragma once
// Open Shop Channel API: endpoints and the JSON -> Catalog parsers.
//
// Everything here is pure: the functions take the response bodies as text and never touch the network or the
// filesystem. The bodies are untrusted, so every limit in OscApi.cpp is a hard bound on
// memory and work, and every failure becomes an ApiError whose `message` is written for the user (Increment 9 shows
// it as is).
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "catalog/Catalog.h"

namespace oscapi {

// The one place that knows the server. Switching to HTTPS later means changing kBaseUrl and kContentsUrl.
inline constexpr const char* kBaseUrl = "http://hbb1.oscwii.org/api/v4";
inline constexpr const char* kContentsUrl = "http://hbb1.oscwii.org/api/contents";
inline constexpr const char* kInformationPath = "/information";
inline constexpr const char* kContentsPath = "/contents";
inline constexpr const char* kFeaturedPath = "/featured-app";

std::string informationUrl();
std::string contentsUrl();
std::string featuredUrl();
// http://hbb1.oscwii.org/api/contents/<slug>/icon.png. The slug must already pass isValidSlug.
std::string iconUrl(std::string_view slug);

// Hard bounds. Bodies above these limits are refused before jansson sees them. The live contents body is 455 KB
// (303 apps), so 2 MB is 4x headroom; the old 8 MB limit let a hostile `[{},{},...]` body make jansson peak at
// 645 MB, and the Wii has ~88 MB. Increment 9 must cap the HTTP read at the same limits (MemorySink maxBody), so an
// oversized body is never buffered before the pre-scan runs.
inline constexpr size_t kMaxInformationBytes = 256 * 1024;
inline constexpr size_t kMaxContentsBytes = 2 * 1024 * 1024;
inline constexpr size_t kMaxFeaturedBytes = 64 * 1024;
// Bytes alone do not bound jansson's memory: tiny values cost 40-240 bytes each (an `{}` is ~240). The pre-scan
// counts structural openers plus commas (~ one per value). The live contents body has 4250 openers + 11227 commas
// (~15.5k); 150k is ~10x that, and also what kMaxApps apps at the real density need.
inline constexpr size_t kMaxNodes = 150000;
inline constexpr int kMaxDepth = 16; // the real data nests 4 deep
inline constexpr size_t kMaxApps = 3000;
inline constexpr size_t kMaxCategories = 32;
inline constexpr size_t kMaxListItems = 64;  // per string array (authors, flags, subdirectories...)
inline constexpr size_t kMaxShortText = 512; // names, URLs, versions, list items
inline constexpr size_t kMaxLongText = 64 * 1024;
inline constexpr size_t kMaxSlug = kMaxSlugLength; // see Catalog.h
inline constexpr size_t kMaxWarnings = 50; // stored and logged; the rest are only counted
inline constexpr size_t kMaxSubdirectories = 256; // per app; the real maximum is 79
// Install paths: the whole subdirectory, and one segment (FAT's limit is 255 per name).
inline constexpr size_t kMaxSubdirPath = 240;
inline constexpr size_t kMaxSubdirSegment = 255;
// release_date is unix seconds. Anything past 2100-01-01 is not a date (a millisecond value looks like year 50000+),
// so it becomes 0 (unknown) instead of a bogus far-future date that would win "newest".
inline constexpr uint64_t kMaxReleaseDate = 4102444800ULL;

enum class ApiErrorCode {
	Ok,
	Empty,     // no body at all
	TooLarge,  // body above the byte limit
	TooDeep,   // nesting above kMaxDepth
	Syntax,    // not JSON, or cut off
	WrongType, // valid JSON, but not the shape the API documents
	NoApps,    // parsed, but not one usable app
};

struct ApiError {
	ApiErrorCode code = ApiErrorCode::Ok;
	std::string message; // user-facing, one sentence
	bool ok() const { return code == ApiErrorCode::Ok; }
};

struct ApiStats {
	size_t rawBytes = 0;     // the JSON text
	size_t domBytes = 0;     // estimate of the jansson tree while it was alive
	size_t entryBytes = 0;   // estimate of the AppEntry data kept
	size_t appsKept = 0;
	size_t appsSkipped = 0;
	size_t categoriesDropped = 0; // beyond what Home shows (log only)
	double parseMs = 0;
	// Peak while converting is about rawBytes + domBytes + entryBytes (the DOM is freed right after).
	size_t peakBytes() const { return rawBytes + domBytes + entryBytes; }
	std::vector<std::string> warnings; // at most kMaxWarnings
	size_t warningCount = 0;
};

// Fills c.categories (API order, at most kMaxCategories). Replaces any existing categories.
ApiError parseInformation(std::string_view json, Catalog& c, ApiStats* stats = nullptr);

// Appends to c.apps, and expects it to be empty: duplicate slugs are only detected within this body, not against
// existing entries (asserted). Bad apps are skipped with a warning; the call fails only when the body is unusable as a whole
// or holds no usable app. Does not call finalize().
ApiError parseContents(std::string_view json, Catalog& c, ApiStats* stats = nullptr);

// /featured-app answers with one full app object; only its slug is used. `slug` is empty on failure.
ApiError parseFeatured(std::string_view json, std::string& slug);

// Categories from the apps' own `category` values, for when /information is missing: the five known ones first (in
// the site's order) with built-in names, then any other value by name.
void deriveCategories(Catalog& c);

struct BuildResult {
	ApiError error;
	Catalog catalog; // valid and finalized only when error.ok()
	ApiStats stats;
	bool ok() const { return error.ok(); }
};

// The whole pipeline. Only a bad contents body is fatal: a missing or bad information or featured body is logged as a
// warning and replaced by the fallbacks (derived categories, newest app). fetchedAt is left for the caller.
BuildResult buildCatalog(std::optional<std::string_view> informationJson, std::string_view contentsJson,
                         std::optional<std::string_view> featuredJson);

} // namespace oscapi
