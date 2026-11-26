#include "net/HttpParse.h"

#include <cctype>
#include <strings.h>

#include "core/Version.h"

namespace httpparse {

namespace {

constexpr size_t kMaxLine = 8192;           // one status/header/chunk-size line
constexpr size_t kMaxHeaderBytes = 64 * 1024;

char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

std::string lowerStr(std::string s) {
	for (char& c : s) c = lower(c);
	return s;
}

bool iequals(const std::string& a, const char* b) {
	size_t i = 0;
	for (; i < a.size() && b[i]; i++)
		if (lower(a[i]) != lower(b[i])) return false;
	return i == a.size() && !b[i];
}

// True if s starts with prefix, ignoring ASCII case.
bool istarts(const std::string& s, const char* prefix) {
	size_t i = 0;
	for (; prefix[i]; i++)
		if (i >= s.size() || lower(s[i]) != lower(prefix[i])) return false;
	return true;
}

std::string trim(const std::string& s) {
	size_t b = 0, e = s.size();
	while (b < e && (s[b] == ' ' || s[b] == '\t')) b++;
	while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) e--;
	return s.substr(b, e - b);
}

bool hasBadChars(const std::string& s) {
	for (unsigned char c : s)
		if (c <= 0x20 || c == 0x7f) return true;
	return false;
}

// Removes "." and ".." segments from an absolute path (query kept as-is by the caller).
std::string removeDotSegments(const std::string& path) {
	std::vector<std::string> out;
	size_t i = 0;
	bool trailingSlash = false;
	while (i <= path.size()) {
		size_t j = path.find('/', i);
		std::string seg = path.substr(i, j == std::string::npos ? std::string::npos : j - i);
		bool last = j == std::string::npos;
		if (seg == "..") {
			if (!out.empty()) out.pop_back();
			trailingSlash = last;
		} else if (seg == ".") {
			trailingSlash = last;
		} else if (!seg.empty()) {
			out.push_back(seg);
			trailingSlash = false;
		} else {
			trailingSlash = last && !out.empty() ? true : trailingSlash;
		}
		if (last) break;
		i = j + 1;
	}
	std::string r;
	for (auto& s : out) r += "/" + s;
	if (r.empty() || trailingSlash) r += "/";
	return r;
}

} // namespace

HttpError parseUrl(const std::string& text, Url& out) {
	if (text.empty() || hasBadChars(text)) return HttpError::Parse;
	size_t pos;
	if (istarts(text, "http://")) pos = 7;
	else if (istarts(text, "https://")) return HttpError::TlsRequired;
	else return HttpError::Parse;

	std::string rest = text.substr(pos);
	size_t hash = rest.find('#');
	if (hash != std::string::npos) rest.erase(hash);
	size_t slash = rest.find_first_of("/?");
	std::string authority = rest.substr(0, slash);
	std::string path = slash == std::string::npos ? "/" : rest.substr(slash);
	if (path[0] == '?') path = "/" + path;

	if (authority.empty() || authority.find('@') != std::string::npos || authority[0] == '[') return HttpError::Parse;
	std::string host = authority;
	int port = 80;
	size_t colon = authority.find(':');
	if (colon != std::string::npos) {
		host = authority.substr(0, colon);
		std::string p = authority.substr(colon + 1);
		if (p.empty() || p.size() > 5) return HttpError::Parse;
		int v = 0;
		for (char c : p) {
			if (c < '0' || c > '9') return HttpError::Parse;
			v = v * 10 + (c - '0');
		}
		if (v < 1 || v > 65535) return HttpError::Parse;
		port = v;
	}
	if (host.empty()) return HttpError::Parse;
	for (char c : host)
		if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '.' || c == '_')) return HttpError::Parse;

	out.host = lowerStr(host);
	out.port = port;
	out.path = path;
	return HttpError::None;
}

