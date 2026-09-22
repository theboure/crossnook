#!/usr/bin/env python3
"""Deterministic plain-HTTP mock of the pinned KOReader progress API subset."""

import argparse
import http.client
import json
import re
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


ACCEPT = "application/vnd.koreader.v1+json"
TEST_USER = "test-user"
TEST_KEY = "dfb450efddbb5387197c84460623675b"
OTHER_USER = "other-user"
OTHER_KEY = "other-key"
DOC_RE = re.compile(r"^[0-9a-f]{32}$")
MAX_REQUEST = 400_000
MAX_POSITION = 65_536
FAULT_MALFORMED = "00000000000000000000000000000001"
FAULT_PROTOCOL = "00000000000000000000000000000002"
FAULT_OVERSIZED = "00000000000000000000000000000003"
FAULT_SERVER = "00000000000000000000000000000004"
TEST_DOCUMENT = "e1a1e9016cfc9bca8c694187943e9c4f"
POSITION_ONE = "/body/DocFragment[1]/body/p[1]/text().0"
POSITION_TWO = "/body/DocFragment[1]/body/p[3]/text().5"
INTEGRATION_USERS = (
    "integration-local-only",
    "integration-remote-only",
    "integration-both-missing",
    "integration-same",
    "integration-same-percentage-different",
    "integration-different-same-percentage",
    "integration-different",
    "integration-unsupported",
    "integration-auth",
    "integration-malformed",
    "integration-timeout",
)


class Store:
    def __init__(self, timestamp_start, integration_fixtures=False):
        self.users = {TEST_USER: TEST_KEY, OTHER_USER: OTHER_KEY}
        self.users.update({username: TEST_KEY for username in INTEGRATION_USERS})
        self.progress = {}
        self.next_timestamp = timestamp_start
        self.lock = threading.Lock()
        if integration_fixtures:
            self.seed_integration(timestamp_start)

    def seed_integration(self, timestamp):
        fixtures = {
            "integration-remote-only": (POSITION_TWO, 0.6543),
            "integration-same": (POSITION_ONE, 0.321),
            "integration-same-percentage-different": (POSITION_ONE, 0.9999),
            "integration-different-same-percentage": (POSITION_TWO, 0.321),
            "integration-different": (POSITION_TWO, 0.6543),
        }
        for username, (position, percentage) in fixtures.items():
            self.progress[(username, TEST_DOCUMENT)] = {
                "document": TEST_DOCUMENT,
                "progress": position,
                "percentage": percentage,
                "device": "remote-test-device",
                "device_id": "remote-test-device-id",
                "timestamp": timestamp,
            }

    def put(self, username, record):
        with self.lock:
            timestamp = self.next_timestamp
            self.next_timestamp += 1
            saved = dict(record)
            saved["timestamp"] = timestamp
            self.progress[(username, record["document"])] = saved
            return timestamp

    def get(self, username, document):
        with self.lock:
            record = self.progress.get((username, document))
            return dict(record) if record is not None else None


class KOSyncHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"
    server_version = "CrossNookKOSyncMock/1"

    def log_message(self, format_string, *args):
        if not self.server.quiet:
            super().log_message(format_string, *args)

    def send_json(self, status, value):
        body = json.dumps(value, separators=(",", ":"), ensure_ascii=False).encode()
        self.send_response(status)
        self.send_header("Content-Type", ACCEPT)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def authenticated_user(self):
        username = self.headers.get("x-auth-user")
        key = self.headers.get("x-auth-key")
        if username and self.server.store.users.get(username) == key:
            return username
        self.server.record_event({"method": self.command, "username": username,
                                  "authenticated": False})
        self.send_json(401, {"code": 2001, "message": "Unauthorized"})
        return None

    def protocol_headers_ok(self):
        if self.headers.get("Accept") != ACCEPT:
            self.send_json(406, {"message": "vendor Accept required"})
            return False
        return True

    def do_GET(self):
        if not self.protocol_headers_ok():
            return
        prefix = "/syncs/progress/"
        if not self.path.startswith(prefix):
            self.send_json(404, {"message": "Not found"})
            return
        username = self.authenticated_user()
        if username is None:
            return
        document = self.path[len(prefix):]
        self.server.record_event({"method": "GET", "username": username,
                                  "document": document})
        if username == "integration-timeout":
            time.sleep(self.server.kosync_stall_seconds)
            return
        if username == "integration-malformed":
            body = b'{"document":'
            self.send_response(200)
            self.send_header("Content-Type", ACCEPT)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if document == FAULT_SERVER:
            self.send_json(500, {"code": 2000, "message": "Injected error"})
            return
        if document == FAULT_MALFORMED:
            body = b'{"document":'
            self.send_response(200)
            self.send_header("Content-Type", ACCEPT)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if document == FAULT_PROTOCOL:
            self.send_json(200, {"document": document})
            return
        if document == FAULT_OVERSIZED:
            body = b'{"padding":"' + (b"x" * 410_000) + b'"}'
            self.send_response(200)
            self.send_header("Content-Type", ACCEPT)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if not DOC_RE.fullmatch(document):
            self.send_json(403, {"code": 2004, "message": "Document field missing"})
            return
        record = self.server.store.get(username, document)
        self.send_json(200, record if record is not None else {})

    def do_PUT(self):
        if not self.protocol_headers_ok():
            return
        if self.path != "/syncs/progress":
            self.send_json(404, {"message": "Not found"})
            return
        username = self.authenticated_user()
        if username is None:
            return
        if self.headers.get_content_type() != "application/json":
            self.send_json(415, {"message": "JSON required"})
            return
        try:
            length = int(self.headers.get("Content-Length", ""))
        except ValueError:
            length = -1
        if length < 0 or length > MAX_REQUEST:
            self.send_json(413, {"message": "Request too large"})
            return
        body = self.rfile.read(length)
        try:
            request = json.loads(body.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            self.send_json(403, {"code": 2003, "message": "Invalid request"})
            return
        if not self.valid_progress(request):
            self.send_json(403, {"code": 2003, "message": "Invalid request"})
            return
        self.server.record_event({
            "method": "PUT",
            "username": username,
            "document": request["document"],
            "progress": request["progress"],
            "percentage": request["percentage"],
        })
        timestamp = self.server.store.put(username, request)
        self.send_json(200, {"document": request["document"],
                             "timestamp": timestamp})

    @staticmethod
    def valid_progress(request):
        if not isinstance(request, dict):
            return False
        document = request.get("document")
        progress = request.get("progress")
        percentage = request.get("percentage")
        device = request.get("device")
        device_id = request.get("device_id")
        return (
            isinstance(document, str) and DOC_RE.fullmatch(document) is not None
            and isinstance(progress, str) and 0 < len(progress.encode()) <= MAX_POSITION
            and isinstance(percentage, (int, float)) and not isinstance(percentage, bool)
            and 0 <= percentage <= 1
            and isinstance(device, str) and bool(device)
            and isinstance(device_id, str) and bool(device_id)
        )


class MockServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address, timestamp_start, quiet=False,
                 integration_fixtures=False, transcript=None,
                 kosync_stall_seconds=2.0):
        super().__init__(address, KOSyncHandler)
        self.store = Store(timestamp_start, integration_fixtures)
        self.quiet = quiet
        self.transcript = transcript
        self.transcript_lock = threading.Lock()
        self.kosync_stall_seconds = kosync_stall_seconds

    def record_event(self, event):
        if not self.transcript:
            return
        line = json.dumps(event, sort_keys=True, separators=(",", ":"))
        with self.transcript_lock:
            with open(self.transcript, "a", encoding="utf-8",
                      newline="\n") as output:
                output.write(line + "\n")


