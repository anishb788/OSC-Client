// Debug builds only (DEBUGTOOLS=1): an on-screen network test bench for real hardware. Each button runs a canned
// NetSelfTest script against the PC test server (tools/http_test_server.py) or the real OSC server, and the panel
// below shows the live log (every LOG line, including the Wii socket backend's init / DNS lines). The same lines go
// to <app dir>/debuglog.txt (the whole session) and, per test run, to its own numbered file in <app dir>/logs/
// (e.g. logs/007-cancel-test.txt). Numbers continue across launches (logs/next.txt), so nothing is overwritten. The server address comes from <app dir>/nettest-host.txt (one line, host:port).
#ifdef OPENSHOP_DEBUGTOOLS
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include "App.h"
#include "core/DebugLog.h"
#include "core/Log.h"
#include "gfx/Theme.h"
#include "net/NetSelfTest.h"
#include "platform/Platform.h"
#include "ui/Widgets.h"

namespace {

constexpr const char* kDefaultHost = "192.168.2.107:8099";

std::string loadHost() {
	std::string text;
	if (!platform::readFile(platform::dataDir() + "/nettest-host.txt", text)) return kDefaultHost;
	std::string host;
	for (char c : text) {
		if (c == '\n' || c == '\r') break;
		if (c != ' ' && c != '\t') host += c;
	}
	if (host.rfind("http://", 0) == 0) host = host.substr(7);
	while (!host.empty() && host.back() == '/') host.pop_back();
	return host.empty() ? kDefaultHost : host;
}

struct Test {
	const char* label;
	const char* slug;  // file-name part
	const char* what; // logged when the test starts
	enum class Kind { Script, ExitNow } kind = Kind::Script;
	bool quitWhenDone = false;
};

constexpr Test kTests[] = {
	{"Full test", "full-test", "300 KB, redirect, 404, OSC server, refused, cancel, 5 MB in the background"},
	{"OSC server", "osc-server", "plain-http GET of the real Open Shop Channel API"},
	{"5 MB download", "5mb-download", "one 5 MB GET from the PC server, byte-checked"},
	{"Cancel test", "cancel-test", "slow download cancelled after 0.7 s"},
	{"Quit mid-DL", "quit-mid-download", "slow background download, then the app quits by itself after 3 s", Test::Kind::Script, true},
	{"Exit now", "exit-now", "quit to the Homebrew Channel immediately, whatever the network is doing", Test::Kind::ExitNow},
};
constexpr int kTestCount = (int)(sizeof kTests / sizeof kTests[0]);

std::string scriptFor(int i, const std::string& host) {
	const std::string s = "http://" + host;
	const std::string osc = "http://hbb1.oscwii.org/api/v4/information";
	switch (i) {
	case 0:
		return "bg:" + s + "/len/5000000," + s + "/len/300000," + s + "/redir/len/5000," + s + "/nothing," + osc +
		       ",http://" + host.substr(0, host.find(':')) + ":1/x,cancel@700:" + s + "/slow/4000000/200000,waitbg";
	case 1: return osc;
	case 2: return s + "/len/5000000";
	case 3: return "cancel@700:" + s + "/slow/4000000/200000";
	case 4: return "bg:" + s + "/slow/4000000/200000,pause@3000";
	default: return {};
	}
}

// Per-test logs live in <app dir>/logs/NNN-<slug>.txt; logs/next.txt holds the next number.
std::string logDir() { return platform::dataDir() + "/logs"; }

int takeLogNumber() {
	mkdir(logDir().c_str(), 0777); // fails harmlessly if it already exists
	std::string text;
	int n = 1;
	if (platform::readFile(logDir() + "/next.txt", text)) n = std::max(1, std::atoi(text.c_str()));
	platform::writeFile(logDir() + "/next.txt", std::to_string(n + 1) + "\n");
	return n;
}

class NetTestScreen : public Screen {
public:
	NetTestScreen() : host_(loadHost()) {
		LOG("[debug] network test screen; server %s (from nettest-host.txt, default %s)", host_.c_str(), kDefaultHost);
	}

