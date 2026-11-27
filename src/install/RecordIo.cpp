// SPDX-License-Identifier: GPL-3.0-or-later
#include "install/RecordIo.h"

#include <cstdio>
#include <cstring>

#include "core/Log.h"

namespace install::rec {

std::string installedDir(const std::string& root) { return root + "/apps/OpenShop/installed"; }

Read readRecord(const std::string& path, size_t maxBytes, const FileOps& ops) {
	Read r;
	auto doRead = [&](const std::string& p) {
		return ops.read ? ops.read(p.c_str(), r.text, maxBytes) : readFileCapped(p, r.text, maxBytes);
	};
	ReadStatus st = doRead(path);
	if (st == ReadStatus::Missing) {
		st = doRead(path + ".bak");
		r.fromBackup = st == ReadStatus::Ok;
	}
	switch (st) {
	case ReadStatus::Ok: r.result = ReadResult::Ok; break;
	case ReadStatus::Missing: r.result = ReadResult::Missing; break;
	case ReadStatus::TooLarge: r.result = ReadResult::TooLarge; break;
	default: r.result = ReadResult::IoError; break;
	}
	if (r.result != ReadResult::Ok) r.text.clear();
	return r;
}

bool writeRecord(const std::string& path, const std::string& text, size_t maxBytes, const FileOps& ops) {
	if (text.size() > maxBytes) {
		LOG("record %s: %zu bytes is over the %zu cap, not written", path.c_str(), text.size(), maxBytes);
		return false;
	}
	const size_t slash = path.rfind('/');
	if (slash != std::string::npos && !makeDirs(path.substr(0, slash), ops)) return false;
	if (!writeFileAtomic(path, text, ops, false, true)) return false;
	// A ".bak" left by an earlier cut is older than what was just written; drop it so the loader never prefers it by mistake.
	auto rm = ops.remove ? ops.remove : +[](const char* p) { return std::remove(p); };
	const std::string bak = path + ".bak";
	rm(bak.c_str());
	return true;
}

bool removeRecord(const std::string& path, const FileOps& ops) {
	auto rm = ops.remove ? ops.remove : +[](const char* p) { return std::remove(p); };
	auto ex = ops.exists ? ops.exists : +[](const char* p) { std::FILE* f = std::fopen(p, "rb"); if (f) std::fclose(f); return f != nullptr; };
	bool clean = true;
	for (const char* suffix : {".bak", ".tmp", ""}) {
		const std::string p = path + suffix;
		if (ex(p.c_str())) {
			rm(p.c_str());
			if (ex(p.c_str())) clean = false;
		}
	}
	return clean;
}

bool shapeOk(std::string_view text, int maxDepth, size_t maxValues) {
	int depth = 0;
	size_t values = 0;
	bool inStr = false, esc = false;
	for (char c : text) {
		if (inStr) {
			if (esc) esc = false;
			else if (c == '\\') esc = true;
			else if (c == '"') inStr = false;
		} else if (c == '"') {
			inStr = true;
		} else if (c == '[' || c == '{') {
			if (++depth > maxDepth || ++values > maxValues) return false;
		} else if (c == ']' || c == '}') {
			if (depth > 0) depth--;
		} else if (c == ',') {
			if (++values > maxValues) return false;
		}
	}
	return true;
}

Json parseObject(std::string_view text, int maxDepth, size_t maxValues, std::string& error) {
	if (!shapeOk(text, maxDepth, maxValues)) { error = "nested too deeply or too many values"; return nullptr; }
	json_error_t e;
	Json j(json_loadb(text.data(), text.size(), JSON_REJECT_DUPLICATES, &e));
	if (!j) { error = std::string("bad JSON: ") + e.text; return nullptr; }
	if (!json_is_object(j.get())) { error = "not a JSON object"; return nullptr; }
	return j;
}

json_t* jsonStr(const std::string& s) { return json_stringn(s.data(), s.size()); }

bool asString(json_t* v, size_t maxLen, std::string& out) {
	if (!json_is_string(v)) return false;
	const size_t n = json_string_length(v);
	const char* s = json_string_value(v);
	if (n > maxLen || std::strlen(s) != n) return false;
	out.assign(s, n);
	return true;
}

bool getString(json_t* obj, const char* key, size_t maxLen, std::string& out) {
	json_t* v = json_object_get(obj, key);
	return v && asString(v, maxLen, out);
}

bool getInt(json_t* obj, const char* key, int64_t& out) {
	json_t* v = json_object_get(obj, key);
	if (!v || !json_is_integer(v)) return false;
	out = (int64_t)json_integer_value(v);
	return true;
}

bool getBool(json_t* obj, const char* key, bool& out) {
	json_t* v = json_object_get(obj, key);
	if (!v || !json_is_boolean(v)) return false;
	out = json_is_true(v);
	return true;
}

json_t* getArray(json_t* obj, const char* key, size_t maxSize) {
	json_t* v = json_object_get(obj, key);
	if (!v || !json_is_array(v) || json_array_size(v) > maxSize) return nullptr;
	return v;
}

} // namespace install::rec
