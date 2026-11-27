// SPDX-License-Identifier: GPL-3.0-or-later
#include "install/Rules.h"

#include "core/NameRules.h"
#include "core/Log.h"

namespace install {

namespace {

constexpr char lowerChar(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool ieq(std::string_view a, std::string_view b) {
	if (a.size() != b.size()) return false;
	for (size_t i = 0; i < a.size(); i++) {
		if (lowerChar(a[i]) != lowerChar(b[i])) return false;
	}
	return true;
}

// Strict UTF-8: no overlong forms, no surrogates, nothing above U+10FFFF, no truncated sequences.
bool validUtf8(std::string_view s) {
	size_t i = 0;
	while (i < s.size()) {
		unsigned char c = static_cast<unsigned char>(s[i]);
		if (c < 0x80) { i++; continue; }
		size_t n;
		unsigned cp;
		unsigned minCp;
		if (c >= 0xC2 && c <= 0xDF) { n = 2; cp = c & 0x1F; minCp = 0x80; }
		else if (c >= 0xE0 && c <= 0xEF) { n = 3; cp = c & 0x0F; minCp = 0x800; }
		else if (c >= 0xF0 && c <= 0xF4) { n = 4; cp = c & 0x07; minCp = 0x10000; }
		else return false; // continuation byte, 0xC0/0xC1 (always overlong) or 0xF5+
		if (i + n > s.size()) return false;
		for (size_t k = 1; k < n; k++) {
			unsigned char cc = static_cast<unsigned char>(s[i + k]);
			if ((cc & 0xC0) != 0x80) return false;
			cp = (cp << 6) | (cc & 0x3F);
		}
		if (cp < minCp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
		i += n;
	}
	return true;
}

// Code points that are valid UTF-8 but must not reach a file name. Above U+FFFF (4-byte forms) is out because the
// Wii's FAT driver only handles the BMP; the rest are invisible or bidi characters (spoofing) and the characters whose
// case fold on the device could collide with an ASCII name (dotted/dotless I, long s, Kelvin and Angstrom signs).
bool badCodePoint(unsigned cp) {
	if (cp > 0xFFFF) return true;
	if (cp == 0xFFFE || cp == 0xFFFF) return true;
	if (cp >= 0x80 && cp <= 0x9F) return true;
	if (cp >= 0x200B && cp <= 0x200F) return true;
	if (cp >= 0x2028 && cp <= 0x202E) return true;
	if (cp >= 0x2060 && cp <= 0x2064) return true;
	if (cp == 0xFEFF) return true;
	return cp == 0x0130 || cp == 0x0131 || cp == 0x017F || cp == 0x212A || cp == 0x212B;
}

// s is already known to be valid UTF-8.
bool hasBadCodePoint(std::string_view s) {
	size_t i = 0;
	while (i < s.size()) {
		unsigned char c = static_cast<unsigned char>(s[i]);
		if (c < 0x80) { i++; continue; }
		size_t n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
		unsigned cp = c & (0x3F >> (n - 1));
		for (size_t k = 1; k < n; k++) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
		if (badCodePoint(cp)) return true;
		i += n;
	}
	return false;
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }

NameReject checkSegment(std::string_view seg) {
	if (seg.empty() || seg == "." || seg == "..") return NameReject::BadSegment;
	if (seg.back() == '.' || seg.back() == ' ') return NameReject::TrailingDotSpace;
	if (seg.front() == ' ') return NameReject::LeadingSpace;
	if (hasBadCodePoint(seg)) return NameReject::BadUnicode;
	if (isReservedDeviceName(seg)) return NameReject::ReservedName;
	for (size_t i = 0; i + 1 < seg.size(); i++) {
		if (seg[i] == '~' && isDigit(seg[i + 1])) return NameReject::FatAlias;
	}
	return NameReject::None;
}

std::string_view trimSlashes(std::string_view s) {
	while (!s.empty() && s.front() == '/') s.remove_prefix(1);
	while (!s.empty() && s.back() == '/') s.remove_suffix(1);
	return s;
}

std::string_view firstSegment(std::string_view s) {
	size_t slash = s.find('/');
	return slash == std::string_view::npos ? s : s.substr(0, slash);
}

} // namespace

EntryName validateEntryName(std::string_view name) {
	EntryName out;
	auto fail = [&](NameReject r) {
		out.ok = false;
		out.reason = r;
		out.segments.clear();
		out.relPath.clear();
		return out;
	};
	if (name.empty()) return fail(NameReject::Empty);
	if (name.size() > kMaxEntryNameBytes) return fail(NameReject::TooLong);
	if (name.find('\\') != std::string_view::npos) return fail(NameReject::Backslash);
	if (name[0] == '/') return fail(NameReject::Absolute);
	if (!validUtf8(name)) return fail(NameReject::BadUtf8);
	for (char c : name) {
		unsigned char u = static_cast<unsigned char>(c);
		if (u < 0x20 || u == 0x7F) return fail(NameReject::BadChar);
	}

	std::string_view body = name;
	if (body.back() == '/') {
		out.isDir = true;
		body.remove_suffix(1);
	}
	// A colon in the first segment is a device or drive prefix ("sd:", "usb:", "C:"); anywhere else it is just illegal.
	if (firstSegment(body).find(':') != std::string_view::npos) return fail(NameReject::DrivePrefix);
	for (char c : body) {
		if (std::string_view("*?\"<>|:").find(c) != std::string_view::npos) return fail(NameReject::BadChar);
	}

	size_t i = 0;
	while (true) {
		size_t j = body.find('/', i);
		std::string_view seg = body.substr(i, j == std::string_view::npos ? std::string_view::npos : j - i);
		NameReject r = checkSegment(seg);
		if (r != NameReject::None) return fail(r);
		if (out.segments.size() >= kMaxEntrySegments) return fail(NameReject::TooDeep);
		out.segments.emplace_back(seg);
		if (j == std::string_view::npos) break;
		i = j + 1;
	}
	for (size_t k = 0; k < out.segments.size(); k++) {
		if (k) out.relPath += '/';
		out.relPath += out.segments[k];
	}
	out.ok = true;
	return out;
}

const char* describe(NameReject r) {
	switch (r) {
	case NameReject::None: return "ok";
	case NameReject::Empty: return "empty name";
	case NameReject::Backslash: return "backslash in name";
	case NameReject::Absolute: return "absolute path";
	case NameReject::DrivePrefix: return "device or drive prefix";
	case NameReject::BadSegment: return "empty, \".\" or \"..\" segment";
	case NameReject::TrailingDotSpace: return "segment ends in a dot or space";
	case NameReject::BadChar: return "illegal character";
	case NameReject::BadUnicode: return "unsafe Unicode character";
	case NameReject::LeadingSpace: return "segment starts with a space";
	case NameReject::BadUtf8: return "invalid UTF-8";
	case NameReject::ReservedName: return "reserved device name";
	case NameReject::FatAlias: return "FAT short-name alias pattern";
	case NameReject::TooLong: return "name too long";
	case NameReject::TooDeep: return "too many segments";
	}
	return "rejected";
}

std::string foldKey(std::string_view path) {
	std::string out(path);
	for (char& c : out) c = lowerChar(c);
	return out;
}

bool isAppsRoot(std::string_view topSegment) { return ieq(topSegment, "apps"); }

bool isSystemRoot(std::string_view topSegment) {
	static const char* const kRoots[] = {"private", "title", "shared2", "ticket", "sys", "import", "meta", "tmp"};
	for (const char* r : kRoots) {
		if (ieq(topSegment, r)) return true;
	}
	return false;
}

bool isOwnRoot(std::string_view path) {
	path = trimSlashes(path);
	std::string_view top = firstSegment(path);
	if (!isAppsRoot(top) || top.size() == path.size()) return false;
	std::string_view rest = path.substr(top.size() + 1);
	return ieq(firstSegment(rest), "OpenShop");
}

bool isJunkRoot(std::string_view topSegment) { return ieq(topSegment, "__MACOSX"); }

std::vector<std::string> filterSubdirHint(const std::vector<std::string>& subdirectories) {
	std::vector<std::string> kept;
	for (const std::string& s : subdirectories) {
		std::string_view p = trimSlashes(s);
		std::string_view top = firstSegment(p);
		const char* why = nullptr;
		if (p.empty()) why = "device root";
		else if (isAppsRoot(p)) why = "the apps folder itself";
		else if (isSystemRoot(top)) why = "system root";
		else if (isOwnRoot(p)) why = "OpenShop's own folder";
		else if (isJunkRoot(top)) why = "junk";
		if (why) {
			LOG("subdirectory hint '%s' ignored (%s)", s.c_str(), why);
			continue;
		}
		kept.push_back(s);
	}
	return kept;
}

} // namespace install
