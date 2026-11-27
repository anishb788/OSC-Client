// SPDX-License-Identifier: GPL-3.0-or-later
// Host tests for install::ZipExtract (Increment 4). Every archive is built at runtime with libzip's writer into a temp dir.
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "Check.h"
#include "ZipFixtures.h"
#include "install/ZipExtract.h"

namespace fs = std::filesystem;
using namespace install;

struct TempDir {
	std::string path;
	TempDir() {
		char tmpl[] = "/tmp/openshop-zip-XXXXXX";
		path = mkdtemp(tmpl);
	}
	~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
	std::string sub(const std::string& s) const { return path + "/" + s; }
};

static bool there(const std::string& p) { std::error_code ec; return fs::symlink_status(p, ec).type() != fs::file_type::not_found; }

static std::string pattern(size_t n, unsigned seed) {
	std::string s(n, '\0');
	uint32_t x = seed * 2654435761u + 1;
	for (size_t i = 0; i < n; i++) { x = x * 1664525u + 1013904223u; s[i] = (char)(x >> 24); }
	return s;
}

static const std::string kBoot = "DOLDATA";
static ZipEntrySpec boot(const std::string& dir = "x") { return zfile("apps/" + dir + "/boot.dol", kBoot); }

static ZipOptions baseOpts(const TempDir& t) {
	ZipOptions o;
	o.packageType = PackageType::Dol;
	o.removeRoots = {"/tmp"};
	(void)t;
	return o;
}

// Builds `entries` and tries to open them. Returns the error code (None when it opened).
static ZipError openCode(const TempDir& t, const std::vector<ZipEntrySpec>& entries, ZipOptions o, ZipPlan* planOut = nullptr) {
	const std::string zp = t.sub("t.zip");
	if (!buildZip(zp, entries)) { std::printf("  fixture build failed\n"); CHECK(false); return ZipError::Open; }
	ZipExtract zx(std::move(o));
	bool ok = zx.open(zp);
	if (planOut) *planOut = zx.plan();
	if (ok) return ZipError::None;
	return zx.failure().code;
}

static void expectReject(const TempDir& t, const char* what, std::vector<ZipEntrySpec> entries, ZipError want, ZipOptions o) {
	ZipError got = openCode(t, entries, std::move(o));
	if (got != want) std::printf("  %s: got '%s', wanted '%s'\n", what, describe(got), describe(want));
	CHECK(got == want);
}

// ---- rejections at open() ---------------------------------------------------------------------------------------------

static void testNameRejects() {
	TempDir t;
	const std::vector<std::string> bad = {"../x", "/apps/x/boot.dol", "sd:/apps/x/boot.dol", "apps\\x\\boot2.dol", "apps/x/a:b",
	                                      "apps/x/CON.txt", "apps/x/a*b", "apps/x/../../y", "apps//x/y"};
	for (const std::string& n : bad) expectReject(t, n.c_str(), {boot(), zfile(n, "z")}, ZipError::BadName, baseOpts(t));
	// A valid name still passes in the same position.
	CHECK(openCode(t, {boot(), zfile("apps/x/ok.txt", "z")}, baseOpts(t)) == ZipError::None);
}

