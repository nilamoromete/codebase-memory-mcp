from __future__ import annotations

import json
import os
import subprocess
from pathlib import Path

import pytest


REPO_ROOT = Path(__file__).parents[2]
INTEGRATION = REPO_ROOT / "integrations" / "jcodemunch"
MANIFEST_PATH = INTEGRATION / "versions.json"
LOCK_PATH = INTEGRATION / "proxy" / "package-lock.json"
STAGE_PATH = INTEGRATION / "stage-runtime.ps1"
VERIFY_PATH = INTEGRATION / "verify-runtime.ps1"
ROLLBACK_PATH = INTEGRATION / "rollback-hybrid.ps1"


def test_manifest_pins_exact_verified_companion_artifacts() -> None:
    manifest = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
    runtime = manifest["jcodemunch"]
    proxy = manifest["stdio_proxy"]
    pi_adapter = manifest["pi_adapter"]

    assert runtime["package"] == "jcodemunch-mcp"
    assert runtime["version"] == "1.108.291"
    assert runtime["artifact"] == "jcodemunch_mcp-1.108.291-py3-none-any.whl"
    assert runtime["sha256"] == "41a25402cb755daa55be060d1e8d65183f8868ac3c0b201079d3fb64d8e09f5d"
    assert runtime["transport"] == "streamable-http"
    assert runtime["bind_host"] == "127.0.0.1"
    assert runtime["bearer_auth"] is True
    assert runtime["http_extra"] is True
    assert runtime["executable"] == "venv/Scripts/python.exe"
    assert runtime["python_version"] == "3.13"
    assert runtime["platform"] == "windows-x64"
    assert runtime["requirements_lock"] == "runtime-requirements.lock"
    assert len(runtime["requirements_sha256"]) == 64
    assert runtime["serve_args"][:5] == ["-I", "-B", "-m", "jcodemunch_mcp", "serve"]

    assert proxy["package"] == "mcp-remote"
    assert proxy["version"] == "0.1.43"
    assert proxy["integrity"] == "sha512-N2pGvTAPlHSH64iftgaVsR9J2+QUgCkTbaFb8XMxRbOFvJTCFONJOUNOxfnxPc0FUKyWUE/jpdhabfBI3g/gPw=="
    assert proxy["transport"] == "http-only"
    assert pi_adapter == {
        "package": "pi-mcp-extension",
        "version": "1.5.0",
        "transport": "stdio",
        "source": "https://registry.npmjs.org/pi-mcp-extension/-/pi-mcp-extension-1.5.0.tgz",
        "integrity": "sha512-tfsgi8qSr3UUKMp4vS9/FwKv+Pn2U4T/rTlAwrZkEIvz616mFrU/Ryp3b69ZDfFdkQVVXriaQmZUj4vlZDV2Uw==",
    }


def test_npm_lock_pins_proxy_root_and_registry_integrity() -> None:
    lock = json.loads(LOCK_PATH.read_text(encoding="utf-8"))
    assert lock["lockfileVersion"] == 3
    assert lock["packages"][""]["dependencies"] == {"mcp-remote": "0.1.43"}
    pinned = lock["packages"]["node_modules/mcp-remote"]
    assert pinned["version"] == "0.1.43"
    assert pinned["resolved"] == "https://registry.npmjs.org/mcp-remote/-/mcp-remote-0.1.43.tgz"
    assert pinned["integrity"] == "sha512-N2pGvTAPlHSH64iftgaVsR9J2+QUgCkTbaFb8XMxRbOFvJTCFONJOUNOxfnxPc0FUKyWUE/jpdhabfBI3g/gPw=="


