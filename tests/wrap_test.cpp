// textwrap::Wrapper (gfx/Wrap.h) with a fake font: ASCII is 10 px wide, any other character 25 px. No SDL.
#include <cstdio>
#include <string>
#include <vector>

#include "gfx/Wrap.h"

static int failures = 0;
#define CHECK(c) \
	do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

using namespace textwrap;

static int g_calls = 0;
static int fakeWidth(std::string_view s) {
	g_calls++;
	int w = 0;
	for (size_t i = 0; i < s.size(); i = charEnd(s, i)) w += (unsigned char)s[i] < 0x80 ? 10 : 25;
	return w;
}

static std::vector<std::string> wrapAll(std::string_view text, int maxW, size_t maxLines = 0, bool* truncated = nullptr) {
	Wrapper w(fakeWidth, text, maxW, maxLines);
	w.step();
	CHECK(w.done());
	if (truncated) *truncated = w.truncated();
	return w.lines();
}

static bool validUtf8(const std::string& s) {
	for (size_t i = 0; i < s.size();) {
		unsigned char c = (unsigned char)s[i];
		size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
		if (!n || i + n > s.size()) return false;
		for (size_t k = 1; k < n; k++) if (((unsigned char)s[i + k] & 0xC0) != 0x80) return false;
		i += n;
	}
	return true;
}

static void testBasic() {
	auto l = wrapAll("one two three four", 100);   // 10 characters per line
	CHECK((l == std::vector<std::string>{"one two", "three four"}));
	CHECK((wrapAll("", 90) == std::vector<std::string>{""}));   // one blank paragraph
	// Paragraphs, blank lines, CR dropped.
	l = wrapAll("a b\r\n\r\nc", 90);
	CHECK((l == std::vector<std::string>{"a b", "", "c"}));
	l = wrapAll("a\n", 90);
	CHECK((l == std::vector<std::string>{"a", ""}));
	// A word that exactly fits, and one that is one pixel over.
	CHECK((wrapAll("abcdefghi", 90) == std::vector<std::string>{"abcdefghi"}));
	CHECK((wrapAll("abcdefghi", 89) == std::vector<std::string>{"abcdefgh", "i"}));
}

static void testHardBreak() {
	auto l = wrapAll("abcdefghijklmnopqrstuvwxyz", 100);
	CHECK((l == std::vector<std::string>{"abcdefghij", "klmnopqrst", "uvwxyz"}));
	// After a short word on the line.
	l = wrapAll("ab abcdefghijklmnopqrstuv", 100);
	CHECK((l == std::vector<std::string>{"ab", "abcdefghij", "klmnopqrst", "uv"}));
	// Multibyte glyphs break between characters, never inside one.
	l = wrapAll("ééééééééé", 100);   // 25 px each: four per line
	CHECK(l.size() == 3 && l[0] == "éééé" && l[1] == "éééé" && l[2] == "é");
	for (auto& s : l) CHECK(validUtf8(s));
}

// A glyph wider than the whole line used to make the hard-break loop spin; now each line takes one character.
static void testGlyphWiderThanLine() {
	auto l = wrapAll("ééé", 10);
	CHECK((l == std::vector<std::string>{"é", "é", "é"}));
	l = wrapAll("x 日本語 y", 12);
	CHECK(l.size() >= 5);
	for (auto& s : l) CHECK(validUtf8(s) && !s.empty());
	// Damaged text (a lone continuation byte) still terminates.
	l = wrapAll(std::string("\x80\x80\x80 a", 5), 5);
	CHECK(!l.empty());
	// maxW 0 and negative: still terminates, one character per line.
	CHECK(wrapAll("abc", 0).size() == 3);
	CHECK(wrapAll("abc", -5).size() == 3);
}

static void testMaxLines() {
	bool t = false;
	auto l = wrapAll("one two three four five six", 100, 2, &t);
	CHECK((l == std::vector<std::string>{"one two", "three four"}) && t);
	// Exactly two lines of text: nothing is cut, no flag.
	l = wrapAll("one two three four", 100, 2, &t);
	CHECK(l.size() == 2 && !t);
	// Only whitespace left over: not truncated.
	l = wrapAll("one two three four   \n \n", 100, 2, &t);
	CHECK(l.size() == 2 && !t);
	// Text left after a hard break, after a paragraph end, and after a blank line.
	l = wrapAll("abcdefghijklmnopqrstuvwxyz", 100, 1, &t);
	CHECK(l.size() == 1 && t);
	l = wrapAll("a\nb", 90, 1, &t);
	CHECK(l.size() == 1 && t);
	l = wrapAll("\nb", 90, 1, &t);
	CHECK(l.size() == 1 && l[0].empty() && t);
	l = wrapAll("a", 90, 1, &t);
	CHECK(l.size() == 1 && !t);
}

// Every word is measured once (plus the characters of a hard-broken word): linear, not quadratic.
static void testLinearMeasuring() {
	std::string text;
	const int words = 5000;
	for (int i = 0; i < words; i++) text += (i % 17 == 0 ? "wordy" : "ab") + std::string(i % 7 == 0 ? "\n" : " ");
	g_calls = 0;
	auto l = wrapAll(text, 400);
	CHECK(!l.empty());
	CHECK(g_calls <= words + 10);
}

// Stepping in slices gives exactly the lines of a single run.
static void testIncremental() {
	std::string text;
	for (int i = 0; i < 3000; i++) text += (i % 5 == 0 ? "lorem" : i % 11 == 0 ? "ipsumdolorsitamet" : "x") + std::string(i % 13 == 0 ? "\n" : " ");
	auto whole = wrapAll(text, 120);
	Wrapper w(fakeWidth, text, 120);
	int slices = 0;
	while (!w.step([] { return true; })) {   // yields at the first check, which is every 32 words
		if (++slices > 100000) { CHECK(false); break; }
	}
	CHECK(slices > 10);
	CHECK(w.lines() == whole);
	// A yield that never fires equals one run too.
	Wrapper w2(fakeWidth, text, 120);
	CHECK(w2.step([] { return false; }) && w2.lines() == whole);
	// Lines grow monotonically while stepping.
	Wrapper w3(fakeWidth, text, 120);
	size_t prev = 0;
	while (!w3.step([] { return true; })) { CHECK(w3.lines().size() >= prev); prev = w3.lines().size(); }
}

int main() {
	testBasic();
	testHardBreak();
	testGlyphWiderThanLine();
	testMaxLines();
	testLinearMeasuring();
	testIncremental();
	if (failures) { std::printf("wrap_test: %d failure(s)\n", failures); return 1; }
	std::puts("wrap_test: all passed");
	return 0;
}