static void testCollisions() {
	TempDir t;
	expectReject(t, "A/x vs a/X", {zfile("apps/A/boot.dol", "1"), zfile("apps/a/BOOT.DOL", "2")}, ZipError::Collision, baseOpts(t));
	expectReject(t, "case-only dup", {boot(), zfile("apps/x/f", "1"), zfile("apps/x/F", "2")}, ZipError::Collision, baseOpts(t));
	expectReject(t, "file vs dir", {boot(), zfile("apps/x/data", "1"), zdir("apps/x/Data/")}, ZipError::Collision, baseOpts(t));
	expectReject(t, "file vs implied parent", {boot(), zfile("apps/x/d", "1"), zfile("apps/x/D/f", "2")}, ZipError::Collision, baseOpts(t));
	expectReject(t, "file vs deep parent", {boot(), zfile("apps/x/a/b", "1"), zfile("apps/X/A/b/c", "2")}, ZipError::Collision, baseOpts(t));
	// Non-ASCII case: e-acute vs E-acute, and Greek, Cyrillic and Latin Extended-A pairs, in names and in parent folders.
	expectReject(t, "e-acute", {boot(), zfile("apps/x/\xc3\xa9", "1"), zfile("apps/x/\xc3\x89", "2")}, ZipError::Collision, baseOpts(t));
	expectReject(t, "sigma", {boot(), zfile("apps/x/\xce\xa3", "1"), zfile("apps/x/\xcf\x83", "2")}, ZipError::Collision, baseOpts(t));
	expectReject(t, "cyrillic", {boot(), zfile("apps/x/\xd0\x96", "1"), zfile("apps/x/\xd0\xb6", "2")}, ZipError::Collision, baseOpts(t));
	expectReject(t, "latin-a", {boot(), zfile("apps/x/\xc5\x81", "1"), zfile("apps/x/\xc5\x82", "2")}, ZipError::Collision, baseOpts(t));
	expectReject(t, "e-acute dir", {boot(), zfile("apps/x/\xc3\xa9", "1"), zfile("apps/x/\xc3\x89/f", "2")}, ZipError::Collision, baseOpts(t));
	CHECK(openCode(t, {boot(), zfile("apps/x/\xc3\xa9", "1"), zfile("apps/x/\xc3\xa8", "2")}, baseOpts(t)) == ZipError::None);
	// The same directory twice is harmless.
	CHECK(openCode(t, {boot(), zdir("apps/x/d/"), zdir("apps/x/D/"), zfile("apps/x/d/f", "1")}, baseOpts(t)) == ZipError::None);
}

static void testKindRejects() {
	TempDir t;
	ZipEntrySpec link = zfile("apps/x/link", "boot.dol");
	link.symlink = true;
	expectReject(t, "symlink", {boot(), link}, ZipError::Symlink, baseOpts(t));
	ZipEntrySpec enc = zfile("apps/x/secret.bin", "hello hello hello");
	enc.encrypted = true;
	expectReject(t, "encrypted", {boot(), enc}, ZipError::Encrypted, baseOpts(t));
	ZipEntrySpec bz = zfile("apps/x/bz.bin", std::string(1000, 'a'), ZIP_CM_BZIP2);
	expectReject(t, "bzip2", {boot(), bz}, ZipError::Method, baseOpts(t));
}

static void testCaps() {
	TempDir t;
	{
		ZipOptions o = baseOpts(t);
		o.maxEntries = 3;
		CHECK(openCode(t, {boot(), zfile("apps/x/a", "1"), zfile("apps/x/b", "1")}, o) == ZipError::None); // exactly 3
		expectReject(t, "entries+1", {boot(), zfile("apps/x/a", "1"), zfile("apps/x/b", "1"), zfile("apps/x/c", "1")},
		             ZipError::TooManyEntries, o);
	}
	{
		// A zip bomb: 2 MB of zeros deflates to a few KB, but it declares 2 MB against a 1 MB cap.
		ZipOptions o = baseOpts(t);
		o.maxInstallBytes = 1u << 20;
		expectReject(t, "bomb", {boot(), zfile("apps/x/zeros", std::string(2u << 20, '\0'))}, ZipError::TooBig, o);
		// Many entries add up too.
		expectReject(t, "sum", {boot(), zfile("apps/x/a", std::string(600 * 1024, 'a')), zfile("apps/x/b", std::string(600 * 1024, 'b'))},
		             ZipError::TooBig, o);
	}
}

