#include "core/DevOptions.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace {
std::string trim(const std::string& s) {
	size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
	return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

bool truthy(const std::string& v) {
	return !v.empty() && v != "0" && v != "false" && v != "no" && v != "off";
}

// The API base without trailing slashes ("<base>/information" is requested).
std::string cleanApiBase(std::string v) {
	while (!v.empty() && v.back() == '/') v.pop_back();
	return v;
}

// A non-negative integer, or -1 (use the default) for anything else.
int parseNonNegative(const std::string& v) {
	if (v.empty() || v.size() > 7) return -1;
	int n = 0;
	for (char c : v) {
		if (c < '0' || c > '9') return -1;
		n = n * 10 + (c - '0');
	}
	return n;
}

// Plain stdio and getenv (no SDL or platform layer) so the host tests can link this file on its own.
bool readSmallFile(const std::string& path, std::string& out) {
	std::FILE* f = std::fopen(path.c_str(), "rb");
	if (!f) return false;
	char buf[512];
	size_t n;
	while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && out.size() < 4096) out.append(buf, n);
	std::fclose(f);
	return true;
}
} // namespace

void DevOptions::parseFail(const std::string& val, DevOptions& o) {
	o.failStartup = o.failRefresh = false;
	if (val == "refresh") o.failRefresh = true;
	else if (val == "both") o.failStartup = o.failRefresh = true;
	else if (val == "none") return;
	else o.failStartup = truthy(val); // "startup", "1" and any other truthy value
}

void DevOptions::parseFile(const std::string& text, DevOptions& o) {
	size_t pos = 0;
	while (pos <= text.size()) {
		size_t end = text.find('\n', pos);
		if (end == std::string::npos) end = text.size();
		std::string line = trim(text.substr(pos, end - pos));
		pos = end + 1;
		if (line.empty() || line[0] == '#') continue;
		size_t eq = line.find('=');
		if (eq == std::string::npos) { o.screen = line; continue; } // bare screen name
		std::string key = trim(line.substr(0, eq)), val = trim(line.substr(eq + 1));
		if (key == "screen") o.screen = val;
		else if (key == "mock") o.mock = truthy(val);
		else if (key == "offline") o.offline = truthy(val);
		else if (key == "refresh") o.refresh = truthy(val);
		else if (key == "fail") parseFail(val, o);
		else if (key == "mockdelay") o.mockDelayMs = parseNonNegative(val);
		else if (key == "ratelimit") o.rateLimitSec = parseNonNegative(val);
		else if (key == "cachettl") o.cacheTtlSec = parseNonNegative(val);
		else if (key == "autorefresh") o.autoRefreshMs = parseNonNegative(val);
		else if (key == "api") o.apiBase = cleanApiBase(val);
		else if (key == "icons") o.iconBase = cleanApiBase(val);
		else if (key == "topbar") o.topbarAll = val == "all";
		else if (key == "perf") o.perf = truthy(val);
		else if (key == "heapprobe") o.heapProbe = truthy(val);
		else if (key == "stackprobe") o.stackProbe = truthy(val);
		else if (key == "commitprobe") o.commitProbe = val;
		else if (key == "commitprobe_files") o.commitProbeFiles = parseNonNegative(val);
		else if (key == "commitprobe_stack") o.commitProbeStackKb = parseNonNegative(val);
		else if (key == "claimprobe") o.claimProbe = truthy(val);
		else if (key == "nettest") o.nettest = val;
		else if (key == "nettestbg") o.nettestBg = val;
		else if (key == "nettestquit") o.nettestQuit = truthy(val);
		else if (key == "appsdevice") o.appsDevice = val;
		else if (key == "appsdeviceswap") o.appsDeviceSwap = truthy(val);
		else if (key == "devjobs") o.devJobs = std::clamp(parseNonNegative(val), 0, kMaxDevJobs);
		else if (key == "devjobms") o.devJobMs = parseNonNegative(val);
		else if (key == "devjobfail") o.devJobFail = truthy(val);
		else if (key == "devjobcommit") o.devJobCommit = truthy(val);
		else if (key == "install") o.installSlug = val;
		else if (key == "badmd5") o.testBadMd5 = truthy(val);
	}
}