bool parseDottedQuad(const std::string& s, uint32_t& out) {
	uint32_t v = 0;
	size_t i = 0;
	for (int part = 0; part < 4; part++) {
		if (part > 0) {
			if (i >= s.size() || s[i] != '.') return false;
			i++;
		}
		size_t start = i;
		uint32_t n = 0;
		while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
			n = n * 10 + static_cast<uint32_t>(s[i++] - '0');
			if (i - start > 3 || n > 255) return false;
		}
		if (i == start || (i - start > 1 && s[start] == '0')) return false;
		v = (v << 8) | n;
	}
	if (i != s.size()) return false;
	out = v;
	return true;
}

std::string toString(const Url& url) {
	std::string s = "http://" + url.host;
	if (url.port != 80) s += ":" + std::to_string(url.port);
	return s + url.path;
}

HttpError resolveLocation(const Url& base, const std::string& location, std::string& out) {
	std::string loc = trim(location);
	if (loc.empty() || hasBadChars(loc)) return HttpError::Parse;

	// A scheme is letters/digits/+-. followed by ':' before any '/', '?' or '#'.
	size_t i = 0;
	while (i < loc.size() && (std::isalnum(static_cast<unsigned char>(loc[i])) || loc[i] == '+' || loc[i] == '-' || loc[i] == '.')) i++;
	if (i > 0 && i < loc.size() && loc[i] == ':') {
		if (istarts(loc, "http://") || istarts(loc, "https://")) { out = loc; return HttpError::None; }
		return HttpError::Parse;
	}

	size_t hash = loc.find('#');
	if (hash != std::string::npos) loc.erase(hash);
	if (loc.empty()) { out = toString(base); return HttpError::None; }

	std::string origin = toString(Url{base.host, base.port, ""});
	if (loc.compare(0, 2, "//") == 0) { out = "http:" + loc; return HttpError::None; }

	std::string pathAndQuery;
	if (loc[0] == '/') {
		pathAndQuery = loc;
	} else if (loc[0] == '?') {
		pathAndQuery = base.path.substr(0, base.path.find('?')) + loc;
	} else {
		std::string basePath = base.path.substr(0, base.path.find('?'));
		pathAndQuery = basePath.substr(0, basePath.rfind('/') + 1) + loc;
	}
	size_t q = pathAndQuery.find('?');
	std::string path = removeDotSegments(pathAndQuery.substr(0, q));
	out = origin + path + (q == std::string::npos ? "" : pathAndQuery.substr(q));
	return HttpError::None;
}

std::string buildRequest(const Url& url, const HttpRequest& req) {
	std::string s = "GET " + url.path + " HTTP/1.1\r\n";
	s += "Host: " + url.host + (url.port != 80 ? ":" + std::to_string(url.port) : "") + "\r\n";
	s += std::string("User-Agent: ") + userAgent() + "\r\n";
	// A caller's own Accept (the catalog asks for JSON, icons for PNG) replaces the default instead of doubling it.
	bool ownAccept = false;
	for (const auto& h : req.headers) {
		if (h.first.size() == 6 && strncasecmp(h.first.c_str(), "accept", 6) == 0) ownAccept = true;
	}
	if (!ownAccept) s += "Accept: */*\r\n";
	s += "Accept-Encoding: identity\r\n";
	s += "Connection: close\r\n";
	for (const auto& h : req.headers) {
		if (h.first.empty() || h.first.find_first_of("\r\n:") != std::string::npos) continue;
		if (h.second.find_first_of("\r\n") != std::string::npos) continue;
		s += h.first + ": " + h.second + "\r\n";
	}
	s += "\r\n";
	return s;
}

// ---- ResponseParser ----

ResponseParser::Status ResponseParser::fail(HttpError e, const char* why) {
	state_ = State::Failed;
	error_ = e;
	detail_ = why;
	return Status::Failed;
}

ResponseParser::Status ResponseParser::finish() {
	state_ = State::Done;
	return Status::Done;
}

