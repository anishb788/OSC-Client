#include "core/FileUtil.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "core/Log.h"
#include "core/PathRoot.h"

namespace {
bool statExists(const char* path) {
	struct stat st;
	return ::stat(path, &st) == 0;
}

// std::remove is overloaded once <algorithm> is in, so it needs a plain function to take the address of.
int realRemove(const char* path) { return std::remove(path); }
int realMkdir(const char* path) { return ::mkdir(path, 0777); }
int realRmdir(const char* path) { return ::rmdir(path); }

// lstat so that a symlink is reported as a link and never followed (the Wii has none, so it is just stat there).
int realStat(const char* path, FileStat* out) {
	*out = FileStat{};
	struct stat st;
	if (::lstat(path, &st) != 0) return errno == ENOENT || errno == ENOTDIR ? 0 : -1;
	out->exists = true;
	out->isDir = S_ISDIR(st.st_mode);
	out->isLink = S_ISLNK(st.st_mode);
	out->size = (uint64_t)st.st_size;
	return 0;
}

bool realList(const std::string& dir, std::vector<std::string>& names) {
	names.clear();
	DIR* d = ::opendir(dir.c_str());
	if (!d) return false;
	while (const dirent* e = ::readdir(d)) {
		if (std::strcmp(e->d_name, ".") != 0 && std::strcmp(e->d_name, "..") != 0) names.emplace_back(e->d_name);
	}
	::closedir(d);
	return true;
}

struct Fs {
	int (*mkdir)(const char*);
	int (*rmdir)(const char*);
	int (*remove)(const char*);
	int (*rename)(const char*, const char*);
	int (*stat)(const char*, FileStat*);
	bool (*list)(const std::string&, std::vector<std::string>&);
	explicit Fs(const FileOps& o)
	    : mkdir(o.mkdir ? o.mkdir : &realMkdir), rmdir(o.rmdir ? o.rmdir : &realRmdir),
	      remove(o.remove ? o.remove : &realRemove), rename(o.rename ? o.rename : &std::rename),
	      stat(o.stat ? o.stat : &realStat), list(o.list ? o.list : &realList), custom(o.stat != nullptr) {}
	// True if something is there. Without an injected stat this follows symlinks (as makeDirs always did).
	bool present(const std::string& p) const {
		if (!custom) return statExists(p.c_str());
		FileStat st;
		return stat(p.c_str(), &st) == 0 && st.exists;
	}
	bool custom;
};
} // namespace

bool commitFile(const std::string& tmp, const std::string& to, const FileOps& ops, bool keepBackup) {
	int (*rename)(const char*, const char*) = ops.rename ? ops.rename : &std::rename;
	int (*remove)(const char*) = ops.remove ? ops.remove : &realRemove;
	auto exists = ops.exists ? ops.exists : statExists;
	const char* from = tmp.c_str();
	const char* dest = to.c_str();
	if (!keepBackup && rename(from, dest) == 0) return true;
	if (!exists(dest)) {
		// Nothing to park: the first rename failed for another reason (or, with keepBackup, this is a new file).
		return keepBackup ? rename(from, dest) == 0 : false;
	}
	// libfat cannot rename over an existing file. Park the old target instead of deleting it, so a failure on the
	// second rename still leaves the previous version in place.
	std::string bak = to + ".bak";
	remove(bak.c_str());
	if (rename(dest, bak.c_str()) != 0) return false;
	if (rename(from, dest) != 0) {
		rename(bak.c_str(), dest);
		return false;
	}
	if (!keepBackup) remove(bak.c_str());
	return true;
}

bool writeFileAtomic(const std::string& path, const std::string& data, const FileOps& ops, bool keepBackup) {
	const std::string tmp = path + ".tmp";
	std::FILE* f = std::fopen(tmp.c_str(), "wb");
	if (!f) return false;
	bool ok = data.empty() || std::fwrite(data.data(), 1, data.size(), f) == data.size();
	// fclose flushes, so a full card can surface here rather than in fwrite.
	if (std::fclose(f) != 0) ok = false;
	if (ok && commitFile(tmp, path, ops, keepBackup)) return true;
	// Through the injected remove too, so a test that "cuts power" at a call also leaves this tmp behind.
	(ops.remove ? ops.remove : &realRemove)(tmp.c_str());
	return false;
}

