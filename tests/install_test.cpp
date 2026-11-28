// SPDX-License-Identifier: GPL-3.0-or-later
// Host tests for the pure install modules. Build and run: make -f Makefile.pc test
// Increment 1: the vendored MD5 and the libzip link.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <zip.h>

#include "Check.h"
#include "core/DevOptions.h"
#include "core/FileUtil.h"
#include "catalog/Catalog.h"
#include "catalog/OscApi.h"
#include "install/AppsDevice.h"
#include "install/JobQueue.h"
#include "install/Journal.h"
#include "install/Manifest.h"
#include "install/Md5Sink.h"
#include "install/Messages.h"
#include "install/MetaXml.h"
#include "install/Preflight.h"
#include "net/NetState.h"
#include "install/RecordIo.h"
#include "install/Rules.h"
#include "install/Version.h"
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


// ---- Increment 3: pure install rules ----

static bool nameOk(const std::string& n) { return install::validateEntryName(n).ok; }

static void testEntryNames() {
	using install::NameReject;
	const char* bad[] = {"../x", "apps/../../x", "/apps/x", "\\apps\\x", "apps\\foo\\boot.dol", "sd:/apps/x", "usb:apps/x",
	                     "C:x", "apps/a:b", "apps/a*b", "apps/a\x01" "b", "apps/a\x7f" "b", "", "apps//x", "apps/x./y",
	                     "apps/x /y", "apps/x/CON.txt", "apps/x/lpt1", "apps/\xc0\xaf", "apps/./x", "apps/..", ".", "..",
	                     "/", "apps/a?b", "apps/a\"b", "apps/a<b", "apps/a>b", "apps/a|b", "apps/x/NUL", "apps/x/com9.dol",
	                     "apps/x/aux.", "apps/x/prn .txt", "a//", "apps/x\\", "apps/a/b\\c"};
	for (const char* b : bad) {
		std::string n(b);
		install::EntryName e = install::validateEntryName(n);
		if (e.ok) std::printf("accepted but should be rejected: '%s'\n", b);
		CHECK(!e.ok);
		CHECK(e.reason != NameReject::None && e.segments.empty() && e.relPath.empty());
	}
	// Specific reasons.
	CHECK(install::validateEntryName("a\\b").reason == NameReject::Backslash);
	CHECK(install::validateEntryName("/a").reason == NameReject::Absolute);
	CHECK(install::validateEntryName("sd:/a").reason == NameReject::DrivePrefix);
	CHECK(install::validateEntryName("usb:apps/x").reason == NameReject::DrivePrefix);
	CHECK(install::validateEntryName("apps/a:b").reason == NameReject::BadChar);
	CHECK(install::validateEntryName("a/../b").reason == NameReject::BadSegment);
	CHECK(install::validateEntryName("a/b./c").reason == NameReject::TrailingDotSpace);
	CHECK(install::validateEntryName("apps/\xc0\xaf").reason == NameReject::BadUtf8);

	// Invalid UTF-8: overlong, surrogate, above U+10FFFF, truncated, stray continuation, 0xF5+.
	CHECK(!nameOk("apps/\xe0\x80\xaf"));
	CHECK(!nameOk("apps/\xed\xa0\x80"));
	CHECK(!nameOk("apps/\xf4\x90\x80\x80"));
	CHECK(!nameOk("apps/\xf0\x80\x80\xaf"));
	CHECK(!nameOk("apps/\xe2\x82"));
	CHECK(!nameOk("apps/\x80"));
	CHECK(!nameOk("apps/\xf5\x80\x80\x80"));
	CHECK(nameOk("apps/caf\xc3\xa9/boot.dol"));          // U+00E9
	CHECK(nameOk("apps/\xe2\x82\xac/x"));                // U+20AC
	CHECK(!nameOk("apps/\xf0\x9f\x8e\xae/x"));           // U+1F3AE: above U+FFFF is refused (see testUnicodeHardening)

	// Length: 240 bytes is fine, 241 is not (the name as given, including a directory slash).
	std::string n240 = "apps/" + std::string(235, 'a');
	CHECK(n240.size() == 240 && nameOk(n240));
	CHECK(!nameOk(n240 + "a"));
	CHECK(install::validateEntryName(n240 + "a").reason == NameReject::TooLong);
	CHECK(!nameOk(n240 + "/"));
	// Segments: 16 fine, 17 not.
	std::string s16, s17;
	for (int i = 0; i < 16; i++) s16 += (i ? "/" : "") + std::string("d");
	s17 = s16 + "/d";
	CHECK(nameOk(s16) && !nameOk(s17));
	CHECK(install::validateEntryName(s17).reason == NameReject::TooDeep);
	CHECK(nameOk(s16 + "/") && !nameOk(s17 + "/"));

	// Reserved names: the part before the first dot, any case, trailing spaces dropped; near misses are fine.
	CHECK(!nameOk("CON") && !nameOk("Con.TXT") && !nameOk("a/NUL.tar.gz") && !nameOk("LPT9") && !nameOk("COM1.x"));
	CHECK(nameOk("COM0") && nameOk("LPT10") && nameOk("console.txt") && nameOk("a/nul0") && nameOk("CONX"));

	// Deliberate addition (consistency with FileUtil's guard, increment 2a): the FAT alias pattern "~<digit>".
	CHECK(!nameOk("apps/OPENSH~1/boot.dol") && !nameOk("apps/a~1") && !nameOk("apps/~9x/y") && !nameOk("x/b~0.dol"));
	CHECK(install::validateEntryName("apps/a~1").reason == NameReject::FatAlias);
	CHECK(nameOk("apps/a~b/x") && nameOk("apps/~/x") && nameOk("apps/a~") && nameOk("apps/~a"));

	// Accepted names and what they report.
	install::EntryName e = install::validateEntryName("apps/foo/boot.dol");
	CHECK(e.ok && !e.isDir && e.relPath == "apps/foo/boot.dol" && e.segments.size() == 3 && e.segments[1] == "foo");
	e = install::validateEntryName("apps/Foo Bar/meta.xml");
	CHECK(e.ok && e.segments[1] == "Foo Bar");
	CHECK(nameOk("config/foo/x.ini"));
	e = install::validateEntryName("apps/foo/");
	CHECK(e.ok && e.isDir && e.relPath == "apps/foo" && e.segments.size() == 2);
	CHECK(!nameOk(" lead/x") && nameOk("a.b/c.d") && nameOk(".hidden/x") && nameOk("a b/c"));
	CHECK(!nameOk("apps//") && !nameOk("//"));
}

static void testUnicodeHardening() {
	using install::NameReject;
	auto reason = [](const std::string& n) { return install::validateEntryName(n).reason; };
	// Above U+FFFF (4-byte UTF-8), including both ends of the range.
	CHECK(reason("apps/\xf0\x9f\x98\x80") == NameReject::BadUnicode);   // U+1F600
	CHECK(reason("apps/\xf0\x90\x80\x80") == NameReject::BadUnicode);   // U+10000
	CHECK(reason("apps/\xf4\x8f\xbf\xbf") == NameReject::BadUnicode);   // U+10FFFF
	CHECK(reason("apps/a\xf0\x90\x80\xaf" "b") == NameReject::BadUnicode); // U+1002F, low 16 bits are '/'
	CHECK(reason("apps/\xf0\x90\x81\xb0") == NameReject::BadUnicode);   // U+10070, low 16 bits are 'p'
	// Noncharacters.
	CHECK(reason("apps/\xef\xbf\xbe") == NameReject::BadUnicode);        // U+FFFE
	CHECK(reason("apps/\xef\xbf\xbf") == NameReject::BadUnicode);        // U+FFFF
	CHECK(nameOk("apps/\xef\xbf\xbd"));                                 // U+FFFD is an ordinary (replacement) character
	// C1 controls U+0080..U+009F.
	CHECK(reason("apps/a\xc2\x80" "b") == NameReject::BadUnicode);
	CHECK(reason("apps/a\xc2\x85" "b") == NameReject::BadUnicode);
	CHECK(reason("apps/a\xc2\x9f" "b") == NameReject::BadUnicode);
	CHECK(nameOk("apps/a\xc2\xa0" "b") && nameOk("apps/a\xc2\xa1")); // U+00A0, U+00A1 are outside the range
	// Invisible and bidi characters.
	for (unsigned cp : {0x200Bu, 0x200Cu, 0x200Du, 0x200Eu, 0x200Fu, 0x2028u, 0x2029u, 0x202Au, 0x202Bu, 0x202Cu, 0x202Du,
	                    0x202Eu, 0x2060u, 0x2061u, 0x2062u, 0x2063u, 0x2064u, 0xFEFFu}) {
		char b[4] = {static_cast<char>(0xE0 | (cp >> 12)), static_cast<char>(0x80 | ((cp >> 6) & 0x3F)),
		             static_cast<char>(0x80 | (cp & 0x3F)), 0};
		CHECK(reason(std::string("apps/a") + b + "b") == NameReject::BadUnicode);
		CHECK(reason(std::string(b) + "apps/x") == NameReject::BadUnicode);
	}
	CHECK(nameOk("apps/a\xe2\x80\xa4" "b") && nameOk("apps/a\xe2\x80\x8a") && nameOk("apps/a\xe2\x80\xaf")); // U+2024, U+200A, U+202F
	CHECK(nameOk("apps/a\xe2\x81\xa5") && nameOk("apps/a\xe2\x81\x9f")); // U+2065 and U+205F are outside the ranges
	// Case-fold look-alikes.
	CHECK(reason("apps/\xc4\xb0") == NameReject::BadUnicode);             // U+0130
	CHECK(reason("apps/\xc4\xb1") == NameReject::BadUnicode);             // U+0131
	CHECK(reason("apps/\xc5\xbf") == NameReject::BadUnicode);             // U+017F
	CHECK(reason("apps/\xe2\x84\xaa") == NameReject::BadUnicode);        // U+212A Kelvin
	CHECK(reason("apps/\xe2\x84\xab") == NameReject::BadUnicode);        // U+212B Angstrom
	CHECK(nameOk("apps/\xc3\x85") && nameOk("apps/\xc4\xb2") && nameOk("apps/\xc5\xbe")); // U+00C5, U+0132, U+017E
	// A segment that starts with a space (any segment, not just the first).
	CHECK(reason(" apps/x") == NameReject::LeadingSpace);
	CHECK(reason("apps/ x") == NameReject::LeadingSpace);
	CHECK(reason("apps/ x/y") == NameReject::LeadingSpace);
	CHECK(reason("a/ /b") == NameReject::TrailingDotSpace); // a lone space also ends in one
	CHECK(nameOk("apps/a b/c d") && nameOk("apps/x y"));
	// Ordinary accented Latin and CJK still pass.
	CHECK(nameOk("apps/caf\xc3\xa9/boot.dol") && nameOk("apps/\xc3\x9c" "ber/\xc3\xb1.dol"));
	CHECK(nameOk("apps/\xe3\x83\x9e\xe3\x83\xaa\xe3\x82\xaa/boot.dol"));          // katakana
	CHECK(nameOk("apps/\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e/x.dol"));            // CJK ideographs
	CHECK(nameOk("apps/\xed\x95\x9c\xea\xb8\x80/x") && nameOk("apps/\xe2\x82\xac/x")); // Hangul, euro sign
	CHECK(nameOk("apps/e\xcc\x81/x"));                                                // combining acute

	// Every NameReject value has its own non-empty description.
	std::set<std::string> seen;
	for (int i = 0; i <= static_cast<int>(NameReject::TooDeep); i++) {
		const char* d = install::describe(static_cast<NameReject>(i));
		CHECK(d && *d);
		CHECK(seen.insert(d ? d : "").second);
	}
	CHECK(seen.size() == static_cast<size_t>(NameReject::TooDeep) + 1);
}

struct NameRow { const char* label; std::string name; bool ok; };

// Inputs from the independent review of Increment 3 (name rules).
static void testReviewerNames() {
	std::string a240(240, 'a'), a241(241, 'a'), s17, s16, s16d;
	for (int i = 0; i < 16; i++) s17 += "a/";
	s17 += "b";
	for (int i = 0; i < 15; i++) s16 += "a/";
	s16d = s16 + "b/";
	s16 += "b";
	const std::vector<NameRow> rows = {
	    {"a/b", "apps/a/b", true},
	    {"embedded NUL", std::string("apps/a\0b", 8), false},
	    {"trailing NUL", std::string("apps/a\0", 7), false},
	    {"%2e%2e is just text", "apps/%2e%2e/x", true},
	    {"%2F is just text", "apps/a%2Fb", true},
	    {"overlong /", "apps/a\xC0\xAF" "b", false},
	    {"overlong E0 80 AF", "apps/\xE0\x80\xAF", false},
	    {"surrogate", "apps/\xED\xA0\x80", false},
	    {"U+FFFE", "apps/\xEF\xBF\xBE", false},
	    {"U+FFFF", "apps/\xEF\xBF\xBF", false},
	    {"nbsp trailing", "apps/a\xC2\xA0", true},
	    {"U+2024 one-dot leader", "apps/a\xE2\x80\xA4", true},
	    {"fullwidth solidus", "apps/a\xEF\xBC\x8F" "b", true},
	    {"fullwidth dot dot", "\xEF\xBC\x8E\xEF\xBC\x8E/x", true},
	    {"U+2215 division slash", "a\xE2\x88\x95" "b", true},
	    {"C1 U+0085", "apps/a\xC2\x85" "b", false},
	    {"LRO U+202E", "apps/a\xE2\x80\xAE" "b", false},
	    {"U+0000 as C0 80", "apps/\xC0\x80", false},
	    {"combining", "apps/e\xCC\x81", true},
	    {"BOM", "\xEF\xBB\xBF" "apps/x", false},
	    {"ZWSP trailing", "apps/a\xE2\x80\x8B", false},
	    {"4-byte emoji", "apps/\xF0\x9F\x98\x80", false},
	    {"U+10070 (low16 'p')", "apps/\xF0\x90\x81\xB0", false},
	    {"U+1002F (low16 '/')", "apps/a\xF0\x90\x80\xAF" "b", false},
	    {"leading space", " apps/x", false},
	    {"segment leading space", "apps/ x", false},
	    {"leading dot", ".hidden/x", true},
	    {"apps/./x", "apps/./x", false},
	    {"tilde1", "apps/PROGRA~1", false},
	    {"tilde at end", "apps/a~", true},
	    {"~a", "apps/~a", true},
	    {"a~~1", "apps/a~~1", false},
	    {"con.x.y", "apps/con.x.y", false},
	    {"CON  .txt", "apps/con  .txt", false},
	    {"conx", "apps/conx", true},
	    {"COM0", "apps/com0", true},
	    {"COM10", "apps/com10", true},
	    {"CLOCK$", "apps/CLOCK$", true},
	    {"CONIN$", "apps/CONIN$", true},
	    {"LPT superscript one", "apps/LPT\xC2\xB9", true},
	    {"dir slash", "apps/a/", true},
	    {"dir //", "apps/a//", false},
	    {"slash only", "/", false},
	    {"a/ /b", "a/ /b", false},
	    {"dir end space", "a /", false},
	    {"dir end dot", "a./", false},
	    {"ADS a:b", "a/b:c", false},
	    {"colon at end of dir", "a/b:", false},
	    {"tab", "a\tb", false},
	    {"DEL", "a\x7f", false},
	    {"brackets etc", "a/[b]=,;+", true},
	    {"!", "a/!x", true},
	    {"%41", "a/%41", true},
	    {"^", "a/^x", true},
	    {"{}", "a/{x}", true},
	    {"apostrophe", "a/it's", true},
	    {"Private", "Private/x", true},
	    {"private", "private/x", true},
	    {"empty", "", false},
	    {"240 bytes", a240, true},
	    {"241 bytes", a241, false},
	    {"17 segments", s17, false},
	    {"16 segments", s16, true},
	    {"16 segments, dir", s16d, true},
	    {"short ext .tmp", "apps/x/boot.dol.tmp", true},
	    {"OPENSHOP caseless", "apps/OPENSHOP/x", true},
	    {"apps", "apps", true},
	};
	for (const NameRow& r : rows) {
		bool got = install::validateEntryName(r.name).ok;
		if (got != r.ok) std::printf("name row '%s': expected %s\n", r.label, r.ok ? "accept" : "reject");
		CHECK(got == r.ok);
	}
}