// The entry count and directory size are checked from the end-of-central-directory record, before libzip opens the file.
static void testPreScan() {
	TempDir t;
	const std::string zp = t.sub("p.zip");
	CHECK(buildZip(zp, {boot(), zfile("apps/x/a", "1")}));
	ZipOptions small = baseOpts(t);
	small.maxEntries = 5;
	{
		// Real count 2, patched to 6 (and a tiny cap): refused as TooManyEntries by the pre-scan.
		CHECK(patchEocd(zp, 6, 0)); // size 0 is a lie too, but the count is checked first
		ZipExtract zx(small);
		CHECK(!zx.open(zp));
		CHECK(zx.failure().code == ZipError::TooManyEntries);
	}
	{
		// Count 60000 against the default cap: libzip alone would have said Open or Corrupt for this lie.
		CHECK(buildZip(zp, {boot(), zfile("apps/x/a", "1")}));
		CHECK(patchEocd(zp, 60000, 100));
		ZipExtract zx(baseOpts(t));
		CHECK(!zx.open(zp));
		CHECK(zx.failure().code == ZipError::TooManyEntries);
	}
	{
		// A directory size bigger than the file.
		CHECK(buildZip(zp, {boot(), zfile("apps/x/a", "1")}));
		CHECK(patchEocd(zp, 2, 0x7fffffffu));
		ZipExtract zx(baseOpts(t));
		CHECK(!zx.open(zp));
		CHECK(zx.failure().code == ZipError::Corrupt);
	}
	{
		// No end-of-central-directory record at all.
		CHECK(spit(zp, std::string(100, 'x')));
		ZipExtract zx(baseOpts(t));
		CHECK(!zx.open(zp));
		CHECK(zx.failure().code == ZipError::Open);
		CHECK(spit(zp, "PK"));
		ZipExtract z2(baseOpts(t));
		CHECK(!z2.open(zp));
		CHECK(z2.failure().code == ZipError::Open);
	}
	{
		// An honest archive with exactly maxEntries entries still opens.
		CHECK(buildZip(zp, {boot(), zfile("apps/x/a", "1")}));
		ZipOptions o = baseOpts(t);
		o.maxEntries = 2;
		ZipExtract zx(o);
		CHECK(zx.open(zp));
	}
}

static void testDuplicateNames() {
	TempDir t;
	const std::string zp = t.sub("d.zip");
	CHECK(buildZip(zp, {boot(), zfile("apps/x/aa", "1"), zfile("apps/x/ab", "2")}));
	CHECK(patchEntryName(zp, "apps/x/ab", "apps/x/aa"));
	ZipExtract zx(baseOpts(t));
	CHECK(!zx.open(zp));
	CHECK(zx.failure().code == ZipError::Collision);
}

static void testLayoutRejects() {
	TempDir t;
	expectReject(t, "no boot", {zfile("apps/x/readme.txt", "hi")}, ZipError::NoBoot, baseOpts(t));
	expectReject(t, "boot too deep", {zfile("apps/x/sub/boot.dol", "hi")}, ZipError::NoBoot, baseOpts(t));
	expectReject(t, "boot at the root", {zfile("boot.dol", "hi")}, ZipError::NoBoot, baseOpts(t));
	ZipOptions elf = baseOpts(t);
	elf.packageType = PackageType::Elf;
	expectReject(t, "elf package, dol only", {boot()}, ZipError::NoBoot, elf);
	CHECK(openCode(t, {zfile("apps/x/boot.elf", "E")}, elf) == ZipError::None);
	expectReject(t, "empty boot.dol", {zfile("apps/x/boot.dol", "")}, ZipError::NoBoot, baseOpts(t));
	expectReject(t, "empty boot.elf", {zfile("apps/x/boot.elf", "")}, ZipError::NoBoot, elf);
	expectReject(t, "dol package, elf only", {zfile("apps/x/boot.elf", "E")}, ZipError::NoBoot, baseOpts(t));
	ZipOptions any = baseOpts(t);
	any.packageType = PackageType::Unknown;
	CHECK(openCode(t, {zfile("apps/x/BOOT.ELF", "E")}, any) == ZipError::None);

	expectReject(t, "OpenShop", {boot(), zfile("apps/OpenShop/boot.dol", "x")}, ZipError::Forbidden, baseOpts(t));
	expectReject(t, "openshop case", {boot(), zfile("APPS/openshop/installed/x", "x")}, ZipError::Forbidden, baseOpts(t));
	expectReject(t, "OpenShop dir", {boot(), zdir("apps/OpenShop/")}, ZipError::Forbidden, baseOpts(t));
	expectReject(t, "private", {boot(), zfile("private/x", "x")}, ZipError::Forbidden, baseOpts(t));
	expectReject(t, "Private case", {boot(), zfile("Private/wii/x", "x")}, ZipError::Forbidden, baseOpts(t));
	expectReject(t, "title dir", {boot(), zdir("title/")}, ZipError::Forbidden, baseOpts(t));

	ZipOptions own = baseOpts(t);
	own.ownsDir = [](const std::string& d) { return d != "other"; };
	expectReject(t, "ownsDir refused", {boot(), zfile("apps/Other/boot.dol", "x")}, ZipError::Forbidden, own);
	CHECK(openCode(t, {boot(), zfile("apps/Mine/boot.dol", "x")}, own) == ZipError::None);
}

