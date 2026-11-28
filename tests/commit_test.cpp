// SPDX-License-Identifier: GPL-3.0-or-later
// Host tests for install::Commit and install::recover (M3 Increment 6): a temp "device root", power cuts injected with fork()
// and _exit() at every file-system call and after every op, and recovery that is itself cut and re-run.
//
// Manual check: commit_test --install-local <zip> <root> [slug] [--update]   runs ZipExtract + Commit on a local zip.
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "Check.h"
#include "ZipFixtures.h"
#include "core/NameRules.h"
#include "install/Commit.h"
#include "install/Journal.h"
#include "install/Manifest.h"
#include "install/RecordIo.h"
#include "install/Recovery.h"
#include "install/ZipExtract.h"

namespace fs = std::filesystem;
using namespace install;

// ---- helpers ----------------------------------------------------------------------------------------------------------
struct Temp {
	std::string path;
	Temp() {
		char t[] = "/tmp/openshop-commit-XXXXXX";
		path = mkdtemp(t);
	}
	~Temp() { std::error_code ec; fs::remove_all(path, ec); }
};

static void put(const std::string& p, const std::string& data) {
	std::error_code ec;
	fs::create_directories(fs::path(p).parent_path(), ec);
	spit(p, data);
}
static void mkd(const std::string& p) { std::error_code ec; fs::create_directories(p, ec); }
static bool there(const std::string& p) { std::error_code ec; return fs::symlink_status(p, ec).type() != fs::file_type::not_found; }
static std::string get(const std::string& p) { std::string s; slurp(p, s); return s; }
static size_t kids(const std::string& p) {
	std::error_code ec;
	size_t n = 0;
	for (auto it = fs::directory_iterator(p, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) n++;
	return n;
}

// Everything on the device except staging (checked separately) and "*.tmp" in installed/ (a cut inside a record write may leave
// one; the next save of that record replaces it). Path -> content, "<dir>" for a directory.
using Snap = std::map<std::string, std::string>;
static void snapRec(const fs::path& base, const fs::path& dir, Snap& out) {
	std::error_code ec;
	for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
		const std::string rel = fs::relative(it->path(), base).generic_string();
		if (rel == "apps/OpenShop/staging") continue;
		const bool installed = rel.rfind("apps/OpenShop/installed/", 0) == 0;
		if (installed && rel.size() > 4 && rel.compare(rel.size() - 4, 4, ".tmp") == 0) continue;
		if (it->is_directory(ec)) {
			out[rel] = "<dir>";
			snapRec(base, it->path(), out);
		} else {
			out[rel] = get(it->path().string());
		}
	}
}
static Snap snap(const std::string& root) { Snap s; snapRec(root, root, s); return s; }
static void diff(const Snap& want, const Snap& got) {
	for (const auto& [k, v] : want) {
		auto it = got.find(k);
		if (it == got.end()) std::printf("    missing  %s\n", k.c_str());
		else if (it->second != v) std::printf("    differs  %s (want %.40s got %.40s)\n", k.c_str(), v.c_str(), it->second.c_str());
	}
	for (const auto& [k, v] : got) if (!want.count(k)) std::printf("    extra    %s\n", k.c_str());
}

struct Dev {
	Temp t;
	std::string sd, usb;
	std::vector<std::string> removeRoots;
	Dev() {
		sd = t.path + "/sdcard";
		usb = t.path + "/usbdrive";
		mkd(sd);
		mkd(usb);
		removeRoots = {sd, usb};
	}
};

// Stages files under <root>/apps/OpenShop/staging/<job>/root and returns the ZipExtract plan for them.
static ZipPlan stage(const std::string& root, int64_t job, const std::vector<std::pair<std::string, std::string>>& files,
                     const std::vector<std::string>& emptyDirs = {}) {
	const std::string base = stagingAbs(root, job) + "/root/";
	ZipPlan plan;
	auto note = [&](const std::string& rel) {
		const std::string a = rel.substr(0, rel.find('/'));
		if (a == "apps") {
			const size_t s = rel.find('/', 5);
			const std::string d = rel.substr(5, s == std::string::npos ? std::string::npos : s - 5);
			for (auto& x : plan.appDirs) if (x.name == d) return;
			plan.appDirs.push_back({d, true});
		} else {
			for (auto& x : plan.otherFolders) if (x.name == a) return;
			plan.otherFolders.push_back({a, false});
		}
	};
	for (const auto& [p, c] : files) { put(base + p, c); note(p); }
	for (const std::string& d : emptyDirs) { mkd(base + d); note(d); }
	return plan;
}

static CommitInput mkInput(const Dev& d, const std::string& root, int64_t job, const std::string& slug, bool update, ZipPlan plan,
                           const std::string& version) {
	CommitInput in;
	in.root = root;
	in.device = root == d.sd ? "sd" : "usb";
	in.jobId = job;
	in.slug = slug;
	in.version = version;
	in.archiveMd5 = "";
	in.installedAt = 1700000000;
	in.update = update;
	in.plan = std::move(plan);
	in.removeRoots = d.removeRoots;
	return in;
}

static bool hasFile(const Manifest& m, const std::string& p) {
	for (const ManifestDir& d : m.dirs) for (const ManifestPath& f : d.files) if (f.path == p) return true;
	for (const ManifestFile& f : m.files) if (f.path == p) return true;
	return false;
}

// ---- the multi-dir update scenario ---------------------------------------------------------------------------------------
// v1 is installed for real through commit(); the user then adds saves; v2 is staged. The archive spells the folder "foo" but the
// device has "Foo".
static const std::vector<std::pair<std::string, std::string>> kV1 = {
    {"apps/Foo/boot.dol", "old"}, {"apps/Foo/old.txt", "gone-in-v2"}, {"apps/Foo/shared/x.txt", "oldx"},
    {"apps/Foo/shared/keep.txt", "keep"}, {"apps/Bar/boot.dol", "oldbar"}, {"config/foo/x.ini", "oldini"}};
static const std::vector<std::pair<std::string, std::string>> kV2 = {
    {"apps/foo/boot.dol", "new"}, {"apps/foo/shared/x.txt", "newx"}, {"apps/foo/newfile", "nf"}, {"apps/Bar/boot.dol", "newbar"},
    {"config/foo/x.ini", "newini"}, {"config/foo/y.ini", "newy"}, {"extra/newdir/z.txt", "z"}};

static void setupUpdate(Dev& d) {
	ZipPlan p1 = stage(d.sd, 1, kV1);
	CommitInput in1 = mkInput(d, d.sd, 1, "foo", false, p1, "1.0");
	writeStartJournal(d.sd, "sd", 1, JournalKind::Install, "foo");
	CommitResult r1 = commit(in1);
	CHECK(r1.ok && r1.stagingRemoved);
	put(d.sd + "/apps/Foo/user.cfg", "mine");
	put(d.sd + "/apps/Foo/saves/a.sav", "save");
	mkd(d.sd + "/apps/Foo/emptyold");
	writeStartJournal(d.sd, "sd", 2, JournalKind::Update, "foo");
}

static CommitInput updateInput(Dev& d, const Manifest* old) {
	ZipPlan p2 = stage(d.sd, 2, kV2, {"apps/foo/emptysub"});
	CommitInput in = mkInput(d, d.sd, 2, "foo", true, p2, "2.0");
	in.oldManifest = old;
	return in;
}

static void setupInstall(Dev& d) {
	writeStartJournal(d.sd, "sd", 5, JournalKind::Install, "bar");
}
// An install over files that are on the device already: config/bar/x.ini (parked, then replaced) and an unrelated file.
static void setupInstallPre(Dev& d) {
	put(d.sd + "/config/bar/x.ini", "mine");
	put(d.sd + "/config/other.ini", "o");
	setupInstall(d);
}
// The device spells the outside folder "Config/Bar", the archive "config/bar": the device's spelling must win everywhere.
static void setupInstallCase(Dev& d) {
	put(d.sd + "/Config/Bar/X.ini", "mine");
	put(d.sd + "/Config/other.ini", "o");
	setupInstall(d);
}
// An update where the old and the new archive disagree about what a name is: data/ (folder) becomes a file, cfg (file) becomes a
// folder, shared/sub/ becomes a file; the user also has data.old (so the kept folder must become data.old2).
static void setupKeep(Dev& d) {
	ZipPlan p1 = stage(d.sd, 1, {{"apps/Foo/boot.dol", "old"}, {"apps/Foo/data/x.txt", "oldx"}, {"apps/Foo/cfg", "oldcfg"},
	                             {"apps/Foo/shared/sub/y", "oldy"}, {"apps/Foo/shared/keep", "k"}});
	CHECK(commit(mkInput(d, d.sd, 1, "foo", false, p1, "1.0")).ok);
	put(d.sd + "/apps/Foo/data/user.sav", "save");
	put(d.sd + "/apps/Foo/data.old", "mine");
	writeStartJournal(d.sd, "sd", 2, JournalKind::Update, "foo");
}
static CommitInput keepInput(Dev& d, const Manifest* old) {
	ZipPlan p2 = stage(d.sd, 2, {{"apps/Foo/boot.dol", "new"}, {"apps/Foo/data", "newfile"}, {"apps/Foo/cfg/a.ini", "a"},
	                             {"apps/Foo/shared/sub", "subfile"}, {"apps/Foo/shared/new", "n"}});
	CommitInput in = mkInput(d, d.sd, 2, "foo", true, p2, "2.0");
	in.oldManifest = old;
	return in;
}
static CommitInput installInput(Dev& d) {
	ZipPlan p = stage(d.sd, 5, {{"apps/Bar2/boot.dol", "b"}, {"apps/Bar2/skins/a.png", "p"}, {"config/bar/x.ini", "i"}, {"extra/n/z", "z"}},
	                  {"apps/Bar2/emptysub"});
	return mkInput(d, d.sd, 5, "bar", false, p, "1.0");
}

