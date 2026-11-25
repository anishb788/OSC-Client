#include "core/DevOptions.h"

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
		else if (key == "autorefresh") o.autoRefreshMs = parseNonNegative(val);
		else if (key == "api") o.apiBase = cleanApiBase(val);
		else if (key == "topbar") o.topbarAll = val == "all";
		else if (key == "perf") o.perf = truthy(val);
		else if (key == "nettest") o.nettest = val;
		else if (key == "nettestbg") o.nettestBg = val;
		else if (key == "nettestquit") o.nettestQuit = truthy(val);
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
	if (const char* v = std::getenv("OPENSHOP_AUTOREFRESH"); v && *v) o.autoRefreshMs = parseNonNegative(v);
	if (const char* v = std::getenv("OPENSHOP_API"); v && *v) o.apiBase = cleanApiBase(v);
	if (const char* v = std::getenv("OPENSHOP_TOPBAR"); v && *v) o.topbarAll = std::string(v) == "all";
	if (const char* v = std::getenv("OPENSHOP_PERF"); v && *v) o.perf = truthy(v);
	if (const char* v = std::getenv("OPENSHOP_NETTEST"); v && *v) o.nettest = v;
	if (const char* v = std::getenv("OPENSHOP_NETTESTBG"); v && *v) o.nettestBg = v;
	if (const char* v = std::getenv("OPENSHOP_NETTESTQUIT"); v && *v) o.nettestQuit = truthy(v);
	return o;
}

std::string DevOptions::describe() const {
	const char* failText = failStartup && failRefresh ? "both" : failStartup ? "startup" : failRefresh ? "refresh" : "none";
	return "screen=" + (screen.empty() ? std::string("(none)") : screen) + " mock=" + (mock ? "1" : "0") +
	       " offline=" + (offline ? "1" : "0") + " refresh=" + (refresh ? "1" : "0") + " fail=" + failText +
	       " topbar=" + (topbarAll ? "all" : "default") + " mockdelay=" + std::to_string(mockDelayMs) +
	       " ratelimit=" + std::to_string(rateLimitSec) + " perf=" + (perf ? "1" : "0") +
	       (apiBase.empty() ? std::string() : " api=" + apiBase) +
	       (autoRefreshMs < 0 ? std::string() : " autorefresh=" + std::to_string(autoRefreshMs)) +
	       (nettest.empty() && nettestBg.empty() ? std::string() : " nettest=" + nettest + " nettestbg=" + nettestBg + " nettestquit=" + (nettestQuit ? "1" : "0"));
}
