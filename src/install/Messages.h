// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// install::Messages (M3 Increment 8): the ONE mapping from install errors (and HTTP transfer errors during an install) to the
// text the user reads. Pure: no SDL, no disk. Every string that a Failed job, a refusal or a toast shows for these cases comes
// from here, so the wording is reviewed in one place.
//
// WORDING RULES (host-tested in install_test): nothing here calls a check "secure", "verified", "safe" or a "tamper" test. The
// archive checksum detects a damaged or cut-off download, nothing more (archives come over plain http, see Md5Sink.h), so the UI's
// only word for a failed one is "damaged". The same goes for the device labels: "SD card" and "USB drive" only.
#include <cstdint>
#include <string>

#include "catalog/Catalog.h" // InstallRefusal
#include "install/AppsDevice.h"
#include "net/Http.h"       // HttpError

namespace install {

// What went wrong with an install, beyond the HTTP result. Pre-flight and the job runner use these.
enum class InstallError {
	Damaged,       // the downloaded file did not match the catalog's checksum or size
	NoSpace,       // not enough free space (the amount comes with it)
	NoAppsDevice,  // no SD card or USB drive
	NotOurServer,  // the URL is not an Open Shop Channel archive URL (a retry or a dev hook with a foreign address)
	CantInstall,   // the catalog entry fails installRefusal(); use refusalText() for the reason
	NetworkDown,   // the network is known to be down
	WriteFailed,   // the card could not be written, or is full
	Unavailable,   // the job system or the network worker is not running
	DiskUnreadable, // the free space could not be read from the device
};

// "SD card" / "USB drive" (AppsDevice's labels; "no device" for None).
const char* deviceName(Device d);

// Whole megabytes (1 MB = 1048576 bytes), rounded UP and never below 1: "1 MB", "35 MB", "1024 MB" (no digit grouping).
std::string megabytes(uint64_t bytes);
// The same for an amount that is FREE: rounded DOWN (never promises more than there is), and under 1 MB "less than 1 MB". The caller
// writes "<that> is free", so the result for a full card reads "less than 1 MB is free".
std::string freeMegabytes(uint64_t bytes);

// `needed` and `free` are bytes (NoSpace only; ignored by the others). `device` names the card in NoSpace and WriteFailed.
std::string installErrorText(InstallError e, Device device = Device::None, uint64_t needed = 0, uint64_t freeBytes = 0);

// The reason an app's Download button shows as "Can't be installed" (and what a refused queue attempt says). None -> "".
std::string refusalText(InstallRefusal r);

// The text for a failed download. `status` is HttpResult::status (used for HttpStatus). Cancelled -> "Cancelled." (the queue shows
// cancelled jobs as such and never displays it as an error). Never mentions the transport, TLS or checksums.
std::string downloadErrorText(HttpError e, int status = 0);

// True for the failures one automatic retry from zero may fix (the Increment 8 retry rule): NoNetwork, Dns, Connect, Timeout and a
// 5xx answer. Never Io, TooLarge, Redirects, Cancelled, 4xx or anything else.
bool isRetryable(HttpError e, int status);

} // namespace install
