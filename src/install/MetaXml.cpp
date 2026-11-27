// SPDX-License-Identifier: GPL-3.0-or-later
#include "install/MetaXml.h"

namespace install {

namespace {

constexpr char lowerChar(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

void appendUtf8(std::string& out, unsigned cp) {
	if (cp < 0x80) out += static_cast<char>(cp);
	else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
	else if (cp < 0x10000) {
		out += static_cast<char>(0xE0 | (cp >> 12));
		out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
		out += static_cast<char>(0x80 | (cp & 0x3F));
	} else {
		out += static_cast<char>(0xF0 | (cp >> 18));
		out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
		out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
		out += static_cast<char>(0x80 | (cp & 0x3F));
	}
}

// Decodes the entity starting at s[i] == '&'. Returns the number of bytes consumed (0 = not an entity: keep the '&').
size_t decodeEntity(const std::string& s, size_t i, std::string& out) {
	size_t end = s.find(';', i);
	if (end == std::string::npos || end - i > 10) return 0;
	std::string ent = s.substr(i + 1, end - i - 1);
	if (ent == "amp") out += '&';
	else if (ent == "lt") out += '<';
	else if (ent == "gt") out += '>';
	else if (ent == "quot") out += '"';
	else if (ent == "apos") out += '\'';
	else if (ent.size() >= 2 && ent[0] == '#') {
		bool hex = ent[1] == 'x' || ent[1] == 'X';
		size_t k = hex ? 2 : 1;
		if (k >= ent.size()) return 0;
		unsigned cp = 0;
		for (; k < ent.size(); k++) {
			char c = ent[k];
			unsigned d;
			if (c >= '0' && c <= '9') d = static_cast<unsigned>(c - '0');
			else if (hex && c >= 'a' && c <= 'f') d = static_cast<unsigned>(c - 'a' + 10);
			else if (hex && c >= 'A' && c <= 'F') d = static_cast<unsigned>(c - 'A' + 10);
			else return 0;
			cp = cp * (hex ? 16 : 10) + d;
			if (cp > 0x10FFFF) return 0;
		}
		if (cp < 0x20 || (cp >= 0xD800 && cp <= 0xDFFF) || cp == 0x7F) out += ' ';
		else appendUtf8(out, cp);
	} else return 0;
	return end - i + 1;
}

// Replaces control characters with spaces and bad UTF-8 with '?', then trims and caps the length.
std::string clean(const std::string& in) {
	std::string out;
	size_t i = 0;
	while (i < in.size()) {
		unsigned char c = static_cast<unsigned char>(in[i]);
		if (c < 0x80) {
			out += (c < 0x20 || c == 0x7F) ? ' ' : static_cast<char>(c);
			i++;
			continue;
		}
		size_t n = 0;
		unsigned cp = 0, minCp = 0;
		if (c >= 0xC2 && c <= 0xDF) { n = 2; cp = c & 0x1F; minCp = 0x80; }
		else if (c >= 0xE0 && c <= 0xEF) { n = 3; cp = c & 0x0F; minCp = 0x800; }
		else if (c >= 0xF0 && c <= 0xF4) { n = 4; cp = c & 0x07; minCp = 0x10000; }
		bool ok = n && i + n <= in.size();
		for (size_t k = 1; ok && k < n; k++) {
			unsigned char cc = static_cast<unsigned char>(in[i + k]);
			if ((cc & 0xC0) != 0x80) ok = false;
			else cp = (cp << 6) | (cc & 0x3F);
		}
		if (ok && (cp < minCp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))) ok = false;
		if (ok) { out.append(in, i, n); i += n; }
		else { out += '?'; i++; }
	}
	size_t b = 0, e = out.size();
	while (b < e && isSpace(out[b])) b++;
	while (e > b && isSpace(out[e - 1])) e--;
	out = out.substr(b, e - b);
	if (out.size() > kMaxMetaFieldBytes) {
		size_t cut = kMaxMetaFieldBytes;
		while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) cut--;
		out.resize(cut);
		while (!out.empty() && isSpace(out.back())) out.pop_back();
	}
	return out;
}

// Text of the first <tag ...>...</tag> in `text` (`low` is its ASCII-lowercase copy). Unclosed content ends at the next
// '<' (or the end of the input); CDATA is taken verbatim.
bool extract(const std::string& text, const std::string& low, const std::string& tag, std::string& result) {
	const std::string open = "<" + tag;
	size_t pos = 0;
	while ((pos = low.find(open, pos)) != std::string::npos) {
		size_t after = pos + open.size();
		pos = after;
		if (after >= low.size()) return false;
		if (low[after] != '>' && !isSpace(low[after])) continue; // <namespace>, <name/> and the like
		size_t gt = low.find('>', after);
		if (gt == std::string::npos) return false;
		if (low[gt - 1] == '/') continue; // <name/>
		std::string raw;
		size_t i = gt + 1;
		while (i < text.size()) {
			if (text.compare(i, 9, "<![CDATA[") == 0) {
				size_t e = text.find("]]>", i + 9);
				if (e == std::string::npos) e = text.size();
				raw.append(text, i + 9, e - (i + 9));
				i = e == text.size() ? e : e + 3;
			} else if (text[i] == '<') {
				break;
			} else if (text[i] == '&') {
				size_t used = decodeEntity(text, i, raw);
				if (used) i += used;
				else { raw += '&'; i++; }
			} else {
				raw += text[i++];
			}
		}
		result = clean(raw);
		if (result.empty()) continue; // <version></version>: look for a later one
		return true;
	}
	return false;
}

} // namespace

MetaInfo parseMetaXml(std::string_view input) {
	MetaInfo info;
	if (input.size() > kMaxMetaXmlBytes) input = input.substr(0, kMaxMetaXmlBytes);
	// Blank out comments so a commented-out <version> is not picked up. An unterminated comment runs to the end.
	std::string text;
	text.reserve(input.size());
	for (size_t i = 0; i < input.size();) {
		if (input.compare(i, 4, "<!--") == 0) {
			size_t e = input.find("-->", i + 4);
			if (e == std::string_view::npos) break;
			i = e + 3;
		} else if (input.compare(i, 9, "<![CDATA[") == 0) {
			// Copy CDATA untouched so a "<!--" inside it is not treated as a comment.
			size_t e = input.find("]]>", i + 9);
			size_t stop = e == std::string_view::npos ? input.size() : e + 3;
			text.append(input.substr(i, stop - i));
			i = stop;
		} else {
			text += input[i++];
		}
	}
	std::string low = text;
	for (char& c : low) c = lowerChar(c);
	info.hasName = extract(text, low, "name", info.name);
	info.hasVersion = extract(text, low, "version", info.version);
	if (!info.hasName) info.name.clear();
	if (!info.hasVersion) info.version.clear();
	return info;
}

} // namespace install
