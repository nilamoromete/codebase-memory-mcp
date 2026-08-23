from __future__ import annotations

import json
import socket
import subprocess
import sys
import time
from pathlib import Path
from urllib.request import urlopen


FIXTURE_ROOT = Path(__file__).parent / "fixtures"
FIXTURE_DATA = FIXTURE_ROOT / "windows-fixtures.json"
FAKE_RUNTIME = FIXTURE_ROOT / "fake_jcodemunch.py"


def _fixture_data() -> dict:
    return json.loads(FIXTURE_DATA.read_text(encoding="utf-8"))


def _wait_for(path: Path, timeout: float = 3.0) -> dict:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.is_file():
            return json.loads(path.read_text(encoding="utf-8"))
        time.sleep(0.02)
    raise AssertionError(f"fixture did not become ready: {path}")


def test_windows_range_fixture_is_deterministic_and_non_overlapping() -> None:
    data = _fixture_data()
    dynamic = data["dynamic_ranges"]
    excluded = data["excluded_ranges"]
    assert dynamic == [{"start": 49152, "end": 65535}]
    assert all(item["start"] <= item["end"] for item in dynamic + excluded)
    assert all(
        not (item["start"] <= dynamic[0]["end"] and item["end"] >= dynamic[0]["start"])
        for item in excluded
    )
    assert data["ports"]["candidate"] < dynamic[0]["start"]


def test_pid_reuse_fixture_changes_identity_without_changing_pid() -> None:
    data = _fixture_data()["process_identities"]
    owned = data["owned"]
    reused = data["pid_reused"]
    assert reused["pid"] == owned["pid"]
    assert reused["start_time"] != owned["start_time"]
    assert reused["command_fingerprint"] != owned["command_fingerprint"]
    assert reused["runtime_identity"] != owned["runtime_identity"]


def test_fake_runtime_identity_binds_loopback_and_is_queryable(tmp_path: Path) -> None:
    port = _free_port(socket.AF_INET)
    ready_file = tmp_path / "ready.json"
    identity_file = tmp_path / "identity.json"
    process = subprocess.Popen(
        [
            sys.executable,
            str(FAKE_RUNTIME),
            "--port",
            str(port),
            "--ready-file",
            str(ready_file),
            "--identity-file",
            str(identity_file),
            "--stop-file",
            str(tmp_path / "stop"),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        ready = _wait_for(ready_file)
        identity = _wait_for(identity_file)
        assert ready["pid"] == process.pid
        assert identity["pid"] == process.pid
        assert identity["bind_host"] == "127.0.0.1"
        assert identity["transport"] == "streamable-http"
        with urlopen(f"http://127.0.0.1:{port}/runtime-identity", timeout=2) as response:
            assert json.load(response) == identity
    finally:
        (tmp_path / "stop").touch()
        process.wait(timeout=3)
    assert not ready_file.exists()
    assert not identity_file.exists()


def test_fake_runtime_identity_rejects_occupied_port(tmp_path: Path) -> None:
    port = _free_port(socket.AF_INET)
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", port))
    listener.listen(1)
    try:
        result = subprocess.run(
            [
                sys.executable,
                str(FAKE_RUNTIME),
                "--port",
                str(port),
                "--ready-file",
                str(tmp_path / "ready.json"),
                "--identity-file",
                str(tmp_path / "identity.json"),
            ],
            check=False,
            capture_output=True,
            text=True,
        )
    finally:
        listener.close()
    assert result.returncode == 98
    assert json.loads(result.stderr)["error"] == "bind_failed"


def _free_port(family: socket.AddressFamily) -> int:
    host = "127.0.0.1" if family is socket.AF_INET else "::1"
    listener = socket.socket(family, socket.SOCK_STREAM)
    listener.bind((host, 0))
    port = listener.getsockname()[1]
    listener.close()
    return port
