#include "core/FileUtil.h"

#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

namespace {
bool statExists(const char* path) {
	struct stat st;
	return ::stat(path, &st) == 0;
}
} // namespace

bool commitFile(const std::string& tmp, const std::string& to, const FileOps& ops, bool keepBackup) {
	int (*rename)(const char*, const char*) = ops.rename ? ops.rename : &std::rename;
	int (*remove)(const char*) = ops.remove ? ops.remove : &std::remove;
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
	(ops.remove ? ops.remove : &std::remove)(tmp.c_str());
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

bool makeDirs(const std::string& dir) {
	if (dir.empty()) return false;
	// Walk the path one component at a time; "sd:/apps/x" starts after the "sd:/" prefix, which is not creatable.
	size_t pos = dir.find(":/");
	pos = pos == std::string::npos ? 0 : pos + 2;
	while (pos < dir.size()) {
		size_t slash = dir.find('/', pos);
		std::string part = dir.substr(0, slash == std::string::npos ? dir.size() : slash);
		pos = slash == std::string::npos ? dir.size() : slash + 1;
		if (part.empty() || part.back() == '/') continue;
		if (::mkdir(part.c_str(), 0777) != 0 && !statExists(part.c_str())) return false;
	}
	return statExists(dir.c_str());
}

bool listDir(const std::string& dir, std::vector<std::string>& names) {
	names.clear();
	DIR* d = ::opendir(dir.c_str());
	if (!d) return false;
	while (const dirent* e = ::readdir(d)) {
		if (std::strcmp(e->d_name, ".") != 0 && std::strcmp(e->d_name, "..") != 0) names.emplace_back(e->d_name);
	}
	::closedir(d);
	return true;
}
