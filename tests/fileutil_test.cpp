// SPDX-License-Identifier: GPL-3.0-or-later
// Host tests for the guarded directory helpers in core/FileUtil (Increment 2a): removeTree's refusals and its symlink
// rule, commitDir including a failed second rename, findCaseInsensitive, removeDirIfEmpty, isEmptyDir, freeSpace and
// makeDirs. Everything runs on a temp tree. Device-style paths ("sd:/...") are only checked for refusal here (the
// guards run before any filesystem call), so nothing ever touches a real "sd:".
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "Check.h"
#include "core/FileUtil.h"

namespace fs = std::filesystem;

struct TempDir {
	std::string path;
	TempDir() {
		char tmpl[] = "/tmp/openshop-fileutil-XXXXXX";
		path = mkdtemp(tmpl);
	}
	~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
	std::string sub(const std::string& s) const { return path + "/" + s; }
};

static void touch(const std::string& p, const std::string& body = "x") {
	fs::create_directories(fs::path(p).parent_path());
	std::ofstream(p, std::ios::binary) << body;
}
static bool there(const std::string& p) { std::error_code ec; return fs::symlink_status(p, ec).type() != fs::file_type::not_found; }

// A refused call must leave `probe` (a file inside the protected area) in place.
static void expectRefused(const std::string& path, const std::string& under, const std::vector<std::string>& roots,
                          const std::string& probe) {
	RemoveResult r = removeTree(path, under, roots);
	if (!r.refused || r.ok || !there(probe)) std::printf("  not refused: path='%s' under='%s' (%s)\n", path.c_str(), under.c_str(), r.error.c_str());
	CHECK(r.refused);
	CHECK(!r.ok);
	CHECK(there(probe));
}

static void testRemoveRefusals() {
	TempDir t;
	const std::string root = t.path;
	touch(t.sub("apps/foo/boot.dol"));
	touch(t.sub("apps/foo2/boot.dol"));
	touch(t.sub("apps/OpenShop/installed/x.json"));
	touch(t.sub("apps/OpenShop/staging/1/f"));
	touch(t.sub("keep/f"));
	const std::vector<std::string> roots = {root};
	const std::string foo = t.sub("apps/foo");

	expectRefused("/", "/", roots, t.sub("keep/f"));
	expectRefused(root, root, roots, t.sub("keep/f"));
	expectRefused(root, t.sub("apps"), roots, t.sub("keep/f"));
	expectRefused(root + "/", root, roots, t.sub("keep/f"));
	expectRefused(t.sub("apps"), root, roots, t.sub("apps/foo/boot.dol"));
	expectRefused(t.sub("apps/"), root, roots, t.sub("apps/foo/boot.dol"));
	expectRefused(t.sub("apps/OpenShop"), t.sub("apps"), roots, t.sub("apps/OpenShop/installed/x.json"));
	expectRefused(t.sub("apps/OpenShop"), root, roots, t.sub("apps/OpenShop/installed/x.json"));
	expectRefused(t.sub("apps/OpenShop/installed"), t.sub("apps"), roots, t.sub("apps/OpenShop/installed/x.json"));
	expectRefused(t.sub("apps/OpenShop/installed"), t.sub("apps/OpenShop"), roots, t.sub("apps/OpenShop/installed/x.json"));
	expectRefused(t.sub("apps/openshop/installed"), t.sub("apps"), roots, t.sub("apps/OpenShop/installed/x.json")); // case
	expectRefused(t.sub("APPS/OPENSHOP"), root, roots, t.sub("apps/OpenShop/installed/x.json"));
	expectRefused(t.sub("apps/OpenShop/staging"), t.sub("apps/OpenShop/staging"), roots, t.sub("apps/OpenShop/staging/1/f"));
	// ".." and "." segments, also ones that would still land inside.
	expectRefused(t.sub("apps/foo/../foo2"), t.sub("apps"), roots, t.sub("apps/foo2/boot.dol"));
	expectRefused(t.sub("apps/foo/.."), t.sub("apps"), roots, t.sub("apps/foo/boot.dol"));
	expectRefused(t.sub("apps/./foo"), t.sub("apps"), roots, t.sub("apps/foo/boot.dol"));
	expectRefused(foo, t.sub("apps/../apps"), roots, t.sub("apps/foo/boot.dol"));
	expectRefused("a/../b", "a", {"a"}, t.sub("keep/f"));
	// Equal, sibling with a shared prefix, parent, unrelated, trailing-slash variants.
	expectRefused(foo, foo, roots, t.sub("apps/foo/boot.dol"));
	expectRefused(foo + "/", foo, roots, t.sub("apps/foo/boot.dol"));
	expectRefused(t.sub("apps/foo2"), foo, roots, t.sub("apps/foo2/boot.dol"));
	expectRefused(t.sub("apps/foo2/"), foo + "/", roots, t.sub("apps/foo2/boot.dol"));
	expectRefused(t.sub("apps"), foo, roots, t.sub("apps/foo/boot.dol"));
	expectRefused(t.sub("keep"), foo, roots, t.sub("keep/f"));
	// mustBeUnder that is a root, or off the roots; roots empty; empty strings.
	expectRefused(t.sub("keep"), root, roots, t.sub("keep/f"));
	expectRefused(t.sub("keep"), "/", roots, t.sub("keep/f"));
	expectRefused(t.sub("keep"), t.sub("keep/.."), roots, t.sub("keep/f"));
	expectRefused(t.sub("apps/foo"), t.sub("apps"), {}, t.sub("apps/foo/boot.dol"));
	expectRefused(t.sub("apps/foo"), t.sub("apps"), {"/nonexistent-root"}, t.sub("apps/foo/boot.dol"));
	expectRefused("", t.sub("apps"), roots, t.sub("apps/foo/boot.dol"));
	expectRefused(foo, "", roots, t.sub("apps/foo/boot.dol"));
	expectRefused("", "", roots, t.sub("apps/foo/boot.dol"));
	expectRefused(foo + std::string("\\x") , t.sub("apps"), roots, t.sub("apps/foo/boot.dol"));
	expectRefused(std::string("a\0b", 3), "a", {"a"}, t.sub("keep/f"));
	// Device paths: refused by the guards alone, before any filesystem call.
	const std::vector<std::string> dev = {"sd:", "usb:"};
	for (const char* p : {"sd:", "sd:/", "usb:/", "sd:/apps", "sd:/apps/", "sd:apps", "SD:/APPS", "sd:/apps/OpenShop", "sd:/Apps/openshop/",
	                      "usb:/apps/OpenShop", "sd:/apps/OpenShop/installed", "sd:/apps/../x", "sd:/./apps/x", "sd:/private", "sd:/apps/foo2"}) {
		RemoveResult r = removeTree(p, "sd:/apps/foo", dev);
		if (!r.refused) std::printf("  device path not refused: %s\n", p);
		CHECK(r.refused);
		r = removeTree(p, "usb:/apps/foo", dev);
		CHECK(r.refused);
	}
	// mustBeUnder = the apps folder still can't reach OpenShop's folders; a different device is not "under".
	CHECK(removeTree("sd:/apps/OpenShop/staging", "sd:/apps", dev).refused);
	CHECK(removeTree("usb:/apps/foo", "sd:/apps", dev).refused);
	CHECK(removeTree("sd:/apps/foo", "sd:/apps", {}).refused);
	// Everything above was refused, so everything is still there.
	CHECK(there(t.sub("apps/foo/boot.dol")) && there(t.sub("apps/foo2/boot.dol")) && there(t.sub("keep/f")));
}

