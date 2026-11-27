// SPDX-License-Identifier: GPL-3.0-or-later
// Lenient extractor for the <name> and <version> of a Homebrew Channel meta.xml (pure). It is not an XML parser: it
// tolerates a missing declaration, a stray '&', CDATA, comments, unclosed tags, CRLF and truncated files.
#pragma once
#include <cstddef>
#include <string>
#include <string_view>

namespace install {

constexpr size_t kMaxMetaXmlBytes = 64 * 1024; // only this much of the input is looked at
constexpr size_t kMaxMetaFieldBytes = 256;     // each returned field is cut to this (on a UTF-8 boundary)

struct MetaInfo {
	bool hasName = false;
	bool hasVersion = false;
	std::string name;
	std::string version;
};

// A field that is missing or empty after trimming leaves its has* flag false. Control characters become spaces and
// invalid UTF-8 bytes become '?', so the result is safe to draw and to store.
MetaInfo parseMetaXml(std::string_view text);

} // namespace install
