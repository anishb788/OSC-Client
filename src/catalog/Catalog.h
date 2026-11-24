#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct AppEntry {
	std::string slug, name, author, category;
	std::string shortDesc, longDesc, version;
	uint32_t archiveSize = 0, uncompressedSize = 0, downloads = 0;
	int64_t releaseDate = 0; // unix seconds
	std::vector<std::string> peripherals, platforms, flags;

	bool hasFlag(std::string_view f) const;
};

struct Category {
	std::string name, display;
};

enum class SortMode { Popular, Newest, Name, Relevance };

class Catalog {
public:
	std::vector<Category> categories;
	std::vector<AppEntry> apps;
	std::string featuredSlug;

	const AppEntry* find(std::string_view slug) const;
	const AppEntry* featured() const;
	const Category* category(std::string_view name) const;
	std::string categoryDisplay(std::string_view name) const;

	// Empty category = all apps.
	std::vector<const AppEntry*> inCategory(std::string_view category) const;
	int count(std::string_view category) const;
	static void sort(std::vector<const AppEntry*>& list, SortMode mode);

	// Every query word must match the name, author, slug or description.
	// Results are ordered by relevance (name matches first), then popularity.
	std::vector<const AppEntry*> search(std::string_view query) const;
};

void loadMockCatalog(Catalog& c);

// Display helpers
std::string formatSize(uint64_t bytes);
std::string formatDate(int64_t unixSeconds);
std::string formatCount(uint32_t n);
std::string peripheralName(std::string_view id);
std::string platformName(std::string_view id);