ReadStatus readFileCapped(const std::string& path, std::string& out, size_t maxBytes) {
	out.clear();
	std::FILE* f = std::fopen(path.c_str(), "rb");
	if (!f) return ReadStatus::Missing;
	ReadStatus rs = ReadStatus::IoError;
	if (std::fseek(f, 0, SEEK_END) == 0) {
		long size = std::ftell(f);
		if (size >= 0 && std::fseek(f, 0, SEEK_SET) == 0) {
			if ((unsigned long)size > maxBytes) {
				rs = ReadStatus::TooLarge;
			} else {
				out.resize((size_t)size);
				size_t got = size ? std::fread(&out[0], 1, (size_t)size, f) : 0;
				// A short read or an error flag means the file changed under us or the card failed.
				if (got == (size_t)size && !std::ferror(f)) rs = ReadStatus::Ok;
			}
		}
	}
	std::fclose(f);
	if (rs != ReadStatus::Ok) out.clear();
	return rs;
}

bool makeDirs(const std::string& dir, const FileOps& ops) {
	if (dir.empty()) return false;
	const Fs fs(ops);
	// Walk the path one component at a time; "sd:/apps/x" starts after the "sd:/" prefix, which is not creatable.
	size_t pos = dir.find(":/");
	pos = pos == std::string::npos ? 0 : pos + 2;
	while (pos < dir.size()) {
		size_t slash = dir.find('/', pos);
		std::string part = dir.substr(0, slash == std::string::npos ? dir.size() : slash);
		pos = slash == std::string::npos ? dir.size() : slash + 1;
		if (part.empty() || part.back() == '/') continue;
		if (fs.mkdir(part.c_str()) != 0 && !fs.present(part)) return false;
	}
	return fs.present(dir);
}

bool listDir(const std::string& dir, std::vector<std::string>& names, const FileOps& ops) {
	return ops.list ? ops.list(dir, names) : realList(dir, names);
}

bool isEmptyDir(const std::string& dir, const FileOps& ops) {
	const Fs fs(ops);
	FileStat st;
	if (fs.stat(dir.c_str(), &st) != 0 || !st.exists || !st.isDir) return false;
	std::vector<std::string> names;
	return fs.list(dir, names) && names.empty();
}

bool removeDirIfEmpty(const std::string& dir, const FileOps& ops) {
	if (!isEmptyDir(dir, ops)) return false;
	return Fs(ops).rmdir(dir.c_str()) == 0;
}

std::optional<std::string> findCaseInsensitive(const std::string& dir, const std::string& name, const FileOps& ops) {
	std::vector<std::string> names;
	if (!Fs(ops).list(dir, names)) return std::nullopt;
	std::sort(names.begin(), names.end());
	for (const std::string& n : names)
		if (n == name) return n;
	for (const std::string& n : names)
		if (pathroot::equalsNoCase(n, name)) return n;
	return std::nullopt;
}

std::optional<uint64_t> freeSpace(const std::string& root) {
	if (root.empty()) return std::nullopt;
	// "sd:" needs its slash; a trailing slash is harmless elsewhere.
	const std::string p = root.back() == '/' ? root : root + "/";
	struct statvfs sv;
	if (::statvfs(p.c_str(), &sv) != 0) return std::nullopt;
	const uint64_t unit = sv.f_frsize ? sv.f_frsize : sv.f_bsize;
	return (uint64_t)sv.f_bavail * unit;
}

bool fsyncFile(std::FILE* f) {
	if (!f) return false;
	if (std::fflush(f) != 0) return false;
	const int fd = ::fileno(f);
	if (fd < 0) return false;
	if (::fsync(fd) == 0) return true;
	if (errno == ENOSYS || errno == ENOTSUP) {
		static bool logged = false;
		if (!logged) {
			logged = true;
			LOG("fsync is not supported here (errno %d); continuing without it", errno);
		}
		return true;
	}
	return false;
}

