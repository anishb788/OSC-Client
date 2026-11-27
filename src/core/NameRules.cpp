// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/NameRules.h"

#include <cctype>
#include <string>

#include "core/Strings.h"

using strutil::lowerAscii;

bool isReservedDeviceName(std::string_view name) {
	// Windows treats everything from the first dot as an extension; trailing spaces before it are dropped too.
	size_t dot = name.find('.');
	if (dot != std::string_view::npos) name = name.substr(0, dot);
	while (!name.empty() && name.back() == ' ') name.remove_suffix(1);
	std::string l = lowerAscii(std::string(name));
	if (l == "con" || l == "prn" || l == "aux" || l == "nul") return true;
	return l.size() == 4 && (l.compare(0, 3, "com") == 0 || l.compare(0, 3, "lpt") == 0) && l[3] >= '1' && l[3] <= '9';
}

bool isValidSlug(std::string_view slug) {
	if (slug.empty() || slug.size() > kMaxSlugLength || slug[0] == '.' || slug.back() == '.') return false;
	if (isReservedDeviceName(slug)) return false;
	for (char c : slug) {
		if (!(std::isalnum((unsigned char)c) && (unsigned char)c < 0x80) && c != '.' && c != '_' && c != '-') return false;
	}
	return true;
}