	~NetTestScreen() override {
		// Leaving for Home mid-test (or at exit, after App::shutdown has logged): close this test's log.
		if (running_ >= 0) LOG("[debug] ---- %s: left the test screen ----", kTests[running_].label);
		debuglog::endSection();
	}

	void draw(App& app, Ui& ui) override {
		const float W = (float)ui.g.width(), H = (float)ui.g.height(), m = theme::margin;
		app.topBar(ui, "Network test", "DEBUG BUILD · server " + host_);

		// Advance the running test; it asks to quit at the end of "Quit mid-DL".
		if (test_) {
			if (test_->tick(ui.dt)) {
				LOG("[debug] test asked to quit: exiting to the Homebrew Channel now");
				app.quit();
			}
			if (!test_->active() && !quitting_) {
				LOG("[debug] ---- %s finished ----", kTests[running_].label);
				debuglog::endSection();
				test_.reset();
				running_ = -1;
			}
		}

		// Buttons: two rows of three.
		const float top = theme::topBarH + 12, bh = 38, gap = 10;
		const float bw = (W - 2 * m - 2 * gap) / 3;
		for (int i = 0; i < kTestCount; i++) {
			Rect r{m + (i % 3) * (bw + gap), top + (i / 3) * (bh + gap), bw, bh};
			Id id = makeId("nettest:btn", (uint32_t)i);
			const bool busy = running_ >= 0;
			Pill style = i == running_ ? Pill::Selected : kTests[i].kind == Test::Kind::ExitNow ? Pill::Destructive : Pill::Normal;
			if (i == 0) ui.setDefaultFocus(id);
			if (pillButton(ui, id, r, kTests[i].label, {.style = style, .fontSize = 15})) start(app, i, busy);
		}

		// Status line + Stop / Clear.
		const float sy = top + 2 * (bh + gap) + 2;
		std::string status = running_ >= 0 ? std::string("Running: ") + kTests[running_].label : "Idle";
		status += app.worker().running() ? "   ·   net worker: running" : "   ·   net worker: NOT RUNNING";
		if (!logName_.empty()) status += "   ·   " + logName_;
		ui.t.drawIn(Font::Medium, 14, status, {m, sy, W - 2 * m - 220, 28}, running_ >= 0 ? theme::accentDark : theme::textMuted);
		if (pillButton(ui, makeId("nettest:stop"), {W - m - 210, sy - 2, 100, 30}, "Stop", {.fontSize = 14}) && test_) {
			LOG("[debug] Stop pressed: cancelling every job");
			app.worker().cancelAll();
			LOG("[debug] ---- %s stopped ----", kTests[running_].label);
			debuglog::endSection();
			test_.reset();
			running_ = -1;
		}
		if (pillButton(ui, makeId("nettest:clear"), {W - m - 100, sy - 2, 100, 30}, "Clear log", {.fontSize = 14})) {
			debuglog::clear();
			scroll_ = 0;
		}

		drawLog(ui, {m, sy + 36, W - 2 * m, H - theme::bottomBarH - 8 - (sy + 36)});

		drawBottomBar(ui);
		if (backButton(ui, "Home")) app.pop();
		ui.t.drawIn(Font::Medium, 13, "+/- scroll the log   ·   HOME: exit menu   ·   logs/ on the SD card",
		            {W - m - 400, H - theme::bottomBarH + 12, 400, 40}, theme::textMuted, Align::Right);
	}

private:
	void start(App& app, int i, bool busy) {
		if (busy && kTests[i].kind != Test::Kind::ExitNow) { // Exit now is allowed mid-test: that's one of the cases
			LOG("[debug] %s ignored: %s is still running (press Stop first)", kTests[i].label, kTests[running_].label);
			return;
		}
		scroll_ = 0;
		if (kTests[i].kind == Test::Kind::ExitNow && busy) {
			// Exit during a test: the exit belongs to that test, so its log stays open and records the shutdown.
			LOG("[debug] ==== Exit now pressed while %s is running: exiting ====", kTests[running_].label);
			app.quit();
			return;
		}
		openLog(i);
		LOG("[debug] ==== %s: %s ====", kTests[i].label, kTests[i].what);
		if (kTests[i].kind == Test::Kind::ExitNow) {
			app.quit(); // the log stays open: App::shutdown's lines (worker stop, net shutdown) end up in it
			return;
		}
		running_ = i;
		quitting_ = kTests[i].quitWhenDone;
		test_ = std::make_unique<net::NetSelfTest>(app.worker(), scriptFor(i, host_), kTests[i].quitWhenDone);
	}

