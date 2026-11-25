#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

enum class PackageType { Unknown, Dol, Elf };

// Fields are declared in the order MockCatalog.cpp initialises them (C++20 designated initialisers must follow
// declaration order). Add new fields with a default value, never reorder.
struct AppEntry {
	std::string slug, name, author, category;
	std::string shortDesc, longDesc, version;
	std::vector<std::string> authors, contributors; // `author` stays the display string
	std::string iconUrl;
	std::string archiveUrl, archiveMd5;
	std::string binaryUrl, binaryMd5;
	std::string metaUrl;
	uint64_t iconSize = 0, archiveSize = 0, binarySize = 0, uncompressedSize = 0;
	uint32_t downloads = 0;
	int64_t releaseDate = 0; // unix seconds
	PackageType packageType = PackageType::Unknown;
	std::vector<std::string> peripherals, platforms, flags, subdirectories;

	// Derived by Catalog::finalize(): lowercase search fields. Do not fill these by hand.
	// The `{}` initialisers keep -Wmissing-field-initializers quiet for generated aggregate initialisation.
	std::string nameL{}, slugL{}, authorL{}, descL{}; // descL = shortDesc + " " + longDesc

	bool hasFlag(std::string_view f) const;
	// The only gate for installing (M3 must check it): an http(s) archive URL and a 32-hex archive MD5. An empty MD5
	// means "refuse", never "skip the check". Over plain HTTP the MD5 protects against corruption, not a MITM.
	bool installable() const;
};

struct Category {
	std::string name, display;
	std::string singular, plural;
};

enum class SortMode { Popular, Newest, Name, Relevance };

class Catalog {
public:
	std::vector<Category> categories;
	std::vector<AppEntry> apps;
	std::string featuredSlug;
	// When this catalog was fetched (unix seconds, 0 = unknown). Mock: when the mock catalog was built; a catalog read
	// from the SD cache carries the cache's fetch time. Shown by the "Couldn't refresh" message and the offline chip.
	int64_t fetchedAt = 0;
	// True when the network was not used because it failed (stale cache fallback) or was switched off (`offline` dev
	// option): Home then says "Offline: catalog from <date>". A fresh cache read at startup is not offline.
	bool offline = false;

	// Must be called once after `apps`, `categories` and `featuredSlug` are filled (loadMockCatalog and the API
	// parser both do). It sanitizes every text field (see sanitizeText), computes the lowercase search fields, the
	// slug index, per-category counts and the featured app. Mutating `apps` afterwards requires calling it again.
	void finalize();

	const AppEntry* find(std::string_view slug) const;
	const AppEntry* featured() const;
	const Category* category(std::string_view name) const;
	std::string categoryDisplay(std::string_view name) const;

	// Empty category = all apps.
	std::vector<const AppEntry*> inCategory(std::string_view category) const;
	int count(std::string_view category) const;
	static void sort(std::vector<const AppEntry*>& list, SortMode mode);

	// Every query word must match the name, author, slug or description (words of 4+ letters also match a name as a
	// loose subsequence, ranked below real substring hits). Results are ordered by relevance (name matches first),
	// then popularity. Does not allocate per app. Relevance order only exists as search()'s output: sort() with
	// SortMode::Relevance is a no-op, so callers that re-sort must keep the original list as their base.
	std::vector<const AppEntry*> search(std::string_view query) const;

private:
	// Derived by finalize(). Indices into `apps`, so Catalog stays safely copyable and movable.
	std::map<std::string, size_t, std::less<>> slugIndex;
	std::map<std::string, int, std::less<>> categoryCounts;
	size_t featuredIdx = static_cast<size_t>(-1);
};

void loadMockCatalog(Catalog& c);

// Text hygiene for catalog strings (used by Catalog::finalize, the one place text is sanitized):
// "\r\n" becomes "\n", tabs become spaces, other control characters (including a lone "\r" and DEL) are dropped,
// UTF-8 bytes are left alone. singleLine also turns every "\n" into a space (name, author, short description,
// version, URLs...), so those fields never contain a line break.
std::string sanitizeText(std::string_view s, bool singleLine);

// Slugs become cache and install paths: [A-Za-z0-9._-]{1,kMaxSlugLength}, no leading dot (so never "." or ".."), no
// trailing dot (Windows/FAT drop it), and not a reserved device name. Parsers must skip apps whose slug fails this.
inline constexpr size_t kMaxSlugLength = 128;
bool isValidSlug(std::string_view slug);
// FAT/Windows device names (CON, PRN, AUX, NUL, COM1-9, LPT1-9), case-insensitive, with or without an extension:
// "con" and "Con.txt" are reserved, "console" and "com0" are not.
bool isReservedDeviceName(std::string_view name);
// True for "http://" or "https://" (any case) followed by at least one character, with no space or control character.
bool isHttpUrl(std::string_view url);

// Display helpers
std::string formatSize(uint64_t bytes);
std::string formatDate(int64_t unixSeconds);
// "Nov 24, 2026, 14:03" in the device's local time (UTC when the platform has no time zone); "an unknown date" for <= 0.
std::string formatDateTime(int64_t unixSeconds);
std::string formatCount(uint32_t n);
std::string peripheralName(std::string_view id);
std::string platformName(std::string_view id);