static void testRemoveWorks() {
	TempDir t;
	const std::vector<std::string> roots = {t.path};
	touch(t.sub("apps/foo/boot.dol"));
	touch(t.sub("apps/foo/a/b/c/d.txt"));
	touch(t.sub("apps/foo/a/e.txt"));
	fs::create_directories(t.sub("apps/foo/empty"));
	touch(t.sub("apps/foo2/boot.dol"));
	touch(t.sub("apps/OpenShop/staging/7/root/apps/x/boot.dol"));
	touch(t.sub("apps/OpenShop/installed/x.json"));

	RemoveResult r = removeTree(t.sub("apps/foo"), t.sub("apps"), roots);
	CHECK(r.ok && !r.refused && r.existed && r.errors == 0 && r.error.empty());
	CHECK(r.files == 3);
	CHECK(r.dirs == 5); // foo, a, b, c, empty
	CHECK(!there(t.sub("apps/foo")));
	CHECK(there(t.sub("apps/foo2/boot.dol"))); // the prefix sibling survives

	// A trailing slash is fine and a second call on a missing path is ok.
	touch(t.sub("apps/bar/f"));
	r = removeTree(t.sub("apps/bar/"), t.sub("apps/"), roots);
	CHECK(r.ok && r.existed);
	r = removeTree(t.sub("apps/bar"), t.sub("apps"), roots);
	CHECK(r.ok && !r.existed && r.files == 0 && r.dirs == 0);

	// A plain file is removed too.
	touch(t.sub("apps/one.txt"));
	r = removeTree(t.sub("apps/one.txt"), t.sub("apps"), roots);
	CHECK(r.ok && r.files == 1 && r.dirs == 0 && !there(t.sub("apps/one.txt")));

	// Staging cleanup is allowed with mustBeUnder inside OpenShop's folder, and only there.
	r = removeTree(t.sub("apps/OpenShop/staging/7"), t.sub("apps/OpenShop/staging"), roots);
	CHECK(r.ok && r.existed && !there(t.sub("apps/OpenShop/staging/7")));
	CHECK(there(t.sub("apps/OpenShop/installed/x.json")));

	// A relative path with the PC root form "./sdcard" works (leading "./" only).
	const std::string cwd = fs::current_path().string();
	CHECK(chdir(t.path.c_str()) == 0);
	touch("sdcard/apps/zed/f");
	r = removeTree("./sdcard/apps/zed", "./sdcard/apps", {"./sdcard"});
	CHECK(r.ok && r.existed && !there("sdcard/apps/zed"));
	touch("sdcard/apps/zed/f");
	r = removeTree("sdcard/apps/zed", "./sdcard/apps", {"./sdcard"}); // the same place written differently
	CHECK(r.ok && !there("sdcard/apps/zed"));
	r = removeTree("sdcard/apps/zed", "./sdcard/apps", {"./other"});
	CHECK(r.refused);
	CHECK(chdir(cwd.c_str()) == 0);
}