// ---- fault injection -----------------------------------------------------------------------------------------------------
static int g_count = 0, g_cut = -1;
static bool g_after = false;
static int hit() { return g_count++; }
static void cutNow() { _exit(77); }
static int hRename(const char* a, const char* b) { int me = hit(); if (me == g_cut && !g_after) cutNow(); int rc = std::rename(a, b); if (me == g_cut && g_after) cutNow(); return rc; }
static int hRemove(const char* p) { int me = hit(); if (me == g_cut && !g_after) cutNow(); int rc = std::remove(p); if (me == g_cut && g_after) cutNow(); return rc; }
static int hMkdir(const char* p) { int me = hit(); if (me == g_cut && !g_after) cutNow(); int rc = ::mkdir(p, 0777); if (me == g_cut && g_after) cutNow(); return rc; }
static int hRmdir(const char* p) { int me = hit(); if (me == g_cut && !g_after) cutNow(); int rc = ::rmdir(p); if (me == g_cut && g_after) cutNow(); return rc; }
static FileOps cutOps() {
	FileOps o;
	o.rename = hRename; o.remove = hRemove; o.mkdir = hMkdir; o.rmdir = hRmdir;
	return o;
}
static void hAfterOp(int i) { if (i == g_cut) cutNow(); }
// Flaky reads: stat and list that fail once (call number g_cut) or from then on (the real answer otherwise).
static int realStatT(const char* p, FileStat* o) {
	*o = FileStat{};
	struct stat st;
	if (::lstat(p, &st) != 0) return errno == ENOENT || errno == ENOTDIR ? 0 : -1;
	o->exists = true;
	o->isDir = S_ISDIR(st.st_mode);
	o->isLink = S_ISLNK(st.st_mode);
	o->size = (uint64_t)st.st_size;
	return 0;
}
static bool realListT(const std::string& d, std::vector<std::string>& names) {
	names.clear();
	std::error_code ec;
	auto it = fs::directory_iterator(d, ec);
	if (ec) return false;
	for (; !ec && it != fs::directory_iterator(); it.increment(ec)) names.push_back(it->path().filename().string());
	return !ec;
}
static int hStatOnce(const char* p, FileStat* o) { if (g_count++ == g_cut) return -1; return realStatT(p, o); }
static int hStatFrom(const char* p, FileStat* o) { if (g_count++ >= g_cut) return -1; return realStatT(p, o); }
static bool hListOnce(const std::string& d, std::vector<std::string>& n) { if (g_count++ == g_cut) return false; return realListT(d, n); }
static bool hListFrom(const std::string& d, std::vector<std::string>& n) { if (g_count++ >= g_cut) return false; return realListT(d, n); }
static bool endsWith(const std::string& s, const char* suf) { const size_t n = std::strlen(suf); return s.size() >= n && s.compare(s.size() - n, n, suf) == 0; }
static int hStatJournalErr(const char* p, FileStat* o) { if (endsWith(p, "/.journal")) return -1; return realStatT(p, o); }
static int hStatJournalBakErr(const char* p, FileStat* o) { if (endsWith(p, "/.journal.bak")) return -1; return realStatT(p, o); }
static ReadStatus hReadJournalIo(const char* p, std::string& out, size_t max) {
	if (endsWith(p, "/.journal")) { out.clear(); return ReadStatus::IoError; }
	return readFileCapped(p, out, max);
}
static int hStatStagingErr(const char* p, FileStat* o) { if (endsWith(p, "/staging/2")) return -1; return realStatT(p, o); }
static int hFailFrom(const char* a, const char* b) { if (g_count++ >= g_cut) return -1; return std::rename(a, b); }
static int hFailOnce(const char* a, const char* b) { if (g_count++ == g_cut) return -1; return std::rename(a, b); }

// Runs `fn` in a child. Returns true if the child was cut (exit code 77), false if it ran to the end.
template <class F> static bool runCut(F fn, int cut, bool after) {
	std::fflush(nullptr);
	const pid_t pid = fork();
	if (pid == 0) {
		g_count = 0; g_cut = cut; g_after = after;
		fn();
		_exit(0);
	}
	int st = 0;
	waitpid(pid, &st, 0);
	if (WIFEXITED(st) && WEXITSTATUS(st) == 77) return true;
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
	return false;
}

static Snap g_old, g_new;
static std::string g_scn;
enum class State { Old, New, Broken };
static State classify(const std::string& root, const char* what) {
	const Snap s = snap(root);
	if (s == g_old) return State::Old;
	if (s == g_new) return State::New;
	std::printf("  BROKEN state after %s (%s); diff against OLD:\n", what, g_scn.c_str());
	diff(g_old, s);
	return State::Broken;
}

static CleanGuard guardFor(const Dev& d, const std::string& root) {
	CleanGuard g;
	g.device = root == d.usb ? "usb" : "sd";
	return g;
}

// Recovery leaves staging for Increment 7's Cleanup job (user decision 2026-11-27), so "settled" runs cleanStaging the way that job will.
static bool settled(const Dev& d, const std::string& root) {
	CHECK(!there(root + "/apps/OpenShop/installed/.journal"));
	CHECK(cleanStaging(root, d.removeRoots, guardFor(d, root)));
	CHECK(kids(stagingBase(root)) == 0);
	return !there(root + "/apps/OpenShop/installed/.journal") && kids(stagingBase(root)) == 0;
}

static std::vector<RecoveryRoot> rootsOf(const Dev& d, bool usb = true) {
	std::vector<RecoveryRoot> r{{"sd", d.sd}};
	if (usb) r.push_back({"usb", d.usb});
	return r;
}

// Scenario hooks: set up the device, return the input for commit().
struct Scenario {
	const char* name;
	void (*setup)(Dev&);
	CommitInput (*input)(Dev&, const Manifest*);
	bool update;
	int64_t jobId;
	const char* slug;
};
static CommitInput installIn(Dev& d, const Manifest*) { return installInput(d); }
static const Scenario kScenarios[] = {{"install", setupInstall, installIn, false, 5, "bar"},
                                      {"update", setupUpdate, updateInput, true, 2, "foo"},
                                      {"install-preexisting", setupInstallPre, installIn, false, 5, "bar"},
                                      {"install-case", setupInstallCase, installIn, false, 5, "bar"},
                                      {"update-keep", setupKeep, keepInput, true, 2, "foo"}};

static void loadOld(Dev& d, Manifest& m, bool& have) {
	ManifestLoad ml = loadManifest(d.sd, "foo", "sd");
	have = ml.status == LoadStatus::Ok;
	if (have) m = ml.manifest;
}

static void runCommitIn(Dev& d, const Scenario& sc, bool useAfter, int cut, bool after, FileOps* base = nullptr) {
	Manifest old;
	bool have = false;
	if (sc.update) loadOld(d, old, have);
	CommitInput in = sc.input(d, have ? &old : nullptr);
	if (useAfter) in.afterOp = hAfterOp;
	else in.fileOps = base ? *base : cutOps();
	(void)cut; (void)after;
	commit(in);
}

// ---- tests ------------------------------------------------------------------------------------------------------------
static void testCarryOver() {
	std::map<std::string, std::vector<DirEntry>> oldT = {
	    {"", {{"boot.dol", false}, {"user.cfg", false}, {"saves", true}, {"shared", true}, {"both", false}, {"Dir", true}}},
	    {"shared", {{"x.txt", false}, {"keep.txt", false}}},
	    {"Dir", {{"a", false}}}};
	std::map<std::string, std::vector<DirEntry>> newT = {
	    {"", {{"BOOT.DOL", false}, {"shared", true}, {"both", true}, {"dir", true}}},
	    {"shared", {{"X.txt", false}}},
	    {"dir", {{"A", false}}}};
	auto lister = [](std::map<std::string, std::vector<DirEntry>>& t) {
		return [&t](const std::string& d, std::vector<DirEntry>& o) {
			auto it = t.find(d);
			if (it == t.end()) return false;
			o = it->second;
			return true;
		};
	};
	CarryResult r = carryOver(lister(oldT), lister(newT));
	CHECK(r.ok);
	const std::vector<std::string> want = {"both.old", "saves", "shared/keep.txt", "user.cfg"};
	CHECK(r.paths == want); // boot.dol shipped (folded name), shared entered, Dir/dir shared and A shipped
	// both: an old FILE against a new folder is kept as both.old (user decision 2026-11-27), reported with its source
	CHECK(r.renamed.size() == 1 && r.renamed[0].from == "both" && r.renamed[0].to == "both.old");
	// the kept name skips names that either tree has: both.old (new tree), both.old2 (old tree) -> both.old3
	{
		auto o2 = oldT, n2 = newT;
		o2[""].push_back({"both.old2", false});
		n2[""].push_back({"BOTH.OLD", false});
		CarryResult r2 = carryOver(lister(o2), lister(n2));
		CHECK(r2.ok && r2.renamed.size() == 1 && r2.renamed[0].to == "both.old3");
		CHECK(std::find(r2.paths.begin(), r2.paths.end(), "both.old2") != r2.paths.end()); // the old both.old2 is carried as it is
	}
	CHECK(carryOver(lister(oldT), lister(newT), 2).ok == false); // over the item cap
	std::map<std::string, std::vector<DirEntry>> none;
	CHECK(!carryOver(lister(none), lister(newT)).ok); // old side unreadable
	// deep nesting is refused, not truncated
	std::map<std::string, std::vector<DirEntry>> deepO, deepN;
	std::string p;
	for (int i = 0; i < 40; i++) {
		deepO[p] = {{"d", true}};
		deepN[p] = {{"d", true}};
		p = p.empty() ? "d" : p + "/d";
	}
	CHECK(!carryOver(lister(deepO), lister(deepN)).ok);
}

