// SPDX-License-Identifier: GPL-3.0-or-later
// Host tests for the pure install modules. Build and run: make -f Makefile.pc test
// Increment 1: the vendored MD5 and the libzip link.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <zip.h>

#include "Check.h"
#include "core/DevOptions.h"
#include "core/FileUtil.h"
#include "install/AppsDevice.h"
#include "third_party/md5/md5.h"

static std::string hex(const unsigned char d[16]) {
	static const char* digits = "0123456789abcdef";
	std::string s;
	for (int i = 0; i < 16; i++) { s += digits[d[i] >> 4]; s += digits[d[i] & 15]; }
	return s;
}

static std::string md5Of(const void* data, unsigned long n) {
	MD5_CTX c;
	unsigned char d[16];
	MD5_Init(&c);
	MD5_Update(&c, data, n);
	MD5_Final(d, &c);
	return hex(d);
}

static std::string md5Of(const std::string& s) { return md5Of(s.data(), s.size()); }

static void testMd5Vectors() {
	// RFC 1321 appendix A.5.
	CHECK(md5Of("") == "d41d8cd98f00b204e9800998ecf8427e");
	CHECK(md5Of("a") == "0cc175b9c0f1b6a831c399e269772661");
	CHECK(md5Of("abc") == "900150983cd24fb0d6963f7d28e17f72");
	CHECK(md5Of("message digest") == "f96b697d7cb7938d525a2f31aaf161d0");
	CHECK(md5Of("abcdefghijklmnopqrstuvwxyz") == "c3fcd3d76192e4007dfb496cca67e13b");
	CHECK(md5Of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789") == "d174ab98d277d9f5a5611c2c9f419d9f");
	CHECK(md5Of("12345678901234567890123456789012345678901234567890123456789012345678901234567890") ==
	      "57edf4a22be3c955ac49da2e2107b67a");
	// Padding edge cases: 55, 56, 63, 64 and 65 bytes straddle the length-field boundary.
	CHECK(md5Of(std::string(55, 'a')) == "ef1772b6dff9a122358552954ad0df65");
	CHECK(md5Of(std::string(56, 'a')) == "3b0c8ac703f828b04c6c197006d17218");
	CHECK(md5Of(std::string(63, 'a')) == "b06521f39153d618550606be297466d5");
	CHECK(md5Of(std::string(64, 'a')) == "014842d480b571495a4a0363793f7367");
	CHECK(md5Of(std::string(65, 'a')) == "c743a45e0d2e6a95cb859adae0248435");
	// One million 'a' (the classic long vector).
	CHECK(md5Of(std::string(1000000, 'a')) == "7707d6ae4e027c70eea2a935c2296f21");
}

static void testMd5Chunked() {
	std::vector<unsigned char> buf(1 << 20);
	unsigned x = 12345;
	for (auto& b : buf) { x = x * 1664525u + 1013904223u; b = static_cast<unsigned char>(x >> 24); }
	const std::string whole = md5Of(buf.data(), buf.size());

	MD5_CTX c;
	unsigned char d[16];
	MD5_Init(&c);
	for (size_t off = 0; off < buf.size(); off += 65536) MD5_Update(&c, buf.data() + off, 65536);
	MD5_Final(d, &c);
	CHECK(hex(d) == whole);

	// Odd chunk sizes (never block-aligned) must agree too.
	MD5_Init(&c);
	size_t off = 0, step = 1;
	while (off < buf.size()) {
		size_t n = step < buf.size() - off ? step : buf.size() - off;
		MD5_Update(&c, buf.data() + off, n);
		off += n;
		step = step * 3 % 4099 + 1;
	}
	MD5_Final(d, &c);
	CHECK(hex(d) == whole);
}

static void testLibzipLinked() {
	const char* v = zip_libzip_version();
	CHECK(v != nullptr && v[0] >= '1' && v[0] <= '9');
	std::printf("libzip %s\n", v ? v : "(null)");
}

// ---- Increment 2b: apps device ------------------------------------------------------------------------------------------
using install::Device;
using install::Fallback;

static std::string tmpDir() {
	char tmpl[] = "/tmp/openshop_appsdev_XXXXXX";
	if (!mkdtemp(tmpl)) {
		std::fprintf(stderr, "FATAL: mkdtemp failed\n");
		std::exit(1); // a hard failure: never fall back to a shared directory
	}
	return tmpl;
}

static std::string slurp(const std::string& p) {
	std::string s;
	readFileCapped(p, s, 4096);
	return s;
}

static void testResolver() {
	auto r = [](Device saved, bool sd, bool usb) { return install::resolveAppsDevice(saved, sd, usb); };
	CHECK(r(Device::Sd, true, true).device == Device::Sd && r(Device::Sd, true, true).fallback == Fallback::None);
	CHECK(r(Device::Sd, true, false).device == Device::Sd && r(Device::Sd, true, false).fallback == Fallback::None);
	CHECK(r(Device::Usb, true, true).device == Device::Usb && r(Device::Usb, true, true).fallback == Fallback::None);
	CHECK(r(Device::Usb, false, true).device == Device::Usb && r(Device::Usb, false, true).fallback == Fallback::None);
	CHECK(r(Device::Usb, true, false).device == Device::Sd && r(Device::Usb, true, false).fallback == Fallback::UsbMissing);
	CHECK(r(Device::Sd, false, true).device == Device::Usb && r(Device::Sd, false, true).fallback == Fallback::NoSd);
	CHECK(r(Device::Sd, false, false).device == Device::None);
	CHECK(r(Device::Usb, false, false).device == Device::None);
	CHECK(std::string(install::deviceLabel(Device::Sd)) == "SD card" && std::string(install::deviceLabel(Device::Usb)) == "USB drive");
	CHECK(install::parseDeviceId(" USB\r") == Device::Usb && install::parseDeviceId("sd") == Device::Sd);
	CHECK(!install::parseDeviceId("floppy") && !install::parseDeviceId(""));
}

static void testAppsDeviceStorage() {
	std::string dir = tmpDir(), ini = dir + "/appsdevice.ini";
	CHECK(install::loadSavedDevice(ini) == Device::Sd); // missing
	bool wrote = false;
	CHECK(install::saveDeviceIfChanged(ini, Device::Usb, &wrote) && wrote);
	CHECK(slurp(ini) == "appsDevice=usb\n");
	CHECK(install::loadSavedDevice(ini) == Device::Usb);
	// Unchanged: no write (an extra comment line survives).
	writeFileAtomic(ini, "# keep\nappsDevice=usb\n");
	CHECK(install::saveDeviceIfChanged(ini, Device::Usb, &wrote) && !wrote);
	CHECK(slurp(ini) == "# keep\nappsDevice=usb\n");
	CHECK(install::saveDeviceIfChanged(ini, Device::Sd, &wrote) && wrote);
	CHECK(install::loadSavedDevice(ini) == Device::Sd);
	writeFileAtomic(ini, "appsDevice=floppy\n");
	CHECK(install::loadSavedDevice(ini) == Device::Sd);
	writeFileAtomic(ini, "nonsense\n");
	CHECK(install::loadSavedDevice(ini) == Device::Sd);
	CHECK(!install::saveDeviceIfChanged(ini, Device::None, &wrote) && !wrote);
	removeTree(ini, dir, {"/tmp"});
	removeDirIfEmpty(dir);
}

static void testAppsDeviceSetting() {
	using S = install::AppsDeviceSetting;
	std::string dir = tmpDir(), ini = dir + "/appsdevice.ini";
	S::Mounts both{true, true, "./sdcard", "./usbdrive"}, sdOnly{true, false, "./sdcard", ""}, usbOnly{false, true, "", "./usbdrive"};

	// Saved usb, no USB: SD this session, the saved value stays usb on disk.
	writeFileAtomic(ini, "appsDevice=usb\n");
	S a;
	a.init(ini, sdOnly);
	CHECK(a.resolved() == Device::Sd && a.saved() == Device::Usb && a.fallback() == Fallback::UsbMissing && a.root() == "./sdcard");
	CHECK(slurp(ini) == "appsDevice=usb\n");
	// No SD: the USB drive.
	S b;
	b.init(ini, usbOnly);
	CHECK(b.resolved() == Device::Usb && b.root() == "./usbdrive");
	S n;
	n.init(ini, S::Mounts{});
	CHECK(n.resolved() == Device::None && n.root().empty());
	std::string why;
	CHECK(n.set(Device::Sd, &why) == S::SetResult::Refused && why == "No SD card found");

	// Override: forces usb when mounted, never saved; ignored when not mounted.
	writeFileAtomic(ini, "appsDevice=sd\n");
	S o;
	o.init(ini, both, Device::Usb);
	CHECK(o.resolved() == Device::Usb && o.root() == "./usbdrive" && o.overridden() && o.saved() == Device::Sd);
	CHECK(slurp(ini) == "appsDevice=sd\n");
	S o2;
	o2.init(ini, sdOnly, Device::Usb);
	CHECK(o2.resolved() == Device::Sd && !o2.overridden() && slurp(ini) == "appsDevice=sd\n");

	// set(): refused while busy (injected predicate), refused for an unmounted device, saves otherwise.
	S c;
	bool busy = true;
	c.init(ini, both);
	c.setBusy([&] { return busy; });
	CHECK(c.set(Device::Usb, &why) == S::SetResult::Refused && why == "Finish or cancel downloads first");
	CHECK(c.resolved() == Device::Sd && slurp(ini) == "appsDevice=sd\n");
	busy = false;
	CHECK(c.set(Device::Usb, &why) == S::SetResult::Switched && c.resolved() == Device::Usb && c.saved() == Device::Usb && c.root() == "./usbdrive");
	CHECK(slurp(ini) == "appsDevice=usb\n");
	CHECK(c.set(Device::Usb) == S::SetResult::Switched); // same device: no write
	S d;
	d.init(ini, sdOnly); // usb saved, SD fallback
	CHECK(d.set(Device::Usb, &why) == S::SetResult::Refused && why == "No USB drive found" && slurp(ini) == "appsDevice=usb\n");
	CHECK(d.set(Device::Sd) == S::SetResult::Switched && d.fallback() == Fallback::None && slurp(ini) == "appsDevice=sd\n");
	removeTree(ini, dir, {"/tmp"});
	removeDirIfEmpty(dir);
}

static void testAppsDeviceDevOptions() {
	DevOptions o;
	DevOptions::parseFile("appsdevice=USB\nappsdeviceswap=1\n", o);
	CHECK(o.appsDevice == "USB" && install::parseDeviceId(o.appsDevice) == Device::Usb && o.appsDeviceSwap);
	CHECK(o.active().find("appsdevice=USB") != std::string::npos && o.active().find("appsdeviceswap=1") != std::string::npos);
	DevOptions p;
	CHECK(p.appsDevice.empty() && !p.appsDeviceSwap && p.active().empty());
	// The environment path of load() (an env value beats the devstart file).
	std::string dir = tmpDir();
	writeFileAtomic(dir + "/devstart", "appsdevice=sd\n");
	setenv("OPENSHOP_APPS_DEVICE", " Usb ", 1);
	setenv("OPENSHOP_APPS_DEVICE_SWAP", "1", 1);
	DevOptions e = DevOptions::load(dir);
	CHECK(e.appsDevice == "Usb" && install::parseDeviceId(e.appsDevice) == Device::Usb && e.appsDeviceSwap);
	unsetenv("OPENSHOP_APPS_DEVICE");
	unsetenv("OPENSHOP_APPS_DEVICE_SWAP");
	DevOptions f = DevOptions::load(dir);
	CHECK(f.appsDevice == "sd" && !f.appsDeviceSwap);
	removeTree(dir + "/devstart", dir, {"/tmp"});
	removeDirIfEmpty(dir);
}

static void testAppsDeviceMore() {
	using S = install::AppsDeviceSetting;
	std::string dir = tmpDir(), ini = dir + "/appsdevice.ini", why;
	S::Mounts both{true, true, "./sdcard", "./usbdrive"}, sdOnly{true, false, "./sdcard", ""}, usbOnly{false, true, "", "./usbdrive"};

	// Override edge cases.
	writeFileAtomic(ini, "appsDevice=usb\n");
	S a;
	a.init(ini, both, Device::Sd);
	CHECK(a.resolved() == Device::Sd && a.fallback() == Fallback::None && a.overridden() && a.saved() == Device::Usb);
	S b;
	b.init(ini, usbOnly, Device::Sd); // override to an unmounted device (saved usb, only USB mounted: no fallback notice)
	CHECK(b.resolved() == Device::Usb && b.fallback() == Fallback::None && !b.overridden());
	S c;
	c.init(ini, both, std::optional<Device>(Device::None)); // invalid override
	CHECK(c.resolved() == Device::Usb && !c.overridden());
	S c2;
	c2.init(ini, both, std::optional<Device>(static_cast<Device>(77)));
	CHECK(c2.resolved() == Device::Usb && !c2.overridden());
	// Saved usb and only USB mounted: USB, and no fallback (NoSd is only for a saved sd).
	S d;
	d.init(ini, usbOnly);
	CHECK(d.resolved() == Device::Usb && d.fallback() == Fallback::None && d.saved() == Device::Usb);
	writeFileAtomic(ini, "appsDevice=sd\n");
	S d2;
	d2.init(ini, usbOnly);
	CHECK(d2.resolved() == Device::Usb && d2.fallback() == Fallback::NoSd && d2.saved() == Device::Sd);
	writeFileAtomic(ini, "appsDevice=usb\n");
	CHECK(slurp(ini) == "appsDevice=usb\n");

	// Garbage file, then set(Sd) (already the effective value) and set(Usb) (overwrites).
	writeFileAtomic(ini, "garbage\n");
	S g;
	g.init(ini, both);
	CHECK(g.saved() == Device::Sd && g.set(Device::Sd) == S::SetResult::Switched);
	CHECK(g.set(Device::Usb) == S::SetResult::Switched && slurp(ini) == "appsDevice=usb\n");

	// set(Sd) in the UsbMissing fallback overwrites the saved usb.
	S f;
	f.init(ini, sdOnly);
	CHECK(f.fallback() == Fallback::UsbMissing && f.saved() == Device::Usb);
	CHECK(f.set(Device::Sd) == S::SetResult::Switched && f.saved() == Device::Sd && f.fallback() == Fallback::None);
	CHECK(slurp(ini) == "appsDevice=sd\n");

	// root() is by value, hasAppsDevice and the epoch.
	S e;
	e.init(ini, S::Mounts{});
	CHECK(!e.hasAppsDevice() && e.root().empty());
	S h;
	h.init(ini, both);
	unsigned ep = h.epoch();
	CHECK(h.set(Device::Sd) == S::SetResult::Switched && h.epoch() == ep); // nothing changed
	CHECK(h.set(Device::Usb) == S::SetResult::Switched && h.epoch() == ep + 1);
	CHECK(h.set(Device::Usb) == S::SetResult::Switched && h.epoch() == ep + 1);
	CHECK(h.set(Device::Sd) == S::SetResult::Switched && h.epoch() == ep + 2 && h.hasAppsDevice());
	CHECK(h.set(Device::Usb, nullptr) == S::SetResult::Switched);
	ep = h.epoch();
	CHECK(h.set(Device::None) == S::SetResult::Refused && h.epoch() == ep);

	// A failing save: switched for the session, reported, and the next call retries the write.
	writeFileAtomic(ini, "appsDevice=sd\n");
	FileOps bad;
	bad.rename = [](const char*, const char*) { return -1; };
	S k;
	k.init(ini, both, std::nullopt, bad);
	CHECK(k.set(Device::Usb, &why) == S::SetResult::SwitchedNotSaved);
	CHECK(k.resolved() == Device::Usb && k.saved() == Device::Usb && k.root() == "./usbdrive");
	CHECK(slurp(ini) == "appsDevice=sd\n");
	CHECK(k.set(Device::Usb) == S::SetResult::SwitchedNotSaved); // retried, still failing
	S k2;
	k2.init(ini, both); // the real file ops: simulate the retry succeeding
	CHECK(k2.set(Device::Usb) == S::SetResult::Switched && slurp(ini) == "appsDevice=usb\n");
	// Same object: failure first, then working ops.
	writeFileAtomic(ini, "appsDevice=sd\n");
	static int fails = 100; // commitFile may retry the rename, so fail every call until told otherwise
	FileOps flaky;
	flaky.rename = [](const char* a2, const char* b2) { return fails-- > 0 ? -1 : ::rename(a2, b2); };
	S m;
	m.init(ini, both, std::nullopt, flaky);
	CHECK(m.set(Device::Usb) == S::SetResult::SwitchedNotSaved && slurp(ini) == "appsDevice=sd\n");
	fails = 0;
	CHECK(m.set(Device::Usb) == S::SetResult::Switched && slurp(ini) == "appsDevice=usb\n");
	CHECK(m.set(Device::Usb) == S::SetResult::Switched); // now really nothing to do

	// Ini parsing: BOM, CRLF, duplicate keys (the last wins), an unterminated last line.
	writeFileAtomic(ini, "\xEF\xBB\xBF" "appsDevice=usb\r\n");
	CHECK(install::loadSavedDevice(ini) == Device::Usb);
	writeFileAtomic(ini, "# c\r\nAppsDevice = USB \r\n");
	CHECK(install::loadSavedDevice(ini) == Device::Usb);
	writeFileAtomic(ini, "appsDevice=usb\nappsDevice=sd\n");
	CHECK(install::loadSavedDevice(ini) == Device::Sd);
	writeFileAtomic(ini, "appsDevice=sd\nappsDevice=usb\n");
	CHECK(install::loadSavedDevice(ini) == Device::Usb);
	writeFileAtomic(ini, "x=1\nappsDevice=usb"); // no final newline
	CHECK(install::loadSavedDevice(ini) == Device::Usb);
	removeTree(ini, dir, {"/tmp"});
	removeDirIfEmpty(dir);
}

int main() {
	testMd5Vectors();
	testMd5Chunked();
	testLibzipLinked();
	testResolver();
	testAppsDeviceStorage();
	testAppsDeviceSetting();
	testAppsDeviceDevOptions();
	testAppsDeviceMore();
	return checkReport("install_test");
}
