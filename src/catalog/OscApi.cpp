#include "catalog/OscApi.h"

#include <jansson.h>

#include <algorithm>
#include <chrono>
#include <cassert>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <memory>
#include <set>

#include "core/Log.h"
#include "core/Strings.h"
#include "core/Time.h"

namespace oscapi {

std::string informationUrl() { return std::string(kBaseUrl) + kInformationPath; }
std::string contentsUrl() { return std::string(kBaseUrl) + kContentsPath; }
std::string featuredUrl() { return std::string(kBaseUrl) + kFeaturedPath; }
std::string iconUrl(std::string_view slug) {
	return std::string(kContentsUrl) + "/" + std::string(slug) + "/icon.png";
}

namespace {

struct JsonDeleter {
	void operator()(json_t* j) const { json_decref(j); }
};
using JsonPtr = std::unique_ptr<json_t, JsonDeleter>;

ApiError fail(ApiErrorCode code, std::string msg) {
	ApiError e;
	e.code = code;
	e.message = std::move(msg);
	return e;
}

// Largest prefix of `s` that is at most `max` bytes and does not end inside a UTF-8 sequence. jansson has already
// rejected invalid UTF-8, so stepping back over continuation bytes is enough.
std::string clip(const char* s, size_t len, size_t max) {
	if (len > max) {
		len = max;
		while (len > 0 && (static_cast<unsigned char>(s[len]) & 0xC0) == 0x80) len--;
	}
	return std::string(s, len);
}

// Untrusted text in a log line: no line breaks or other control characters forged into the log.
std::string logSafe(std::string s) {
	for (char& c : s) {
		if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) c = '?';
	}
	return s;
}

void warn(ApiStats* st, const char* fmt, const std::string& a = std::string(), const std::string& b = std::string()) {
	// The arguments are untrusted text: cap each at 60 bytes (never mid-character) and neutralise control characters.
	std::string ca = logSafe(clip(a.data(), a.size(), 60)), cb = logSafe(clip(b.data(), b.size(), 60));
	char buf[256];
	std::snprintf(buf, sizeof buf, fmt, ca.c_str(), cb.c_str());
	if (st) {
		st->warningCount++;
		// Past the cap only the count grows, so a hostile body cannot flood the log; buildCatalog logs one summary.
		if (st->warningCount > kMaxWarnings) return;
		st->warnings.emplace_back(buf);
	}
	LOG("oscapi: %s", buf);
}

// Cheap pre-checks before jansson allocates anything: size, emptiness, nesting depth and node count. jansson's own
// depth limit is 2048 recursive calls, far too deep for the Wii's small thread stacks, and its memory is driven by
// the number of values rather than bytes (see kMaxNodes).
ApiError precheck(std::string_view json, size_t maxBytes, const char* what) {
	if (json.empty()) return fail(ApiErrorCode::Empty, std::string("The shop sent an empty ") + what + ".");
	if (json.size() > maxBytes)
		return fail(ApiErrorCode::TooLarge, std::string("The shop's ") + what + " is larger than expected.");
	int depth = 0;
	size_t nodes = 0; // openers + commas: about one per value
	bool inStr = false, esc = false;
	for (char ch : json) {
		if (inStr) {
			if (esc) esc = false;
			else if (ch == '\\') esc = true;
			else if (ch == '"') inStr = false;
		} else if (ch == '"') {
			inStr = true;
		} else if (ch == '[' || ch == '{') {
			if (++depth > kMaxDepth)
				return fail(ApiErrorCode::TooDeep, std::string("The shop's ") + what + " is damaged (nested too deeply).");
			if (++nodes > kMaxNodes) break;
		} else if (ch == ',') {
			if (++nodes > kMaxNodes) break;
		} else if (ch == ']' || ch == '}') {
			if (depth > 0) depth--;
		}
	}
	if (nodes > kMaxNodes) return fail(ApiErrorCode::TooLarge, std::string("The shop's ") + what + " is larger than expected.");
	return {};
}

ApiError load(std::string_view json, size_t maxBytes, const char* what, JsonPtr& out) {
	ApiError e = precheck(json, maxBytes, what);
	if (!e.ok()) return e;
	json_error_t err;
	out.reset(json_loadb(json.data(), json.size(), JSON_DECODE_ANY, &err));
	if (!out) {
		LOG("oscapi: %s: %s (line %d, column %d)", what, logSafe(err.text).c_str(), err.line, err.column);
		return fail(ApiErrorCode::Syntax, std::string("The shop's ") + what + " was cut off or damaged.");
	}
	return {};
}

// Rough size of a jansson tree: a node is ~40 bytes, an object member adds a hash bucket and key copy. This is a
// ballpark for the log line only, not a bound: the real limits are kMaxContentsBytes and kMaxNodes in precheck.
void domSize(const json_t* j, size_t& bytes, int depth = 0) {
	if (!j || depth > kMaxDepth + 1) return;
	bytes += 40;
	if (json_is_string(j)) {
		bytes += json_string_length(j) + 1;
	} else if (json_is_array(j)) {
		bytes += 8 * json_array_size(j);
		for (size_t i = 0; i < json_array_size(j); i++) domSize(json_array_get(j, i), bytes, depth + 1);
	} else if (json_is_object(j)) {
		const char* key;
		json_t* v;
		json_object_foreach(const_cast<json_t*>(j), key, v) {
			bytes += 64 + std::char_traits<char>::length(key);
			domSize(v, bytes, depth + 1);
		}
	}
}

// ---- Lenient field access: a wrong type or a missing field is an empty string or 0 ----

const json_t* member(const json_t* o, const char* key) {
	return json_is_object(o) ? json_object_get(o, key) : nullptr;
}

std::string str(const json_t* o, const char* key, size_t max = kMaxShortText) {
	const json_t* v = member(o, key);
	return json_is_string(v) ? clip(json_string_value(v), json_string_length(v), max) : std::string();
}

// For identifiers (slug, URLs): a value over `max` is invalid, never clipped, because a truncated URL or slug names
// something else. `*tooLong` tells the caller to warn.
std::string strExact(const json_t* o, const char* key, size_t max, bool* tooLong) {
	const json_t* v = member(o, key);
	if (!json_is_string(v)) return std::string();
	if (json_string_length(v) > max) {
		if (tooLong) *tooLong = true;
		return std::string();
	}
	return std::string(json_string_value(v), json_string_length(v));
}

uint64_t num(const json_t* o, const char* key, uint64_t max) {
	const json_t* v = member(o, key);
	if (json_is_integer(v)) {
		json_int_t i = json_integer_value(v);
		return i <= 0 ? 0 : std::min<uint64_t>(static_cast<uint64_t>(i), max);
	}
	if (json_is_real(v)) {
		double d = json_real_value(v);
		if (!(d > 0)) return 0; // negative, zero and NaN
		if (d >= static_cast<double>(max)) return max;
		return static_cast<uint64_t>(d);
	}
	return 0;
}

// With `overlong` set, items longer than kMaxShortText are skipped (and counted) instead of clipped, for values where a
// clipped copy would be a different, wrong value (paths). Otherwise they are clipped, which is fine for display text.
std::vector<std::string> strList(const json_t* o, const char* key, size_t* overlong = nullptr,
                                 size_t maxItems = kMaxListItems) {
	std::vector<std::string> out;
	const json_t* a = member(o, key);
	if (!json_is_array(a)) return out;
	size_t n = std::min(json_array_size(a), maxItems);
	for (size_t i = 0; i < n; i++) {
		const json_t* v = json_array_get(a, i);
		if (!json_is_string(v) || json_string_length(v) == 0) continue;
		if (overlong && json_string_length(v) > kMaxShortText) {
			(*overlong)++;
			continue;
		}
		std::string s = clip(json_string_value(v), json_string_length(v), kMaxShortText);
		// The live data repeats entries (wii_remote twice); the UI should not show them twice.
		if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(std::move(s));
	}
	return out;
}

bool isMd5(const std::string& s) {
	if (s.size() != 32) return false;
	return std::all_of(s.begin(), s.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
}

std::string md5Of(const json_t* asset) {
	std::string h = str(asset, "hash", 64);
	h = strutil::lowerAscii(h);
	return isMd5(h) ? h : std::string(); // a bad hash is dropped, never half-trusted
}

// A name must show something. Beyond ASCII controls and spaces, a name of only Unicode spaces and zero-width
// characters would render as an empty row.
bool isInvisibleCodePoint(uint32_t cp) {
	return cp == 0xA0 || cp == 0x85 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200F) || cp == 0x2028 || cp == 0x2029 ||
	       cp == 0x202F || cp == 0x205F || cp == 0x3000 || cp == 0xFEFF;
}

bool hasVisibleChar(const std::string& s) {
	for (size_t i = 0; i < s.size();) {
		unsigned char u = static_cast<unsigned char>(s[i]);
		if (u < 0x80) {
			if (u > 0x20 && u != 0x7F) return true;
			i++;
			continue;
		}
		// jansson guarantees valid UTF-8, so the lead byte gives the length.
		size_t n = u >= 0xF0 ? 4 : u >= 0xE0 ? 3 : 2;
		uint32_t cp = u & (0xFF >> (n + 1));
		for (size_t k = 1; k < n && i + k < s.size(); k++) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
		if (!isInvisibleCodePoint(cp)) return true;
		i += n;
	}
	return false;
}

using strutil::lowerAscii;

// Subdirectories become install paths later (and the uninstaller deletes them), so only plain absolute paths with
// FAT-safe segments survive. A trailing slash is rejected rather than stripped: the stored string is then exactly the
// path M3 will use, with no second normalisation to disagree about.
bool safeSubdir(const std::string& p) {
	if (p.size() < 2 || p.size() > kMaxSubdirPath || p[0] != '/' || p.back() == '/') return false;
	size_t i = 1;
	while (i <= p.size()) {
		size_t j = p.find('/', i);
		if (j == std::string::npos) j = p.size();
		std::string_view seg(p.data() + i, j - i);
		if (seg.empty() || seg.size() > kMaxSubdirSegment || seg == "." || seg == "..") return false;
		if (seg.back() == '.' || seg.back() == ' ') return false; // FAT/Windows silently drop these
		for (char c : seg) {
			if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F || std::string_view("\\<>:\"|?*").find(c) != std::string_view::npos)
				return false;
		}
		if (isReservedDeviceName(seg)) return false;
		i = j + 1;
	}
	return true;
}