static void testNewInstall() {
	Dev d;
	// config/foo/x.ini exists already (preexisting), config exists, extra does not.
	put(d.sd + "/config/bar/x.ini", "mine");
	put(d.sd + "/config/other.ini", "o");
	writeStartJournal(d.sd, "sd", 5, JournalKind::Install, "bar");
	ZipPlan p = stage(d.sd, 5,
	                  {{"apps/Bar2/boot.dol", "b"}, {"apps/Bar2/skins/a.png", "p"}, {"config/bar/x.ini", "i"}, {"config/bar/y.ini", "j"},
	                   {"extra/n/z", "z"}},
	                  {"apps/Bar2/emptysub"});
	CommitInput in = mkInput(d, d.sd, 5, "bar", false, p, "1.0");
	CommitResult r = commit(in);
	CHECK(r.ok);
	CHECK(r.stagingRemoved);
	CHECK(settled(d, d.sd));
	CHECK(get(d.sd + "/apps/Bar2/boot.dol") == "b");
	CHECK(get(d.sd + "/apps/Bar2/skins/a.png") == "p");
	CHECK(there(d.sd + "/apps/Bar2/emptysub"));
	CHECK(get(d.sd + "/config/bar/x.ini") == "i");
	CHECK(get(d.sd + "/extra/n/z") == "z");
	ManifestLoad ml = loadManifest(d.sd, "bar", "sd");
	CHECK(ml.status == LoadStatus::Ok);
	const Manifest& m = ml.manifest;
	CHECK(m.slug == "bar" && m.device == "sd" && m.version == "1.0" && m.installedAt == 1700000000);
	CHECK(m.dirs.size() == 1 && m.dirs[0].path == "apps/Bar2" && m.dirs[0].created);
	CHECK(hasFile(m, "apps/Bar2/boot.dol") && hasFile(m, "apps/Bar2/skins/a.png"));
	CHECK(m.dirs[0].createdSubdirs.size() == 2); // skins, emptysub
	bool x = false, y = false;
	for (const ManifestFile& f : m.files) {
		if (f.path == "config/bar/x.ini") { x = true; CHECK(f.preexisting); }
		if (f.path == "config/bar/y.ini") { y = true; CHECK(!f.preexisting); }
	}
	CHECK(x && y);
	// created dirs: extra, extra/n only (config and config/bar existed)
	std::vector<std::string> cd;
	for (const ManifestPath& c : m.createdDirs) cd.push_back(c.path);
	CHECK((cd == std::vector<std::string>{"extra", "extra/n"}));
	// the preexisting file was parked, then removed with the staging
	CHECK(get(d.sd + "/config/other.ini") == "o");
}

static void testExistsRefused() {
	Dev d;
	put(d.sd + "/apps/Bar2/boot.dol", "theirs");
	writeStartJournal(d.sd, "sd", 5, JournalKind::Install, "bar");
	CommitInput in = installInput(d);
	const Snap before = snap(d.sd);
	CommitResult r = commit(in);
	CHECK(!r.ok && r.error == CommitError::Exists);
	CHECK(get(d.sd + "/apps/Bar2/boot.dol") == "theirs");
	CHECK(!there(d.sd + "/config/bar"));
	CHECK(there(stagingAbs(d.sd, 5) + "/root/apps/Bar2/boot.dol")); // staging left for the caller
	(void)before;
}

static void testUpdateCarryOver() {
	Dev d;
	setupUpdate(d);
	Manifest old;
	bool have = false;
	loadOld(d, old, have);
	CHECK(have);
	CommitInput in = updateInput(d, &old);
	CommitResult r = commit(in);
	CHECK(r.ok && r.stagingRemoved);
	CHECK(settled(d, d.sd));
	const std::string foo = d.sd + "/apps/Foo";
	CHECK(there(foo)); // replaced under the EXISTING casing
	{
		std::vector<std::string> names;
		listDir(d.sd + "/apps", names);
		std::sort(names.begin(), names.end());
		CHECK((names == std::vector<std::string>{"Bar", "Foo", "OpenShop"}));
	}
	CHECK(get(foo + "/boot.dol") == "new");
	CHECK(get(foo + "/shared/x.txt") == "newx");
	CHECK(get(foo + "/newfile") == "nf");
	CHECK(get(foo + "/user.cfg") == "mine");        // user file carried over
	CHECK(get(foo + "/saves/a.sav") == "save");      // whole folder carried as one rename
	CHECK(get(foo + "/old.txt") == "gone-in-v2");    // shipped by v1, not by v2: kept
	CHECK(get(foo + "/shared/keep.txt") == "keep");  // folder in both trees: entered
	CHECK(there(foo + "/emptyold") && there(foo + "/emptysub"));
	CHECK(get(d.sd + "/apps/Bar/boot.dol") == "newbar");
	CHECK(get(d.sd + "/config/foo/x.ini") == "newini" && get(d.sd + "/config/foo/y.ini") == "newy");
	CHECK(get(d.sd + "/extra/newdir/z.txt") == "z");
	ManifestLoad ml = loadManifest(d.sd, "foo", "sd");
	CHECK(ml.status == LoadStatus::Ok && ml.manifest.version == "2.0");
	const Manifest& m = ml.manifest;
	CHECK(m.dirs.size() == 2);
	CHECK(hasFile(m, "apps/Foo/boot.dol") && hasFile(m, "apps/Foo/newfile") && hasFile(m, "apps/Foo/old.txt")); // merged with v1
	CHECK(!hasFile(m, "apps/Foo/user.cfg") && !hasFile(m, "apps/Foo/saves/a.sav")); // never ours
	for (const ManifestDir& md : m.dirs) CHECK(md.created); // v1's created=true is kept
	for (const ManifestFile& f : m.files) if (f.path == "config/foo/x.ini") CHECK(!f.preexisting); // old value kept
	// the old Foo/boot.dol went away with the staging
	CHECK(kids(stagingBase(d.sd)) == 0);
}

static void testUpdateUnmanifested() {
	Dev d;
	put(d.sd + "/apps/foo/boot.dol", "olddol");
	put(d.sd + "/apps/foo/save.dat", "save");
	put(d.sd + "/apps/foo/sub/keep", "k");
	writeStartJournal(d.sd, "sd", 3, JournalKind::Update, "foo");
	ZipPlan p = stage(d.sd, 3, {{"apps/foo/boot.dol", "newdol"}, {"apps/foo/lib/a", "a"}, {"apps/foo/sub/n", "n"}});
	CommitInput in = mkInput(d, d.sd, 3, "foo", true, p, "2.0");
	CommitResult r = commit(in);
	CHECK(r.ok);
	CHECK(get(d.sd + "/apps/foo/boot.dol") == "newdol");
	CHECK(get(d.sd + "/apps/foo/save.dat") == "save");
	CHECK(get(d.sd + "/apps/foo/sub/keep") == "k" && get(d.sd + "/apps/foo/sub/n") == "n");
	ManifestLoad ml = loadManifest(d.sd, "foo", "sd");
	CHECK(ml.status == LoadStatus::Ok);
	CHECK(ml.manifest.dirs.size() == 1);
	const ManifestDir& md = ml.manifest.dirs[0];
	CHECK(md.path == "apps/foo" && !md.created && md.adopted); // own folder: adopted
	std::vector<std::string> files;
	for (const ManifestPath& f : md.files) files.push_back(f.path);
	std::sort(files.begin(), files.end());
	CHECK((files == std::vector<std::string>{"apps/foo/boot.dol", "apps/foo/lib/a", "apps/foo/sub/n"})); // only what the archive wrote
	std::vector<std::string> subs;
	for (const ManifestPath& f : md.createdSubdirs) subs.push_back(f.path);
	CHECK((subs == std::vector<std::string>{"apps/foo/lib"})); // sub/ was there
	CHECK(settled(d, d.sd));
}

static void testCaseInPlace() {
	Dev d;
	put(d.sd + "/apps/WiiMC/boot.dol", "old");
	put(d.sd + "/apps/WiiMC/settings.ini", "mine");
	writeStartJournal(d.sd, "sd", 3, JournalKind::Update, "wiimc");
	ZipPlan p = stage(d.sd, 3, {{"apps/wiimc/boot.dol", "new"}});
	CommitResult r = commit(mkInput(d, d.sd, 3, "wiimc", true, p, "2"));
	CHECK(r.ok);
	std::vector<std::string> names;
	listDir(d.sd + "/apps", names);
	std::sort(names.begin(), names.end());
	CHECK((names == std::vector<std::string>{"OpenShop", "WiiMC"}));
	CHECK(get(d.sd + "/apps/WiiMC/boot.dol") == "new" && get(d.sd + "/apps/WiiMC/settings.ini") == "mine");
	ManifestLoad ml = loadManifest(d.sd, "wiimc", "sd");
	CHECK(ml.status == LoadStatus::Ok && ml.manifest.dirs[0].path == "apps/WiiMC");
}

static void testPlanRefusals() {
	// an old tree with too many items in a folder both trees have: refused at planning time, nothing changed
	Dev d;
	put(d.sd + "/apps/foo/boot.dol", "old");
	mkd(d.sd + "/apps/foo/shared");
	for (size_t i = 0; i < kMaxJournalOps + 10; i++) spit(d.sd + "/apps/foo/shared/f" + std::to_string(i), "");
	writeStartJournal(d.sd, "sd", 3, JournalKind::Update, "foo");
	ZipPlan p = stage(d.sd, 3, {{"apps/foo/boot.dol", "new"}, {"apps/foo/shared/n", "n"}});
	CommitResult r = commit(mkInput(d, d.sd, 3, "foo", true, p, "2"));
	CHECK(!r.ok && r.error == CommitError::TooMany);
	CHECK(get(d.sd + "/apps/foo/boot.dol") == "old");
	CHECK(there(d.sd + "/apps/foo/shared/f5") && !there(d.sd + "/apps/OpenShop/installed/foo.json"));
	// a symlink in the staged tree
	Dev e;
	writeStartJournal(e.sd, "sd", 3, JournalKind::Install, "foo");
	ZipPlan q = stage(e.sd, 3, {{"apps/foo/boot.dol", "new"}});
	CHECK(symlink("/etc", (stagingAbs(e.sd, 3) + "/root/apps/foo/link").c_str()) == 0);
	CommitResult r2 = commit(mkInput(e, e.sd, 3, "foo", false, q, "1"));
	CHECK(!r2.ok && r2.error == CommitError::Unsafe);
	CHECK(!there(e.sd + "/apps/foo"));
	// a file where a folder must go
	Dev f;
	put(f.sd + "/config", "a file");
	writeStartJournal(f.sd, "sd", 3, JournalKind::Install, "foo");
	ZipPlan s = stage(f.sd, 3, {{"apps/foo/boot.dol", "new"}, {"config/foo/x", "x"}});
	CommitResult r3 = commit(mkInput(f, f.sd, 3, "foo", false, s, "1"));
	CHECK(!r3.ok && r3.error == CommitError::Conflict);
	CHECK(!there(f.sd + "/apps/foo"));
	// bad arguments
	CommitInput bad = mkInput(f, f.sd, 3, "../x", false, s, "1");
	CHECK(commit(bad).error == CommitError::BadInput);
}

