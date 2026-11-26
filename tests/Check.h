// SPDX-License-Identifier: GPL-3.0-or-later
// Shared check helper for the M3 host tests, in the style of core_test.cpp: CHECK prints "FAIL file:line: expr" and counts;
// checkReport() prints the summary and gives the exit code (non-zero on any failure).
#pragma once
#include <cstdio>

inline int g_checkFailures = 0;
inline int g_checkCount = 0;

#define CHECK(cond) \
	do { g_checkCount++; if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_checkFailures++; } } while (0)

// Use as the last line of main(): return checkReport("name");
inline int checkReport(const char* name) {
	if (g_checkFailures) {
		std::printf("%s: %d of %d checks FAILED\n", name, g_checkFailures, g_checkCount);
		return 1;
	}
	std::printf("%s: all %d checks passed\n", name, g_checkCount);
	return 0;
}
