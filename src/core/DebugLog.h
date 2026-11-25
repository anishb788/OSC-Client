#pragma once
// Debug-build log capture (OPENSHOP_DEBUGTOOLS, `./dev build DEBUGTOOLS=1`). Every LOG line from any thread is kept
// in a ring buffer, so the on-screen network test can show it on a real Wii (whose OSReport output nobody sees) and
// the main thread can copy it to a file on the SD card. The file is written only from the main thread (pump()), so
// the worker thread never touches the SD card for logging.
#ifdef OPENSHOP_DEBUGTOOLS
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace debuglog {
// Call once on the main thread before any other thread starts (creates the mutex).
void init();
// Any thread. Formats one line into the ring buffer (the oldest lines drop out past the cap).
void push(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// Main thread. Starts copying lines to `path` (truncates it; lines already buffered are written first).
void openFile(const std::string& path);
// Main thread, once per frame: appends lines pushed since the last call to the file and flushes it.
void pump();
// Main thread: pumps, then closes the file.
void closeFile();
// Main thread. Also copies every line from now on into `path` (a per-test log), until endSection(). Lines pushed
// before this call never go to the new file. Returns false if the file couldn't be created.
bool beginSection(const std::string& path);
// Main thread: pumps, then closes the per-test log (no-op when none is open).
void endSection();
bool sectionOpen();
// The newest `max` lines, oldest first.
void tail(std::vector<std::string>& out, size_t max);
// Drops the buffered lines (the file keeps them).
void clear();
} // namespace debuglog
#endif
