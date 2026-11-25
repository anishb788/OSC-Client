#include "core/DebugLog.h"

#ifdef OPENSHOP_DEBUGTOOLS
#include <SDL.h>

#include <cstdarg>
#include <cstdio>
#include <deque>

namespace debuglog {
namespace {
constexpr size_t kCap = 600;
SDL_mutex* g_mutex = nullptr;
std::deque<std::string> g_lines;
uint64_t g_pushed = 0;  // total lines ever pushed (the file catches up to this)
uint64_t g_written = 0; // lines already written to the file
std::FILE* g_file = nullptr;
std::FILE* g_section = nullptr; // the per-test log, if one is open

struct Lock {
	Lock() { if (g_mutex) SDL_LockMutex(g_mutex); }
	~Lock() { if (g_mutex) SDL_UnlockMutex(g_mutex); }
};
} // namespace

void init() {
	if (!g_mutex) g_mutex = SDL_CreateMutex();
}

void push(const char* fmt, ...) {
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	std::string s(buf);
	while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
	Lock l;
	g_lines.push_back(std::move(s));
	if (g_lines.size() > kCap) g_lines.pop_front();
	g_pushed++;
}

void openFile(const std::string& path) {
	if (g_file) std::fclose(g_file);
	g_file = std::fopen(path.c_str(), "w");
	{
		Lock l;
		// Write what is still buffered, i.e. the startup lines logged before the file was opened.
		g_written = g_pushed - g_lines.size();
	}
	pump();
}

void pump() {
	if (!g_file && !g_section) {
		Lock l;
		g_written = g_pushed;
		return;
	}
	std::vector<std::string> fresh;
	{
		Lock l;
		uint64_t oldest = g_pushed - g_lines.size();
		if (g_written < oldest) g_written = oldest; // the ring overflowed between pumps: those lines are lost
		for (uint64_t i = g_written; i < g_pushed; i++) fresh.push_back(g_lines[(size_t)(i - oldest)]);
		g_written = g_pushed;
	}
	if (fresh.empty()) return;
	for (std::FILE* f : {g_file, g_section}) {
		if (!f) continue;
		for (auto& s : fresh) {
			std::fputs(s.c_str(), f);
			std::fputc('\n', f);
		}
		std::fflush(f); // a crash or a power-off still leaves everything up to the last frame on the card
	}
}

bool beginSection(const std::string& path) {
	endSection(); // also pumps, so earlier lines stay out of the new file
	g_section = std::fopen(path.c_str(), "w");
	return g_section != nullptr;
}

void endSection() {
	pump();
	if (g_section) std::fclose(g_section);
	g_section = nullptr;
}

bool sectionOpen() { return g_section != nullptr; }

void closeFile() {
	endSection();
	if (g_file) std::fclose(g_file);
	g_file = nullptr;
}

void tail(std::vector<std::string>& out, size_t max) {
	Lock l;
	size_t n = std::min(max, g_lines.size());
	out.assign(g_lines.end() - (std::ptrdiff_t)n, g_lines.end());
}

void clear() {
	Lock l;
	g_written = g_pushed; // nothing pending is lost: pump() runs every frame, before anyone can press Clear
	g_lines.clear();
}
} // namespace debuglog
#endif
