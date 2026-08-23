from __future__ import annotations

import json
import os
import subprocess
import time
from pathlib import Path


REPO_ROOT = Path(__file__).parents[2]
INTEGRATION = REPO_ROOT / "integrations" / "jcodemunch"
BRIDGE = INTEGRATION / "jcodemunch-bridge.ps1"
LIFECYCLE = INTEGRATION / "jcodemunch-lifecycle.ps1"
FAKE_PROXY = Path(__file__).parent / "fixtures" / "fake_stdio_proxy.mjs"


def _prepare_proxy(tmp_path: Path, state_root: Path) -> tuple[dict[str, str], Path]:
    entrypoint = (
        state_root
        / "runtimes"
        / "mcp-remote"
        / "0.1.43"
        / "node_modules"
        / "mcp-remote"
        / "dist"
        / "proxy.js"
    )
    entrypoint.parent.mkdir(parents=True)
    entrypoint.write_bytes(FAKE_PROXY.read_bytes())
    observation = tmp_path / "proxy-observation.json"
    environment = {
        **os.environ,
        "JCODEMUNCH_LIFECYCLE_TEST_MODE": "1",
        "JCODEMUNCH_PROXY_OBSERVATION": str(observation),
    }
    return environment, observation


def _start_bridge(
    tmp_path: Path,
    state_root: Path,
    environment: dict[str, str],
    client: str,
    workspace: Path,
) -> subprocess.Popen[str]:
    return subprocess.Popen(
        [
            "pwsh",
            "-NoProfile",
            "-NonInteractive",
            "-File",
            str(BRIDGE),
            "-Client",
            client,
            "-SessionId",
            f"{client}-{workspace.name}",
            "-WorkspaceRoot",
            str(workspace),
            "-StateRoot",
            str(state_root),
            "-ConfigRoot",
            str(tmp_path / "configs"),
            "-ParentPid",
            str(os.getpid()),
            "-GraceSeconds",
            "0",
        ],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=environment,
    )


def _initialize(process: subprocess.Popen[str]) -> dict:
    assert process.stdin is not None
    assert process.stdout is not None
    process.stdin.write(
        json.dumps(
            {
                "jsonrpc": "2.0",
                "id": 1,
                "method": "initialize",
                "params": {
                    "protocolVersion": "2025-03-26",
                    "capabilities": {},
                    "clientInfo": {"name": "bridge-test", "version": "1"},
                },
            }
        )
        + "\n"
    )
    process.stdin.flush()
    return json.loads(process.stdout.readline())


def _request(process: subprocess.Popen[str], payload: dict) -> dict:
    assert process.stdin is not None
    assert process.stdout is not None
    process.stdin.write(json.dumps(payload) + "\n")
    process.stdin.flush()
    return json.loads(process.stdout.readline())


def _wait_until(predicate, timeout: float = 15) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.1)
    raise AssertionError("condition was not reached before timeout")


def _cleanup(state_root: Path, config_root: Path, environment: dict[str, str]) -> None:
    subprocess.run(
        [
            "pwsh",
            "-NoProfile",
            "-NonInteractive",
            "-File",
            str(LIFECYCLE),
            "-Command",
            "clean",
            "-StateRoot",
            str(state_root),
            "-ConfigRoot",
            str(config_root),
            "-Now",
            "-Json",
        ],
        capture_output=True,
        text=True,
        check=False,
        env=environment,
    )


def test_bridge_proxies_initialize_without_putting_token_in_argv(tmp_path: Path) -> None:
    state_root = tmp_path / "state"
    workspace = tmp_path / "project-codex"
    workspace.mkdir()
    environment, observation_path = _prepare_proxy(tmp_path, state_root)
    process = _start_bridge(tmp_path, state_root, environment, "codex", workspace)
    try:
        response = _initialize(process)
        assert response["result"]["serverInfo"]["name"] == "jcodemunch-mcp"
        observation = json.loads(observation_path.read_text(encoding="utf-8"))
        assert observation["transport"] == "http-only"
        assert observation["header_is_placeholder"] is True
        assert observation["token_present_in_environment"] is True
        assert observation["argv_contains_token"] is False
        leases = json.loads((state_root / "leases.json").read_text(encoding="utf-8"))["leases"]
        assert leases[0]["client"] == "codex"
        assert Path(leases[0]["workspace_root"]) == workspace.resolve()
        assert leases[0]["pid"] == process.pid
    finally:
        if process.stdin:
            process.stdin.close()
        process.wait(timeout=20)
        _cleanup(state_root, tmp_path / "configs", environment)
    assert process.returncode == 0, process.stderr.read() if process.stderr else ""
    _wait_until(lambda: not (state_root / "owner.json").exists())


