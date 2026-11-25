#pragma once

// __FILE_NAME__ (basename only) needs GCC 12+ or a recent Clang; fall back to the full __FILE__.
#ifdef __FILE_NAME__
#define OPENSHOP_LOG_FILE __FILE_NAME__
#else
#define OPENSHOP_LOG_FILE __FILE__
#endif

// Debug logging that bypasses the on-screen UI.
//   Wii:  SYS_Report -> OSReport UART -> Dolphin's OSREPORT log (streamed by ./dev run)
//   PC:   stderr
#ifdef __wii__
#include <ogc/system.h>
#define OPENSHOP_LOG_RAW(fmt, ...) SYS_Report("[%s:%d] " fmt "\n", OPENSHOP_LOG_FILE, __LINE__, ##__VA_ARGS__)
#else
#include <cstdio>
#define OPENSHOP_LOG_RAW(fmt, ...) std::fprintf(stderr, "[%s:%d] " fmt "\n", OPENSHOP_LOG_FILE, __LINE__, ##__VA_ARGS__)
#endif

// Debug builds (DEBUGTOOLS=1) also keep every line for the on-screen log and debuglog.txt (core/DebugLog.h).
#ifdef OPENSHOP_DEBUGTOOLS
#include "core/DebugLog.h"
#define LOG(fmt, ...)                                                                        \
	do {                                                                                     \
		OPENSHOP_LOG_RAW(fmt, ##__VA_ARGS__);                                                \
		::debuglog::push("[%s:%d] " fmt, OPENSHOP_LOG_FILE, __LINE__, ##__VA_ARGS__);        \
	} while (0)
#else
#define LOG(fmt, ...) OPENSHOP_LOG_RAW(fmt, ##__VA_ARGS__)
#endif