static void testRemoveSymlinks() {
	TempDir t;
	const std::vector<std::string> roots = {t.path};
	touch(t.sub("outside/precious.txt"));
	touch(t.sub("outside/deep/more.txt"));
	touch(t.sub("apps/foo/real.txt"));
	CHECK(symlink(t.sub("outside").c_str(), t.sub("apps/foo/linkdir").c_str()) == 0);
	CHECK(symlink(t.sub("outside/precious.txt").c_str(), t.sub("apps/foo/linkfile").c_str()) == 0);
	CHECK(symlink(t.sub("nowhere").c_str(), t.sub("apps/foo/dangling").c_str()) == 0);
	RemoveResult r = removeTree(t.sub("apps/foo"), t.sub("apps"), roots);
	CHECK(r.ok && r.errors == 0);
	CHECK(r.files == 4); // real.txt and the three links
	CHECK(!there(t.sub("apps/foo")));
	CHECK(there(t.sub("outside/precious.txt")) && there(t.sub("outside/deep/more.txt")));

	// The path itself being a symlink to a directory: only the link goes.
	CHECK(symlink(t.sub("outside").c_str(), t.sub("apps/lnk").c_str()) == 0);
	r = removeTree(t.sub("apps/lnk"), t.sub("apps"), roots);
	CHECK(r.ok && r.files == 1 && r.dirs == 0);
	CHECK(!there(t.sub("apps/lnk")) && there(t.sub("outside/precious.txt")));
}

static void testRemoveDepthAndFaults() {
	TempDir t;
	const std::vector<std::string> roots = {t.path};
	// 32 levels below the path is the limit.
	std::string deep = t.sub("apps/d");
	for (int i = 0; i < kRemoveMaxDepth; i++) deep += "/n";
	touch(deep + "/f");
	RemoveResult r = removeTree(t.sub("apps/d"), t.sub("apps"), roots);
	CHECK(r.ok && !there(t.sub("apps/d")));
	std::string tooDeep = t.sub("apps/e");
	for (int i = 0; i < kRemoveMaxDepth + 3; i++) tooDeep += "/n";
	touch(tooDeep + "/f");
	r = removeTree(t.sub("apps/e"), t.sub("apps"), roots);
	CHECK(!r.ok && !r.refused && r.errors > 0 && r.error.find("too deep") != std::string::npos);
	CHECK(there(tooDeep + "/f")); // the too-deep branch is left as it was

	// Injected failures: the first error is reported and the walk goes on.
	touch(t.sub("apps/x/a"));
	touch(t.sub("apps/x/b"));
	touch(t.sub("apps/x/c"));
	static std::string failName;
	failName = "b";
	FileOps ops;
	ops.remove = [](const char* p) { return std::string(p).size() >= 1 && std::string(p).back() == 'b' ? -1 : std::remove(p); };
	r = removeTree(t.sub("apps/x"), t.sub("apps"), roots, ops);
	CHECK(!r.ok && r.errors == 2); // b, then the directory that is not empty (rmdir fails for real)
	CHECK(r.error == "remove failed: " + t.sub("apps/x/b"));
	CHECK(r.files == 2 && !there(t.sub("apps/x/a")) && !there(t.sub("apps/x/c")) && there(t.sub("apps/x/b")));
	FileOps ops2;
	ops2.rmdir = [](const char*) { return -1; };
	r = removeTree(t.sub("apps/x"), t.sub("apps"), roots, ops2);
	CHECK(!r.ok && r.error.rfind("rmdir failed", 0) == 0 && r.dirs == 0);
	FileOps ops3;
	ops3.list = [](const std::string&, std::vector<std::string>&) { return false; };
	r = removeTree(t.sub("apps/x"), t.sub("apps"), roots, ops3);
	CHECK(!r.ok && r.error.rfind("cannot list", 0) == 0);
	FileOps ops4;
	ops4.stat = [](const char*, FileStat*) { return -1; };
	r = removeTree(t.sub("apps/x"), t.sub("apps"), roots, ops4);
	CHECK(!r.ok && !r.refused && !r.existed);
	// A hostile listing (names with slashes or dots) is not followed.
	FileOps ops5;
	ops5.list = [](const std::string&, std::vector<std::string>& n) { n = {"../../keep", "a/b", ""}; return true; };
	touch(t.sub("keep/f"));
	r = removeTree(t.sub("apps/x"), t.sub("apps"), roots, ops5);
	CHECK(!r.ok && r.errors >= 3 && there(t.sub("keep/f")));
}