def test_three_client_bridges_share_one_runtime_and_keep_workspace_leases(tmp_path: Path) -> None:
    state_root = tmp_path / "state"
    environment, _ = _prepare_proxy(tmp_path, state_root)
    processes: list[subprocess.Popen[str]] = []
    try:
        for client in ("codex", "claude", "pi"):
            workspace = tmp_path / f"project-{client}"
            workspace.mkdir()
            process = _start_bridge(tmp_path, state_root, environment, client, workspace)
            processes.append(process)
            assert _initialize(process)["result"]["serverInfo"]["name"] == "jcodemunch-mcp"

        leases = json.loads((state_root / "leases.json").read_text(encoding="utf-8"))["leases"]
        owner = json.loads((state_root / "owner.json").read_text(encoding="utf-8"))
        assert {lease["client"] for lease in leases} == {"codex", "claude", "pi"}
        assert len({lease["workspace_id"] for lease in leases}) == 3
        assert len({owner["owner_id"]}) == 1

        for process in processes[:-1]:
            assert process.stdin is not None
            process.stdin.close()
            process.wait(timeout=20)
            assert (state_root / "owner.json").exists()
        assert json.loads((state_root / "leases.json").read_text(encoding="utf-8"))["leases"]

        final = processes[-1]
        assert final.stdin is not None
        final.stdin.close()
        final.wait(timeout=20)
        _wait_until(lambda: not (state_root / "owner.json").exists())
        assert json.loads((state_root / "leases.json").read_text(encoding="utf-8"))["leases"] == []
    finally:
        for process in processes:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=10)
        _cleanup(state_root, tmp_path / "configs", environment)


def test_three_projects_are_bound_to_their_own_workspace_on_one_runtime(tmp_path: Path) -> None:
    state_root = tmp_path / "state"
    environment, _ = _prepare_proxy(tmp_path, state_root)
    processes: list[tuple[subprocess.Popen[str], Path]] = []
    try:
        for number, client in enumerate(("codex", "claude", "pi"), start=1):
            workspace = tmp_path / f"unique-project-{number}"
            workspace.mkdir()
            (workspace / f"only-{number}.txt").write_text(f"unique-{number}", encoding="utf-8")
            process = _start_bridge(tmp_path, state_root, environment, client, workspace)
            processes.append((process, workspace.resolve()))
            assert _initialize(process)["result"]["serverInfo"]["name"] == "jcodemunch-mcp"
            tools = _request(process, {"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}})
            assert {tool["name"] for tool in tools["result"]["tools"]} >= {
                "index_folder",
                "search_symbols",
            }

        for index, (process, workspace) in enumerate(processes, start=1):
            indexed = _request(
                process,
                {
                    "jsonrpc": "2.0",
                    "id": 10 + index,
                    "method": "tools/call",
                    "params": {"name": "index_folder", "arguments": {"path": "C:/wrong-project"}},
                },
            )
            index_payload = json.loads(indexed["result"]["content"][0]["text"])
            assert Path(index_payload["arguments"]["path"]) == workspace

            searched = _request(
                process,
                {
                    "jsonrpc": "2.0",
                    "id": 20 + index,
                    "method": "tools/call",
                    "params": {
                        "name": "search_symbols",
                        "arguments": {"repo": "another/repo", "query": f"unique-{index}"},
                    },
                },
            )
            search_payload = json.loads(searched["result"]["content"][0]["text"])
            assert Path(search_payload["arguments"]["repo"]) == workspace
            assert search_payload["arguments"]["query"] == f"unique-{index}"

            escaped = _request(
                process,
                {
                    "jsonrpc": "2.0",
                    "id": 30 + index,
                    "method": "tools/call",
                    "params": {"name": "index_file", "arguments": {"path": "../foreign.py"}},
                },
            )
            assert escaped["error"]["code"] == -32602
            assert "escapes the bound workspace" in escaped["error"]["message"]

            if index == 1:
                outside = tmp_path / "outside-project"
                outside.mkdir()
                (outside / "secret.py").write_text("outside", encoding="utf-8")
                link = workspace / "linked-outside"
                try:
                    link.symlink_to(outside, target_is_directory=True)
                except OSError:
                    link = None
                if link is not None:
                    linked_escape = _request(
                        process,
                        {
                            "jsonrpc": "2.0",
                            "id": 35,
                            "method": "tools/call",
                            "params": {
                                "name": "index_file",
                                "arguments": {"path": "linked-outside/secret.py"},
                            },
                        },
                    )
                    assert linked_escape["error"]["code"] == -32602
                    assert "through a link" in linked_escape["error"]["message"]

            global_call = _request(
                process,
                {
                    "jsonrpc": "2.0",
                    "id": 40 + index,
                    "method": "tools/call",
                    "params": {"name": "list_repos", "arguments": {}},
                },
            )
            assert global_call["error"]["code"] == -32602
            assert "disabled by workspace isolation" in global_call["error"]["message"]
    finally:
        for process, _workspace in processes:
            if process.stdin and not process.stdin.closed:
                process.stdin.close()
            if process.poll() is None:
                try:
                    process.wait(timeout=20)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=10)
        _cleanup(state_root, tmp_path / "configs", environment)


def test_bridge_releases_lease_when_proxy_child_exits_while_stdin_is_open(tmp_path: Path) -> None:
    state_root = tmp_path / "state"
    workspace = tmp_path / "project"
    workspace.mkdir()
    environment, _ = _prepare_proxy(tmp_path, state_root)
    environment["JCODEMUNCH_PROXY_EXIT_IMMEDIATELY"] = "1"
    process = _start_bridge(tmp_path, state_root, environment, "codex", workspace)
    try:
        process.wait(timeout=60)
        assert process.returncode == 7
        _wait_until(lambda: not (state_root / "owner.json").exists())
        assert json.loads((state_root / "leases.json").read_text(encoding="utf-8"))["leases"] == []
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=10)
        _cleanup(state_root, tmp_path / "configs", environment)