bool ResponseParser::deliver(const uint8_t* data, size_t len) {
	if (maxBody_ && bodyBytes_ + len > maxBody_) {
		fail(HttpError::TooLarge, "body larger than maxBody");
		return false;
	}
	bodyBytes_ += len;
	if (!sink_.onData(data, len)) {
		fail(sink_.failure(), "sink refused data");
		return false;
	}
	return true;
}

bool ResponseParser::onStatusLine(const std::string& line) {
	// "HTTP/1.x SSS[ reason]"
	if (line.size() < 12 || !istarts(line, "HTTP/1.") || (line[7] != '0' && line[7] != '1') || line[8] != ' ') {
		fail(HttpError::Parse, "bad status line");
		return false;
	}
	int code = 0;
	for (int i = 9; i < 12; i++) {
		if (line[i] < '0' || line[i] > '9') { fail(HttpError::Parse, "bad status code"); return false; }
		code = code * 10 + (line[i] - '0');
	}
	if (line.size() > 12 && line[12] != ' ') { fail(HttpError::Parse, "bad status line"); return false; }
	if (code < 100) { fail(HttpError::Parse, "bad status code"); return false; }
	status_ = code;
	contentLength_ = -1;
	chunked_ = false;
	sawLength_ = false;
	location_.clear();
	state_ = State::Headers;
	return true;
}

bool ResponseParser::onHeader(const std::string& line) {
	if (line[0] == ' ' || line[0] == '\t') { fail(HttpError::Parse, "folded header"); return false; }
	size_t colon = line.find(':');
	if (colon == std::string::npos || colon == 0) { fail(HttpError::Parse, "bad header"); return false; }
	std::string name = line.substr(0, colon);
	if (name.find_first_of(" \t") != std::string::npos) { fail(HttpError::Parse, "bad header name"); return false; }
	std::string value = trim(line.substr(colon + 1));

	if (iequals(name, "Content-Length")) {
		if (value.empty() || value.size() > 18) { fail(HttpError::Parse, "bad Content-Length"); return false; }
		int64_t v = 0;
		for (char c : value) {
			if (c < '0' || c > '9') { fail(HttpError::Parse, "bad Content-Length"); return false; }
			v = v * 10 + (c - '0');
		}
		if (sawLength_ && v != contentLength_) { fail(HttpError::Parse, "conflicting Content-Length"); return false; }
		sawLength_ = true;
		contentLength_ = v;
	} else if (iequals(name, "Transfer-Encoding")) {
		std::string v = lowerStr(value);
		if (v == "chunked") chunked_ = true;
		else if (v != "identity") { fail(HttpError::Parse, "unsupported Transfer-Encoding"); return false; }
	} else if (iequals(name, "Location")) {
		location_ = value;
	}
	return true;
}

bool ResponseParser::onHeadersDone() {
	headerBytes_ = 0; // the cap is per header block; chunk-size lines and trailers count afresh
	if (status_ >= 100 && status_ < 200) {
		// Interim response (100 Continue ...): the real status line follows. Capped so a server can't keep us parsing
		// forever.
		if (++interim_ > kMaxInterim) { fail(HttpError::Parse, "too many interim responses"); return false; }
		state_ = State::StatusLine;
		return true;
	}
	bool redirectCode = status_ == 301 || status_ == 302 || status_ == 303 || status_ == 307 || status_ == 308;
	if (redirectCode && !location_.empty()) {
		redirect_ = true;
		finish();
		return true;
	}
	if (status_ < 200 || status_ >= 300) {
		error_ = HttpError::HttpStatus;
		detail_ = "HTTP " + std::to_string(status_);
		finish();
		return true;
	}
	if (chunked_) contentLength_ = -1; // Transfer-Encoding wins over Content-Length
	if (maxBody_ && contentLength_ > 0 && static_cast<uint64_t>(contentLength_) > maxBody_) {
		fail(HttpError::TooLarge, "Content-Length larger than maxBody");
		return false;
	}
	if (!sink_.onStart(status_, contentLength_)) {
		fail(sink_.failure(), "sink refused start");
		return false;
	}
	bool noBody = status_ == 204 || status_ == 205;
	if (noBody || (!chunked_ && contentLength_ == 0)) { finish(); return true; }
	if (chunked_) {
		state_ = State::ChunkSize;
	} else if (contentLength_ > 0) {
		remaining_ = static_cast<uint64_t>(contentLength_);
		state_ = State::Length;
	} else {
		state_ = State::UntilClose;
	}
	return true;
}

