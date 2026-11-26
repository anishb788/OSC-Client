#include "gfx/IconStore.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <vector>

#include "catalog/Catalog.h"
#include "core/Strings.h"

namespace iconstore {

namespace {
using strutil::lowerAscii;

bool endsWith(const std::string& s, const char* suf) {
	size_t n = std::char_traits<char>::length(suf);
	return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}
}  // namespace

std::string dirOf(const std::string& cacheDir) { return cacheDir + "/" + kSubdir; }

std::string pathOf(const std::string& cacheDir, std::string_view slug) {
	if (cacheDir.empty() || !isValidSlug(slug)) return {};
	return dirOf(cacheDir) + "/" + std::string(slug) + ".png";
}

Read read(const std::string& cacheDir, std::string_view slug, uint64_t expectedSize, size_t maxBytes, std::string& out) {
	out.clear();
	const std::string path = pathOf(cacheDir, slug);
	if (path.empty()) return Read::BadSlug;
	switch (readFileCapped(path, out, maxBytes)) {
	case ReadStatus::Ok: break;
	case ReadStatus::Missing: return Read::Missing;
	case ReadStatus::TooLarge: return Read::TooLarge;
	case ReadStatus::IoError: return Read::IoError;
	}
	if (expectedSize > 0 && out.size() != expectedSize) {
		out.clear();
		return Read::SizeMismatch;
	}
	return Read::Ok;
}

bool write(const std::string& cacheDir, std::string_view slug, const std::string& bytes, const FileOps& ops) {
	const std::string path = pathOf(cacheDir, slug);
	if (path.empty() || !makeDirs(dirOf(cacheDir))) return false;
	return writeFileAtomic(path, bytes, ops);
}

bool remove(const std::string& cacheDir, std::string_view slug) {
	const std::string path = pathOf(cacheDir, slug);
	if (path.empty()) return false;
	if (std::remove(path.c_str()) == 0) return true;
	// A failed remove of a file that is not there is fine; one that is still there (read-only card) is not.
	if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
		std::fclose(f);
		return false;
	}
	return true;
}

Pruner::Pruner(std::string cacheDir, const std::set<std::string>& keep) : dir_(std::move(cacheDir)) {
	for (const std::string& s : keep) keep_.insert(lowerAscii(s));
}

bool Pruner::step(size_t perStep) {
	// Step 1 is the directory snapshot and nothing else: one listDir can take a while on a card with thousands of files,
	// and the caller gets control back (and can cancel) before any file is looked at or deleted.
	if (!started_) {
		started_ = true;
		const auto t0 = std::chrono::steady_clock::now();
		if (!listDir(dirOf(dir_), names_)) names_.clear();
		snapshotMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		scanned_ = names_.size();
		return !names_.empty();
	}
	// Later steps: sorting names into keep / delete costs only string work, so it gets a bigger batch than a deletion
	// (an SD write, many ms on libfat); either limit ends the step.
	const size_t classifyMax = std::max<size_t>(perStep, 1) * 32;
	size_t classified = 0, deleted = 0;
	while (deleted < perStep && classified < classifyMax && (next_ < todo_.size() || at_ < names_.size())) {
		if (next_ < todo_.size()) {
			if (std::remove((dirOf(dir_) + "/" + todo_[next_]).c_str()) == 0) removed_++;
			next_++;
			deleted++;
			continue;
		}
		const std::string& n = names_[at_++];
		classified++;
		if (endsWith(n, ".tmp") || endsWith(n, ".bak")) {
			todo_.push_back(n);
		} else if (endsWith(n, ".png") && n.size() > 4) {
			const std::string slug = n.substr(0, n.size() - 4);
			if (isValidSlug(slug) && !keep_.count(lowerAscii(slug))) todo_.push_back(n);
		}
	}
	if (next_ >= todo_.size() && at_ >= names_.size()) {
		names_.clear();
		todo_.clear();
		next_ = at_ = 0;
		return false;
	}
	return true;
}

}  // namespace iconstore