// Reference snapshots for a scenario: old = before commit (journal removed), new = after a clean commit.
static void references(const Scenario& sc) {
	Dev a;
	sc.setup(a);
	Manifest old;
	bool have = false;
	if (sc.update) loadOld(a, old, have);
	CommitInput in = sc.input(a, have ? &old : nullptr);
	removeJournal(a.sd);
	g_old = snap(a.sd);
	writeStartJournal(a.sd, "sd", in.jobId, sc.update ? JournalKind::Update : JournalKind::Install, in.slug);
	CommitResult r = commit(in);
	CHECK(r.ok);
	g_new = snap(a.sd);
	g_scn = sc.name;
	CHECK(g_old != g_new);
}

// Power cuts at every file-system call (before and after it) and after every op, then recovery; recovery run twice.
// With `damage`, a staged manifest that has not been copied yet is destroyed after the cut, so recovery must ROLL BACK: that path
// otherwise only runs for in-process failures.
static void testCrashEverywhere(const Scenario& sc, bool useAfter, bool damage = false) {
	references(sc);
	int cuts = 0, olds = 0, news = 0, rolledBackRuns = 0;
	for (int after = 0; after < (useAfter ? 1 : 2); after++) {
		for (int cut = 0;; cut++) {
			Dev d;
			sc.setup(d);
			const bool cutHappened = runCut([&] { runCommitIn(d, sc, useAfter, cut, after != 0); }, cut, after != 0);
			if (!cutHappened) break;
			cuts++;
			bool damaged = false;
			if (damage) {
				const std::string sm = stagingAbs(d.sd, sc.jobId) + "/manifest.json";
				const std::string jt = get(d.sd + "/apps/OpenShop/installed/.journal");
				if (there(sm) && jt.find("\"phase\":\"commit\"") != std::string::npos &&
				    get(d.sd + "/apps/OpenShop/installed/" + sc.slug + ".json") != get(sm)) {
					spit(sm, "garbage");
					damaged = true;
					rolledBackRuns++;
				}
			}
			recover(rootsOf(d));
			const State s = classify(d.sd, "commit cut + recover");
			CHECK(s != State::Broken);
			if (damaged) CHECK(s == State::Old);
			olds += s == State::Old;
			news += s == State::New;
			CHECK(settled(d, d.sd));
			recover(rootsOf(d)); // idempotent
			CHECK(classify(d.sd, "second recover") == s);
			if (s == State::Broken || cut > 4000) break;
		}
	}
	std::printf("  %s/%s%s: %d cuts, %d recovered old, %d recovered new, %d forced roll-backs\n", sc.name, useAfter ? "after-op" : "fs-call",
	            damage ? "/damaged-manifest" : "", cuts, olds, news, rolledBackRuns);
	CHECK(cuts > 5 && news > 0 && (useAfter || olds > 0));
	if (damage) CHECK(rolledBackRuns > 5);
}