static void testFoldAndRoots() {
	CHECK(install::isAppsRoot("apps") && install::isAppsRoot("APPS") && install::isAppsRoot("Apps") && install::isAppsRoot("aPPs"));
	CHECK(!install::isAppsRoot("app") && !install::isAppsRoot("apps2") && !install::isAppsRoot("") && !install::isAppsRoot("apps/") &&
	      !install::isAppsRoot("xapps"));

	CHECK(install::foldKey("apps/WiiMC/x") == install::foldKey("apps/wiimc/X"));
	CHECK(install::foldKey("apps/a") != install::foldKey("apps/b"));
	CHECK(install::foldKey("\xc3\x89") == "\xc3\x89"); // non-ASCII bytes are untouched
	CHECK(install::foldKey("") == "");

	const char* roots[] = {"private", "title", "shared2", "ticket", "sys", "import", "meta", "tmp"};
	for (const char* r : roots) {
		CHECK(install::isSystemRoot(r));
		std::string up = r;
		for (char& c : up) c = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
		CHECK(install::isSystemRoot(up));
	}
	CHECK(install::isSystemRoot("Private") && install::isSystemRoot("sYs"));
	CHECK(!install::isSystemRoot("apps") && !install::isSystemRoot("") && !install::isSystemRoot("privat") &&
	      !install::isSystemRoot("private2") && !install::isSystemRoot("MCBACKUP") && !install::isSystemRoot("priv/ate"));
	CHECK(install::isOwnRoot("apps/OpenShop") && install::isOwnRoot("apps/openshop/staging/1") && install::isOwnRoot("/APPS/OPENSHOP/") &&
	      install::isOwnRoot("/apps/OpenShop"));
	CHECK(!install::isOwnRoot("apps/OpenShop2") && !install::isOwnRoot("apps") && !install::isOwnRoot("OpenShop") &&
	      !install::isOwnRoot("apps/foo/OpenShop") && !install::isOwnRoot("") && !install::isOwnRoot("other/apps/OpenShop"));
	CHECK(install::isJunkRoot("__MACOSX") && install::isJunkRoot("__macosx") && !install::isJunkRoot("MACOSX") && !install::isJunkRoot(""));

	std::vector<std::string> in = {"/", "/apps", "/apps/OpenShop", "/apps/foo", "/private", "/Private/x", "/title", "/shared2",
	                               "/ticket", "/sys", "/import", "/meta", "/tmp", "/__MACOSX", "/MCBACKUP", "/apps/openshop/x",
	                               "/apps/OpenShop2", "/WiiFlow", "/privatex"};
	std::vector<std::string> out = install::filterSubdirHint(in);
	std::vector<std::string> want = {"/apps/foo", "/MCBACKUP", "/apps/OpenShop2", "/WiiFlow", "/privatex"};
	CHECK(out == want);
	CHECK(install::filterSubdirHint({}).empty());
	CHECK(install::filterSubdirHint({"/apps/OpenShop", "/APPS/openshop/x", "/"}).empty());
}

