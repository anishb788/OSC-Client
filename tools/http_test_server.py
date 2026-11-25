#!/usr/bin/env python3
"""Local HTTP test server for the OpenShop network layer (stdlib only).

  tools/http_test_server.py [--port N] [--host 127.0.0.1]     (port 0 = ephemeral; the chosen port is printed)

Endpoints (same set as tests/LoopbackServer.h; bodies use the same byte pattern so a client can verify them):
  /len/N             Content-Length body of N bytes
  /chunked/N/C       chunked body of N bytes in chunks of C bytes
  /redir/PATH        302 to http://<this server>/PATH
  /redir-https       302 to https://example.org/
  /stall             headers + a few bytes, then nothing until the client disconnects
  /junk-json         200 with truncated, invalid JSON
  /slow/N/RATE       N bytes dripped at about RATE bytes/s (e.g. /slow/4194304/204800 for the contention check)
  /headers           echoes the request head back as the body
Every request head is logged to stderr, so the User-Agent can be checked by eye.
"""
import argparse
import socketserver
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

_PERIOD = 65536


def _build_pattern():
    # byte i = (i*167 + (i>>8)*13 + 5) & 0xff; it repeats every 65536 bytes.
    return bytes(((i * 167 + (i >> 8) * 13 + 5) & 0xFF) for i in range(_PERIOD))


_PATTERN = _build_pattern()


def pattern(offset, n):
    out = bytearray()
    while n > 0:
        start = offset % _PERIOD
        take = min(n, _PERIOD - start)
        out += _PATTERN[start:start + take]
        offset += take
        n -= take
    return bytes(out)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass

    def _head(self, code, headers):
        self.send_response(code)
        for k, v in headers:
            self.send_header(k, v)
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True

    def do_GET(self):
        head = "%s %s %s\r\n%s" % (self.command, self.path, self.request_version, self.headers)
        sys.stderr.write("--- request ---\n%s\n" % head.replace("\r\n", "\n"))
        sys.stderr.flush()
        parts = self.path.split("?")[0].strip("/").split("/")
        try:
            self._route(parts, head)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def _route(self, parts, head):
        kind = parts[0]
        if kind == "len" and len(parts) == 2:
            n = int(parts[1])
            self._head(200, [("Content-Length", str(n))])
            sent = 0
            while sent < n:
                step = min(65536, n - sent)
                self.wfile.write(pattern(sent, step))
                sent += step
        elif kind == "chunked" and len(parts) == 3:
            n, c = int(parts[1]), max(1, int(parts[2]))
            self._head(200, [("Transfer-Encoding", "chunked")])
            sent = 0
            while sent < n:
                step = min(c, n - sent)
                self.wfile.write(b"%x\r\n" % step + pattern(sent, step) + b"\r\n")
                sent += step
            self.wfile.write(b"0\r\n\r\n")
        elif kind == "redir" and len(parts) >= 2:
            host = self.headers.get("Host", "127.0.0.1")
            self._head(302, [("Location", "http://%s/%s" % (host, "/".join(parts[1:]))), ("Content-Length", "0")])
        elif kind == "redir-https":
            self._head(302, [("Location", "https://example.org/"), ("Content-Length", "0")])
        elif kind == "stall":
            self._head(200, [("Content-Length", "100000")])
            self.wfile.write(b"abc")
            self.wfile.flush()
            self.connection.settimeout(1.0)
            while True:
                try:
                    if self.connection.recv(1024) == b"":
                        break
                except OSError as e:
                    if isinstance(e, TimeoutError) or "timed out" in str(e):
                        continue
                    break
        elif kind == "junk-json":
            body = b'{"apps": [{"slug": "x", "name": '
            self._head(200, [("Content-Type", "application/json"), ("Content-Length", str(len(body)))])
            self.wfile.write(body)
        elif kind == "slow" and len(parts) == 3:
            n, rate = int(parts[1]), max(1, int(parts[2]))
            self._head(200, [("Content-Length", str(n))])
            per = max(1, rate // 100)    # a slice every 10 ms
            sent = 0
            nxt = time.monotonic()
            while sent < n:
                step = min(per, n - sent)
                self.wfile.write(pattern(sent, step))
                self.wfile.flush()
                sent += step
                nxt += 0.01
                time.sleep(max(0.0, nxt - time.monotonic()))
        elif kind == "headers":
            body = head.encode()
            self._head(200, [("Content-Length", str(len(body)))])
            self.wfile.write(body)
        else:
            self._head(404, [("Content-Length", "0")])


class Server(ThreadingHTTPServer):
    daemon_threads = True
    request_queue_size = 32


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=0)
    args = ap.parse_args()
    srv = Server((args.host, args.port), Handler)
    print("PORT %d" % srv.server_address[1], flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
