#pragma once

// Debug logging that bypasses the on-screen UI.
//   Wii:  SYS_Report -> OSReport UART -> Dolphin's OSREPORT log (streamed by ./dev run)
//   PC:   stderr
#ifdef __wii__
#include <ogc/system.h>
#define LOG(fmt, ...) SYS_Report("[%s:%d] " fmt "\n", __FILE_NAME__, __LINE__, ##__VA_ARGS__)
#else
#include <cstdio>
#define LOG(fmt, ...) std::fprintf(stderr, "[%s:%d] " fmt "\n", __FILE_NAME__, __LINE__, ##__VA_ARGS__)
#endif