bool ResponseParser::onChunkSize(const std::string& line) {
	// hex-size [; extensions]
	size_t end = line.find(';');
	std::string hex = trim(line.substr(0, end));
	if (hex.empty() || hex.size() > 15) { fail(HttpError::Parse, "bad chunk size"); return false; }
	uint64_t v = 0;
	for (char c : hex) {
		int d;
		if (c >= '0' && c <= '9') d = c - '0';
		else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
		else { fail(HttpError::Parse, "bad chunk size"); return false; }
		v = v * 16 + static_cast<uint64_t>(d);
	}
	headerBytes_ = 0;
	if (v == 0) { state_ = State::Trailers; return true; }
	if (maxBody_ && bodyBytes_ + v > maxBody_) { fail(HttpError::TooLarge, "chunked body larger than maxBody"); return false; }
	remaining_ = v;
	state_ = State::ChunkData;
	return true;
}

// Called for each complete line (CR LF stripped) in the line-oriented states.
bool ResponseParser::onLine(const std::string& line) {
	switch (state_) {
	case State::StatusLine:
		if (line.empty()) return true; // tolerate stray blank lines before the status line
		return onStatusLine(line);
	case State::Headers:
		if (line.empty()) return onHeadersDone();
		return onHeader(line);
	case State::ChunkSize:
		return onChunkSize(line);
	case State::ChunkEnd:
		if (!line.empty()) { fail(HttpError::Parse, "missing CRLF after chunk"); return false; }
		state_ = State::ChunkSize;
		return true;
	case State::Trailers:
		if (line.empty()) { finish(); return true; }
		if (line[0] == ' ' || line[0] == '\t') { fail(HttpError::Parse, "folded trailer"); return false; }
		return true; // trailers are validated for shape only and otherwise ignored
	default:
		return true;
	}
}

ResponseParser::Status ResponseParser::feed(const uint8_t* data, size_t len) {
	size_t i = 0;
	while (i < len) {
		switch (state_) {
		case State::Done: return Status::Done;
		case State::Failed: return Status::Failed;
		case State::Length:
		case State::ChunkData: {
			size_t n = len - i;
			if (n > remaining_) n = static_cast<size_t>(remaining_);
			if (!deliver(data + i, n)) return Status::Failed;
			i += n;
			remaining_ -= n;
			if (remaining_ == 0) {
				if (state_ == State::Length) return finish();
				state_ = State::ChunkEnd;
			}
			break;
		}
		case State::UntilClose:
			if (!deliver(data + i, len - i)) return Status::Failed;
			return Status::NeedMore;
		default: { // line-oriented states: one byte at a time
			char c = static_cast<char>(data[i++]);
			if (c == '\n') {
				if (!line_.empty() && line_.back() == '\r') line_.pop_back();
				std::string line;
				line.swap(line_);
				if (!onLine(line)) return Status::Failed;
				if (state_ == State::Done) return Status::Done;
			} else {
				line_.push_back(c);
				headerBytes_++;
				if (line_.size() > kMaxLine || headerBytes_ > kMaxHeaderBytes) return fail(HttpError::Parse, "header too long");
			}
			break;
		}
		}
	}
	return state_ == State::Done ? Status::Done : state_ == State::Failed ? Status::Failed : Status::NeedMore;
}

ResponseParser::Status ResponseParser::finishEof() {
	if (state_ == State::Done) return Status::Done;
	if (state_ == State::Failed) return Status::Failed;
	if (state_ == State::UntilClose) return finish();
	return fail(HttpError::Io, "connection closed before the response was complete");
}

} // namespace httpparse
