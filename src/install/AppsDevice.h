#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
// The "Show apps on" device (M3 Increment 2b): which storage device's apps OpenShop shows and installs to. Plain C++ and
// stdio only, so the host tests (install_test) link it with FileUtil.cpp and nothing else.
//
// STORAGE: M4 3a's core/Settings has not landed, so the saved choice is one line, "appsDevice=sd|usb", in
// <dataDir>/appsdevice.ini (written with writeFileAtomic, and only when the value changes). When 3a lands its first
// load moves the key into settings.ini and removes this file.
#include <functional>
#include <optional>
#include <string>

#include "core/FileUtil.h"

namespace install {

enum class Device { None, Sd, Usb };
enum class Fallback { None, UsbMissing, NoSd };

// The one place that spells the device names (moved behind Messages in Increment 8). Platform's StorageRoot::id / label are
// built from these two functions (not the other way round: this file links into the host tests without Platform), and App
// matches storage roots with deviceId().
const char* deviceLabel(Device d); // "SD card", "USB drive", "no device"
const char* deviceId(Device d);    // "sd", "usb", "none"
std::optional<Device> parseDeviceId(const std::string& s); // "sd" / "usb" only (case-insensitive, trimmed)

struct Resolution {
	Device device = Device::None;
	Fallback fallback = Fallback::None;
};
// `saved` is Sd or Usb. Mount state is read once at start-up, so the result is fixed for the session.
//   saved sd,  SD mounted            -> Sd
//   saved usb, USB mounted           -> Usb
//   saved usb, no USB, SD mounted    -> Sd, UsbMissing (the saved value is NOT changed)
//   no SD, USB mounted               -> Usb, NoSd (whatever was saved; the Wii mini)
//   neither                          -> None
Resolution resolveAppsDevice(Device saved, bool sdMounted, bool usbMounted);

// ---- storage ----------------------------------------------------------------------------------------------------------
// A missing, unreadable or garbage value gives Sd (logged). Parsing: an optional UTF-8 BOM, LF or CRLF line ends, an
// unterminated last line, keys and values trimmed and case-insensitive, other lines ignored; with a repeated appsDevice
// key the LAST one wins (as M4 3a's Ini will).
Device loadSavedDevice(const std::string& iniPath);
// Writes "appsDevice=<id>\n" unless the file already holds that value. `wrote` (optional) says whether a write happened.
// False only if a needed write failed.
bool saveDeviceIfChanged(const std::string& iniPath, Device dev, bool* wrote = nullptr, const FileOps& ops = {});

// ---- the setting ------------------------------------------------------------------------------------------------------
class AppsDeviceSetting {
public:
	struct Mounts {
		bool sd = false, usb = false;
		std::string sdRoot, usbRoot;
	};
	using BusyFn = std::function<bool()>;
	// Switched: now on the device and the choice is stored. SwitchedNotSaved: now on the device for this session, but the
	// save failed (the UI says "Couldn't remember this choice"); calling set() again retries the save. Refused: nothing
	// changed, `reason` says why.
	enum class SetResult { Switched, SwitchedNotSaved, Refused };

	// THREADING: set(), root() and every other member are main-thread only.

	// Loads the saved value, resolves it and logs one line. `devOverride` (dev option appsdevice=) forces the session
	// device if that device is mounted; it is never saved.
	void init(const std::string& iniPath, const Mounts& mounts, std::optional<Device> devOverride = std::nullopt,
	          const FileOps& ops = {});

	Device saved() const { return saved_; }       // the stored choice (Sd or Usb)
	Device resolved() const { return resolved_; } // what is in use this session
	Fallback fallback() const { return fallback_; }
	bool overridden() const { return overridden_; }
	// The resolved device's root, BY VALUE (set() reassigns it, so a reference would dangle). Empty for None: check
	// hasAppsDevice() and never concatenate onto an empty root ("" + "/apps/x" would be a path on the filesystem root).
	// Jobs copy the root into their JobSpec at queue time (Increments 8/9) and never re-read it later.
	std::string root() const { return root_; }
	bool hasAppsDevice() const { return resolved_ != Device::None; }
	// Bumped whenever the resolved device changes (init counts). Increment 11's scan and queued jobs remember the epoch
	// they started under and drop their result when it has moved on.
	unsigned epoch() const { return epoch_; }

	// Increment 7 plugs in `[]{ return Jobs::busy(); }` (any queued or running job). With no predicate set the setting
	// is never busy; App installs a logging placeholder at init so the wiring point is explicit.
	void setBusy(BusyFn f) { busy_ = std::move(f); }

	// Saves the choice, then switches to it (see SetResult). Refuses while busy, for an unknown device, or when the device
	// is not mounted. Choosing the device already in use and already stored changes nothing and writes nothing. A failed
	// save is logged and the switch still happens for this session. Increment 11 adds the re-scan.
	// In the UsbMissing fallback (saved usb, session on the SD card) set(Sd) overwrites the saved usb with sd: that is the
	// user's explicit choice. M4's row shows resolved(), not saved().
	SetResult set(Device dev, std::string* reason = nullptr);

private:
	std::string iniPath_;
	Mounts mounts_;
	FileOps ops_;
	BusyFn busy_;
	Device saved_ = Device::Sd, resolved_ = Device::None;
	Device persisted_ = Device::Sd; // what the file holds (Sd when missing or garbage); differs from saved_ after a failed save
	unsigned epoch_ = 0;
	Fallback fallback_ = Fallback::None;
	bool overridden_ = false;
	std::string root_;
	void apply(Device d);
};

} // namespace install
