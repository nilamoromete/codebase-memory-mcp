from __future__ import annotations

import base64
import hashlib
import json
import os
import socket
import subprocess
import time
import tomllib
from pathlib import Path
from urllib.error import HTTPError
from urllib.request import Request, urlopen

import pytest


REPO_ROOT = Path(__file__).parents[2]
LIFECYCLE_SCRIPT = (
    REPO_ROOT / "integrations" / "jcodemunch" / "jcodemunch-lifecycle.ps1"
)


@pytest.fixture(autouse=True)
def _stop_fake_runtime_after_test(tmp_path: Path):
    """Never leave a disposable fixture server running after a test."""
    yield

    state_root = tmp_path / "state"
    owner_path = state_root / "owner.json"
    if owner_path.exists():
        _run_manager(tmp_path, "clean", "-Now")
    runtime_markers = (
        state_root / "runtime.ready.json",
        state_root / "runtime.identity.json",
    )
    if not any(marker.exists() for marker in runtime_markers):
        return

    (state_root / "runtime.stop").touch(exist_ok=True)
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline and any(
        marker.exists() for marker in runtime_markers
    ):
        time.sleep(0.05)

    remaining = [str(marker) for marker in runtime_markers if marker.exists()]
    assert not remaining, f"fake jCodeMunch runtime did not stop: {remaining}"


