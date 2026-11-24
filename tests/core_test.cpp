// Host tests for small pure pieces outside the catalog: dev option parsing and the catalog install guard.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "catalog/InstallGuard.h"
#include "core/DevOptions.h"

static int failures = 0;
#define CHECK(cond) \
	do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static DevOptions parse(const std::string& text) {
	DevOptions o;
	DevOptions::parseFile(text, o);
	return o;
}

static void testDevOptions() {
	DevOptions o = parse("screen=detail\nmock=1\nfail=1\n");
	CHECK(o.screen == "detail" && o.mock && o.fail && !o.offline && !o.refresh);

	CHECK(parse("fail=0\n").fail == false);
	CHECK(parse("fail=false\n").fail == false);
	CHECK(parse("fail=off\n").fail == false);
	CHECK(parse("fail=yes\n").fail == true);
	CHECK(parse("fail=\n").fail == false);

	// CRLF, spaces, comments, unknown keys, bare screen name (old format), last value wins.
	o = parse("# comment\r\n  screen = list \r\nunknown=1\r\noffline=1\r\nrefresh=1\r\n");
	CHECK(o.screen == "list" && o.offline && o.refresh && !o.fail);
	CHECK(parse("search").screen == "search");
	CHECK(parse("screen=home\nscreen=dialog\n").screen == "dialog");
	CHECK(parse("").screen.empty());

	// topbar=all previews the top-bar buttons; anything else leaves them to their features.
	CHECK(!parse("").topbarAll);
	CHECK(parse("topbar=all\n").topbarAll);
	CHECK(parse(" topbar = all \r\n").topbarAll);
	CHECK(!parse("topbar=none\n").topbarAll);
	CHECK(!parse("topbar=1\n").topbarAll);
	CHECK(!parse("topbar=\n").topbarAll);
	CHECK(!parse("topbar=all\ntopbar=\n").topbarAll);
	CHECK(parse("topbar=all\nscreen=list\n").screen == "list");

	o = parse("screen=search\nfail=1");
	CHECK(o.describe() == "screen=search mock=0 offline=0 refresh=0 fail=1 topbar=default");
}

static void testInstallGuard() {
	InstallState ok;
	CHECK(catalogInstallRefusal(ok) == nullptr);

	InstallState twice;
	twice.alreadyInstalled = true;
	CHECK(catalogInstallRefusal(twice) != nullptr); // the second install is refused

	InstallState otherScreen;
	otherScreen.onlyLoadingScreen = false;
	CHECK(catalogInstallRefusal(otherScreen) != nullptr);

	InstallState pending;
	pending.screenChangePending = true;
	CHECK(catalogInstallRefusal(pending) != nullptr);
}

int main() {
	testDevOptions();
	testInstallGuard();
	if (failures) { std::printf("core_test: %d failure(s)\n", failures); return 1; }
	std::printf("core_test: all passed\n");
	return 0;
}