DevOptions DevOptions::load(const std::string& dataDir) {
	DevOptions o;
	std::string text;
	if (readSmallFile(dataDir + "/devstart", text)) parseFile(text, o);
	if (const char* v = std::getenv("OPENSHOP_START"); v && *v) o.screen = v;
	if (const char* v = std::getenv("OPENSHOP_MOCK"); v && *v) o.mock = truthy(v);
	if (const char* v = std::getenv("OPENSHOP_OFFLINE"); v && *v) o.offline = truthy(v);
	if (const char* v = std::getenv("OPENSHOP_REFRESH"); v && *v) o.refresh = truthy(v);
	if (const char* v = std::getenv("OPENSHOP_FAIL"); v && *v) parseFail(v, o);
	if (const char* v = std::getenv("OPENSHOP_MOCKDELAY"); v && *v) o.mockDelayMs = parseNonNegative(v);
	if (const char* v = std::getenv("OPENSHOP_RATELIMIT"); v && *v) o.rateLimitSec = parseNonNegative(v);
	if (const char* v = std::getenv("OPENSHOP_CACHETTL"); v && *v) o.cacheTtlSec = parseNonNegative(v);
	if (const char* v = std::getenv("OPENSHOP_AUTOREFRESH"); v && *v) o.autoRefreshMs = parseNonNegative(v);
	if (const char* v = std::getenv("OPENSHOP_API"); v && *v) o.apiBase = cleanApiBase(v);
	if (const char* v = std::getenv("OPENSHOP_ICONS"); v && *v) o.iconBase = cleanApiBase(v);
	if (const char* v = std::getenv("OPENSHOP_TOPBAR"); v && *v) o.topbarAll = std::string(v) == "all";
	if (const char* v = std::getenv("OPENSHOP_PERF"); v && *v) o.perf = truthy(v);
	if (const char* v = std::getenv("OPENSHOP_HEAPPROBE"); v && *v) o.heapProbe = truthy(v);
	if (const char* v = std::getenv("OPENSHOP_STACKPROBE"); v && *v) o.stackProbe = truthy(v);
	if (const char* v = std::getenv("OPENSHOP_COMMITPROBE"); v && *v) o.commitProbe = trim(v);
	if (const char* v = std::getenv("OPENSHOP_COMMITPROBE_FILES"); v && *v) o.commitProbeFiles = parseNonNegative(v);
	if (const char* v = std::getenv("OPENSHOP_COMMITPROBE_STACK"); v && *v) o.commitProbeStackKb = parseNonNegative(v);
	if (const char* v = std::getenv("OPENSHOP_CLAIMPROBE"); v && *v) o.claimProbe = truthy(v);
	if (const char* v = std::getenv("OPENSHOP_NETTEST"); v && *v) o.nettest = v;
	if (const char* v = std::getenv("OPENSHOP_NETTESTBG"); v && *v) o.nettestBg = v;
	if (const char* v = std::getenv("OPENSHOP_NETTESTQUIT"); v && *v) o.nettestQuit = truthy(v);
	if (const char* v = std::getenv("OPENSHOP_APPS_DEVICE"); v && *v) o.appsDevice = trim(v);
	if (const char* v = std::getenv("OPENSHOP_APPS_DEVICE_SWAP"); v && *v) o.appsDeviceSwap = truthy(v);
	if (const char* v = std::getenv("OPENSHOP_DEVJOBS"); v && *v) o.devJobs = std::clamp(parseNonNegative(v), 0, kMaxDevJobs);
	if (const char* v = std::getenv("OPENSHOP_DEVJOBMS"); v && *v) o.devJobMs = parseNonNegative(v);
	if (const char* v = std::getenv("OPENSHOP_DEVJOBFAIL"); v && *v) o.devJobFail = truthy(v);
	if (const char* v = std::getenv("OPENSHOP_DEVJOBCOMMIT"); v && *v) o.devJobCommit = truthy(v);
	if (const char* v = std::getenv("OPENSHOP_DEV_INSTALL"); v && *v) o.installSlug = trim(v);
	if (const char* v = std::getenv("OPENSHOP_TEST_BAD_MD5"); v && *v) o.testBadMd5 = truthy(v);
	return o;
}