// The optional stop poll (Increment 7: a cancelled Cleanup job): polled per entry on every level, ends the walk with `stopped`,
// leaves a smaller ordinary tree (no directory on the way is removed) and the same call finishes it later. Both walks (the fd walk
// and the path walk that injected ops select).
static void testRemoveStop() {
	for (int walk = 0; walk < 2; walk++) {
		TempDir t;
		const std::vector<std::string> roots = {t.path};
		FileOps ops; // walk 1: an injected rmdir selects the path walk (real behaviour)
		if (walk == 1) ops.rmdir = [](const char* p) { return ::rmdir(p); };
		auto build = [&] {
			for (const char* d : {"a", "b", "c"})
				for (const char* f : {"1", "2", "3"}) touch(t.sub(std::string("apps/s/") + d + "/" + f));
			touch(t.sub("apps/s/z"));
			touch(t.sub("apps/keep/f"));
		};
		build();
		// never stops: same as no poll
		int polls = 0;
		RemoveResult r = removeTree(t.sub("apps/s"), t.sub("apps"), roots, ops, [&] { polls++; return false; });
		CHECK(r.ok && !r.stopped && !there(t.sub("apps/s")) && polls == 13); // every entry is polled: a, b, c, z and the nine files
		build();
		// stops before the first entry
		r = removeTree(t.sub("apps/s"), t.sub("apps"), roots, ops, [] { return true; });
		CHECK(!r.ok && r.stopped && !r.refused && r.errors == 0 && r.files == 0 && r.dirs == 0);
		CHECK(there(t.sub("apps/s/a/1")) && there(t.sub("apps/s/z")) && there(t.sub("apps/keep/f")));
		// stops inside the second directory (polls: a, a/1, a/2, a/3, b, b/1): a is gone, b and the top directory stay, nothing else
		int n = 0;
		r = removeTree(t.sub("apps/s"), t.sub("apps"), roots, ops, [&] { return ++n > 5; });
		CHECK(!r.ok && r.stopped && r.errors == 0 && r.files == 3 && r.dirs == 1);
		CHECK(there(t.sub("apps/s")) && !there(t.sub("apps/s/a")) && there(t.sub("apps/s/b/1")) && there(t.sub("apps/s/c/1")));
		CHECK(there(t.sub("apps/keep/f")));
		// retry without the poll finishes the job
		r = removeTree(t.sub("apps/s"), t.sub("apps"), roots, ops);
		CHECK(r.ok && !r.stopped && !there(t.sub("apps/s")) && there(t.sub("apps/keep/f")));
		// a stop poll on a missing path: ok, never polled
		bool polled = false;
		r = removeTree(t.sub("apps/s"), t.sub("apps"), roots, ops, [&] { polled = true; return true; });
		CHECK(r.ok && !r.stopped && !polled);
	}
}

// ---- new in the review round ----------------------------------------------------------------------------------------
static std::string gSwapDir, gSwapEntry, gSwapLink;
static int gHookCalls = 0;
// Fires after the fd walk listed gSwapDir: replaces gSwapEntry (a child, or the dir itself) by a symlink to gSwapLink.
static void swapHook(const std::string& dir) {
	gHookCalls++;
	if (dir != gSwapDir) return;
	fs::rename(gSwapEntry, gSwapEntry + ".moved");
	fs::create_symlink(gSwapLink, gSwapEntry);
}

static void testFdWalkRace() {
	const char* scenarios[] = {"child", "grandchild", "top"};
	for (const char* sc : scenarios) {
		TempDir t;
		const std::vector<std::string> roots = {t.path};
		touch(t.sub("apps/x/sub/deeper/f"));
		touch(t.sub("apps/x/top.txt"));
		touch(t.sub("outside/precious/g"));
		touch(t.sub("outside/precious/deep/f"));
		gHookCalls = 0;
		gSwapLink = t.sub("outside/precious");
		if (std::string(sc) == "child") { gSwapDir = t.sub("apps/x"); gSwapEntry = t.sub("apps/x/sub"); }
		else if (std::string(sc) == "grandchild") { gSwapDir = t.sub("apps/x/sub"); gSwapEntry = t.sub("apps/x/sub/deeper"); }
		else { gSwapDir = t.sub("apps/x"); gSwapEntry = t.sub("apps/x"); }
		FileOps ops;
		ops.walkHook = swapHook;
		RemoveResult r = removeTree(t.sub("apps/x"), t.sub("apps"), roots, ops);
		if (!there(t.sub("outside/precious/g")) || !there(t.sub("outside/precious/deep/f"))) std::printf("  race scenario %s deleted outside files\n", sc);
		CHECK(gHookCalls > 0);
		CHECK(there(t.sub("outside/precious/g")));
		CHECK(there(t.sub("outside/precious/deep/f")));
		CHECK(there(t.sub("outside/precious")));
		(void)r;
	}
	// The swapped-in link itself is unlinked (child scenario): the link is gone, the moved-aside real dir's content too.
	{
		TempDir t;
		touch(t.sub("apps/x/sub/f"));
		touch(t.sub("outside/precious/g"));
		gSwapDir = t.sub("apps/x");
		gSwapEntry = t.sub("apps/x/sub");
		gSwapLink = t.sub("outside/precious");
		FileOps ops;
		ops.walkHook = swapHook;
		removeTree(t.sub("apps/x"), t.sub("apps"), {t.path}, ops);
		CHECK(!there(t.sub("apps/x/sub")) && there(t.sub("outside/precious/g")));
	}
	// An ancestor below the root that is a symlink is not walked through.
	{
		TempDir t;
		touch(t.sub("elsewhere/x/f"));
		fs::create_directories(t.sub("root"));
		CHECK(symlink(t.sub("elsewhere").c_str(), t.sub("root/apps").c_str()) == 0);
		RemoveResult r = removeTree(t.sub("root/apps/x"), t.sub("root/apps"), {t.sub("root")});
		CHECK(!r.ok && !r.refused);
		CHECK(there(t.sub("elsewhere/x/f")));
	}
	// A root that is itself a symlink is trusted (./sdcard may be one).
	{
		TempDir t;
		touch(t.sub("real/apps/x/f"));
		CHECK(symlink(t.sub("real").c_str(), t.sub("link").c_str()) == 0);
		RemoveResult r = removeTree(t.sub("link/apps/x"), t.sub("link/apps"), {t.sub("link")});
		CHECK(r.ok && r.existed && !there(t.sub("real/apps/x")) && there(t.sub("real/apps")));
	}
}