	void openLog(int i) {
		char name[64];
		std::snprintf(name, sizeof name, "%03d-%s.txt", takeLogNumber(), kTests[i].slug);
		const std::string path = logDir() + "/" + name;
		if (!debuglog::beginSection(path)) {
			LOG("[debug] ERROR: could not create %s (only debuglog.txt will have this test)", path.c_str());
			logName_.clear();
			return;
		}
		logName_ = std::string("logs/") + name;
		char when[32] = "unknown";
		std::time_t t = std::time(nullptr);
		if (std::tm* tm = std::localtime(&t)) std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", tm);
		LOG("[debug] log %s · test \"%s\" · server %s · console clock %s", logName_.c_str(), kTests[i].label,
		    host_.c_str(), when);
	}

	void drawLog(Ui& ui, Rect box) {
		ui.g.fillRoundRect(box.inset(-1), 11, theme::border.withAlpha(0.6f));
		ui.g.fillRoundRect(box, 10, theme::card);
		const int fs = 12;
		const float lh = (float)ui.t.lineHeight(Font::Medium, fs);
		const int visible = std::max(1, (int)((box.h - 12) / lh));

		// Wrap only the newest lines (cheap enough per frame), then show a window of them.
		std::vector<std::string> raw;
		debuglog::tail(raw, 120);
		std::vector<std::string> lines;
		for (auto& r : raw) {
			auto w = ui.t.wrap(Font::Medium, fs, r, (int)box.w - 20, 4);
			if (w.empty()) w.push_back("");
			for (size_t k = 0; k < w.size(); k++) lines.push_back(k ? "    " + w[k] : w[k]);
		}
		const int maxScroll = std::max(0, (int)lines.size() - visible);
		if (ui.pressed(BTN_MINUS)) scroll_ += visible - 1; // older
		if (ui.pressed(BTN_PLUS)) scroll_ -= visible - 1;  // newer
		scroll_ = std::clamp(scroll_ - ui.wheel() * 3, 0, maxScroll);
		const int first = std::max(0, (int)lines.size() - visible - scroll_);

		ui.g.pushClip(box.inset(3));
		for (int i = 0; i < visible && first + i < (int)lines.size(); i++) {
			const std::string& l = lines[first + i];
			Color c = theme::text;
			if (l.find("FAIL") != std::string::npos || l.find("ERROR") != std::string::npos || l.find("BAD") != std::string::npos)
				c = Color{200, 40, 30};
			else if (l.find("[debug]") != std::string::npos) c = theme::accentDark;
			ui.t.drawIn(Font::Medium, fs, l, {box.x + 10, box.y + 6 + i * lh, box.w - 20, lh}, c);
		}
		ui.g.popClip();
		if (scroll_ > 0)
			ui.t.drawIn(Font::Bold, 12, "(scrolled back: press + for newer)", {box.x, box.bottom() - 18, box.w - 12, 16},
			            theme::accentDark, Align::Right);
	}

	std::string host_;
	std::unique_ptr<net::NetSelfTest> test_;
	int running_ = -1;
	bool quitting_ = false;
	int scroll_ = 0;
	std::string logName_; // the current / last per-test log, shown in the status line
};

} // namespace

std::unique_ptr<Screen> makeNetTestScreen() { return std::make_unique<NetTestScreen>(); }
#endif