static std::string readAll(const char* path) {
	std::ifstream f(path, std::ios::binary);
	if (!f) { std::fprintf(stderr, "cannot read %s (run from the repo root)\n", path); std::exit(2); }
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

static void testFixtureSweep() {
	std::string json = readAll("tests/fixtures/contents.json");
	Catalog cat;
	CHECK(oscapi::parseContents(json, cat).ok());
	CHECK(cat.apps.size() == 303);
	std::set<std::string> dropped, kept;
	size_t total = 0, droppedOcc = 0;
	for (const AppEntry& a : cat.apps) {
		std::vector<std::string> f = install::filterSubdirHint(a.subdirectories);
		total += a.subdirectories.size();
		std::set<std::string> fs(f.begin(), f.end());
		for (const std::string& s : a.subdirectories) (fs.count(s) ? kept : dropped).insert(s);
		droppedOcc += a.subdirectories.size() - f.size();
		CHECK(f.size() <= a.subdirectories.size());
	}
	std::printf("fixture sweep: %zu hints in %zu apps; dropped %zu (%zu distinct), kept %zu distinct\n", total, cat.apps.size(),
	            droppedOcc, dropped.size(), kept.size());
	for (const std::string& d : dropped) std::printf("  dropped: %s\n", d.c_str());
	for (const std::string& d : dropped) {
		std::string t = d.substr(1, d.find('/', 1) == std::string::npos ? std::string::npos : d.find('/', 1) - 1);
		CHECK(install::isJunkRoot(t) || install::isSystemRoot(t) || install::isOwnRoot(d) || d == "/" || d == "/apps");
	}
	CHECK(kept.count("/MCBACKUP") && kept.count("/WiiFlow") && kept.count("/Snes9xRX"));
	CHECK(!kept.count("/__MACOSX") && !kept.count("/private") && !kept.count("/apps/OpenShop"));
	// Every kept hint is a valid entry name once the leading slash is gone.
	for (const std::string& k : kept) {
		if (!nameOk(k.substr(1)) ) std::printf("  kept hint that validateEntryName refuses: %s\n", k.c_str());
	}
}

static void testSlugPaths() {
	const char* bad[] = {"../x", ".", "..", "a/b", "sd:x"};
	for (const char* s : bad) {
		CHECK(!isValidSlug(s));
		if (std::string(s).find('/') == std::string::npos || std::string(s) == "../x") CHECK(!nameOk(std::string("apps/") + s));
	}
	CHECK(isValidSlug("WiiMC") && nameOk("apps/WiiMC/boot.dol"));
}

static void testVersions() {
	using install::VersionRelation;
	using R = VersionRelation;
	auto cmp = [](const char* a, const char* b) { return install::compareVersions(a, b); };
	CHECK(cmp("1.2", "1.10") == R::CatalogNewer);
	CHECK(cmp("1.10", "1.2") == R::CatalogOlder);
	CHECK(cmp("v1.2", "1.2") == R::Same);
	CHECK(cmp("V1.2", "v1.2") == R::Same);
	CHECK(cmp("1.2.0", "1.2") == R::Same);
	CHECK(cmp("1.2", "1.2.0.0") == R::Same);
	CHECK(cmp("1.2", "1.2.1") == R::CatalogNewer);
	CHECK(cmp("1.2.1", "1.2") == R::CatalogOlder);
	CHECK(cmp(" 2.0 ", "2.0") == R::Same);
	CHECK(cmp("\t2.0\r\n", " v2.0") == R::Same);
	CHECK(cmp("4.0 r1283", "4.0 r1290") == R::Different);
	CHECK(cmp("4.0 r1283", "4.0 R1283") == R::Same);
	CHECK(cmp("", "") == R::Same);
	CHECK(cmp("  ", "") == R::Same);
	CHECK(cmp("", "1.0") == R::Different);
	CHECK(cmp("1.0", "") == R::Different);
	CHECK(cmp("Beta", "beta") == R::Same);
	CHECK(cmp("beta", "rc1") == R::Different);
	CHECK(cmp("1.0", "1.0b") == R::Different);
	CHECK(cmp("1.0", "1..0") == R::Different);
	CHECK(cmp("1.", "1") == R::Different);
	CHECK(cmp("01.2", "1.2") == R::Same);
	CHECK(cmp("99999999999999999999999.1", "99999999999999999999998.9") == R::CatalogOlder);
	CHECK(cmp("1", "2") == R::CatalogNewer && cmp("10", "9") == R::CatalogOlder);
	CHECK(cmp("v", "v") == R::Same && cmp("v", "1") == R::Different);
	CHECK(cmp("vista", "ista") == R::Different);
}

static void testMetaXml() {
	using install::parseMetaXml;
	install::MetaInfo m = parseMetaXml("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n<app version=\"1\">\r\n"
	                                   "  <name>Wii Donut</name>\r\n  <coder>x</coder>\r\n  <version>1.2.3</version>\r\n</app>\r\n");
	CHECK(m.hasName && m.name == "Wii Donut" && m.hasVersion && m.version == "1.2.3");
	m = parseMetaXml("<app><name>A</name><version>2</version></app>"); // no declaration
	CHECK(m.name == "A" && m.version == "2");
	m = parseMetaXml("<app><name>Tom & Jerry &amp; Co &lt;3 &#65;&#x42; &bogus; &</name><version>1&2</version></app>");
	CHECK(m.name == "Tom & Jerry & Co <3 AB &bogus; &" && m.version == "1&2");
	m = parseMetaXml("<app><name>Only</name></app>"); // missing version
	CHECK(m.hasName && m.name == "Only" && !m.hasVersion && m.version.empty());
	m = parseMetaXml("<app><name>Trunc</name><version>1.0");
	CHECK(m.name == "Trunc" && m.hasVersion && m.version == "1.0");
	m = parseMetaXml("<app><name>Trunc</name><vers");
	CHECK(m.hasName && !m.hasVersion);
	m = parseMetaXml("<app><name>Unclosed<version>3</version>");
	CHECK(m.name == "Unclosed" && m.version == "3");
	m = parseMetaXml("<app><name><![CDATA[A <b> & c]]></name><version><![CDATA[1.0]]></version></app>");
	CHECK(m.name == "A <b> & c" && m.version == "1.0");
	m = parseMetaXml("<name><![CDATA[never closed");
	CHECK(m.name == "never closed");
	m = parseMetaXml("<app><!-- <version>9</version> --><NAME lang=\"en\">  Mixed Case \n</NAME><Version>4</Version></app>");
	CHECK(m.name == "Mixed Case" && m.version == "4");
	m = parseMetaXml("<app><namespace>x</namespace><name/><name>Real</name><version></version><version>5</version></app>");
	CHECK(m.name == "Real" && m.version == "5");
	m = parseMetaXml("<app><!-- never closed <name>x</name>");
	CHECK(!m.hasName && !m.hasVersion);
	CHECK(!parseMetaXml("").hasName && !parseMetaXml("<").hasName && !parseMetaXml("<name").hasName && !parseMetaXml("<name>").hasName);
	CHECK(!parseMetaXml(std::string_view("\0\0\0", 3)).hasName);
	CHECK(!parseMetaXml("<name>   </name>").hasName);
	// Control characters, invalid UTF-8, entities for controls.
	m = parseMetaXml(std::string("<name>a\x01" "b\xff" "c&#1;d\xc3\xa9</name>"));
	CHECK(m.name == "a b?c d\xc3\xa9");
	// Length cap on a UTF-8 boundary, and the input cap.
	m = parseMetaXml("<name>" + std::string(300, 'x') + "</name>");
	CHECK(m.name.size() == install::kMaxMetaFieldBytes);
	std::string eu;
	for (int i = 0; i < 200; i++) eu += "\xc3\xa9";
	m = parseMetaXml("<name>" + eu + "</name>");
	CHECK(m.name.size() == 256 && m.name.size() % 2 == 0 && m.name.substr(0, 2) == "\xc3\xa9");
	m = parseMetaXml("<name>a</name>" + std::string(install::kMaxMetaXmlBytes, ' ') + "<version>9</version>");
	CHECK(m.hasName && !m.hasVersion);
	// A big hostile blob of '<' and '&' finishes quickly and returns nothing.
	m = parseMetaXml(std::string(100000, '<') + std::string(100000, '&'));
	CHECK(!m.hasName && !m.hasVersion);
	m = parseMetaXml("<name>" + std::string(100000, '&'));
	CHECK(m.hasName);
}

// ---- Increment 5: manifests and journal ---------------------------------------------------------------------------------
namespace fs = std::filesystem;
using namespace install;

struct RecTemp {
	std::string path;
	RecTemp() { char t[] = "/tmp/openshop-records-XXXXXX"; path = mkdtemp(t); }
	~RecTemp() { std::error_code ec; fs::remove_all(path, ec); }
};

static void put(const std::string& p, const std::string& body) {
	fs::create_directories(fs::path(p).parent_path());
	std::ofstream(p, std::ios::binary) << body;
}
static bool has(const std::string& p) { std::error_code ec; return fs::exists(p, ec); }

static Manifest sampleManifest(const std::string& slug = "wiimc") {
	Manifest m;
	m.slug = slug;
	m.device = "sd";
	m.version = "1.2.3";
	m.installedAt = 1700000000;
	m.archiveMd5 = "0123456789abcdef0123456789abcdef";
	ManifestDir d;
	d.path = "apps/WiiMC";
	d.created = true;
	d.files = {{"apps/WiiMC/boot.dol"}, {"apps/WiiMC/skins/a.png"}};
	d.createdSubdirs = {{"apps/WiiMC/skins"}};
	m.dirs.push_back(d);
	m.files = {{"config/wiimc/x.ini", false}, {"config/shared.ini", true}};
	m.createdDirs = {{"config"}, {"config/wiimc"}};
	validateManifest(m);
	return m;
}

static Journal sampleJournal(JournalPhase ph) {
	Journal j;
	j.phase = ph;
	j.jobId = 7;
	j.kind = JournalKind::Update;
	j.slug = "wiimc";
	j.device = "usb";
	j.stagingDir = stagingRelPath(7);
	if (ph == JournalPhase::Commit) {
		j.ops = {
		    {OpType::ParkDir, "apps/WiiMC", "apps/OpenShop/staging/7/old/apps/WiiMC", false, false},
		    {OpType::RenameDir, "apps/WiiMC", "apps/OpenShop/staging/7/root/apps/WiiMC", false, false},
		    {OpType::RestoreFile, "apps/WiiMC/user.cfg", "apps/OpenShop/staging/7/old/apps/WiiMC/user.cfg", false, false},
		    {OpType::Mkdir, "config/wiimc", "", false, true},
		    {OpType::ParkFile, "config/wiimc/x.ini", "apps/OpenShop/staging/7/old/config/wiimc/x.ini", false, false},
		    {OpType::WriteFile, "config/wiimc/x.ini", "apps/OpenShop/staging/7/root/config/wiimc/x.ini", true, false},
		    {OpType::WriteManifest, "apps/OpenShop/installed/wiimc.json", "apps/OpenShop/staging/7/manifest.json", false, false},
		};
		j.step = 3;
	}
	return j;
}

static Journal sampleInstallJournal() {
	Journal j;
	j.phase = JournalPhase::Commit;
	j.jobId = 9;
	j.kind = JournalKind::Install;
	j.slug = "foo";
	j.device = "sd";
	j.stagingDir = stagingRelPath(9);
	j.ops = {
	    {OpType::RenameDir, "apps/foo", "apps/OpenShop/staging/9/root/apps/foo", false, false},
	    {OpType::Mkdir, "config", "", false, false},
	    {OpType::Mkdir, "config/foo", "", false, true},
	    {OpType::Mkdir, "apps/foo/sub", "", false, true},
	    {OpType::WriteFile, "config/foo/a.ini", "apps/OpenShop/staging/9/root/config/foo/a.ini", false, false},
	    {OpType::ParkFile, "config/shared.ini", "apps/OpenShop/staging/9/old/config/shared.ini", false, false},
	    {OpType::WriteFile, "config/shared.ini", "apps/OpenShop/staging/9/root/config/shared.ini", true, false},
	    {OpType::WriteManifest, "apps/OpenShop/installed/foo.json", "apps/OpenShop/staging/9/manifest.json", false, false},
	};
	j.step = 2;
	return j;
}

static void testManifestRoundTrip() {
	RecTemp t;
	Manifest m = sampleManifest();
	CHECK(saveManifest(t.path, m));
	CHECK(has(t.path + "/apps/OpenShop/installed/wiimc.json"));
	CHECK(!has(t.path + "/apps/OpenShop/installed/wiimc.json.tmp"));
	ManifestLoad l = loadManifest(t.path, "wiimc", "sd");
	CHECK(l.status == LoadStatus::Ok);
	const Manifest& r = l.manifest;
	CHECK(r.slug == "wiimc" && r.device == "sd" && r.version == "1.2.3" && r.installedAt == 1700000000);
	CHECK(r.archiveMd5 == m.archiveMd5);
	CHECK(r.dirs.size() == 1 && r.dirs[0].path == "apps/WiiMC" && r.dirs[0].created && !r.dirs[0].adopted && r.dirs[0].valid);
	CHECK(r.dirs[0].files.size() == 2 && r.dirs[0].files[1].path == "apps/WiiMC/skins/a.png" && r.dirs[0].files[1].valid);
	CHECK(r.dirs[0].createdSubdirs.size() == 1 && r.dirs[0].createdSubdirs[0].path == "apps/WiiMC/skins");
	CHECK(r.files.size() == 2 && r.files[0].path == "config/wiimc/x.ini" && !r.files[0].preexisting && r.files[1].preexisting);
	CHECK(r.createdDirs.size() == 2 && r.createdDirs[1].path == "config/wiimc");
	// A save over an existing record replaces it and leaves no tmp or bak.
	m.version = "2.0";
	CHECK(saveManifest(t.path, m));
	CHECK(loadManifest(t.path, "wiimc", "sd").manifest.version == "2.0");
	CHECK(!has(t.path + "/apps/OpenShop/installed/wiimc.json.tmp") && !has(t.path + "/apps/OpenShop/installed/wiimc.json.bak"));
	// Case-insensitive lookup is not needed, but a slug that is not valid never touches the disk.
	CHECK(loadManifest(t.path, "../x", "sd").status == LoadStatus::Missing);
	CHECK(loadManifest(t.path, "nothere", "sd").status == LoadStatus::Missing);
	CHECK(removeManifest(t.path, "wiimc"));
	CHECK(loadManifest(t.path, "wiimc", "sd").status == LoadStatus::Missing);
	// Save refuses what could never load.
	Manifest bad = sampleManifest();
	bad.slug = "../x";
	CHECK(!saveManifest(t.path, bad));
	bad = sampleManifest();
	bad.device = "floppy";
	CHECK(!saveManifest(t.path, bad));
	bad = sampleManifest();
	bad.archiveMd5 = "zz";
	CHECK(!saveManifest(t.path, bad));
	CHECK(!has(t.path + "/apps/OpenShop/installed/x.json"));
}

static void testManifestCorrupt() {
	const std::string good = manifestToJson(sampleManifest());
	CHECK(parseManifest(good).ok);
	// Truncation at every length is rejected or (for a prefix that is a whole object) never crashes.
	int accepted = 0;
	for (size_t n = 0; n < good.size(); n++) if (parseManifest(good.substr(0, n)).ok) accepted++;
	CHECK(accepted == 0);
	auto bad = [](std::string_view text) { return !parseManifest(text).ok; };
	CHECK(bad(""));
	CHECK(bad("[]"));
	CHECK(bad("null"));
	CHECK(bad("{}"));
	CHECK(bad(R"({"formatVersion":1,"device":"sd"})"));                         // no slug
	CHECK(bad(R"({"formatVersion":2,"slug":"a","device":"sd"})"));              // unknown future version
	CHECK(bad(R"({"formatVersion":0,"slug":"a","device":"sd"})"));
	CHECK(bad(R"({"formatVersion":"1","slug":"a","device":"sd"})"));
	CHECK(bad(R"({"formatVersion":1,"slug":7,"device":"sd"})"));
	CHECK(bad(R"({"formatVersion":1,"slug":"../x","device":"sd"})"));
	CHECK(bad(R"({"formatVersion":1,"slug":"a/b","device":"sd"})"));
	CHECK(bad(R"({"formatVersion":1,"slug":"a","device":"floppy"})"));
	CHECK(bad(R"({"formatVersion":1,"slug":"a","device":"sd","dirs":{}})"));
	CHECK(bad(R"({"formatVersion":1,"slug":"a","device":"sd","dirs":[1]})"));
	CHECK(bad(R"({"formatVersion":1,"slug":"a","device":"sd","dirs":[{"path":7}]})"));
	CHECK(bad(R"({"formatVersion":1,"slug":"a","device":"sd","dirs":[{"path":"apps/a","files":[1]}]})"));
	CHECK(bad(R"({"formatVersion":1,"slug":"a","device":"sd","files":["x"]})"));
	CHECK(bad(R"({"formatVersion":1,"slug":"a","device":"sd","createdDirs":[[]]})"));
	CHECK(bad(R"({"formatVersion":1,"slug":"a","device":"sd","version":5})"));
	CHECK(bad(R"({"formatVersion":1,"slug":"a","device":"sd","installedAt":"now"})"));
	CHECK(bad(R"({"formatVersion":1,"slug":"a","device":"sd","installedAt":-1})"));
	CHECK(bad(R"({"formatVersion":1,"slug":"a","device":"sd","archiveMd5":"xyz"})"));
	CHECK(bad(R"({"formatVersion":1,"slug":"a","slug":"b","device":"sd"})"));  // duplicate keys
	CHECK(bad(std::string(R"({"formatVersion":1,"slug":"a","device":"sd","version":")") + std::string(300, 'v') + "\"}"));
	CHECK(bad(std::string(R"({"formatVersion":1,"slug":"a","device":"sd","files":[{"path":")") + std::string(2000, 'p') + "\"}]}"));
	// Minimal and lenient forms are fine: an unknown field is ignored, optional fields default.
	ManifestParse p = parseManifest(R"({"formatVersion":1,"slug":"a","device":"usb","future":{"x":[1,2]},"dirs":[{"path":"apps/a"}],"archiveMd5":"0123456789ABCDEF0123456789ABCDEF"})");
	CHECK(p.ok && p.manifest.device == "usb" && p.manifest.dirs.size() == 1 && p.manifest.archiveMd5 == "0123456789abcdef0123456789abcdef");
	// At least one dirs[] entry is required (L4).
	CHECK(bad(R"({"formatVersion":1,"slug":"a","device":"sd"})"));
	CHECK(bad(R"({"formatVersion":1,"slug":"a","device":"sd","dirs":[]})"));
	// The flags are lenient and read as the safe value on a wrong type (L6): created/adopted false, preexisting true.
	p = parseManifest(R"({"formatVersion":1,"slug":"a","device":"sd","dirs":[{"path":"apps/a","created":"yes","adopted":1}],"files":[{"path":"config/a/x","preexisting":1},{"path":"config/a/y","preexisting":false}]})");
	CHECK(p.ok && !p.manifest.dirs[0].created && !p.manifest.dirs[0].adopted && p.manifest.files[0].preexisting && !p.manifest.files[1].preexisting);
	// Two dirs entries for one folder, also across case and accents, are refused (the nice-to-have dedupe).
	CHECK(bad(R"({"formatVersion":1,"slug":"a","device":"sd","dirs":[{"path":"apps/a"},{"path":"apps/A"}]})"));
	CHECK(bad(std::string(R"({"formatVersion":1,"slug":"a","device":"sd","dirs":[{"path":"apps/\u00c9clair"},{"path":"apps/\u00e9clair"}]})")));
	// An unknown field inside a dir and a file entry is ignored too; adopted of the wrong type reads false; a missing
	// "preexisting" reads true (never remove what we cannot vouch for).
	p = parseManifest(R"({"formatVersion":1,"slug":"a","device":"sd","dirs":[{"path":"apps/a","created":false,"adopted":"yes","extra":1,"files":["apps/a/boot.dol"]}],"files":[{"path":"config/a/x"}]})");
	CHECK(p.ok && !p.manifest.dirs[0].adopted && !p.manifest.dirs[0].created && p.manifest.dirs[0].createdSubdirs.empty());
	CHECK(p.ok && p.manifest.files[0].preexisting);
	p = parseManifest(R"({"formatVersion":1,"slug":"a","device":"sd","dirs":[{"path":"apps/a","adopted":true}]})");
	CHECK(p.ok && p.manifest.dirs[0].adopted);
	// Hostile shapes: deep nesting is refused before jansson recurses, a huge array is capped, a NUL escape is refused.
	CHECK(bad(std::string(100000, '[')));
	CHECK(bad(R"({"formatVersion":1,"slug":"a","device":"sd","x":)" + std::string(50, '[') + std::string(50, ']') + "}"));
	CHECK(bad(R"({"formatVersion":1,"slug":"a","device":"sd","version":"a\u0000b"})"));
	{
		std::string many = R"({"formatVersion":1,"slug":"a","device":"sd","dirs":[{"path":"apps/a"}],"createdDirs":[)";
		for (size_t i = 0; i <= kMaxManifestPaths; i++) many += i ? ",\"c\"" : "\"c\"";
		many += "]}";
		CHECK(bad(many));
		std::string ok = R"({"formatVersion":1,"slug":"a","device":"sd","dirs":[{"path":"apps/a"}],"createdDirs":[)";
		for (size_t i = 0; i < kMaxManifestPaths; i++) ok += i ? ",\"c\"" : "\"c\"";
		ok += "]}";
		CHECK(parseManifest(ok).ok);
	}
	CHECK(bad(std::string(kMaxManifestBytes + 1, ' ')));
	// Binary garbage.
	std::string junk;
	for (int i = 0; i < 4096; i++) junk += (char)((i * 131 + 7) & 0xff);
	CHECK(bad(junk));

	// On disk: oversize, garbage, wrong slug and a directory in place of the file all give Corrupt, never a crash.
	RecTemp t;
	const std::string dir = t.path + "/apps/OpenShop/installed/";
	put(dir + "big.json", std::string(kMaxManifestBytes + 1, ' '));
	CHECK(loadManifest(t.path, "big", "sd").status == LoadStatus::Corrupt);
	put(dir + "junk.json", junk);
	CHECK(loadManifest(t.path, "junk", "sd").status == LoadStatus::Corrupt);
	put(dir + "other.json", manifestToJson(sampleManifest("wiimc")));
	CHECK(loadManifest(t.path, "other", "sd").status == LoadStatus::Corrupt);
	fs::create_directories(dir + "dirrec.json");
	CHECK(loadManifest(t.path, "dirrec", "sd").status == LoadStatus::Corrupt);
}

static void testManifestPathFlags() {
	Manifest m = sampleManifest();
	m.dirs.push_back({"apps/OpenShop", true, false, {{"apps/OpenShop/x"}}, {}, true});
	m.dirs.push_back({"apps/../x", true, false, {{"apps/../x/y"}}, {}, true});
	m.dirs.push_back({"apps/ok", true, false, {{"apps/ok/boot.dol"}, {"apps/other/boot.dol"}, {"apps/ok/../../x"}, {"apps/OK2/x"}, {"apps/ok/CON.txt"}}, {{"apps/ok"}}, true});
	m.files.push_back({"private/x", false, true});
	m.files.push_back({"apps/foo/x", false, true});
	m.files.push_back({"a\\b", false, true});
	m.files.push_back({"sd:/x", false, true});
	m.files.push_back({"/etc/x", false, true});
	m.createdDirs.push_back({"title"});
	m.createdDirs.push_back({"config/"});
	m.files.push_back({"boot.elf", false, true});     // a file directly in the device root: two segments are required
	m.files.push_back({"autoexec.bin", false, true});
	m.dirs[0].files.push_back({"apps"});
	validateManifest(m);
	CHECK(m.dirs[0].valid);
	CHECK(!m.dirs[1].valid && !m.dirs[1].files[0].valid);   // own folder
	CHECK(!m.dirs[2].valid && !m.dirs[2].files[0].valid);   // traversal
	CHECK(m.dirs[3].valid && m.dirs[3].files[0].valid);
	CHECK(!m.dirs[3].files[1].valid && !m.dirs[3].files[2].valid && !m.dirs[3].files[3].valid && !m.dirs[3].files[4].valid);
	CHECK(!m.dirs[3].createdSubdirs[0].valid);              // not strictly below its own dir
	CHECK(m.files[0].valid && m.files[1].valid);
	for (size_t i = 2; i < m.files.size(); i++) CHECK(!m.files[i].valid);
	CHECK(m.files[m.files.size() - 1].path == "autoexec.bin" && !isOutsideFilePath("boot.elf") && isOutsideFilePath("config/x"));
	CHECK(isOutsidePath("config") && isOutsidePath("boot.elf") && !isTargetFilePath("boot.elf") && isTargetFilePath("apps/a/boot.dol"));
	CHECK(!m.dirs[0].files.back().valid);
	CHECK(m.createdDirs[0].valid && m.createdDirs[1].valid && !m.createdDirs[2].valid && !m.createdDirs[3].valid);
	// Kept as data: a round trip keeps the entries and they come back flagged.
	RecTemp t;
	CHECK(saveManifest(t.path, m));
	ManifestLoad l = loadManifest(t.path, "wiimc", "sd");
	CHECK(l.status == LoadStatus::Ok);
	CHECK(l.manifest.files.size() == m.files.size() && l.manifest.files[4].path == "a\\b" && !l.manifest.files[4].valid);
	CHECK(l.manifest.dirs.size() == 4 && !l.manifest.dirs[1].valid && l.manifest.dirs[3].valid);
	// Invalid paths never answer a query.
	std::vector<Manifest> all{l.manifest};
	CHECK(manifestsReferencing(all, "private/x").empty());
	CHECK(manifestsReferencing(all, "apps/OpenShop/x").empty());
	CHECK(dirOwners(all, "apps/OpenShop").empty());
	CHECK(manifestsReferencing(all, "config/shared.ini").size() == 1);
}

static void testManifestQueries() {
	Manifest a = sampleManifest("wiimc");
	Manifest b = sampleManifest("other");
	b.dirs.clear();
	b.dirs.push_back({"apps/wiimc", false, false, {{"apps/wiimc/boot.dol"}}, {}, true});
	b.files = {{"config/shared.ini", true}};
	b.createdDirs.clear();
	Manifest c = sampleManifest("third");
	c.dirs.clear();
	c.files = {{"Config/Third/y.ini", false}};
	c.createdDirs = {{"config/third"}};
	validateManifest(b);
	validateManifest(c);
	std::vector<Manifest> all{a, b, c};
	std::vector<std::string> r = manifestsReferencing(all, "config/shared.ini");
	CHECK(r.size() == 2 && r[0] == "wiimc" && r[1] == "other");
	CHECK(manifestsReferencing(all, "CONFIG/Shared.INI", "wiimc") == std::vector<std::string>{"other"});
	CHECK(manifestsReferencing(all, "config/third/y.ini") == std::vector<std::string>{"third"});
	CHECK(manifestsReferencing(all, "config/nothing").empty());
	CHECK(manifestsReferencing(all, "apps/WiiMC/skins") == std::vector<std::string>{"wiimc"}); // a created subdir counts
	CHECK(manifestsReferencing(all, "config") == std::vector<std::string>{"wiimc"});           // a created dir counts
	r = dirOwners(all, "apps/WiiMC");
	CHECK(r.size() == 2 && r[0] == "wiimc" && r[1] == "other"); // "other" lists apps/wiimc
	CHECK(dirOwners(all, "apps/wiimc", "WIIMC") == std::vector<std::string>{"other"});
	CHECK(dirOwners(all, "apps/none").empty());
	CHECK(dirOwners(all, "apps/WiiMC/skins").empty()); // not a dir entry

	// listManifests over a device: good ones, a corrupt one, junk names, a lone .bak, a journal that is not a manifest.
	RecTemp t;
	CHECK(!listManifests(t.path, "sd").listed);
	CHECK(saveManifest(t.path, a) && saveManifest(t.path, b));
	const std::string dir = t.path + "/apps/OpenShop/installed/";
	put(dir + "broken.json", "{\"formatVersion\":");
	put(dir + "notes.txt", "hi");
	put(dir + "x.json.tmp", "half");
	put(dir + ".json", "{}");
	put(dir + "bad name.json", "{}");
	put(dir + ".journal", manifestToJson(a)); // not a manifest, not listed
	put(dir + "onlybak.json.bak", manifestToJson(sampleManifest("onlybak")));
	put(dir + "wiimc.json.bak", manifestToJson(sampleManifest("wiimc"))); // stale; the .json wins
	ManifestList ml = listManifests(t.path, "sd");
	CHECK(ml.listed && !ml.truncated);
	CHECK(ml.manifests.size() == 3);
	CHECK(ml.corrupt.size() == 1 && ml.corrupt[0] == "broken");
	std::set<std::string> slugs;
	for (const Manifest& m : ml.manifests) slugs.insert(m.slug);
	CHECK(slugs == std::set<std::string>({"wiimc", "other", "onlybak"}));
	// Many files: capped, not unbounded.
	RecTemp t2;
	for (size_t i = 0; i < kMaxManifestsPerDevice + 5; i++) put(t2.path + "/apps/OpenShop/installed/s" + std::to_string(i) + ".json", "x");
	ml = listManifests(t2.path, "sd");
	CHECK(ml.truncated && ml.manifests.size() + ml.corrupt.size() == kMaxManifestsPerDevice);
}

static void testMergeForUpdate() {
	Manifest old = sampleManifest("wiimc");
	old.dirs[0].files.push_back({"apps/WiiMC/dropped.bin"});
	old.dirs[0].createdSubdirs.push_back({"apps/WiiMC/olddir"});
	old.files.push_back({"config/wiimc/dropped.ini", false});
	old.createdDirs.push_back({"config/olddir"});
	old.dirs.push_back({"apps/WiiMC2", true, false, {{"apps/WiiMC2/boot.dol"}}, {}, true});
	validateManifest(old);

	Manifest neu;
	neu.slug = "wiimc";
	neu.device = "sd";
	neu.version = "2.0";
	neu.installedAt = 1800000000;
	neu.archiveMd5 = "ffffffffffffffffffffffffffffffff";
	// The update thinks it found the folder (created=false) and the file pre-existing; the old values win.
	neu.dirs.push_back({"apps/wiimc", false, false, {{"apps/wiimc/boot.dol"}, {"apps/wiimc/new.dat"}}, {{"apps/wiimc/skins"}}, true});
	neu.files = {{"config/wiimc/x.ini", true, true}, {"config/new.ini", false, true}};
	neu.createdDirs = {{"config/newdir"}};
	validateManifest(neu);

	Manifest m = mergeForUpdate(&old, neu);
	CHECK(m.version == "2.0" && m.installedAt == 1800000000 && m.archiveMd5 == neu.archiveMd5 && m.slug == "wiimc");
	CHECK(m.dirs.size() == 2); // apps/wiimc (merged with the old apps/WiiMC) and the old apps/WiiMC2 kept whole
	const ManifestDir& d = m.dirs[0];
	CHECK(d.path == "apps/wiimc" && d.created && !d.adopted);                  // old created kept
	std::vector<std::string> files;
	for (const ManifestPath& f : d.files) files.push_back(f.path);
	CHECK((files == std::vector<std::string>{"apps/wiimc/boot.dol", "apps/wiimc/new.dat", "apps/WiiMC/skins/a.png", "apps/WiiMC/dropped.bin"}));
	CHECK(d.createdSubdirs.size() == 2 && d.createdSubdirs[0].path == "apps/wiimc/skins" && d.createdSubdirs[1].path == "apps/WiiMC/olddir");
	CHECK(m.dirs[1].path == "apps/WiiMC2" && m.dirs[1].created);
	CHECK(m.files.size() == 4);
	bool sawX = false, sawDropped = false, sawNew = false;
	for (const ManifestFile& f : m.files) {
		if (f.path == "config/wiimc/x.ini") { sawX = true; CHECK(!f.preexisting); } // old false beats new true
		if (f.path == "config/wiimc/dropped.ini") sawDropped = true;
		if (f.path == "config/new.ini") { sawNew = true; CHECK(!f.preexisting); }
	}
	CHECK(sawX && sawDropped && sawNew);
	// "config/shared.ini" was only in old (preexisting) and is kept as listed, preexisting intact.
	bool shared = false;
	for (const ManifestFile& f : m.files) if (f.path == "config/shared.ini") shared = f.preexisting;
	CHECK(shared);
	std::vector<std::string> cd;
	for (const ManifestPath& p : m.createdDirs) cd.push_back(p.path);
	CHECK((cd == std::vector<std::string>{"config/newdir", "config", "config/wiimc", "config/olddir"}));
	// adopted and created are kept from old when old had adopted.
	old.dirs[0].created = false;
	old.dirs[0].adopted = true;
	m = mergeForUpdate(&old, neu);
	CHECK(!m.dirs[0].created && m.dirs[0].adopted);
	// Merging twice is stable (idempotent on the lists).
	Manifest m2 = mergeForUpdate(&m, neu);
	CHECK(m2.dirs[0].files.size() == m.dirs[0].files.size() && m2.files.size() == m.files.size() && m2.createdDirs.size() == m.createdDirs.size());

	// No old manifest (update over a folder OpenShop didn't install).
	Manifest n2;
	n2.slug = "wiimc";
	n2.device = "sd";
	n2.dirs.push_back({"apps/WiiMC", true, false, {{"apps/WiiMC/boot.dol"}}, {{"apps/WiiMC/newsub"}}, true});
	n2.dirs.push_back({"apps/OtherApp", true, true, {{"apps/OtherApp/boot.dol"}}, {}, true});
	n2.files = {{"config/wiimc/x.ini", true, true}};
	m = mergeForUpdate(nullptr, n2);
	CHECK(m.dirs.size() == 2);
	CHECK(!m.dirs[0].created && m.dirs[0].adopted);   // the app's own folder (name == slug, case-insensitive)
	CHECK(!m.dirs[1].created && !m.dirs[1].adopted);  // a replaced different-app folder
	CHECK(m.dirs[0].files.size() == 1 && m.dirs[0].createdSubdirs.size() == 1 && m.files.size() == 1 && m.files[0].preexisting);
	// The adopted flag survives a save, a load and a later merge with old.
	RecTemp t;
	CHECK(saveManifest(t.path, m));
	ManifestLoad l = loadManifest(t.path, "wiimc", "sd");
	CHECK(l.status == LoadStatus::Ok && l.manifest.dirs[0].adopted && !l.manifest.dirs[0].created && !l.manifest.dirs[1].adopted);
	Manifest m3 = mergeForUpdate(&l.manifest, n2);
	CHECK(m3.dirs[0].adopted && !m3.dirs[0].created && !m3.dirs[1].adopted);
	// A hostile old manifest with the same file listed 40000 times merges quickly (no quadratic scan).
	Manifest big = old;
	for (int i = 0; i < 30000; i++) big.files.push_back({"config/big/f" + std::to_string(i), false, true});
	m = mergeForUpdate(&big, neu);
	CHECK(m.files.size() >= 30000);
}

// ---- crash-safe writes --------------------------------------------------------------------------------------------------
static int g_renameCalls = 0;
static int g_failAtRename = 0;
static int renameFailAt(const char* a, const char* b) {
	g_renameCalls++;
	if (g_failAtRename && g_renameCalls >= g_failAtRename) return -1; // from call N on: a dead medium
	return std::rename(a, b);
}
// libfat: cannot rename over an existing file.
static int renameLikeFat(const char* a, const char* b) {
	g_renameCalls++;
	if (g_failAtRename && g_renameCalls == g_failAtRename) return -1;
	if (has(b)) return -1;
	return std::rename(a, b);
}
static int renameExitAt(const char* a, const char* b) {
	if (++g_renameCalls == g_failAtRename) _Exit(0); // power cut right before this rename
	if (has(b)) return -1;
	return std::rename(a, b);
}
static size_t shortWrite(const void* d, size_t n, std::FILE* f) { return std::fwrite(d, 1, n / 2, f); }
static bool failingFsync(std::FILE*) { return false; }
static int g_fsyncCalls = 0;
static bool countingFsync(std::FILE* f) { g_fsyncCalls++; return fsyncFile(f); }

static bool runForked(void (*body)()) {
	pid_t pid = fork();
	if (pid == 0) { body(); _Exit(1); } // _Exit(0) inside means "the cut happened"
	int st = 0;
	waitpid(pid, &st, 0);
	return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

static std::string g_crashRoot;
static void crashSave() {
	FileOps ops;
	ops.rename = renameExitAt;
	Manifest m = sampleManifest();
	m.version = "NEW";
	saveManifest(g_crashRoot, m, ops);
}

static void testRecordWrites() {
	RecTemp t;
	const std::string rec = t.path + "/apps/OpenShop/installed/wiimc.json";
	Manifest m = sampleManifest();
	CHECK(saveManifest(t.path, m));
	// fsync happens before the rename.
	{
		FileOps ops;
		ops.fsync = countingFsync;
		g_fsyncCalls = 0;
		m.version = "fsynced";
		CHECK(saveManifest(t.path, m, ops) && g_fsyncCalls == 1);
		CHECK(loadManifest(t.path, "wiimc", "sd").manifest.version == "fsynced");
	}
	// A failed fsync, a short write and a failed rename each leave the previous record intact and no tmp behind.
	m.version = "NEW";
	{
		FileOps ops;
		ops.fsync = failingFsync;
		CHECK(!saveManifest(t.path, m, ops));
	}
	{
		FileOps ops;
		ops.write = shortWrite;
		CHECK(!saveManifest(t.path, m, ops));
	}
	{
		FileOps ops;
		ops.rename = renameFailAt;
		g_renameCalls = 0;
		g_failAtRename = 1;
		CHECK(!saveManifest(t.path, m, ops));
		g_failAtRename = 0;
	}
	CHECK(loadManifest(t.path, "wiimc", "sd").manifest.version == "fsynced");
	CHECK(!has(rec + ".tmp"));

	// libfat dance: the old record is parked as .bak, then the new one renamed in. Fail the second rename: the old one is back.
	{
		FileOps ops;
		ops.rename = renameLikeFat;
		g_renameCalls = 0;
		g_failAtRename = 3; // 1: tmp->rec fails (exists), 2: rec->bak, 3: tmp->rec
		CHECK(!saveManifest(t.path, m, ops));
		g_failAtRename = 0;
		CHECK(loadManifest(t.path, "wiimc", "sd").manifest.version == "fsynced");
		g_renameCalls = 0;
		CHECK(saveManifest(t.path, m, ops)); // and it works when nothing fails
		CHECK(loadManifest(t.path, "wiimc", "sd").manifest.version == "NEW");
		CHECK(!has(rec + ".bak") && !has(rec + ".tmp"));
	}

	// A real cut (forked child exits right before a rename). Cut 1: before the first rename, a complete tmp is left, and the
	// record is the old one. Cut 3 (the dance): record parked as .bak, nothing under the real name; the loader finds the .bak.
	g_crashRoot = t.path;
	for (int cut : {1, 2, 3}) {
		RecTemp t2;
		g_crashRoot = t2.path;
		Manifest old = sampleManifest();
		old.version = "OLD";
		CHECK(saveManifest(t2.path, old));
		g_renameCalls = 0;
		g_failAtRename = cut;
		CHECK(runForked(crashSave));
		g_failAtRename = 0;
		const std::string r2 = t2.path + "/apps/OpenShop/installed/wiimc.json";
		ManifestLoad l = loadManifest(t2.path, "wiimc", "sd");
		CHECK(l.status == LoadStatus::Ok);
		CHECK(l.manifest.version == "OLD");
		CHECK(l.fromBackup == (cut == 3));
		CHECK(has(r2 + ".tmp"));               // the half-done save is left over, never under the real name
		CHECK(!has(r2) == (cut == 3));
		ManifestList ml = listManifests(t2.path, "sd");
		CHECK(ml.manifests.size() == 1 && ml.corrupt.empty() && ml.manifests[0].version == "OLD");
		// The next save cleans up and wins.
		Manifest nx = old;
		nx.version = "AFTER";
		CHECK(saveManifest(t2.path, nx));
		CHECK(loadManifest(t2.path, "wiimc", "sd").manifest.version == "AFTER" && !has(r2 + ".bak") && !has(r2 + ".tmp"));
	}
	// A partial record under the real name (a medium that lied) is Corrupt, and a leftover half .tmp does not matter.
	put(rec, manifestToJson(sampleManifest()).substr(0, 60));
	CHECK(loadManifest(t.path, "wiimc", "sd").status == LoadStatus::Corrupt);
	CHECK(listManifests(t.path, "sd").corrupt.size() == 1);
	CHECK(removeManifest(t.path, "wiimc") && !has(rec));
}

static void testJournal() {
	for (JournalPhase ph : {JournalPhase::Start, JournalPhase::Commit}) {
		Journal j = sampleJournal(ph);
		CHECK(validateJournal(j).empty());
		RecTemp t;
		CHECK(loadJournal(t.path, "usb").status == LoadStatus::Missing);
		CHECK(saveJournal(t.path, j));
		CHECK(has(t.path + "/apps/OpenShop/installed/.journal"));
		JournalLoad l = loadJournal(t.path, "usb");
		CHECK(l.status == LoadStatus::Ok);
		const Journal& r = l.journal;
		CHECK(r.phase == ph && r.jobId == 7 && r.kind == JournalKind::Update && r.slug == "wiimc" && r.device == "usb");
		CHECK(r.stagingDir == "apps/OpenShop/staging/7");
		CHECK(r.ops.size() == j.ops.size() && r.step == j.step);
		for (size_t i = 0; i < j.ops.size(); i++) {
			CHECK(r.ops[i].type == j.ops[i].type && r.ops[i].path == j.ops[i].path && r.ops[i].other == j.ops[i].other);
			CHECK(r.ops[i].preexisting == j.ops[i].preexisting && r.ops[i].created == j.ops[i].created);
		}
		// Updating the step rewrites the file.
		if (ph == JournalPhase::Commit) {
			j.step = 5;
			CHECK(saveJournal(t.path, j));
			CHECK(loadJournal(t.path, "usb").journal.step == 5);
		}
		CHECK(removeJournal(t.path));
		CHECK(loadJournal(t.path, "usb").status == LoadStatus::Missing && !has(t.path + "/apps/OpenShop/installed/.journal.tmp"));
	}
	// Both kinds and a step of -1 (commit written, nothing done yet).
	Journal j = sampleJournal(JournalPhase::Commit);
	j.step = -1;
	CHECK(parseJournal(journalToJson(j)).ok);
	j.step = (int)j.ops.size() - 1;
	CHECK(parseJournal(journalToJson(j)).ok);
	j = sampleInstallJournal();
	CHECK(validateJournal(j).empty() && parseJournal(journalToJson(j)).ok);
	j.step = -1;
	CHECK(parseJournal(journalToJson(j)).ok);
}

static void testJournalCorrupt() {
	const Journal good = sampleJournal(JournalPhase::Commit);
	const std::string text = journalToJson(good);
	CHECK(parseJournal(text).ok);
	int accepted = 0;
	for (size_t n = 0; n < text.size(); n++) if (parseJournal(text.substr(0, n)).ok) accepted++;
	CHECK(accepted == 0);
	auto badJ = [&](Journal j) { return !validateJournal(j).empty(); };
	Journal j = good;
	j.jobId = -1;                     CHECK(badJ(j));
	j = good; j.slug = "../x";        CHECK(badJ(j));
	j = good; j.device = "nand";      CHECK(badJ(j));
	j = good; j.stagingDir = "apps/OpenShop/staging/8"; CHECK(badJ(j));
	j = good; j.stagingDir = "apps/OpenShop/staging/7/../x"; CHECK(badJ(j));
	j = good; j.stagingDir = "apps/foo";  CHECK(badJ(j));
	j = good; j.step = 7;             CHECK(badJ(j));
	j = good; j.step = -2;            CHECK(badJ(j));
	j = good; j.formatVersion = 2;    CHECK(badJ(j));
	j = sampleJournal(JournalPhase::Start); j.step = 0; CHECK(badJ(j));
	j = sampleJournal(JournalPhase::Start); j.ops.push_back(good.ops[0]); CHECK(badJ(j));
	// Op paths: escapes, system roots, another job's staging, wrong shapes.
	auto withOp = [&](JournalOp op) { Journal x = good; x.ops[0] = op; return validateJournal(x); };
	JournalOp op = good.ops[0];
	CHECK(withOp(op).empty());
	op = good.ops[0]; op.path = "../x";                        CHECK(!withOp(op).empty());
	op = good.ops[0]; op.path = "apps/OpenShop";               CHECK(!withOp(op).empty());
	op = good.ops[0]; op.path = "apps/foo/bar";                CHECK(!withOp(op).empty());   // a dir op needs apps/<dir>
	op = good.ops[0]; op.path = "private/x";                   CHECK(!withOp(op).empty());
	op = good.ops[0]; op.path = "/apps/foo";                   CHECK(!withOp(op).empty());
	op = good.ops[0]; op.path = "sd:/apps/foo";                CHECK(!withOp(op).empty());
	op = good.ops[0]; op.other = "apps/OpenShop/staging/8/old/x";   CHECK(!withOp(op).empty());
	op = good.ops[0]; op.other = "apps/OpenShop/staging/7";    CHECK(!withOp(op).empty());   // not strictly inside
	op = good.ops[0]; op.other = "apps/OpenShop/staging/7/../8/x"; CHECK(!withOp(op).empty());
	op = good.ops[0]; op.other = "apps/OpenShop/staging/7/a\\b";   CHECK(!withOp(op).empty());
	op = good.ops[0]; op.other = "";                           CHECK(!withOp(op).empty());
	op = good.ops[3]; op.other = "apps/OpenShop/staging/7/x";  CHECK(!withOp(op).empty());   // mkdir takes no other
	op = good.ops[3]; op.path = "title/x";                     CHECK(!withOp(op).empty());
	op = good.ops[6]; op.path = "apps/OpenShop/installed/other.json"; CHECK(!withOp(op).empty());
	op = good.ops[5]; op.path = "apps/OpenShop/installed/.journal"; CHECK(!withOp(op).empty());
	op = good.ops[5]; op.path = "apps/OpenShop/staging/7/x";   CHECK(!withOp(op).empty());
	// JSON level.
	auto badText = [](const std::string& s) { return !parseJournal(s).ok; };
	CHECK(badText("")); CHECK(badText("[]")); CHECK(badText("{}"));
	CHECK(badText(std::string(100000, '[')));
	CHECK(badText(std::string(kMaxJournalBytes + 1, ' ')));
	std::string t2 = text;
	auto swapIn = [&](const std::string& from, const std::string& to) {
		std::string s = t2;
		size_t p = s.find(from);
		if (p == std::string::npos) return std::string("?");
		return s.replace(p, from.size(), to);
	};
	CHECK(parseJournal(swapIn("\"formatVersion\":1", "\"formatVersion\":1,\"extra\":{\"a\":[1]}")).ok); // unknown field ignored
	CHECK(badText(swapIn("\"formatVersion\":1", "\"formatVersion\":2")));
	CHECK(badText(swapIn("\"phase\":\"commit\"", "\"phase\":\"done\"")));
	CHECK(badText(swapIn("\"kind\":\"update\"", "\"kind\":7")));
	CHECK(badText(swapIn("\"jobId\":7", "\"jobId\":\"7\"")));
	CHECK(badText(swapIn("\"op\":\"parkDir\"", "\"op\":\"format\"")));
	CHECK(badText(swapIn("\"step\":3", "\"step\":99")));
	CHECK(badText(swapIn("\"step\":3", "\"step\":\"3\"")));
	CHECK(badText(swapIn("\"ops\":[", "\"ops\":{},\"x\":[")));
	CHECK(badText(swapIn("\"preexisting\":true", "\"preexisting\":1")));
	CHECK(badText(swapIn("\"slug\":\"wiimc\"", "\"slug\":\"wiimc\",\"slug\":\"x\"")));
	{
		std::string many = R"({"formatVersion":1,"phase":"commit","jobId":7,"kind":"install","slug":"a","device":"sd","stagingDir":"apps/OpenShop/staging/7","step":-1,"ops":[)";
		for (size_t i = 0; i <= kMaxJournalOps; i++) many += i ? ",{\"op\":\"mkdir\",\"path\":\"c\"}" : "{\"op\":\"mkdir\",\"path\":\"c\"}";
		many += "]}";
		CHECK(badText(many));
	}
	// On disk: corrupt and oversized journals load as Corrupt (recovery must leave them), a lone .bak is found.
	RecTemp t;
	const std::string jp = t.path + "/apps/OpenShop/installed/.journal";
	put(jp, text.substr(0, text.size() / 2));
	CHECK(loadJournal(t.path, "usb").status == LoadStatus::Corrupt);
	put(jp, std::string(kMaxJournalBytes + 1, 'x'));
	CHECK(loadJournal(t.path, "usb").status == LoadStatus::Corrupt);
	std::remove(jp.c_str());
	put(jp + ".bak", text);
	JournalLoad l = loadJournal(t.path, "usb");
	CHECK(l.status == LoadStatus::Ok && l.fromBackup && l.journal.step == 3);
	CHECK(removeJournal(t.path) && !has(jp + ".bak"));
	// Save refuses an invalid journal and writes nothing.
	Journal bad = good;
	bad.ops[0].other = "elsewhere/x";
	CHECK(!saveJournal(t.path, bad) && !has(jp));
}

static std::string g_crashJRoot;
static void crashJournal() {
	FileOps ops;
	ops.rename = renameExitAt;
	Journal j = sampleJournal(JournalPhase::Commit);
	j.step = 6;
	saveJournal(g_crashJRoot, j, ops);
}

static void testJournalCrash() {
	for (int cut : {1, 2, 3}) {
		RecTemp t;
		g_crashJRoot = t.path;
		Journal start = sampleJournal(JournalPhase::Start);
		CHECK(saveJournal(t.path, start));
		g_renameCalls = 0;
		g_failAtRename = cut;
		CHECK(runForked(crashJournal));
		g_failAtRename = 0;
		// Whatever the cut, the journal is readable and is the old (start) or complete new (commit) one, never a hybrid.
		JournalLoad l = loadJournal(t.path, "usb");
		CHECK(l.status == LoadStatus::Ok);
		CHECK(l.journal.phase == JournalPhase::Start && l.journal.ops.empty());
		CHECK(l.fromBackup == (cut == 3));
	}
	// A short write or failed fsync on the commit-phase save keeps the start-phase journal.
	RecTemp t;
	CHECK(saveJournal(t.path, sampleJournal(JournalPhase::Start)));
	FileOps ops;
	ops.write = shortWrite;
	CHECK(!saveJournal(t.path, sampleJournal(JournalPhase::Commit), ops));
	ops = {};
	ops.fsync = failingFsync;
	CHECK(!saveJournal(t.path, sampleJournal(JournalPhase::Commit), ops));
	CHECK(loadJournal(t.path, "usb").journal.phase == JournalPhase::Start);
	CHECK(!has(t.path + "/apps/OpenShop/installed/.journal.tmp"));
}

// ---- review fixes: identity folding, shapes, caps, bindings, consistency, cuts ---------------------------------------------
static const std::string kEacute = "\xc3\xa9"; // e with acute
static const std::string kEcap = "\xc3\x89";   // E with acute

static std::string filesJson(const char* key, size_t n, const char* prefix) {
	std::string s;
	for (size_t i = 0; i < n; i++) s += std::string(i ? "," : "") + "\"" + prefix + std::to_string(i) + "\"";
	return std::string("\"") + key + "\":[" + s + "]";
}

static void testManifestUnicodeFold() {
	Manifest a = sampleManifest("wiimc");
	a.files.push_back({"config/Caf" + kEacute + "/x.ini", false, true});
	a.dirs.push_back({"apps/" + kEcap + "clair", true, false, {{"apps/" + kEacute + "clair/boot.dol"}}, {}, true});
	validateManifest(a);
	// The dir apps/Eclair(acute) lists a file whose dir segment is spelled in the other case: valid (FAT sees one folder).
	CHECK(a.dirs.back().valid && a.dirs.back().files[0].valid);
	CHECK(pathStrictlyUnder("apps/" + kEacute + "clair/boot.dol", "apps/" + kEcap + "clair"));
	CHECK(!pathStrictlyUnder("apps/" + kEacute + "clair", "apps/" + kEcap + "clair"));
	CHECK(!pathStrictlyUnder("apps/" + kEacute + "clairs/x", "apps/" + kEcap + "clair"));
	Manifest b = sampleManifest("other");
	b.dirs.clear();
	b.dirs.push_back({"apps/" + kEacute + "clair", false, false, {}, {}, true});
	b.files.clear();
	b.createdDirs.clear();
	validateManifest(b);
	std::vector<Manifest> all{a, b};
	CHECK(manifestsReferencing(all, "config/CAF" + kEcap + "/x.ini") == std::vector<std::string>{"wiimc"});
	CHECK(manifestsReferencing(all, "config/caf" + kEacute + "/X.INI") == std::vector<std::string>{"wiimc"});
	CHECK(manifestsReferencing(all, "config/caf/x.ini").empty());
	CHECK((dirOwners(all, "apps/" + kEacute + "CLAIR") == std::vector<std::string>{"wiimc", "other"}));
	CHECK(dirOwners(all, "apps/" + kEcap + "clair", "other") == std::vector<std::string>{"wiimc"});
	// Merge matches the dir and dedupes files by the Unicode fold.
	Manifest neu = sampleManifest("wiimc");
	neu.dirs.clear();
	neu.dirs.push_back({"apps/" + kEacute + "clair", false, false, {{"apps/" + kEacute + "clair/boot.dol"}, {"apps/" + kEacute + "clair/n.dat"}}, {}, true});
	neu.files = {{"config/CAF" + kEcap + "/x.ini", true, true}};
	neu.createdDirs.clear();
	validateManifest(neu);
	Manifest m = mergeForUpdate(&a, neu);
	CHECK(m.dirs.size() == 2); // apps/Eclair merged with the old one, plus the old apps/WiiMC
	CHECK(m.dirs[0].created && m.dirs[0].files.size() == 2);
	bool found = false;
	for (const ManifestFile& f : m.files) if (foldKeyUnicode(f.path) == foldKeyUnicode("config/caf" + kEacute + "/x.ini")) { found = true; CHECK(!f.preexisting); }
	CHECK(found);
	size_t cafes = 0;
	for (const ManifestFile& f : m.files) if (foldKeyUnicode(f.path).find("caf") != std::string::npos) cafes++;
	CHECK(cafes == 1);
	// Round trip through disk keeps the accented names.
	RecTemp t;
	CHECK(saveManifest(t.path, a));
	ManifestLoad l = loadManifest(t.path, "wiimc", "sd");
	CHECK(l.status == LoadStatus::Ok && l.manifest.dirs.size() == 2 && l.manifest.dirs[1].valid && l.manifest.dirs[1].files[0].valid);
	// An own-name (name == slug) adoption check is ASCII only: no accented fold of the slug.
	Manifest un = sampleManifest("wiimc");
	un.dirs = {{"apps/WiiMC", true, false, {{"apps/WiiMC/boot.dol"}}, {}, true}};
	CHECK(mergeForUpdate(nullptr, un).dirs[0].adopted);
	// The index answers the same as the one-shot queries (L1).
	const ManifestIndex ix = buildManifestIndex(all);
	CHECK(manifestsReferencing(ix, "CONFIG/CAF" + kEcap + "/x.ini") == manifestsReferencing(all, "CONFIG/CAF" + kEcap + "/x.ini"));
	CHECK(dirOwners(ix, "apps/" + kEacute + "clair") == dirOwners(all, "apps/" + kEacute + "clair"));
	CHECK(dirOwners(ix, "apps/" + kEacute + "clair", "WIIMC") == std::vector<std::string>{"other"});
	CHECK(manifestsReferencing(ix, "nothing").empty() && dirOwners(ix, "apps/zzz").empty());
	// A file listed twice in one manifest names the slug once.
	Manifest dup = sampleManifest("wiimc");
	dup.files.push_back({"config/wiimc/x.ini", false, true});
	validateManifest(dup);
	CHECK(manifestsReferencing(std::vector<Manifest>{dup}, "config/wiimc/x.ini").size() == 1);
}

static void testManifestCapsAndShapes() {
	auto bad = [](const std::string& s) { return !parseManifest(s).ok; };
	const std::string head = R"({"formatVersion":1,"slug":"a","device":"sd","dirs":[{"path":"apps/a"}],)";
	// A junk array of many tiny values is refused before jansson allocates for it (the memory-shape check).
	{
		std::string junk = head + "\"x\":[";
		for (int i = 0; i < 600000; i++) junk += i ? ",0" : "0";
		junk += "]}";
		CHECK(junk.size() < kMaxManifestBytes);
		const ManifestParse p = parseManifest(junk);
		CHECK(!p.ok && p.error.find("too many values") != std::string::npos);
		CHECK(!rec::shapeOk(junk, 8, kMaxManifestValues));
		CHECK(rec::shapeOk(junk, 8, 1000000));
		CHECK(rec::shapeOk("[[[1,2],[3]],\"[[[[,,,\"]", 3, 8) && !rec::shapeOk("[[[[1]]]]", 3, 100) && !rec::shapeOk("[1,2,3]", 3, 2) && rec::shapeOk("[1,2,3]", 3, 3));
		std::string objs = head + "\"x\":[";
		for (int i = 0; i < 100000; i++) objs += i ? ",{}" : "{}";
		CHECK(bad(objs + "]}"));
	}
	// Total paths over all arrays together: 20000 + 20000 fit, 20000 + 20001 do not.
	CHECK(parseManifest(R"({"formatVersion":1,"slug":"a","device":"sd","dirs":[{"path":"apps/a",)" + filesJson("files", 20000, "apps/a/f") + "," +
	                    filesJson("createdSubdirs", 20000, "apps/a/d") + "}]}").ok);
	CHECK(bad(R"({"formatVersion":1,"slug":"a","device":"sd","dirs":[{"path":"apps/a",)" + filesJson("files", 20000, "apps/a/f") + "," +
	          filesJson("createdSubdirs", 20001, "apps/a/d") + "}]}"));
	CHECK(bad(R"({"formatVersion":1,"slug":"a","device":"sd","dirs":[{"path":"apps/a",)" + filesJson("files", 20000, "apps/a/f") + "}]," +
	          filesJson("createdDirs", 20001, "c") + "}"));
	// More than 64 dirs: refused on parse and on save; exactly 64 are fine.
	{
		Manifest m = sampleManifest();
		m.dirs.clear();
		for (int i = 0; i < 64; i++) m.dirs.push_back({"apps/d" + std::to_string(i), true, false, {}, {}, true});
		RecTemp t;
		CHECK(saveManifest(t.path, m) && loadManifest(t.path, "wiimc", "sd").manifest.dirs.size() == 64);
		m.dirs.push_back({"apps/d64", true, false, {}, {}, true});
		CHECK(!saveManifest(t.path, m));
		std::string text = head.substr(0, head.find("\"dirs\"")) + "\"dirs\":[";
		for (int i = 0; i < 65; i++) text += std::string(i ? "," : "") + "{\"path\":\"apps/d" + std::to_string(i) + "\"}";
		CHECK(bad(text + "]}"));
	}
	// Memory at the cap: 40000 file entries (the most values per path) save and load; more paths or bytes are refused on save.
	{
		RecTemp t;
		Manifest m = sampleManifest();
		m.files.clear();
		m.createdDirs.clear();
		for (size_t i = 0; i < kMaxManifestPaths - 3; i++) m.files.push_back({"config/f/" + std::to_string(i), i % 2 == 0, true});
		CHECK(manifestPathCount(m) == kMaxManifestPaths);
		const std::string json = manifestToJson(m);
		CHECK(json.size() < kMaxManifestBytes && manifestSaveProblem(m).empty());
		CHECK(saveManifest(t.path, m));
		ManifestLoad l = loadManifest(t.path, "wiimc", "sd");
		CHECK(l.status == LoadStatus::Ok && manifestPathCount(l.manifest) == kMaxManifestPaths && l.bytes == json.size());
		m.files.push_back({"config/f/over", false, true});
		CHECK(!manifestSaveProblem(m).empty() && !saveManifest(t.path, m));
		// Long paths: 40000 x ~110 bytes is over the byte cap, refused at plan time.
		Manifest fat = sampleManifest();
		fat.createdDirs.clear();
		for (size_t i = 0; i < kMaxManifestPaths - 6; i++) fat.createdDirs.push_back({"config/" + std::string(100, 'x') + std::to_string(i), true});
		const std::string why = manifestSaveProblem(fat);
		CHECK(why.find("bytes") != std::string::npos);
		CHECK(loadManifest(t.path, "wiimc", "sd").status == LoadStatus::Ok); // the earlier record is untouched
	}
	// listManifests is bounded in total: five manifests of 40000 paths fit the 200000 cap, the sixth is not kept.
	{
		RecTemp t;
		for (int i = 0; i < 6; i++) {
			Manifest m = sampleManifest("m" + std::to_string(i));
			m.files.clear();
			m.createdDirs.clear();
			for (int k = 0; k < 39997; k++) m.createdDirs.push_back({"config/d" + std::to_string(k), true});
			CHECK(manifestPathCount(m) == kMaxManifestPaths && saveManifest(t.path, m));
		}
		const ManifestList ml = listManifests(t.path, "sd");
		CHECK(ml.listed && ml.truncated && ml.manifests.size() == 5);
	}
}

static void testManifestSaveValidation() {
	RecTemp t;
	// M7: a dirs[].files path over the 1024-byte read cap would save fine and then never load. It is refused up front.
	Manifest m = sampleManifest();
	m.dirs[0].files.push_back({"apps/WiiMC/" + std::string(1089, 'a')});
	CHECK(!manifestSaveProblem(m).empty() && !saveManifest(t.path, m) && !has(t.path + "/apps/OpenShop/installed/wiimc.json"));
	CHECK(!parseManifest(manifestToJson(m)).ok);
	// Invalid UTF-8 is an error, not a silently dropped field.
	m = sampleManifest();
	m.files.push_back({"config/bad\xff" "name", false, true});
	CHECK(manifestToJson(m).empty() && !saveManifest(t.path, m));
	m = sampleManifest();
	m.version = "1.\xc3";
	CHECK(manifestToJson(m).empty() && !saveManifest(t.path, m));
	// An embedded NUL cannot round trip.
	m = sampleManifest();
	m.files.push_back({std::string("config/a\0b", 10), false, true});
	CHECK(!saveManifest(t.path, m));
	// No dirs: refused.
	m = sampleManifest();
	m.dirs.clear();
	CHECK(!saveManifest(t.path, m));
	// Duplicate dirs: refused.
	m = sampleManifest();
	m.dirs.push_back({"apps/wiimc", true, false, {}, {}, true});
	CHECK(!saveManifest(t.path, m));
	CHECK(!has(t.path + "/apps/OpenShop/installed/wiimc.json"));

	// M8: the plan-time dry run of the merge. 30000 old + 20000 new outside files are over the 40000 cap up front.
	Manifest old = sampleManifest("wiimc");
	old.files.clear();
	for (int i = 0; i < 30000; i++) old.files.push_back({"config/old/f" + std::to_string(i), false, true});
	Manifest neu = sampleManifest("wiimc");
	neu.files.clear();
	for (int i = 0; i < 20000; i++) neu.files.push_back({"config/new/f" + std::to_string(i), false, true});
	validateManifest(old);
	validateManifest(neu);
	CHECK(!dryRunMerge(&old, neu).empty());
	CHECK(dryRunMerge(nullptr, neu).empty());
	// With an exists predicate the old extras that are gone from the disk drop out, and it fits.
	size_t asked = 0;
	ExistsFn gone = [&](const std::string&) { asked++; return false; };
	CHECK(dryRunMerge(&old, neu, gone).empty() && asked >= 30000);
	Manifest merged = mergeForUpdate(&old, neu, gone);
	CHECK(merged.files.size() == 20000);
	ExistsFn onlyEven = [](const std::string& p) { return std::stoi(p.substr(p.rfind('f') + 1)) % 2 == 0 || p.find("old") == std::string::npos; };
	merged = mergeForUpdate(&old, neu, onlyEven);
	CHECK(merged.files.size() == 20000 + 15000);
	// The predicate never drops what the new manifest has, nor old entries it finds; sampleManifest's old lists are kept.
	Manifest o2 = sampleManifest("wiimc");
	Manifest n2 = sampleManifest("wiimc");
	n2.dirs[0].files = {{"apps/WiiMC/boot.dol"}};
	ExistsFn none = [](const std::string&) { return false; };
	merged = mergeForUpdate(&o2, n2, none);
	CHECK(merged.dirs[0].files.size() == 1 && merged.dirs[0].files[0].path == "apps/WiiMC/boot.dol");
	merged = mergeForUpdate(&o2, n2);
	CHECK(merged.dirs[0].files.size() == 2);
	// The merge of a duplicate-free pair that would fit is clean.
	CHECK(dryRunMerge(&o2, n2).empty());
}

static void testManifestDeviceAndFlags() {
	RecTemp t;
	Manifest m = sampleManifest("wiimc"); // device "sd"
	CHECK(saveManifest(t.path, m));
	CHECK(loadManifest(t.path, "wiimc", "sd").status == LoadStatus::Ok);
	ManifestLoad l = loadManifest(t.path, "wiimc", "usb");
	CHECK(l.status == LoadStatus::Corrupt && l.error.find("device") != std::string::npos);
	ManifestList ml = listManifests(t.path, "usb");
	CHECK(ml.manifests.empty() && ml.corrupt.size() == 1);
	ml = listManifests(t.path, "sd");
	CHECK(ml.manifests.size() == 1 && ml.corrupt.empty());
	// A journal for the other device is Corrupt too.
	Journal j = sampleJournal(JournalPhase::Commit); // device "usb"
	CHECK(saveJournal(t.path, j));
	CHECK(loadJournal(t.path, "usb").status == LoadStatus::Ok);
	JournalLoad jl = loadJournal(t.path, "sd");
	CHECK(jl.status == LoadStatus::Corrupt && jl.error.find("device") != std::string::npos);
	// Both the record and a stale .bak present: the record wins, and loading does not need the .bak.
	const std::string rec = t.path + "/apps/OpenShop/installed/wiimc.json";
	Manifest stale = sampleManifest("wiimc");
	stale.version = "STALE";
	put(rec + ".bak", manifestToJson(stale));
	l = loadManifest(t.path, "wiimc", "sd");
	CHECK(l.status == LoadStatus::Ok && !l.fromBackup && l.manifest.version == "1.2.3");
	ml = listManifests(t.path, "sd");
	CHECK(ml.manifests.size() == 1 && ml.manifests[0].version == "1.2.3");
	// Root-level files (a single segment) are never valid in files[], dirs[].files or journal file ops; createdDirs may be.
	m = sampleManifest();
	m.files = {{"boot.elf", false, true}, {"autoexec.bin", true, true}, {"config/ok.ini", false, true}};
	m.createdDirs = {{"config"}};
	validateManifest(m);
	CHECK(!m.files[0].valid && !m.files[1].valid && m.files[2].valid && m.createdDirs[0].valid);
	CHECK(manifestsReferencing(std::vector<Manifest>{m}, "boot.elf").empty());
}

static void testJournalBindings() {
	const Journal good = sampleJournal(JournalPhase::Commit);
	CHECK(validateJournal(good).empty());
	auto with = [&](size_t i, auto edit) { Journal j = good; edit(j.ops[i]); return validateJournal(j); };
	const std::string S = "apps/OpenShop/staging/7";
	// M3: `other` is bound to the job and the path.
	CHECK(!with(1, [&](JournalOp& o) { o.other = S + "/root/apps/Other"; }).empty());          // renameDir: another path's staging
	CHECK(!with(1, [&](JournalOp& o) { o.other = S + "/old/apps/WiiMC"; }).empty());           // renameDir out of old/
	CHECK(!with(1, [&](JournalOp& o) { o.other = "apps/OpenShop/staging/8/root/apps/WiiMC"; }).empty());
	CHECK(!with(0, [&](JournalOp& o) { o.other = S + "/old/WiiMC"; }).empty());                // the old layout
	CHECK(!with(0, [&](JournalOp& o) { o.other = S + "/root/apps/WiiMC"; }).empty());          // parkDir from root/
	CHECK(!with(2, [&](JournalOp& o) { o.other = S + "/old/apps/WiiMC/other.cfg"; }).empty()); // restoreFile of another file
	CHECK(!with(4, [&](JournalOp& o) { o.other = S + "/old/config/wiimc/y.ini"; }).empty());   // parkFile
	CHECK(!with(5, [&](JournalOp& o) { o.other = S + "/root/config/wiimc/y.ini"; }).empty());  // writeFile
	CHECK(!with(5, [&](JournalOp& o) { o.other = S + "/old/config/wiimc/x.ini"; }).empty());   // writeFile out of old/
	CHECK(!with(6, [&](JournalOp& o) { o.other = S + "/root/manifest.json"; }).empty());       // writeManifest
	CHECK(!with(6, [&](JournalOp& o) { o.other = ""; }).empty());
	CHECK(!with(6, [&](JournalOp& o) { o.other = "apps/OpenShop/staging/8/manifest.json"; }).empty());
	CHECK(with(6, [&](JournalOp& o) { o.path = "apps/OpenShop/installed/WIIMC.json"; }).empty());
	CHECK(stagedRootPath(S, "apps/a") == S + "/root/apps/a" && stagedOldPath(S, "x/y") == S + "/old/x/y" && stagedManifestPath(S) == S + "/manifest.json");
	// M2: file ops need two or more segments; mkdir may be one.
	CHECK(!with(5, [&](JournalOp& o) { o.path = "boot.elf"; o.other = S + "/root/boot.elf"; }).empty());
	CHECK(!with(5, [&](JournalOp& o) { o.path = "autoexec.bin"; o.other = S + "/root/autoexec.bin"; }).empty());
	CHECK(!with(4, [&](JournalOp& o) { o.path = "boot.elf"; o.other = S + "/old/boot.elf"; }).empty());
	CHECK(!with(2, [&](JournalOp& o) { o.path = "boot.elf"; o.other = S + "/old/boot.elf"; }).empty());
	CHECK(with(3, [&](JournalOp& o) { o.path = "config"; }).empty());
	CHECK(with(3, [&](JournalOp& o) { o.path = "apps/WiiMC/sub/deeper"; }).empty()); // a folder below the new app dir
	// writeFile is for paths outside apps/ only.
	CHECK(!with(5, [&](JournalOp& o) { o.path = "apps/WiiMC/x.bin"; o.other = S + "/root/apps/WiiMC/x.bin"; }).empty());
	// Carry-over: a whole subfolder is one plain restoreFile (path is a folder), as is parkFile of a folder.
	{
		Journal j = good;
		j.ops.insert(j.ops.begin() + 3, {OpType::RestoreFile, "apps/WiiMC/saves", S + "/old/apps/WiiMC/saves", false, false});
		j.ops.insert(j.ops.begin() + 4, {OpType::Mkdir, "apps/WiiMC/skins", "", false, true});
		CHECK(validateJournal(j).empty() && parseJournal(journalToJson(j)).ok);
	}
	// A restoreFile may keep an old entry under "<name>.old" / ".old<N>" (user decision 2026-11-27), and nothing else.
	{
		auto kept = [&](const std::string& path, const std::string& other) {
			Journal j = good;
			j.ops.insert(j.ops.begin() + 3, {OpType::RestoreFile, path, S + "/old/" + other, false, false});
			return validateJournal(j);
		};
		CHECK(kept("apps/WiiMC/data.old", "apps/WiiMC/data").empty());
		CHECK(kept("apps/WiiMC/data.old2", "apps/WiiMC/data").empty() && kept("apps/WiiMC/s/d.old17", "apps/WiiMC/s/d").empty());
		CHECK(!kept("apps/WiiMC/data.old1", "apps/WiiMC/data").empty() && !kept("apps/WiiMC/data.old02", "apps/WiiMC/data").empty());
		CHECK(!kept("apps/WiiMC/data.old2x", "apps/WiiMC/data").empty() && !kept("apps/WiiMC/data.bak", "apps/WiiMC/data").empty());
		CHECK(!kept("apps/WiiMC/data.old", "apps/WiiMC/other").empty());                    // another entry
		CHECK(!kept("apps/WiiMC/s/data.old", "apps/WiiMC/data").empty());                   // another folder
		CHECK(!kept("apps/WiiMC/data.old", "apps/Other/data").empty());                     // another app dir
		CHECK(!kept("apps/WiiMC/data.old", "../x").empty());
	}
	// M4: writeFile needs the preexisting key; JournalOp defaults to the safe value.
	CHECK(JournalOp{}.preexisting && !JournalOp{}.created);
	std::string text = journalToJson(good);
	const size_t at = text.find(",\"preexisting\":true");
	CHECK(at != std::string::npos);
	if (at != std::string::npos) {
		std::string without = text;
		without.erase(at, std::string(",\"preexisting\":true").size());
		CHECK(parseJournal(text).ok && !parseJournal(without).ok);
		std::string wrong = text;
		wrong.replace(at, std::string(",\"preexisting\":true").size(), ",\"preexisting\":\"yes\"");
		CHECK(!parseJournal(wrong).ok);
	}
	// A parsed non-writeFile op reports the plain defaults.
	const Journal back = parseJournal(text).journal;
	CHECK(back.ops[0].preexisting == false && back.ops[5].preexisting == true);
}

static void testJournalConsistency() {
	const std::string S = "apps/OpenShop/staging/7";
	const Journal good = sampleJournal(JournalPhase::Commit);
	// L2: kind and order.
	Journal j = good;
	j.kind = JournalKind::Install;
	CHECK(!validateJournal(j).empty());                                  // parkDir and restoreFile in an install
	CHECK(validateJournal(sampleInstallJournal()).empty());
	j = sampleInstallJournal();
	j.ops.insert(j.ops.begin(), {OpType::ParkDir, "apps/foo", stagedOldPath(j.stagingDir, "apps/foo"), false, false});
	CHECK(!validateJournal(j).empty());
	j = sampleInstallJournal();
	j.ops.erase(j.ops.begin() + 5);                                      // the writeFile{preexisting:true} lost its parkFile
	CHECK(!validateJournal(j).empty());
	j = sampleInstallJournal();
	j.ops[5].path = "config/other.ini";
	j.ops[5].other = stagedOldPath(j.stagingDir, "config/other.ini");   // parked a different file
	CHECK(!validateJournal(j).empty());
	j = sampleInstallJournal();
	std::swap(j.ops[5], j.ops[6]);                                       // the park comes after the write
	CHECK(!validateJournal(j).empty());
	j = good;
	j.ops.erase(j.ops.begin() + 1);                                      // parkDir without a renameDir
	j.step = -1;
	CHECK(!validateJournal(j).empty());
	j = good;
	std::swap(j.ops[1], j.ops[2]);                                       // restoreFile before the renameDir
	CHECK(!validateJournal(j).empty());
	j = good;
	j.ops.erase(j.ops.begin());                                          // restoreFile without a parked dir
	j.step = -1;
	CHECK(!validateJournal(j).empty());
	j = good;
	std::swap(j.ops[0], j.ops[1]);                                       // renameDir before its parkDir
	CHECK(!validateJournal(j).empty());
	// writeManifest: exactly one, last.
	j = good;
	std::swap(j.ops[5], j.ops[6]);
	CHECK(!validateJournal(j).empty());
	j = good;
	j.ops.push_back(j.ops.back());
	CHECK(!validateJournal(j).empty());
	j = good;
	j.ops.pop_back();
	j.step = 2;
	CHECK(!validateJournal(j).empty());
	j = good;
	j.ops.clear();
	j.step = -1;
	CHECK(!validateJournal(j).empty());
	// A restoreFile of a path outside every parked app dir.
	j = good;
	j.ops[2].path = "apps/Other/user.cfg";
	j.ops[2].other = stagedOldPath(S, "apps/Other/user.cfg");
	CHECK(!validateJournal(j).empty());
	// Updates may rename a new dir with no park (the update adds another dir).
	j = good;
	j.ops.insert(j.ops.begin() + 2, {OpType::RenameDir, "apps/Second", stagedRootPath(S, "apps/Second"), false, false});
	CHECK(validateJournal(j).empty());
}

static void testJournalSizeAndFlush() {
	// The periodic flush is gone (review M4): the commit phase is written once, so there is no flush policy to test.
	// Plan-time fit: ops over the cap, and bytes over the cap.
	auto manyOps = [](size_t n, size_t pad) {
		Journal j = sampleInstallJournal();
		const JournalOp last = j.ops.back();
		j.ops.clear();
		for (size_t i = 0; i < n; i++) j.ops.push_back({OpType::Mkdir, "config/" + std::string(pad, 'a') + std::to_string(i), "", false, true});
		j.ops.push_back(last);
		j.step = -1;
		return j;
	};
	CHECK(journalSaveProblem(manyOps(100, 10)).empty());
	CHECK(journalSaveProblem(manyOps(kMaxJournalOps, 5)).find("too many") != std::string::npos);
	CHECK(journalSaveProblem(manyOps(kMaxJournalOps - 1, 5)).empty() || journalSaveProblem(manyOps(kMaxJournalOps - 1, 5)).find("bytes") != std::string::npos);
	const std::string why = journalSaveProblem(manyOps(kMaxJournalOps - 1, 190));
	CHECK(why.find("bytes") != std::string::npos);
	RecTemp t;
	CHECK(!saveJournal(t.path, manyOps(kMaxJournalOps - 1, 190)) && !has(t.path + "/apps/OpenShop/installed/.journal"));
	// A journal at a legal size round trips (memory at the cap: the biggest legal one still loads).
	{
		Journal big = manyOps(kMaxJournalOps - 1, 5);
		if (journalSaveProblem(big).empty()) {
			CHECK(saveJournal(t.path, big));
			JournalLoad l = loadJournal(t.path, "sd");
			CHECK(l.status == LoadStatus::Ok && l.journal.ops.size() == kMaxJournalOps);
		}
	}
	// Junk array in a journal is refused by the value pre-scan.
	std::string junk = R"({"formatVersion":1,"phase":"start","jobId":7,"kind":"install","slug":"a","device":"sd","stagingDir":"apps/OpenShop/staging/7","x":[)";
	for (int i = 0; i < 400000; i++) junk += i ? ",0" : "0";
	junk += "]}";
	CHECK(junk.size() < kMaxJournalBytes && !parseJournal(junk).ok);
	// Invalid UTF-8 in a field is a failed serialisation, never a dropped field.
	Journal u = sampleInstallJournal();
	u.slug = "a\xff";
	CHECK(!journalSaveProblem(u).empty() && !saveJournal(t.path, u));
}

static std::string g_cutRoot;
static int g_removeCalls = 0, g_failAtRemove = 0;
static int removeExitAt(const char* p) {
	if (++g_removeCalls == g_failAtRemove) _Exit(0); // power cut right before this remove
	return std::remove(p);
}
static size_t writeThenExit(const void* d, size_t n, std::FILE* f) {
	std::fwrite(d, 1, n / 2, f);
	std::fflush(f);
	_Exit(0); // power cut half way through the data
}
static void crashRemoveManifest() {
	FileOps ops;
	ops.remove = removeExitAt;
	removeManifest(g_cutRoot, "wiimc", ops);
}
static void crashRemoveJournal() {
	FileOps ops;
	ops.remove = removeExitAt;
	removeJournal(g_cutRoot, ops);
}
static void crashSaveDuringWrite() {
	FileOps ops;
	ops.write = writeThenExit;
	Manifest m = sampleManifest();
	m.version = "NEW";
	saveManifest(g_cutRoot, m, ops);
}
static void crashSaveAtRemove() {
	FileOps ops;
	ops.remove = removeExitAt;
	Manifest m = sampleManifest();
	m.version = "NEW";
	saveManifest(g_cutRoot, m, ops);
}

static void testRecordCuts() {
	// M6: removing a record deletes .bak, .tmp, then the record. A cut anywhere leaves the real record readable, never only
	// a stale .bak that would be loaded in its place.
	for (int cut : {1, 2, 3}) {
		RecTemp t;
		g_cutRoot = t.path;
		Manifest cur = sampleManifest();
		cur.version = "CUR";
		const std::string rec = t.path + "/apps/OpenShop/installed/wiimc.json";
		CHECK(saveManifest(t.path, cur));
		Manifest stale = sampleManifest();
		stale.version = "STALE";
		put(rec + ".bak", manifestToJson(stale));
		put(rec + ".tmp", "half");
		g_removeCalls = 0;
		g_failAtRemove = cut;
		CHECK(runForked(crashRemoveManifest));
		g_failAtRemove = 0;
		ManifestLoad l = loadManifest(t.path, "wiimc", "sd");
		CHECK(l.status == LoadStatus::Ok && !l.fromBackup && l.manifest.version == "CUR");
		CHECK(has(rec + ".bak") == (cut < 2) && has(rec + ".tmp") == (cut < 3));
		CHECK(removeManifest(t.path, "wiimc") && loadManifest(t.path, "wiimc", "sd").status == LoadStatus::Missing);
	}
	// The same for the journal.
	for (int cut : {1, 2, 3}) {
		RecTemp t;
		g_cutRoot = t.path;
		Journal cur = sampleJournal(JournalPhase::Commit);
		cur.step = 4;
		CHECK(saveJournal(t.path, cur));
		const std::string jp = t.path + "/apps/OpenShop/installed/.journal";
		Journal stale = sampleJournal(JournalPhase::Start);
		put(jp + ".bak", journalToJson(stale));
		put(jp + ".tmp", "half");
		g_removeCalls = 0;
		g_failAtRemove = cut;
		CHECK(runForked(crashRemoveJournal));
		g_failAtRemove = 0;
		JournalLoad l = loadJournal(t.path, "usb");
		CHECK(l.status == LoadStatus::Ok && !l.fromBackup && l.journal.phase == JournalPhase::Commit && l.journal.step == 4);
	}
	// A cut half way through the data write: the old record stays, the half tmp is never read, the next save wins.
	{
		RecTemp t;
		g_cutRoot = t.path;
		Manifest old = sampleManifest();
		old.version = "OLD";
		CHECK(saveManifest(t.path, old));
		CHECK(runForked(crashSaveDuringWrite));
		const std::string rec = t.path + "/apps/OpenShop/installed/wiimc.json";
		CHECK(has(rec + ".tmp"));
		ManifestLoad l = loadManifest(t.path, "wiimc", "sd");
		CHECK(l.status == LoadStatus::Ok && l.manifest.version == "OLD" && !l.fromBackup);
		ManifestList ml = listManifests(t.path, "sd");
		CHECK(ml.manifests.size() == 1 && ml.corrupt.empty());
		old.version = "AFTER";
		CHECK(saveManifest(t.path, old) && loadManifest(t.path, "wiimc", "sd").manifest.version == "AFTER" && !has(rec + ".tmp"));
	}
	// A cut in the save's own cleanup (the remove of a stale .bak after the rename): the NEW record is in place, and it wins.
	{
		RecTemp t;
		g_cutRoot = t.path;
		Manifest old = sampleManifest();
		old.version = "OLD";
		CHECK(saveManifest(t.path, old));
		const std::string rec = t.path + "/apps/OpenShop/installed/wiimc.json";
		put(rec + ".bak", manifestToJson(old));
		g_removeCalls = 0;
		g_failAtRemove = 1;
		CHECK(runForked(crashSaveAtRemove));
		g_failAtRemove = 0;
		ManifestLoad l = loadManifest(t.path, "wiimc", "sd");
		CHECK(l.status == LoadStatus::Ok && !l.fromBackup && l.manifest.version == "NEW" && has(rec + ".bak"));
		CHECK(saveManifest(t.path, old) && !has(rec + ".bak"));
	}
	// A cut during the journal write keeps the previous journal.
	{
		RecTemp t;
		CHECK(saveJournal(t.path, sampleJournal(JournalPhase::Start)));
		g_cutRoot = t.path;
		struct L { static void body() {
			FileOps ops;
			ops.write = writeThenExit;
			saveJournal(g_cutRoot, sampleJournal(JournalPhase::Commit), ops);
		} };
		CHECK(runForked(&L::body));
		JournalLoad l = loadJournal(t.path, "usb");
		CHECK(l.status == LoadStatus::Ok && l.journal.phase == JournalPhase::Start);
	}
}

// ---- Increment 7: the pure job queue ---------------------------------------------------------------------------------------------
static JobSpec specOf(const std::string& slug) {
	JobSpec s;
	s.slug = slug;
	s.name = slug;
	return s;
}
// Runs the head to completion the way Jobs does.
static JobId startHead(JobQueue& q) {
	const JobEntry* n = q.nextToStart();
	if (!n) return 0;
	const JobId id = n->id;
	CHECK(q.markStarted(id));
	return id;
}

static void testJobQueue() {
	JobQueue q;
	CHECK(!q.busy() && q.activeCount() == 0 && q.nextToStart() == nullptr && q.nextQueued() == 0);
	// FIFO, one at a time
	const JobId a = q.add(JobKind::Install, specOf("a")), b = q.add(JobKind::Install, specOf("b")), c = q.add(JobKind::Update, specOf("c"));
	const JobId d = q.add(JobKind::Delete, specOf("d")); // a Delete queued behind the installs
	CHECK(a && b && c && d && a < b && b < c && c < d);
	CHECK(q.activeCount() == 4 && q.userActiveCount() == 4 && q.busy());
	CHECK(q.position(a) == 0 && q.position(b) == 1 && q.position(c) == 2 && q.position(d) == 3 && q.position(999) == -1);
	CHECK(q.nextQueued() == a && q.nextToStart() && q.nextToStart()->id == a);
	CHECK(!q.markStarted(b)); // only the head can start
	CHECK(startHead(q) == a);
	CHECK(q.running() && q.running()->id == a && q.nextToStart() == nullptr); // one at a time
	CHECK(q.nextQueued() == b && q.position(a) == 0 && q.position(b) == 1);
	CHECK(q.find(a)->state == JobState::Installing);
	q.find(a)->cell->setStage(Stage::Downloading);
	q.syncRunning();
	CHECK(q.find(a)->state == JobState::Downloading);
	q.find(a)->cell->setStage(Stage::Verifying);
	q.syncRunning();
	CHECK(q.find(a)->state == JobState::Verifying);
	CHECK(q.finish(a, JobState::Done, ""));
	CHECK(q.history().size() == 1 && q.history()[0].id == a && q.history()[0].state == JobState::Done);
	CHECK(q.activeCount() == 3 && q.nextToStart() && q.nextToStart()->id == b);
	// cancelling a queued job removes it: no history, no running entry
	CHECK(q.cancel(c));
	CHECK(q.find(c) == nullptr && q.activeCount() == 2 && q.history().size() == 1);
	CHECK(!q.cancel(c)); // gone
	// cancelling the running job: Cancelled at once, the entry stays until the worker is done, no history entry
	CHECK(startHead(q) == b);
	CHECK(q.cancel(b));
	CHECK(q.find(b)->state == JobState::Cancelled && q.find(b)->cell->cancelled());
	CHECK(q.nextToStart() == nullptr); // d waits for the worker to let go of b
	CHECK(!q.cancel(b) && !q.remove(b)); // already cancelled; a running entry cannot be removed
	q.syncRunning();
	CHECK(q.find(b)->state == JobState::Cancelled); // the stage cannot revive it
	CHECK(q.finish(b, JobState::Done, "")); // the worker raced to Done: still Cancelled
	CHECK(q.find(b) == nullptr && q.history().size() == 1);
	CHECK(q.nextToStart() && q.nextToStart()->id == d && q.busy());
	// a job in commit cannot be cancelled
	CHECK(startHead(q) == d);
	CHECK(q.find(d)->cell->enterCommit() && q.find(d)->cell->inCommit());
	CHECK(!q.cancel(d) && q.find(d)->state != JobState::Cancelled && !q.find(d)->cell->cancelled());
	CHECK(q.finish(d, JobState::Failed, "boom"));
	CHECK(q.history().size() == 2 && q.history()[0].id == d && q.history()[0].state == JobState::Failed && q.history()[0].error == "boom");
	CHECK(!q.busy() && q.activeCount() == 0);
	// and a cancelled job cannot begin to commit
	{
		const JobId e = q.add(JobKind::Install, specOf("e"));
		CHECK(startHead(q) == e && q.cancel(e));
		CHECK(!q.find(e)->cell->enterCommit());
		CHECK(q.finish(e, JobState::Cancelled, ""));
	}
	// remove: a history entry, a queued job; not a running one
	const JobId f = q.add(JobKind::Install, specOf("f")), g = q.add(JobKind::Install, specOf("g"));
	CHECK(q.remove(g) && q.find(g) == nullptr);
	CHECK(startHead(q) == f && !q.remove(f));
	CHECK(q.finish(f, JobState::Done, ""));
	CHECK(q.remove(f) && q.history().size() == 2); // out of the history
	CHECK(!q.remove(f));
	// retry: only a Failed history entry, with the re-resolved spec, same kind; the old entry goes
	const JobId failedId = q.history()[0].id;
	CHECK(q.history()[0].state == JobState::Failed && q.history()[0].kind == JobKind::Delete);
	CHECK(q.retry(a, specOf("a2")) == 0); // a is Done
	const JobId r = q.retry(failedId, specOf("d2"));
	CHECK(r != 0 && r != failedId && q.find(failedId) == nullptr && q.find(r)->kind == JobKind::Delete && q.find(r)->spec.slug == "d2");
	CHECK(q.find(r)->state == JobState::Queued && q.position(r) == 0 && q.history().size() == 1);
	CHECK(q.retry(r, specOf("x")) == 0 && q.retry(12345, specOf("x")) == 0);
	// clearFinished
	CHECK(q.history().size() == 1);
	q.clearFinished();
	CHECK(q.history().empty() && q.activeCount() == 1);
	CHECK(q.cancel(r) && !q.busy());
}

static void testJobQueueHistoryAndCounts() {
	JobQueue q;
	// history capped at 20, newest kept
	std::vector<JobId> ids;
	for (int i = 0; i < 25; i++) {
		const JobId id = q.add(JobKind::Install, specOf("app" + std::to_string(i)));
		ids.push_back(id);
		CHECK(startHead(q) == id);
		CHECK(q.finish(id, i % 3 == 0 ? JobState::Failed : JobState::Done, "e"));
	}
	CHECK(q.history().size() == kHistoryMax && kHistoryMax == 20);
	CHECK(q.history().front().id == ids[24] && q.history().back().id == ids[5]); // ids 0..4 fell off
	CHECK(q.find(ids[4]) == nullptr && q.find(ids[5]) != nullptr);
	// internal jobs: counted in the line, never in the history, and Cleanup does not make the queue busy
	JobQueue p;
	const JobId cl = p.add(JobKind::Cleanup, specOf(""));
	CHECK(p.activeCount() == 1 && p.userActiveCount() == 0 && !p.busy());
	const JobId sc = p.add(JobKind::Scan, specOf(""));
	CHECK(p.activeCount() == 2 && p.userActiveCount() == 0 && p.busy()); // a Scan does block switching the device
	CHECK(startHead(p) == cl && p.finish(cl, JobState::Done, ""));
	CHECK(startHead(p) == sc && p.finish(sc, JobState::Failed, "x"));
	CHECK(p.history().empty() && p.activeCount() == 0);
	// a mix: counts, position, next in the queue
	const JobId s1 = p.add(JobKind::Sleep, specOf("s1")), s2 = p.add(JobKind::Sleep, specOf("s2")), s3 = p.add(JobKind::Sleep, specOf("s3"));
	CHECK(p.nextQueued() == s1);
	CHECK(startHead(p) == s1);
	CHECK(p.nextQueued() == s2 && p.position(s3) == 2 && p.activeCount() == 3);
	// shutdown: queued jobs go, the running one is cancelled, the answer says whether one could not be
	CHECK(!p.cancelAll());
	CHECK(p.activeCount() == 1 && p.find(s1)->state == JobState::Cancelled && p.find(s2) == nullptr);
	CHECK(p.finish(s1, JobState::Cancelled, "") && p.activeCount() == 0);
	const JobId s4 = p.add(JobKind::Sleep, specOf("s4"));
	CHECK(startHead(p) == s4 && p.find(s4)->cell->enterCommit());
	CHECK(p.cancelAll()); // committing: stuck
	CHECK(p.find(s4)->state != JobState::Cancelled);
	// positions and "next" count USER jobs only (review M1): Cleanup ahead of a user job does not push it back
	JobQueue u;
	const JobId uc = u.add(JobKind::Cleanup, specOf("")), u1 = u.add(JobKind::Install, specOf("u1")), us = u.add(JobKind::Scan, specOf("")),
	            u2 = u.add(JobKind::Install, specOf("u2"));
	CHECK(u.activeCount() == 4 && u.userActiveCount() == 2);
	CHECK(u.position(uc) == -1 && u.position(us) == -1 && u.position(u1) == 0 && u.position(u2) == 1 && u.position(12345) == -1);
	CHECK(u.nextQueued() == u1);
	CHECK(startHead(u) == uc); // Cleanup is running; the first user job is still "next", not one behind
	CHECK(u.nextQueued() == u1 && u.position(u1) == 0 && u.position(uc) == -1);
	CHECK(u.finish(uc, JobState::Done, "") && startHead(u) == u1);
	CHECK(u.nextQueued() == u2 && u.position(u1) == 0 && u.position(u2) == 1 && u.position(us) == -1);
	// a cell whose runner has returned cannot be cancelled any more (review M6); cancelled and committing cells keep their phase
	ProgressCell fin;
	fin.markFinished();
	CHECK(fin.finished() && !fin.requestCancel() && !fin.cancelled() && !fin.inCommit());
	ProgressCell fc, fm;
	CHECK(fc.requestCancel());
	fc.markFinished();
	CHECK(fc.cancelled() && !fc.finished());
	CHECK(fm.enterCommit());
	fm.markFinished();
	CHECK(fm.inCommit() && !fm.finished() && !fm.requestCancel());
	// ids keep growing; the cell saturates its 32-bit counters
	const JobId s5 = p.add(JobKind::Sleep, specOf("s5"));
	CHECK(s5 > s4);
	ProgressCell cell;
	cell.setProgress(6ull << 30, 5ull << 32);
	CHECK(cell.done() == 0xFFFFFFFFu && cell.total() == 0xFFFFFFFFu);
	CHECK(std::string(stageLabel(Stage::Cleaning)) == "Cleaning up" && stateForStage(Stage::Sleeping) == JobState::Installing &&
	      stateForStage(Stage::Downloading) == JobState::Downloading);
}

static void testDevJobOptions() {
	DevOptions o;
	DevOptions::parseFile("devjobs=3\ndevjobms=200\ndevjobfail=1\ndevjobcommit=1\n", o);
	CHECK(o.devJobs == 3 && o.devJobMs == 200 && o.devJobFail && o.devJobCommit);
	CHECK(o.describe().find("devjobs=3") != std::string::npos && o.active().find("devjobfail=1") != std::string::npos);
	DevOptions p;
	DevOptions::parseFile("devjobs=abc\ndevjobs=-4\n", p);
	CHECK(p.devJobs == 0 && p.active().find("devjobs") == std::string::npos);
	CHECK(DevOptions().active().find("devjob") == std::string::npos);
	// capped at 100 (review L9): in the devstart file and in the environment
	DevOptions c;
	DevOptions::parseFile("devjobs=100\n", c);
	CHECK(c.devJobs == 100);
	DevOptions::parseFile("devjobs=101\n", c);
	CHECK(c.devJobs == 100);
	DevOptions::parseFile("devjobs=999999\n", c);
	CHECK(c.devJobs == 100 && DevOptions::kMaxDevJobs == 100);
	setenv("OPENSHOP_DEVJOBS", "5000", 1);
	CHECK(DevOptions::load("/nonexistent-openshop-dir").devJobs == 100);
	setenv("OPENSHOP_DEVJOBS", "7", 1);
	CHECK(DevOptions::load("/nonexistent-openshop-dir").devJobs == 7);
	unsetenv("OPENSHOP_DEVJOBS");
}

// ---- Increment 8: Preflight, Md5Sink, Messages, NetState, the install= dev option ----

static JobSpec mameSpec() {
	JobSpec s;
	s.slug = "mame4wii";
	s.name = "MAME4Wii";
	s.url = "http://hbb1.oscwii.org/api/contents/mame4wii/mame4wii.zip";
	s.md5 = "e996304d3eb08046ff9aeb76996fa93a";
	s.archiveSize = 78687769;
	s.uncompressedSize = 1798841;
	s.packageType = static_cast<uint8_t>(PackageType::Dol);
	s.dirs = {"/mame", "/mame/roms", "/apps/mame4wii"};
	s.targetRoot = "sd:";
	s.device = "sd";
	return s;
}

static void testPreflight() {
	constexpr uint64_t kMb = 1024 * 1024;
	// The formula, with mame4wii's real sizes: archive + max(unc, archive) = 2 x 78687769, plus 10% of that sum, plus 1 MB.
	const uint64_t sum = 78687769ull * 2;
	CHECK(spaceNeeded(78687769, 1798841) == sum + sum / 10 + kMb);
	CHECK(spaceNeeded(78687769, 1798841) == 157375538ull + 15737553ull + 1048576ull);
	// An archive smaller than its extracted size: archive + unc.
	CHECK(spaceNeeded(1000, 5000) == 6000 + 600 + kMb);
	CHECK(spaceNeeded(0, 0) == kMb);
	CHECK(spaceNeeded(UINT64_MAX, UINT64_MAX) == UINT64_MAX && spaceNeeded(UINT64_MAX / 2 + 1, 0) == UINT64_MAX); // saturates, never wraps

	const JobSpec s = mameSpec();
	CHECK(refusalOf(s) == InstallRefusal::None);
	const uint64_t need = spaceNeeded(s.archiveSize, s.uncompressedSize);
	PreflightEnv env;
	env.freeSpace = [&](const std::string& root) -> std::optional<uint64_t> { return root == "sd:" ? std::optional<uint64_t>(need) : std::nullopt; };
	PreflightResult r = preflight(s, Device::Sd, env);
	CHECK(r.ok() && r.needed == need && r.message.empty());
	// One byte short -> NoSpace, with the amount in the text.
	env.freeSpace = [&](const std::string&) -> std::optional<uint64_t> { return need - 1; };
	r = preflight(s, Device::Sd, env);
	CHECK(r.fail == PreflightFail::NoSpace && r.needed == need && r.have == need - 1);
	CHECK(r.message == installErrorText(InstallError::NoSpace, Device::Sd, need, need - 1));
	CHECK(r.message.find(megabytes(need)) != std::string::npos && r.message.find("SD card") != std::string::npos);
	CHECK(megabytes(need) == "167 MB" && megabytes(1) == "1 MB" && megabytes(0) == "1 MB" && megabytes(kMb) == "1 MB" && megabytes(kMb + 1) == "2 MB");
	// Needed rounds UP, free rounds DOWN (the text never promises more room than there is), under 1 MB is "less than 1 MB".
	CHECK(installErrorText(InstallError::NoSpace, Device::Sd, 5 * kMb + kMb / 5, 5 * kMb + kMb / 10) ==
	      "Not enough space on the SD card: about 6 MB is needed, 5 MB is free.");
	CHECK(installErrorText(InstallError::NoSpace, Device::Sd, 5 * kMb, 0) ==
	      "Not enough space on the SD card: about 5 MB is needed, less than 1 MB is free.");
	CHECK(freeMegabytes(0) == "less than 1 MB" && freeMegabytes(kMb - 1) == "less than 1 MB" && freeMegabytes(kMb) == "1 MB" &&
	      freeMegabytes(2 * kMb - 1) == "1 MB" && freeMegabytes(2 * kMb) == "2 MB");
	r = preflight(s, Device::Usb, env);
	CHECK(r.message.find("USB drive") != std::string::npos);
	// Unknown passes at queue time, Down fails.
	env.freeSpace = [&](const std::string&) -> std::optional<uint64_t> { return need + 5; };
	env.net = net::NetState::Unknown;
	CHECK(preflight(s, Device::Sd, env).ok());
	env.net = net::NetState::Up;
	CHECK(preflight(s, Device::Sd, env).ok());
	env.net = net::NetState::Down;
	r = preflight(s, Device::Sd, env);
	CHECK(r.fail == PreflightFail::NetworkDown && r.message == installErrorText(InstallError::NetworkDown));
	env.net = net::NetState::Unknown;
	// A URL that is not an allowlisted OSC archive URL fails (the real allowlist), whatever else is fine.
	JobSpec bad = s;
	bad.url = "http://example.org/api/contents/mame4wii/mame4wii.zip";
	r = preflight(bad, Device::Sd, env);
	CHECK(!r.ok() && (r.fail == PreflightFail::Refused || r.fail == PreflightFail::UrlNotAllowed));
	bad.url = "https://hbb1.oscwii.org/api/contents/mame4wii/mame4wii.zip"; // the https form is never allowed
	CHECK(!preflight(bad, Device::Sd, env).ok());
	// decide() on its own: a refusal that installRefusal() would not give but the URL rule still catches.
	PreflightInput in;
	in.refusal = InstallRefusal::None;
	in.urlAllowed = false;
	in.hasAppsDevice = true;
	in.freeBytes = UINT64_MAX;
	CHECK(decide(in).fail == PreflightFail::UrlNotAllowed);
	in.urlAllowed = true;
	CHECK(decide(in).ok());
	// An apps device of None fails with exactly the plain text.
	in.hasAppsDevice = false;
	r = decide(in);
	CHECK(r.fail == PreflightFail::NoAppsDevice && r.message == "No SD card or USB drive found.");
	JobSpec none = s;
	none.targetRoot.clear();
	none.device = "none";
	r = preflight(none, Device::None, env);
	CHECK(r.fail == PreflightFail::NoAppsDevice && r.message == "No SD card or USB drive found.");
	// Free space that cannot be read is a refusal, not "plenty".
	in.hasAppsDevice = true;
	in.freeBytes.reset();
	CHECK(decide(in).fail == PreflightFail::DiskUnreadable);
	// A refusal beats everything after it.
	in.freeBytes = 0;
	in.refusal = InstallRefusal::BadMd5;
	r = decide(in);
	CHECK(r.fail == PreflightFail::Refused && r.message == refusalText(InstallRefusal::BadMd5));
	// A spec the catalog would refuse (no MD5) is refused from the copy too: "empty means refuse".
	JobSpec nomd5 = s;
	nomd5.md5.clear();
	CHECK(refusalOf(nomd5) == InstallRefusal::BadMd5);
	// Real fixture entries pass the spec round trip: every installable fixture app keeps passing refusalOf.
	// NetState: Down expires by itself, Unknown and Up never do.
	const uint32_t t0 = 1000;
	CHECK(net::effectiveNetState(net::NetState::Down, t0, t0 + net::kDownTtlMs - 1) == net::NetState::Down);
	CHECK(net::effectiveNetState(net::NetState::Down, t0, t0 + net::kDownTtlMs) == net::NetState::Unknown);
	CHECK(net::effectiveNetState(net::NetState::Up, t0, t0 + 10 * net::kDownTtlMs) == net::NetState::Up);
	CHECK(net::effectiveNetState(net::NetState::Down, 0xFFFFFFF0u, 0x00000010u) == net::NetState::Down); // across the clock wrap
	net::setNetState(net::NetState::Down, 5000);
	CHECK(net::netState(5000 + 100) == net::NetState::Down && net::netState(5000 + net::kDownTtlMs + 1) == net::NetState::Unknown);
	net::setNetState(net::NetState::Unknown, 0);
}

// An inner sink that remembers what it got and can refuse after a number of bytes.
struct CaptureSink : HttpSink {
	std::string got;
	bool started = false, finished = false, finishedOk = false;
	size_t failAfter = SIZE_MAX; // refuses data once this many bytes are stored
	bool refuseStart = false;
	bool onStart(int, int64_t) override { started = true; return !refuseStart; }
	bool onData(const uint8_t* d, size_t n) override {
		if (got.size() + n > failAfter) return false;
		got.append(reinterpret_cast<const char*>(d), n);
		return true;
	}
	void onFinish(bool ok) override { finished = true; finishedOk = ok; }
};

static void testMd5Sink() {
	std::string body(200003, 0);
	for (size_t i = 0; i < body.size(); i++) body[i] = static_cast<char>(i * 167 + (i >> 8) * 13 + 5);
	const std::string want = md5Of(body.data(), body.size());
	// The chunked digest equals the one-shot digest, whatever the slicing.
	for (size_t chunk : {size_t(1), size_t(63), size_t(64), size_t(4096), size_t(65536), body.size()}) {
		auto cap = std::make_unique<CaptureSink>();
		CaptureSink* c = cap.get();
		Md5Sink m(std::move(cap), body.size());
		CHECK(m.onStart(200, static_cast<int64_t>(body.size())));
		bool ok = true;
		for (size_t i = 0; i < body.size(); i += chunk) ok = ok && m.onData(reinterpret_cast<const uint8_t*>(body.data()) + i, std::min(chunk, body.size() - i));
		CHECK(ok);
		m.onFinish(true);
		CHECK(m.digestHex() == want && m.bytes() == body.size() && m.finished());
		CHECK(c->got == body && c->finished && c->finishedOk);
	}
	CHECK(md5Hex(reinterpret_cast<const unsigned char*>("0123456789abcdef")).size() == 32);
	CHECK(sameHex("ABCDEF0123456789abcdef0123456789", "abcdef0123456789ABCDEF0123456789") && !sameHex("ab", "abc") && !sameHex("ab", "ac"));
	{
		// Content-Length that is not the catalog's size -> TooLarge, before anything is written; both directions.
		for (int64_t cl : {int64_t(99), int64_t(101), int64_t(0)}) {
			auto cap = std::make_unique<CaptureSink>();
			CaptureSink* c = cap.get();
			Md5Sink m(std::move(cap), 100);
			CHECK(!m.onStart(200, cl) && m.failure() == HttpError::TooLarge && !c->started);
		}
		// An unknown length (chunked) passes the header rule.
		Md5Sink m(std::make_unique<CaptureSink>(), 100);
		CHECK(m.onStart(200, -1));
	}
	{
		// Exactly the size is fine; one byte more is TooLarge and that byte is not written or hashed.
		auto cap = std::make_unique<CaptureSink>();
		CaptureSink* c = cap.get();
		Md5Sink m(std::move(cap), 10);
		const uint8_t d[11] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
		CHECK(m.onStart(200, 10) && m.onData(d, 10) && m.bytes() == 10);
		CHECK(!m.onData(d + 10, 1) && m.failure() == HttpError::TooLarge && m.bytes() == 10 && c->got.size() == 10);
		// Straddling the limit in one slice: refused whole.
		Md5Sink m2(std::make_unique<CaptureSink>(), 10);
		CHECK(m2.onStart(200, -1) && m2.onData(d, 6) && !m2.onData(d, 5) && m2.failure() == HttpError::TooLarge && m2.bytes() == 6);
		m2.onFinish(false);
		CHECK(m2.digestHex().empty()); // no digest for a failed transfer
	}
	{
		// The inner sink failing (a write error, ENOSPC) -> Io; a refused start too.
		auto cap = std::make_unique<CaptureSink>();
		cap->failAfter = 5;
		Md5Sink m(std::move(cap), 100);
		const uint8_t d[8] = {0};
		CHECK(m.onStart(200, 100) && m.onData(d, 5) && !m.onData(d, 3) && m.failure() == HttpError::Io && m.bytes() == 5);
		auto cap2 = std::make_unique<CaptureSink>();
		cap2->refuseStart = true;
		Md5Sink m2(std::move(cap2), 100);
		CHECK(!m2.onStart(200, 100) && m2.failure() == HttpError::Io);
	}
	{
		// A short body is not refused by the sink: verify compares bytes() with the catalog's size.
		Md5Sink m(std::make_unique<CaptureSink>(), 100);
		const uint8_t d[4] = {1, 2, 3, 4};
		CHECK(m.onStart(200, -1) && m.onData(d, 4));
		m.onFinish(true);
		CHECK(m.bytes() == 4 && m.digestHex() == md5Of(d, 4));
	}
	// The real FileSink underneath, into a temp dir: the .tmp becomes the file only on commit(), and nothing is left on failure.
	{
		const std::string dir = "/tmp/openshop-md5sink-test";
		std::filesystem::remove_all(dir);
		std::filesystem::create_directories(dir);
		auto fsink = std::make_unique<FileSink>(dir + "/download.zip");
		FileSink* f = fsink.get();
		Md5Sink m(std::move(fsink), 11);
		CHECK(m.onStart(200, 11) && m.onData(reinterpret_cast<const uint8_t*>("hello world"), 11));
		m.onFinish(true);
		CHECK(std::filesystem::exists(dir + "/download.zip.tmp") && !std::filesystem::exists(dir + "/download.zip"));
		CHECK(f->commit() && std::filesystem::exists(dir + "/download.zip") && !std::filesystem::exists(dir + "/download.zip.tmp"));
		CHECK(m.digestHex() == "5eb63bbbe01eeed093cb22bb8f5acdc3");
		{
			Md5Sink m3(std::make_unique<FileSink>(dir + "/other.zip"), 3);
			CHECK(m3.onStart(200, -1) && !m3.onData(reinterpret_cast<const uint8_t*>("abcd"), 4));
			m3.onFinish(false);
		}
		CHECK(!std::filesystem::exists(dir + "/other.zip") && !std::filesystem::exists(dir + "/other.zip.tmp")); // the tmp is removed
		std::filesystem::remove_all(dir);
	}
}

static std::string lower(std::string s) {
	for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return s;
}
static bool cleanWording(const std::string& t) {
	const std::string l = lower(t);
	for (const char* w : {"secure", "verif", "tamper", "safe", "md5", "checksum", "hash", "https", "encrypt", "unencrypted", "tls"})
		if (l.find(w) != std::string::npos) return false;
	return true;
}

static void testMessages() {
	// Every HttpError maps to a non-empty text (None to ""), none with security wording.
	for (int i = 0; i <= static_cast<int>(HttpError::Redirects); i++) {
		const HttpError e = static_cast<HttpError>(i);
		const std::string t = downloadErrorText(e, 503);
		CHECK(e == HttpError::None ? t.empty() : !t.empty());
		CHECK(cleanWording(t));
	}
	CHECK(downloadErrorText(HttpError::Redirects) == "The shop's server sent an unexpected answer.");
	CHECK(downloadErrorText(HttpError::TlsRequired) == downloadErrorText(HttpError::Redirects)); // can't occur for http, maps to "unexpected"
	CHECK(downloadErrorText(HttpError::HttpStatus, 404).find("404") != std::string::npos);
	CHECK(!downloadErrorText(HttpError::HttpStatus, 0).empty());
	for (int i = 0; i <= static_cast<int>(InstallError::DiskUnreadable); i++) {
		const std::string t = installErrorText(static_cast<InstallError>(i), Device::Usb, 5 * 1024 * 1024, 1024);
		CHECK(!t.empty() && cleanWording(t));
		CHECK(!installErrorText(static_cast<InstallError>(i)).empty()); // and with no device and no amounts
	}
	CHECK(installErrorText(InstallError::Damaged) == "The download was damaged. Please try again.");
	CHECK(installErrorText(InstallError::NoAppsDevice) == "No SD card or USB drive found.");
	CHECK(installErrorText(InstallError::NoSpace, Device::Sd, 5 * 1024 * 1024, 1024).find("5 MB is needed, less than 1 MB is free") != std::string::npos);
	CHECK(std::string(deviceName(Device::Sd)) == "SD card" && std::string(deviceName(Device::Usb)) == "USB drive");
	// Every refusal reason has a text (None has none) and clean wording; each says why in plain words.
	for (int i = 0; i <= static_cast<int>(InstallRefusal::NoAppDir); i++) {
		const InstallRefusal r = static_cast<InstallRefusal>(i);
		const std::string t = refusalText(r);
		CHECK(r == InstallRefusal::None ? t.empty() : (!t.empty() && cleanWording(t)));
	}
}

static void testInstallDevOption() {
	DevOptions o;
	DevOptions::parseFile("install=TinyLoad\nbadmd5=1\n", o);
	CHECK(o.installSlug == "TinyLoad" && o.testBadMd5);
	CHECK(o.describe().find("install=TinyLoad") != std::string::npos && o.active().find("install=TinyLoad") != std::string::npos);
	CHECK(o.active().find("badmd5=1") != std::string::npos);
	CHECK(DevOptions().active().find("install") == std::string::npos && DevOptions().installSlug.empty() && !DevOptions().testBadMd5);
	setenv("OPENSHOP_DEV_INSTALL", " Eject_DVD ", 1);
	setenv("OPENSHOP_TEST_BAD_MD5", "1", 1);
	const DevOptions e = DevOptions::load("/nonexistent-openshop-dir");
	CHECK(e.installSlug == "Eject_DVD" && e.testBadMd5);
	setenv("OPENSHOP_TEST_BAD_MD5", "0", 1);
	CHECK(!DevOptions::load("/nonexistent-openshop-dir").testBadMd5);
	unsetenv("OPENSHOP_DEV_INSTALL");
	unsetenv("OPENSHOP_TEST_BAD_MD5");
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
	testEntryNames();
	testUnicodeHardening();
	testReviewerNames();
	testFoldAndRoots();
	testFixtureSweep();
	testSlugPaths();
	testVersions();
	testMetaXml();
	testManifestRoundTrip();
	testManifestCorrupt();
	testManifestPathFlags();
	testManifestQueries();
	testMergeForUpdate();
	testRecordWrites();
	testJournal();
	testJournalCorrupt();
	testJournalCrash();
	testManifestUnicodeFold();
	testManifestCapsAndShapes();
	testManifestSaveValidation();
	testManifestDeviceAndFlags();
	testJournalBindings();
	testJournalConsistency();
	testJournalSizeAndFlush();
	testRecordCuts();
	testJobQueue();
	testJobQueueHistoryAndCounts();
	testDevJobOptions();
	testPreflight();
	testMd5Sink();
	testMessages();
	testInstallDevOption();
	return checkReport("install_test");
}