// ---- skipped entries and undeclared folders --------------------------------------------------------------------------

static void runAll(ZipExtract& zx, StepResult want = StepResult::Done) {
	StepResult r = StepResult::Running;
	for (int i = 0; i < 100000 && r == StepResult::Running; i++) r = zx.step(5);
	if (r != want) std::printf("  step ended with %d, wanted %d (%s)\n", (int)r, (int)want, zx.failure().detail.c_str());
	CHECK(r == want);
}

static void testSkipped() {
	TempDir t;
	const std::string zp = t.sub("s.zip");
	CHECK(buildZip(zp, {zfile("readme.txt", "hello"), zfile("__MACOSX/apps/x/._boot.dol", "junk"), zfile("__MACOSX/._x", "j"),
	                    zdir("apps/"), boot(), zfile("apps/loose.txt", "l")}));
	ZipExtract zx(baseOpts(t));
	CHECK(zx.open(zp));
	CHECK(zx.plan().skippedMacosx == 2);
	CHECK(zx.plan().skippedRootFiles == 2); // readme.txt and apps/loose.txt
	CHECK(zx.plan().appDirs.size() == 1 && zx.plan().appDirs[0].name == "x" && zx.plan().appDirs[0].hasBoot);
	CHECK(!zx.plan().needsConfirmation());
	CHECK(zx.plan().entryCount == 6);
	CHECK(zx.plan().declaredTotal == kBoot.size());
	CHECK(zx.begin(t.sub("stage"), 100u << 20));
	runAll(zx);
	const std::string root = t.sub("stage/root");
	CHECK(there(root + "/apps/x/boot.dol"));
	CHECK(!there(root + "/readme.txt"));
	CHECK(!there(root + "/__MACOSX"));
	CHECK(!there(root + "/apps/loose.txt"));
}

static void testUndeclared() {
	TempDir t;
	const std::string zp = t.sub("u.zip");
	// The undeclared folder comes first, so a step that ignored the rule would write it before anything else.
	CHECK(buildZip(zp, {zfile("config/foo/x.ini", "ini"), boot(), zfile("WiiFlow/a.txt", "w")}));
	{
		ZipExtract zx(baseOpts(t)); // no hint at all
		CHECK(zx.open(zp));
		CHECK(zx.plan().needsConfirmation());
		CHECK(zx.plan().otherFolders.size() == 2);
		CHECK(zx.plan().otherFolders[0].name == "config" && zx.plan().otherFolders[0].needsConfirmation);
		CHECK(zx.begin(t.sub("s1"), 100u << 20));
		StepResult r = zx.step(50);
		CHECK(r == StepResult::NeedsConfirmation);
		CHECK(!there(t.sub("s1/root/config")));
		CHECK(zx.step(50) == StepResult::NeedsConfirmation); // still waiting, nothing lost
		zx.confirmUndeclared();
		runAll(zx);
		std::string got;
		CHECK(slurp(t.sub("s1/root/config/foo/x.ini"), got) && got == "ini");
		CHECK(there(t.sub("s1/root/WiiFlow/a.txt")));
	}
	{
		ZipOptions o = baseOpts(t);
		o.subdirHint = {"/Config", "/wiiflow"}; // declared, case-insensitive
		ZipExtract zx(o);
		CHECK(zx.open(zp));
		CHECK(!zx.plan().needsConfirmation());
		CHECK(zx.begin(t.sub("s2"), 100u << 20));
		runAll(zx);
		CHECK(there(t.sub("s2/root/config/foo/x.ini")));
	}
	{
		// Declined: cancel instead of confirming. The staging root goes away.
		ZipExtract zx(baseOpts(t));
		CHECK(zx.open(zp));
		CHECK(zx.begin(t.sub("s3"), 100u << 20));
		CHECK(zx.step(50) == StepResult::NeedsConfirmation);
		zx.requestCancel();
		CHECK(zx.step(50) == StepResult::Error);
		CHECK(zx.failure().code == ZipError::Cancelled);
		CHECK(!there(t.sub("s3/root")));
		CHECK(zx.rootRemoved());
	}
}

