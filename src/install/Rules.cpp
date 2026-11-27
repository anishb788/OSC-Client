// SPDX-License-Identifier: GPL-3.0-or-later
#include "install/Rules.h"

#include <cstdint>

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

namespace {
// Simple case folding (to lower case) for the blocks FAT's up-case table covers that matter here.
uint32_t foldCp(uint32_t c) {
	if (c < 0x80) return (c >= 'A' && c <= 'Z') ? c + 32 : c;
	if (c < 0x100) {
		if (c == 0xB5) return 0x3BC;                          // micro sign
		if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32; // Latin-1 capitals
		return c;
	}
	if (c < 0x180) { // Latin Extended-A
		if (c == 0x130 || c == 0x131 || c == 0x138 || c == 0x149) return c;
		if (c == 0x178) return 0xFF;
		if (c == 0x17F) return 's';
		if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) return (c & 1) ? c : c + 1;
		return (c & 1) ? c + 1 : c; // 0x139-0x148 and 0x179-0x17E: odd capitals
	}
	if (c < 0x250) { // Latin Extended-B
		switch (c) {
			case 0x181: return 0x253; case 0x186: return 0x254; case 0x189: return 0x256; case 0x18A: return 0x257;
			case 0x18E: return 0x1DD; case 0x18F: return 0x259; case 0x190: return 0x25B; case 0x193: return 0x260;
			case 0x194: return 0x263; case 0x196: return 0x269; case 0x197: return 0x268; case 0x19C: return 0x26F;
			case 0x19D: return 0x272; case 0x19F: return 0x275; case 0x1A6: return 0x280; case 0x1A9: return 0x283;
			case 0x1AE: return 0x288; case 0x1B1: return 0x28A; case 0x1B2: return 0x28B; case 0x1B7: return 0x292;
			case 0x1C4: case 0x1C5: return 0x1C6; case 0x1C7: case 0x1C8: return 0x1C9; case 0x1CA: case 0x1CB: return 0x1CC;
			case 0x1F1: case 0x1F2: return 0x1F3; case 0x1F6: return 0x195; case 0x1F7: return 0x1BF; case 0x220: return 0x19E;
			case 0x182: case 0x184: case 0x187: case 0x18B: case 0x191: case 0x198: case 0x1A0: case 0x1A2: case 0x1A4:
			case 0x1A7: case 0x1AC: case 0x1AF: case 0x1B3: case 0x1B5: case 0x1B8: case 0x1BC: case 0x1F4:
				return c + 1;
			default: break;
		}
		if ((c >= 0x1CD && c <= 0x1DC)) return (c & 1) ? c + 1 : c;
		if ((c >= 0x1DE && c <= 0x1EF) || (c >= 0x1F8 && c <= 0x21F) || (c >= 0x222 && c <= 0x233) || (c >= 0x246 && c <= 0x24F))
			return (c & 1) ? c : c + 1;
		return c;
	}
	if (c >= 0x370 && c < 0x400) { // Greek
		if (c == 0x370 || c == 0x372 || c == 0x376) return c + 1;
		if (c == 0x37F) return 0x3F3;
		if (c == 0x386) return 0x3AC;
		if (c >= 0x388 && c <= 0x38A) return c + 37;
		if (c == 0x38C) return 0x3CC;
		if (c == 0x38E || c == 0x38F) return c + 63;
		if ((c >= 0x391 && c <= 0x3A1) || (c >= 0x3A3 && c <= 0x3AB)) return c + 32;
		switch (c) {
			case 0x3C2: return 0x3C3; case 0x3CF: return 0x3D7; case 0x3D0: return 0x3B2; case 0x3D1: return 0x3B8;
			case 0x3D5: return 0x3C6; case 0x3D6: return 0x3C0; case 0x3F0: return 0x3BA; case 0x3F1: return 0x3C1;
			case 0x3F4: return 0x3B8; case 0x3F5: return 0x3B5; case 0x3F7: return 0x3F8; case 0x3F9: return 0x3F2;
			case 0x3FA: return 0x3FB; case 0x3FD: case 0x3FE: case 0x3FF: return c - 130;
			default: break;
		}
		if (c >= 0x3D8 && c <= 0x3EF) return (c & 1) ? c : c + 1;
		return c;
	}
	if (c >= 0x400 && c < 0x530) { // Cyrillic
		if (c <= 0x40F) return c + 80;
		if (c <= 0x42F) return c + 32;
		if (c >= 0x460 && c <= 0x481) return (c & 1) ? c : c + 1;
		if (c >= 0x48A && c <= 0x4BF) return (c & 1) ? c : c + 1;
		if (c == 0x4C0) return 0x4CF;
		if (c >= 0x4C1 && c <= 0x4CE) return (c & 1) ? c + 1 : c;
		if (c >= 0x4D0 && c <= 0x52F) return (c & 1) ? c : c + 1;
		return c;
	}
	return c;
}

void appendUtf8(std::string& out, uint32_t c) {
	if (c < 0x80) out += (char)c;
	else if (c < 0x800) { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 0x3F)); }
	else if (c < 0x10000) { out += (char)(0xE0 | (c >> 12)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
	else { out += (char)(0xF0 | (c >> 18)); out += (char)(0x80 | ((c >> 12) & 0x3F)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
}
} // namespace

std::string foldKeyUnicode(std::string_view path) {
	std::string out;
	out.reserve(path.size());
	for (size_t i = 0; i < path.size();) {
		const unsigned char b = (unsigned char)path[i];
		size_t len = b < 0x80 ? 1 : (b >= 0xC2 && b <= 0xDF) ? 2 : (b >= 0xE0 && b <= 0xEF) ? 3 : (b >= 0xF0 && b <= 0xF4) ? 4 : 0;
		uint32_t cp = len == 1 ? b : len == 2 ? (b & 0x1Fu) : len == 3 ? (b & 0x0Fu) : (b & 0x07u);
		bool ok = len != 0 && i + len <= path.size();
		for (size_t k = 1; ok && k < len; k++) {
			const unsigned char cb = (unsigned char)path[i + k];
			if ((cb & 0xC0) != 0x80) ok = false;
			else cp = (cp << 6) | (cb & 0x3Fu);
		}
		if (ok && ((len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF))) ok = false;
		if (!ok) { out += (char)b; i++; continue; } // not valid UTF-8: keep the byte
		appendUtf8(out, foldCp(cp));
		i += len;
	}
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
