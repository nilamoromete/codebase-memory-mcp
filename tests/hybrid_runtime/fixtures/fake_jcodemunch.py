"""Small deterministic Windows-process substitute for lifecycle tests.

The fixture deliberately exposes identity through a local HTTP endpoint and
records its process metadata in JSON files. It does not read user config or
touch any repository state outside the paths supplied by the test.
"""

from __future__ import annotations

import argparse
import json
import os
import signal
import socket
import socketserver
import sys
import threading
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler
from pathlib import Path


class _Handler(BaseHTTPRequestHandler):
    server_version = "jcodemunch-fixture/1"

    def _authorized(self) -> bool:
        bearer_token = getattr(self.server, "bearer_token", None)
        if bearer_token and self.headers.get("Authorization") != f"Bearer {bearer_token}":
            self.send_error(HTTPStatus.UNAUTHORIZED)
            return False
        return True

    def do_GET(self) -> None:  # noqa: N802 - stdlib handler API
        if not self._authorized():
            return
        payload = getattr(self.server, "identity_payload")
        if self.path not in {"/health", "/runtime-identity"}:
            self.send_error(HTTPStatus.NOT_FOUND)
            return
        body = json.dumps(payload, sort_keys=True).encode("utf-8")
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self) -> None:  # noqa: N802 - stdlib handler API
        if not self._authorized():
            return
        if self.path != "/mcp":
            self.send_error(HTTPStatus.NOT_FOUND)
            return
        length = int(self.headers.get("Content-Length", "0"))
        message = json.loads(self.rfile.read(length) or b"{}")
        method = message.get("method")
        if method == "notifications/initialized":
            self.send_response(HTTPStatus.ACCEPTED)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if method == "initialize":
            result = {
                "protocolVersion": "2025-03-26",
                "capabilities": {"resources": {}},
                "serverInfo": {
                    "name": getattr(self.server, "mcp_server_name"),
                    "version": "fixture",
                },
            }
        elif method == "resources/read" and message.get("params", {}).get("uri") == "munch://runtime/identity":
            result = {
                "contents": [
                    {
                        "uri": "munch://runtime/identity",
                        "mimeType": "application/json",
                        "text": json.dumps(getattr(self.server, "mcp_identity_payload")),
                    }
                ]
            }
        elif method == "tools/list":
            result = {
                "tools": [
                    {
                        "name": "index_folder",
                        "inputSchema": {
                            "type": "object",
                            "properties": {"path": {"type": "string"}},
                            "required": ["path"],
                        },
                    },
                    {
                        "name": "search_symbols",
                        "inputSchema": {
                            "type": "object",
                            "properties": {
                                "repo": {"type": "string"},
                                "query": {"type": "string"},
                            },
                            "required": ["repo", "query"],
                        },
                    },
                    {
                        "name": "index_file",
                        "inputSchema": {
                            "type": "object",
                            "properties": {"path": {"type": "string"}},
                            "required": ["path"],
                        },
                    },
                    {
                        "name": "list_repos",
                        "inputSchema": {"type": "object", "properties": {}},
                    },
                ]
            }
        elif method == "tools/call":
            params = message.get("params", {})
            result = {
                "content": [
                    {
                        "type": "text",
                        "text": json.dumps(
                            {
                                "tool": params.get("name"),
                                "arguments": params.get("arguments", {}),
                            },
                            sort_keys=True,
                        ),
                    }
                ]
            }
        else:
            self.send_error(HTTPStatus.BAD_REQUEST)
            return
        response = {"jsonrpc": "2.0", "id": message.get("id"), "result": result}
        body = f"event: message\r\ndata: {json.dumps(response, separators=(',', ':'))}\r\n\r\n".encode("utf-8")
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Mcp-Session-Id", "fixture-session")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_DELETE(self) -> None:  # noqa: N802 - stdlib handler API
        if not self._authorized():
            return
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def log_message(self, *_args: object) -> None:
        return


class _Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = False
    daemon_threads = True


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--ready-file", type=Path, required=True)
    parser.add_argument("--identity-file", type=Path, required=True)
    parser.add_argument("--stop-file", type=Path)
    parser.add_argument("--launch-id", default="fixture-launch-001")
    parser.add_argument("--runtime-identity", default="fixture-runtime-001")
    parser.add_argument("--server-name", default="jcodemunch")
    return parser.parse_args()


def main() -> int:
    args = _parse_args()
    pid_log = os.environ.get("JCODEMUNCH_TEST_PID_LOG")
    if pid_log:
        with Path(pid_log).open("a", encoding="utf-8") as stream:
            stream.write(f"{os.getpid()}\n")
    identity = {
        "server_name": args.server_name,
        "transport": "streamable-http",
        "bind_host": args.host,
        "port": args.port,
        "pid": os.getpid(),
        "launch_id": args.launch_id,
        "runtime_identity": args.runtime_identity,
        "command_fingerprint": "sha256:owned-jcodemunch-fixture",
    }
    args.identity_file.parent.mkdir(parents=True, exist_ok=True)
    args.identity_file.write_text(json.dumps(identity, sort_keys=True), encoding="utf-8")

    try:
        server = _Server((args.host, args.port), _Handler)
    except OSError as exc:
        args.identity_file.unlink(missing_ok=True)
        print(json.dumps({"error": "bind_failed", "errno": exc.errno}), file=sys.stderr)
        return 98

    server.identity_payload = identity
    server.bearer_token = os.environ.get("JCODEMUNCH_HTTP_TOKEN")
    server.mcp_server_name = (
        "invalid-bootstrap-fixture"
        if os.environ.get("JCODEMUNCH_TEST_PROBE_FAILURE") == "1"
        else "jcodemunch-mcp"
    )
    server.mcp_identity_payload = {
        "schema": "munch.runtime.identity/v1",
        "product": "jcodemunch-mcp",
        "version": "1.108.291",
        "transport": "streamable-http",
        "pid": os.getpid(),
        "process_start": {"value": "fixture", "source": "os"},
        "instance_id": args.runtime_identity,
        "launch_id": os.environ.get("JCODEMUNCH_LAUNCH_ID"),
    }
    ready = {
        "pid": os.getpid(),
        "host": args.host,
        "port": args.port,
        "runtime_identity": args.runtime_identity,
    }
    args.ready_file.parent.mkdir(parents=True, exist_ok=True)
    args.ready_file.write_text(json.dumps(ready, sort_keys=True), encoding="utf-8")

    stopped = threading.Event()

    def stop(*_signal_args: object) -> None:
        if not stopped.is_set():
            stopped.set()
            threading.Thread(target=server.shutdown, daemon=True).start()

    signal.signal(signal.SIGTERM, stop)
    if hasattr(signal, "SIGBREAK"):
        signal.signal(signal.SIGBREAK, stop)
    if args.stop_file is not None:
        def watch_stop_file() -> None:
            while not stopped.is_set():
                if args.stop_file.is_file():
                    stop()
                    return
                time.sleep(0.02)

        threading.Thread(target=watch_stop_file, daemon=True).start()
    try:
        server.serve_forever(poll_interval=0.05)
    finally:
        server.server_close()
        args.ready_file.unlink(missing_ok=True)
        args.identity_file.unlink(missing_ok=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
