// SPDX-License-Identifier: GPL-3.0-or-later
#include "install/Messages.h"

namespace install {

const char* deviceName(Device d) { return deviceLabel(d); }

std::string megabytes(uint64_t bytes) {
	constexpr uint64_t kMb = 1024 * 1024;
	uint64_t mb = bytes / kMb + (bytes % kMb ? 1 : 0);
	if (mb < 1) mb = 1;
	return std::to_string(mb) + " MB";
}

std::string freeMegabytes(uint64_t bytes) {
	constexpr uint64_t kMb = 1024 * 1024;
	if (bytes < kMb) return "less than 1 MB";
	return std::to_string(bytes / kMb) + " MB";
}

std::string installErrorText(InstallError e, Device device, uint64_t needed, uint64_t freeBytes) {
	const std::string dev = device == Device::None ? "the card" : std::string("the ") + deviceName(device);
	switch (e) {
	case InstallError::Damaged: return "The download was damaged. Please try again.";
	case InstallError::NoSpace:
		return "Not enough space on " + dev + ": about " + megabytes(needed) + " is needed, " + freeMegabytes(freeBytes) + " is free.";
	case InstallError::NoAppsDevice: return "No SD card or USB drive found.";
	case InstallError::NotOurServer: return "This app can't be downloaded from here.";
	case InstallError::CantInstall: return "This app can't be installed.";
	case InstallError::NetworkDown: return "The Wii isn't connected to the internet. Check your network settings.";
	case InstallError::WriteFailed: return "Couldn't write to " + dev + ". It may be full, locked or removed.";
	case InstallError::Unavailable: return "Downloads aren't available right now.";
	case InstallError::DiskUnreadable: return "Couldn't read how much space is left on " + dev + ".";
	}
	return "Something went wrong.";
}

std::string refusalText(InstallRefusal r) {
	switch (r) {
	case InstallRefusal::None: return "";
	case InstallRefusal::NoArchiveUrl: return "This app has no download.";
	case InstallRefusal::BadMd5: return "This app's download information is incomplete.";
	case InstallRefusal::UnknownPackage: return "This kind of app can't be installed.";
	case InstallRefusal::BadArchiveSize: return "This app's download is too large for an SD card or USB drive.";
	case InstallRefusal::ArchiveTooLarge: return "This app's download is too large.";
	case InstallRefusal::BadInstallSize: return "This app's download information is incomplete.";
	case InstallRefusal::InstallTooLarge: return "This app is too large to install.";
	case InstallRefusal::NoAppDir: return "This app's download information is incomplete.";
	}
	return "This app can't be installed.";
}

std::string downloadErrorText(HttpError e, int status) {
	switch (e) {
	case HttpError::None: return "";
	case HttpError::NoNetwork: return "The Wii isn't connected to the internet. Check your network settings.";
	case HttpError::Dns: return "Couldn't find the server. Check your internet connection and DNS settings.";
	case HttpError::Connect: return "Couldn't connect to the server.";
	case HttpError::Timeout: return "The server took too long to respond.";
	case HttpError::HttpStatus:
		return status > 0 ? "The server couldn't provide this app (HTTP " + std::to_string(status) + ")." : "The server couldn't provide this app.";
	case HttpError::Redirects: // a 3xx: the shop's archive URLs never redirect, so this is an answer we do not follow
	case HttpError::TlsRequired:
	case HttpError::Parse: return "The shop's server sent an unexpected answer.";
	case HttpError::TooLarge: return "The download was not the size the shop said it would be.";
	case HttpError::Io: return "The download was interrupted or couldn't be saved.";
	case HttpError::Cancelled: return "Cancelled.";
	}
	return "Something went wrong.";
}

bool isRetryable(HttpError e, int status) {
	switch (e) {
	case HttpError::NoNetwork:
	case HttpError::Dns:
	case HttpError::Connect:
	case HttpError::Timeout: return true;
	case HttpError::HttpStatus: return status >= 500 && status <= 599;
	default: return false;
	}
}

} // namespace install
