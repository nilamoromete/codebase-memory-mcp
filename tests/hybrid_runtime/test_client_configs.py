from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

import pytest


REPO_ROOT = Path(__file__).parents[2]
INTEGRATION = REPO_ROOT / "integrations" / "jcodemunch"
INSTALLER = INTEGRATION / "install-hybrid.ps1"
ROLLBACK = INTEGRATION / "rollback-hybrid.ps1"
RECEIPT = INTEGRATION / "write-install-receipt.ps1"
LIFECYCLE = INTEGRATION / "jcodemunch-lifecycle.ps1"
TRANSACTION = INTEGRATION / "HybridInstallTransaction.psm1"
PROVENANCE = INTEGRATION / "write-candidate-provenance.ps1"
INTEGRATION_MANIFEST = INTEGRATION / "write-integration-manifest.ps1"
EMBED_FRONTEND = REPO_ROOT / "scripts" / "embed-frontend.sh"


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def test_embed_uses_compiler_target_when_shell_and_target_platform_differ(tmp_path: Path) -> None:
    bash = shutil.which("bash")
    if bash is None:
        pytest.skip("bash is required for the frontend embedding regression")

    (tmp_path / "scripts").mkdir()
    (tmp_path / "src" / "ui").mkdir(parents=True)
    (tmp_path / "dist").mkdir()
    shutil.copy2(EMBED_FRONTEND, tmp_path / "scripts" / "embed-frontend.sh")
    (tmp_path / "dist" / "probe.txt").write_text("probe", encoding="utf-8")

    fake_cc = tmp_path / "fake-cc.sh"
    fake_cc.write_text(
        "#!/usr/bin/env bash\n"
        "if [[ \"${1:-}\" == '-dumpmachine' ]]; then printf 'x86_64-w64-windows-gnu\\n'; exit 0; fi\n"
        "out=''\n"
        "while [[ $# -gt 0 ]]; do\n"
        "  if [[ \"$1\" == '-o' ]]; then shift; out=\"$1\"; fi\n"
        "  shift\n"
        "done\n"
        "printf 'coff-probe' > \"$out\"\n",
        encoding="utf-8",
        newline="\n",
    )
    command = (
        "uname() { printf 'Linux\\n'; }; export -f uname; "
        "export CC='bash fake-cc.sh'; "
        "scripts/embed-frontend.sh dist build/embedded"
    )
    subprocess.run([bash, "-lc", command], cwd=tmp_path, check=True, capture_output=True, text=True)

    generated = _read(tmp_path / "src" / "ui" / "embedded_assets.c")
    assert "_binary_probe_txt_data" in generated
    assert "_binary_probe_txt_start" not in generated
    assert (tmp_path / "build" / "embedded" / "embed_probe_txt.o").read_bytes() == b"coff-probe"


def test_installer_cuts_over_all_three_clients_and_commits_receipt_last() -> None:
    source = _read(INSTALLER)
    stage = source.index("stage-runtime.ps1")
    native_install = source.rindex("--clients=claude,codex,pi")
    configure = source.index("-Command configure")
    handshake = source.index("Test-CbmMcpHandshake $installedBinary")
    receipt = source.index("write-install-receipt.ps1")
    committed = source.index("-Status applied")

    assert stage < native_install < configure < handshake < receipt < committed
    assert "verify-runtime.ps1" in source
    assert "rollback-hybrid.ps1" in source
    assert "-ApplyClientConfigs" in source
    assert "write-integration-manifest.ps1" in source
    assert "ExpectedIntegrationManifestSha256" in source
    assert "ReceiptPath=$receiptPath" in source


def test_config_transaction_keeps_secret_out_of_client_surfaces() -> None:
    source = _read(LIFECYCLE)
    assert 'command="pwsh"' in source
    assert 'transport="stdio"' in source
    assert "$manifest.pi_adapter.package" in source
    assert "$manifest.pi_adapter.version" in source
    assert '$piSpec="file:' in source
    assert "pi_adapter_integrity" in source
    assert "jcodemunch-bridge.ps1" in source
    assert source.count("IsNullOrWhiteSpace([string]$_)") >= 2
    config_region = source[source.index("function Invoke-ClientConfigTransaction") : source.index("function Configure-Lifecycle")]
    assert "JCODEMUNCH_HTTP_TOKEN" not in config_region
    assert "Set-TokenEnvironmentValue" not in config_region


