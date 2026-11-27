// SPDX-License-Identifier: GPL-3.0-or-later
#include "install/Version.h"

#include <string>
#include <vector>

namespace install {

namespace {

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }
bool isDigit(char c) { return c >= '0' && c <= '9'; }

std::string normalise(std::string_view s) {
	while (!s.empty() && isSpace(s.front())) s.remove_prefix(1);
	while (!s.empty() && isSpace(s.back())) s.remove_suffix(1);
	std::string out(s);
	for (char& c : out) {
		if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
	}
	if (out.size() >= 2 && out[0] == 'v' && isDigit(out[1])) out.erase(0, 1);
	return out;
}

// "1", "1.2", "10.0.3": digits separated by single dots. Parts are kept as digit strings without leading zeros, so
// there is no overflow however long they are.
bool parseDotted(const std::string& s, std::vector<std::string>& parts) {
	if (s.empty()) return false;
	std::string cur;
	for (size_t i = 0; i <= s.size(); i++) {
		if (i == s.size() || s[i] == '.') {
			if (cur.empty()) return false;
			size_t nz = cur.find_first_not_of('0');
			parts.push_back(nz == std::string::npos ? "0" : cur.substr(nz));
			cur.clear();
		} else if (isDigit(s[i])) {
			cur += s[i];
		} else {
			return false;
		}
	}
	return true;
}

int cmpNum(const std::string& a, const std::string& b) {
	if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
	int c = a.compare(b);
	return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

} // namespace

VersionRelation compareVersions(std::string_view installed, std::string_view catalog) {
	std::string a = normalise(installed), b = normalise(catalog);
	std::vector<std::string> pa, pb;
	if (parseDotted(a, pa) && parseDotted(b, pb)) {
		size_t n = pa.size() > pb.size() ? pa.size() : pb.size();
		for (size_t i = 0; i < n; i++) {
			int c = cmpNum(i < pa.size() ? pa[i] : "0", i < pb.size() ? pb[i] : "0");
			if (c < 0) return VersionRelation::CatalogNewer;
			if (c > 0) return VersionRelation::CatalogOlder;
		}
		return VersionRelation::Same;
	}
	return a == b ? VersionRelation::Same : VersionRelation::Different;
}

} // namespace install