// An asset URL (binary and meta; archives go through rebuildArchiveUrl): http(s) only (never file:, ftp:,
// javascript: or a bare path), and never clipped. Anything else is blanked with a warning, so the entry stays but
// that download is unavailable.
std::string urlOf(const json_t* asset, const std::string& slug, const char* what, ApiStats* stats) {
	bool tooLong = false;
	std::string u = strExact(asset, "url", kMaxShortText, &tooLong);
	if (tooLong) {
		warn(stats, "'%s': dropped over-long %s URL", slug, what);
		return std::string();
	}
	if (!u.empty() && !isHttpUrl(u)) {
		warn(stats, "'%s': dropped %s URL with an unsupported scheme", slug, what);
		return std::string();
	}
	return u;
}

// The archive URL is rebuilt, never copied: only the OSC host, plain http, one validated file name.
std::string archiveUrlOf(const json_t* archive, const std::string& slug, ApiStats* stats) {
	bool tooLong = false;
	std::string u = strExact(archive, "url", kMaxShortText, &tooLong);
	if (tooLong) {
		warn(stats, "'%s': dropped over-long %s URL", slug, "archive");
		return std::string();
	}
	if (u.empty()) return u;
	std::string rebuilt = rebuildArchiveUrl(slug, u);
	if (rebuilt.empty()) warn(stats, "'%s': dropped %s URL that is not an OSC archive", slug, "archive");
	return rebuilt;
}