static void testDeviceNormalisation() {
	const std::vector<std::string> dev = {"sd:", "usb:"};
	// "sd:foo" (no slash after the colon) is refused in path, mustBeUnder and roots; the bare root stays a root.
	CHECK(removeTree("sd:apps/foo", "sd:/apps", dev).refused);
	CHECK(removeTree("sd:/apps/foo", "sd:apps", dev).refused);
	CHECK(removeTree("sd:/apps/foo", "sd:/apps", {"sd:x"}).refused);
	CHECK(removeTree("sd:foo", "sd:", dev).refused);
	CHECK(removeTree("sd:", "sd:", dev).refused);
	// Executed path is "<device>:/" + components: record what the path walk is asked to touch.
	static std::vector<std::string> seen;
	seen.clear();
	FileOps ops;
	ops.stat = [](const char* p, FileStat* o) { seen.push_back(p); *o = FileStat{}; return 0; };
	RemoveResult r = removeTree("sd:/apps//foo/", "sd:/apps", dev, ops);
	CHECK(r.ok && !r.existed && seen.size() == 1 && seen[0] == "sd:/apps/foo");
	seen.clear();
	r = removeTree("SD:/Apps/./foo", "sd:/apps", dev, ops);
	CHECK(r.refused); // "." segment
	r = removeTree("SD:/Apps/foo", "sd:/apps", dev, ops);
	CHECK(r.ok && seen.size() == 1 && seen[0] == "SD:/Apps/foo");
}

static void testFatAliases() {
	const std::vector<std::string> dev = {"sd:"};
	for (const char* p : {"sd:/apps/OPENSH~1", "sd:/apps/OPENSH~1/installed", "sd:/apps/OpenShop./x", "sd:/apps/OpenShop /x",
	                      "sd:/apps/foo.", "sd:/apps/foo ", "sd:/apps/FOO~2/x", "sd:/apps/a~9b"}) {
		RemoveResult r = removeTree(p, "sd:/apps", dev);
		if (!r.refused) std::printf("  alias not refused: %s\n", p);
		CHECK(r.refused);
	}
	CHECK(removeTree("sd:/apps/foo/x", "sd:/apps/OPENSH~1", dev).refused);
	// Not aliases: a tilde without a digit, and a dot inside a name. They get as far as the stat (injected: not there).
	FileOps ops;
	ops.stat = [](const char*, FileStat* o) { *o = FileStat{}; return 0; };
	CHECK(removeTree("sd:/apps/a~b", "sd:/apps", dev, ops).ok);
	CHECK(removeTree("sd:/apps/a.b", "sd:/apps", dev, ops).ok);
}

static void testStagingOnly() {
	TempDir t;
	const std::vector<std::string> roots = {t.path};
	touch(t.sub("apps/OpenShop/installed/x.json"));
	touch(t.sub("apps/OpenShop/staging/1/f"));
	touch(t.sub("apps/OpenShop/journal.json"));
	expectRefused(t.sub("apps/OpenShop/installed/x.json"), t.sub("apps/OpenShop/installed"), roots, t.sub("apps/OpenShop/installed/x.json"));
	expectRefused(t.sub("apps/OpenShop/installed/x.json"), t.sub("apps/OpenShop"), roots, t.sub("apps/OpenShop/installed/x.json"));
	expectRefused(t.sub("apps/OpenShop/journal.json"), t.sub("apps/OpenShop"), roots, t.sub("apps/OpenShop/journal.json"));
	expectRefused(t.sub("apps/OpenShop/staging/1"), t.sub("apps/OpenShop/installed"), roots, t.sub("apps/OpenShop/staging/1/f"));
	// Staging itself is the one allowed mustBeUnder (any case), and below it.
	const std::vector<std::string> dev = {"sd:"};
	CHECK(removeTree("sd:/apps/OpenShop/installed/x", "sd:/apps/OpenShop/installed", dev).refused);
	CHECK(removeTree("sd:/apps/OpenShop/installed/x", "sd:/apps/OpenShop", dev).refused);
	CHECK(removeTree("sd:/APPS/openshop/Installed/x", "sd:/apps/openshop/installed", dev).refused);
	FileOps ops;
	ops.stat = [](const char*, FileStat* o) { *o = FileStat{}; return 0; };
	CHECK(removeTree("sd:/apps/OpenShop/staging/1", "sd:/apps/OpenShop/staging", dev, ops).ok);
	CHECK(removeTree("SD:/APPS/OPENSHOP/STAGING/1/root", "sd:/apps/openshop/Staging/1", dev, ops).ok);
	CHECK(removeTree("sd:/apps/OpenShop/staging", "sd:/apps/OpenShop/staging", dev, ops).refused);
	RemoveResult r = removeTree(t.sub("apps/OpenShop/staging/1"), t.sub("apps/OpenShop/staging"), roots);
	CHECK(r.ok && r.existed && !there(t.sub("apps/OpenShop/staging/1")) && there(t.sub("apps/OpenShop/installed/x.json")));
}

