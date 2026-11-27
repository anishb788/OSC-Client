// SPDX-License-Identifier: GPL-3.0-or-later
#include "install/ZipExtract.h"

#include <sys/stat.h>
#include <zip.h>

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <new>
#include <set>

#include "core/Log.h"
#include "install/Rules.h"

namespace install {

namespace {
constexpr size_t kChunk = 64 * 1024;
constexpr uint64_t kSpaceSlack = UINT64_C(1) << 20;   // begin() wants the declared total plus 1 MB free
constexpr size_t kTailRead = 64 * 1024 + 22;           // the end-of-central-directory record sits in the last 64 KB + 22 bytes
constexpr uint64_t kMaxCdEntryBytes = 46 + 3 * 65535;  // a central-directory header: 46 fixed bytes, name, extra, comment

uint64_t rd(const unsigned char* p, int bytes) {
	uint64_t v = 0;
	for (int i = bytes - 1; i >= 0; i--) v = (v << 8) | p[i];
	return v;
}

// Reads the end-of-central-directory record (and the zip64 one when present) BEFORE libzip sees the file, so a huge entry
// count or an implausible directory size is refused without libzip allocating for it. Bounded: one tail read, one small read.
ZipError preScan(const std::string& path, size_t maxEntries, std::string& why) {
	std::FILE* f = std::fopen(path.c_str(), "rb");
	if (!f) { why = "cannot open the file"; return ZipError::Open; }
	struct Closer { std::FILE* f; ~Closer() { std::fclose(f); } } closer{f};
	if (fseeko(f, 0, SEEK_END) != 0) { why = "cannot seek"; return ZipError::Open; }
	const off_t sz = ftello(f);
	if (sz < 22) { why = "too small to be a zip"; return ZipError::Open; }
	const uint64_t fileSize = (uint64_t)sz;
	const size_t tailLen = (size_t)std::min<uint64_t>(fileSize, kTailRead);
	const uint64_t tailOff = fileSize - tailLen;
	std::unique_ptr<unsigned char[]> tail(new (std::nothrow) unsigned char[tailLen]);
	if (!tail) { why = "no memory"; return ZipError::Open; }
	if (fseeko(f, (off_t)tailOff, SEEK_SET) != 0 || std::fread(tail.get(), 1, tailLen, f) != tailLen) {
		why = "cannot read the end of the file";
		return ZipError::Open;
	}
	size_t pos = tailLen - 22;
	for (;; pos--) {
		if (std::memcmp(tail.get() + pos, "PK\x05\x06", 4) == 0) break;
		if (pos == 0) { why = "no end-of-central-directory record"; return ZipError::Open; }
	}
	const unsigned char* e = tail.get() + pos;
	uint64_t count = rd(e + 10, 2), cdSize = rd(e + 12, 4), cdOff = rd(e + 16, 4);
	if ((count == 0xFFFF || cdSize == 0xFFFFFFFFu || cdOff == 0xFFFFFFFFu) && pos >= 20 &&
	    std::memcmp(e - 20, "PK\x06\x07", 4) == 0) {
		const uint64_t z64Off = rd(e - 20 + 8, 8);
		unsigned char z[56];
		if (z64Off > fileSize || fileSize - z64Off < sizeof z || fseeko(f, (off_t)z64Off, SEEK_SET) != 0 ||
		    std::fread(z, 1, sizeof z, f) != sizeof z || std::memcmp(z, "PK\x06\x06", 4) != 0) {
			why = "bad zip64 end-of-central-directory record";
			return ZipError::Corrupt;
		}
		count = rd(z + 32, 8);
		cdSize = rd(z + 40, 8);
		cdOff = rd(z + 48, 8);
	}
	if (count > maxEntries) { why = std::to_string(count) + " entries"; return ZipError::TooManyEntries; }
	if (cdSize > fileSize || cdOff > fileSize || cdOff + cdSize > fileSize) { why = "central directory outside the file"; return ZipError::Corrupt; }
	if (cdSize > (uint64_t)maxEntries * kMaxCdEntryBytes) { why = "central directory too large"; return ZipError::TooBig; }
	return ZipError::None;
}

// A collision key: the folded name with '/' mapped to '\0' (below every character a name may hold), so a folder's
// descendants sort directly after the folder's own key.
struct CollisionKey {
	std::string key;
	bool isDir;
	bool operator<(const CollisionKey& o) const { return key != o.key ? key < o.key : isDir < o.isDir; }
};

std::string trimSlashes(const std::string& s) {
	size_t a = 0, b = s.size();
	while (a < b && s[a] == '/') a++;
	while (b > a && s[b - 1] == '/') b--;
	return s.substr(a, b - a);
}

// root + "/" + rel, only when rel is clean and the result is strictly below root. Checked on the string that is used.
bool joinUnder(const std::string& root, const std::string& rel, std::string& out) {
	if (root.empty() || rel.empty() || rel.front() == '/' || rel.back() == '/') return false;
	if (rel.find('\\') != std::string::npos || rel.find("//") != std::string::npos) return false;
	size_t pos = 0;
	while (pos <= rel.size()) {
		size_t e = rel.find('/', pos);
		if (e == std::string::npos) e = rel.size();
		const std::string seg = rel.substr(pos, e - pos);
		if (seg.empty() || seg == "." || seg == "..") return false;
		pos = e + 1;
	}
	out = root + "/" + rel;
	return out.size() > root.size() + 1 && out.compare(0, root.size() + 1, root + "/") == 0;
}

// A staging directory with no empty, '.' or '..' segment (one leading '/' is fine), no backslash and no trailing '/'.
bool cleanStagingDir(const std::string& d) {
	if (d.empty() || d.back() == '/' || d.find('\\') != std::string::npos) return false;
	size_t pos = d.front() == '/' ? 1 : 0;
	while (pos <= d.size()) {
		size_t e = d.find('/', pos);
		if (e == std::string::npos) e = d.size();
		const std::string seg = d.substr(pos, e - pos);
		if (seg.empty() || seg == "." || seg == "..") return false;
		pos = e + 1;
	}
	return true;
}

std::string parentOf(const std::string& p) {
	size_t s = p.rfind('/');
	return s == std::string::npos ? std::string() : p.substr(0, s);
}
} // namespace

const char* describe(ZipError e) {
	switch (e) {
		case ZipError::None: return "none";
		case ZipError::Open: return "cannot open the archive";
		case ZipError::BadName: return "bad entry name";
		case ZipError::Collision: return "entries collide on a FAT file system";
		case ZipError::Symlink: return "symlink or special file";
		case ZipError::BadEntry: return "malformed entry";
		case ZipError::Encrypted: return "encrypted entry";
		case ZipError::Method: return "unsupported compression method";
		case ZipError::TooManyEntries: return "too many entries";
		case ZipError::TooBig: return "too much data";
		case ZipError::FileTooBig: return "file too big for FAT32";
		case ZipError::NoBoot: return "no matching boot file";
		case ZipError::Forbidden: return "entry in a forbidden place";
		case ZipError::NoSpace: return "not enough free space";
		case ZipError::Io: return "write error";
		case ZipError::SizeMismatch: return "entry size does not match its header";
		case ZipError::Corrupt: return "damaged entry";
		case ZipError::Cancelled: return "cancelled";
		case ZipError::BadState: return "call out of order";
	}
	return "?";
}

ZipExtract::ZipExtract(ZipOptions opts) : opts_(std::move(opts)) {}

ZipExtract::~ZipExtract() {
	// A failed cleanup is not retried here; callers check rootRemoved() after a failure.
	closeCurrent();
	if (state_ == State::Extracting) cleanupRoot(); // an unfinished run leaves nothing behind
	if (zip_) zip_discard(zip_);
}

bool ZipExtract::fail(ZipError code, std::string detail, std::string entry) {
	failure_.code = code;
	failure_.detail = std::move(detail);
	failure_.entry = std::move(entry);
	LOG("ZipExtract: %s: %s%s%s", describe(code), failure_.detail.c_str(), failure_.entry.empty() ? "" : " | ",
	    failure_.entry.c_str());
	return false;
}

bool ZipExtract::failOpen(ZipError code, std::string detail, std::string entry) {
	state_ = State::Failed;
	if (zip_) { zip_discard(zip_); zip_ = nullptr; }
	items_.clear();
	return fail(code, std::move(detail), std::move(entry));
}

void ZipExtract::closeCurrent() {
	if (zf_) { zip_fclose(zf_); zf_ = nullptr; }
	if (out_) { std::fclose(out_); out_ = nullptr; }
}

void ZipExtract::cleanupRoot() {
	closeCurrent();
	if (!rootCreated_ || rootRemoved_) return;
	RemoveResult r = removeTree(root_, stagingDir_, opts_.removeRoots, opts_.fileOps);
	rootRemoved_ = r.ok;
	if (!r.ok) LOG("ZipExtract: could not remove %s: %s", root_.c_str(), r.error.c_str());
}

StepResult ZipExtract::stepFail(ZipError code, std::string detail, std::string entry) {
	fail(code, std::move(detail), std::move(entry));
	cleanupRoot();
	state_ = State::Failed;
	return StepResult::Error;
}

// ---- open: validation of the whole central directory ----------------------------------------------------------------

bool ZipExtract::open(const std::string& zipPath) {
	if (state_ != State::Idle) return fail(ZipError::BadState, "open() called twice");
	{
		std::string why;
		const ZipError pe = preScan(zipPath, opts_.maxEntries, why);
		if (pe != ZipError::None) return failOpen(pe, why);
	}
	if (opts_.uncompressedSize) LOG("ZipExtract: the catalog says %llu bytes uncompressed", (unsigned long long)opts_.uncompressedSize);
	int zerr = 0;
	zip_ = zip_open(zipPath.c_str(), ZIP_RDONLY, &zerr);
	if (!zip_) return failOpen(ZipError::Open, "zip_open failed, code " + std::to_string(zerr));

	const zip_int64_t n64 = zip_get_num_entries(zip_, 0);
	if (n64 < 0) return failOpen(ZipError::Open, "no entry count");
	const size_t n = (size_t)n64;
	plan_.entryCount = n;
	if (n > opts_.maxEntries) return failOpen(ZipError::TooManyEntries, std::to_string(n) + " entries");

	std::set<std::string> hint;
	for (const std::string& h : opts_.subdirHint) {
		const std::string t = trimSlashes(h);
		if (!t.empty() && t.find('/') == std::string::npos) hint.insert(foldKey(t)); // a top-level folder only
	}

	std::vector<CollisionKey> keys;
	keys.reserve(n);
	std::set<std::string> topSeen; // folded top-level folders (other than apps)
	std::set<std::string> appSeen;
	uint64_t sum = 0;
	bool haveBoot = false;

	for (size_t i = 0; i < n; i++) {
		zip_stat_t st;
		zip_stat_init(&st);
		if (zip_stat_index(zip_, (zip_uint64_t)i, ZIP_FL_ENC_RAW, &st) != 0 || !(st.valid & ZIP_STAT_NAME))
			return failOpen(ZipError::Open, "unreadable entry " + std::to_string(i));
		const std::string rawName = st.name;
		EntryName en = validateEntryName(rawName);
		if (!en.ok) return failOpen(ZipError::BadName, describe(en.reason), rawName);
		const std::string& top = en.segments[0];

		if (isJunkRoot(top)) { plan_.skippedMacosx++; LOG("ZipExtract: skip %s", rawName.c_str()); continue; }
		if (isSystemRoot(top)) return failOpen(ZipError::Forbidden, "system folder", rawName);
		if (en.segments.size() == 1 && !en.isDir) {
			plan_.skippedRootFiles++;
			LOG("ZipExtract: skip root file %s", rawName.c_str());
			continue;
		}
		const bool inApps = isAppsRoot(top);
		if (inApps && isOwnRoot(en.relPath)) return failOpen(ZipError::Forbidden, "OpenShop's own folder", rawName);
		if (inApps && en.segments.size() == 2 && !en.isDir) {
			plan_.skippedRootFiles++;
			LOG("ZipExtract: skip loose file in apps/ %s", rawName.c_str());
			continue;
		}

		// Kind of entry: encryption, method, symlink.
		zip_uint8_t opsys = 0;
		zip_uint32_t attr = 0;
		bool unixDir = false;
		if (zip_file_get_external_attributes(zip_, (zip_uint64_t)i, 0, &opsys, &attr) == 0 && opsys == ZIP_OPSYS_UNIX) {
			const unsigned mode = (attr >> 16) & 0170000u;
			if (mode == 0120000u) return failOpen(ZipError::Symlink, "symlink", rawName);
			if (mode != 0 && mode != 0100000u && mode != 0040000u) return failOpen(ZipError::Symlink, "special file", rawName);
			unixDir = mode == 0040000u;
		}
		if (unixDir && !en.isDir) return failOpen(ZipError::BadEntry, "directory without a trailing slash", rawName);
		if (!(st.valid & ZIP_STAT_SIZE)) return failOpen(ZipError::Open, "no size", rawName);
		if (en.isDir) {
			if (st.size != 0) return failOpen(ZipError::BadEntry, "directory with data", rawName);
		} else {
			if ((st.valid & ZIP_STAT_ENCRYPTION_METHOD) && st.encryption_method != ZIP_EM_NONE)
				return failOpen(ZipError::Encrypted, "encrypted", rawName);
			if (!(st.valid & ZIP_STAT_COMP_METHOD) || (st.comp_method != ZIP_CM_STORE && st.comp_method != ZIP_CM_DEFLATE))
				return failOpen(ZipError::Method, "method " + std::to_string(st.comp_method), rawName);
			if (st.size > kFat32MaxFile) return failOpen(ZipError::FileTooBig, "file of 4 GiB or more", rawName);
			if (st.size > opts_.maxInstallBytes || sum + st.size > opts_.maxInstallBytes)
				return failOpen(ZipError::TooBig, "declared total over the cap", rawName);
			sum += st.size;
		}

		// Where does it go?
		bool undeclared = false;
		if (inApps) {
			if (en.segments.size() >= 2) {
				const std::string dirKey = foldKey(en.segments[1]);
				if (opts_.ownsDir && !opts_.ownsDir(dirKey)) return failOpen(ZipError::Forbidden, "app folder belongs to another app", rawName);
				if (appSeen.insert(dirKey).second) plan_.appDirs.push_back({en.segments[1], false});
				if (en.segments.size() == 3 && !en.isDir) {
					const std::string leaf = foldKey(en.segments[2]);
					const bool wantDol = opts_.packageType != PackageType::Elf, wantElf = opts_.packageType != PackageType::Dol;
					if (st.size > 0 && ((wantDol && leaf == "boot.dol") || (wantElf && leaf == "boot.elf"))) { // an empty boot file is no boot file
						haveBoot = true;
						for (ZipPlan::AppDir& d : plan_.appDirs)
							if (foldKey(d.name) == dirKey) d.hasBoot = true;
					}
				}
			}
		} else {
			const std::string topKey = foldKey(top);
			undeclared = hint.count(topKey) == 0;
			if (topSeen.insert(topKey).second) plan_.otherFolders.push_back({top, undeclared});
		}

		// Collisions: one key per entry; the parents every entry implies are found by the sorted scan below.
		keys.push_back({foldKeyUnicode(en.relPath), en.isDir});
		std::replace(keys.back().key.begin(), keys.back().key.end(), '/', '\0');

		Item it;
		it.index = (uint32_t)i;
		it.size = en.isDir ? 0 : st.size;
		it.isDir = en.isDir;
		it.undeclared = undeclared;
		items_.push_back(it);
	}

	// A file collides with an equal key, and with any key that starts with its own key + '\0' (it would have to be a folder).
	// Such descendants sort directly after the file's key. Two folders with one key are harmless.
	std::sort(keys.begin(), keys.end());
	for (size_t i = 0; i + 1 < keys.size(); i++) {
		const CollisionKey& a = keys[i];
		const CollisionKey& b = keys[i + 1];
		std::string shown = a.key; // for the log: the '\0' separators back to '/'
		std::replace(shown.begin(), shown.end(), '\0', '/');
		if (a.key == b.key) {
			if (!a.isDir || !b.isDir) return failOpen(ZipError::Collision, "two entries fold to one name", shown);
		} else if (!a.isDir && b.key.size() > a.key.size() && b.key[a.key.size()] == '\0' &&
		           b.key.compare(0, a.key.size(), a.key) == 0) {
			return failOpen(ZipError::Collision, "a file and a folder fold to one name", shown);
		}
	}
	keys.clear();
	keys.shrink_to_fit();
	if (!haveBoot) return failOpen(ZipError::NoBoot, "no apps/<dir>/boot.dol or boot.elf for this package type");

	plan_.declaredTotal = sum;
	plan_.writtenEntries = items_.size();
	state_ = State::Opened;
	LOG("ZipExtract: %zu entries, %zu to write, %llu bytes declared", n, items_.size(), (unsigned long long)sum);
	return true;
}

// ---- begin ------------------------------------------------------------------------------------------------------------

bool ZipExtract::begin(const std::string& stagingDir, uint64_t freeBytes) {
	if (state_ != State::Opened) return fail(ZipError::BadState, "begin() needs a validated archive");
	if (opts_.removeRoots.empty()) return fail(ZipError::Io, "no removeRoots, so a failed run could not be cleaned up");
	const uint64_t needed = plan_.declaredTotal + kSpaceSlack;
	if (freeBytes < needed) {
		fail(ZipError::NoSpace, "need " + std::to_string(needed) + " bytes, have " + std::to_string(freeBytes));
		failure_.needed = needed;
		return false;
	}
	std::string root;
	if (!cleanStagingDir(stagingDir) || !joinUnder(stagingDir, "root", root)) return fail(ZipError::Io, "bad staging directory");
	if (!makeDirs(stagingDir, opts_.fileOps)) return fail(ZipError::Io, "cannot create the staging directory");
	// <staging>/root must be new: the extraction never merges into something that is already there.
	struct stat sb;
	if (::stat(root.c_str(), &sb) == 0) return fail(ZipError::Io, "the staging root already exists");
	if (!makeDirs(root, opts_.fileOps)) return fail(ZipError::Io, "cannot create the staging root");
	stagingDir_ = stagingDir;
	root_ = root;
	rootCreated_ = true;
	rootRemoved_ = false;
	buf_.reset(new (std::nothrow) unsigned char[kChunk]);
	if (!buf_) {
		cleanupRoot();
		state_ = State::Failed;
		return fail(ZipError::Io, "no memory for the copy buffer");
	}
	state_ = State::Extracting;
	return true;
}

// ---- step -------------------------------------------------------------------------------------------------------------

bool ZipExtract::openEntry(const Item& it) {
	zip_stat_t st;
	zip_stat_init(&st);
	if (zip_stat_index(zip_, it.index, ZIP_FL_ENC_RAW, &st) != 0 || !(st.valid & ZIP_STAT_NAME))
		return fail(ZipError::Corrupt, "entry vanished");
	curName_ = st.name;
	// Everything is checked again on the way out: the name, the declared size, and the path that will be written.
	const EntryName en = validateEntryName(curName_);
	if (!en.ok || en.isDir != it.isDir) return fail(ZipError::BadName, "name changed since validation", curName_);
	if (!it.isDir && ((st.valid & ZIP_STAT_SIZE) == 0 || st.size != it.size))
		return fail(ZipError::SizeMismatch, "declared size changed", curName_);
	std::string path;
	if (!joinUnder(root_, en.relPath, path)) return fail(ZipError::BadName, "path escapes the staging root", curName_);
	if (it.isDir) {
		if (!makeDirs(path, opts_.fileOps)) return fail(ZipError::Io, "cannot create the folder", curName_);
		return true;
	}
	const std::string parent = parentOf(path);
	std::string chk;
	if (parent.size() <= root_.size() || !joinUnder(root_, parent.substr(root_.size() + 1), chk))
		return fail(ZipError::BadName, "parent escapes the staging root", curName_);
	if (!makeDirs(parent, opts_.fileOps)) return fail(ZipError::Io, "cannot create the parent folder", curName_);
	struct stat sb;
	// BACKSTOP only: open() already rejected every folded-name collision (including simple Unicode case folding). This catches
	// whatever the fold missed (a case pair FAT knows and foldKeyUnicode does not) on the real file system, before overwriting.
	if (::lstat(path.c_str(), &sb) == 0) return fail(ZipError::Collision, "the output file already exists", curName_);
	zf_ = zip_fopen_index(zip_, it.index, 0);
	if (!zf_) return fail(ZipError::Corrupt, zip_strerror(zip_), curName_);
	out_ = std::fopen(path.c_str(), "wb");
	if (!out_) {
		const int e = errno;
		return fail(ZipError::Io, std::string("cannot create the file: ") + std::strerror(e), curName_);
	}
	curWritten_ = 0;
	return true;
}

StepResult ZipExtract::step(unsigned budgetMs) {
	if (state_ == State::Done) return StepResult::Done;
	if (state_ != State::Extracting) {
		if (state_ != State::Failed) fail(ZipError::BadState, "step() before begin()");
		return StepResult::Error;
	}
	const auto t0 = std::chrono::steady_clock::now();
	for (;;) {
		if (cancel_) return stepFail(ZipError::Cancelled, "cancelled");

		if (!zf_) {
			if (next_ >= items_.size()) {
				state_ = State::Done;
				return StepResult::Done;
			}
			const Item& it = items_[next_];
			if (it.undeclared && !confirmed_) return StepResult::NeedsConfirmation;
			if (!openEntry(it)) {
				ZipFailure f = failure_;
				cleanupRoot();
				failure_ = f;
				state_ = State::Failed;
				return StepResult::Error;
			}
			if (it.isDir) next_++;
		} else {
			const Item& it = items_[next_];
			const uint64_t remaining = it.size - curWritten_;
			// One byte more than is left, so an entry that is longer than declared is caught at its first extra byte.
			const size_t want = (size_t)std::min<uint64_t>(kChunk, remaining + 1);
			const zip_int64_t got = zip_fread(zf_, buf_.get(), want);
			if (got < 0) return stepFail(ZipError::Corrupt, zip_file_strerror(zf_), curName_);
			if ((uint64_t)got > remaining) return stepFail(ZipError::SizeMismatch, "more data than declared", curName_);
			if (got == 0) {
				if (curWritten_ != it.size) return stepFail(ZipError::SizeMismatch, "less data than declared", curName_);
				const int zc = zip_fclose(zf_);
				zf_ = nullptr;
				if (zc != 0) return stepFail(ZipError::Corrupt, "checksum or read error at the end of the entry", curName_);
				const int fc = std::fclose(out_);
				out_ = nullptr;
				if (fc != 0) return stepFail(ZipError::Io, "close failed (disk full?)", curName_);
				next_++;
			} else {
				const size_t w = opts_.testWriteFn ? opts_.testWriteFn(buf_.get(), 1, (size_t)got, out_)
				                               : std::fwrite(buf_.get(), 1, (size_t)got, out_);
				if (w != (size_t)got) return stepFail(ZipError::Io, "write failed (disk full?)", curName_);
				curWritten_ += (uint64_t)got;
				actualTotal_ += (uint64_t)got;
				bytesDone_ = actualTotal_;
			}
		}
		const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
		if (ms >= (long long)budgetMs) return StepResult::Running;
	}
}

} // namespace install
