// SPDX-License-Identifier: GPL-3.0-or-later
#include "install/Manifest.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <set>

#include "core/Log.h"
#include "core/NameRules.h"
#include "install/RecordIo.h"
#include "install/Rules.h"

namespace install {
namespace {
constexpr size_t kMaxKeptPathBytes = 1024; // longer strings make the file corrupt; shorter bad ones are kept flagged invalid
constexpr int kMaxNesting = 8;

// Slugs are always ASCII: foldKey. Paths (which can hold any valid UTF-8 name): foldKeyUnicode.
bool slugEq(std::string_view a, std::string_view b) { return foldKey(a) == foldKey(b); }
bool pathEq(std::string_view a, std::string_view b) { return foldKeyUnicode(a) == foldKeyUnicode(b); }

std::string lastSegment(std::string_view p) {
	const size_t s = p.rfind('/');
	return std::string(s == std::string_view::npos ? p : p.substr(s + 1));
}

EntryName plainName(std::string_view p) {
	EntryName e = validateEntryName(p);
	if (e.ok && (e.isDir || e.relPath != p)) e.ok = false;
	return e;
}

bool validHex32(const std::string& s) {
	if (s.size() != 32) return false;
	for (char c : s) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
	return true;
}

// `fp` (folded path) lies strictly below `foldedDirSlash` (folded dir plus one '/').
bool underFolded(const std::string& fp, const std::string& foldedDirSlash) {
	return fp.size() > foldedDirSlash.size() && fp.compare(0, foldedDirSlash.size(), foldedDirSlash) == 0;
}
} // namespace

bool pathStrictlyUnder(std::string_view p, std::string_view dir) {
	if (dir.empty()) return false;
	std::string fd = foldKeyUnicode(dir);
	fd += '/';
	return underFolded(foldKeyUnicode(p), fd);
}

bool isAppDirPath(std::string_view p) {
	const EntryName e = plainName(p);
	return e.ok && e.segments.size() == 2 && isAppsRoot(e.segments[0]) && !isOwnRoot(p) && !isJunkRoot(e.segments[1]);
}

bool isOutsidePath(std::string_view p) {
	const EntryName e = plainName(p);
	return e.ok && !isAppsRoot(e.segments[0]) && !isSystemRoot(e.segments[0]) && !isJunkRoot(e.segments[0]);
}

bool isTargetPath(std::string_view p) {
	const EntryName e = plainName(p);
	if (!e.ok) return false;
	if (isAppsRoot(e.segments[0])) return e.segments.size() >= 3 && !isOwnRoot(p) && !isJunkRoot(e.segments[1]);
	return !isSystemRoot(e.segments[0]) && !isJunkRoot(e.segments[0]);
}

// plainName() guarantees a normalised path, so a '/' is there exactly when there are two or more segments.
bool isOutsideFilePath(std::string_view p) { return isOutsidePath(p) && p.find('/') != std::string_view::npos; }
bool isTargetFilePath(std::string_view p) { return isTargetPath(p) && p.find('/') != std::string_view::npos; }

void validateManifest(Manifest& m) {
	for (ManifestDir& d : m.dirs) {
		d.valid = isAppDirPath(d.path);
		std::string fd;
		if (d.valid) fd = foldKeyUnicode(d.path) + "/"; // once per dir, not once per file
		for (ManifestPath& f : d.files) f.valid = d.valid && isTargetFilePath(f.path) && underFolded(foldKeyUnicode(f.path), fd);
		for (ManifestPath& f : d.createdSubdirs) f.valid = d.valid && isTargetPath(f.path) && underFolded(foldKeyUnicode(f.path), fd);
	}
	for (ManifestFile& f : m.files) f.valid = isOutsideFilePath(f.path);
	for (ManifestPath& p : m.createdDirs) p.valid = isOutsidePath(p.path);
}

size_t manifestPathCount(const Manifest& m) {
	size_t n = m.files.size() + m.createdDirs.size();
	for (const ManifestDir& d : m.dirs) n += d.files.size() + d.createdSubdirs.size();
	return n;
}

// ---- JSON ---------------------------------------------------------------------------------------------------------------
std::string manifestToJson(const Manifest& m) {
	using rec::Json;
	bool badUtf8 = false;
	auto str = [&](const std::string& s) {
		json_t* v = rec::jsonStr(s);
		if (!v) badUtf8 = true;
		return v;
	};
	Json root(json_object());
	json_object_set_new(root.get(), "formatVersion", json_integer(kManifestFormat));
	json_object_set_new(root.get(), "slug", str(m.slug));
	json_object_set_new(root.get(), "device", str(m.device));
	json_object_set_new(root.get(), "version", str(m.version));
	json_object_set_new(root.get(), "installedAt", json_integer(m.installedAt));
	json_object_set_new(root.get(), "archiveMd5", str(m.archiveMd5));
	json_t* dirs = json_array();
	for (const ManifestDir& d : m.dirs) {
		json_t* o = json_object();
		json_object_set_new(o, "path", str(d.path));
		json_object_set_new(o, "created", json_boolean(d.created));
		json_object_set_new(o, "adopted", json_boolean(d.adopted));
		json_t* files = json_array();
		for (const ManifestPath& f : d.files) json_array_append_new(files, str(f.path));
		json_object_set_new(o, "files", files);
		json_t* subs = json_array();
		for (const ManifestPath& f : d.createdSubdirs) json_array_append_new(subs, str(f.path));
		json_object_set_new(o, "createdSubdirs", subs);
		json_array_append_new(dirs, o);
	}
	json_object_set_new(root.get(), "dirs", dirs);
	json_t* files = json_array();
	for (const ManifestFile& f : m.files) {
		json_t* o = json_object();
		json_object_set_new(o, "path", str(f.path));
		json_object_set_new(o, "preexisting", json_boolean(f.preexisting));
		json_array_append_new(files, o);
	}
	json_object_set_new(root.get(), "files", files);
	json_t* cd = json_array();
	for (const ManifestPath& p : m.createdDirs) json_array_append_new(cd, str(p.path));
	json_object_set_new(root.get(), "createdDirs", cd);
	if (badUtf8) return {};
	// ENSURE_ASCII: a stray byte can never make the file unreadable by our own UTF-8 check.
	char* dump = json_dumps(root.get(), JSON_COMPACT | JSON_ENSURE_ASCII);
	std::string out = dump ? dump : "";
	std::free(dump);
	return out;
}

namespace {
bool readPathList(json_t* arr, size_t& budget, std::vector<ManifestPath>& out) {
	const size_t n = json_array_size(arr);
	if (n > budget) return false;
	budget -= n;
	out.reserve(n);
	for (size_t i = 0; i < n; i++) {
		ManifestPath p;
		if (!rec::asString(json_array_get(arr, i), kMaxKeptPathBytes, p.path)) return false;
		out.push_back(std::move(p));
	}
	return true;
}
} // namespace

ManifestParse parseManifest(std::string_view text) {
	ManifestParse r;
	auto fail = [&](const char* why) {
		r.ok = false;
		r.manifest = Manifest{};
		r.error = why;
		return r;
	};
	if (text.size() > kMaxManifestBytes) return fail("too large");
	std::string err;
	rec::Json doc = rec::parseObject(text, kMaxNesting, kMaxManifestValues, err);
	if (!doc) { r.error = err; return r; }
	json_t* o = doc.get();
	Manifest& m = r.manifest;

	int64_t fv = 0;
	if (!rec::getInt(o, "formatVersion", fv)) return fail("formatVersion missing or not a number");
	if (fv < 1 || fv > kManifestFormat) return fail("unsupported formatVersion");
	if (!rec::getString(o, "slug", kMaxSlugLength, m.slug) || !isValidSlug(m.slug)) return fail("bad slug");
	if (!rec::getString(o, "device", 8, m.device) || (m.device != "sd" && m.device != "usb")) return fail("bad device");
	if (json_object_get(o, "version") && !rec::getString(o, "version", kMaxManifestVersionBytes, m.version)) return fail("bad version");
	if (json_object_get(o, "installedAt") && !rec::getInt(o, "installedAt", m.installedAt)) return fail("bad installedAt");
	if (m.installedAt < 0) return fail("bad installedAt");
	if (json_object_get(o, "archiveMd5")) {
		if (!rec::getString(o, "archiveMd5", 32, m.archiveMd5)) return fail("bad archiveMd5");
		std::transform(m.archiveMd5.begin(), m.archiveMd5.end(), m.archiveMd5.begin(),
		               [](char c) { return c >= 'A' && c <= 'F' ? (char)(c + 32) : c; });
		if (!m.archiveMd5.empty() && !validHex32(m.archiveMd5)) return fail("bad archiveMd5");
	}

	size_t budget = kMaxManifestPaths;
	json_t* dirs = rec::getArray(o, "dirs", kMaxManifestDirs);
	if (!dirs) return fail("bad or missing dirs");
	if (json_array_size(dirs) == 0) return fail("no dirs");
	std::set<std::string> dirKeys;
	for (size_t i = 0; i < json_array_size(dirs); i++) {
		json_t* d = json_array_get(dirs, i);
		if (!json_is_object(d)) return fail("dirs entry is not an object");
		ManifestDir md;
		if (!rec::getString(d, "path", kMaxKeptPathBytes, md.path)) return fail("dirs entry without path");
		if (!dirKeys.insert(foldKeyUnicode(md.path)).second) return fail("duplicate dirs entry");
		// Flags: anything but a boolean reads as the safe value (false).
		rec::getBool(d, "created", md.created);
		rec::getBool(d, "adopted", md.adopted);
		if (json_t* a = json_object_get(d, "files")) {
			if (!json_is_array(a) || !readPathList(a, budget, md.files)) return fail("bad dirs.files");
		}
		if (json_t* a = json_object_get(d, "createdSubdirs")) {
			if (!json_is_array(a) || !readPathList(a, budget, md.createdSubdirs)) return fail("bad dirs.createdSubdirs");
		}
		m.dirs.push_back(std::move(md));
	}
	if (json_t* a = json_object_get(o, "files")) {
		if (!json_is_array(a)) return fail("bad files");
		const size_t n = json_array_size(a);
		if (n > budget) return fail("too many paths");
		budget -= n;
		m.files.reserve(n);
		for (size_t i = 0; i < n; i++) {
			json_t* f = json_array_get(a, i);
			ManifestFile mf;
			mf.preexisting = true; // a missing or malformed flag must never make Delete remove something we can't vouch for
			if (!json_is_object(f) || !rec::getString(f, "path", kMaxKeptPathBytes, mf.path)) return fail("bad files entry");
			rec::getBool(f, "preexisting", mf.preexisting);
			m.files.push_back(std::move(mf));
		}
	}
	if (json_t* a = json_object_get(o, "createdDirs")) {
		if (!json_is_array(a) || !readPathList(a, budget, m.createdDirs)) return fail("bad createdDirs");
	}
	validateManifest(m);
	r.ok = true;
	return r;
}

// ---- files --------------------------------------------------------------------------------------------------------------
std::string manifestRelPath(const std::string& slug) { return "apps/OpenShop/installed/" + slug + ".json"; }
std::string manifestPath(const std::string& root, const std::string& slug) { return root + "/" + manifestRelPath(slug); }

namespace {
// Builds the JSON and checks everything saveManifest requires. `json` is set when the text is ready to write.
std::string saveCheck(const Manifest& m, std::string& json) {
	if (!isValidSlug(m.slug)) return "bad slug";
	if (m.device != "sd" && m.device != "usb") return "bad device";
	if (m.version.size() > kMaxManifestVersionBytes) return "version too long";
	if (m.dirs.size() > kMaxManifestDirs) return "too many dirs";
	if (m.installedAt < 0) return "bad installedAt";
	if (!m.archiveMd5.empty() && !validHex32(m.archiveMd5)) return "bad archiveMd5";
	const size_t n = manifestPathCount(m);
	if (n > kMaxManifestPaths) return std::to_string(n) + " paths is over the cap";
	json = manifestToJson(m);
	if (json.empty()) return "not serialisable (invalid UTF-8)";
	if (json.size() > kMaxManifestBytes) return std::to_string(json.size()) + " bytes is over the cap";
	const ManifestParse back = parseManifest(json);
	if (!back.ok) return "would not load back: " + back.error;
	return {};
}
} // namespace

std::string manifestSaveProblem(const Manifest& m) {
	std::string json;
	return saveCheck(m, json);
}

bool saveManifest(const std::string& root, const Manifest& m, const FileOps& ops) {
	std::string json;
	const std::string bad = saveCheck(m, json);
	if (!bad.empty()) {
		LOG("manifest %s: refused to save: %s", m.slug.c_str(), bad.c_str());
		return false;
	}
	return rec::writeRecord(manifestPath(root, m.slug), json, kMaxManifestBytes, ops);
}

ManifestLoad loadManifest(const std::string& root, const std::string& slug, std::string_view device, const FileOps& ops) {
	ManifestLoad r;
	if (!isValidSlug(slug)) { r.status = LoadStatus::Missing; return r; }
	const rec::Read rd = rec::readRecord(manifestPath(root, slug), kMaxManifestBytes, ops);
	if (rd.result == rec::ReadResult::Missing) return r;
	if (rd.result != rec::ReadResult::Ok) {
		r.status = LoadStatus::Corrupt;
		r.error = rd.result == rec::ReadResult::TooLarge ? "too large" : "unreadable";
		return r;
	}
	r.bytes = rd.text.size();
	ManifestParse p = parseManifest(rd.text);
	if (p.ok && !slugEq(p.manifest.slug, slug)) { p.ok = false; p.error = "slug does not match the file name"; }
	if (p.ok && p.manifest.device != device) { p.ok = false; p.error = "written for another device"; }
	if (!p.ok) {
		r.status = LoadStatus::Corrupt;
		r.error = p.error;
		LOG("manifest %s is corrupt: %s", slug.c_str(), p.error.c_str());
		return r;
	}
	r.status = LoadStatus::Ok;
	r.manifest = std::move(p.manifest);
	r.fromBackup = rd.fromBackup;
	return r;
}

bool removeManifest(const std::string& root, const std::string& slug, const FileOps& ops) {
	if (!isValidSlug(slug)) return false;
	return rec::removeRecord(manifestPath(root, slug), ops);
}

ManifestList listManifests(const std::string& root, std::string_view device, const FileOps& ops) {
	ManifestList out;
	std::vector<std::string> names;
	if (!listDir(rec::installedDir(root), names, ops)) return out;
	out.listed = true;
	std::sort(names.begin(), names.end());
	std::set<std::string> have; // folded slugs seen as "<slug>.json"
	std::vector<std::string> slugs;
	auto ends = [](const std::string& s, const char* suf) {
		const size_t n = std::char_traits<char>::length(suf);
		return s.size() > n && foldKey(s.substr(s.size() - n)) == suf;
	};
	for (const std::string& n : names) {
		if (ends(n, ".json")) {
			const std::string slug = n.substr(0, n.size() - 5);
			if (!isValidSlug(slug)) continue;
			if (have.insert(foldKey(slug)).second) slugs.push_back(slug);
		}
	}
	for (const std::string& n : names) {
		if (ends(n, ".json.bak")) {
			const std::string slug = n.substr(0, n.size() - 9);
			if (isValidSlug(slug) && have.insert(foldKey(slug)).second) slugs.push_back(slug);
		}
	}
	size_t totalPaths = 0, totalBytes = 0;
	for (const std::string& slug : slugs) {
		if (out.manifests.size() + out.corrupt.size() >= kMaxManifestsPerDevice || totalBytes >= kMaxListedBytes) {
			out.truncated = true;
			break;
		}
		ManifestLoad l = loadManifest(root, slug, device, ops);
		totalBytes += l.bytes;
		if (l.status == LoadStatus::Ok) {
			const size_t n = manifestPathCount(l.manifest);
			if (totalPaths + n > kMaxListedPaths) { out.truncated = true; break; }
			totalPaths += n;
			out.manifests.push_back(std::move(l.manifest));
		} else if (l.status == LoadStatus::Corrupt) {
			out.corrupt.push_back(slug);
		}
	}
	return out;
}

// ---- queries ------------------------------------------------------------------------------------------------------------
ManifestIndex buildManifestIndex(const std::vector<Manifest>& all) {
	ManifestIndex ix;
	auto add = [](std::unordered_map<std::string, std::vector<std::string>>& map, const std::string& path, const std::string& slug) {
		std::vector<std::string>& v = map[foldKeyUnicode(path)];
		if (v.empty() || v.back() != slug) v.push_back(slug); // manifests are added one at a time, so only the last can repeat
	};
	for (const Manifest& m : all) {
		for (const ManifestDir& d : m.dirs) {
			if (d.valid) add(ix.dirs, d.path, m.slug);
			for (const ManifestPath& p : d.files) if (p.valid) add(ix.paths, p.path, m.slug);
			for (const ManifestPath& p : d.createdSubdirs) if (p.valid) add(ix.paths, p.path, m.slug);
		}
		for (const ManifestPath& p : m.createdDirs) if (p.valid) add(ix.paths, p.path, m.slug);
		for (const ManifestFile& f : m.files) if (f.valid) add(ix.paths, f.path, m.slug);
	}
	return ix;
}

namespace {
std::vector<std::string> lookup(const std::unordered_map<std::string, std::vector<std::string>>& map, std::string_view path,
                                std::string_view exceptSlug) {
	std::vector<std::string> out;
	auto it = map.find(foldKeyUnicode(path));
	if (it == map.end()) return out;
	for (const std::string& s : it->second)
		if (exceptSlug.empty() || !slugEq(s, exceptSlug)) out.push_back(s);
	return out;
}
} // namespace

std::vector<std::string> manifestsReferencing(const ManifestIndex& index, std::string_view path, std::string_view exceptSlug) {
	return lookup(index.paths, path, exceptSlug);
}
std::vector<std::string> dirOwners(const ManifestIndex& index, std::string_view dir, std::string_view exceptSlug) {
	return lookup(index.dirs, dir, exceptSlug);
}
std::vector<std::string> manifestsReferencing(const std::vector<Manifest>& all, std::string_view path, std::string_view exceptSlug) {
	return manifestsReferencing(buildManifestIndex(all), path, exceptSlug);
}
std::vector<std::string> dirOwners(const std::vector<Manifest>& all, std::string_view dir, std::string_view exceptSlug) {
	return dirOwners(buildManifestIndex(all), dir, exceptSlug);
}

// ---- merge --------------------------------------------------------------------------------------------------------------
namespace {
// new entries first, then the old ones `neu` doesn't have (and that `exists` still finds)
void unionPaths(std::vector<ManifestPath>& dst, const std::vector<ManifestPath>& old, const ExistsFn& exists) {
	std::set<std::string> seen;
	std::vector<ManifestPath> out;
	for (const ManifestPath& p : dst) if (seen.insert(foldKeyUnicode(p.path)).second) out.push_back(p);
	for (const ManifestPath& p : old)
		if (seen.insert(foldKeyUnicode(p.path)).second && (!exists || exists(p.path))) out.push_back(p);
	dst = std::move(out);
}
} // namespace

Manifest mergeForUpdate(const Manifest* old, const Manifest& neu, const ExistsFn& exists) {
	Manifest out = neu;
	if (!old) {
		for (ManifestDir& d : out.dirs) {
			d.created = false;
			d.adopted = slugEq(lastSegment(d.path), out.slug);
		}
		validateManifest(out);
		return out;
	}
	for (ManifestDir& d : out.dirs) {
		const ManifestDir* od = nullptr;
		for (const ManifestDir& c : old->dirs) if (pathEq(c.path, d.path)) { od = &c; break; }
		if (!od) continue;
		d.created = od->created;
		d.adopted = od->adopted;
		unionPaths(d.files, od->files, exists);
		unionPaths(d.createdSubdirs, od->createdSubdirs, exists);
	}
	for (const ManifestDir& od : old->dirs) {
		bool there = false;
		for (const ManifestDir& d : out.dirs) if (pathEq(d.path, od.path)) { there = true; break; }
		if (there) continue;
		ManifestDir kept = od; // an old dir the update doesn't ship is kept whole (lists filtered like the others)
		kept.files.clear();
		kept.createdSubdirs.clear();
		unionPaths(kept.files, od.files, exists);
		unionPaths(kept.createdSubdirs, od.createdSubdirs, exists);
		out.dirs.push_back(std::move(kept));
	}
	std::map<std::string, const ManifestFile*> oldFiles;
	for (const ManifestFile& of : old->files) oldFiles.emplace(foldKeyUnicode(of.path), &of);
	std::set<std::string> have;
	for (ManifestFile& f : out.files) {
		const std::string k = foldKeyUnicode(f.path);
		have.insert(k);
		auto it = oldFiles.find(k);
		if (it != oldFiles.end()) f.preexisting = it->second->preexisting;
	}
	for (const ManifestFile& of : old->files)
		if (have.insert(foldKeyUnicode(of.path)).second && (!exists || exists(of.path))) out.files.push_back(of);
	unionPaths(out.createdDirs, old->createdDirs, exists);
	validateManifest(out);
	return out;
}

std::string dryRunMerge(const Manifest* old, const Manifest& neu, const ExistsFn& exists) {
	return manifestSaveProblem(mergeForUpdate(old, neu, exists));
}

} // namespace install
