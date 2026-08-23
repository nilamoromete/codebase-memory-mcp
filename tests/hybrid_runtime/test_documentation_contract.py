from __future__ import annotations

from pathlib import Path


REPO_ROOT = Path(__file__).parents[2]
RUNBOOK = REPO_ROOT / "docs" / "hybrid-runtime-runbook.md"
INTEGRATION_README = REPO_ROOT / "integrations" / "jcodemunch" / "README.md"
ROOT_README = REPO_ROOT / "README.md"


def test_runbook_covers_every_required_operator_journey() -> None:
    text = RUNBOOK.read_text(encoding="utf-8").lower()
    for term in (
        "status",
        "upgrade",
        "port collision",
        "reselection",
        "multiple clients and projects",
        "generation-bound grace",
        "exact-owner cleanup",
        "rollback",
        "install-receipt.json",
        "restart codex, claude code, and pi",
    ):
        assert term in text
    assert "stop-process -name" in text
    assert "never" in text


def test_documentation_names_both_engines_and_exact_pins() -> None:
    combined = "\n".join(
        path.read_text(encoding="utf-8")
        for path in (ROOT_README, INTEGRATION_README, RUNBOOK)
    )
    for term in (
        "codebase-memory-mcp",
        "jCodeMunch",
        "1.108.291",
        "mcp-remote",
        "0.1.43",
        "pi-mcp-extension",
        "1.5.0",
        "127.0.0.1",
    ):
        assert term in combined