// Recovery itself is cut at every call (for a sample of commit cuts), then run again.
static void testCrashDuringRecovery(const Scenario& sc, int stride, bool damage = false) {
	references(sc);
	int total = 0;
	for (int cut = 0; cut < 4000; cut += stride) {
		Dev d;
		sc.setup(d);
		if (!runCut([&] { runCommitIn(d, sc, false, cut, false); }, cut, false)) break;
		bool damaged = false;
		if (damage) {
			const std::string sm = stagingAbs(d.sd, sc.jobId) + "/manifest.json";
			if (there(sm) && get(d.sd + "/apps/OpenShop/installed/.journal").find("\"phase\":\"commit\"") != std::string::npos &&
			    get(d.sd + "/apps/OpenShop/installed/" + sc.slug + ".json") != get(sm)) {
				spit(sm, "garbage");
				damaged = true;
			}
		}
		// the crashed device is copied for every recovery cut
		for (int after = 0; after < 2; after++) {
			for (int rcut = 0;; rcut++) {
				Temp copy;
				std::error_code ec;
				fs::copy(d.t.path, copy.path, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
				// paths differ, so the journal's relative paths are what matter; the roots are the copy's
				const std::string sd = copy.path + "/sdcard", usb = copy.path + "/usbdrive";
				const std::vector<RecoveryRoot> roots = {{"sd", sd}, {"usb", usb}};
							const bool cutHappened = runCut([&] { recover(roots, cutOps()); }, rcut, after != 0);
				if (!cutHappened) break;
				total++;
				recover(roots);
				const State s = classify(sd, "recovery cut + recover");
				CHECK(s != State::Broken);
				if (damaged) CHECK(s == State::Old);
				CHECK(!there(sd + "/apps/OpenShop/installed/.journal"));
				if (s == State::Broken) return;
			}
		}
	}
	std::printf("  %s: %d recovery cuts survived\n", sc.name, total);
	CHECK(total > 10);
}

// A failing rename (once, or from the Nth on) makes the commit roll back in-process (or leave the journal), then recovery.
static void testFailInjection(const Scenario& sc) {
	references(sc);
	int rolled = 0, left = 0, refused = 0, okc = 0;
	for (int mode = 0; mode < 2; mode++) {
		for (int n = 0;; n++) {
			Dev d;
			sc.setup(d);
			g_count = 0; g_cut = n;
			Manifest old;
			bool have = false;
			if (sc.update) loadOld(d, old, have);
			CommitInput in = sc.input(d, have ? &old : nullptr);
			in.fileOps.rename = mode == 0 ? hFailOnce : hFailFrom;
			const CommitResult r = commit(in);
			if (r.ok) {
				okc++;
				if (g_count <= n) break; // the failure point is past the end
				recover(rootsOf(d));
				CHECK(classify(d.sd, "ok commit with a failed rename") == State::New);
				continue;
			}
			if (r.error == CommitError::OpFailed) {
				if (r.rolledBack) {
					rolled++;
					if (classify(d.sd, "in-process rollback") != State::Old) {
						CHECK(false);
						std::printf("    mode %d n %d: %s\n", mode, n, r.detail.c_str());
					}
					CHECK(settled(d, d.sd));
				} else {
					left++;
					CHECK(r.journalLeft);
				}
			} else refused++;
			recover(rootsOf(d));
			const State s = classify(d.sd, "failure + recover");
			CHECK(s != State::Broken);
			CHECK(settled(d, d.sd));
			if (r.error != CommitError::OpFailed) CHECK(s == State::Old);
			if (n > 3000) break;
		}
	}
	std::printf("  %s: rename failures: %d rolled back in process, %d left to recovery, %d refused at planning, %d ok\n", sc.name, rolled, left,
	            refused, okc);
	CHECK(rolled > 0);
}

static void testRecoveryRoots() {
	Dev d;
	// sd: an install cut after its renames (roll forward); usb: a start-phase journal plus staging with a leftover root/
	setupInstall(d);
	runCut([&] { CommitInput in = installInput(d); in.afterOp = hAfterOp; commit(in); }, 2, false);
	writeStartJournal(d.usb, "usb", 9, JournalKind::Install, "zzz");
	put(stagingAbs(d.usb, 9) + "/root/apps/zzz/boot.dol", "x"); // a failed extraction's leftover root/
	put(stagingAbs(d.usb, 10) + "/junk", "x");
	// only sd is mounted: the usb journal and staging are untouched, nothing is logged as recovered for it
	std::vector<RootRecovery> r1 = recover(rootsOf(d, false));
	CHECK(r1.size() == 1 && r1[0].id == "sd" && r1[0].outcome == RecoveryOutcome::RolledForward);
	CHECK(there(d.usb + "/apps/OpenShop/installed/.journal") && there(stagingAbs(d.usb, 9) + "/root/apps/zzz/boot.dol"));
	CHECK(get(d.sd + "/apps/Bar2/boot.dol") == "b");
	CHECK(loadManifest(d.sd, "bar", "sd").status == LoadStatus::Ok);
	// a later start with the usb drive present
	std::vector<RootRecovery> r2 = recover(rootsOf(d));
	CHECK(r2.size() == 2 && r2[0].outcome == RecoveryOutcome::Nothing && r2[1].outcome == RecoveryOutcome::DroppedStartJournal);
	CHECK(!there(d.usb + "/apps/OpenShop/installed/.journal"));
	CHECK(!there(d.usb + "/apps/zzz"));
	// Start-up recovery never bulk-deletes staging (user decision 2026-11-27): the finished sd job's staging (its old/ tree is
	// the Cleanup job's to remove), the usb job's leftover root/ and a journal-less folder are all still there.
	CHECK(there(stagingAbs(d.sd, 5) + "/manifest.json") && there(stagingAbs(d.usb, 9) + "/root/apps/zzz/boot.dol") && there(stagingAbs(d.usb, 10) + "/junk"));
	CHECK(cleanStaging(d.usb, d.removeRoots, guardFor(d, d.usb)) && cleanStaging(d.sd, d.removeRoots, guardFor(d, d.sd)) && kids(stagingBase(d.usb)) == 0 && kids(stagingBase(d.sd)) == 0);
	// nothing left: nothing happens
	CHECK(recover(rootsOf(d))[0].outcome == RecoveryOutcome::Nothing);
}

static void testCorruptAndForeign() {
	Dev d;
	// garbage journal on sd
	put(d.sd + "/apps/OpenShop/installed/.journal", "{not json");
	put(stagingAbs(d.sd, 4) + "/old/apps/Foo/boot.dol", "precious");
	// a journal written for usb sitting on sd
	writeStartJournal(d.usb, "usb", 6, JournalKind::Install, "x");
	put(d.usb + "/apps/OpenShop/staging/6/root/a", "x");
	const std::string foreign = get(d.usb + "/apps/OpenShop/installed/.journal");
	put(d.sd + "/apps/OpenShop/installed/.journal", foreign);
	std::vector<RootRecovery> r = recover({{"sd", d.sd}});
	CHECK(r[0].outcome == RecoveryOutcome::LeftAlone);
	CHECK(get(d.sd + "/apps/OpenShop/installed/.journal") == foreign);
	CHECK(get(stagingAbs(d.sd, 4) + "/old/apps/Foo/boot.dol") == "precious");
	put(d.sd + "/apps/OpenShop/installed/.journal", "{not json");
	r = recover({{"sd", d.sd}});
	CHECK(r[0].outcome == RecoveryOutcome::LeftAlone);
	CHECK(get(d.sd + "/apps/OpenShop/installed/.journal") == "{not json");
	CHECK(get(stagingAbs(d.sd, 4) + "/old/apps/Foo/boot.dol") == "precious");
	// the usb journal is fine on usb
	CHECK(recover({{"usb", d.usb}})[0].outcome == RecoveryOutcome::DroppedStartJournal);
	CHECK(there(d.usb + "/apps/OpenShop/staging/6/root/a")); // its staging is left for the Cleanup job
	// a commit-phase journal whose ops were tampered with (op bound to another job's staging) is corrupt too
	Dev e;
	setupInstall(e);
	runCut([&] { CommitInput in = installInput(e); in.afterOp = hAfterOp; commit(in); }, 1, false);
	std::string j = get(e.sd + "/apps/OpenShop/installed/.journal");
	const size_t at = j.find("staging/5/root");
	CHECK(at != std::string::npos);
	j.replace(at, 14, "staging/6/root");
	put(e.sd + "/apps/OpenShop/installed/.journal", j);
	CHECK(recover({{"sd", e.sd}})[0].outcome == RecoveryOutcome::LeftAlone);
	CHECK(there(stagingAbs(e.sd, 5) + "/manifest.json")); // staging untouched
}

static void testStagingCleanupStaysInside() {
	Dev d;
	put(d.sd + "/apps/OpenShop/installed/foo.json", "{}");
	put(d.sd + "/apps/Foo/boot.dol", "app");
	put(d.sd + "/apps/OpenShop/staging2/keep", "k"); // a sibling whose name starts with "staging"
	put(d.sd + "/outside/precious", "p");
	put(stagingAbs(d.sd, 7) + "/root/apps/x/boot.dol", "x");
	put(stagingAbs(d.sd, 7) + "/old/apps/x/y", "y");
	put(d.sd + "/apps/OpenShop/staging/stray-file", "s");
	CHECK(symlink((d.sd + "/outside").c_str(), (stagingAbs(d.sd, 7) + "/root/link").c_str()) == 0);
	CHECK(symlink((d.sd + "/apps/Foo").c_str(), (d.sd + "/apps/OpenShop/staging/toplink").c_str()) == 0);
	CHECK(cleanStaging(d.sd, d.removeRoots, guardFor(d, d.sd)));
	CHECK(kids(stagingBase(d.sd)) == 0);
	CHECK(get(d.sd + "/outside/precious") == "p");
	CHECK(get(d.sd + "/apps/Foo/boot.dol") == "app");
	CHECK(get(d.sd + "/apps/OpenShop/installed/foo.json") == "{}");
	CHECK(get(d.sd + "/apps/OpenShop/staging2/keep") == "k");
	// bad roots refuse: nothing is removed
	put(stagingAbs(d.sd, 8) + "/root/a", "a");
	CHECK(!cleanStaging(d.sd, {}, guardFor(d, d.sd)));
	CHECK(there(stagingAbs(d.sd, 8) + "/root/a"));
}

// The Cleanup guard (Increment 7): journal-named dir, active jobs, unclear journals, stop.
static void testCleanGuard() {
	// a start-phase journal naming job 4: staging/4 stays, the others go
	{
		Dev d;
		CHECK(writeStartJournal(d.sd, "sd", 4, JournalKind::Install, "foo"));
		put(stagingAbs(d.sd, 4) + "/root/a", "a");
		put(stagingAbs(d.sd, 5) + "/root/b", "b");
		put(d.sd + "/apps/OpenShop/staging/stray", "s");
		CleanStats cs;
		CHECK(cleanStaging(d.sd, d.removeRoots, guardFor(d, d.sd), {}, &cs));
		CHECK(cs.skippedJournal == 1 && cs.removed == 2);
		CHECK(there(stagingAbs(d.sd, 4) + "/root/a") && !there(stagingAbs(d.sd, 5)) && !there(d.sd + "/apps/OpenShop/staging/stray"));
	}
	// active job ids are skipped
	{
		Dev d;
		put(stagingAbs(d.sd, 7) + "/root/a", "a");
		put(stagingAbs(d.sd, 8) + "/root/a", "a");
		CleanGuard g = guardFor(d, d.sd);
		g.activeJobIds = {7};
		CleanStats cs;
		CHECK(cleanStaging(d.sd, d.removeRoots, g, {}, &cs));
		CHECK(cs.skippedActive == 1 && cs.removed == 1 && there(stagingAbs(d.sd, 7) + "/root/a") && !there(stagingAbs(d.sd, 8)));
	}
	// a corrupt journal, a journal for the other device and a journal that cannot be told from absent: NOTHING is removed
	{
		Dev d;
		put(stagingAbs(d.sd, 3) + "/old/x", "precious");
		put(d.sd + "/apps/OpenShop/installed/.journal", "{not json");
		CleanStats cs;
		CHECK(!cleanStaging(d.sd, d.removeRoots, guardFor(d, d.sd), {}, &cs));
		CHECK(cs.journalUnclear && cs.removed == 0 && get(stagingAbs(d.sd, 3) + "/old/x") == "precious");
		writeStartJournal(d.usb, "usb", 6, JournalKind::Install, "x");
		put(d.sd + "/apps/OpenShop/installed/.journal", get(d.usb + "/apps/OpenShop/installed/.journal"));
		CHECK(!cleanStaging(d.sd, d.removeRoots, guardFor(d, d.sd)));
		CHECK(get(stagingAbs(d.sd, 3) + "/old/x") == "precious");
	}
	{
		Dev d;
		put(stagingAbs(d.sd, 3) + "/old/x", "precious");
		FileOps o;
		o.stat = hStatJournalErr;
		CleanStats cs;
		CHECK(!cleanStaging(d.sd, d.removeRoots, guardFor(d, d.sd), o, &cs));
		CHECK(cs.journalUnclear && get(stagingAbs(d.sd, 3) + "/old/x") == "precious");
		CHECK(cleanStaging(d.sd, d.removeRoots, guardFor(d, d.sd)) && !there(stagingAbs(d.sd, 3)));
	}
	// stop() ends the run early
	{
		Dev d;
		put(stagingAbs(d.sd, 1) + "/a", "a");
		put(stagingAbs(d.sd, 2) + "/a", "a");
		CleanGuard g = guardFor(d, d.sd);
		g.stop = [] { return true; };
		CleanStats cs;
		CHECK(!cleanStaging(d.sd, d.removeRoots, g, {}, &cs));
		CHECK(cs.stopped && cs.removed == 0 && there(stagingAbs(d.sd, 1) + "/a"));
	}
	// no staging folder at all: fine
	{
		Dev d;
		CHECK(cleanStaging(d.sd, d.removeRoots, guardFor(d, d.sd)));
	}
}

static void testBrokenStagedManifestRollsBack() {
	const Scenario& sc = kScenarios[1];
	references(sc);
	Manifest old;
	bool have = false;
	int last = 0;
	{
		Dev f; // a clean run tells how many ops there are; the op before writeManifest is the last rename
		sc.setup(f);
		loadOld(f, old, have);
		last = commit(sc.input(f, &old)).ops - 2;
	}
	// cut after the last rename but before the manifest copy, with the staged manifest damaged: the whole job is rolled back
	Dev g;
	sc.setup(g);
	loadOld(g, old, have);
	runCut([&] { CommitInput in = sc.input(g, &old); in.afterOp = hAfterOp; commit(in); }, last, false);
	put(stagingAbs(g.sd, 2) + "/manifest.json", "garbage");
	CHECK(recover(rootsOf(g))[0].outcome == RecoveryOutcome::RolledBack);
	CHECK(classify(g.sd, "roll back with a damaged staged manifest") == State::Old);
	// a journal whose staging folder is gone: dropped, nothing is moved (undoing without the staging would unmake the install)
	Dev h;
	sc.setup(h);
	loadOld(h, old, have);
	runCut([&] { CommitInput in = sc.input(h, &old); in.afterOp = hAfterOp; commit(in); }, last, false);
	std::error_code ec;
	fs::remove_all(stagingAbs(h.sd, 2), ec);
	CHECK(recover(rootsOf(h))[0].outcome == RecoveryOutcome::DroppedNoStaging); // L6: its own outcome, not "cleaned"
	CHECK(there(h.sd + "/apps/Foo/newfile"));
	CHECK(!there(h.sd + "/apps/OpenShop/installed/.journal"));
	// Install state UNKNOWN: the renames were done but the staged manifest is gone, so installed/foo.json is still v1's. Nothing
	// moved, and the stale record is what remains (documented in Recovery.h).
	{
		ManifestLoad ml = loadManifest(h.sd, "foo", "sd");
		CHECK(ml.status == LoadStatus::Ok && ml.manifest.version == "1.0");
	}
	CHECK(get(h.sd + "/apps/Foo/boot.dol") == "new" && get(h.sd + "/apps/Foo/user.cfg") == "mine");
}

static void testRealZip() {
	Dev d;
	const std::string zp = d.t.path + "/t.zip";
	CHECK(buildZip(zp, {zfile("apps/Real/boot.dol", "DOL"), zfile("apps/Real/res/a.txt", "A"), zfile("config/real/c.ini", "C"),
	                    zfile("readme.txt", "skipped")}));
	ZipOptions zo;
	zo.packageType = PackageType::Dol;
	zo.removeRoots = d.removeRoots;
	ZipExtract zx(zo);
	CHECK(zx.open(zp));
	if (zx.plan().needsConfirmation()) zx.confirmUndeclared();
	writeStartJournal(d.sd, "sd", 11, JournalKind::Install, "real");
	CHECK(zx.begin(stagingAbs(d.sd, 11), 1ull << 30));
	StepResult sr;
	do sr = zx.step(50); while (sr == StepResult::Running);
	CHECK(sr == StepResult::Done);
	CommitInput in = mkInput(d, d.sd, 11, "real", false, zx.plan(), "1");
	CommitResult r = commit(in);
	CHECK(r.ok && r.stagingRemoved);
	CHECK(get(d.sd + "/apps/Real/res/a.txt") == "A" && get(d.sd + "/config/real/c.ini") == "C");
	CHECK(!there(d.sd + "/readme.txt"));
	CHECK(loadManifest(d.sd, "real", "sd").status == LoadStatus::Ok);
}

// ---- review fixes (2026-11-27) -------------------------------------------------------------------------------------------
// Decision (b): an old folder against a new file of the same name (and the reverse) is kept under "<name>.old".
static void testKeptRename() {
	Dev d;
	setupKeep(d);
	Manifest old;
	bool have = false;
	loadOld(d, old, have);
	CHECK(have);
	CommitResult r = commit(keepInput(d, &old));
	CHECK(r.ok && r.stagingRemoved);
	const std::string foo = d.sd + "/apps/Foo";
	CHECK(get(foo + "/boot.dol") == "new");
	CHECK(get(foo + "/data") == "newfile");                                   // the new FILE is shipped
	CHECK(get(foo + "/data.old") == "mine");                                  // the user's own data.old is untouched ...
	CHECK(get(foo + "/data.old2/x.txt") == "oldx" && get(foo + "/data.old2/user.sav") == "save"); // ... so the folder became .old2
	CHECK(get(foo + "/cfg/a.ini") == "a" && get(foo + "/cfg.old") == "oldcfg"); // old FILE against a new folder
	CHECK(get(foo + "/shared/sub") == "subfile" && get(foo + "/shared/sub.old/y") == "oldy"); // nested
	CHECK(get(foo + "/shared/keep") == "k" && get(foo + "/shared/new") == "n");
	CHECK(r.kept.size() == 3);
	auto keptTo = [&](const std::string& from) {
		for (const auto& k : r.kept) if (k.from == from) return k.to;
		return std::string();
	};
	CHECK(keptTo("apps/Foo/data") == "apps/Foo/data.old2" && keptTo("apps/Foo/cfg") == "apps/Foo/cfg.old" &&
	      keptTo("apps/Foo/shared/sub") == "apps/Foo/shared/sub.old");
	ManifestLoad ml = loadManifest(d.sd, "foo", "sd");
	CHECK(ml.status == LoadStatus::Ok);
	const Manifest& m = ml.manifest;
	// Kept entries are untracked user data: never in the manifest. The old v1 entries below/at the old path are gone with them.
	for (const ManifestDir& md : m.dirs) {
		for (const ManifestPath& f : md.files) CHECK(f.path.find(".old") == std::string::npos);
		for (const ManifestPath& f : md.createdSubdirs) CHECK(f.path.find(".old") == std::string::npos);
	}
	CHECK(hasFile(m, "apps/Foo/data") && !hasFile(m, "apps/Foo/data/x.txt") && !hasFile(m, "apps/Foo/shared/sub/y"));
	CHECK(hasFile(m, "apps/Foo/cfg/a.ini") && !hasFile(m, "apps/Foo/cfg"));
	bool cfgDir = false;
	for (const ManifestPath& c : m.dirs[0].createdSubdirs) if (c.path == "apps/Foo/cfg") cfgDir = true;
	CHECK(cfgDir); // the folder cfg/ did not exist after the update's own point of view: the update created it
	CHECK(settled(d, d.sd));
}

// An old name the records refuse stops the update (NameRefused, nothing changed); only counts are TooMany.
static void testNameRefusedAndTooMany() {
	Dev d;
	put(d.sd + "/apps/foo/boot.dol", "old");
	put(d.sd + "/apps/foo/trailing.", "x"); // FAT would drop the dot: the records refuse the name
	writeStartJournal(d.sd, "sd", 3, JournalKind::Update, "foo");
	ZipPlan p = stage(d.sd, 3, {{"apps/foo/boot.dol", "new"}});
	CommitResult r = commit(mkInput(d, d.sd, 3, "foo", true, p, "2"));
	CHECK(!r.ok && r.error == CommitError::NameRefused);
	CHECK(r.detail.find("trailing.") != std::string::npos);
	CHECK(get(d.sd + "/apps/foo/boot.dol") == "old" && get(d.sd + "/apps/foo/trailing.") == "x");
	CHECK(!there(d.sd + "/apps/OpenShop/installed/.journal.tmp") && !there(stagingAbs(d.sd, 3) + "/manifest.json"));
	// a kept name that would be too long is refused too (a 238-character folder against a file)
	Dev e;
	const std::string longName(238, 'n');
	put(e.sd + "/apps/foo/" + longName + "/a", "x");
	writeStartJournal(e.sd, "sd", 3, JournalKind::Update, "foo");
	ZipPlan q = stage(e.sd, 3, {{"apps/foo/boot.dol", "new"}, {"apps/foo/" + longName, "file"}});
	CommitResult r2 = commit(mkInput(e, e.sd, 3, "foo", true, q, "2"));
	CHECK(!r2.ok && r2.error == CommitError::NameRefused);
	CHECK(get(e.sd + "/apps/foo/" + longName + "/a") == "x");
}

// L2: an update without an old manifest records the app dirs that it created as created:true.
static void testUpdateNoManifestCreatedFlags() {
	Dev d;
	put(d.sd + "/apps/foo/boot.dol", "old");
	writeStartJournal(d.sd, "sd", 3, JournalKind::Update, "foo");
	ZipPlan p = stage(d.sd, 3, {{"apps/foo/boot.dol", "new"}, {"apps/Extra/boot.dol", "e"}, {"apps/Extra/sub/f", "f"}});
	CommitResult r = commit(mkInput(d, d.sd, 3, "foo", true, p, "2"));
	CHECK(r.ok);
	ManifestLoad ml = loadManifest(d.sd, "foo", "sd");
	CHECK(ml.status == LoadStatus::Ok && ml.manifest.dirs.size() == 2);
	for (const ManifestDir& md : ml.manifest.dirs) {
		if (md.path == "apps/foo") CHECK(!md.created && md.adopted);
		else CHECK(md.path == "apps/Extra" && md.created && !md.adopted);
	}
}

// Outside folders spelled differently on the device ("Config/Bar") and in the archive ("config/bar"): the device's spelling wins,
// on ext4 as on FAT (the crash tests cover the same scenario as "install-case").
static void testOutsideCase() {
	Dev d;
	setupInstallCase(d);
	CommitResult r = commit(installInput(d));
	CHECK(r.ok);
	std::vector<std::string> top;
	listDir(d.sd, top);
	std::sort(top.begin(), top.end());
	CHECK((top == std::vector<std::string>{"Config", "apps", "extra"}));
	std::vector<std::string> inCfg;
	listDir(d.sd + "/Config", inCfg);
	std::sort(inCfg.begin(), inCfg.end());
	CHECK((inCfg == std::vector<std::string>{"Bar", "other.ini"}));
	CHECK(get(d.sd + "/Config/Bar/X.ini") == "i");
	std::vector<std::string> inBar;
	listDir(d.sd + "/Config/Bar", inBar);
	CHECK(inBar.size() == 1);
	ManifestLoad ml = loadManifest(d.sd, "bar", "sd");
	CHECK(ml.status == LoadStatus::Ok);
	bool found = false;
	for (const ManifestFile& f : ml.manifest.files)
		if (f.path == "Config/Bar/X.ini") { found = true; CHECK(f.preexisting); }
	CHECK(found);
	for (const ManifestPath& c : ml.manifest.createdDirs) CHECK(c.path.rfind("extra", 0) == 0); // Config/Bar were there
	CHECK(settled(d, d.sd));
}

// L4: a crafted journal must not be able to move an app dir (or take one away) when the staged tree is not there.
static Journal craftedJournal(int64_t job, JournalKind kind) {
	Journal j;
	j.phase = JournalPhase::Commit;
	j.jobId = job;
	j.kind = kind;
	j.slug = "foo";
	j.device = "sd";
	j.stagingDir = stagingRelPath(job);
	return j;
}
static JournalOp wmOp(const Journal& j) { return {OpType::WriteManifest, manifestRelPath("foo"), stagedManifestPath(j.stagingDir), false, false}; }
static void testCraftedJournals() {
	{
		Dev d; // renameDir of an existing app, no staged manifest, no staged root: rollBack must leave apps/Foo where it is
		put(d.sd + "/apps/Foo/boot.dol", "mine");
		put(stagingAbs(d.sd, 4) + "/marker", "x");
		Journal j = craftedJournal(4, JournalKind::Install);
		j.ops.push_back({OpType::RenameDir, "apps/Foo", stagedRootPath(j.stagingDir, "apps/Foo"), false, false});
		j.ops.push_back(wmOp(j));
		CHECK(journalSaveProblem(j).empty() && saveJournal(d.sd, j));
		const std::vector<RootRecovery> r = recover(rootsOf(d, false));
		CHECK(r[0].outcome == RecoveryOutcome::RolledBack);
		CHECK(get(d.sd + "/apps/Foo/boot.dol") == "mine");
		CHECK(!there(stagingAbs(d.sd, 4) + "/root"));
		// the same for a staged file that is not there: a user file at the path stays put
		Dev e;
		put(e.sd + "/config/foo/x.ini", "precious");
		put(stagingAbs(e.sd, 4) + "/marker", "x");
		Journal k = craftedJournal(4, JournalKind::Install);
		k.ops.push_back({OpType::WriteFile, "config/foo/x.ini", stagedRootPath(k.stagingDir, "config/foo/x.ini"), false, false});
		k.ops.push_back(wmOp(k));
		CHECK(saveJournal(e.sd, k));
		CHECK(recover(rootsOf(e, false))[0].outcome == RecoveryOutcome::RolledBack);
		CHECK(get(e.sd + "/config/foo/x.ini") == "precious" && !there(stagingAbs(e.sd, 4) + "/root"));
	}
	{
		Dev d; // parkDir + renameDir with an intact staged manifest but no staged replacement: roll forward must not park apps/Foo
		put(d.sd + "/apps/Foo/boot.dol", "mine");
		Journal j = craftedJournal(4, JournalKind::Update);
		j.ops.push_back({OpType::ParkDir, "apps/Foo", stagedOldPath(j.stagingDir, "apps/Foo"), false, false});
		j.ops.push_back({OpType::RenameDir, "apps/Foo", stagedRootPath(j.stagingDir, "apps/Foo"), false, false});
		j.ops.push_back(wmOp(j));
		Manifest m;
		m.slug = "foo";
		m.device = "sd";
		m.version = "1";
		ManifestDir md;
		md.path = "apps/Foo";
		md.files.push_back({"apps/Foo/boot.dol", true});
		m.dirs.push_back(md);
		validateManifest(m);
		CHECK(manifestSaveProblem(m).empty());
		CHECK(rec::writeRecord(d.sd + "/" + stagedManifestPath(j.stagingDir), manifestToJson(m), kMaxManifestBytes));
		CHECK(saveJournal(d.sd, j));
		CHECK(stagedManifestOk(d.sd, j, {}));
		CHECK(recover(rootsOf(d, false))[0].outcome == RecoveryOutcome::RolledBack); // the forward ParkDir was refused
		CHECK(get(d.sd + "/apps/Foo/boot.dol") == "mine");
		CHECK(!there(stagingAbs(d.sd, 4) + "/old") && !there(d.sd + "/apps/OpenShop/installed/foo.json"));
	}
}

// RestoreFile with the entry in neither tree is a failure (roll back), not a skip.
static void testRestoreNeitherSide() {
	const Scenario& sc = kScenarios[1];
	Dev d;
	sc.setup(d);
	Manifest old;
	bool have = false;
	loadOld(d, old, have);
	// cut after op 1 (parkDir, renameDir done; the restoreFile ops are still to run)
	CHECK(runCut([&] { CommitInput in = sc.input(d, &old); in.afterOp = hAfterOp; commit(in); }, 1, false));
	CHECK(there(stagingAbs(d.sd, 2) + "/old/apps/Foo/saves/a.sav"));
	CHECK(get(d.sd + "/apps/OpenShop/installed/.journal").find("\"step\":-1") != std::string::npos); // written once (review M4)
	std::error_code ec;
	fs::remove_all(stagingAbs(d.sd, 2) + "/old/apps/Foo/saves", ec); // the user's saves vanish from the parked tree
	const std::vector<RootRecovery> r = recover(rootsOf(d, false));
	CHECK(r[0].outcome == RecoveryOutcome::RolledBack);
	CHECK(get(d.sd + "/apps/Foo/boot.dol") == "old" && get(d.sd + "/apps/Foo/user.cfg") == "mine"); // the old app is back
	CHECK(!there(d.sd + "/apps/Foo/newfile"));
}

// M1: a journal that cannot be read, or a stat that fails, is never read as "no journal" or "no staging".
static void testReadErrorsAreNotAbsence() {
	const Scenario& sc = kScenarios[1];
	// the journal file is there but unreadable (EIO): the parked old app (and the saves) in staging survive
	{
		Dev d;
		sc.setup(d);
		Manifest old;
		bool have = false;
		loadOld(d, old, have);
		CHECK(runCut([&] { CommitInput in = sc.input(d, &old); in.afterOp = hAfterOp; commit(in); }, 1, false));
		const std::string jtext = get(d.sd + "/apps/OpenShop/installed/.journal");
		FileOps o;
		o.read = hReadJournalIo;
		const std::vector<RootRecovery> r = recover(rootsOf(d, false), o);
		CHECK(r[0].outcome == RecoveryOutcome::LeftAlone);
		CHECK(get(d.sd + "/apps/OpenShop/installed/.journal") == jtext);
		CHECK(get(stagingAbs(d.sd, 2) + "/old/apps/Foo/saves/a.sav") == "save"); // the save survives
		// a later start with a healthy card recovers it
		CHECK(recover(rootsOf(d, false))[0].outcome == RecoveryOutcome::RolledForward);
		CHECK(get(d.sd + "/apps/Foo/saves/a.sav") == "save");
	}
	// no journal on the card, but lstat of ".journal" (or of its backup) fails: not "no journal" -> LeftAlone, staging untouched
	for (int which = 0; which < 2; which++) {
		Dev d;
		put(stagingAbs(d.sd, 4) + "/old/apps/Foo/saves/a.sav", "save");
		FileOps o;
		o.stat = which == 0 ? hStatJournalErr : hStatJournalBakErr;
		CHECK(recover(rootsOf(d, false), o)[0].outcome == RecoveryOutcome::LeftAlone);
		CHECK(get(stagingAbs(d.sd, 4) + "/old/apps/Foo/saves/a.sav") == "save");
		CHECK(recover(rootsOf(d, false))[0].outcome == RecoveryOutcome::Nothing); // with a healthy card: nothing, and still nothing deleted
		CHECK(get(stagingAbs(d.sd, 4) + "/old/apps/Foo/saves/a.sav") == "save");
	}
	// the staging folder cannot be checked: Failed, journal and staging stay, nothing moves
	{
		Dev d;
		sc.setup(d);
		Manifest old;
		bool have = false;
		loadOld(d, old, have);
		CHECK(runCut([&] { CommitInput in = sc.input(d, &old); in.afterOp = hAfterOp; commit(in); }, 1, false));
		FileOps o;
		o.stat = hStatStagingErr;
		const std::vector<RootRecovery> r = recover(rootsOf(d, false), o);
		CHECK(r[0].outcome == RecoveryOutcome::Failed);
		CHECK(there(d.sd + "/apps/OpenShop/installed/.journal") && there(stagingAbs(d.sd, 2) + "/old/apps/Foo/saves/a.sav"));
		// Increment 7's guard: the Cleanup job after a Failed recovery leaves the dir the journal names alone (and removes the others)
		put(stagingAbs(d.sd, 99) + "/junk", "j");
		CleanStats cs;
		CHECK(cleanStaging(d.sd, d.removeRoots, guardFor(d, d.sd), {}, &cs));
		CHECK(cs.skippedJournal == 1 && cs.removed == 1 && !there(stagingAbs(d.sd, 99)));
		CHECK(there(d.sd + "/apps/OpenShop/installed/.journal") && there(stagingAbs(d.sd, 2) + "/old/apps/Foo/saves/a.sav"));
		CHECK(recover(rootsOf(d, false))[0].outcome == RecoveryOutcome::RolledForward);
	}
	// readFileCapped: only ENOENT/ENOTDIR is Missing
	{
		Dev d;
		std::string out;
		CHECK(readFileCapped(d.sd + "/nope", out, 10) == ReadStatus::Missing);
		put(d.sd + "/afile", "x");
		CHECK(readFileCapped(d.sd + "/afile/below", out, 10) == ReadStatus::Missing);                 // ENOTDIR
		CHECK(readFileCapped(d.sd + "/" + std::string(300, 'a'), out, 10) == ReadStatus::IoError);     // ENAMETOOLONG is not "absent"
		CHECK(presenceOf(d.sd + "/" + std::string(300, 'a')) == Presence::Error);
		CHECK(presenceOf(d.sd + "/nope") == Presence::Absent && presenceOf(d.sd + "/afile") == Presence::Present);
	}
}

// A flaky card during a commit: stat or listing that fails once or from some call on. The commit refuses (nothing changed), or fails and rolls
// back with everything intact, or leaves its journal; recovery then finishes or undoes it. Never a broken or half-lost tree.
static void testFlakyReads(const Scenario& sc) {
	references(sc);
	int refused = 0, rolled = 0, left = 0, okc = 0;
	for (int mode = 0; mode < 4; mode++) {
		for (int n = 0;; n++) {
			Dev d;
			sc.setup(d);
			g_count = 0; g_cut = n;
			Manifest old;
			bool have = false;
			if (sc.update) loadOld(d, old, have);
			CommitInput in = sc.input(d, have ? &old : nullptr);
			if (mode == 0) in.fileOps.stat = hStatOnce;
			else if (mode == 1) in.fileOps.stat = hStatFrom;
			else if (mode == 2) in.fileOps.list = hListOnce;
			else in.fileOps.list = hListFrom;
			const CommitResult r = commit(in);
			if (r.ok) {
				okc++;
				if (g_count <= n) break; // the failure point is past the end
				recover(rootsOf(d));
				if (classify(d.sd, "ok commit with a flaky read") != State::New) { CHECK(false); std::printf("    mode %d n %d\n", mode, n); }
				continue;
			}
			if (r.error == CommitError::OpFailed) {
				if (r.rolledBack) {
					rolled++;
					CHECK(classify(d.sd, "flaky read, in-process rollback") == State::Old);
				} else {
					left++;
				}
			} else {
				refused++; // the start-phase journal is still there; recovery drops it, then the device must equal OLD
			}
			recover(rootsOf(d));
			const State s = classify(d.sd, "flaky read + recover");
			CHECK(s != State::Broken);
			CHECK(settled(d, d.sd));
			if (r.error != CommitError::OpFailed) CHECK(s == State::Old);
			if (n > 4000) break;
		}
	}
	std::printf("  %s: flaky stat/list: %d refused at planning, %d rolled back in process, %d left to recovery, %d ok\n", sc.name, refused, rolled, left, okc);
	CHECK(refused > 0 && rolled + left > 0);
}

// Leftover staging (recovery never deletes it) must not confuse the next job.
static void testLeftoverStaging() {
	Dev d;
	// an interrupted install, recovered: its staging stays
	setupInstall(d);
	CHECK(runCut([&] { CommitInput in = installInput(d); in.afterOp = hAfterOp; commit(in); }, 2, false));
	CHECK(recover(rootsOf(d, false))[0].outcome == RecoveryOutcome::RolledForward);
	CHECK(there(stagingAbs(d.sd, 5) + "/manifest.json"));
	put(stagingAbs(d.sd, 6) + "/junk", "x"); // abandoned: no journal names it
	CHECK(freshJobId(d.sd, 5) == 7 && freshJobId(d.sd, 7) == 7 && freshJobId(d.sd, 1) == 1 && freshJobId(d.sd, 0) == 0);
	// another job commits on top of that, and its cleanup leaves the others alone
	writeStartJournal(d.sd, "sd", 8, JournalKind::Install, "baz");
	ZipPlan p = stage(d.sd, 8, {{"apps/Baz/boot.dol", "z"}});
	CHECK(commit(mkInput(d, d.sd, 8, "baz", false, p, "1")).ok);
	CHECK(there(stagingAbs(d.sd, 5) + "/manifest.json") && there(stagingAbs(d.sd, 6) + "/junk") && !there(stagingAbs(d.sd, 8)));
	// the same id again: refused cleanly (a staging dir that is not new), nothing is touched
	CHECK(get(d.sd + "/apps/Bar2/boot.dol") == "b");
	ZipPlan q = stage(d.sd, 5, {{"apps/Qux/boot.dol", "q"}});
	CommitResult r = commit(mkInput(d, d.sd, 5, "qux", false, q, "1"));
	CHECK(!r.ok && r.error == CommitError::BadInput);
	CHECK(!there(d.sd + "/apps/Qux"));
	// a failing stat in freshJobId is -1, not "free"
	FileOps o;
	o.stat = hStatStagingErr;
	CHECK(freshJobId(d.sd, 2, o) == -1);
}

// A plan near the op cap: no O(ops^2) work in the plan, the commit or the roll back. Prints the timings.
static void testNearCap() {
	using Clock = std::chrono::steady_clock;
	auto ms = [](Clock::time_point a) { return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - a).count(); };
	constexpr int kFiles = (int)kMaxJournalOps - 40;
	Dev d;
	put(d.sd + "/apps/foo/boot.dol", "old");
	mkd(d.sd + "/apps/foo/shared");
	for (int i = 0; i < kFiles; i++) spit(d.sd + "/apps/foo/shared/f" + std::to_string(i), "");
	auto input = [&](int64_t job) {
		ZipPlan p = stage(d.sd, job, {{"apps/foo/boot.dol", "new"}, {"apps/foo/shared/n", "n"}});
		return mkInput(d, d.sd, job, "foo", true, p, "2");
	};
	// a roll back from near the end (the 100th restore from the end fails): every restore is undone
	writeStartJournal(d.sd, "sd", 3, JournalKind::Update, "foo");
	CommitInput in = input(3);
	in.fileOps.rename = hFailOnce;
	g_count = 0; g_cut = kFiles - 100;
	auto t0 = Clock::now();
	const CommitResult r = commit(in);
	const long long failMs = ms(t0);
	if (!r.ok && r.error != CommitError::OpFailed) std::printf("    refused: %d %s\n", (int)r.error, r.detail.c_str());
	CHECK(!r.ok && r.error == CommitError::OpFailed && r.rolledBack);
	CHECK(kids(d.sd + "/apps/foo/shared") == (size_t)kFiles && get(d.sd + "/apps/foo/boot.dol") == "old");
	CHECK(!there(d.sd + "/apps/OpenShop/installed/.journal"));
	// and a whole commit
	writeStartJournal(d.sd, "sd", 4, JournalKind::Update, "foo");
	t0 = Clock::now();
	const CommitResult r2 = commit(input(4));
	const long long okMs = ms(t0);
	CHECK(r2.ok && r2.ops >= kFiles + 3 && r2.ops <= (int)kMaxJournalOps);
	CHECK(kids(d.sd + "/apps/foo/shared") == (size_t)kFiles + 1 && get(d.sd + "/apps/foo/boot.dol") == "new");
	// a cut at the end, then recovery (roll forward over ~20000 probed ops)
	Dev e;
	put(e.sd + "/apps/foo/boot.dol", "old");
	mkd(e.sd + "/apps/foo/shared");
	for (int i = 0; i < kFiles; i++) spit(e.sd + "/apps/foo/shared/f" + std::to_string(i), "");
	writeStartJournal(e.sd, "sd", 3, JournalKind::Update, "foo");
	ZipPlan p3 = stage(e.sd, 3, {{"apps/foo/boot.dol", "new"}, {"apps/foo/shared/n", "n"}});
	CommitInput cin = mkInput(e, e.sd, 3, "foo", true, p3, "2");
	cin.afterOp = hAfterOp;
	g_cut = kFiles / 2;
	CHECK(runCut([&] { commit(cin); }, kFiles / 2, false));
	t0 = Clock::now();
	CHECK(recover(rootsOf(e, false))[0].outcome == RecoveryOutcome::RolledForward);
	const long long recMs = ms(t0);
	CHECK(kids(e.sd + "/apps/foo/shared") == (size_t)kFiles + 1);
	std::printf("  near the op cap (%d restore ops): failed commit + rollback %lld ms, whole commit %lld ms, roll-forward recovery %lld ms\n", kFiles,
	            failMs, okMs, recMs);
	CHECK(failMs < 60000 && okMs < 60000 && recMs < 60000); // a quadratic walk would be minutes
}