// ---- extraction ------------------------------------------------------------------------------------------------------

static void testValidExtract() {
	TempDir t;
	const std::string zp = t.sub("v.zip");
	const std::string big = pattern(300 * 1024 + 17, 1); // several 64 KB chunks
	const std::string text(100000, 'q');
	CHECK(buildZip(zp, {zdir("apps/"), zdir("apps/one/"), zfile("apps/one/boot.dol", big, ZIP_CM_STORE),
	                    zfile("apps/one/meta.xml", "<app/>", ZIP_CM_DEFLATE), zfile("apps/Two/boot.dol", pattern(70000, 2), ZIP_CM_DEFLATE),
	                    zfile("apps/Two/empty.bin", ""), zfile("apps/Two/sub/deep/t.txt", text, ZIP_CM_STORE),
	                    zfile("config/foo/x.ini", "[a]\nb=1\n"), zdir("config/empty/")}));
	ZipOptions o = baseOpts(t);
	o.subdirHint = {"/config"};
	ZipExtract zx(o);
	CHECK(zx.open(zp));
	const uint64_t total = big.size() + 6 + 70000 + 0 + text.size() + 8;
	CHECK(zx.plan().declaredTotal == total);
	CHECK(zx.plan().appDirs.size() == 2 && zx.plan().appDirs[0].name == "one" && zx.plan().appDirs[1].name == "Two");
	CHECK(zx.plan().appDirs[0].hasBoot && zx.plan().appDirs[1].hasBoot);
	CHECK(zx.plan().otherFolders.size() == 1 && !zx.plan().otherFolders[0].needsConfirmation);
	CHECK(zx.totalBytes() == total);

	CHECK(!zx.begin(t.sub("st"), total + (1u << 20) - 1)); // one byte short
	CHECK(zx.failure().code == ZipError::NoSpace && zx.failure().needed == total + (1u << 20));
	CHECK(!there(t.sub("st")));
	CHECK(zx.begin(t.sub("st"), total + (1u << 20)));    // exactly enough
	CHECK(zx.root() == t.sub("st/root"));
	runAll(zx);
	CHECK(zx.bytesWritten() == total);
	const std::string root = t.sub("st/root");
	std::string got;
	CHECK(slurp(root + "/apps/one/boot.dol", got) && got == big);
	CHECK(slurp(root + "/apps/one/meta.xml", got) && got == "<app/>");
	CHECK(slurp(root + "/apps/Two/boot.dol", got) && got == pattern(70000, 2));
	CHECK(slurp(root + "/apps/Two/empty.bin", got) && got.empty());
	CHECK(slurp(root + "/apps/Two/sub/deep/t.txt", got) && got == text);
	CHECK(slurp(root + "/config/foo/x.ini", got) && got == "[a]\nb=1\n");
	CHECK(fs::is_directory(root + "/config/empty"));
	CHECK(zx.step(10) == StepResult::Done); // idempotent
}