// ---------------------------------------------------------------------------------------------------------------------
namespace {
using pathroot::equalsNoCase;
using pathroot::Parts;

// Segment check on the raw text, before any normalisation hides a "..".
bool rawSegmentsOk(const std::string& p) {
	if (p.empty() || p.find('\0') != std::string::npos || p.find('\\') != std::string::npos) return false;
	std::string rest = p;
	bool device = false;
	const size_t colon = p.find(':'), slash = p.find('/');
	if (colon != std::string::npos && colon > 0 && (slash == std::string::npos || colon < slash)) {
		rest = p.substr(colon + 1);
		device = true;
		// "sd:foo" would be read as "sd:/foo" by the guards but resolved against the device's cwd by libfat.
		if (!rest.empty() && rest[0] != '/') return false;
	}
	std::vector<std::string> segs;
	pathroot::splitInto(rest, segs);
	for (size_t i = 0; i < segs.size(); i++) {
		if (segs[i] == "..") return false;
		if (segs[i] == "." && !(i == 0 && !device && segs.size() > 1)) return false;
	}
	return true;
}

std::string currentDir() {
	char buf[1024];
	return ::getcwd(buf, sizeof buf) ? std::string(buf) : std::string();
}

RemoveResult refuse(const char* why) {
	RemoveResult r;
	r.refused = true;
	r.error = std::string("refused: ") + why;
	LOG("removeTree %s", r.error.c_str());
	return r;
}

bool compEq(const Parts& p, size_t i, const char* name) { return i < p.comps.size() && equalsNoCase(p.comps[i], name); }

// FAT names the guards cannot match by text: a trailing dot or space is dropped by FAT, and "~<digit>" is the 8.3 alias
// of a long name ("OPENSH~1" is "OpenShop"). Callers never produce these, so they are refused on device paths.
bool fatAliasName(const std::string& c) {
	if (c.empty() || c.back() == '.' || c.back() == ' ') return true;
	for (size_t i = 0; i + 1 < c.size(); i++)
		if (c[i] == '~' && c[i + 1] >= '0' && c[i + 1] <= '9') return true;
	return false;
}

// One bad entry refuses the whole call. Names are taken from the raw text (the cwd of a relative root is not ours to judge).
bool rootsOk(const std::vector<std::string>& roots, std::string& why) {
	for (const std::string& r : roots) {
		if (r.empty()) { why = "empty root"; return false; }
		if (!rawSegmentsOk(r)) { why = "bad root"; return false; }
		std::string rest = r;
		const size_t colon = r.find(':'), slash = r.find('/');
		if (colon != std::string::npos && colon > 0 && (slash == std::string::npos || colon < slash)) rest = r.substr(colon + 1);
		std::vector<std::string> segs;
		pathroot::splitInto(rest, segs);
		size_t real = 0;
		for (const std::string& c : segs) {
			if (c.empty() || c == ".") continue;
			real++;
			if (equalsNoCase(c, "apps") || equalsNoCase(c, "OpenShop")) { why = "root contains apps or OpenShop"; return false; }
		}
		if (real == 0 && rest == r) { why = "root is the filesystem root"; return false; } // "/", "./", "."
	}
	return true;
}

struct Remover {
	const Fs& fs;
	RemoveResult& res;
	void fail(const std::string& what, const std::string& path) {
		res.errors++;
		if (res.error.empty()) res.error = what + ": " + path;
	}
	bool badName(const std::string& n) const {
		return n.empty() || n == "." || n == ".." || n.find('/') != std::string::npos || n.find('\\') != std::string::npos;
	}
	// The path-based walk: injected FileOps, and the Wii.
	void walk(const std::string& path, int depth) {
		FileStat st;
		if (fs.stat(path.c_str(), &st) != 0) { fail("stat failed", path); return; }
		if (!st.exists) return; // vanished under us
		if (!st.isDir) { // file or symlink: unlink, never follow
			if (fs.remove(path.c_str()) == 0) res.files++;
			else fail("remove failed", path);
			return;
		}
		if (depth > kRemoveMaxDepth) { fail("too deep", path); return; }
		std::vector<std::string> names;
		if (!fs.list(path, names)) { fail("cannot list", path); return; }
		std::sort(names.begin(), names.end());
		for (const std::string& n : names) {
			if (badName(n)) {
				fail("bad entry name", path);
				continue;
			}
			walk(path + "/" + n, depth + 1);
		}
		if (fs.rmdir(path.c_str()) == 0) res.dirs++;
		else fail("rmdir failed", path);
	}
};

// The descriptor walk is PC only: FAT has no symlinks to swap in, and libfat has no openat/fstatat/unlinkat, so the Wii
// keeps the path walk above.
#ifndef __wii__

// Removes the contents of the directory `dirfd` (which this takes over and closes), not the directory itself.
void fdWalkContents(Remover& rm, int dirfd, const std::string& path, int depth, void (*hook)(const std::string&)) {
	std::vector<std::string> names;
	bool listed = false;
	if (const int dupFd = ::dup(dirfd); dupFd >= 0) {
		if (DIR* d = ::fdopendir(dupFd)) {
			listed = true;
			while (const dirent* e = ::readdir(d))
				if (std::strcmp(e->d_name, ".") != 0 && std::strcmp(e->d_name, "..") != 0) names.emplace_back(e->d_name);
			::closedir(d); // closes dup
		} else {
			::close(dupFd);
		}
	}
	if (!listed) {
		rm.fail("cannot list", path);
		::close(dirfd);
		return;
	}
	std::sort(names.begin(), names.end());
	if (hook) hook(path);
	for (const std::string& n : names) {
		if (rm.badName(n)) { rm.fail("bad entry name", path); continue; }
		const std::string child = path + "/" + n;
		struct stat st;
		if (::fstatat(dirfd, n.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
			if (errno != ENOENT) rm.fail("stat failed", child); // ENOENT: vanished under us
			continue;
		}
		if (!S_ISDIR(st.st_mode)) { // file or symlink: unlink, never follow
			if (::unlinkat(dirfd, n.c_str(), 0) == 0) rm.res.files++;
			else rm.fail("remove failed", child);
			continue;
		}
		if (depth + 1 > kRemoveMaxDepth) { rm.fail("too deep", child); continue; }
		const int sub = ::openat(dirfd, n.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
		if (sub < 0) { rm.fail("cannot open", child); continue; } // includes "swapped for a symlink" (ELOOP)
		fdWalkContents(rm, sub, child, depth + 1, hook);
		if (::unlinkat(dirfd, n.c_str(), AT_REMOVEDIR) == 0) rm.res.dirs++;
		else rm.fail("rmdir failed", child);
	}
	::close(dirfd);
}

// `real` is a plain (non-device) path, `below` the number of its trailing components that lie under the trusted root.
RemoveResult fdRemove(const std::string& real, size_t below, void (*hook)(const std::string&)) {
	RemoveResult res;
	const Fs realFs{FileOps{}};
	Remover rm{realFs, res};
	std::vector<std::string> raw, comps;
	pathroot::splitInto(real, raw);
	for (const std::string& c : raw)
		if (!c.empty() && c != ".") comps.push_back(c);
	if (below > comps.size()) below = comps.size();
	if (below == 0) { res.errors = 1; res.error = "no component to remove: " + real; return res; }
	std::string prefix = real[0] == '/' ? "/" : "";
	for (size_t i = 0; i + below < comps.size(); i++) prefix += (prefix.empty() || prefix == "/" ? "" : "/") + comps[i];
	if (prefix.empty()) prefix = ".";
	// The root and what is above it are trusted (the root may be a symlink); everything below is opened O_NOFOLLOW.
	int fd = ::open(prefix.c_str(), O_RDONLY | O_DIRECTORY);
	if (fd < 0) {
		if (errno == ENOENT || errno == ENOTDIR) { res.ok = true; return res; } // root not there: nothing to remove
		res.errors = 1;
		res.error = "stat failed: " + real;
		return res;
	}
	std::string at = prefix;
	for (size_t i = comps.size() - below; i + 1 < comps.size(); i++) {
		at += (at.back() == '/' ? "" : "/") + comps[i];
		const int next = ::openat(fd, comps[i].c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
		const int err = errno;
		::close(fd);
		if (next < 0) {
			if (err == ENOENT) { res.ok = true; return res; }
			res.errors = 1;
			res.error = "cannot open: " + at;
			return res;
		}
		fd = next;
	}
	const std::string& name = comps.back();
	const std::string full = at + (at.back() == '/' ? "" : "/") + name;
	struct stat st;
	if (::fstatat(fd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
		const bool gone = errno == ENOENT;
		::close(fd);
		if (gone) res.ok = true;
		else { res.errors = 1; res.error = "stat failed: " + full; }
		return res;
	}
	res.existed = true;
	if (!S_ISDIR(st.st_mode)) {
		if (::unlinkat(fd, name.c_str(), 0) == 0) res.files++;
		else rm.fail("remove failed", full);
	} else {
		const int top = ::openat(fd, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
		if (top < 0) {
			rm.fail("cannot open", full);
		} else {
			fdWalkContents(rm, top, full, 0, hook);
			if (::unlinkat(fd, name.c_str(), AT_REMOVEDIR) == 0) res.dirs++;
			else rm.fail("rmdir failed", full);
		}
	}
	::close(fd);
	res.ok = res.errors == 0;
	return res;
}
#endif
} // namespace

RemoveResult removeTree(const std::string& path, const std::string& mustBeUnder, const std::vector<std::string>& roots,
                        const FileOps& ops) {
	if (path.empty()) return refuse("empty path");
	if (mustBeUnder.empty()) return refuse("empty mustBeUnder");
	if (roots.empty()) return refuse("no roots given");
	if (!rawSegmentsOk(path)) return refuse("bad segment in path");
	if (!rawSegmentsOk(mustBeUnder)) return refuse("bad segment in mustBeUnder");
	std::string why;
	if (!rootsOk(roots, why)) return refuse(why.c_str());

	const std::string cwd = currentDir();
	const Parts pp = pathroot::parse(path, cwd), mp = pathroot::parse(mustBeUnder, cwd);
	if (pp.comps.empty()) return refuse("path is a root");
	const bool icase = !pp.device.empty(); // FAT
	if (icase) {
		for (const std::string& c : pp.comps) if (fatAliasName(c)) return refuse("FAT alias or trailing dot/space in path");
		for (const std::string& c : mp.comps) if (fatAliasName(c)) return refuse("FAT alias or trailing dot/space in mustBeUnder");
	}
	if (!pathroot::under(mp, pp, icase) || pp.comps.size() <= mp.comps.size()) return refuse("not strictly under mustBeUnder");

	// The longest root that holds the path.
	Parts root;
	bool found = false;
	for (const std::string& r : roots) {
		Parts rp = pathroot::parse(r, cwd);
		if (!pathroot::under(rp, pp, icase)) continue;
		if (!found || rp.comps.size() > root.comps.size()) { root = rp; found = true; }
	}
	if (!found) return refuse("not on a known root");
	if (!pathroot::under(root, mp, icase) || mp.comps.size() <= root.comps.size()) return refuse("mustBeUnder is a root or off the root");

	const size_t n = root.comps.size(); // path's components from here on are relative to the root
	if (pp.comps.size() == n) return refuse("path is a root");
	if (compEq(pp, n, "apps")) {
		if (pp.comps.size() == n + 1) return refuse("path is <root>/apps");
		if (compEq(pp, n + 1, "OpenShop")) {
			if (pp.comps.size() == n + 2) return refuse("path is <root>/apps/OpenShop");
			// Inside OpenShop's own folder: only staging cleanup, so mustBeUnder is at or below apps/OpenShop/staging.
			if (!(compEq(mp, n, "apps") && compEq(mp, n + 1, "OpenShop") && compEq(mp, n + 2, "staging")))
				return refuse("inside apps/OpenShop without a mustBeUnder at or below staging");
		}
	}

	// The string that runs is the normalised one for a device path ("sd:foo" and "sd:/foo" cannot differ), and the given
	// one (minus trailing slashes) for a plain path.
	std::string real;
	if (!pp.device.empty()) {
		real = pp.device;
		for (const std::string& c : pp.comps) real += "/" + c;
	} else {
		real = path;
		while (real.size() > 1 && real.back() == '/') real.pop_back();
	}

	RemoveResult res;
#ifndef __wii__
	if (pp.device.empty() && !ops.stat && !ops.list && !ops.remove && !ops.rmdir) {
		res = fdRemove(real, pp.comps.size() - n, ops.walkHook);
		return res;
	}
#endif
	const Fs fs(ops);
	FileStat st;
	if (fs.stat(real.c_str(), &st) != 0) {
		res.error = "stat failed: " + real;
		res.errors = 1;
		return res;
	}
	if (!st.exists) { res.ok = true; return res; }
	res.existed = true;
	Remover{fs, res}.walk(real, 0);
	res.ok = res.errors == 0;
	return res;
}

// ---------------------------------------------------------------------------------------------------------------------
namespace {
// The parent directory text of `p`, or "" when there is none. A device root ("sd:") is returned as such.
std::string parentOf(const std::string& p) {
	const size_t s = p.find_last_of('/');
	return s == std::string::npos || s == 0 ? std::string() : p.substr(0, s);
}

// Makes the parents of `p`. A device root ("sd:") exists by definition; libfat may refuse mkdir/stat on it.
bool ensureParent(const std::string& p, const FileOps& ops) {
	const std::string parent = parentOf(p);
	if (parent.empty() || parent.back() == ':') return true;
	return makeDirs(parent, ops);
}

bool isRealDir(const Fs& fs, const std::string& p) {
	FileStat st;
	return fs.stat(p.c_str(), &st) == 0 && st.exists && st.isDir && !st.isLink;
}
} // namespace

bool parkDir(const std::string& target, const std::string& park, const FileOps& ops) {
	if (target.empty() || park.empty()) return false;
	const Fs fs(ops);
	if (!isRealDir(fs, target)) return false;
	// A park inside the target would be moved into itself.
	std::string t = target;
	while (t.size() > 1 && t.back() == '/') t.pop_back();
	if (park == t || park.compare(0, t.size() + 1, t + "/") == 0) return false;
	if (fs.present(park)) return false; // never rename over a leftover: libfat can't, and it may be someone's data
	if (!ensureParent(park, ops)) return false;
	return fs.rename(target.c_str(), park.c_str()) == 0;
}

bool renameStaged(const std::string& staged, const std::string& target, const FileOps& ops) {
	if (staged.empty() || target.empty()) return false;
	const Fs fs(ops);
	if (!isRealDir(fs, staged)) return false;
	if (fs.present(target)) return false;
	if (!ensureParent(target, ops)) return false;
	return fs.rename(staged.c_str(), target.c_str()) == 0;
}

bool restoreParked(const std::string& park, const std::string& target, const FileOps& ops) {
	if (park.empty() || target.empty()) return false;
	const Fs fs(ops);
	if (!isRealDir(fs, park)) return false;
	if (fs.present(target)) return false;
	return fs.rename(park.c_str(), target.c_str()) == 0;
}

CommitDirResult commitDir(const std::string& staged, const std::string& target, const std::string& park,
                          const FileOps& ops) {
	const Fs fs(ops);
	CommitDirResult r;
	if (staged.empty() || target.empty() || park.empty()) return r;
	if (!isRealDir(fs, staged)) return r;
	if (!fs.present(target)) {
		if (!renameStaged(staged, target, ops)) return r;
		r.ok = true;
		r.step = CommitDirStep::Renamed;
		return r;
	}
	if (!parkDir(target, park, ops)) return r;
	if (!renameStaged(staged, target, ops)) {
		r.step = restoreParked(park, target, ops) ? CommitDirStep::RolledBack : CommitDirStep::Stuck;
		return r;
	}
	r.ok = true;
	r.step = CommitDirStep::Replaced;
	return r;
}
