// The dev self-test's script parser (net/NetSelfTest.h): pure string handling, no network. Linked with the worker
// sources only because NetSelfTest.cpp lives next to the class that uses it.
#include <cstdio>
#include <string>

#include "net/NetSelfTest.h"

using namespace net;
using Kind = NetTestItem::Kind;

static int failures = 0;
#define CHECK(cond) \
	do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void testBasics() {
	CHECK(parseNetTestScript("").empty());
	CHECK(parseNetTestScript(",,,").empty());

	auto v = parseNetTestScript("http://h/a,,bg:http://h/b,waitbg,pause@250,cancel@100:http://h:8080/c,https://h/d,");
	CHECK(v.size() == 6);
	if (v.size() != 6) return;
	CHECK(v[0].kind == Kind::Get && v[0].url == "http://h/a");
	CHECK(v[1].kind == Kind::Bg && v[1].url == "http://h/b");
	CHECK(v[2].kind == Kind::WaitBg);
	CHECK(v[3].kind == Kind::Pause && v[3].ms == 250);
	CHECK(v[4].kind == Kind::Cancel && v[4].ms == 100 && v[4].url == "http://h:8080/c");   // the URL's own ':' is kept
	CHECK(v[5].kind == Kind::Get && v[5].url == "https://h/d");   // issued, so the TlsRequired path can be checked
}

static void testMalformed() {
	// Each of these is logged and skipped, never turned into a request. The valid item after it still parses.
	for (const char* bad : {"cancel@", "cancel@:http://h/x", "cancel@100", "cancel@100:", "cancel@abc:http://h/x",
	                        "cancel@1x:http://h/x", "cancel@100:notaurl", "cancel@-:http://h/x", "pause@", "pause@abc",
	                        "pause@-", "pause@1.5", "bg:", "bg:notaurl", "bg:ftp://h/x", "garbage", "waitbg2", "WAITBG",
	                        "ftp://h/x", "h/x", "http:/h", "pause250"}) {
		auto v = parseNetTestScript(std::string(bad) + ",http://h/ok");
		CHECK(v.size() == 1);
		CHECK(!v.empty() && v.back().kind == Kind::Get && v.back().url == "http://h/ok");
	}
}

static void testNumbers() {
	auto v = parseNetTestScript("pause@-5,cancel@-100:http://h/x,pause@0,pause@99999999999999");
	CHECK(v.size() == 4);
	if (v.size() != 4) return;
	CHECK(v[0].kind == Kind::Pause && v[0].ms == 0);    // negative clamps to 0
	CHECK(v[1].kind == Kind::Cancel && v[1].ms == 0);
	CHECK(v[2].ms == 0);
	CHECK(v[3].ms == 1000000000);                       // saturates instead of overflowing
}

int main() {
	testBasics();
	testMalformed();
	testNumbers();
	if (failures) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("nettest_script_test: ok\n");
	return 0;
}
