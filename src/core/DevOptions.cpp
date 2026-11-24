#include "core/DevOptions.h"

#include <SDL.h>

#include "platform/Platform.h"

namespace {
std::string trim(const std::string& s) {
	size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
	return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

bool truthy(const std::string& v) {
	return !v.empty() && v != "0" && v != "false" && v != "no" && v != "off";
}
} // namespace

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
	}
}

DevOptions DevOptions::load(const std::string& dataDir) {
	DevOptions o;
	std::string text;
	if (platform::readFile(dataDir + "/devstart", text)) parseFile(text, o);
	if (const char* v = SDL_getenv("OPENSHOP_START"); v && *v) o.screen = v;
	if (const char* v = SDL_getenv("OPENSHOP_MOCK"); v && *v) o.mock = truthy(v);
	if (const char* v = SDL_getenv("OPENSHOP_OFFLINE"); v && *v) o.offline = truthy(v);
	if (const char* v = SDL_getenv("OPENSHOP_REFRESH"); v && *v) o.refresh = truthy(v);
	return o;
}

std::string DevOptions::describe() const {
	return "screen=" + (screen.empty() ? std::string("(none)") : screen) + " mock=" + (mock ? "1" : "0") +
	       " offline=" + (offline ? "1" : "0") + " refresh=" + (refresh ? "1" : "0");
}
