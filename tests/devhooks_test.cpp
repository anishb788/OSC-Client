// SPDX-License-Identifier: GPL-3.0-or-later
// Built by the devhooks-check target in Makefile.pc with -DOPENSHOP_DEVHOOKS=1 (the other tests build without it).
#include <cstdio>

#include "catalog/OscApi.h"

#if !(defined(OPENSHOP_DEVHOOKS) && OPENSHOP_DEVHOOKS)
#error "devhooks_test must be built with -DOPENSHOP_DEVHOOKS=1"
#endif

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } \
	} while (0)

int main() {
	const char* dev = "http://127.0.0.1:8080/api/contents/u/u.zip";
	CHECK(!oscapi::isAllowedDownloadUrl(dev));
	oscapi::allowDevDownloadBase("");
	CHECK(!oscapi::isAllowedDownloadUrl(dev)); // an empty base adds nothing
	CHECK(!oscapi::isAllowedDownloadUrl("/u/u.zip") && !oscapi::isAllowedDownloadUrl("//u/u.zip"));
	oscapi::allowDevDownloadBase("http://127.0.0.1:8080/api/contents");
	CHECK(oscapi::isAllowedDownloadUrl(dev));
	CHECK(!oscapi::isAllowedDownloadUrl("http://127.0.0.1:8080/api/contents/u/u.exe"));
	CHECK(!oscapi::isAllowedDownloadUrl("http://127.0.0.1:8080/api/contents/u/v/u.zip"));
	CHECK(!oscapi::isAllowedDownloadUrl("http://127.0.0.1:9090/api/contents/u/u.zip"));
	CHECK(oscapi::isAllowedDownloadUrl("http://hbb1.oscwii.org/api/contents/u/u.zip")); // the real host stays allowed
	oscapi::allowDevDownloadBase("");
	CHECK(!oscapi::isAllowedDownloadUrl(dev));
	if (failures) { std::fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
	std::puts("devhooks_test: all passed");
	return 0;
}