def test_receipt_is_provenance_complete_and_secret_free() -> None:
    source = _read(RECEIPT)
    for field in (
        "CandidateProvenance",
        "source_commit",
        "source_branch",
        "merge_base",
        "CandidateBinary",
        "InstalledBinary",
        "SnapshotTransactionId",
        "installed_at",
        "architecture",
        "sha256",
        "integration_assets",
        "file_count",
    ):
        assert field in source
    assert "JCODEMUNCH_HTTP_TOKEN" not in source
    assert "Get-JCodeMunchSecret" not in source


def test_rollback_delegates_to_identity_safe_lifecycle_command() -> None:
    source = _read(ROLLBACK)
    restore = source.index("Restore-HybridInstallJournal")
    stop = source.index("-Command stop")
    assert stop < restore
    assert "Enter-HybridInstallLock" in source
    assert "LockAlreadyHeld" in source
    assert "StartsWith($runtimePrefix" in source
    assert "outside owned runtime root" in source
    assert '"--binary-source"' in source
    assert '"uninstall","--yes","--binary-only"' in source
    assert "-SkipPaths @($binaryTarget)" in source
    assert "PublishedRuntimePaths" in source
    assert "activation_environment" in source
    assert '$start.Environment[$name]=$value' in source
    assert "Stop-Process" not in source


def test_unified_transaction_snapshots_bytes_acl_and_binary_targets() -> None:
    installer = _read(INSTALLER)
    transaction = _read(TRANSACTION)
    assert "$installedBinary" in installer
    assert "hooks_planned" in installer
    assert "skill_files_planned" in installer
    assert "New-HybridInstallJournal" in installer
    assert "-BinaryTarget $installedBinary" in installer
    assert "--binary-only" in installer
    assert "cleanup_files_planned" in installer
    assert "cleanup_directories_planned" in installer
    assert "Enter-HybridInstallLock" in installer
    assert "acl_sddl" in transaction
    assert "security_descriptor_base64" in transaction
    assert "content_sha256" in transaction
    assert "Restored content verification failed" in transaction
    assert "binary_target" in transaction
    assert "SkipPaths" in transaction
    assert "activation_environment" in transaction
    assert "ActivationEnvironment" in transaction


def test_integration_assets_are_exactly_manifested_without_links_or_bytecode() -> None:
    source = _read(INTEGRATION_MANIFEST)
    assert "integration-manifest.json" in source
    assert "ReparsePoint" in source
    assert "Executable Python bytecode is forbidden" in source
    assert "manifest_sha256" in source
    assert "file_count" in source
    lifecycle = _read(LIFECYCLE)
    assert "$rootIntegrationManifest" in lifecycle
    assert "[string]::Equals($_.FullName,$rootIntegrationManifest" in lifecycle


def test_candidate_provenance_is_emitted_only_by_the_canonical_clean_build() -> None:
    source = _read(PROVENANCE)
    installer = _read(INSTALLER)
    for contract in (
        "scripts/build.sh",
        "build/attested-candidate",
        '"--with-ui"',
        "npm.cmd",
        'NPM=`"$msysNpm`"',
        '"--version"',
        "HEAD^{tree}",
        "post_build_worktree_clean",
        "transcript_sha256",
        "canonical-clean-build",
    ):
        assert contract in source
    assert "schema_version=4" in source
    assert "schema_version-ne4" in installer
    assert "Attested source identity changed before installation" in installer
    assert "git -C $source ls-files" in source
    assert "Ignored or untracked files exist inside the integration source" in source
    assert "integration_assets" in source
    assert "SourceAssetPaths" in installer
    assert "unexpected assets" in installer
