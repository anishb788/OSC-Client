// Host-side tests for pure catalog logic. Build and run: make -f Makefile.pc test
#include <cstdio>
#include <cstdlib>

#include "catalog/Catalog.h"

static int failures = 0;
#define CHECK(cond) \
	do { if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static AppEntry app(const char* slug, const char* name, const char* author, const char* shortD, const char* longD,
                    uint32_t downloads, int64_t date, const char* cat = "games") {
	AppEntry a;
	a.slug = slug; a.name = name; a.author = author; a.shortDesc = shortD; a.longDesc = longD;
	a.downloads = downloads; a.releaseDate = date; a.category = cat;
	return a;
}

static std::vector<std::string> slugs(const std::vector<const AppEntry*>& v) {
	std::vector<std::string> out;
	for (auto* a : v) out.push_back(a->slug);
	return out;
}

static Catalog sample() {
	Catalog c;
	c.apps = {
		app("usbloadergx", "USB Loader GX", "Cyan", "Launch games from USB", "A loader.", 500, 100),
		app("wiiflow", "WiiFlow Lite", "Fix94", "Game launcher", "Supports usb and sd devices.", 900, 300),
		app("snes9x", "Snes9x GX", "Tantric", "SNES emulator", "Emulates the Super Nintendo.", 400, 200, "emulators"),
		app("nes", "Nintendo Emulator", "X", "An emulator", "Plays nes games", 100, 150, "emulators"),
		app("abc", "Zebra", "Y", "Stripes", "Not related.", 50, 50, "demos"),
		app("tmx", "Text Menu Xtra", "Z", "Menu", "Odd.", 10, 20, "media"),
	};
	c.finalize();
	return c;
}

static void testSanitize() {
	CHECK(sanitizeText("a\r\nb", false) == "a\nb");
	CHECK(sanitizeText("a\rb", false) == "ab");
	CHECK(sanitizeText("a\tb", false) == "a b");
	CHECK(sanitizeText("a\x01\x1f\x7f" "b", false) == "ab");
	CHECK(sanitizeText("line1\nline2", false) == "line1\nline2");
	CHECK(sanitizeText("line1\r\nline2\n", true) == "line1 line2 ");
	CHECK(sanitizeText("caf\xc3\xa9 \xe2\x80\xa6", true) == "caf\xc3\xa9 \xe2\x80\xa6"); // UTF-8 untouched

	Catalog c;
	c.apps = {app("a", "Na\tme\r\n2", "Au\nthor", "Sh\r\nort", "Lo\r\nng\r\ntext", 1, 1)};
	c.finalize();
	CHECK(c.apps[0].name == "Na me 2");
	CHECK(c.apps[0].author == "Au thor");
	CHECK(c.apps[0].shortDesc == "Sh ort");
	CHECK(c.apps[0].longDesc == "Lo\nng\ntext");
	CHECK(c.apps[0].nameL == "na me 2");
}

static void testSlug() {
	CHECK(isValidSlug("usbloadergx"));
	CHECK(isValidSlug("smw-wiigii"));
	CHECK(isValidSlug("Space_Cadet.v2"));
	CHECK(!isValidSlug(""));
	CHECK(!isValidSlug("."));
	CHECK(!isValidSlug(".."));
	CHECK(!isValidSlug(".hidden"));
	CHECK(!isValidSlug("a/b"));
	CHECK(!isValidSlug("a\\b"));
	CHECK(!isValidSlug("a b"));
	CHECK(!isValidSlug("a\n"));
	CHECK(!isValidSlug("caf\xc3\xa9"));
	// Trailing dot (FAT and Windows drop it), device names, and the length limit.
	CHECK(!isValidSlug("a.") && !isValidSlug("a..") && isValidSlug("a.b"));
	for (const char* dev : {"con", "CON", "Prn", "aux", "NUL", "com1", "COM9", "lpt1", "LPT9", "con.txt", "Nul.tar.gz"})
		CHECK(!isValidSlug(dev));
	CHECK(isValidSlug("console") && isValidSlug("com0") && isValidSlug("com10") && isValidSlug("lpt") && isValidSlug("xcon"));
	CHECK(isValidSlug(std::string(kMaxSlugLength, 'a')) && !isValidSlug(std::string(kMaxSlugLength + 1, 'a')));
	CHECK(isReservedDeviceName("com1 .txt") && !isReservedDeviceName("com1x"));
}

static AppEntry installableApp() {
	AppEntry a;
	a.slug = "x";
	a.archiveUrl = "http://hbb1.oscwii.org/api/contents/x/x.zip";
	a.archiveMd5 = "0123456789abcdef0123456789ABCDEF";
	a.packageType = PackageType::Dol;
	a.archiveSize = 1000;
	a.uncompressedSize = 2000;
	a.subdirectories = {"/apps/x"};
	return a;
}

static void testInstallable() {
	AppEntry a;
	CHECK(!a.installable() && a.installRefusal() == InstallRefusal::NoArchiveUrl);
	a = installableApp();
	CHECK(a.installable() && a.installRefusal() == InstallRefusal::None);
	a.uncompressedSize = 500; // smaller than the archive: allowed
	CHECK(a.installable());

	// URLs: only the rebuilt OSC form.
	for (const char* u : {"", "https://hbb1.oscwii.org/api/contents/x/x.zip", "http://example.org/a.zip", "ftp://example.org/a.zip",
	                      "file:///etc/passwd", "example.org/a.zip", "http://", "http://hbb1.oscwii.org/api/contents/x/x.zip/y",
	                      "http://hbb1.oscwii.org/api/contents/x/x.exe", "HTTP://hbb1.oscwii.org/api/contents/x/x.zip"}) {
		a = installableApp();
		a.archiveUrl = u;
		CHECK(a.installRefusal() == InstallRefusal::NoArchiveUrl);
	}

	auto refusal = [](auto mutate) {
		AppEntry e = installableApp();
		mutate(e);
		return e.installRefusal();
	};
	for (const char* m : {"", "0123456789abcdef0123456789abcde", "0123456789abcdef0123456789abcdeg", "0123456789abcdef0123456789abcdef0"})
		CHECK(refusal([&](AppEntry& e) { e.archiveMd5 = m; }) == InstallRefusal::BadMd5);
	CHECK(refusal([](AppEntry& e) { e.packageType = PackageType::Unknown; }) == InstallRefusal::UnknownPackage);
	CHECK(refusal([](AppEntry& e) { e.packageType = PackageType::Elf; }) == InstallRefusal::None);
	CHECK(refusal([](AppEntry& e) { e.archiveSize = 0; }) == InstallRefusal::BadArchiveSize);
	CHECK(refusal([](AppEntry& e) { e.archiveSize = UINT64_C(4) << 30; }) == InstallRefusal::BadArchiveSize);
	CHECK(refusal([](AppEntry& e) { e.archiveSize = kFat32MaxFile; }) == InstallRefusal::ArchiveTooLarge);
	CHECK(refusal([](AppEntry& e) { e.archiveSize = kMaxArchiveBytes + 1; }) == InstallRefusal::ArchiveTooLarge);
	CHECK(refusal([](AppEntry& e) { e.archiveSize = kMaxArchiveBytes; }) == InstallRefusal::None);
	CHECK(refusal([](AppEntry& e) { e.uncompressedSize = 0; }) == InstallRefusal::BadInstallSize);
	CHECK(refusal([](AppEntry& e) { e.uncompressedSize = kMaxInstallBytes + 1; }) == InstallRefusal::InstallTooLarge);
	CHECK(refusal([](AppEntry& e) { e.uncompressedSize = kMaxInstallBytes; }) == InstallRefusal::None);
	// The URL must carry this entry's own slug.
	CHECK(refusal([](AppEntry& e) { e.slug = "a"; e.archiveUrl = "http://hbb1.oscwii.org/api/contents/b/b.zip"; }) ==
	      InstallRefusal::NoArchiveUrl);
	CHECK(refusal([](AppEntry& e) { e.slug = "a"; e.archiveUrl = "http://hbb1.oscwii.org/api/contents/a/a.zip"; }) == InstallRefusal::None);
	CHECK(refusal([](AppEntry& e) { e.slug = "xy"; }) == InstallRefusal::NoArchiveUrl); // URL is for "x"
	CHECK(refusal([](AppEntry& e) { e.subdirectories.clear(); }) == InstallRefusal::NoAppDir);
	CHECK(refusal([](AppEntry& e) { e.subdirectories = {"/private/wii", "/apps", "/apps/"}; }) == InstallRefusal::NoAppDir);
	CHECK(refusal([](AppEntry& e) { e.subdirectories = {"/private/wii", "/apps/y"}; }) == InstallRefusal::None);
}

static void testSearch() {
	Catalog c = sample();
	CHECK(c.search("").empty());
	CHECK(c.search("zzzz").empty());

	// Name prefix beats a description-only hit: "usb" is in WiiFlow's long description.
	auto r = slugs(c.search("usb"));
	CHECK(r.size() == 2 && r[0] == "usbloadergx" && r[1] == "wiiflow");

	// Case-insensitive, and every word must match (multi-word scoring).
	r = slugs(c.search("USB GX"));
	CHECK(r.size() == 1 && r[0] == "usbloadergx");
	r = slugs(c.search("snes emulator"));
	CHECK(!r.empty() && r[0] == "snes9x");
	CHECK(c.search("snes zebra").empty());

	// Fuzzy subsequence for words of 4+ letters ("usbg" in "usb loader gx"), not for 3 letters.
	r = slugs(c.search("usbg"));
	CHECK(r.size() == 1 && r[0] == "usbloadergx");
	CHECK(c.search("tmx").size() == 1);          // slug substring still matches
	CHECK(c.search("txt").empty());              // 3 letters: no fuzzy match on "Text Menu Xtra"
	CHECK(!c.search("txtm").empty());            // 4 letters: fuzzy match

	// Fuzzy hits rank below real substring hits (desc hit for "game" vs fuzzy name hit).
	Catalog f;
	f.apps = {app("fz", "Great Amazing Marvel Epic", "A", "x", "y", 9999, 1),
	          app("real", "Other", "B", "A game launcher", "z", 1, 1)};
	f.finalize();
	r = slugs(f.search("game"));
	CHECK(r.size() == 2 && r[0] == "real" && r[1] == "fz");

	// Ties are broken by downloads.
	Catalog t;
	t.apps = {app("a", "Foo One", "A", "", "", 1, 1), app("b", "Foo Two", "B", "", "", 5, 1)};
	t.finalize();
	r = slugs(t.search("foo"));
	CHECK(r.size() == 2 && r[0] == "b");
}

static void testSort() {
	Catalog c = sample();
	auto all = c.inCategory("");
	CHECK(all.size() == 6);
	Catalog::sort(all, SortMode::Name);
	CHECK(all.front()->slug == "usbloadergx" || all.front()->name == "Nintendo Emulator");
	CHECK(all.front()->name == "Nintendo Emulator" && all.back()->name == "Zebra");
	Catalog::sort(all, SortMode::Popular);
	CHECK(all.front()->slug == "wiiflow");
	Catalog::sort(all, SortMode::Newest);
	CHECK(all.front()->slug == "wiiflow" && all.back()->slug == "tmx");
	auto before = slugs(all);
	Catalog::sort(all, SortMode::Relevance);
	CHECK(slugs(all) == before); // no-op by design
}

static void testFeaturedAndIndex() {
	Catalog c = sample();
	CHECK(c.featured() && c.featured()->slug == "wiiflow"); // no featuredSlug: newest releaseDate, not front()
	c.featuredSlug = "snes9x";
	c.finalize();
	CHECK(c.featured() && c.featured()->slug == "snes9x");
	c.featuredSlug = "does-not-exist";
	c.finalize();
	CHECK(c.featured() && c.featured()->slug == "wiiflow");

	Catalog empty;
	empty.finalize();
	CHECK(empty.featured() == nullptr);
	CHECK(empty.count("") == 0);

	CHECK(c.find("nes") && c.find("nes")->name == "Nintendo Emulator");
	CHECK(c.find("nope") == nullptr);
	CHECK(c.count("") == 6 && c.count("games") == 2 && c.count("emulators") == 2 && c.count("nope") == 0);

	Catalog copy = c; // indices, not pointers: copies stay valid
	CHECK(copy.find("nes") == &copy.apps[3]);
	CHECK(copy.featured() == &copy.apps[1]);

	Catalog mock;
	loadMockCatalog(mock);
	CHECK(mock.apps.size() > 30);
	CHECK(mock.featured() != nullptr && mock.featured()->slug == mock.featuredSlug);
	CHECK(!mock.apps[0].nameL.empty() && !mock.apps[0].iconUrl.empty());
	for (auto& a : mock.apps) {
		CHECK(isValidSlug(a.slug));
		CHECK(a.installable());
		if (!a.installable()) std::fprintf(stderr, "  mock entry not installable: %s\n", a.slug.c_str());
	}
}

int main() {
	testSanitize();
	testSlug();
	testInstallable();
	testSearch();
	testSort();
	testFeaturedAndIndex();
	if (failures) { std::fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
	std::puts("catalog_test: all passed");
	return 0;
}