static int installLocal(int argc, char** argv) {
	if (argc < 4) { std::printf("usage: commit_test --install-local <zip> <root> [slug] [--update]\n"); return 2; }
	const std::string zip = argv[2], root = argv[3];
	std::string slug = "local";
	bool update = false;
	for (int i = 4; i < argc; i++) {
		if (!std::strcmp(argv[i], "--update")) update = true;
		else slug = argv[i];
	}
	std::vector<std::string> removeRoots{root};
	ZipOptions zo;
	zo.removeRoots = removeRoots;
	ZipExtract zx(zo);
	if (!zx.open(zip)) { std::printf("open failed: %s\n", zx.failure().detail.c_str()); return 1; }
	writeStartJournal(root, "sd", 1, update ? JournalKind::Update : JournalKind::Install, slug);
	if (!zx.begin(stagingAbs(root, 1), freeSpace(root).value_or(0))) { std::printf("begin failed: %s\n", zx.failure().detail.c_str()); return 1; }
	if (zx.plan().needsConfirmation()) zx.confirmUndeclared();
	StepResult sr;
	do sr = zx.step(100); while (sr == StepResult::Running);
	if (sr != StepResult::Done) { std::printf("extract failed: %s\n", zx.failure().detail.c_str()); return 1; }
	CommitInput in;
	in.root = root;
	in.device = "sd";
	in.jobId = 1;
	in.slug = slug;
	in.version = "local";
	in.update = update;
	in.plan = zx.plan();
	in.removeRoots = removeRoots;
	Manifest old;
	if (update) {
		ManifestLoad ml = loadManifest(root, slug, "sd");
		if (ml.status == LoadStatus::Ok) { old = ml.manifest; in.oldManifest = &old; }
	}
	CommitResult r = commit(in);
	std::printf("commit: %s (%d ops) %s\n", r.ok ? "ok" : "FAILED", r.ops, r.detail.c_str());
	return r.ok ? 0 : 1;
}

