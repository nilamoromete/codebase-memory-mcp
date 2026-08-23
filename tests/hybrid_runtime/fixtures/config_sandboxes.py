from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Mapping


@dataclass(frozen=True)
class ClientConfigSandbox:
    """Disposable configuration root for one supported client."""

    name: str
    root: Path
    environment: Mapping[str, str]


def create_config_sandboxes(base: Path) -> dict[str, ClientConfigSandbox]:
    """Create all client roots below ``base`` and return their test env.

    The caller must provide a pytest ``tmp_path`` (or an equivalent
    disposable directory). No platform user-home or live client path is
    resolved by this helper.
    """

    root = base / "jcodemunch-client-configs"
    root.mkdir(parents=True, exist_ok=True)
    user_home = root / "user-home"
    user_home.mkdir()

    client_roots = {
        "codex": root / "codex",
        "claude": root / "claude",
        "pi": root / "pi",
    }
    for client_root in client_roots.values():
        client_root.mkdir()

    environment = {
        "CODEX_HOME": str(client_roots["codex"]),
        "CLAUDE_CONFIG_DIR": str(client_roots["claude"]),
        "PI_CODING_AGENT_DIR": str(client_roots["pi"]),
        "HOME": str(user_home),
        "USERPROFILE": str(user_home),
        "APPDATA": str(user_home / "AppData" / "Roaming"),
        "LOCALAPPDATA": str(user_home / "AppData" / "Local"),
        "XDG_CONFIG_HOME": str(user_home / ".config"),
    }
    return {
        name: ClientConfigSandbox(name=name, root=client_root, environment=environment)
        for name, client_root in client_roots.items()
    }


def apply_config_sandbox_environment(
    sandboxes: Mapping[str, ClientConfigSandbox],
    environ: dict[str, str],
) -> None:
    """Apply only disposable client paths to a test environment mapping."""

    if set(sandboxes) != {"codex", "claude", "pi"}:
        raise ValueError("all three client sandboxes are required")
    for key, value in sandboxes["codex"].environment.items():
        environ[key] = value
