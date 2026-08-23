from __future__ import annotations

import json
import os
import subprocess
from pathlib import Path


REPO_ROOT = Path(__file__).parents[2]
MODULE = REPO_ROOT / "integrations" / "jcodemunch" / "HybridInstallTransaction.psm1"


def test_unified_journal_restores_bytes_acl_and_removes_only_recorded_new_file(
    tmp_path: Path,
) -> None:
    state = tmp_path / "state"
    sandbox = tmp_path / "client-home"
    sandbox.mkdir()
    existing = sandbox / "existing.toml"
    created = sandbox / "created.json"
    binary = sandbox / "codebase-memory-mcp.exe"
    legacy_directory = sandbox / "legacy-empty"
    outside = tmp_path / "outside-sentinel.txt"
    existing.write_bytes(b'original = "bytes"\r\n')
    binary.write_bytes(b"recorded binary")
    outside.write_bytes(b"must-not-change")
    legacy_directory.mkdir()
    journal = state / "install-transaction.json"

    command = """
Import-Module $env:HYBRID_MODULE -Force
$before=(Get-Acl -LiteralPath $env:HYBRID_EXISTING).Sddl
$j=New-HybridInstallJournal -StateRoot $env:HYBRID_STATE -JournalPath $env:HYBRID_JOURNAL -Paths @($env:HYBRID_EXISTING,$env:HYBRID_CREATED,$env:HYBRID_BINARY) -DirectoryPaths @($env:HYBRID_DIRECTORY) -BinaryTarget $env:HYBRID_BINARY
[IO.File]::WriteAllText($env:HYBRID_EXISTING,'changed')
[IO.File]::WriteAllText($env:HYBRID_CREATED,'new')
[IO.File]::WriteAllText($env:HYBRID_BINARY,'native barrier restored this')
Remove-Item -LiteralPath $env:HYBRID_DIRECTORY -Force
[void](Set-HybridInstallJournalStatus -JournalPath $env:HYBRID_JOURNAL -Status applied)
$result=Restore-HybridInstallJournal -JournalPath $env:HYBRID_JOURNAL -SkipPaths @($env:HYBRID_BINARY)
$after=(Get-Acl -LiteralPath $env:HYBRID_EXISTING).Sddl
[ordered]@{before=$before;after=$after;result=$result}|ConvertTo-Json -Depth 10 -Compress
"""
    result = subprocess.run(
        [
            "pwsh",
            "-NoProfile",
            "-NonInteractive",
            "-Command",
            command,
        ],
        capture_output=True,
        text=True,
        check=False,
        env={
            **os.environ,
            "HYBRID_MODULE": str(MODULE),
            "HYBRID_STATE": str(state),
            "HYBRID_JOURNAL": str(journal),
            "HYBRID_EXISTING": str(existing),
            "HYBRID_CREATED": str(created),
            "HYBRID_BINARY": str(binary),
            "HYBRID_DIRECTORY": str(legacy_directory),
        },
    )
    assert result.returncode == 0, result.stderr
    payload = json.loads(result.stdout)
    assert payload["result"] is not None, payload
    assert payload["result"]["status"] == "rolled_back"
    assert payload["result"]["failed"] == 0
    assert payload["before"] == payload["after"]
    assert existing.read_bytes() == b'original = "bytes"\r\n'
    assert binary.read_bytes() == b"native barrier restored this"
    assert not created.exists()
    assert legacy_directory.is_dir()
    assert outside.read_bytes() == b"must-not-change"
    journal_data = json.loads(journal.read_text(encoding="utf-8"))
    assert journal_data["status"] == "rolled_back"
    assert Path(journal_data["binary_target"]) == binary
    assert all(str(sandbox) in entry["path"] for entry in journal_data["entries"])


def test_install_lock_is_exclusive_and_reusable_after_release(tmp_path: Path) -> None:
    state = tmp_path / "state"
    command = r"""
Import-Module $env:HYBRID_MODULE -Force
$first=Enter-HybridInstallLock -StateRoot $env:HYBRID_STATE
$blocked=$false
try { try { $second=Enter-HybridInstallLock -StateRoot $env:HYBRID_STATE } catch { $blocked=$true } }
finally { Exit-HybridInstallLock -Lock $first }
$third=Enter-HybridInstallLock -StateRoot $env:HYBRID_STATE
Exit-HybridInstallLock -Lock $third
[ordered]@{blocked=$blocked;reacquired=$true}|ConvertTo-Json -Compress
"""
    result = subprocess.run(
        ["pwsh", "-NoProfile", "-NonInteractive", "-Command", command],
        capture_output=True,
        text=True,
        check=False,
        env={**os.environ, "HYBRID_MODULE": str(MODULE), "HYBRID_STATE": str(state)},
    )
    assert result.returncode == 0, result.stderr
    assert json.loads(result.stdout) == {"blocked": True, "reacquired": True}