size_t entryBytesOf(const AppEntry& a) {
	size_t n = sizeof(AppEntry);
	for (const std::string* s : {&a.slug, &a.name, &a.author, &a.category, &a.shortDesc, &a.longDesc, &a.version,
	                             &a.iconUrl, &a.archiveUrl, &a.archiveMd5, &a.binaryUrl, &a.binaryMd5, &a.metaUrl})
		n += s->capacity();
	n += a.nameL.capacity() + a.slugL.capacity() + a.authorL.capacity() + a.descL.capacity();
	for (const auto* v : {&a.authors, &a.contributors, &a.peripherals, &a.platforms, &a.flags, &a.subdirectories}) {
		n += v->capacity() * sizeof(std::string);
		for (const auto& s : *v) n += s.capacity();
	}
	return n;
}

using timeutil::msSince;

struct KnownCategory {
	const char *name, *display, *singular, *plural;
};
const KnownCategory kKnown[] = {
	{"demos", "Demos", "demo", "demos"},
	{"emulators", "Emulators", "emulator", "emulators"},
	{"games", "Games", "game", "games"},
	{"media", "Media", "media application", "media applications"},
	{"utilities", "Utilities", "utility", "utilities"},
};

} // namespace

ApiError parseInformation(std::string_view json, Catalog& c, ApiStats* stats) {
	JsonPtr root;
	ApiError e = load(json, kMaxInformationBytes, "category list", root);
	if (!e.ok()) return e;
	if (!json_is_object(root.get()))
		return fail(ApiErrorCode::WrongType, "The shop's category list is not in the expected format.");
	const json_t* arr = member(root.get(), "available_categories");
	if (!json_is_array(arr))
		return fail(ApiErrorCode::WrongType, "The shop's category list is missing its categories.");

	std::vector<Category> cats;
	std::set<std::string> seen;
	for (size_t i = 0; i < json_array_size(arr); i++) {
		const json_t* o = json_array_get(arr, i);
		Category cat;
		cat.name = str(o, "name", 64);
		if (cat.name.empty() || !seen.insert(cat.name).second) {
			warn(stats, "skipping category #%s with a missing or repeated name", std::to_string(i));
			continue;
		}
		if (cats.size() >= kMaxCategories) {
			if (stats) stats->categoriesDropped++;
			continue;
		}
		cat.display = str(o, "display_name", 64);
		if (cat.display.empty()) cat.display = cat.name;
		cat.singular = str(o, "singular", 64);
		cat.plural = str(o, "plural", 64);
		if (cat.singular.empty()) cat.singular = "app";
		if (cat.plural.empty()) cat.plural = "apps";
		cats.push_back(std::move(cat));
	}
	if (cats.empty()) return fail(ApiErrorCode::NoApps, "The shop's category list is empty.");
	c.categories = std::move(cats);
	if (c.categories.size() > 5)
		LOG("oscapi: %zu categories; Home shows the first 5 and All apps reaches the rest", c.categories.size());
	return {};
}

