// SPDX-License-Identifier: GPL-3.0-or-later
// Pure name predicates shared by the catalog and the install rules (no SDL, libogc or file access), so neither has to
// depend on the other.
#pragma once
#include <cstddef>
#include <string_view>

// Slugs become cache and install paths: [A-Za-z0-9._-]{1,kMaxSlugLength}, no leading dot (so never "." or ".."), no
// trailing dot (Windows/FAT drop it), and not a reserved device name. Parsers must skip apps whose slug fails this.
inline constexpr size_t kMaxSlugLength = 128;
bool isValidSlug(std::string_view slug);
// FAT/Windows device names (CON, PRN, AUX, NUL, COM1-9, LPT1-9), case-insensitive, with or without an extension:
// "con" and "Con.txt" are reserved, "console" and "com0" are not.
bool isReservedDeviceName(std::string_view name);
