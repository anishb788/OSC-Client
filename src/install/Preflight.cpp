// SPDX-License-Identifier: GPL-3.0-or-later
#include "install/Preflight.h"

#include <algorithm>
#include <limits>

#include "catalog/OscApi.h"
#include "core/FileUtil.h"

namespace install {

uint64_t spaceNeeded(uint64_t archiveSize, uint64_t uncompressedSize) {
	constexpr uint64_t kMax = std::numeric_limits<uint64_t>::max();
	constexpr uint64_t kMb = 1024 * 1024;
	const uint64_t big = std::max(uncompressedSize, archiveSize);
	if (archiveSize > kMax - big) return kMax;
	const uint64_t sum = archiveSize + big;
	const uint64_t slack = sum / 10 + kMb; // 10% taken over the whole sum, not over one part
	if (sum > kMax - slack) return kMax;
	return sum + slack;
}

PreflightResult decide(const PreflightInput& in) {
	PreflightResult r;
	auto fail = [&](PreflightFail f, std::string msg) {
		r.fail = f;
		r.message = std::move(msg);
		return r;
	};
	if (in.refusal != InstallRefusal::None) return fail(PreflightFail::Refused, refusalText(in.refusal));
	if (!in.urlAllowed) return fail(PreflightFail::UrlNotAllowed, installErrorText(InstallError::NotOurServer));
	if (!in.hasAppsDevice) return fail(PreflightFail::NoAppsDevice, installErrorText(InstallError::NoAppsDevice));
	if (!in.freeBytes) return fail(PreflightFail::DiskUnreadable, installErrorText(InstallError::DiskUnreadable, in.device));
	r.needed = spaceNeeded(in.archiveSize, in.uncompressedSize);
	r.have = *in.freeBytes;
	if (r.have < r.needed)
		return fail(PreflightFail::NoSpace, installErrorText(InstallError::NoSpace, in.device, r.needed, r.have));
	if (in.net == net::NetState::Down) return fail(PreflightFail::NetworkDown, installErrorText(InstallError::NetworkDown));
	return r;
}

InstallRefusal refusalOf(const JobSpec& spec) {
	AppEntry e;
	e.slug = spec.slug;
	e.archiveUrl = spec.url;
	e.archiveMd5 = spec.md5;
	e.archiveSize = spec.archiveSize;
	e.uncompressedSize = spec.uncompressedSize;
	e.packageType = static_cast<PackageType>(spec.packageType);
	e.subdirectories = spec.dirs;
	return e.installRefusal();
}

PreflightResult preflight(const JobSpec& spec, Device device, const PreflightEnv& env) {
	PreflightInput in;
	in.refusal = env.refusal ? env.refusal(spec) : refusalOf(spec);
	in.urlAllowed = env.urlAllowed ? env.urlAllowed(spec.url) : oscapi::isAllowedDownloadUrl(spec.url);
	in.hasAppsDevice = device != Device::None && !spec.targetRoot.empty();
	in.device = device;
	in.archiveSize = spec.archiveSize;
	in.uncompressedSize = spec.uncompressedSize;
	in.net = env.net;
	// Only a decision that reaches the space rule needs the disk: skip the query when something earlier fails anyway.
	if (in.refusal == InstallRefusal::None && in.urlAllowed && in.hasAppsDevice)
		in.freeBytes = env.freeSpace ? env.freeSpace(spec.targetRoot) : freeSpace(spec.targetRoot);
	return decide(in);
}

} // namespace install
