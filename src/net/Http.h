#pragma once
// Platform-neutral HTTP types shared by M2 (catalog, icons) and M3 (archive downloads). No sockets, no SDL, no
// libogc: the transports live behind Transport.h (Increment 5a) and the wire format in HttpParse.h.
//
// M3 HAND-OFF (archive downloads). Everything M3 needs from M2 is already here; read these in this order:
//  - Streaming: subclass HttpSink (below). onData() gets the body in slices, so a download goes straight to the SD card
//    through a sink (MemorySink and FileSink below; FileSink writes a .tmp and renames it) and never sits in RAM.
//    Set HttpRequest::maxBody and totalTimeoutMs for catalog-style fetches; a long archive may leave totalTimeoutMs 0.
//  - Progress and cancel: ProgressFn and CancelFlag below; Worker::progress(id) and Worker::cancel(id) (Worker.h).
//  - Threads: ONE cooperative network worker (Worker.h RULES, HttpTransfer.h DECISION). M3 submits its downloads to it
//    as Background jobs. No second network thread and no netMutex.
//  - Catalog pointers: background work never holds `const AppEntry*` or the Catalog; copy slug, URLs, sizes and the MD5
//    when the job is enqueued and look the app up again with catalog.find(slug) when the job reports back.
//  - Catalog replacement: App::installCatalog() accepts a new catalog only while the LoadingScreen recorded for the
//    load is the only screen (startup and Refresh); App::catalogGeneration() changes on every accepted install (App.h).
//  - Top bar: replace App::openDownloads() and App::openMyApps() (M4 replaces openSettings()); the capsules, badge and
//    progress strip already exist behind the `topbar=all` dev option.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/FileUtil.h"

enum class HttpError {
	None,
	NoNetwork,    // the console has no usable network connection
	Dns,          // host name did not resolve
	Connect,      // TCP connect refused / failed
	Timeout,      // connect, idle or total timer expired
	HttpStatus,   // server answered with a non-2xx status (HttpResult::status has it)
	TlsRequired,  // https:// URL (or redirect to one); OpenShop uses plain HTTP only (project decision)
	TooLarge,     // body exceeds HttpRequest::maxBody
	Parse,        // malformed URL or malformed response
	Io,           // socket error, early close, or the sink refused data (e.g. SD card write failed)
	Cancelled,    // the cancel flag was raised
	Redirects,    // more than HttpRequest::maxRedirects redirects
};

struct HttpRequest {
	std::string url;
	std::vector<std::pair<std::string, std::string>> headers; // extra headers: name, value
	uint64_t maxBody = 0;          // 0 = unlimited
	int connectTimeoutMs = 8000;
	int idleTimeoutMs = 10000;     // no bytes received for this long
	// 0 = none. Callers must set it (catalog and icon fetches) because the idle timer alone is defeated by a server
	// that drips one byte at a time; only M3's long archive downloads may leave it 0.
	int totalTimeoutMs = 0;
	int maxRedirects = 3;
};

struct HttpResult {
	HttpError error = HttpError::None;
	int status = 0;                // last HTTP status seen, 0 if none
	std::string detail;            // developer-facing detail for the log, not for the user
	std::string finalUrl;          // URL of the response after redirects
	uint64_t bytesReceived = 0;    // body bytes handed to the sink
	bool ok() const { return error == HttpError::None; }
};

// User-facing text for a result, the single place wording lives (the "Couldn't refresh" dialog shows it).
std::string describe(const HttpResult& r);

// Receives the response body. All callbacks run on the thread that drives the transfer.
class HttpSink {
public:
	virtual ~HttpSink() = default;
	// 2xx response headers are in. contentLength is -1 when unknown. Return false to abort.
	virtual bool onStart(int status, int64_t contentLength) = 0;
	// Return false to abort the transfer; the client then reports failure().
	virtual bool onData(const uint8_t* data, size_t len) = 0;
	// Exactly once at the end, ok = the whole body was delivered.
	virtual void onFinish(bool ok) = 0;
	// Why the sink aborted (what the client reports when onStart/onData returned false).
	virtual HttpError failure() const { return HttpError::Io; }
};

// Progress: bytes received so far and total (-1 unknown). The client rate-limits calls.
using ProgressFn = std::function<void(uint64_t received, int64_t total)>;
// Cancellation: raised from another thread, checked between reads and inside poll waits. May be null.
using CancelFlag = const std::atomic<bool>*;

// Collects the body in memory. Aborts with TooLarge once more than maxBody arrives. The default of 8 MB keeps a bogus
// Content-Length or a runaway server from exhausting the Wii's ~88 MB; an explicit 0 means unlimited. The up-front
// reserve is capped at 1 MB whatever the server claims, and an allocation failure also reports TooLarge.
class MemorySink : public HttpSink {
public:
	static constexpr uint64_t kDefaultMaxBody = 8ull * 1024 * 1024;
	explicit MemorySink(uint64_t maxBody = kDefaultMaxBody) : maxBody_(maxBody) {}
	bool onStart(int status, int64_t contentLength) override;
	bool onData(const uint8_t* data, size_t len) override;
	void onFinish(bool ok) override { finished_ = true; ok_ = ok; }
	HttpError failure() const override { return failure_; }

	const std::string& body() const { return body_; }
	std::string& body() { return body_; }
	bool finished() const { return finished_; }
	bool finishedOk() const { return finished_ && ok_; }

private:
	uint64_t maxBody_;
	std::string body_;
	HttpError failure_ = HttpError::Io;
	bool finished_ = false;
	bool ok_ = false;
};

// Writes the body to "<path>.tmp" through a fixed 64 KB stdio buffer. After a successful transfer commit() renames the
// tmp into place (never removing the target first, see commit()); abort() (or the destructor, if never committed) removes the tmp, so a failed or cancelled
// download never leaves a partial file under the real name. M3 wraps this to add an MD5 check.
class FileSink : public HttpSink {
public:
	explicit FileSink(std::string path) : path_(std::move(path)), tmpPath_(path_ + ".tmp") {}
	~FileSink() override;
	FileSink(const FileSink&) = delete;
	FileSink& operator=(const FileSink&) = delete;

	bool onStart(int status, int64_t contentLength) override;
	bool onData(const uint8_t* data, size_t len) override;
	void onFinish(bool ok) override;

	// True if the target now holds the full body. A plain rename is tried first; if that fails because the target exists
	// (libfat cannot rename over a file) the old target is parked as "<path>.bak" and restored if the move fails.
	bool commit();
	void setFileOps(const FileOps& ops) { ops_ = ops; }
	void abort();    // closes and deletes the tmp file
	const std::string& tmpPath() const { return tmpPath_; }
	uint64_t bytesWritten() const { return written_; }

private:
	void closeFile();
	std::string path_, tmpPath_;
	std::FILE* file_ = nullptr;
	std::unique_ptr<char[]> buffer_;
	uint64_t written_ = 0;
	FileOps ops_;
	bool opened_ = false;
	bool finishedOk_ = false;
	bool writeFailed_ = false;
	bool committed_ = false;
};