def raw_request(port, method, path, body=None, user=TEST_USER, key=TEST_KEY,
                accept=ACCEPT):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
    headers = {"Accept": accept, "x-auth-user": user, "x-auth-key": key}
    encoded = None
    if body is not None:
        encoded = body if isinstance(body, bytes) else json.dumps(body).encode()
        headers["Content-Type"] = "application/json"
    connection.request(method, path, body=encoded, headers=headers)
    response = connection.getresponse()
    payload = response.read()
    status = response.status
    connection.close()
    return status, payload


def self_test():
    fixtures = json.loads(Path(__file__).with_name("kosync-fixtures.json").read_text(
        encoding="utf-8"))
    assert fixtures["documents"]["test.epub"] == fixtures["documents"]["valid2.epub"]
    assert fixtures["documents"]["foreign.epub"] != fixtures["documents"]["test.epub"]
    assert fixtures["put_request"]["document"] == fixtures["documents"]["test.epub"]
    assert fixtures["put_response"]["timestamp"] == 1_700_000_000
    server = MockServer(("127.0.0.1", 0), 1_700_000_000, quiet=True)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    port = server.server_address[1]
    document = "519220cea448409961e6b3081a36eca3"
    other = "11111111111111111111111111111111"
    record = {
        "document": document,
        "progress": "/body/DocFragment[1]/body/p[1]/text().0",
        "percentage": 0.321,
        "device": "crossnook-test-device",
        "device_id": "crossnook-test-device-id",
    }
    try:
        status, payload = raw_request(port, "GET", f"/syncs/progress/{document}")
        assert status == 200 and json.loads(payload) == {}
        assert raw_request(port, "PUT", "/syncs/progress", b"{")[0] == 403
        assert raw_request(port, "PUT", "/syncs/progress", record,
                           key="wrong")[0] == 401
        status, payload = raw_request(port, "PUT", "/syncs/progress", record)
        assert status == 200 and json.loads(payload)["timestamp"] == 1_700_000_000
        status, payload = raw_request(port, "GET", f"/syncs/progress/{document}")
        assert status == 200 and json.loads(payload)["progress"] == record["progress"]
        updated = dict(record, progress="/updated", percentage=0.5)
        assert raw_request(port, "PUT", "/syncs/progress", updated)[0] == 200
        status, payload = raw_request(port, "GET", f"/syncs/progress/{document}")
        assert json.loads(payload)["progress"] == "/updated"
        independent = dict(record, document=other, progress="/other")
        assert raw_request(port, "PUT", "/syncs/progress", independent)[0] == 200
        status, payload = raw_request(port, "GET", f"/syncs/progress/{document}")
        assert json.loads(payload)["progress"] == "/updated"
        assert raw_request(port, "PUT", "/syncs/progress", record,
                           user=OTHER_USER, key=OTHER_KEY)[0] == 200
        status, payload = raw_request(port, "GET", f"/syncs/progress/{document}",
                                      user=OTHER_USER, key=OTHER_KEY)
        assert json.loads(payload)["progress"] == record["progress"]
        status, payload = raw_request(port, "GET", f"/syncs/progress/{document}")
        assert json.loads(payload)["progress"] == "/updated"
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=3)
    print("KOSYNC MOCK SERVER SELF TEST OK")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--timestamp-start", type=int, default=1_700_000_000)
    parser.add_argument("--quiet", action="store_true")
    parser.add_argument("--integration-fixtures", action="store_true")
    parser.add_argument("--transcript")
    parser.add_argument("--kosync-stall-seconds", type=float, default=2.0)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return
    server = MockServer((args.host, args.port), args.timestamp_start, args.quiet,
                        args.integration_fixtures, args.transcript,
                        args.kosync_stall_seconds)
    print(f"KOSYNC MOCK listening http://{args.host}:{args.port}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
