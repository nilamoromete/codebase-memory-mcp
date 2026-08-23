from __future__ import annotations

import json
from pathlib import Path


REPO_ROOT = Path(__file__).parents[2]
INTEGRATION_ROOT = REPO_ROOT / "integrations" / "jcodemunch"
LIFECYCLE_SCRIPT = INTEGRATION_ROOT / "jcodemunch-lifecycle.ps1"
MODULE = INTEGRATION_ROOT / "JCodeMunchLifecycle.psm1"
BRIDGE = INTEGRATION_ROOT / "jcodemunch-bridge.ps1"
WORKSPACE_PROXY = INTEGRATION_ROOT / "workspace-proxy.mjs"
VERSIONS_MANIFEST = INTEGRATION_ROOT / "versions.json"


def _read(path: Path) -> str:
    assert path.is_file(), f"required hybrid runtime contract is missing: {path}"
    return path.read_text(encoding="utf-8")


def _manifest() -> dict:
    return json.loads(_read(VERSIONS_MANIFEST))


def test_runtime_manifest_pins_verified_loopback_dependencies() -> None:
    manifest = _manifest()

    runtime = manifest["jcodemunch"]
    assert runtime["version"] == "1.108.291"
    assert runtime["artifact"] == "jcodemunch_mcp-1.108.291-py3-none-any.whl"
    assert runtime["sha256"] == (
        "41a25402cb755daa55be060d1e8d65183f8868ac3c0b201079d3fb64d8e09f5d"
    )
    assert runtime["transport"] == "streamable-http"
    assert runtime["bind_host"] == "127.0.0.1"
    assert runtime["bearer_auth"] is True

    proxy = manifest["stdio_proxy"]
    assert proxy["package"] == "mcp-remote"
    assert proxy["version"] == "0.1.43"
    assert proxy["integrity"] == (
        "sha512-N2pGvTAPlHSH64iftgaVsR9J2+QUgCkTbaFb8XMxRbOFvJTCFONJOUNOxfnxPc0FUKyWUE/jpdhabfBI3g/gPw=="
    )
    assert proxy["transport"] == "http-only"


def test_default_state_root_is_product_scoped_not_farmos_scoped() -> None:
    source = _read(MODULE)
    assert r"CodebaseMemoryMcp\companions\jcodemunch" in source
    assert r"Aliat\jcodemunch" not in source
    assert "FarmOS" not in source


def test_lifecycle_exposes_shared_commands_and_exact_owner_identity() -> None:
    manager = _read(LIFECYCLE_SCRIPT)
    for command in (
        "configure",
        "acquire",
        "release",
        "heartbeat",
        "status",
        "stop",
        "clean",
        "rollback",
    ):
        assert command in manager

    for field in (
        "pid",
        "process_start_time",
        "executable_path",
        "executable_sha256",
        "runtime_version",
        "command_fingerprint",
        "runtime_identity",
        "launch_id",
        "port",
    ):
        assert field in manager


def test_bridge_is_the_lifecycle_boundary_and_keeps_secret_out_of_argv() -> None:
    source = _read(BRIDGE)
    proxy = _read(WORKSPACE_PROXY)
    combined = source + proxy
    lowered = combined.lower()

    assert 'Invoke-LifecycleCommand -Command "acquire"' in source
    assert 'Invoke-LifecycleCommand -Command "release"' in source
    assert "-ClientPid" in source
    assert "$PID" in source
    assert "--transport" in proxy
    assert "http-only" in proxy
    assert "JCODEMUNCH_HTTP_TOKEN" in combined
    assert "--header" in proxy
    assert "${JCODEMUNCH_HTTP_TOKEN}" in proxy
    assert "-token" not in lowered
    assert "--token" not in lowered
    assert "bearer $token" not in lowered
    assert source.index("Assert-InstalledRuntime") < source.index(
        'Invoke-LifecycleCommand -Command "acquire"'
    )
    assert "-ReceiptPath" in source
    assert "-IntegrationRoot" in source


def test_bridge_uses_only_staged_proxy_and_releases_in_finally() -> None:
    source = _read(BRIDGE)
    lowered = source.lower()

    assert "finally" in lowered
    assert "mcp-remote" in lowered
    assert "npx" not in lowered
    assert "start-process" not in lowered or "Wait" in source
    assert "WorkspaceRoot" in source
    assert "SessionId" in source
    assert "Client" in source


def test_runtime_launch_disables_python_bytecode_writes() -> None:
    source = _read(LIFECYCLE_SCRIPT)
    assert "PYTHONDONTWRITEBYTECODE" in source
    assert 'SetEnvironmentVariable("PYTHONDONTWRITEBYTECODE","1","Process")' in source
    assert "previousRuntimeEnvironment" in source
    for name in ("PYTHONPATH", "PYTHONHOME", "PYTHONSTARTUP", "PYTHONNOUSERSITE"):
        assert name in source


def test_node_children_drop_injection_environment() -> None:
    bridge = _read(BRIDGE)
    proxy = _read(WORKSPACE_PROXY)
    for name in ("NODE_OPTIONS", "NODE_PATH"):
        assert f'Environment.Remove("{name}")' in bridge
        assert f"delete childEnvironment.{name}" in proxy


def test_workspace_proxy_binds_repo_and_rejects_escape_and_global_inventory() -> None:
    source = _read(WORKSPACE_PROXY)
    assert "bound.repo = workspaceRoot" in source
    assert "bound.path = workspaceRoot" in source
    assert "escapes the bound workspace" in source
    assert '"list_repos"' in source
    assert "toolSchemas" in source


def test_wrapper_requires_disposable_roots_in_test_mode_and_private_acl() -> None:
    manager = _read(LIFECYCLE_SCRIPT)
    module = _read(MODULE)

    assert "StateRoot" in manager
    assert "ConfigRoot" in manager
    assert "TEST_MODE" in manager.upper()
    assert "USERPROFILE" in manager
    assert "(OI)(CI)(F)" in module


def test_cleanup_never_enumerates_or_kills_by_process_name() -> None:
    source = _read(LIFECYCLE_SCRIPT).lower()
    forbidden = (
        "get-process jcodemunch",
        "get-process -name",
        "taskkill /im",
        "stop-process -name",
    )
    assert not any(item in source for item in forbidden)
