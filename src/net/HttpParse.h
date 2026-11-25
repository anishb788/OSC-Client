#pragma once
// The wire-format half of the HTTP client: URL parsing, request building and an incremental response parser. No
// sockets, no SDL. The parser keeps all of its state between feed() calls and never assumes the rest of the response
// arrives soon, because a transfer can be paused between slices by the worker (Increment 5a).
#include <cstdint>
#include <string>

#include "net/Http.h"

namespace httpparse {

struct Url {
	std::string host;       // lower-cased
	int port = 80;
	std::string path = "/"; // always starts with '/', includes the query, never the fragment
};

// Parses http://host[:port][/path][?query]. https:// gives TlsRequired; anything else malformed gives Parse
// (other schemes, userinfo, IPv6 literals, bad ports, whitespace or control characters).
HttpError parseUrl(const std::string& text, Url& out);
std::string toString(const Url& url);

// Strict dotted-quad IPv4 literal ("1.2.3.4") to host byte order: exactly four decimal parts of 0..255, no whitespace,
// no sign, no trailing dot. A part with a leading zero ("01") is rejected rather than read as octal the way inet_aton
// would, so a literal never means two things. Anything else is a hostname for the resolver.
bool parseDottedQuad(const std::string& s, uint32_t& out);

// Resolves a Location header against the URL it came from (RFC 3986 section 5, without the rarely used cases).
// The result is an absolute URL string that still has to go through parseUrl (so an https target is reported there).
// Non-http(s) schemes give Parse.
HttpError resolveLocation(const Url& base, const std::string& location, std::string& out);

// "GET <path> HTTP/1.1" with Host, User-Agent, Accept, Accept-Encoding: identity, Connection: close, then the
// request's extra headers (any with CR/LF or an empty name are dropped so a value can't inject headers).
std::string buildRequest(const Url& url, const HttpRequest& req);

class ResponseParser {
public:
	enum class Status { NeedMore, Done, Failed };

	// Body bytes of a 2xx response go to `sink`. maxBody 0 = unlimited. onFinish() is NOT called here; the transfer
	// owns that so it can also report transport failures.
	explicit ResponseParser(HttpSink& sink, uint64_t maxBody = 0) : sink_(sink), maxBody_(maxBody) {}

	Status feed(const uint8_t* data, size_t len);
	// The peer closed the connection. Done for a read-until-close body, Failed (Io) if the response was cut short.
	Status finishEof();

	HttpError error() const { return error_; }
	const std::string& detail() const { return detail_; }
	int status() const { return status_; }
	int64_t contentLength() const { return contentLength_; }   // -1 unknown / chunked
	uint64_t bodyBytes() const { return bodyBytes_; }          // bytes handed to the sink
	// True when Done because of a 301/302/303/307/308 that carries a Location (no body is read or delivered).
	bool isRedirect() const { return redirect_; }
	const std::string& location() const { return location_; }
	// Done with a non-2xx, non-redirect status: error() is HttpStatus and no body is delivered.

private:
	enum class State { StatusLine, Headers, Length, ChunkSize, ChunkData, ChunkEnd, Trailers, UntilClose, Done, Failed };

	Status fail(HttpError e, const char* why);
	Status finish();
	bool onLine(const std::string& line);   // false = failed (error_ set)
	bool onStatusLine(const std::string& line);
	bool onHeader(const std::string& line);
	bool onHeadersDone();
	bool onChunkSize(const std::string& line);
	bool deliver(const uint8_t* data, size_t len);

	HttpSink& sink_;
	uint64_t maxBody_;
	State state_ = State::StatusLine;
	std::string line_;
	size_t headerBytes_ = 0;
	static constexpr int kMaxInterim = 8;
	int interim_ = 0;
	uint64_t remaining_ = 0;   // bytes left in the current Content-Length body or chunk

	int status_ = 0;
	int64_t contentLength_ = -1;
	bool chunked_ = false;
	bool sawLength_ = false;
	std::string location_;
	bool redirect_ = false;
	uint64_t bodyBytes_ = 0;
	HttpError error_ = HttpError::None;
	std::string detail_;
};

} // namespace httpparse
