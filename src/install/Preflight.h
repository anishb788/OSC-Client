// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// install::Preflight (M3 Increment 8): "may this install start?", asked at queue time on the main thread and again, authoritatively,
// when the job starts. A pure decision (decide(), host-tested) plus the platform queries that feed it (preflight()).
//
// THE RULES, in this order (the first that fails wins):
//   1. the entry passes installRefusal() (re-derived from the copied JobSpec, so a Retry or a dev hook is checked like the UI's job);
//   2. the URL passes oscapi::isAllowedDownloadUrl (checked again at download time, not only when the catalog was parsed);
//   3. an apps device exists (SD card or USB drive) with a root to write to;
//   4. free space on that device >= spaceNeeded(): archive + max(uncompressed, archive) + 10% of that sum + 1 MB. The archive and the
//      extracted tree can exist side by side until the job ends (the zip stays until extraction has finished), and the 10% plus 1 MB
//      cover FAT's cluster rounding and the manifest/journal. A device whose free space cannot be read is refused too;
//   5. the network is not KNOWN to be down (Unknown passes: the first transfer finds out; Down expires, see net/NetState.h).
//
// CHECKSUM WORDING (comment only): archives come over plain http; MD5 detects corruption and truncation, not tampering.
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "catalog/Catalog.h"
#include "install/JobQueue.h" // JobSpec
#include "install/Messages.h"
#include "net/NetState.h"

namespace install {

// archive + max(uncompressed, archive), plus 10% of that sum, plus 1 MB. Saturates at UINT64_MAX instead of overflowing.
uint64_t spaceNeeded(uint64_t archiveSize, uint64_t uncompressedSize);

struct PreflightInput {
	InstallRefusal refusal = InstallRefusal::None;
	bool urlAllowed = false;
	bool hasAppsDevice = false;
	Device device = Device::None;          // for the wording only
	uint64_t archiveSize = 0, uncompressedSize = 0;
	std::optional<uint64_t> freeBytes;     // nullopt: could not be read
	net::NetState net = net::NetState::Unknown;
};

enum class PreflightFail { None, Refused, UrlNotAllowed, NoAppsDevice, DiskUnreadable, NoSpace, NetworkDown };

struct PreflightResult {
	PreflightFail fail = PreflightFail::None;
	std::string message; // UI-ready; empty when ok
	uint64_t needed = 0, have = 0; // NoSpace: bytes
	bool ok() const { return fail == PreflightFail::None; }
};

PreflightResult decide(const PreflightInput& in);

// The queries decide() needs, injectable for tests. Null members mean the real thing.
struct PreflightEnv {
	std::function<InstallRefusal(const JobSpec&)> refusal;           // default: installRefusal() of an AppEntry built from the spec
	std::function<bool(const std::string&)> urlAllowed;              // default: oscapi::isAllowedDownloadUrl
	std::function<std::optional<uint64_t>(const std::string&)> freeSpace; // default: FileUtil's freeSpace(root)
	net::NetState net = net::NetState::Unknown;                      // the caller reads net::netState(now)
};

// The catalog's own gate for what the spec carries (slug, url, md5, sizes, package type, dirs hint).
InstallRefusal refusalOf(const JobSpec& spec);

// Builds the input from the spec and the environment (a disk query: statvfs of spec.targetRoot) and decides. `device` is the device
// the target root is (Device::None when there is none; then spec.targetRoot must be empty too).
PreflightResult preflight(const JobSpec& spec, Device device, const PreflightEnv& env = {});

} // namespace install
