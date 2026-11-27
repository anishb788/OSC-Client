// SPDX-License-Identifier: GPL-3.0-or-later
#include "install/AppsDevice.h"

#include <cctype>

#include "core/Log.h"

namespace install {

namespace {
std::string trimLower(const std::string& s) {
	size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
	std::string t = a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
	for (char& c : t) c = (char)std::tolower((unsigned char)c);
	return t;
}

// The value of the appsDevice key, nullopt if there is none.
std::optional<std::string> findValue(const std::string& text) {
	std::optional<std::string> found;
	size_t pos = text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0; // UTF-8 BOM
	while (pos <= text.size()) {
		size_t end = text.find('\n', pos);
		if (end == std::string::npos) end = text.size();
		std::string line = text.substr(pos, end - pos);
		pos = end + 1;
		size_t eq = line.find('=');
		if (eq == std::string::npos) continue;
		if (trimLower(line.substr(0, eq)) == "appsdevice") found = line.substr(eq + 1); // the last one wins
	}
	return found;
}
} // namespace

const char* deviceLabel(Device d) { return d == Device::Sd ? "SD card" : d == Device::Usb ? "USB drive" : "no device"; }
const char* deviceId(Device d) { return d == Device::Sd ? "sd" : d == Device::Usb ? "usb" : "none"; }

std::optional<Device> parseDeviceId(const std::string& s) {
	std::string t = trimLower(s);
	if (t == "sd") return Device::Sd;
	if (t == "usb") return Device::Usb;
	return std::nullopt;
}

Resolution resolveAppsDevice(Device saved, bool sd, bool usb) {
	if (saved == Device::Usb) {
		if (usb) return {Device::Usb, Fallback::None};
		if (sd) return {Device::Sd, Fallback::UsbMissing};
		return {Device::None, Fallback::None};
	}
	if (sd) return {Device::Sd, Fallback::None};
	if (usb) return {Device::Usb, Fallback::NoSd};
	return {Device::None, Fallback::None};
}

Device loadSavedDevice(const std::string& iniPath) {
	std::string text;
	ReadStatus st = readFileCapped(iniPath, text, 4096);
	if (st == ReadStatus::Missing) {
		LOG("Apps device storage: %s not found, using sd (nothing written)", iniPath.c_str());
		return Device::Sd;
	}
	if (st != ReadStatus::Ok) {
		LOG("Apps device storage: %s unreadable, using sd (nothing written)", iniPath.c_str());
		return Device::Sd;
	}
	auto v = findValue(text);
	auto d = v ? parseDeviceId(*v) : std::nullopt;
	if (!d) {
		LOG("Apps device storage: %s has %s, using sd (nothing written)", iniPath.c_str(),
		    v ? ("the unknown value '" + trimLower(*v) + "'").c_str() : "no appsDevice line");
		return Device::Sd;
	}
	LOG("Apps device storage: loaded appsDevice=%s from %s", deviceId(*d), iniPath.c_str());
	return *d;
}

bool saveDeviceIfChanged(const std::string& iniPath, Device dev, bool* wrote, const FileOps& ops) {
	if (wrote) *wrote = false;
	if (dev != Device::Sd && dev != Device::Usb) {
		LOG("Apps device storage: refusing to save '%s' (nothing written)", deviceId(dev));
		return false;
	}
	std::string text;
	if (readFileCapped(iniPath, text, 4096) == ReadStatus::Ok) {
		auto v = findValue(text);
		auto cur = v ? parseDeviceId(*v) : std::nullopt;
		if (cur && *cur == dev) {
			LOG("Apps device storage: appsDevice=%s unchanged in %s (no write)", deviceId(dev), iniPath.c_str());
			return true;
		}
	}
	if (!writeFileAtomic(iniPath, std::string("appsDevice=") + deviceId(dev) + "\n", ops)) {
		LOG("Apps device storage: ERROR writing appsDevice=%s to %s", deviceId(dev), iniPath.c_str());
		return false;
	}
	if (wrote) *wrote = true;
	LOG("Apps device storage: saved appsDevice=%s to %s (written)", deviceId(dev), iniPath.c_str());
	return true;
}

void AppsDeviceSetting::apply(Device d) {
	if (d != resolved_) ++epoch_;
	resolved_ = d;
	root_ = d == Device::Sd ? mounts_.sdRoot : d == Device::Usb ? mounts_.usbRoot : std::string();
}

void AppsDeviceSetting::init(const std::string& iniPath, const Mounts& mounts, std::optional<Device> devOverride,
                             const FileOps& ops) {
	iniPath_ = iniPath;
	mounts_ = mounts;
	ops_ = ops;
	saved_ = persisted_ = loadSavedDevice(iniPath);
	Resolution r = resolveAppsDevice(saved_, mounts.sd, mounts.usb);
	overridden_ = false;
	std::string note;
	if (devOverride && (*devOverride == Device::Sd || *devOverride == Device::Usb)) {
		bool mounted = *devOverride == Device::Sd ? mounts.sd : mounts.usb;
		if (mounted) {
			r = {*devOverride, Fallback::None};
			overridden_ = true;
			note = std::string("; dev override ") + deviceId(*devOverride) + ", never saved";
		} else {
			note = std::string("; dev override ") + deviceId(*devOverride) + " ignored, " +
			       (*devOverride == Device::Sd ? "no SD card" : "no USB drive") + " mounted";
		}
	}
	fallback_ = r.fallback;
	apply(r.device);
	std::string why;
	if (r.fallback == Fallback::UsbMissing) why = "; no USB drive at start-up, using the SD card for this session";
	else if (r.fallback == Fallback::NoSd) why = "; no SD card at start-up, using the USB drive";
	else if (r.device == Device::None) why = "; no SD card or USB drive found";
	LOG("Apps device: %s (saved: %s%s%s) root=%s", deviceLabel(resolved_), deviceId(saved_), why.c_str(), note.c_str(),
	    root_.empty() ? "(none)" : root_.c_str());
}

AppsDeviceSetting::SetResult AppsDeviceSetting::set(Device dev, std::string* reason) {
	auto refuse = [&](const char* why) {
		if (reason) *reason = why;
		LOG("Apps device: switch to %s refused: %s", deviceLabel(dev), why);
		return SetResult::Refused;
	};
	if (dev != Device::Sd && dev != Device::Usb) return refuse("Unknown device");
	if (busy_ && busy_()) return refuse("Finish or cancel downloads first");
	if (!(dev == Device::Sd ? mounts_.sd : mounts_.usb)) return refuse(dev == Device::Sd ? "No SD card found" : "No USB drive found");
	if (dev == resolved_ && dev == saved_ && dev == persisted_) {
		LOG("Apps device: already %s (nothing to do)", deviceLabel(dev));
		return SetResult::Switched;
	}
	bool saved = saveDeviceIfChanged(iniPath_, dev, nullptr, ops_);
	if (saved) persisted_ = dev;
	else LOG("Apps device: could not save the choice; using the %s for this session only", deviceLabel(dev));
	saved_ = dev;
	fallback_ = Fallback::None;
	overridden_ = false;
	apply(dev);
	LOG("Apps device: switched to %s (saved: %s%s) root=%s", deviceLabel(resolved_), deviceId(saved_),
	    saved ? "" : ", NOT stored", root_.c_str());
	return saved ? SetResult::Switched : SetResult::SwitchedNotSaved;
}

} // namespace install