static void testLies() {
	TempDir t;
	const std::string payload(200000, 'z'); // deflates to a few hundred bytes
	{
		// Declared smaller than the real data: rejected on the first extra byte, nothing left behind.
		const std::string zp = t.sub("l1.zip");
		CHECK(buildZip(zp, {boot(), zfile("apps/x/lie.bin", payload)}));
		CHECK(patchCentralUncompressedSize(zp, "apps/x/lie.bin", 1000));
		ZipExtract zx(baseOpts(t));
		CHECK(zx.open(zp)); // the header looks fine
		CHECK(zx.plan().declaredTotal == kBoot.size() + 1000);
		CHECK(zx.begin(t.sub("l1"), 100u << 20));
		runAll(zx, StepResult::Error);
		const ZipError c = zx.failure().code;
		std::printf("  declared-smaller ended with: %s (%s)\n", describe(c), zx.failure().detail.c_str());
		CHECK(c == ZipError::SizeMismatch || c == ZipError::Corrupt);
		CHECK(!there(t.sub("l1/root")));
		CHECK(zx.rootRemoved());
	}
	{
		// Declared larger than the real data: a short entry fails too.
		const std::string zp = t.sub("l2.zip");
		CHECK(buildZip(zp, {boot(), zfile("apps/x/lie.bin", "short")}));
		CHECK(patchCentralUncompressedSize(zp, "apps/x/lie.bin", 5000));
		ZipExtract zx(baseOpts(t));
		CHECK(zx.open(zp));
		CHECK(zx.begin(t.sub("l2"), 100u << 20));
		runAll(zx, StepResult::Error);
		const ZipError c = zx.failure().code;
		std::printf("  declared-larger ended with: %s (%s)\n", describe(c), zx.failure().detail.c_str());
		CHECK(c == ZipError::SizeMismatch || c == ZipError::Corrupt);
		CHECK(!there(t.sub("l2/root")));
	}
	{
		// A stored entry whose central size is patched down.
		const std::string zp = t.sub("l3.zip");
		CHECK(buildZip(zp, {boot(), zfile("apps/x/lie.bin", payload, ZIP_CM_STORE)}));
		CHECK(patchCentralUncompressedSize(zp, "apps/x/lie.bin", 10));
		ZipExtract zx(baseOpts(t));
		if (zx.open(zp)) {
			CHECK(zx.begin(t.sub("l3"), 100u << 20));
			runAll(zx, StepResult::Error);
			CHECK(!there(t.sub("l3/root")));
		} else {
			CHECK(!zx.failure().detail.empty()); // libzip itself may notice at open
		}
	}
}

static size_t g_writes = 0;
static size_t failingWrite(const void* p, size_t size, size_t n, std::FILE* f) {
	if (++g_writes >= 3) { errno = ENOSPC; return 0; }
	return std::fwrite(p, size, n, f);
}

static void testCancelAndEnospc() {
	TempDir t;
	const std::string zp = t.sub("c.zip");
	CHECK(buildZip(zp, {boot(), zfile("apps/x/big.bin", pattern(1024 * 1024, 3), ZIP_CM_STORE), zfile("config/a", "a")}));
	{
		ZipOptions o = baseOpts(t);
		o.subdirHint = {"/config"};
		ZipExtract zx(o);
		CHECK(zx.open(zp));
		CHECK(zx.begin(t.sub("c1"), 100u << 20));
		// Work until the big file is in progress (some bytes written, but not all), then cancel.
		StepResult r = StepResult::Running;
		for (int i = 0; i < 1000 && r == StepResult::Running && zx.bytesWritten() < 200 * 1024; i++) r = zx.step(0);
		CHECK(r == StepResult::Running);
		CHECK(zx.bytesWritten() >= 200 * 1024 && zx.bytesWritten() < zx.totalBytes());
		CHECK(there(t.sub("c1/root/apps/x/big.bin")));
		zx.requestCancel();
		CHECK(zx.step(10) == StepResult::Error);
		CHECK(zx.failure().code == ZipError::Cancelled);
		CHECK(!there(t.sub("c1/root")));
		CHECK(zx.rootRemoved());
		CHECK(zx.step(10) == StepResult::Error); // stays failed
	}
	{
		ZipOptions o = baseOpts(t);
		o.subdirHint = {"/config"};
		o.testWriteFn = &failingWrite;
		g_writes = 0;
		ZipExtract zx(o);
		CHECK(zx.open(zp));
		CHECK(zx.begin(t.sub("c2"), 100u << 20));
		runAll(zx, StepResult::Error);
		CHECK(zx.failure().code == ZipError::Io);
		CHECK(!there(t.sub("c2/root")));
		CHECK(there(t.sub("c2"))); // only <staging>/root is removed, not the staging dir
	}
	{
		// A destroyed, unfinished extraction cleans up after itself.
		{
			ZipOptions o = baseOpts(t);
			o.subdirHint = {"/config"};
			ZipExtract zx(o);
			CHECK(zx.open(zp));
			CHECK(zx.begin(t.sub("c3"), 100u << 20));
			zx.step(0);
			CHECK(there(t.sub("c3/root")));
		}
		CHECK(!there(t.sub("c3/root")));
	}
	{
		// begin() refuses a staging root that already exists, and does not remove what it did not create.
		fs::create_directories(t.sub("c4/root"));
		ZipExtract zx(baseOpts(t));
		CHECK(zx.open(zp));
		CHECK(!zx.begin(t.sub("c4"), 100u << 20));
		CHECK(zx.failure().code == ZipError::Io);
		CHECK(there(t.sub("c4/root")));
	}
}