std::string DevOptions::describe() const {
	const char* failText = failStartup && failRefresh ? "both" : failStartup ? "startup" : failRefresh ? "refresh" : "none";
	return "screen=" + (screen.empty() ? std::string("(none)") : screen) + " mock=" + (mock ? "1" : "0") +
	       " offline=" + (offline ? "1" : "0") + " refresh=" + (refresh ? "1" : "0") + " fail=" + failText +
	       " topbar=" + (topbarAll ? "all" : "default") + " mockdelay=" + std::to_string(mockDelayMs) +
	       " ratelimit=" + std::to_string(rateLimitSec) + " perf=" + (perf ? "1" : "0") +
	       (cacheTtlSec < 0 ? std::string() : " cachettl=" + std::to_string(cacheTtlSec)) +
	       (apiBase.empty() ? std::string() : " api=" + apiBase) +
	       (iconBase.empty() ? std::string() : " icons=" + iconBase) +
	       (autoRefreshMs < 0 ? std::string() : " autorefresh=" + std::to_string(autoRefreshMs)) +
	       (heapProbe ? std::string(" heapprobe=1") : std::string()) + (stackProbe ? std::string(" stackprobe=1") : std::string()) +
	       (commitProbe.empty() ? std::string() : " commitprobe=" + commitProbe) +
	       (commitProbeFiles < 0 ? std::string() : " commitprobe_files=" + std::to_string(commitProbeFiles)) +
	       (commitProbeStackKb < 0 ? std::string() : " commitprobe_stack=" + std::to_string(commitProbeStackKb)) +
	       (claimProbe ? std::string(" claimprobe=1") : std::string()) +
	       (appsDevice.empty() ? std::string() : " appsdevice=" + appsDevice) + (appsDeviceSwap ? std::string(" appsdeviceswap=1") : std::string()) +
	       (devJobs > 0 ? " devjobs=" + std::to_string(devJobs) + (devJobMs >= 0 ? " devjobms=" + std::to_string(devJobMs) : std::string()) +
	                          (devJobFail ? " devjobfail=1" : "") + (devJobCommit ? " devjobcommit=1" : "")
	                    : std::string()) +
	       (installSlug.empty() ? std::string() : " install=" + installSlug + (testBadMd5 ? " badmd5=1" : "")) +
	       (nettest.empty() && nettestBg.empty() ? std::string() : " nettest=" + nettest + " nettestbg=" + nettestBg + " nettestquit=" + (nettestQuit ? "1" : "0"));
}

std::string DevOptions::active() const {
	std::string out;
	auto add = [&out](const std::string& s) { out += (out.empty() ? "" : " ") + s; };
	// "home" is the normal start, so it does not count.
	if (!screen.empty() && screen != "home") add("screen=" + screen);
	if (mock) add("mock=1");
	if (offline) add("offline=1");
	if (refresh) add("refresh=1");
	if (failStartup || failRefresh) add(std::string("fail=") + (failStartup && failRefresh ? "both" : failStartup ? "startup" : "refresh"));
	if (topbarAll) add("topbar=all");
	if (mockDelayMs >= 0) add("mockdelay=" + std::to_string(mockDelayMs));
	if (rateLimitSec >= 0) add("ratelimit=" + std::to_string(rateLimitSec));
	if (cacheTtlSec >= 0) add("cachettl=" + std::to_string(cacheTtlSec));
	if (!apiBase.empty()) add("api=" + apiBase);
	if (!iconBase.empty()) add("icons=" + iconBase);
	if (autoRefreshMs >= 0) add("autorefresh=" + std::to_string(autoRefreshMs));
	if (perf) add("perf=1");
	if (heapProbe) add("heapprobe=1");
	if (stackProbe) add("stackprobe=1");
	if (!commitProbe.empty()) add("commitprobe=" + commitProbe);
	if (commitProbeFiles >= 0) add("commitprobe_files=" + std::to_string(commitProbeFiles));
	if (commitProbeStackKb >= 0) add("commitprobe_stack=" + std::to_string(commitProbeStackKb));
	if (claimProbe) add("claimprobe=1");
	if (!appsDevice.empty()) add("appsdevice=" + appsDevice);
	if (appsDeviceSwap) add("appsdeviceswap=1");
	if (devJobs > 0) add("devjobs=" + std::to_string(devJobs));
	if (devJobs > 0 && devJobMs >= 0) add("devjobms=" + std::to_string(devJobMs));
	if (devJobs > 0 && devJobFail) add("devjobfail=1");
	if (devJobs > 0 && devJobCommit) add("devjobcommit=1");
	if (!installSlug.empty()) add("install=" + installSlug);
	if (!installSlug.empty() && testBadMd5) add("badmd5=1");
	if (!nettest.empty() || !nettestBg.empty() || nettestQuit) add("nettest=" + nettest + " nettestbg=" + nettestBg);
	return out;
}
