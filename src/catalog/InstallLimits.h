// SPDX-License-Identifier: GPL-3.0-or-later
// Pure size limits shared by the catalog (AppEntry::installRefusal) and the install code.
#pragma once
#include <cstddef>
#include <cstdint>

inline constexpr uint64_t kMaxArchiveBytes = UINT64_C(512) * 1024 * 1024;  // largest archive OpenShop downloads
inline constexpr uint64_t kMaxInstallBytes = UINT64_C(1024) * 1024 * 1024; // largest total uncompressed size
inline constexpr uint64_t kFat32MaxFile = (UINT64_C(4) << 30) - 1;         // FAT32: 4 GiB - 1 per file
inline constexpr size_t kMaxEntries = 20000; // archive entries; provisional until Increment 2's heap answer
