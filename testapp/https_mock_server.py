#!/usr/bin/env python3
"""TEST-ONLY HTTPS transport and KOSync mock server."""

import argparse
import socket
import ssl
import time

from kosync_mock_server import KOSyncHandler, MockServer


class HTTPSHandler(KOSyncHandler):
    def do_GET(self):
        if self.path == "/https/get":
            self.send_bytes(b"hello over verified TLS")
            return
        if self.path == "/https/large":
            self.send_bytes(b"x" * 70_000)
            return
        if self.path == "/https/short-body":
            self.send_abbreviated(
                b"HTTP/1.0 200 OK\r\nContent-Length: 100\r\n\r\nxx", True)
            return
        if self.path == "/https/short-headers":
            self.send_abbreviated(
                b"HTTP/1.0 200 OK\r\nContent-Length: 2\r\n", True)
            return
        if self.path == "/https/raw-eof":
            self.send_abbreviated(
                b"HTTP/1.0 200 OK\r\nContent-Length: 100\r\n\r\nxx", False)
            return
        if self.path == "/https/stall":
            time.sleep(self.server.stall_seconds)
            self.send_bytes(b"late response")
            return
        super().do_GET()

    def do_PUT(self):
        if self.path == "/https/write-stall":
            time.sleep(self.server.stall_seconds)
            return
        if self.path == "/https/put":
            try:
                length = int(self.headers.get("Content-Length", ""))
            except ValueError:
                length = -1
            if length < 0 or length > 4096:
                self.send_error(413)
                return
            body = self.rfile.read(length)
            self.send_bytes(body)
            return
        super().do_PUT()

    def send_bytes(self, body):
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def send_abbreviated(self, payload, clean_close):
        self.wfile.write(payload)
        self.wfile.flush()
        self.close_connection = True
        if clean_close:
            try:
                self.request.unwrap()
            except (OSError, ssl.SSLError):
                pass
        else:
            descriptor = self.request.detach()
            raw_socket = socket.socket(fileno=descriptor)
            raw_socket.shutdown(socket.SHUT_RDWR)
            raw_socket.close()


class HTTPSMockServer(MockServer):
    def __init__(self, address, timestamp_start, quiet, stall_seconds):
        super().__init__(address, timestamp_start, quiet)
        self.RequestHandlerClass = HTTPSHandler
        self.stall_seconds = stall_seconds


def run_raw_listener(host, port, mode, delay):
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind((host, port))
    listener.listen(4)
    print(f"HTTPS MOCK {mode} listening on {host}:{port}", flush=True)
    while True:
        connection, _ = listener.accept()
        try:
            if mode == "plain":
                connection.sendall(b"HTTP/1.0 200 OK\r\n\r\nnot TLS")
            else:
                time.sleep(delay)
        finally:
            connection.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--cert")
    parser.add_argument("--key")
    parser.add_argument("--sni-log")
    parser.add_argument("--timestamp-start", type=int, default=1_700_000_000)
    parser.add_argument("--stall-seconds", type=float, default=2.0)
    parser.add_argument("--mode",
                        choices=("tls", "plain", "handshake-stall"),
                        default="tls")
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()

    if args.mode in ("plain", "handshake-stall"):
        run_raw_listener(args.host, args.port, args.mode, args.stall_seconds)
        return
    if not args.cert or not args.key:
        parser.error("--cert and --key are required in tls mode")

    server = HTTPSMockServer((args.host, args.port), args.timestamp_start,
                             args.quiet, args.stall_seconds)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.maximum_version = ssl.TLSVersion.TLSv1_2
    context.set_ciphers("ECDHE-RSA-AES128-GCM-SHA256")
    context.load_cert_chain(args.cert, args.key)

    def record_sni(_socket, server_name, _context):
        if args.sni_log:
            with open(args.sni_log, "a", encoding="ascii") as output:
                output.write((server_name or "<none>") + "\n")

    context.set_servername_callback(record_sni)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    print(f"HTTPS MOCK listening https://{args.host}:{args.port}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