static void testBadRoots() {
	TempDir t;
	touch(t.sub("apps/foo/f"));
	const std::string foo = t.sub("apps/foo"), under = t.sub("apps");
	for (const std::vector<std::string>& roots : std::vector<std::vector<std::string>>{
	         {"/"}, {""}, {t.path, ""}, {t.path, "/"}, {t.sub("apps")}, {t.sub("apps/OpenShop")}, {t.path, t.sub("x/OPENSHOP")},
	         {t.path, t.sub("Apps/y")}, {t.path, "sd:/apps"}, {"."}, {"./"}, {t.sub("a/../b")}}) {
		RemoveResult r = removeTree(foo, under, roots);
		if (!r.refused) std::printf("  bad roots accepted (first: '%s')\n", roots[0].c_str());
		CHECK(r.refused);
		CHECK(there(t.sub("apps/foo/f")));
	}
	CHECK(removeTree("sd:/apps/foo", "sd:/apps", {"sd:", ""}).refused);
	CHECK(removeTree("sd:/apps/foo", "sd:/apps", {"sd:", "/"}).refused);
	CHECK(removeTree("sd:/apps/foo", "sd:/apps", {"sd:", "usb:/OpenShop"}).refused);
	// A good vector still works.
	CHECK(removeTree(foo, under, {t.path}).ok);
}

static void testDirPrimitives() {
	TempDir t;
	// parkDir / renameStaged / restoreParked one by one.
	touch(t.sub("apps/a/old.txt"));
	touch(t.sub("stage/a/new.txt"));
	CHECK(parkDir(t.sub("apps/a"), t.sub("park/a")));
	CHECK(!there(t.sub("apps/a")) && there(t.sub("park/a/old.txt")));
	CHECK(renameStaged(t.sub("stage/a"), t.sub("apps/a")));
	CHECK(there(t.sub("apps/a/new.txt")) && !there(t.sub("stage/a")));
	// Cut between the two renames, recovery A: finish.
	touch(t.sub("apps/b/old.txt"));
	touch(t.sub("stage/b/new.txt"));
	CHECK(parkDir(t.sub("apps/b"), t.sub("park/b")));
	CHECK(renameStaged(t.sub("stage/b"), t.sub("apps/b")));
	CHECK(there(t.sub("apps/b/new.txt")) && there(t.sub("park/b/old.txt")));
	// Cut between the two renames, recovery B: restore (staged stays).
	touch(t.sub("apps/c/old.txt"));
	touch(t.sub("stage/c/new.txt"));
	CHECK(parkDir(t.sub("apps/c"), t.sub("park/c")));
	CHECK(restoreParked(t.sub("park/c"), t.sub("apps/c")));
	CHECK(there(t.sub("apps/c/old.txt")) && there(t.sub("stage/c/new.txt")) && !there(t.sub("park/c")));
	// Refusals: park exists, target missing, target exists on restore/rename, park inside target, staged not a dir.
	touch(t.sub("park/c/leftover"));
	CHECK(!parkDir(t.sub("apps/c"), t.sub("park/c")) && there(t.sub("apps/c/old.txt")));
	CHECK(!parkDir(t.sub("apps/missing"), t.sub("park/m")));
	CHECK(!parkDir(t.sub("apps/c"), t.sub("apps/c/inside")));
	CHECK(!parkDir(t.sub("apps/c"), t.sub("apps/c")));
	CHECK(!there(t.sub("apps/c/inside")));
	CHECK(!renameStaged(t.sub("stage/c"), t.sub("apps/c")) && there(t.sub("stage/c/new.txt")));
	CHECK(!restoreParked(t.sub("park/c"), t.sub("apps/c")));
	CHECK(!restoreParked(t.sub("park/none"), t.sub("apps/none")));
	touch(t.sub("stage/file"));
	CHECK(!renameStaged(t.sub("stage/file"), t.sub("apps/file")) && there(t.sub("stage/file")));
	CHECK(symlink(t.sub("stage/c").c_str(), t.sub("stage/lnk").c_str()) == 0);
	CHECK(!renameStaged(t.sub("stage/lnk"), t.sub("apps/lnk")) && there(t.sub("stage/lnk")));
	CHECK(commitDir(t.sub("stage/lnk"), t.sub("apps/lnk"), t.sub("park/lnk")).step == CommitDirStep::NotStarted);
	CHECK(commitDir(t.sub("stage/file"), t.sub("apps/file"), t.sub("park/file")).step == CommitDirStep::NotStarted);
	// Injected rename failures reach each primitive's result.
	FileOps nor;
	nor.rename = [](const char*, const char*) { return -1; };
	touch(t.sub("apps/d/f"));
	CHECK(!parkDir(t.sub("apps/d"), t.sub("park/d"), nor) && there(t.sub("apps/d/f")));
	touch(t.sub("stage/d/f"));
	CHECK(!renameStaged(t.sub("stage/d"), t.sub("apps/d2"), nor));
	// A device-root parent counts as existing: only the stat/rename are asked, never a mkdir of "sd:".
	static std::vector<std::string> mk;
	mk.clear();
	FileOps dev;
	dev.mkdir = [](const char* p) { mk.push_back(p); return 0; };
	dev.stat = [](const char* p, FileStat* o) {
		*o = FileStat{};
		o->exists = std::string(p) == "sd:/stage";
		o->isDir = o->exists;
		return 0;
	};
	dev.rename = [](const char*, const char*) { return 0; };
	CommitDirResult r = commitDir("sd:/stage", "sd:/foo", "sd:/park", dev);
	CHECK(r.ok && r.step == CommitDirStep::Renamed && mk.empty());
}