static void testStateAndStaging() {
	TempDir t;
	const std::string zp = t.sub("m.zip");
	CHECK(buildZip(zp, {boot()}));
	{
		// open() failure is terminal.
		ZipExtract zx(baseOpts(t));
		CHECK(!zx.open(t.sub("missing.zip")));
		CHECK(!zx.open(zp));
		CHECK(zx.failure().code == ZipError::BadState);
		CHECK(!zx.begin(t.sub("g"), 1u << 30));
		CHECK(zx.failure().code == ZipError::BadState);
	}
	{
		// A collision failure is terminal too: a second open() of a good file is refused.
		const std::string bad = t.sub("bad.zip");
		CHECK(buildZip(bad, {boot(), zfile("apps/x/f", "1"), zfile("apps/x/F", "2")}));
		ZipExtract zx(baseOpts(t));
		CHECK(!zx.open(bad));
		CHECK(zx.failure().code == ZipError::Collision);
		CHECK(!zx.open(zp));
		CHECK(zx.failure().code == ZipError::BadState);
	}
	{
		// step() before begin(), begin() twice.
		ZipExtract zx(baseOpts(t));
		CHECK(zx.open(zp));
		CHECK(zx.step(10) == StepResult::Error);
		CHECK(zx.failure().code == ZipError::BadState);
		CHECK(zx.begin(t.sub("b1"), 100u << 20));
		CHECK(!zx.begin(t.sub("b2"), 100u << 20));
		CHECK(zx.failure().code == ZipError::BadState);
		CHECK(!there(t.sub("b2")));
		runAll(zx);
	}
	{
		// Staging directory validation: nothing is created for a bad one.
		const std::vector<std::string> bad = {"", "/", t.sub("rel/../x"), t.sub("rel/./x"), t.sub("rel//x"), t.sub("rel\\x"),
		                                      t.sub("rel/"), "..", "./x"};
		for (const std::string& d : bad) {
			ZipExtract zx(baseOpts(t));
			CHECK(zx.open(zp));
			CHECK(!zx.begin(d, 100u << 20));
			CHECK(zx.failure().code == ZipError::Io);
		}
		CHECK(!there(t.sub("rel")));
		ZipExtract zx(baseOpts(t));
		CHECK(zx.open(zp));
		CHECK(!zx.begin(t.sub("a/../c"), 100u << 20));
		CHECK(!there(t.sub("c")));
		CHECK(zx.begin(t.sub("fine/er"), 100u << 20));
	}
	{
		// No removeRoots: a failed run could not be cleaned up, so begin() refuses and creates nothing.
		ZipOptions o = baseOpts(t);
		o.removeRoots.clear();
		ZipExtract zx(o);
		CHECK(zx.open(zp));
		CHECK(!zx.begin(t.sub("nr"), 100u << 20));
		CHECK(zx.failure().code == ZipError::Io);
		CHECK(!there(t.sub("nr")));
	}
}

static void testMisc() {
	TempDir t;
	ZipExtract zx(baseOpts(t));
	CHECK(!zx.open(t.sub("missing.zip")));
	CHECK(zx.failure().code == ZipError::Open);
	CHECK(zx.step(10) == StepResult::Error);
	{
		spit(t.sub("junk.zip"), "this is not a zip file at all");
		ZipExtract z2(baseOpts(t));
		CHECK(!z2.open(t.sub("junk.zip")));
		CHECK(z2.failure().code == ZipError::Open);
		CHECK(!z2.begin(t.sub("s"), 1u << 30));
		CHECK(z2.failure().code == ZipError::BadState);
	}
}

int main() {
	testNameRejects();
	testCollisions();
	testKindRejects();
	testCaps();
	testPreScan();
	testDuplicateNames();
	testLayoutRejects();
	testSkipped();
	testUndeclared();
	testValidExtract();
	testLies();
	testCancelAndEnospc();
	testStateAndStaging();
	testMisc();
	return checkReport("zip_test");
}