ApiError parseContents(std::string_view json, Catalog& c, ApiStats* stats) {
	assert(c.apps.empty()); // the duplicate-slug check below only sees this body's apps
	auto t0 = std::chrono::steady_clock::now();
	JsonPtr root;
	ApiError e = load(json, kMaxContentsBytes, "app list", root);
	if (!e.ok()) return e;
	if (!json_is_array(root.get())) return fail(ApiErrorCode::WrongType, "The shop's app list is not in the expected format.");

	size_t dom = 0;
	domSize(root.get(), dom);
	if (stats) {
		stats->rawBytes = json.size();
		stats->domBytes = dom;
	}

	std::set<std::string> seenSlugs; // lowercase: the SD card is case-insensitive, so "Foo" and "foo" would collide
	size_t kept = 0, skipped = 0, entryBytes = 0, smaller = 0;
	size_t total = json_array_size(root.get());
	std::vector<AppEntry> apps;
	apps.reserve(std::min(total, kMaxApps));
	for (size_t i = 0; i < total; i++) {
		const json_t* o = json_array_get(root.get(), i);
		if (apps.size() >= kMaxApps) {
			warn(stats, "app list truncated at %s apps", std::to_string(kMaxApps));
			skipped += total - i;
			break;
		}
		if (!json_is_object(o)) {
			warn(stats, "skipping entry #%s: not an object", std::to_string(i));
			skipped++;
			continue;
		}
		AppEntry a;
		bool slugTooLong = false;
		a.slug = strExact(o, "slug", kMaxSlug, &slugTooLong);
		if (slugTooLong || !isValidSlug(a.slug)) {
			warn(stats, "skipping entry #%s: bad slug '%s'", std::to_string(i), a.slug);
			skipped++;
			continue;
		}
		a.name = str(o, "name");
		if (!hasVisibleChar(a.name)) {
			warn(stats, "skipping '%s': no name", a.slug);
			skipped++;
			continue;
		}
		if (!seenSlugs.insert(lowerAscii(a.slug)).second) {
			warn(stats, "skipping '%s': duplicate slug", a.slug);
			skipped++;
			continue;
		}
		a.author = str(o, "author");
		a.category = str(o, "category", 64);
		a.version = str(o, "version", 64);
		const json_t* desc = member(o, "description");
		a.shortDesc = str(desc, "short", 2048);
		a.longDesc = str(desc, "long", kMaxLongText);
		a.authors = strList(o, "authors");
		a.contributors = strList(o, "contributors");
		if (a.author.empty() && !a.authors.empty()) a.author = a.authors.front();

		const json_t* assets = member(o, "assets");
		const json_t* icon = member(assets, "icon");
		const json_t* archive = member(assets, "archive");
		const json_t* binary = member(assets, "binary");
		const json_t* meta = member(assets, "meta");
		// Icons always come from the plain-HTTP pattern (the API's own icon URL is https, which the Wii cannot do).
		if (json_is_object(icon)) a.iconUrl = iconUrl(a.slug);
		a.iconSize = num(icon, "size", UINT64_C(1) << 40);
		a.archiveUrl = archiveUrlOf(archive, a.slug, stats);
		a.archiveMd5 = md5Of(archive);
		a.archiveSize = num(archive, "size", UINT64_C(1) << 40);
		a.binaryUrl = urlOf(binary, a.slug, "binary", stats);
		a.binaryMd5 = md5Of(binary);
		a.binarySize = num(binary, "size", UINT64_C(1) << 40);
		a.metaUrl = urlOf(meta, a.slug, "meta", stats);

		a.uncompressedSize = num(o, "uncompressed_size", UINT64_C(1) << 40);
		a.downloads = static_cast<uint32_t>(num(o, "downloads", UINT32_MAX));
		// Not clamped: a clamped far-future date would win "newest" and be shown as a real date. Unknown is 0.
		uint64_t rd = num(o, "release_date", UINT64_MAX >> 1);
		a.releaseDate = rd > kMaxReleaseDate ? 0 : static_cast<int64_t>(rd);
		if (rd > kMaxReleaseDate) warn(stats, "'%s': release date out of range, treated as unknown", a.slug);
		std::string pt = lowerAscii(str(o, "package_type", 16));
		a.packageType = pt == "dol" ? PackageType::Dol : pt == "elf" ? PackageType::Elf : PackageType::Unknown;
		a.peripherals = strList(o, "peripherals");
		a.platforms = strList(o, "supported_platforms");
		a.flags = strList(o, "flags");
		size_t overlong = 0;
		// The real data has up to 79 subdirectories (cavestory), so the general 64-item cap would silently lose some
		// of the folders an uninstall must remove.
		if (json_array_size(member(o, "subdirectories")) > kMaxSubdirectories)
			warn(stats, "'%s': more than %s subdirectories, the rest ignored", a.slug, std::to_string(kMaxSubdirectories));
		for (auto& s : strList(o, "subdirectories", &overlong, kMaxSubdirectories)) {
			if (safeSubdir(s)) a.subdirectories.push_back(std::move(s));
			else warn(stats, "'%s': dropped unsafe subdirectory '%s'", a.slug, s);
		}
		if (overlong) warn(stats, "'%s': dropped over-long subdirectories (%s)", a.slug, std::to_string(overlong));
		if (a.uncompressedSize > 0 && a.archiveSize > 0 && a.uncompressedSize < a.archiveSize) smaller++;
		entryBytes += entryBytesOf(a);
		apps.push_back(std::move(a));
		kept++;
	}
	root.reset(); // the tree is no longer needed; free it before anything else allocates
	if (smaller > 0) LOG("oscapi: %zu apps have an uncompressed size below the archive size (allowed)", smaller);

	if (kept == 0) return fail(ApiErrorCode::NoApps, "The shop's app list had no usable apps.");
	for (auto& a : apps) c.apps.push_back(std::move(a));
	if (stats) {
		stats->appsKept = kept;
		stats->appsSkipped = skipped;
		stats->entryBytes = entryBytes;
		stats->parseMs = msSince(t0);
	}
	return {};
}

