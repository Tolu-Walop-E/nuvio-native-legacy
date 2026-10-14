#!/usr/bin/env python3
"""Local range/fault server for Nuvio network tests; never contacts a CDN."""
from __future__ import annotations

import argparse
import json
import os
import re
import socket
import ssl
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, quote, urlsplit

SIZES = {"mp4": 65 * 1024**3, "mkv": 118 * 1024**3, "small": 8 * 1024**2}
state_lock = threading.Lock()
state = {
    "mode": "normal", "stall_after": 0, "stall_ms": 120_000, "close_after": 0,
    "retry_after": 1, "expire_after": 0.0, "started": time.monotonic(),
    "redirect_target": "",
}
connection_ids, connection_next = {}, 1


def connection_id(sock):
    global connection_next
    key = sock.fileno()
    with state_lock:
        if key not in connection_ids:
            connection_ids[key] = connection_next
            connection_next += 1
        return connection_ids[key]


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "NuvioFault/1"

    def log_message(self, fmt, *args):
        return

    def record(self, status, start=-1, end=-1):
        row = {
            "time": time.time(), "connection": connection_id(self.connection),
            "client": self.client_address[0], "method": self.command,
            "path": urlsplit(self.path).path, "range_start": start,
            "range_end": end, "status": status,
        }
        line = json.dumps(row, separators=(",", ":"))
        print(line, flush=True)
        log_path = getattr(self.server, "account_log", "")
        if log_path:
            with state_lock:
                with open(log_path, "a", encoding="utf-8") as handle:
                    handle.write(line + "\n")

    def send_json(self, status, value):
        body = json.dumps(value).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD": self.wfile.write(body)
        self.record(status)

    def do_POST(self):
        self.do_GET()

    def do_HEAD(self):
        self.do_GET()

    def do_GET(self):
        parsed = urlsplit(self.path)
        path, query = parsed.path, parse_qs(parsed.query)
        if path == "/control":
            with state_lock:
                for key in ("mode", "redirect_target"):
                    if key in query: state[key] = query[key][-1]
                for key in ("stall_after", "stall_ms", "close_after", "retry_after"):
                    if key in query: state[key] = int(query[key][-1])
                if "expire_after" in query:
                    state["expire_after"] = float(query["expire_after"][-1])
                    state["started"] = time.monotonic()
                snapshot = dict(state)
            return self.send_json(200, snapshot)
        if path == "/accounting":
            return self.send_json(200, {"connections": len(connection_ids)})
        if path == "/manifest.json":
            return self.send_json(200, {
                "id": "space.nuvio.network-fixture", "version": "1.0.0",
                "name": "Nuvio Network Fixture", "resources": ["stream"],
                "types": ["movie"], "idPrefixes": ["tt"],
            })
        if path.startswith("/stream/movie/"):
            host, port = self.server.server_address[:2]
            base = f"http://{host}:{port}"
            return self.send_json(200, {"streams": [
                {"name": "Fixture 65 GB MP4", "url": base + "/file/mp4"},
                {"name": "Fixture 118 GB MKV", "url": base + "/file/mkv"},
            ]})
        match = re.fullmatch(r"/r/(\d+)(/.*)", path)
        if match:
            count, rest = int(match.group(1)), match.group(2)
            target = rest if count <= 1 else f"/r/{count - 1}{rest}"
            return self.redirect(target, query)
        if path.startswith("/redirect-target"):
            with state_lock: target = state["redirect_target"]
            if not target: return self.send_json(409, {"error": "redirect_target unset"})
            self.send_response(302); self.send_header("Location", target)
            self.send_header("Content-Length", "0"); self.end_headers(); self.record(302); return
        match = re.fullmatch(r"/file/(mp4|mkv|small)", path)
        if match:
            return self.file(SIZES[match.group(1)])
        self.send_error(404)
        self.record(404)

    def redirect(self, target, query):
        suffix = "?" + "&".join(f"{quote(k)}={quote(v[-1])}" for k, v in query.items()) if query else ""
        self.send_response(302)
        self.send_header("Location", target + suffix)
        self.send_header("Content-Length", "0")
        self.end_headers(); self.record(302)

    def file(self, size):
        with state_lock: cfg = dict(state)
        elapsed = time.monotonic() - cfg["started"]
        mode = cfg["mode"]
        if cfg["expire_after"] and elapsed >= cfg["expire_after"]: mode = "expired"
        status = {"429": 429, "503": 503, "expired": 403}.get(mode)
        if status:
            self.send_response(status)
            if status in (429, 503): self.send_header("Retry-After", str(cfg["retry_after"]))
            self.send_header("Content-Length", "0"); self.end_headers(); self.record(status); return
        start, end = 0, size - 1
        raw = self.headers.get("Range", "")
        match = re.fullmatch(r"bytes=(\d+)-(\d*)", raw)
        partial = bool(match) and mode != "ignore_range"
        if partial:
            start = int(match.group(1)); end = int(match.group(2) or end)
            if start >= size:
                self.send_response(416); self.send_header("Content-Range", f"bytes */{size}")
                self.send_header("Content-Length", "0"); self.end_headers(); self.record(416, start, end); return
            end = min(end, size - 1)
        length = end - start + 1 if partial else size
        code = 206 if partial else 200
        self.send_response(code)
        self.send_header("Content-Type", "video/mp4")
        self.send_header("Accept-Ranges", "bytes")
        if partial: self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.send_header("Content-Length", str(length))
        self.end_headers(); self.record(code, start, end)
        if self.command == "HEAD": return
        sent, block = 0, bytes((i % 251 for i in range(64 * 1024)))
        try:
            while sent < length:
                if cfg["stall_after"] and sent >= cfg["stall_after"]:
                    time.sleep(cfg["stall_ms"] / 1000.0); return
                if cfg["close_after"] and sent >= cfg["close_after"]:
                    self.connection.shutdown(socket.SHUT_RDWR); self.connection.close(); return
                amount = min(len(block), length - sent)
                self.wfile.write(block[:amount]); sent += amount
        except (BrokenPipeError, ConnectionResetError, OSError):
            return


def serve(port, account_log, cert="", key=""):
    server = ThreadingHTTPServer(("0.0.0.0", port), Handler)
    server.account_log = account_log
    if cert:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        server.socket = context.wrap_socket(server.socket, server_side=True)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server, thread


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--https-port", type=int, default=0)
    parser.add_argument("--cert", default="")
    parser.add_argument("--key", default="")
    parser.add_argument("--log", default="")
    args = parser.parse_args()
    servers = [serve(args.port, args.log)]
    if args.https_port:
        if not args.cert or not args.key: parser.error("--https-port requires --cert and --key")
        servers.append(serve(args.https_port, args.log, args.cert, args.key))
    print(json.dumps({"ready": True, "http": args.port, "https": args.https_port}), flush=True)
    try:
        while True: time.sleep(3600)
    except KeyboardInterrupt:
        for server, _ in servers: server.shutdown()


if __name__ == "__main__":
    main()