static void testCommitDir() {
	TempDir t;
	// New target (and missing parents).
	touch(t.sub("stage/a/new.txt"), "new");
	CommitDirResult r = commitDir(t.sub("stage/a"), t.sub("apps/a"), t.sub("park/a"));
	CHECK(r.ok && r.step == CommitDirStep::Renamed);
	CHECK(there(t.sub("apps/a/new.txt")) && !there(t.sub("stage/a")) && !there(t.sub("park/a")));

	// Replace: the old one is parked.
	touch(t.sub("stage/b/new.txt"), "new");
	touch(t.sub("apps/b/old.txt"), "old");
	r = commitDir(t.sub("stage/b"), t.sub("apps/b"), t.sub("park/b"));
	CHECK(r.ok && r.step == CommitDirStep::Replaced);
	CHECK(there(t.sub("apps/b/new.txt")) && !there(t.sub("apps/b/old.txt")) && there(t.sub("park/b/old.txt")) && !there(t.sub("stage/b")));

	// Injected failure of the second rename: the old target comes back, staged stays staged.
	static std::string failFrom;
	failFrom = t.sub("stage/c");
	FileOps ops;
	ops.rename = [](const char* from, const char* to) { return failFrom == from ? -1 : std::rename(from, to); };
	touch(t.sub("stage/c/new.txt"), "new");
	touch(t.sub("apps/c/old.txt"), "old");
	r = commitDir(t.sub("stage/c"), t.sub("apps/c"), t.sub("park/c"), ops);
	CHECK(!r.ok && r.step == CommitDirStep::RolledBack);
	CHECK(there(t.sub("apps/c/old.txt")) && !there(t.sub("apps/c/new.txt")) && there(t.sub("stage/c/new.txt")) && !there(t.sub("park/c")));

	// The restore fails too: Stuck, and the old copy is at parkDir.
	touch(t.sub("stage/d/new.txt"), "new");
	touch(t.sub("apps/d/old.txt"), "old");
	static std::string dFrom, dPark, dTarget;
	dFrom = t.sub("stage/d");
	dPark = t.sub("park/d");
	dTarget = t.sub("apps/d");
	FileOps ops2;
	ops2.rename = [](const char* from, const char* to) {
		if (dFrom == from) return -1;
		if (dPark == from && dTarget == to) return -1;
		return std::rename(from, to);
	};
	r = commitDir(t.sub("stage/d"), t.sub("apps/d"), t.sub("park/d"), ops2);
	CHECK(!r.ok && r.step == CommitDirStep::Stuck);
	CHECK(there(t.sub("park/d/old.txt")) && !there(t.sub("apps/d")) && there(t.sub("stage/d/new.txt")));

	// The first rename (parking) fails: nothing moved.
	touch(t.sub("stage/e/new.txt"), "new");
	touch(t.sub("apps/e/old.txt"), "old");
	FileOps ops3;
	ops3.rename = [](const char*, const char*) { return -1; };
	r = commitDir(t.sub("stage/e"), t.sub("apps/e"), t.sub("park/e"), ops3);
	CHECK(!r.ok && r.step == CommitDirStep::NotStarted);
	CHECK(there(t.sub("apps/e/old.txt")) && there(t.sub("stage/e/new.txt")) && !there(t.sub("park/e")));
	// A plain rename that fails for a new target.
	r = commitDir(t.sub("stage/e"), t.sub("apps/e2"), t.sub("park/e2"), ops3);
	CHECK(!r.ok && r.step == CommitDirStep::NotStarted && there(t.sub("stage/e/new.txt")));

	// Refusals before anything moves: missing staged, a leftover parkDir, an mkdir failure, empty arguments.
	r = commitDir(t.sub("stage/missing"), t.sub("apps/m"), t.sub("park/m"));
	CHECK(!r.ok && r.step == CommitDirStep::NotStarted);
	touch(t.sub("park/e/leftover"));
	r = commitDir(t.sub("stage/e"), t.sub("apps/e"), t.sub("park/e"));
	CHECK(!r.ok && r.step == CommitDirStep::NotStarted && there(t.sub("apps/e/old.txt")) && there(t.sub("park/e/leftover")));
	touch(t.sub("stage/f/x"));
	FileOps ops4;
	ops4.mkdir = [](const char*) { return -1; };
	r = commitDir(t.sub("stage/f"), t.sub("newparent/f"), t.sub("park/f"), ops4);
	CHECK(!r.ok && r.step == CommitDirStep::NotStarted && there(t.sub("stage/f/x")));
	r = commitDir("", t.sub("apps/f"), t.sub("park/f"));
	CHECK(!r.ok && r.step == CommitDirStep::NotStarted);
}

