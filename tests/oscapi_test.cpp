// Host-side tests for the OSC API parser: the captured real responses in tests/fixtures/ (run from the repo root),
// hand-made hostile inputs, and a seeded mutation fuzz. Build and run: make -f Makefile.pc test
#include <jansson.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <random>

#include <fcntl.h>
#include <unistd.h>
#include <sstream>

#include "catalog/OscApi.h"

static int failures = 0;
// Failures go to a saved copy of the real stderr, so they stay visible while a test redirects fd 2 (the fuzz run
// silences the parser's logging; captureStderr reads it back).
static int g_errFd = 2;
#define CHECK(cond) \
	do { if (!(cond)) { dprintf(g_errFd, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

using namespace oscapi;

static std::string readFile(const char* path) {
	std::ifstream f(path, std::ios::binary);
	if (!f) { std::fprintf(stderr, "cannot read %s (run from the repo root)\n", path); std::exit(2); }
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

static std::string info, contents, featured;

// Runs `f` with fd 2 pointed at a temporary file and returns what was written there.
static std::string captureStderr(const std::function<void()>& f) {
	std::fflush(stderr);
	FILE* t = std::tmpfile();
	int saved = dup(2);
	if (!t || saved < 0) { f(); return std::string(); }
	dup2(fileno(t), 2);
	f();
	std::fflush(stderr);
	dup2(saved, 2);
	close(saved);
	std::string out;
	char buf[4096];
	ssize_t n;
	off_t off = 0;
	while ((n = pread(fileno(t), buf, sizeof buf, off)) > 0) { out.append(buf, static_cast<size_t>(n)); off += n; }
	std::fclose(t);
	return out;
}

static bool validUtf8(const std::string& s) {
	for (size_t i = 0; i < s.size();) {
		unsigned char u = static_cast<unsigned char>(s[i]);
		size_t n = u < 0x80 ? 1 : (u & 0xE0) == 0xC0 ? 2 : (u & 0xF0) == 0xE0 ? 3 : (u & 0xF8) == 0xF0 ? 4 : 0;
		if (!n || i + n > s.size()) return false;
		for (size_t k = 1; k < n; k++) if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
		i += n;
	}
	return true;
}

// Peak resident set (VmHWM) in KB, or 0 where /proc is missing.
static size_t peakRssKb() {
	std::ifstream f("/proc/self/status");
	std::string line;
	while (std::getline(f, line)) {
		if (line.compare(0, 6, "VmHWM:") == 0) return static_cast<size_t>(std::atoll(line.c_str() + 6));
	}
	return 0;
}

// One valid app object with the given slug, for building hostile lists.
static std::string appJson(const std::string& slug, const std::string& extra = "") {
	return "{\"slug\":\"" + slug + "\",\"name\":\"N " + slug + "\",\"category\":\"games\"" + extra + "}";
}
static std::string list(std::initializer_list<std::string> items) {
	std::string s = "[";
	for (auto& i : items) { if (s.size() > 1) s += ","; s += i; }
	return s + "]";
}
static BuildResult build(const std::string& c) { return buildCatalog(std::nullopt, c, std::nullopt); }

static void testRealFixtures() {
	BuildResult r = buildCatalog(info, contents, featured);
	CHECK(r.ok());
	const Catalog& c = r.catalog;
	CHECK(c.apps.size() >= 300 && c.apps.size() <= 310);
	CHECK(r.stats.appsKept == c.apps.size());
	CHECK(c.categories.size() == 5);
	CHECK(c.categories[0].name == "demos" && c.categories[0].display == "Demos");
	CHECK(c.categories[3].name == "media" && c.categories[3].singular == "media application" &&
	      c.categories[3].plural == "media applications");
	CHECK(c.categories[4].name == "utilities");
	CHECK(c.featured() && c.featured()->slug == "gxgeo" && c.featuredSlug == "gxgeo");

	const AppEntry* d = c.find("WiiDonut");
	CHECK(d != nullptr);
	if (d) {
		CHECK(d->name == "Wii Donut" && d->author == "jornmann" && d->category == "demos");
		CHECK(d->authors.size() == 1 && d->authors[0] == "Andy Sloane" && d->contributors.empty());
		CHECK(d->shortDesc == "A spinning donut for your Wii!");
		CHECK(d->longDesc.find("Jogeir Liljedahl") != std::string::npos && d->longDesc.find('\r') == std::string::npos);
		CHECK(d->version == "1.0.0" && d->downloads == 32727 && d->releaseDate == 1644364800);
		CHECK(d->packageType == PackageType::Dol);
		CHECK(d->archiveMd5 == "4884206359441a725639c50aad33b74d" && d->archiveSize == 226230);
		CHECK(d->binaryMd5 == "d41f21924dd7a7d359721bbc53a782b0" && d->binarySize == 430656);
		CHECK(d->iconSize == 2404 && d->uncompressedSize == 433665);
		CHECK(d->iconUrl == "http://hbb1.oscwii.org/api/contents/WiiDonut/icon.png");
		CHECK(d->archiveUrl == "http://hbb1.oscwii.org/api/contents/WiiDonut/WiiDonut.zip");
		CHECK(d->metaUrl == "https://hbb1.oscwii.org/unzipped_apps/WiiDonut/apps/WiiDonut/meta.xml");
		CHECK(d->subdirectories.size() == 1 && d->subdirectories[0] == "/apps/WiiDonut");
		CHECK(d->platforms.size() == 3 && d->peripherals.size() == 1 && d->peripherals[0] == "wii_remote");
		CHECK(!d->nameL.empty() && !d->descL.empty());
	}
	const AppEntry* g = c.find("gxgeo");
	CHECK(g && g->peripherals.size() == 4); // "wii_remote" appears twice in the API and is shown once
	CHECK(g && g->subdirectories.size() == 6);

	size_t elf = 0, flagged = 0;
	for (const auto& a : c.apps) {
		CHECK(isValidSlug(a.slug) && !a.name.empty());
		CHECK(a.archiveMd5.size() == 32 && a.binaryMd5.size() == 32);
		CHECK(!a.iconUrl.empty() && !a.archiveUrl.empty() && !a.metaUrl.empty());
		CHECK(a.packageType != PackageType::Unknown);
		CHECK(a.name.find('\n') == std::string::npos && a.longDesc.find('\r') == std::string::npos);
		CHECK(c.category(a.category) != nullptr);
		if (a.packageType == PackageType::Elf) elf++;
		if (!a.flags.empty()) flagged++;
	}
	CHECK(elf == 16 && flagged > 0);
	// The strict rules reject nothing real: every app, slug, URL and all 1047 subdirectories survive, silently.
	size_t subdirs = 0, installable = 0, noDate = 0;
	for (const auto& a : c.apps) {
		subdirs += a.subdirectories.size();
		if (a.installable()) installable++;
		CHECK(!a.binaryUrl.empty());
		if (a.releaseDate <= 0) noDate++;
	}
	CHECK(subdirs == 1047 && noDate == 1); // lilyskate has a negative release_date upstream
	CHECK(c.find("cavestory") && c.find("cavestory")->subdirectories.size() == 79);
	CHECK(installable == c.apps.size());
	// Every archive URL is rebuilt to the plain-http form, and the stricter gate still passes all of them.
	for (const auto& a : c.apps) {
		CHECK(a.archiveUrl == "http://hbb1.oscwii.org/api/contents/" + a.slug + "/" + a.slug + ".zip");
		CHECK(a.installRefusal() == InstallRefusal::None && isAllowedDownloadUrl(a.archiveUrl));
	}
	const AppEntry* mame = c.find("mame4wii");
	CHECK(mame && mame->uncompressedSize < mame->archiveSize && mame->installable());
	CHECK(r.stats.appsSkipped == 0 && r.stats.warningCount == 0);
	CHECK(c.count("") == static_cast<int>(c.apps.size()));
	CHECK(c.count("demos") + c.count("emulators") + c.count("games") + c.count("media") + c.count("utilities") ==
	      static_cast<int>(c.apps.size()));
	CHECK(r.stats.parseMs >= 0 && r.stats.peakBytes() > contents.size());
	std::fprintf(stderr, "real contents: %zu apps, parse %.1f ms, raw %zu KB, dom ~%zu KB, entries ~%zu KB\n",
	             c.apps.size(), r.stats.parseMs, r.stats.rawBytes / 1024, r.stats.domBytes / 1024, r.stats.entryBytes / 1024);
}

static void testFallbacks() {
	// No information: categories derived in the site's order; no featured: newest app.
	BuildResult r = build(contents);
	CHECK(r.ok() && r.catalog.categories.size() == 5);
	CHECK(r.catalog.categories[0].name == "demos" && r.catalog.categories[0].display == "Demos");
	CHECK(r.catalog.categories[4].name == "utilities" && r.catalog.categories[4].plural == "utilities");
	int64_t newestDate = 0;
	for (const auto& a : r.catalog.apps) newestDate = std::max(newestDate, a.releaseDate);
	CHECK(r.catalog.featured()->releaseDate == newestDate);

	// Unknown featured slug, junk featured, junk information: still ok, with warnings.
	r = buildCatalog(std::nullopt, contents, std::string_view("{\"slug\":\"nope-not-here\"}"));
	CHECK(r.ok() && r.catalog.featured()->releaseDate == newestDate && r.stats.warningCount > 0);
	r = buildCatalog(std::string_view("not json"), contents, std::string_view("{\"slu"));
	CHECK(r.ok() && r.catalog.categories.size() == 5 && r.stats.warningCount >= 2);

	// Unknown categories: derived after the known ones, by name; apps keep them.
	r = build(list({appJson("a"), appJson("b", ",\"category\":\"zzz\""), appJson("c", ",\"category\":\"demos\"")}));
	CHECK(r.ok() && r.catalog.categories.size() == 3);
	CHECK(r.catalog.categories[0].name == "demos" && r.catalog.categories[1].name == "games" &&
	      r.catalog.categories[2].name == "zzz" && r.catalog.categories[2].display == "Zzz");

	// With an information list that lacks an app's category the app stays (under All apps) and a warning is logged.
	r = buildCatalog(info, list({appJson("a", ",\"category\":\"zzz\"")}), std::nullopt);
	CHECK(r.ok() && r.catalog.apps.size() == 1 && r.catalog.count("") == 1 && r.stats.warningCount >= 1);

	// More than 5 categories are all kept (Home takes the first 5), in API order.
	std::string cats = "{\"available_categories\":[";
	for (int i = 0; i < 9; i++) cats += std::string(i ? "," : "") + "{\"name\":\"c" + std::to_string(i) + "\"}";
	cats += "]}";
	Catalog c;
	CHECK(parseInformation(cats, c).ok() && c.categories.size() == 9 && c.categories[8].name == "c8");
	CHECK(c.categories[0].display == "c0" && c.categories[0].singular == "app" && c.categories[0].plural == "apps");
}

static void testJunk() {
	// Not parsable / wrong shape: a clear error, no catalog.
	struct Case { std::string body; ApiErrorCode code; };
	std::string big(kMaxContentsBytes + 1, ' ');
	std::string deep(500, '['); deep += std::string(500, ']');
	std::string deepObj;
	for (int i = 0; i < 100; i++) deepObj += "{\"a\":";
	std::vector<Case> cases = {
		{"", ApiErrorCode::Empty},
		{big, ApiErrorCode::TooLarge},
		{deep, ApiErrorCode::TooDeep},
		{deepObj, ApiErrorCode::TooDeep},
		{"[", ApiErrorCode::Syntax},
		{contents.substr(0, contents.size() / 2), ApiErrorCode::Syntax}, // truncated
		{contents.substr(0, 40), ApiErrorCode::Syntax},
		{"<html>502 Bad Gateway</html>", ApiErrorCode::Syntax},
		{"\xEF\xBB\xBF[]", ApiErrorCode::Syntax}, // BOM
		{"[\"\xFF\xFE\"]", ApiErrorCode::Syntax}, // invalid UTF-8
		{"[{\"slug\":\"a\xC3\"}]", ApiErrorCode::Syntax}, // cut-off UTF-8
		{"[\"\\u0000\"]", ApiErrorCode::Syntax}, // NUL escape
		{"{}", ApiErrorCode::WrongType},
		{"{\"apps\":[]}", ApiErrorCode::WrongType},
		{"null", ApiErrorCode::WrongType},
		{"42", ApiErrorCode::WrongType},
		{"\"str\"", ApiErrorCode::WrongType},
		{"true", ApiErrorCode::WrongType},
		{"[]", ApiErrorCode::NoApps},
		{"[null,1,\"x\",[],true]", ApiErrorCode::NoApps},
		{list({"{}", "{\"slug\":\"a\"}", "{\"name\":\"b\"}"}), ApiErrorCode::NoApps},
	};
	for (auto& k : cases) {
		BuildResult r = build(k.body);
		CHECK(!r.ok() && r.error.code == k.code && !r.error.message.empty());
		if (r.ok() || r.error.code != k.code) std::fprintf(stderr, "  case body starts: %.40s\n", k.body.c_str());
		CHECK(r.catalog.apps.empty());
	}
	CHECK(build("[]").error.message.find("no usable apps") != std::string::npos);

	// information and featured with the wrong shape.
	Catalog c;
	CHECK(parseInformation("[]", c).code == ApiErrorCode::WrongType);
	CHECK(parseInformation("{\"available_categories\":5}", c).code == ApiErrorCode::WrongType);
	CHECK(parseInformation("{\"available_categories\":[]}", c).code == ApiErrorCode::NoApps);
	CHECK(parseInformation("{\"available_categories\":[{\"name\":5},null,{}]}", c).code == ApiErrorCode::NoApps);
	CHECK(c.categories.empty());
	std::string slug = "x";
	CHECK(parseFeatured("[]", slug).code == ApiErrorCode::WrongType && slug.empty());
	CHECK(parseFeatured("{\"slug\":5}", slug).code == ApiErrorCode::WrongType);
	CHECK(parseFeatured("{\"slug\":\"../x\"}", slug).code == ApiErrorCode::WrongType && slug.empty());
	CHECK(parseFeatured("{\"slug\":\"ok-1\"}", slug).ok() && slug == "ok-1");
	CHECK(parseFeatured("", slug).code == ApiErrorCode::Empty);
	CHECK(parseFeatured("{\"slug\":\"oops", slug).code == ApiErrorCode::Syntax);
	// The 404 body from the real server is valid JSON without a slug.
	CHECK(parseFeatured("{\"status\":404,\"error\":\"Not Found\"}", slug).code == ApiErrorCode::WrongType);
}

static void testAppRecovery() {
	// Bad apps are skipped, good ones survive.
	BuildResult r = build(list({appJson("good1"), "null", "[1]", "{\"name\":\"no slug\"}", "{\"slug\":\"noname\"}",
	                            "{\"slug\":\"blank\",\"name\":\" \\n\\t \"}", appJson("../x"), appJson("a/b"),
	                            appJson(".hidden"), appJson("sp ace"), appJson("tab\\tx"), appJson("good1"),
	                            appJson("GOOD1"), "{\"slug\":7,\"name\":\"n\"}", appJson("good2")}));
	CHECK(r.ok() && r.catalog.apps.size() == 2);
	CHECK(r.catalog.find("good1") && r.catalog.find("good2") && !r.catalog.find("GOOD1"));
	CHECK(r.stats.appsKept == 2 && r.stats.appsSkipped == 13 && r.stats.warningCount >= 13);

	// Duplicate slug: the first wins.
	r = build(list({"{\"slug\":\"d\",\"name\":\"First\",\"category\":\"games\"}",
	                "{\"slug\":\"d\",\"name\":\"Second\",\"category\":\"games\"}"}));
	CHECK(r.ok() && r.catalog.apps.size() == 1 && r.catalog.apps[0].name == "First");

	// Wrong types, nulls and missing fields become empty or zero; unknown fields are ignored.
	r = build(list({"{\"slug\":\"w\",\"name\":\"W\",\"author\":7,\"authors\":\"x\",\"contributors\":[1,null,\"c\",[]],"
	                "\"category\":null,\"description\":[\"s\"],\"downloads\":\"many\",\"assets\":[],"
	                "\"flags\":{\"a\":1},\"package_type\":5,\"peripherals\":null,\"release_date\":null,"
	                "\"subdirectories\":[\"/ok\",\"..\",\"/a/../b\",\"rel\",\"/a\\\\b\",\"//x\",3],"
	                "\"supported_platforms\":[\"wii\",\"wii\",\"\"],\"uncompressed_size\":[],\"version\":false,"
	                "\"shop\":{\"x\":1},\"notices\":[1],\"brand_new_field\":{\"deep\":[1,2]}}"}));
	CHECK(r.ok() && r.catalog.apps.size() == 1);
	const AppEntry& a = r.catalog.apps[0];
	CHECK(a.author.empty() && a.authors.empty() && a.contributors.size() == 1 && a.category.empty());
	CHECK(a.shortDesc.empty() && a.longDesc.empty() && a.downloads == 0 && a.iconUrl.empty() && a.archiveUrl.empty());
	CHECK(a.flags.empty() && a.packageType == PackageType::Unknown && a.peripherals.empty() && a.releaseDate == 0);
	CHECK(a.subdirectories.size() == 1 && a.subdirectories[0] == "/ok");
	CHECK(a.platforms.size() == 1 && a.uncompressedSize == 0 && a.version.empty());

	// Author falls back to the first of authors.
	r = build(list({appJson("au", ",\"authors\":[\"Zed\",\"Y\"]")}));
	CHECK(r.ok() && r.catalog.apps[0].author == "Zed" && r.catalog.apps[0].authorL == "zed");

	// Numbers: negative, huge, fractional, exponent and out-of-int64 values clamp instead of wrapping.
	r = build(list({appJson("n", ",\"downloads\":-5,\"release_date\":-99,\"uncompressed_size\":-1,"
	                              "\"assets\":{\"archive\":{\"size\":-3,\"hash\":\"x\"},\"binary\":{\"size\":1e300},"
	                              "\"icon\":{\"size\":99999999999}}"),
	                appJson("m", ",\"downloads\":1e30,\"release_date\":9e18,\"uncompressed_size\":1.5e3"),
	                appJson("k", ",\"downloads\":4000000000.7,\"release_date\":1.7e9")}));
	CHECK(r.ok() && r.catalog.apps.size() == 3);
	// An integer literal beyond int64 (or a real beyond double) is refused by jansson: a clear Syntax error.
	CHECK(build(list({appJson("o", ",\"downloads\":99999999999999999999")})).error.code == ApiErrorCode::Syntax);
	CHECK(build(list({appJson("o", ",\"downloads\":1e999")})).error.code == ApiErrorCode::Syntax);
	const AppEntry& n = r.catalog.apps[0];
	CHECK(n.downloads == 0 && n.releaseDate == 0 && n.uncompressedSize == 0 && n.archiveSize == 0);
	CHECK(n.binarySize == (UINT64_C(1) << 40) && n.iconSize == 99999999999ULL && n.iconUrl.size() > 0);
	const AppEntry& m = r.catalog.apps[1];
	CHECK(m.downloads == UINT32_MAX && m.releaseDate == 0 && m.uncompressedSize == 1500); // 9e18 is not a date
	CHECK(r.catalog.apps[2].downloads == 4000000000u && r.catalog.apps[2].releaseDate == 1700000000);

	// Hashes: only 32 hex digits survive, lowercased.
	r = build(list({appJson("h", ",\"assets\":{\"archive\":{\"hash\":\"ABCDEF0123456789ABCDEF0123456789\"},"
	                              "\"binary\":{\"hash\":\"abc\"}}")}));
	CHECK(r.ok() && r.catalog.apps[0].archiveMd5 == "abcdef0123456789abcdef0123456789" && r.catalog.apps[0].binaryMd5.empty());

	// Unknown package type, flags, platforms and peripherals are kept as plain strings.
	r = build(list({appJson("u", ",\"package_type\":\"exe\",\"flags\":[\"NEW_FLAG\"],\"supported_platforms\":[\"gamecube\"],"
	                              "\"peripherals\":[\"jetpack\"]")}));
	CHECK(r.ok() && r.catalog.apps[0].packageType == PackageType::Unknown && r.catalog.apps[0].hasFlag("NEW_FLAG"));
	CHECK(r.catalog.apps[0].platforms[0] == "gamecube" && r.catalog.apps[0].peripherals[0] == "jetpack");
	r = build(list({appJson("e", ",\"package_type\":\"ELF\"")}));
	CHECK(r.ok() && r.catalog.apps[0].packageType == PackageType::Elf);

	// Text hygiene is Catalog::finalize's job: CRLF, tabs, newlines and control characters in names and descriptions.
	r = build(list({"{\"slug\":\"t\",\"name\":\"Na\\tme\\r\\nX\\u0007Y\",\"author\":\"A\\nB\","
	                "\"description\":{\"short\":\"s\\r\\nt\",\"long\":\"l1\\r\\nl2\\tz\\r\\n\"},\"category\":\"games\"}"}));
	CHECK(r.ok());
	const AppEntry& t = r.catalog.apps[0];
	CHECK(t.name == "Na me XY" && t.author == "A B" && t.shortDesc == "s t" && t.longDesc == "l1\nl2 z\n");
}

static void testBounds() {
	// Huge strings are clipped at a UTF-8 boundary; huge arrays are capped; many apps are capped.
	std::string longName(100000, 'a');
	std::string euro;
	for (int i = 0; i < 5000; i++) euro += "\xE2\x82\xAC"; // 3-byte characters, so a cut can land mid-character
	std::string items = "[";
	for (int i = 0; i < 5000; i++) items += std::string(i ? "," : "") + "\"p" + std::to_string(i) + "\"";
	items += "]";
	BuildResult r = build(list({"{\"slug\":\"big\",\"name\":\"" + longName + "\",\"author\":\"" + euro +
	                            "\",\"category\":\"games\",\"description\":{\"short\":\"" + euro + "\",\"long\":\"" +
	                            std::string(200000, 'z') + "\"},\"flags\":" + items + ",\"peripherals\":" + items + "}"}));
	CHECK(r.ok());
	const AppEntry& a = r.catalog.apps[0];
	CHECK(a.name.size() == kMaxShortText && a.longDesc.size() == kMaxLongText);
	CHECK(a.author.size() <= kMaxShortText && a.author.size() % 3 == 0);
	CHECK(a.shortDesc.size() <= 2048 && a.shortDesc.size() % 3 == 0);
	CHECK(a.flags.size() == kMaxListItems && a.peripherals.size() == kMaxListItems);
	CHECK(build(list({appJson(std::string(kMaxSlug, 'a'))})).ok());
	CHECK(!build(list({appJson(std::string(kMaxSlug + 1, 'a'))})).ok());
	CHECK(!build(list({appJson(std::string(5000, 'a'))})).ok());

	std::string many = "[";
	for (size_t i = 0; i < kMaxApps + 500; i++) many += std::string(i ? "," : "") + appJson("a" + std::to_string(i));
	many += "]";
	r = build(many);
	CHECK(r.ok() && r.catalog.apps.size() == kMaxApps && r.stats.appsSkipped == 500);
	CHECK(r.stats.warnings.size() <= kMaxWarnings);

	// Warnings are bounded even when every app is bad.
	std::string bad = "[";
	for (int i = 0; i < 2000; i++) bad += std::string(i ? "," : "") + "{\"slug\":\"../" + std::to_string(i) + "\",\"name\":\"n\"}";
	bad += "," + appJson("ok") + "]";
	r = build(bad);
	CHECK(r.ok() && r.catalog.apps.size() == 1 && r.stats.warnings.size() == kMaxWarnings && r.stats.warningCount >= 2000);

	// Many categories are capped.
	std::string cats = "{\"available_categories\":[";
	for (int i = 0; i < 500; i++) cats += std::string(i ? "," : "") + "{\"name\":\"c" + std::to_string(i % 400) + "\"}";
	cats += "]}";
	Catalog c;
	ApiStats st;
	CHECK(parseInformation(cats, c, &st).ok() && c.categories.size() == kMaxCategories && st.categoriesDropped > 0);

	// Nesting right at the limit is accepted, one more is not; brackets inside strings do not count.
	std::string atLimit(kMaxDepth - 1, '['); atLimit += std::string(kMaxDepth - 1, ']');
	CHECK(build("[" + atLimit + "," + appJson("x") + "]").ok());
	std::string tooDeep(kMaxDepth, '['); tooDeep += std::string(kMaxDepth, ']');
	CHECK(build("[" + tooDeep + "," + appJson("x") + "]").error.code == ApiErrorCode::TooDeep);
	CHECK(build(list({appJson("s", ",\"version\":\"[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[\\\"[[[[\"")})).ok());
}

static void testUrls() {
	CHECK(informationUrl() == "http://hbb1.oscwii.org/api/v4/information");
	CHECK(contentsUrl() == "http://hbb1.oscwii.org/api/v4/contents");
	CHECK(featuredUrl() == "http://hbb1.oscwii.org/api/v4/featured-app");
	CHECK(iconUrl("abc") == "http://hbb1.oscwii.org/api/contents/abc/icon.png");
}


// ---- Hostile-input limits ----

static std::string repeat(const std::string& unit, size_t times) {
	std::string s;
	s.reserve(unit.size() * times);
	for (size_t i = 0; i < times; i++) s += unit;
	return s;
}

static void testNodeLimit() {
	// The measured amplification: an 8 MB `[{},{},...]` made jansson peak at 645 MB. The byte limit alone now stops
	// it, and so does the node limit for bodies that fit under the byte limit.
	CHECK(kMaxContentsBytes <= 2 * 1024 * 1024);
	size_t rssBefore = peakRssKb();
	auto t0 = std::chrono::steady_clock::now();
	std::string huge = "[" + repeat("{},", 8 * 1024 * 1024 / 3) + "{}]";
	CHECK(huge.size() > 8 * 1024 * 1024 - 8 && build(huge).error.code == ApiErrorCode::TooLarge);
	// Under the byte limit but far over the node limit: three kinds of tiny elements.
	std::string objs = "[" + repeat("{},", (kMaxContentsBytes - 16) / 3) + "{}]";
	std::string nums = "[" + repeat("1,", (kMaxContentsBytes - 16) / 2) + "1]";
	std::string arrs = "[" + repeat("[],", (kMaxContentsBytes - 16) / 3) + "[]]";
	for (const std::string* b : {&objs, &nums, &arrs}) {
		CHECK(b->size() <= kMaxContentsBytes);
		BuildResult r = build(*b);
		CHECK(!r.ok() && r.error.code == ApiErrorCode::TooLarge && r.catalog.apps.empty());
	}
	std::string slugOut;
	CHECK(parseFeatured("[" + repeat("1,", kMaxNodes) + "1]", slugOut).code == ApiErrorCode::TooLarge);
	double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	CHECK(ms < 2000); // rejected by a linear scan, not by building a tree
	size_t rssAfter = peakRssKb();
	if (rssBefore && rssAfter) CHECK(rssAfter - rssBefore < 64 * 1024); // the old code peaked ~645 MB higher
	std::fprintf(stderr, "node limit: hostile bodies rejected in %.0f ms, peak RSS grew %zu KB\n", ms, rssAfter - rssBefore);

	// The limit itself: exactly kMaxNodes nodes passes the scan (and then has no apps), one more does not.
	std::string atLimit = "[" + repeat("1,", kMaxNodes - 1) + "1]";
	CHECK(build(atLimit).error.code == ApiErrorCode::NoApps);
	std::string over = "[" + repeat("1,", kMaxNodes) + "1]";
	CHECK(build(over).error.code == ApiErrorCode::TooLarge);
	// Commas and brackets inside strings are not nodes.
	CHECK(build(list({appJson("s", ",\"version\":\"" + repeat(",{[", kMaxNodes) + "\"")})).ok()); // 450 KB, under the byte limit
}

static void testDepthScan() {
	std::string tooDeep(kMaxDepth, '['); tooDeep += std::string(kMaxDepth, ']');
	// `\\"` is an escaped backslash and then the closing quote: the brackets after it are real.
	CHECK(build("[\"\\\\\"," + tooDeep + "]").error.code == ApiErrorCode::TooDeep);
	// `\"` does not end the string: the brackets stay inside it.
	CHECK(build(list({appJson("e", ",\"version\":\"\\\"" + std::string(40, '[') + "\"")})).ok());
	// An unterminated string swallows the brackets inside it: a syntax error, not "too deep".
	CHECK(build("[\"abc " + std::string(40, '[')).error.code == ApiErrorCode::Syntax);
	CHECK(build("[{\"a\":\"" + std::string(40, '[')).error.code == ApiErrorCode::Syntax);
}

static std::vector<std::string> subdirsOf(const std::string& jsonList, size_t* warnings = nullptr) {
	BuildResult r = build(list({appJson("sd", ",\"subdirectories\":[" + jsonList + "]")}));
	CHECK(r.ok());
	if (warnings) *warnings = r.stats.warningCount;
	return r.ok() ? r.catalog.apps[0].subdirectories : std::vector<std::string>();
}

static void testSubdirs() {
	// Accepted: plain absolute paths, spaces and non-ASCII inside a segment, dots inside a name, up to 240 bytes.
	std::string max239 = "\"/" + std::string(239, 'a') + "\"";
	std::string seg255ok = "\"/" + std::string(100, 'b') + "/" + std::string(100, 'c') + "\"";
	size_t warned = 0;
	auto ok = subdirsOf("\"/apps/Foo\",\"/a b\",\"/caf\xC3\xA9\",\"/a.b/c-d_e\",\"/x\"," + max239 + "," + seg255ok, &warned);
	CHECK(ok.size() == 7 && warned == 0);
	// Rejected, each on its own so a pass cannot hide behind a neighbour.
	const char* bad[] = {
		"\"/\"", "\"/.\"", "\"/..\"", "\"/a/./b\"", "\"/a/../b\"", "\"/a.\"", "\"/a \"", "\"/...\"", "\"/a:b\"",
		"\"/COM1\"", "\"/a/con.txt\"", "\"/a/\"", "\"/a//b\"", "\"//a\"", "\"rel\"", "\"a/b\"", "\"/a\\\\b\"", "\"/a\\u0001b\"",
		"\"/a\\u007fb\"", "\"/a\\nb\"", "\"/a<b\"", "\"/a>b\"", "\"/a\\\"b\"", "\"/a|b\"", "\"/a?b\"", "\"/a*b\"", "\"/a:\"",
		"\"/lpt9.x\"", "\"/NUL\"", "\"/aux .txt\"", "\"/Prn\"", "\"/ok/.hidden.\"", "\"/a/ \"",
	};
	for (const char* b : bad) {
		warned = 0;
		auto r = subdirsOf(b, &warned);
		CHECK(r.empty() && warned == 1);
		if (!r.empty() || warned != 1) std::fprintf(stderr, "  subdirectory accepted or silent: %s\n", b);
	}
	// Overlong: 241 bytes in total, one 300-byte segment, and a path of many short segments over 240.
	CHECK(subdirsOf("\"/" + std::string(240, 'a') + "\"").empty());
	CHECK(subdirsOf("\"/" + std::string(300, 'a') + "\"").empty());
	CHECK(subdirsOf("\"" + repeat("/abcdefgh", 30) + "\"").empty());  // 270 bytes
	CHECK(subdirsOf("\"" + repeat("/abcdefgh", 26) + "\"").size() == 1); // 234 bytes
	// An item over the 512-byte list limit is dropped, never clipped into a shorter valid-looking path.
	warned = 0;
	CHECK(subdirsOf("\"/ok\",\"/" + std::string(100, 'a') + "/" + std::string(600, 'b') + "\"", &warned).size() == 1 && warned >= 1);
	CHECK(subdirsOf("\"/" + std::string(511, 'a') + "\"").empty());
	// More than kMaxSubdirectories: the extra ones are ignored with a warning (the real maximum is 79).
	std::string many;
	for (int i = 0; i < 300; i++) many += std::string(i ? "," : "") + "\"/d" + std::to_string(i) + "\"";
	warned = 0;
	CHECK(subdirsOf(many, &warned).size() == kMaxSubdirectories && warned >= 1);
	// Reserved names are only reserved as a whole segment base.
	CHECK(subdirsOf("\"/console\",\"/com0\",\"/a/nul2\",\"/xcon.txt\"").size() == 4);
}

static BuildResult withAssets(const std::string& assets) {
	return build(list({appJson("u", ",\"assets\":" + assets)}));
}

static std::string urlAsset(const std::string& url) {
	return "{\"archive\":{\"url\":\"" + url + "\",\"hash\":\"0123456789abcdef0123456789abcdef\"},"
	       "\"binary\":{\"url\":\"" + url + "\"},\"meta\":{\"url\":\"" + url + "\"},\"icon\":{\"url\":\"" + url + "\"}}";
}

static void testAssetUrls() {
	const char* good = "https://hbb1.oscwii.org/api/contents/u/u.zip";
	const char* rebuilt = "http://hbb1.oscwii.org/api/contents/u/u.zip";
	BuildResult r = withAssets(urlAsset(good));
	CHECK(r.ok() && r.catalog.apps[0].archiveUrl == rebuilt && r.stats.warningCount == 0);
	CHECK(r.catalog.apps[0].metaUrl == good); // binary and meta stay as parsed
	r = withAssets(urlAsset("HTTP://HBB1.OSCWII.ORG/api/contents/u/U.ZIP"));
	CHECK(r.ok() && r.catalog.apps[0].archiveUrl == "http://hbb1.oscwii.org/api/contents/u/U.ZIP");
	r = withAssets(urlAsset("http://hbb1.oscwii.org:80/api/contents/u/u.zip"));
	CHECK(r.ok() && r.catalog.apps[0].archiveUrl == rebuilt);

	// Not http(s) at all: archive, binary and meta are all blanked.
	const char* bad[] = {"file:///etc/passwd", "ftp://hbb1.oscwii.org/x.zip", "javascript:alert(1)", "hbb1.oscwii.org/x.zip",
	                     "/api/contents/x.zip", "//hbb1.oscwii.org/x.zip", "http://", "https://", "data:text/html,x",
	                     "http://a b/x", "https:/x", " http://x/y"};
	for (const char* u : bad) {
		r = withAssets(urlAsset(u));
		CHECK(r.ok());
		const AppEntry& a = r.catalog.apps[0];
		CHECK(a.archiveUrl.empty() && a.binaryUrl.empty() && a.metaUrl.empty() && !a.installable());
		CHECK(r.stats.warningCount == 3); // archive, binary and meta; the icon URL is rebuilt from the slug, never copied
		CHECK(a.iconUrl == iconUrl("u"));
		if (a.installable() || r.stats.warningCount != 3) std::fprintf(stderr, "  url accepted or silent: %s\n", u);
	}
	// The MD5 survives a bad URL, but the app is still not installable.
	r = withAssets(urlAsset("ftp://x/y"));
	CHECK(r.catalog.apps[0].archiveMd5.size() == 32);

	// Crafted archive URLs (the slug here is "u"): each is blanked with a warning; binary and meta (valid http) stay.
	std::string long129 = std::string(125, 'a') + ".zip";
	const char* evil[] = {"https://example.org/api/contents/u/u.zip", "https://hbb1.oscwii.org.evil/api/contents/u/u.zip",
	                      "https://hbb1.oscwii.org@evil.org/api/contents/u/u.zip", "https://evil.org@hbb1.oscwii.org/api/contents/u/u.zip",
	                      "https://hbb1.oscwii.org:8080/api/contents/u/u.zip", "http://hbb1.oscwii.org:443/api/contents/u/u.zip",
	                      "https://hbb1.oscwii.org/api/contents/u/u.zip?x", "https://hbb1.oscwii.org/api/contents/u/u.zip#f",
	                      "https://hbb1.oscwii.org/api/contents/u/%2e%2e.zip", "https://hbb1.oscwii.org/api/contents/%2e%2e/u.zip",
	                      "https://hbb1.oscwii.org/api/contents/u/../u.zip", "https://hbb1.oscwii.org/api/contents/other/u.zip",
	                      "https://hbb1.oscwii.org/api/contents/u/u.exe", "https://hbb1.oscwii.org/api/contents/u/CON.zip",
	                      "https://hbb1.oscwii.org/api/contents/u/.u.zip", "https://hbb1.oscwii.org/api/contents/u/u/u.zip",
	                      "https://hbb1.oscwii.org/api/contents/u/.zip", "https://hbb1.oscwii.org/unzipped_apps/u/u.zip",
	                      "https://hbb1.oscwii.org/api/contents/u/", "https://hbb1.oscwii.org",
	                      "ftp://hbb1.oscwii.org/api/contents/u/u.zip"};
	for (const char* u : evil) {
		r = withAssets("{\"archive\":{\"url\":\"" + std::string(u) + "\",\"hash\":\"0123456789abcdef0123456789abcdef\"}}");
		CHECK(r.ok() && r.catalog.apps[0].archiveUrl.empty() && r.stats.warningCount == 1);
		if (!r.catalog.apps[0].archiveUrl.empty() || r.stats.warningCount != 1) std::fprintf(stderr, "  url accepted or silent: %s\n", u);
	}
	r = withAssets("{\"archive\":{\"url\":\"https://hbb1.oscwii.org/api/contents/u/" + long129 + "\"}}");
	CHECK(r.ok() && r.catalog.apps[0].archiveUrl.empty() && r.stats.warningCount == 1); // 129-byte file name
	r = withAssets("{\"archive\":{\"url\":\"https://hbb1.oscwii.org/api/contents/u/" + long129.substr(1) + "\"}}");
	CHECK(r.ok() && !r.catalog.apps[0].archiveUrl.empty() && r.stats.warningCount == 0); // 128 bytes is fine

	// rebuildArchiveUrl directly.
	CHECK(rebuildArchiveUrl("u", good) == rebuilt);
	CHECK(rebuildArchiveUrl("u", "http://hbb1.oscwii.org/api/contents/u/a-b_c.1.ZIP") ==
	      "http://hbb1.oscwii.org/api/contents/u/a-b_c.1.ZIP");
	CHECK(rebuildArchiveUrl("bad/slug", good).empty() && rebuildArchiveUrl("", good).empty());
	CHECK(rebuildArchiveUrl("u", "https://hbb1.oscwii.org/api/contents/u/a b.zip").empty());

	// More host, syntax and file-name cases (slug "u"). Each is rejected.
	const char* reject[] = {
	    "http://hbb1.oscwii.org./api/contents/u/u.zip", "http://hbb1.oscwii.org:/api/contents/u/u.zip",
	    "http://hbb1.oscwii.org:080/api/contents/u/u.zip", "https://hbb1.oscwii.org:/api/contents/u/u.zip",
	    "http://hbb1.oscwii.org\\@evil/api/contents/u/u.zip", "http://hbb1.oscwii.org/api/contents/u\\u.zip",
	    "http://hbb1.oscwii.org/api\\contents/u/u.zip", "http://hbb1.oscwii.org/api/contents/u//u.zip",
	    "http://hbb1.oscwii.org/api/contents//u/u.zip", "http://hbb1.oscwii.org/api/contents/u/u.zip/",
	    "http://hbb1.oscwii.org/api/contents/u/u%00.zip", "http://hbb1.oscwii.org/api/contents/u/u.zip%00",
	    "http://hbb1.oscwii.org/api/contents/u/u.zip\n", "http://hbb1.oscwii.org/api/contents/u/u.zip ",
	    "http://hbb1.oscwii.org/api/contents/u/com1.zip", "http://hbb1.oscwii.org/api/contents/u/NUL.tar.zip",
	    "http://hbb1.oscwii.org/api/contents/u/COM9.zip", "http://hbb1.oscwii.org/api/contents/u/LPT1.zip",
	    "http://hbb1.oscwii.org/api/contents/u/lpt9.ZIP", "http://hbb1.oscwii.org/api/contents/u/u.zip.exe",
	    "http://hbb1.oscwii.org/api/contents/u/zip", "http://hbb1.oscwii.org/api/contents/u/a.zi"};
	for (const char* u : reject) {
		CHECK(rebuildArchiveUrl("u", u).empty());
		if (!rebuildArchiveUrl("u", u).empty()) std::fprintf(stderr, "  accepted: %s\n", u);
	}
	// A fullwidth "h" (U+FF48) or a Cyrillic letter in the host is not the host.
	CHECK(rebuildArchiveUrl("u", "http://\xEF\xBD\x88" "bb1.oscwii.org/api/contents/u/u.zip").empty());
	CHECK(rebuildArchiveUrl("u", "http://hbb1.osc\xD1\x80" "ii.org/api/contents/u/u.zip").empty());
	// https with its default port is fine, and is rebuilt as plain http.
	CHECK(rebuildArchiveUrl("u", "https://hbb1.oscwii.org:443/api/contents/u/u.zip") == rebuilt);
	CHECK(rebuildArchiveUrl("u", "http://hbb1.oscwii.org:80/api/contents/u/u.zip") == rebuilt);
	// Odd but valid file names.
	for (const char* f : {"u..zip", "u.zip.zip", "-.zip", "x.ZiP", "_.zip", "1.zip", "com10.zip", "con1.zip"}) {
		std::string want = std::string("http://hbb1.oscwii.org/api/contents/u/") + f;
		CHECK(rebuildArchiveUrl("u", want) == want);
		CHECK(isAllowedDownloadUrl(want));
	}

	// isAllowedDownloadUrl accepts only rebuilt URLs.
	CHECK(isAllowedDownloadUrl(rebuilt));
	CHECK(!isAllowedDownloadUrl(good) && !isAllowedDownloadUrl("http://example.org/api/contents/u/u.zip"));
	CHECK(!isAllowedDownloadUrl("http://hbb1.oscwii.org/api/contents/u/u.zip/extra"));
	CHECK(!isAllowedDownloadUrl("http://hbb1.oscwii.org/api/contents/u/v/u.zip"));
	CHECK(!isAllowedDownloadUrl("http://hbb1.oscwii.org/api/contents/u/u.exe") && !isAllowedDownloadUrl("") &&
	      !isAllowedDownloadUrl("http://hbb1.oscwii.org/api/contents/u/") && !isAllowedDownloadUrl("http://hbb1.oscwii.org/api/contents//u.zip"));
	CHECK(!isAllowedDownloadUrl("http://hbb1.oscwii.org/api/contents/u/u.zip?x") && !isAllowedDownloadUrl("http://hbb1.oscwii.org/api/contents/u/CON.zip"));

	CHECK(!isAllowedDownloadUrl("HTTP://hbb1.oscwii.org/api/contents/u/u.zip"));
	CHECK(!isAllowedDownloadUrl("http://hbb1.oscwii.org/api/contents/u/u.zip\n"));
	CHECK(!isAllowedDownloadUrl("http://hbb1.oscwii.org/api/contents/u/u.zip/x") && !isAllowedDownloadUrl("http://hbb1.oscwii.org/api/contents/u"));
	CHECK(isAllowedDownloadUrl(rebuilt) && !isAllowedDownloadUrl("https://hbb1.oscwii.org/api/contents/u/u.zip"));
	CHECK(isAllowedDownloadUrl("http://hbb1.oscwii.org/api/contents/u/" + long129.substr(1)));  // 128-byte file name
	CHECK(!isAllowedDownloadUrl("http://hbb1.oscwii.org/api/contents/u/" + long129));           // 129 bytes

	// Exactly the limit is kept whole; one more byte is dropped, never clipped to a different URL (binary/meta).
	std::string base = "https://h/";
	std::string atMax = base + std::string(kMaxShortText - base.size(), 'a');
	r = withAssets(urlAsset(atMax));
	CHECK(r.ok() && r.catalog.apps[0].metaUrl == atMax && r.catalog.apps[0].archiveUrl.empty() && r.stats.warningCount == 1);
	r = withAssets(urlAsset(atMax + "a"));
	CHECK(r.ok() && r.catalog.apps[0].archiveUrl.empty() && r.catalog.apps[0].metaUrl.empty() && r.stats.warningCount == 3);
	CHECK(!r.catalog.apps[0].installable());

	// An over-long slug is invalid, not clipped to 128 characters.
	r = build(list({appJson(std::string(kMaxSlug, 'a')), "{\"slug\":\"" + std::string(kMaxSlug + 1, 'a') + "\",\"name\":\"n\"}"}));
	CHECK(r.ok() && r.catalog.apps.size() == 1 && r.catalog.apps[0].slug.size() == kMaxSlug);
	// No md5: not installable, whatever the URL.
	r = withAssets("{\"archive\":{\"url\":\"https://hbb1.oscwii.org/api/contents/u/y.zip\"}}");
	CHECK(r.ok() && !r.catalog.apps[0].installable() && r.catalog.apps[0].archiveUrl == "http://hbb1.oscwii.org/api/contents/u/y.zip");
	CHECK(r.catalog.apps[0].installRefusal() == InstallRefusal::BadMd5);
}

static void testDates() {
	auto date = [](const char* literal) {
		BuildResult r = build(list({appJson("d", std::string(",\"release_date\":") + literal)}));
		CHECK(r.ok());
		return r.ok() ? r.catalog.apps[0].releaseDate : -1;
	};
	CHECK(date("1644364800") == 1644364800);
	CHECK(date("4102444800") == 4102444800LL);          // 2100-01-01 itself is still a date
	CHECK(date("4102444801") == 0);
	CHECK(date("1644364800000") == 0);                  // milliseconds look like year 54000
	CHECK(date("9223372036854775807") == 0 && date("1e18") == 0);
	CHECK(date("-1") == 0 && date("0") == 0 && date("\"1644364800\"") == 0);
	// A bogus date must not win "newest" for the featured fallback.
	BuildResult r = build(list({appJson("old", ",\"release_date\":1000"), appJson("ms", ",\"release_date\":1644364800000")}));
	CHECK(r.ok() && r.catalog.featured()->slug == "old");
	// Sizes still clamp at 2^40.
	r = build(list({appJson("big", ",\"uncompressed_size\":99999999999999")}));
	CHECK(r.ok() && r.catalog.apps[0].uncompressedSize == (UINT64_C(1) << 40));
}

static void testWarnings() {
	// A hostile body of bad apps: the log gets kMaxWarnings lines plus one summary, not one line per app.
	std::string bad = "[";
	for (int i = 0; i < 2000; i++) bad += "{\"slug\":\"../" + std::to_string(i) + "\",\"name\":\"n\"},";
	bad += appJson("ok") + "]";
	BuildResult r;
	std::string log = captureStderr([&] { r = build(bad); });
	size_t badSlugLines = 0, pos = 0;
	while ((pos = log.find("bad slug", pos)) != std::string::npos) { badSlugLines++; pos++; }
	CHECK(r.ok() && r.stats.warningCount >= 2000 && r.stats.warnings.size() == kMaxWarnings);
	CHECK(badSlugLines == kMaxWarnings);
	CHECK(log.find("1950 more warnings suppressed") != std::string::npos);
	CHECK(std::count(log.begin(), log.end(), '\n') < static_cast<long>(kMaxWarnings) + 10);
	// Few warnings: no summary line.
	log = captureStderr([&] { r = build(list({appJson("a"), appJson("../b")})); });
	CHECK(r.ok() && log.find("suppressed") == std::string::npos && log.find("bad slug") != std::string::npos);

	// A warning never cuts a UTF-8 character in half: this slug is 91 bytes, and the 60-byte cut lands mid-character.
	std::string euro = "a" + repeat("\xE2\x82\xAC", 30);
	r = build(list({appJson("ok"), "{\"slug\":\"" + euro + "\",\"name\":\"n\"}"}));
	CHECK(r.ok() && r.stats.warnings.size() == 1);
	if (!r.stats.warnings.empty()) {
		CHECK(validUtf8(r.stats.warnings[0]) && r.stats.warnings[0].find("\xE2\x82\xAC") != std::string::npos);
		CHECK(r.stats.warnings[0].size() < 120);
	}
	// Control characters in untrusted text never reach the log or the warning list.
	r = build(list({appJson("ok"), "{\"slug\":\"a\\nb\\u001b[31m\",\"name\":\"n\"}"}));
	CHECK(r.ok() && r.stats.warnings.size() == 1 && r.stats.warnings[0].find('\n') == std::string::npos &&
	      r.stats.warnings[0].find('\x1b') == std::string::npos);
	// jansson's own message echoes bytes of the body ("invalid token near ...").
	log = captureStderr([&] { r = build(std::string("[\x01\x1b\x07]")); });
	CHECK(!r.ok() && r.error.code == ApiErrorCode::Syntax && !log.empty());
	for (char ch : log) CHECK(ch == '\n' || (static_cast<unsigned char>(ch) >= 0x20 && ch != 0x7F));
}

static void testNames() {
	// A name made only of Unicode spaces and zero-width characters shows nothing: the app is skipped.
	const char* invisible[] = {"\\u00a0", "\\u2000\\u200a\\u200b", "\\u202f\\u205f", "\\u3000", "\\ufeff"};
	for (int i = 0; i < 5; i++) {
		BuildResult r = build(list({appJson("ok"), std::string("{\"slug\":\"v\",\"name\":\"") + invisible[i] + "\"}"}));
		CHECK(r.ok() && r.catalog.apps.size() == 1 && r.stats.appsSkipped == 1);
		if (!r.ok() || r.catalog.apps.size() != 1) std::fprintf(stderr, "  invisible name kept: %s\n", invisible[i]);
	}
	// ... but one visible character anywhere keeps it, including non-ASCII ones.
	for (const char* n : {"\\u00a0 \\u3000\\ufeff x", "\\u00a0x", "\\u00e9", "\\u3042", "\\ud83c\\udfae", "\\u3000\\u3000a"}) {
		BuildResult r = build(list({std::string("{\"slug\":\"v\",\"name\":\"") + n + "\"}"}));
		CHECK(r.ok() && r.catalog.apps.size() == 1);
	}
}

// ---- Fuzz ----

// Whatever the bytes are, parsing must finish, stay bounded and hand back only sane data.
static void checkSane(const BuildResult& r) {
	if (!r.ok()) { CHECK(!r.error.message.empty() && r.catalog.apps.empty()); return; }
	const Catalog& c = r.catalog;
	CHECK(!c.apps.empty() && c.apps.size() <= kMaxApps && c.categories.size() <= kMaxCategories);
	CHECK(r.stats.warnings.size() <= kMaxWarnings);
	for (const auto& a : c.apps) {
		CHECK(isValidSlug(a.slug) && a.slug.size() <= kMaxSlug && c.find(a.slug) != nullptr);
		CHECK(a.name.size() <= kMaxShortText && a.longDesc.size() <= kMaxLongText && a.shortDesc.size() <= 2048);
		CHECK(a.flags.size() <= kMaxListItems && a.subdirectories.size() <= kMaxSubdirectories);
		CHECK(a.archiveMd5.empty() || a.archiveMd5.size() == 32);
		for (char ch : a.name + a.author + a.shortDesc + a.version + a.archiveUrl) CHECK(static_cast<unsigned char>(ch) >= 0x20 && ch != 0x7F);
	}
	CHECK(c.featured() != nullptr);
}

static std::string fuzzSeed; // a valid small contents body

static void fuzzOne(const std::string& body) {
	checkSane(buildCatalog(std::nullopt, body, std::nullopt));
	// The same mutated bytes as the information and featured documents, next to a valid contents body so those two
	// really go through buildCatalog (a bad contents body would return before looking at them).
	BuildResult r = buildCatalog(body, fuzzSeed, body);
	CHECK(r.ok());
	checkSane(r);
}

static std::string mutate(std::string w, std::mt19937& rng) {
	static const char* const snippets[] = {"null", "[", "]", "{", "}", "\"", "\\", "-1", "1e999", "99999999999999999999",
	                                       "\"\"", ",", ":", "\\u0000", "\\ud800", "\xFF", "true", "\\n"};
	int edits = 1 + rng() % 6;
	for (int e = 0; e < edits && !w.empty(); e++) {
		size_t at = rng() % w.size();
		switch (rng() % 5) {
		case 0: w[at] = static_cast<char>(rng()); break;
		case 1: w.insert(at, snippets[rng() % (sizeof snippets / sizeof snippets[0])]); break;
		case 2: w.erase(at, 1 + rng() % 8); break;
		case 3: { size_t len = std::min<size_t>(w.size() - at, 1 + rng() % 40); w.insert(rng() % w.size(), w.substr(at, len)); break; }
		default: w[at] = "[]{}\",:"[rng() % 7]; break;
		}
	}
	if (rng() % 6 == 0 && !w.empty()) w.resize(rng() % w.size());
	return w;
}

static void testFuzz() {
	// A small seed: the first few real apps, re-serialised, so thousands of iterations stay fast under the sanitizers.
	json_t* all = json_loadb(contents.data(), contents.size(), 0, nullptr);
	CHECK(all != nullptr);
	if (!all) return;
	json_t* few = json_array();
	for (size_t i = 0; i < 6 && i < json_array_size(all); i++) json_array_append(few, json_array_get(all, i));
	char* dumped = json_dumps(few, 0);
	std::string seed = dumped;
	std::free(dumped);
	json_decref(few);
	json_decref(all);
	CHECK(build(seed).ok());
	fuzzSeed = seed;

	// The parser logs every skipped app and every build; thousands of fuzz iterations would bury the test output.
	// CHECK writes to g_errFd, a saved copy of the real stderr, so failures stay visible during the silent run.
	std::fflush(stderr);
	int savedErr = dup(2);
	int devNull = open("/dev/null", O_WRONLY);
	if (savedErr >= 0 && devNull >= 0) dup2(devNull, 2);
	if (devNull >= 0) close(devNull);

	std::mt19937 rng(20241125);
	for (int i = 0; i < 3000; i++) fuzzOne(mutate(seed, rng));
	// Random bytes and random truncations.
	for (int i = 0; i < 300; i++) {
		std::string w(rng() % 400, 0);
		for (char& ch : w) ch = static_cast<char>(rng());
		fuzzOne(w);
	}
	for (int i = 0; i < 200; i++) fuzzOne(seed.substr(0, rng() % seed.size()));
	// A few mutations of the whole real body, for deep and wide structure.
	for (int i = 0; i < 12; i++) checkSane(buildCatalog(info, mutate(contents, rng), featured));
	for (int i = 0; i < 200; i++) {
		Catalog c;
		parseInformation(mutate(info, rng), c);
		CHECK(c.categories.size() <= kMaxCategories);
		std::string s;
		parseFeatured(mutate(featured, rng), s);
		CHECK(s.empty() || isValidSlug(s));
	}
	if (savedErr >= 0) { std::fflush(stderr); dup2(savedErr, 2); close(savedErr); }
}

int main() {
	int dupErr = dup(2);
	if (dupErr >= 0) g_errFd = dupErr;
	info = readFile("tests/fixtures/information.json");
	contents = readFile("tests/fixtures/contents.json");
	featured = readFile("tests/fixtures/featured.json");
	testRealFixtures();
	testFallbacks();
	testJunk();
	testAppRecovery();
	testBounds();
	testUrls();
	testNodeLimit();
	testDepthScan();
	testSubdirs();
	testAssetUrls();
	testDates();
	testWarnings();
	testNames();
	testFuzz();
	if (failures) { std::fprintf(stderr, "oscapi_test: %d failure(s)\n", failures); return 1; }
	std::puts("oscapi_test: all passed");
	return 0;
}
