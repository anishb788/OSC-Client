#pragma once
// Dev-only network self-test (Increment 7). OFF unless `nettest` / `nettestbg` is set in devstart (Wii) or
// OPENSHOP_NETTEST / OPENSHOP_NETTESTBG (PC); `./dev run --nettest <script>` writes them. It submits real HttpGetJobs
// through the Worker, one after another, and logs the outcome of each, while the normal UI keeps running. It never
// touches the UI or the catalog. Everything it prints starts with "[nettest]".
//
// Script: items separated by commas (no spaces), run in order. An item that waits blocks the next one.
//   <url>             GET it (Catalog class), wait for the result
//   bg:<url>          start a Background GET and carry on (checks interleaving; its progress is logged every second)
//   cancel@<ms>:<url> GET it, cancel it <ms> later, wait for the (Cancelled) result and log how long that took
//   pause@<ms>        do nothing for <ms>
//   waitbg            wait until every bg: job has finished
// `nettestbg=<url>` is shorthand for putting "bg:<url>" first. With `nettestquit=1` the app quits when the script
// ends, even with bg jobs still running, which exercises Worker::stop() mid-download.
//
// Per request it logs status, error, body bytes, duration, a CRC-32 and the first bytes of the body. For the
// tools/http_test_server.py /len/N and /slow/N/R bodies it also checks every byte against the server's pattern.
// It also reports how long the UI frames were while the request ran (max frame time), which is the "UI stays
// responsive during net_init / the fetch" check.
//
// Compiled into release builds on purpose: it runs only when devstart / the environment asks for it, the same trust
// level as the other dev start options.
#include <memory>
#include <string>
#include <vector>

#include "core/Log.h"
#include "net/Worker.h"

namespace net {

// Copies every "[nettest]" line to a text file as well (the OSReport log is invisible on a real Wii). Main thread
// only, like everything the self-test logs. Pass an empty path to close the file.
void nettestLogTo(const std::string& path);
void nettestLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
#define NTLOG(fmt, ...)                       \
	do {                                      \
		LOG(fmt, ##__VA_ARGS__);              \
		::net::nettestLog(fmt, ##__VA_ARGS__); \
	} while (0)

// One parsed script item (see the syntax above).
struct NetTestItem {
	enum class Kind { Get, Bg, Cancel, Pause, WaitBg } kind = Kind::Get;
	std::string url;
	int ms = 0;
};

// Pure, platform-neutral script parser. Empty tokens are ignored. Negative times clamp to 0; a malformed cancel@ /
// pause@ token (no ':', no or non-numeric time, empty URL), an empty bg: URL, and any other token that does not start
// with http:// or https:// are logged and skipped, never issued as a request. (https:// is accepted so the
// TlsRequired path can be checked; the HTTP client rejects it.)
std::vector<NetTestItem> parseNetTestScript(const std::string& script);

class NetSelfTest {
public:
	NetSelfTest(Worker& worker, const std::string& script, bool quitWhenDone);
	// Called once per frame from the main thread, after drainResults(). Returns true when the app should quit.
	bool tick(float dt);
	bool active() const { return !finished_; }

private:
	using Item = NetTestItem;
	struct Stats;

	void startNext();
	void startJob(const Item& it, bool background);
	void logProgress();

	Worker& worker_;
	bool quit_;
	std::vector<Item> items_;
	size_t next_ = 0;
	bool finished_ = false;
	bool begun_ = false;

	std::shared_ptr<Stats> current_;   // the item the script is waiting on (null = not waiting on a job)
	JobId currentId_ = 0;
	int cancelAtMs_ = -1;
	std::vector<std::pair<JobId, std::shared_ptr<Stats>>> bg_;
	uint32_t pauseUntil_ = 0;
	bool pausing_ = false;
	bool waitingBg_ = false;

	uint32_t nowMs_ = 0;       // sum of dt, ms (frame clock; avoids SDL in the header)
	uint32_t itemStartMs_ = 0;
	uint32_t lastProgressMs_ = 0;
	float maxFrameMs_ = 0;
	int frames_ = 0;
	int index_ = 0;
};

}  // namespace net