def _run_manager(tmp_path: Path, command: str, *arguments: str, env_overrides: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    state_root = tmp_path / "state"
    config_root = tmp_path / "configs"
    state_root.mkdir(exist_ok=True)
    config_root.mkdir(exist_ok=True)
    pi_artifact = (
        state_root
        / "runtimes"
        / "pi-mcp-extension"
        / "1.5.0"
        / "pi-mcp-extension-1.5.0.tgz"
    )
    pi_artifact.parent.mkdir(parents=True, exist_ok=True)
    pi_bytes = b"disposable-pi-adapter-fixture"
    if not pi_artifact.exists():
        pi_artifact.write_bytes(pi_bytes)
    pi_integrity = "sha512-" + base64.b64encode(hashlib.sha512(pi_bytes).digest()).decode("ascii")
    manager_arguments = list(arguments)
    if command in {"acquire", "heartbeat"} and "-ClientPid" not in manager_arguments:
        manager_arguments.extend(("-ClientPid", str(os.getpid())))

    return subprocess.run(
        [
            "pwsh",
            "-NoProfile",
            "-NonInteractive",
            "-File",
            str(LIFECYCLE_SCRIPT),
            "-Command",
            command,
            "-StateRoot",
            str(state_root),
            "-ConfigRoot",
            str(config_root),
            "-Json",
            *manager_arguments,
        ],
        check=False,
        capture_output=True,
        text=True,
        env={
            **os.environ,
            "JCODEMUNCH_LIFECYCLE_TEST_MODE": "1",
            "JCODEMUNCH_TEST_PI_INTEGRITY": pi_integrity,
            **(env_overrides or {}),
        },
    )


def _payload(result: subprocess.CompletedProcess[str]) -> dict:
    assert result.stdout, result.stderr
    return json.loads(result.stdout)


def _hold(port: int, family: socket.AddressFamily) -> socket.socket:
    listener = socket.socket(family, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    host = "127.0.0.1" if family is socket.AF_INET else "::1"
    listener.bind((host, port))
    listener.listen(1)
    return listener


def _free_port(family: socket.AddressFamily) -> int:
    listener = socket.socket(family, socket.SOCK_STREAM)
    host = "127.0.0.1" if family is socket.AF_INET else "::1"
    listener.bind((host, 0))
    port = listener.getsockname()[1]
    listener.close()
    return port


def _process_exists(pid: int) -> bool:
    result = subprocess.run(
        [
            "pwsh",
            "-NoProfile",
            "-NonInteractive",
            "-Command",
            (
                f"if (Get-Process -Id {pid} -ErrorAction SilentlyContinue) "
                "{ 'live' } else { 'stopped' }"
            ),
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    return result.stdout.strip() == "live"


def test_configure_selects_and_persists_a_safe_non_dynamic_port(tmp_path: Path) -> None:
    result = _run_manager(
        tmp_path,
        "configure",
        "-CandidatePorts",
        "45123,45124",
        "-DenylistedPorts",
        "45123",
        "-DynamicRanges",
        "49152-65535",
        "-ExcludedRanges",
        "49152-49160",
    )

    assert result.returncode == 0, result.stderr
    payload = _payload(result)
    assert payload["endpoint"]["host"] == "127.0.0.1"
    assert payload["endpoint"]["port"] == 45124
    assert payload["endpoint"]["port"] < 49152
    assert payload["endpoint"]["port"] not in {45123, 49152}
    assert Path(payload["state_path"]).is_file()


@pytest.mark.parametrize("family", [socket.AF_INET, socket.AF_INET6])
def test_configure_rejects_an_occupied_loopback_port(
    tmp_path: Path, family: socket.AddressFamily
) -> None:
    port = _free_port(family)
    listener = _hold(port, family)
    try:
        result = _run_manager(tmp_path, "configure", "-CandidatePorts", str(port))
    finally:
        listener.close()

    assert result.returncode != 0
    payload = _payload(result)
    assert payload["error"]["code"] == "port_unavailable"
    assert payload["error"]["port"] == port



@pytest.mark.parametrize(
    ("candidate", "options", "reason"),
    [
        ("0", {}, "invalid_port"),
        ("1023", {}, "privileged"),
        ("65536", {}, "invalid_port"),
        ("45130", {"-ReservedRanges": "45130-45130"}, "reserved"),
        ("45131", {"-DenylistedPorts": "45131"}, "denylisted"),
        ("45132", {"-ExcludedRanges": "45132-45132"}, "excluded_range"),
        ("49152", {"-DynamicRanges": "49152-65535"}, "dynamic_range"),
    ],
)
def test_configure_rejects_unsafe_candidates_with_structured_reasons(
    tmp_path: Path, candidate: str, options: dict[str, str], reason: str
) -> None:
    arguments = ["-CandidatePorts", candidate]
    for key, value in options.items():
        arguments.extend([key, value])
    result = _run_manager(tmp_path, "configure", *arguments)

    assert result.returncode != 0
    payload = _payload(result)
    assert payload["error"]["code"] == "port_unavailable"
    assert payload["error"]["rejections"][0]["reason"] == reason
    assert not (tmp_path / "state" / "port.json").exists()


def test_configure_fails_closed_when_range_discovery_is_unavailable(tmp_path: Path) -> None:
    result = _run_manager(
        tmp_path,
        "configure",
        "-CandidatePorts",
        "45133",
        env_overrides={"JCODEMUNCH_RANGE_DISCOVERY": "unavailable"},
    )

    assert result.returncode != 0
    payload = _payload(result)
    assert payload["error"]["code"] == "range_discovery_unavailable"
    assert not (tmp_path / "state" / "port.json").exists()


def test_configure_fails_closed_on_malformed_range_without_persisting_state(tmp_path: Path) -> None:
    result = _run_manager(
        tmp_path,
        "configure",
        "-CandidatePorts",
        "45134",
        "-DynamicRanges",
        "not-a-range",
    )

    assert result.returncode != 0
    payload = _payload(result)
    assert payload["error"]["code"] == "invalid_range"
    assert not (tmp_path / "state" / "port.json").exists()
def test_acquire_fails_closed_when_persisted_port_is_foreign(tmp_path: Path) -> None:
    configured = _run_manager(tmp_path, "configure", "-CandidatePorts", "45125")
    assert configured.returncode == 0, configured.stderr

    listener = _hold(45125, socket.AF_INET)
    try:
        result = _run_manager(tmp_path, "acquire", "-Client", "codex", "-SessionId", "s1")
        assert listener.fileno() != -1
    finally:
        listener.close()

    assert result.returncode != 0
    payload = _payload(result)
    assert payload["error"]["code"] == "port_collision"
    assert payload["error"]["port"] == 45125
    assert payload["error"]["action"] == "reselect-port"


def test_explicit_reselection_moves_off_foreign_persisted_port(tmp_path: Path) -> None:
    configured = _run_manager(tmp_path, "configure", "-CandidatePorts", "45125,45127")
    assert configured.returncode == 0, configured.stderr

    listener = _hold(45125, socket.AF_INET)
    try:
        collision = _run_manager(
            tmp_path, "acquire", "-Client", "codex", "-SessionId", "s1"
        )
        assert collision.returncode != 0

        reselected = _run_manager(
            tmp_path,
            "configure",
            "-ReselectPort",
            "-CandidatePorts",
            "45125,45127",
        )
        assert reselected.returncode == 0, reselected.stderr
        payload = _payload(reselected)
        assert payload["endpoint"]["port"] == 45127
        assert listener.fileno() != -1
    finally:
        listener.close()


@pytest.mark.parametrize("field", ["pid", "start_time", "command_fingerprint", "port", "runtime_identity", "launch_id"])
def test_stop_refuses_partial_ownership_matches(tmp_path: Path, field: str) -> None:
    configured = _run_manager(tmp_path, "configure", "-CandidatePorts", "45126")
    assert configured.returncode == 0, configured.stderr

    result = _run_manager(
        tmp_path,
        "stop",
        "-OwnershipFixture",
        field,
    )
    assert result.returncode != 0
    payload = _payload(result)
    assert payload["error"]["code"] == "ownership_mismatch"
    assert payload["error"]["field"] == field
    assert payload["process_action"] == "none"


def test_acquire_is_idempotent_and_shared_across_three_clients(tmp_path: Path) -> None:
    first = _run_manager(tmp_path, "acquire", "-Client", "codex", "-SessionId", "codex-1")
    second = _run_manager(tmp_path, "acquire", "-Client", "codex", "-SessionId", "codex-1")
    third = _run_manager(tmp_path, "acquire", "-Client", "claude", "-SessionId", "claude-1")
    fourth = _run_manager(tmp_path, "acquire", "-Client", "pi", "-SessionId", "pi-1")

    for result in (first, second, third, fourth):
        assert result.returncode == 0, result.stderr

    payloads = [_payload(result) for result in (first, second, third, fourth)]
    assert {payload["server"]["pid"] for payload in payloads}.__len__() == 1
    assert payloads[1]["idempotent"] is True
    assert {lease["client"] for lease in payloads[-1]["leases"]} == {"codex", "claude", "pi"}


def test_tampered_launch_id_refuses_stop_without_killing_runtime(tmp_path: Path) -> None:
    acquired = _run_manager(tmp_path, "acquire", "-Client", "codex", "-SessionId", "launch-proof")
    assert acquired.returncode == 0, acquired.stderr
    owner_path = tmp_path / "state" / "owner.json"
    owner = json.loads(owner_path.read_text(encoding="utf-8"))
    pid = owner["pid"]
    owner["launch_id"] = "tampered-launch-id"
    owner_path.write_text(json.dumps(owner), encoding="utf-8")

    refused = _run_manager(tmp_path, "stop")
    assert refused.returncode != 0
    payload = _payload(refused)
    assert payload["error"]["code"] == "ownership_mismatch"
    assert "launch_id" in payload["error"]["failed_checks"]
    assert _process_exists(pid)


def test_monitor_failure_cleans_newly_started_exact_runtime(tmp_path: Path) -> None:
    failed = _run_manager(
        tmp_path,
        "acquire",
        "-Client",
        "codex",
        "-SessionId",
        "monitor-failure",
        env_overrides={"JCODEMUNCH_TEST_MONITOR_FAILURE": "1"},
    )
    assert failed.returncode != 0
    assert _payload(failed)["error"]["code"] == "monitor_start_failed"
    assert not (tmp_path / "state" / "owner.json").exists()
    assert not (tmp_path / "state" / "runtime.ready.json").exists()


def test_acquire_uses_live_client_pid_for_lease_ownership(tmp_path: Path) -> None:
    acquired = _run_manager(
        tmp_path,
        "acquire",
        "-Client",
        "codex",
        "-SessionId",
        "codex-live-pid",
        "-ClientPid",
        str(os.getpid()),
    )
    assert acquired.returncode == 0, acquired.stderr

    lease = _payload(acquired)["leases"][0]
    assert lease["pid"] == os.getpid()
    assert lease["process_start_time"].startswith("ticks:")

    cleaned = _run_manager(tmp_path, "clean", "-Now")
    assert cleaned.returncode == 0, cleaned.stderr
    assert _payload(cleaned)["server_action"] == "keep"
    assert _payload(cleaned)["remaining_leases"] == 1


def test_same_client_session_in_different_workspaces_has_independent_leases(
    tmp_path: Path,
) -> None:
    workspace_a = tmp_path / "client-a" / "app"
    workspace_b = tmp_path / "client-b" / "app"
    workspace_a.mkdir(parents=True)
    workspace_b.mkdir(parents=True)
    configured = _run_manager(
        tmp_path, "configure", "-CandidatePorts", "45200"
    )
    assert configured.returncode == 0, configured.stderr

    first = _run_manager(
        tmp_path,
        "acquire",
        "-Client",
        "codex",
        "-SessionId",
        "shared-session",
        "-WorkspaceRoot",
        str(workspace_a),
    )
    second = _run_manager(
        tmp_path,
        "acquire",
        "-Client",
        "codex",
        "-SessionId",
        "shared-session",
        "-WorkspaceRoot",
        str(workspace_b),
    )

    assert first.returncode == 0, first.stderr
    assert second.returncode == 0, second.stderr
    payload = _payload(second)
    assert payload["idempotent"] is False
    assert len(payload["leases"]) == 2
    assert len({lease["workspace_id"] for lease in payload["leases"]}) == 2
    assert {
        Path(lease["workspace_root"]).resolve() for lease in payload["leases"]
    } == {workspace_a.resolve(), workspace_b.resolve()}

    released = _run_manager(
        tmp_path,
        "release",
        "-Client",
        "codex",
        "-SessionId",
        "shared-session",
        "-WorkspaceRoot",
        str(workspace_a),
    )
    assert released.returncode == 0, released.stderr
    assert _payload(released)["remaining_leases"] == 1

    status = _run_manager(tmp_path, "status")
    assert status.returncode == 0, status.stderr
    assert [
        Path(lease["workspace_root"]).resolve()
        for lease in _payload(status)["leases"]
    ] == [workspace_b.resolve()]

    final_release = _run_manager(
        tmp_path,
        "release",
        "-Client",
        "codex",
        "-SessionId",
        "shared-session",
        "-WorkspaceRoot",
        str(workspace_b),
        "-GraceSeconds",
        "0",
    )
    assert final_release.returncode == 0, final_release.stderr
    stopped = _run_manager(tmp_path, "clean", "-Now")
    assert stopped.returncode == 0, stopped.stderr


def test_acquire_records_complete_owned_runtime_tuple(tmp_path: Path) -> None:
    acquired = _run_manager(tmp_path, "acquire", "-Client", "codex", "-SessionId", "tuple-1")
    assert acquired.returncode == 0, acquired.stderr

    owner = json.loads((tmp_path / "state" / "owner.json").read_text(encoding="utf-8"))
    assert owner["pid"] > 0
    assert owner["process_start_time"].startswith("ticks:")
    assert owner["port"] == _payload(acquired)["server"]["port"]
    assert owner["runtime_identity"]
    assert owner["command_fingerprint"]
    assert owner["runtime_version"] == "1.108.291"
    assert len(owner["executable_sha256"]) == 64
    assert owner["executable_path"] or owner["command_line"]


def test_runtime_requires_the_generated_bearer_token(tmp_path: Path) -> None:
    acquired = _run_manager(
        tmp_path, "acquire", "-Client", "codex", "-SessionId", "auth-1"
    )
    assert acquired.returncode == 0, acquired.stderr
    endpoint = _payload(acquired)["endpoint"]
    token_document = json.loads(
        (tmp_path / "state" / "secrets" / "token.json").read_text(
            encoding="utf-8"
        )
    )

    with pytest.raises(HTTPError) as unauthorized:
        urlopen(f"{endpoint}/health", timeout=2)
    assert unauthorized.value.code == 401

    request = Request(
        f"{endpoint}/health",
        headers={"Authorization": f"Bearer {token_document['token']}"},
    )
    with urlopen(request, timeout=2) as response:
        assert response.status == 200


def test_runtime_listener_child_is_owned_through_verified_launcher(tmp_path: Path) -> None:
    acquired = _run_manager(
        tmp_path,
        "acquire",
        "-Client",
        "codex",
        "-SessionId",
        "child-runtime",
        env_overrides={"JCODEMUNCH_TEST_CHILD_PROCESS": "1"},
    )
    assert acquired.returncode == 0, acquired.stderr

    owner = _payload(acquired)["server"]
    assert owner["pid"] != owner["launcher_pid"]
    assert owner["process_start_time"].startswith("ticks:")
    assert owner["launcher_process_start_time"].startswith("ticks:")

    stopped = _run_manager(tmp_path, "stop")
    assert stopped.returncode == 0, stopped.stderr
    assert _payload(stopped)["process_action"] == "stopped"


def test_failed_child_runtime_bootstrap_leaves_no_process_or_listener(
    tmp_path: Path,
) -> None:
    port = _free_port(socket.AF_INET)
    pid_log = tmp_path / "failed-bootstrap-pids.txt"

    failed = _run_manager(
        tmp_path,
        "configure",
        "-CandidatePorts",
        str(port),
        env_overrides={
            "JCODEMUNCH_TEST_CHILD_PROCESS": "1",
            "JCODEMUNCH_TEST_PROBE_FAILURE": "1",
            "JCODEMUNCH_TEST_PID_LOG": str(pid_log),
        },
    )

    assert failed.returncode != 0
    assert _payload(failed)["error"]["code"] == "port_unavailable"
    child_pids = [int(line) for line in pid_log.read_text().splitlines() if line]
    assert child_pids
    assert all(not _process_exists(pid) for pid in child_pids)
    listener = _hold(port, socket.AF_INET)
    listener.close()
    assert not (tmp_path / "state" / "runtime.ready.json").exists()
    assert not (tmp_path / "state" / "runtime.identity.json").exists()


def test_release_preserves_server_until_final_lease_and_grace_expiry(tmp_path: Path) -> None:
    for client, session in (("codex", "c1"), ("claude", "c1"), ("pi", "p1")):
        result = _run_manager(tmp_path, "acquire", "-Client", client, "-SessionId", session)
        assert result.returncode == 0, result.stderr

    one_release = _run_manager(tmp_path, "release", "-Client", "codex", "-SessionId", "c1")
    assert one_release.returncode == 0, one_release.stderr
    assert _payload(one_release)["server_action"] == "keep"

    second_release = _run_manager(
        tmp_path,
        "release",
        "-Client",
        "claude",
        "-SessionId",
        "c1",
    )
    assert second_release.returncode == 0, second_release.stderr
    assert _payload(second_release)["remaining_leases"] == 1

    final_release = _run_manager(
        tmp_path,
        "release",
        "-Client",
        "pi",
        "-SessionId",
        "p1",
        "-GraceSeconds",
        "30",
    )
    assert final_release.returncode == 0, final_release.stderr
    assert _payload(final_release)["server_action"] == "keep"

    stopped = _run_manager(tmp_path, "clean", "-Now")
    assert stopped.returncode == 0, stopped.stderr
    assert _payload(stopped)["server_action"] == "stop"


def test_acquire_during_grace_cancels_pending_shutdown(tmp_path: Path) -> None:
    acquired = _run_manager(tmp_path, "acquire", "-Client", "codex", "-SessionId", "c1")
    assert acquired.returncode == 0, acquired.stderr
    released = _run_manager(tmp_path, "release", "-Client", "codex", "-SessionId", "c1")
    assert released.returncode == 0, released.stderr

    renewed = _run_manager(tmp_path, "acquire", "-Client", "pi", "-SessionId", "p1")
    assert renewed.returncode == 0, renewed.stderr
    assert _payload(renewed)["shutdown_cancelled"] is True


def test_final_release_watcher_stops_runtime_after_grace(tmp_path: Path) -> None:
    configured = _run_manager(
        tmp_path, "configure", "-CandidatePorts", "45201"
    )
    assert configured.returncode == 0, configured.stderr
    acquired = _run_manager(
        tmp_path, "acquire", "-Client", "codex", "-SessionId", "watch-stop"
    )
    assert acquired.returncode == 0, acquired.stderr

    released = _run_manager(
        tmp_path,
        "release",
        "-Client",
        "codex",
        "-SessionId",
        "watch-stop",
        "-GraceSeconds",
        "1",
    )
    assert released.returncode == 0, released.stderr
    watcher_pid = _payload(released)["watcher"]["pid"]
    assert watcher_pid > 0

    # Full exact-owner validation includes Windows process metadata, binary
    # hashing, and an authenticated MCP identity probe. Under CI/load that can
    # take longer than the one-second grace without indicating a stuck watcher.
    deadline = time.monotonic() + 30
    owner_path = tmp_path / "state" / "owner.json"
    while time.monotonic() < deadline and (
        owner_path.exists() or _process_exists(watcher_pid)
    ):
        time.sleep(0.1)

    assert not owner_path.exists()
    assert not _process_exists(watcher_pid)
    assert not (tmp_path / "state" / "runtime.ready.json").exists()


def test_acquire_during_grace_terminates_cancelled_watcher(tmp_path: Path) -> None:
    configured = _run_manager(
        tmp_path, "configure", "-CandidatePorts", "45202"
    )
    assert configured.returncode == 0, configured.stderr
    acquired = _run_manager(
        tmp_path, "acquire", "-Client", "codex", "-SessionId", "watch-cancel"
    )
    assert acquired.returncode == 0, acquired.stderr
    owner_pid = _payload(acquired)["server"]["pid"]
    released = _run_manager(
        tmp_path,
        "release",
        "-Client",
        "codex",
        "-SessionId",
        "watch-cancel",
        "-GraceSeconds",
        "30",
    )
    assert released.returncode == 0, released.stderr
    watcher_pid = _payload(released)["watcher"]["pid"]
    assert watcher_pid > 0

    renewed = _run_manager(
        tmp_path, "acquire", "-Client", "pi", "-SessionId", "watch-renew"
    )
    assert renewed.returncode == 0, renewed.stderr
    assert _payload(renewed)["shutdown_cancelled"] is True

    deadline = time.monotonic() + 3
    while time.monotonic() < deadline and _process_exists(watcher_pid):
        time.sleep(0.1)

    assert not _process_exists(watcher_pid)
    assert _payload(renewed)["server"]["pid"] == owner_pid
    assert (tmp_path / "state" / "runtime.ready.json").exists()
    stopped = _run_manager(tmp_path, "stop")
    assert stopped.returncode == 0, stopped.stderr


def test_dead_client_lease_is_reaped_and_runtime_stops_after_grace(
    tmp_path: Path,
) -> None:
    # This test exercises lease reconciliation, not the default port pool.
    # Give it an isolated candidate set so an earlier fixture or unrelated
    # listener on the first global default cannot mask the lifecycle contract.
    configured = _run_manager(
        tmp_path,
        "configure",
        "-CandidatePorts",
        ",".join(str(port) for port in range(45300, 45310)),
    )
    assert configured.returncode == 0, configured.stderr

    client = subprocess.Popen(
        ["pwsh", "-NoProfile", "-NonInteractive", "-Command", "Start-Sleep -Seconds 60"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    try:
        acquired = _run_manager(
            tmp_path,
            "acquire",
            "-Client",
            "codex",
            "-SessionId",
            "abrupt-client",
            "-ClientPid",
            str(client.pid),
            "-GraceSeconds",
            "1",
        )
        assert acquired.returncode == 0, acquired.stderr
        payload = _payload(acquired)
        owner_pid = payload["server"]["pid"]
        monitor_pid = payload["monitor"]["pid"]
        assert _process_exists(owner_pid)
        assert _process_exists(monitor_pid)

        client.terminate()
        client.wait(timeout=5)
        deadline = time.monotonic() + 12
        owner_path = tmp_path / "state" / "owner.json"
        while time.monotonic() < deadline and (
            owner_path.exists()
            or _process_exists(owner_pid)
            or _process_exists(monitor_pid)
        ):
            time.sleep(0.1)

        assert not owner_path.exists()
        assert not _process_exists(owner_pid)
        assert not _process_exists(monitor_pid)
        leases = json.loads((tmp_path / "state" / "leases.json").read_text())
        assert leases["leases"] == []
    finally:
        if client.poll() is None:
            client.terminate()
            client.wait(timeout=5)


def test_rollback_restores_all_client_config_bytes(tmp_path: Path) -> None:
    configs = {}
    for name, content in {
        "codex.toml": b"[mcp_servers]\nexisting = true\n",
        "claude.json": b'{"hooks":{"existing":[]},"keep":true}\n',
        "pi.json": b'{"extensions":["existing"],"keep":true}\n',
    }.items():
        path = tmp_path / name
        path.write_bytes(content)
        configs[name] = (path, content)

    configured = _run_manager(
        tmp_path,
        "configure",
        "-CandidatePorts",
        "45127",
        "-CodexConfig",
        str(configs["codex.toml"][0]),
        "-ClaudeConfig",
        str(configs["claude.json"][0]),
        "-PiConfig",
        str(configs["pi.json"][0]),
    )
    assert configured.returncode == 0, configured.stderr
    rollback = _run_manager(tmp_path, "rollback")
    assert rollback.returncode == 0, rollback.stderr
    assert _payload(rollback)["restored"] == 3

    for path, original in configs.values():
        assert path.read_bytes() == original


def _client_config_arguments(tmp_path: Path) -> tuple[list[str], dict[str, Path]]:
    configs = tmp_path / "client-configs"
    codex = configs / "codex" / "config.toml"
    claude = configs / "claude" / "global.json"
    claude_settings = configs / "claude" / "settings.json"
    pi_settings = configs / "pi" / "settings.json"
    pi_mcp = configs / "pi" / "mcp.json"
    integration = tmp_path / "installed-integration"
    for path in (codex, claude, claude_settings, pi_settings):
        path.parent.mkdir(parents=True, exist_ok=True)
    codex.write_text(
        '[model_providers.keep]\nname = "preserved"\n\n'
        '[mcp_servers.jcodemunch]\ncommand = "legacy.exe"\n\n'
        '[[profiles]]\nname = "preserved-array-entry"\n',
        encoding="utf-8",
    )
    claude.write_text(
        json.dumps({"keep": True, "mcpServers": {"other": {"command": "other"}}}),
        encoding="utf-8",
    )
    claude_settings.write_text(
        json.dumps(
            {
                "keep": True,
                "hooks": {
                    "SessionStart": [
                        {"hooks": [{"type": "command", "command": "existing-start"}]}
                    ]
                },
            }
        ),
        encoding="utf-8",
    )
    pi_settings.write_text(
        json.dumps(
            {
                "keep": True,
                "packages": [
                    "",
                    None,
                    "npm:existing@1.0.0",
                    r"C:\Users\fixture\Aliat\jcodemunch\integration\jcodemunch_lifecycle\pi",
                ],
                "extensions": [None, "existing.ts", "jcodemunch-lifecycle.ts"],
            }
        ),
        encoding="utf-8",
    )
    paths = {
        "codex": codex,
        "claude": claude,
        "claude_settings": claude_settings,
        "pi_settings": pi_settings,
        "pi_mcp": pi_mcp,
        "integration": integration,
    }
    arguments = [
        "-CandidatePorts",
        "45129",
        "-ApplyClientConfigs",
        "-CodexConfig",
        str(codex),
        "-ClaudeConfig",
        str(claude),
        "-ClaudeSettingsConfig",
        str(claude_settings),
        "-PiConfig",
        str(pi_mcp),
        "-PiSettingsConfig",
        str(pi_settings),
        "-IntegrationRoot",
        str(integration),
        "-SourceAssetPaths",
        "jcodemunch-lifecycle.ps1,jcodemunch-bridge.ps1,proxy/package-lock.json",
    ]
    return arguments, paths


def test_reselection_after_client_cutover_requires_full_transaction(
    tmp_path: Path,
) -> None:
    arguments, paths = _client_config_arguments(tmp_path)
    configured = _run_manager(tmp_path, "configure", *arguments)
    assert configured.returncode == 0, configured.stderr

    state_root = tmp_path / "state"
    port_path = state_root / "port.json"
    backups_path = state_root / "backups.json"
    original_port = port_path.read_bytes()
    original_backups = backups_path.read_bytes()
    original_configs = {
        key: path.read_bytes()
        for key, path in paths.items()
        if key != "integration" and path.is_file()
    }

    reselected = _run_manager(
        tmp_path,
        "configure",
        "-ReselectPort",
        "-CandidatePorts",
        "45130",
    )

    assert reselected.returncode != 0
    assert _payload(reselected)["error"]["code"] == (
        "reselection_requires_client_transaction"
    )
    assert port_path.read_bytes() == original_port
    assert backups_path.read_bytes() == original_backups
    for key, original in original_configs.items():
        assert paths[key].read_bytes() == original


def test_failed_transactional_reselection_restores_last_valid_cutover(
    tmp_path: Path,
) -> None:
    arguments, paths = _client_config_arguments(tmp_path)
    configured = _run_manager(tmp_path, "configure", *arguments)
    assert configured.returncode == 0, configured.stderr

    state_root = tmp_path / "state"
    port_path = state_root / "port.json"
    backups_path = state_root / "backups.json"
    original_port = port_path.read_bytes()
    original_backups = backups_path.read_bytes()
    original_configs = {
        key: path.read_bytes()
        for key, path in paths.items()
        if key != "integration" and path.is_file()
    }
    reselection_arguments = list(arguments)
    reselection_arguments[1] = "45130"

    failed = _run_manager(
        tmp_path,
        "configure",
        "-ReselectPort",
        *reselection_arguments,
        "-FailureInjectionAfterWrites",
        "3",
    )

    assert failed.returncode != 0
    failure_payload = _payload(failed)
    assert failure_payload["error"]["code"] == "config_transaction_failed", failure_payload
    assert port_path.read_bytes() == original_port
    assert backups_path.read_bytes() == original_backups
    for key, original in original_configs.items():
        assert paths[key].read_bytes() == original


def test_configure_merges_client_configs_and_preserves_unrelated_values(
    tmp_path: Path,
) -> None:
    arguments, paths = _client_config_arguments(tmp_path)
    result = _run_manager(tmp_path, "configure", *arguments)
    assert result.returncode == 0, result.stderr

    token = json.loads(
        (tmp_path / "state" / "secrets" / "token.json").read_text(encoding="utf-8")
    )["token"]
    bridge = str(paths["integration"] / "jcodemunch-bridge.ps1")

    codex = tomllib.loads(paths["codex"].read_text(encoding="utf-8"))
    assert codex["model_providers"]["keep"]["name"] == "preserved"
    assert codex["profiles"] == [{"name": "preserved-array-entry"}]
    assert codex["mcp_servers"]["jcodemunch"] == {
        "command": "pwsh",
        "args": [
            "-NoProfile",
            "-NonInteractive",
            "-File",
            bridge,
            "-Client",
            "codex",
        ],
        "enabled": True,
        "startup_timeout_sec": 60,
        "tool_timeout_sec": 300,
    }

    claude = json.loads(paths["claude"].read_text(encoding="utf-8"))
    assert claude["keep"] is True
    assert claude["mcpServers"]["other"] == {"command": "other"}
    assert claude["mcpServers"]["jcodemunch"] == {
        "type": "stdio",
        "command": "pwsh",
        "args": [
            "-NoProfile",
            "-NonInteractive",
            "-File",
            bridge,
            "-Client",
            "claude",
        ],
    }

    claude_settings = json.loads(
        paths["claude_settings"].read_text(encoding="utf-8")
    )
    assert claude_settings["keep"] is True
    assert claude_settings["hooks"]["SessionStart"][0]["hooks"][0]["command"] == (
        "existing-start"
    )
    hook_text = json.dumps(claude_settings["hooks"])
    assert "claude-session.ps1" not in hook_text

    pi_settings = json.loads(paths["pi_settings"].read_text(encoding="utf-8"))
    assert pi_settings["keep"] is True
    assert pi_settings["packages"][0] == "npm:existing@1.0.0"
    assert pi_settings["packages"][1].startswith("file:")
    assert pi_settings["packages"][1].endswith("/pi-mcp-extension-1.5.0.tgz")
    assert pi_settings["extensions"] == ["existing.ts"]

    pi_mcp = json.loads(paths["pi_mcp"].read_text(encoding="utf-8"))
    assert pi_mcp["mcpServers"]["jcodemunch"] == {
        "transport": "stdio",
        "command": "pwsh",
        "args": [
            "-NoProfile",
            "-NonInteractive",
            "-File",
            bridge,
            "-Client",
            "pi",
        ],
        "lifecycle": "eager",
    }
    assert (paths["integration"] / "jcodemunch-lifecycle.ps1").is_file()
    assert (paths["integration"] / "jcodemunch-bridge.ps1").is_file()
    assert (paths["integration"] / "proxy" / "package-lock.json").is_file()
    serialized = "\n".join(
        path.read_text(encoding="utf-8")
        for path in (
            paths["codex"],
            paths["claude"],
            paths["claude_settings"],
            paths["pi_settings"],
            paths["pi_mcp"],
        )
    )
    assert token not in serialized


def test_quoted_codex_jcodemunch_table_fails_without_duplicate_or_byte_change(
    tmp_path: Path,
) -> None:
    arguments, paths = _client_config_arguments(tmp_path)
    quoted = (
        '[mcp_servers."jcodemunch"]\n'
        'command = "existing"\n'
        'args = []\n'
    ).encode()
    paths["codex"].write_bytes(quoted)
    failed = _run_manager(tmp_path, "configure", *arguments)
    assert failed.returncode != 0
    assert _payload(failed)["error"]["code"] == "config_transaction_failed"
    assert paths["codex"].read_bytes() == quoted


def test_config_transaction_failure_restores_existing_and_removes_new_files(
    tmp_path: Path,
) -> None:
    arguments, paths = _client_config_arguments(tmp_path)
    originals = {
        key: path.read_bytes()
        for key, path in paths.items()
        if key != "integration" and path.is_file()
    }

    failed = _run_manager(
        tmp_path,
        "configure",
        *arguments,
        "-FailureInjectionAfterWrites",
        "3",
    )
    assert failed.returncode != 0
    assert _payload(failed)["error"]["code"] == "config_transaction_failed"
    for key, original in originals.items():
        assert paths[key].read_bytes() == original
    assert not paths["pi_mcp"].exists()
    assert not any(paths["integration"].rglob("*.*"))


def test_rollback_restores_exact_bytes_and_deletes_files_created_by_cutover(
    tmp_path: Path,
) -> None:
    arguments, paths = _client_config_arguments(tmp_path)
    originals = {
        key: path.read_bytes()
        for key, path in paths.items()
        if key != "integration" and path.is_file()
    }

    configured = _run_manager(tmp_path, "configure", *arguments)
    assert configured.returncode == 0, configured.stderr
    rollback = _run_manager(tmp_path, "rollback")
    assert rollback.returncode == 0, rollback.stderr

    payload = _payload(rollback)
    assert payload["failed"] == 0
    for key, original in originals.items():
        assert paths[key].read_bytes() == original
    assert not paths["pi_mcp"].exists()
    assert not any(paths["integration"].rglob("*.*"))


def test_new_cutover_after_rollback_captures_a_fresh_baseline(tmp_path: Path) -> None:
    arguments, paths = _client_config_arguments(tmp_path)
    configured = _run_manager(tmp_path, "configure", *arguments)
    assert configured.returncode == 0, configured.stderr
    first_rollback = _run_manager(tmp_path, "rollback")
    assert first_rollback.returncode == 0, first_rollback.stderr

    modified_baseline = paths["codex"].read_bytes() + b'\n[operator]\nkeep = "new"\n'
    paths["codex"].write_bytes(modified_baseline)
    second_arguments = list(arguments)
    second_arguments[1] = "45130"

    reconfigured = _run_manager(
        tmp_path,
        "configure",
        "-ReselectPort",
        *second_arguments,
    )
    assert reconfigured.returncode == 0, reconfigured.stderr
    second_rollback = _run_manager(tmp_path, "rollback")
    assert second_rollback.returncode == 0, second_rollback.stderr

    assert paths["codex"].read_bytes() == modified_baseline
    backups = json.loads((tmp_path / "state" / "backups.json").read_text())
    assert backups["status"] == "rolled_back"


def test_rollback_refuses_live_leases_then_stops_idle_runtime(tmp_path: Path) -> None:
    arguments, paths = _client_config_arguments(tmp_path)
    configured = _run_manager(tmp_path, "configure", *arguments)
    assert configured.returncode == 0, configured.stderr
    applied_configs = {
        key: path.read_bytes()
        for key, path in paths.items()
        if key != "integration" and path.is_file()
    }
    client = subprocess.Popen(
        ["pwsh", "-NoProfile", "-NonInteractive", "-Command", "Start-Sleep -Seconds 60"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    try:
        acquired = _run_manager(
            tmp_path,
            "acquire",
            "-Client",
            "codex",
            "-SessionId",
            "rollback-live",
            "-ClientPid",
            str(client.pid),
        )
        assert acquired.returncode == 0, acquired.stderr
        owner_pid = _payload(acquired)["server"]["pid"]

        refused = _run_manager(tmp_path, "rollback")
        assert refused.returncode != 0
        assert _payload(refused)["error"]["code"] == "rollback_requires_idle"
        assert _process_exists(owner_pid)
        for key, content in applied_configs.items():
            assert paths[key].read_bytes() == content

        released = _run_manager(
            tmp_path,
            "release",
            "-Client",
            "codex",
            "-SessionId",
            "rollback-live",
            "-GraceSeconds",
            "30",
        )
        assert released.returncode == 0, released.stderr
        watcher_pid = _payload(released)["watcher"]["pid"]
        rolled_back = _run_manager(tmp_path, "rollback")
        assert rolled_back.returncode == 0, rolled_back.stderr

        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and _process_exists(watcher_pid):
            time.sleep(0.1)
        assert not _process_exists(owner_pid)
        assert not _process_exists(watcher_pid)
        assert not (tmp_path / "state" / "owner.json").exists()
        assert not (tmp_path / "state" / "shutdown.json").exists()
        assert not (tmp_path / "state" / "monitor.json").exists()
        assert not any(paths["integration"].rglob("*.*"))
    finally:
        if client.poll() is None:
            client.terminate()
            client.wait(timeout=5)
        _run_manager(tmp_path, "clean", "-Now")


def test_status_is_useful_and_diagnostic_logs_redact_secret_material(tmp_path: Path) -> None:
    configured = _run_manager(tmp_path, "configure", "-CandidatePorts", "45128")
    assert configured.returncode == 0, configured.stderr

    token_path = tmp_path / "state" / "secrets" / "token.json"
    token = json.loads(token_path.read_text(encoding="utf-8"))["token"]
    status = _run_manager(tmp_path, "status")

    assert status.returncode == 0, status.stderr
    payload = _payload(status)
    assert payload["diagnostics"]["ownership_proof"]["checks"] == [
        "pid",
        "process_start_time",
        "executable_path",
        "executable_sha256",
        "runtime_version",
        "command_line",
        "command_fingerprint",
        "port",
        "runtime_identity",
        "launch_id",
    ]
    assert payload["diagnostics"]["token_store"] == "configured"
    assert payload["diagnostics"]["log_path"].endswith("diagnostics.jsonl")

    log_path = tmp_path / "state" / "diagnostics.jsonl"
    log_text = log_path.read_text(encoding="utf-8")
    assert "command_started" in log_text
    assert "command_succeeded" in log_text
    assert token not in configured.stdout
    assert token not in status.stdout
    assert token not in log_text


@pytest.mark.parametrize("field", ["process_start_time", "runtime_identity", "command_fingerprint"])
def test_stop_refuses_recorded_identity_mismatch(tmp_path: Path, field: str) -> None:
    acquired = _run_manager(tmp_path, "acquire", "-Client", "codex", "-SessionId", "ownership-mismatch")
    assert acquired.returncode == 0, acquired.stderr

    owner_path = tmp_path / "state" / "owner.json"
    owner = json.loads(owner_path.read_text(encoding="utf-8"))
    original = owner[field]
    owner[field] = f"foreign-{field}"
    owner_path.write_text(json.dumps(owner), encoding="utf-8")

    result = _run_manager(tmp_path, "stop")
    assert result.returncode != 0
    payload = _payload(result)
    assert payload["error"]["code"] == "ownership_mismatch"
    assert field in payload["error"]["failed_checks"]
    assert payload["process_action"] == "none"

    owner[field] = original
    owner_path.write_text(json.dumps(owner), encoding="utf-8")
    stopped = _run_manager(tmp_path, "stop")
    assert stopped.returncode == 0, stopped.stderr


def test_stop_refuses_recorded_executable_or_command_mismatch(tmp_path: Path) -> None:
    acquired = _run_manager(tmp_path, "acquire", "-Client", "codex", "-SessionId", "path-mismatch")
    assert acquired.returncode == 0, acquired.stderr

    owner_path = tmp_path / "state" / "owner.json"
    owner = json.loads(owner_path.read_text(encoding="utf-8"))
    identity_field = "executable_path" if owner.get("executable_path") else "command_line"
    original = owner[identity_field]
    owner[identity_field] = "C:/foreign/jcodemunch.exe"
    owner_path.write_text(json.dumps(owner), encoding="utf-8")

    result = _run_manager(tmp_path, "stop")
    assert result.returncode != 0
    payload = _payload(result)
    assert payload["error"]["code"] == "ownership_mismatch"
    assert identity_field in payload["error"]["failed_checks"]
    assert payload["process_action"] == "none"

    owner[identity_field] = original
    owner_path.write_text(json.dumps(owner), encoding="utf-8")
    stopped = _run_manager(tmp_path, "stop")
    assert stopped.returncode == 0, stopped.stderr
