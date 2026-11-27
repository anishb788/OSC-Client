// SPDX-License-Identifier: GPL-3.0-or-later
// Host tests for the pure install modules. Build and run: make -f Makefile.pc test
// Increment 1: the vendored MD5 and the libzip link.
#include <cstdio>
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
#include "install/MetaXml.h"
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
	return checkReport("install_test");
}