ApiError parseFeatured(std::string_view json, std::string& slug) {
	slug.clear();
	JsonPtr root;
	ApiError e = load(json, kMaxFeaturedBytes, "featured app", root);
	if (!e.ok()) return e;
	if (!json_is_object(root.get()))
		return fail(ApiErrorCode::WrongType, "The shop's featured app is not in the expected format.");
	std::string s = strExact(root.get(), "slug", kMaxSlug, nullptr);
	if (!isValidSlug(s))
		return fail(ApiErrorCode::WrongType, "The shop's featured app has no usable name.");
	slug = std::move(s);
	return {};
}

void deriveCategories(Catalog& c) {
	std::set<std::string> present;
	for (const auto& a : c.apps) present.insert(a.category);
	c.categories.clear();
	for (const auto& k : kKnown) {
		if (present.erase(k.name)) c.categories.push_back({k.name, k.display, k.singular, k.plural});
	}
	for (const auto& name : present) {
		if (name.empty() || c.categories.size() >= kMaxCategories) continue;
		std::string disp = name;
		disp[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(disp[0])));
		c.categories.push_back({name, disp, "app", "apps"});
	}
}

BuildResult buildCatalog(std::optional<std::string_view> informationJson, std::string_view contentsJson,
                         std::optional<std::string_view> featuredJson) {
	BuildResult r;
	Catalog& c = r.catalog;
	r.error = parseContents(contentsJson, c, &r.stats);
	if (!r.error.ok()) return r;

	bool haveCategories = false;
	if (informationJson) {
		ApiError ie = parseInformation(*informationJson, c, &r.stats);
		if (ie.ok()) haveCategories = true;
		else warn(&r.stats, "category list unusable (%s); deriving categories", ie.message);
	}
	if (!haveCategories) deriveCategories(c);

	if (featuredJson) {
		ApiError fe = parseFeatured(*featuredJson, c.featuredSlug);
		if (!fe.ok()) warn(&r.stats, "featured app unusable (%s); using the newest app", fe.message);
	}

	c.finalize(); // sanitizes text, indexes, and falls back to the newest app when the featured slug is unknown
	if (!c.featuredSlug.empty() && !c.find(c.featuredSlug)) {
		warn(&r.stats, "featured app '%s' is not in the catalog; using the newest app", c.featuredSlug);
	}
	if (const AppEntry* f = c.featured()) c.featuredSlug = f->slug; // report the app Home will really show
	size_t unknownCat = 0;
	for (const auto& a : c.apps) {
		if (!c.category(a.category)) unknownCat++;
	}
	if (unknownCat) warn(&r.stats, "%s apps have a category the shop did not list (shown under All apps only)", std::to_string(unknownCat));
	if (r.stats.warningCount > kMaxWarnings)
		LOG("oscapi: %zu more warnings suppressed", r.stats.warningCount - kMaxWarnings);
	if (r.stats.categoriesDropped) LOG("oscapi: %zu categories beyond the limit were ignored", r.stats.categoriesDropped);

	LOG("oscapi: %zu apps (%zu skipped), %zu categories, parse %.1f ms; memory raw %zu + dom ~%zu + entries ~%zu = peak ~%zu KB",
	    r.stats.appsKept, r.stats.appsSkipped, c.categories.size(), r.stats.parseMs, r.stats.rawBytes / 1024,
	    r.stats.domBytes / 1024, r.stats.entryBytes / 1024, r.stats.peakBytes() / 1024);
	return r;
}

} // namespace oscapi