static void testSmallHelpers() {
	TempDir t;
	// findCaseInsensitive: the real on-disk name.
	fs::create_directories(t.sub("d/WiiMC"));
	touch(t.sub("d/Other.txt"));
	std::optional<std::string> f = findCaseInsensitive(t.sub("d"), "wiimc");
	CHECK(f && *f == "WiiMC");
	CHECK(findCaseInsensitive(t.sub("d"), "WIIMC") == std::optional<std::string>("WiiMC"));
	CHECK(findCaseInsensitive(t.sub("d"), "other.TXT") == std::optional<std::string>("Other.txt"));
	CHECK(!findCaseInsensitive(t.sub("d"), "wiimc2"));
	CHECK(!findCaseInsensitive(t.sub("d"), "wii"));
	CHECK(!findCaseInsensitive(t.sub("missing"), "wiimc"));
	CHECK(!findCaseInsensitive(t.sub("d"), ""));
	// An exact match wins over a case variant.
	fs::create_directories(t.sub("e/abc"));
	fs::create_directories(t.sub("e/ABC"));
	CHECK(findCaseInsensitive(t.sub("e"), "abc") == std::optional<std::string>("abc"));
	CHECK(findCaseInsensitive(t.sub("e"), "ABC") == std::optional<std::string>("ABC"));

	// isEmptyDir / removeDirIfEmpty.
	fs::create_directories(t.sub("empty"));
	touch(t.sub("full/f"));
	CHECK(isEmptyDir(t.sub("empty")));
	CHECK(!isEmptyDir(t.sub("full")));
	CHECK(!isEmptyDir(t.sub("missing")));
	CHECK(!isEmptyDir(t.sub("full/f"))); // a file
	CHECK(!removeDirIfEmpty(t.sub("full")));
	CHECK(there(t.sub("full/f")));
	CHECK(!removeDirIfEmpty(t.sub("missing")));
	CHECK(!removeDirIfEmpty(t.sub("full/f")) && there(t.sub("full/f")));
	CHECK(removeDirIfEmpty(t.sub("empty")));
	CHECK(!there(t.sub("empty")));
	fs::create_directories(t.sub("empty2"));
	FileOps ops;
	ops.rmdir = [](const char*) { return -1; };
	CHECK(!removeDirIfEmpty(t.sub("empty2"), ops) && there(t.sub("empty2")));

	// freeSpace.
	std::optional<uint64_t> fsp = freeSpace(t.path);
	CHECK(fsp.has_value());
	CHECK(freeSpace(t.path + "/") .has_value());
	CHECK(!freeSpace(t.sub("missing")).has_value());
	CHECK(!freeSpace("").has_value());

	// makeDirs, also through injected ops, and listDir.
	CHECK(makeDirs(t.sub("m/n/o")));
	CHECK(makeDirs(t.sub("m/n/o"))); // already there
	CHECK(there(t.sub("m/n/o")));
	FileOps nomk;
	nomk.mkdir = [](const char*) { return -1; };
	CHECK(!makeDirs(t.sub("p/q"), nomk));
	CHECK(makeDirs(t.sub("m/n"), nomk)); // nothing to create
	std::vector<std::string> names;
	CHECK(listDir(t.sub("m"), names) && names.size() == 1 && names[0] == "n");
	CHECK(!listDir(t.sub("missing"), names));

	// fsyncFile on a real file, and on null.
	std::FILE* fp = std::fopen(t.sub("sync.bin").c_str(), "wb");
	CHECK(fp != nullptr);
	if (fp) {
		std::fputs("hello", fp);
		CHECK(fsyncFile(fp));
		std::fclose(fp);
	}
	CHECK(!fsyncFile(nullptr));
	CHECK(fs::file_size(t.sub("sync.bin")) == 5);
}

int main() {
	testRemoveRefusals();
	testRemoveWorks();
	testRemoveSymlinks();
	testRemoveDepthAndFaults();
	testRemoveStop();
	testFdWalkRace();
	testDeviceNormalisation();
	testFatAliases();
	testStagingOnly();
	testBadRoots();
	testDirPrimitives();
	testCommitDir();
	testSmallHelpers();
	return checkReport("fileutil_test");
}
