#pragma once
// A tiny HTTP/1.0-style server on 127.0.0.1 (ephemeral port) for the real-socket tests, so they never need the
// network. One thread accepts, one thread per connection. It mirrors the endpoints of tools/http_test_server.py:
//   /len/N            Content-Length body of N pattern bytes
//   /chunked/N/C      chunked body of N pattern bytes in chunks of C
//   /redir/PATH       302 to http://127.0.0.1:PORT/PATH
//   /redir-https      302 to https://example.org/
//   /stall            headers and a few bytes, then silence until the client leaves
//   /slow/N/RATE      Content-Length body of N bytes dripped at about RATE bytes/s
//   /headers          echoes the request head back as the body
//   /reset            closes the connection with an RST as soon as it is accepted
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

// Same formula as tools/http_test_server.py.
inline uint8_t patternByte(uint64_t i) { return static_cast<uint8_t>(i * 167 + (i >> 8) * 13 + 5); }

inline std::string patternBody(uint64_t n) {
	std::string s(n, 0);
	for (uint64_t i = 0; i < n; i++) s[i] = static_cast<char>(patternByte(i));
	return s;
}

class LoopbackServer {
public:
	LoopbackServer() {
		listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
		int one = 1;
		setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
		sockaddr_in sa{};
		sa.sin_family = AF_INET;
		sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0 || ::listen(listenFd_, 16) != 0) {
			std::fprintf(stderr, "LoopbackServer: bind/listen failed\n");
			std::abort();
		}
		socklen_t len = sizeof sa;
		getsockname(listenFd_, reinterpret_cast<sockaddr*>(&sa), &len);
		port_ = ntohs(sa.sin_port);
		acceptor_ = std::thread([this] { acceptLoop(); });
	}
	~LoopbackServer() {
		stop_ = true;
		acceptor_.join();
		for (auto& t : conns_) t.join();
		::close(listenFd_);
	}
	LoopbackServer(const LoopbackServer&) = delete;
	LoopbackServer& operator=(const LoopbackServer&) = delete;

	int port() const { return port_; }
	std::string url(const std::string& path) const { return "http://127.0.0.1:" + std::to_string(port_) + path; }
	std::vector<std::string> requests() {
		std::lock_guard<std::mutex> l(mu_);
		return requests_;
	}

private:
	void acceptLoop() {
		while (!stop_) {
			pollfd p{listenFd_, POLLIN, 0};
			if (::poll(&p, 1, 20) <= 0) continue;
			int fd = ::accept(listenFd_, nullptr, nullptr);
			if (fd < 0) continue;
			conns_.emplace_back([this, fd] { serve(fd); });
		}
	}

	bool sendAll(int fd, const char* d, size_t n) {
		while (n > 0 && !stop_) {
			ssize_t w = ::send(fd, d, n, MSG_NOSIGNAL);
			if (w < 0) return false;
			d += w;
			n -= static_cast<size_t>(w);
		}
		return n == 0;
	}

	void serve(int fd) {
		std::string head;
		char buf[2048];
		while (head.find("\r\n\r\n") == std::string::npos && head.size() < 16384 && !stop_) {
			pollfd p{fd, POLLIN, 0};
			if (::poll(&p, 1, 20) <= 0) continue;
			ssize_t n = ::recv(fd, buf, sizeof buf, 0);
			if (n <= 0) {
				::close(fd);
				return;
			}
			head.append(buf, static_cast<size_t>(n));
		}
		{
			std::lock_guard<std::mutex> l(mu_);
			requests_.push_back(head);
		}
		std::string path = head.substr(head.find(' ') + 1);
		path = path.substr(0, path.find(' '));
		unsigned long long a = 0, b = 0;

		if (path == "/reset") {
			linger lg{1, 0};
			setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
		} else if (std::sscanf(path.c_str(), "/len/%llu", &a) == 1) {
			std::string h = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(a) + "\r\nConnection: close\r\n\r\n";
			sendAll(fd, h.data(), h.size());
			std::string body = patternBody(a);
			sendAll(fd, body.data(), body.size());
		} else if (std::sscanf(path.c_str(), "/chunked/%llu/%llu", &a, &b) == 2 && b > 0) {
			std::string body = patternBody(a), out = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
			for (size_t i = 0; i < body.size(); i += b) {
				size_t n = std::min<size_t>(b, body.size() - i);
				char hex[32];
				std::snprintf(hex, sizeof hex, "%zx\r\n", n);
				out += hex;
				out.append(body, i, n);
				out += "\r\n";
			}
			out += "0\r\n\r\n";
			sendAll(fd, out.data(), out.size());
		} else if (path.rfind("/redir/", 0) == 0) {
			std::string h = "HTTP/1.1 302 Found\r\nLocation: " + url(path.substr(6)) + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
			sendAll(fd, h.data(), h.size());
		} else if (path == "/redir-https") {
			std::string h = "HTTP/1.1 302 Found\r\nLocation: https://example.org/\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
			sendAll(fd, h.data(), h.size());
		} else if (path == "/stall") {
			std::string h = "HTTP/1.1 200 OK\r\nContent-Length: 100000\r\n\r\nabc";
			sendAll(fd, h.data(), h.size());
			while (!stop_) {
				pollfd p{fd, POLLIN, 0};
				if (::poll(&p, 1, 20) > 0 && ::recv(fd, buf, sizeof buf, 0) <= 0) break;
			}
		} else if (std::sscanf(path.c_str(), "/slow/%llu/%llu", &a, &b) == 2 && b > 0) {
			std::string h = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(a) + "\r\nConnection: close\r\n\r\n";
			sendAll(fd, h.data(), h.size());
			std::string body = patternBody(a);
			size_t per = std::max<size_t>(1, b / 100);   // a slice every 10 ms
			for (size_t i = 0; i < body.size() && !stop_; i += per) {
				if (!sendAll(fd, body.data() + i, std::min(per, body.size() - i))) break;
				usleep(10000);
			}
		} else if (path == "/headers") {
			std::string h = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(head.size()) + "\r\nConnection: close\r\n\r\n" + head;
			sendAll(fd, h.data(), h.size());
		} else {
			static const char nf[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
			sendAll(fd, nf, sizeof nf - 1);
		}
		::close(fd);
	}

	int listenFd_ = -1;
	int port_ = 0;
	std::atomic<bool> stop_{false};
	std::thread acceptor_;
	std::vector<std::thread> conns_;   // only touched by the acceptor thread until the destructor joins it
	std::mutex mu_;
	std::vector<std::string> requests_;
};
