// SPDX-License-Identifier: GPL-3.0-or-later
// Zip fixtures built at runtime with libzip's writer (no binary fixtures in git), plus a central-directory patcher for the
// "declared size is a lie" cases.
#pragma once
#include <sys/stat.h>
#include <zip.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <list>
#include <string>
#include <vector>

struct ZipEntrySpec {
	std::string name;
	std::string data;
	bool dir = false;
	zip_int32_t method = ZIP_CM_DEFLATE; // ZIP_CM_STORE, ZIP_CM_DEFLATE, ZIP_CM_BZIP2
	bool symlink = false;
	bool encrypted = false;
};

inline ZipEntrySpec zfile(std::string name, std::string data, zip_int32_t method = ZIP_CM_DEFLATE) {
	ZipEntrySpec e;
	e.name = std::move(name);
	e.data = std::move(data);
	e.method = method;
	return e;
}
inline ZipEntrySpec zdir(std::string name) {
	ZipEntrySpec e;
	e.name = std::move(name);
	e.dir = true;
	return e;
}

// Writes `entries` to `path`. False if libzip refuses (for example a method this libzip was built without).
inline bool buildZip(const std::string& path, const std::vector<ZipEntrySpec>& entries) {
	int err = 0;
	zip_t* z = zip_open(path.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &err);
	if (!z) return false;
	std::list<std::string> keep; // zip_source_buffer needs the data to live until zip_close
	bool ok = true;
	for (const ZipEntrySpec& e : entries) {
		if (e.dir) {
			if (zip_dir_add(z, e.name.c_str(), ZIP_FL_ENC_UTF_8) < 0) { ok = false; break; }
			continue;
		}
		keep.push_back(e.data);
		zip_source_t* src = zip_source_buffer(z, keep.back().data(), keep.back().size(), 0);
		if (!src) { ok = false; break; }
		zip_int64_t idx = zip_file_add(z, e.name.c_str(), src, ZIP_FL_ENC_UTF_8);
		if (idx < 0) { zip_source_free(src); ok = false; break; }
		if (zip_set_file_compression(z, (zip_uint64_t)idx, e.method, 0) != 0) { ok = false; break; }
		if (e.symlink && zip_file_set_external_attributes(z, (zip_uint64_t)idx, 0, ZIP_OPSYS_UNIX, (zip_uint32_t)(0120777u << 16)) != 0) { ok = false; break; }
		if (e.encrypted && zip_file_set_encryption(z, (zip_uint64_t)idx, ZIP_EM_TRAD_PKWARE, "secret") != 0) { ok = false; break; }
	}
	if (!ok) { zip_discard(z); return false; }
	if (zip_close(z) != 0) { zip_discard(z); return false; }
	return true;
}

inline bool slurp(const std::string& path, std::string& out) {
	std::FILE* f = std::fopen(path.c_str(), "rb");
	if (!f) return false;
	out.clear();
	char b[8192];
	size_t n;
	while ((n = std::fread(b, 1, sizeof b, f)) > 0) out.append(b, n);
	std::fclose(f);
	return true;
}
inline bool spit(const std::string& path, const std::string& data) {
	std::FILE* f = std::fopen(path.c_str(), "wb");
	if (!f) return false;
	bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
	return std::fclose(f) == 0 && ok;
}

// Rewrites the uncompressed size in the CENTRAL DIRECTORY header of the entry called `name` (the local header and the
// data are untouched), so the archive declares a size its data does not have. True if the entry was found.
inline bool patchCentralUncompressedSize(const std::string& path, const std::string& name, uint32_t newSize) {
	std::string z;
	if (!slurp(path, z)) return false;
	for (size_t i = 0; i + 46 <= z.size(); i++) {
		if (std::memcmp(&z[i], "PK\x01\x02", 4) != 0) continue;
		const size_t nlen = (unsigned char)z[i + 28] | ((unsigned char)z[i + 29] << 8);
		if (i + 46 + nlen > z.size() || nlen != name.size() || z.compare(i + 46, nlen, name) != 0) continue;
		for (int b = 0; b < 4; b++) z[i + 24 + b] = (char)((newSize >> (8 * b)) & 0xff);
		return spit(path, z);
	}
	return false;
}

// Rewrites the entry count and central-directory size in the end-of-central-directory record (the last "PK\5\6").
inline bool patchEocd(const std::string& path, uint16_t count, uint32_t cdSize) {
	std::string z;
	if (!slurp(path, z) || z.size() < 22) return false;
	for (size_t i = z.size() - 22 + 1; i-- > 0;) {
		if (std::memcmp(&z[i], "PK\x05\x06", 4) != 0) continue;
		for (int b = 0; b < 2; b++) { z[i + 8 + b] = (char)((count >> (8 * b)) & 0xff); z[i + 10 + b] = (char)((count >> (8 * b)) & 0xff); }
		for (int b = 0; b < 4; b++) z[i + 12 + b] = (char)((cdSize >> (8 * b)) & 0xff);
		return spit(path, z);
	}
	return false;
}

// Renames an entry in BOTH its central-directory header and its local header; `to` must have the same length as `from`.
inline bool patchEntryName(const std::string& path, const std::string& from, const std::string& to) {
	if (from.size() != to.size()) return false;
	std::string z;
	if (!slurp(path, z)) return false;
	int hits = 0;
	for (size_t i = 0; i + 30 <= z.size(); i++) {
		size_t nameAt = 0;
		if (std::memcmp(&z[i], "PK\x01\x02", 4) == 0) nameAt = i + 46;
		else if (std::memcmp(&z[i], "PK\x03\x04", 4) == 0) nameAt = i + 30;
		else continue;
		const size_t nlenAt = nameAt == i + 46 ? i + 28 : i + 26;
		const size_t nlen = (unsigned char)z[nlenAt] | ((unsigned char)z[nlenAt + 1] << 8);
		if (nlen != from.size() || nameAt + nlen > z.size() || z.compare(nameAt, nlen, from) != 0) continue;
		z.replace(nameAt, nlen, to);
		hits++;
	}
	return hits == 2 && spit(path, z);
}