int main(int argc, char** argv) {
	if (argc >= 2 && !std::strcmp(argv[1], "--install-local")) return installLocal(argc, argv);
	std::printf("carry-over\n");
	testCarryOver();
	std::printf("new install\n");
	testNewInstall();
	testExistsRefused();
	std::printf("update\n");
	testUpdateCarryOver();
	testUpdateUnmanifested();
	testCaseInPlace();
	testPlanRefusals();
	testRealZip();
	testKeptRename();
	testNameRefusedAndTooMany();
	testUpdateNoManifestCreatedFlags();
	testOutsideCase();
	std::printf("recovery\n");
	testRecoveryRoots();
	testCorruptAndForeign();
	testStagingCleanupStaysInside();
	testCleanGuard();
	testBrokenStagedManifestRollsBack();
	testCraftedJournals();
	testRestoreNeitherSide();
	testReadErrorsAreNotAbsence();
	testLeftoverStaging();
	testNearCap();
	std::printf("power cuts\n");
	for (const Scenario& sc : kScenarios) {
		testCrashEverywhere(sc, false);
		testCrashEverywhere(sc, true);
		testCrashEverywhere(sc, false, true);
		testCrashEverywhere(sc, true, true);
	}
	std::printf("failure injection\n");
	for (const Scenario& sc : kScenarios) testFailInjection(sc);
	std::printf("flaky reads\n");
	for (const Scenario& sc : kScenarios) testFlakyReads(sc);
	std::printf("recovery cut and re-run\n");
	testCrashDuringRecovery(kScenarios[0], 1);
	testCrashDuringRecovery(kScenarios[1], 5);
	testCrashDuringRecovery(kScenarios[0], 2, true); // the roll-back path, cut and re-run
	testCrashDuringRecovery(kScenarios[1], 7, true);
	testCrashDuringRecovery(kScenarios[4], 7);
	return checkReport("commit_test");
}
