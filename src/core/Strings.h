#pragma once
// Small string helpers shared across the app, so each file does not carry its own copy.
#include <cctype>
#include <string>
#include <string_view>

namespace strutil {
// ASCII lower-casing (the C locale's tolower; bytes above 127 are left alone, which is what the API data, URLs and
// header names need).
inline std::string lowerAscii(std::string_view s) {
	std::string out(s);
	for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return out;
}
} // namespace strutil
