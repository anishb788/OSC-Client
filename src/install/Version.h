// SPDX-License-Identifier: GPL-3.0-or-later
// Installed-vs-catalog version comparison (pure).
#pragma once
#include <string_view>

namespace install {

enum class VersionRelation { Same, CatalogNewer, CatalogOlder, Different };

// Both sides are trimmed (ASCII whitespace), lower-cased and lose a leading 'v' that is followed by a digit.
// Two dotted-numeric strings ("1.2", "1.10.3") compare numerically, missing parts counting as 0 ("1.2.0" == "1.2").
// Anything else is Same when equal and Different otherwise ("4.0 r1283" vs "4.0 r1290"); Different means an update
// is offered. Two empty strings are Same; one empty string is Different.
VersionRelation compareVersions(std::string_view installed, std::string_view catalog);

} // namespace install