def test_staging_is_private_verified_and_side_by_side() -> None:
    stage = STAGE_PATH.read_text(encoding="utf-8")
    verify = VERIFY_PATH.read_text(encoding="utf-8")
    lowered = stage.lower()

    assert '".staging-"+[Guid]::NewGuid()' in stage
    assert "Get-FileHash" in stage
    assert "importlib.metadata" in stage
    assert "npm ci" in lowered
    assert "--ignore-scripts" in stage
    assert "$pipOutput=@(& $venvPython" in stage
    assert "--require-hashes" in stage
    assert "--only-binary=:all:" in stage
    assert "--no-deps" in stage
    assert "pip check" in stage
    assert "requirements_sha256" in stage
    assert "$npmOutput=@(& $Npm" in stage
    assert "[IO.Directory]::Move" in stage
    assert "partially published companion composition" in stage
    assert "published" in stage
    assert "Atomic publication cleanup failed" in stage
    assert "Assert-SafeChild" in stage
    assert "SHA512" in stage
    assert "installation-manifest.json" in stage
    assert '@("venv")' in stage
    assert 'venv/Lib/site-packages' not in stage
    assert "PYTHONDONTWRITEBYTECODE" in stage
    assert "NODE_OPTIONS" in stage
    assert "NODE_PATH" in stage
    assert 'SetEnvironmentVariable($name,$null,"Process")' in stage
    assert 'Filter "*.pyc"' in stage
    assert "Refusing runtime reuse without the existing installation receipt" in stage
    assert "ReparsePoint" in stage

    assert "Pinned jCodeMunch artifact integrity check failed" in verify
    assert "importlib.metadata" in verify
    assert "streamable-http" in verify
    assert "mcp-remote version or lockfile integrity mismatch" in verify
    assert "Installed-code integrity mismatch" in verify
    assert "Installed receipt is missing" in verify
    assert "Pinned Pi extension integrity mismatch" in verify
    assert "Assert-NoReparsePoint $piVersionRoot" in verify
    assert "JCODEMUNCH_HTTP_TOKEN" not in verify
    assert "Assert-RegularNonReparseFile" in verify
    assert "Integration verification requires an expected manifest hash or installed receipt" in verify
    assert "integration-manifest.json" in verify
    assert "-I -B -c" in verify
    assert "-I -B -m" in verify
    for name in ("PYTHONPATH", "PYTHONHOME", "PYTHONSTARTUP", "PYTHONNOUSERSITE"):
        assert name in verify
    assert verify.index("$receipt=Get-Content") < verify.index("$versionOutput=(& $runtimePython")
    assert verify.index("Test-IntegrationManifest $IntegrationRoot") < verify.index("$manifest=Get-Content")


def _create_junction(link: Path, target: Path) -> None:
    result = subprocess.run(
        [
            "pwsh",
            "-NoProfile",
            "-NonInteractive",
            "-Command",
            "New-Item -ItemType Junction -Path $env:LINK -Target $env:TARGET | Out-Null",
        ],
        capture_output=True,
        text=True,
        check=False,
        env={**os.environ, "LINK": str(link), "TARGET": str(target)},
    )
    assert result.returncode == 0, result.stderr


@pytest.mark.skipif(os.name != "nt", reason="Windows junction contract")
@pytest.mark.parametrize("junction_kind", ["destination", "package_parent"])
def test_staging_rejects_reparse_ancestors_before_network(
    tmp_path: Path, junction_kind: str
) -> None:
    outside = tmp_path / "outside"
    outside.mkdir()
    sentinel = outside / "sentinel.txt"
    sentinel.write_text("preserve", encoding="utf-8")
    if junction_kind == "destination":
        destination = tmp_path / "runtime-link"
        _create_junction(destination, outside)
    else:
        destination = tmp_path / "runtime"
        destination.mkdir()
        _create_junction(destination / "jcodemunch-mcp", outside)

    result = subprocess.run(
        [
            "pwsh",
            "-NoProfile",
            "-NonInteractive",
            "-File",
            str(STAGE_PATH),
            "-DestinationRoot",
            str(destination),
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode != 0
    assert "reparse point" in (result.stdout + result.stderr).lower()
    assert sentinel.read_text(encoding="utf-8") == "preserve"


@pytest.mark.skipif(os.name != "nt", reason="Windows junction contract")
def test_rollback_rejects_reparse_state_root_before_recursive_cleanup(tmp_path: Path) -> None:
    outside = tmp_path / "outside-state"
    outside.mkdir()
    sentinel = outside / "sentinel.txt"
    sentinel.write_text("preserve", encoding="utf-8")
    state_link = tmp_path / "state-link"
    _create_junction(state_link, outside)

    result = subprocess.run(
        [
            "pwsh",
            "-NoProfile",
            "-NonInteractive",
            "-File",
            str(ROLLBACK_PATH),
            "-StateRoot",
            str(state_link),
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode != 0
    assert "reparse point" in (result.stdout + result.stderr).lower()
    assert sentinel.read_text(encoding="utf-8") == "preserve"
