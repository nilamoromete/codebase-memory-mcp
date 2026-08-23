from __future__ import annotations

import os
from pathlib import Path

from .fixtures.config_sandboxes import (
    apply_config_sandbox_environment,
    create_config_sandboxes,
)


def test_all_client_config_roots_are_under_the_disposable_test_root(tmp_path: Path) -> None:
    sandboxes = create_config_sandboxes(tmp_path)

    assert set(sandboxes) == {"codex", "claude", "pi"}
    for sandbox in sandboxes.values():
        assert sandbox.root.is_relative_to(tmp_path)
        assert sandbox.root.is_dir()
        assert sandbox.root != Path.home()
        assert all(
            Path(value).is_relative_to(tmp_path)
            for key, value in sandbox.environment.items()
            if key.endswith(("HOME", "DIR", "APPDATA", "LOCALAPPDATA", "CONFIG_HOME"))
        )


def test_applying_sandbox_environment_does_not_mutate_process_environment(
    tmp_path: Path,
) -> None:
    sandboxes = create_config_sandboxes(tmp_path)
    original = dict(os.environ)
    isolated = dict(original)

    apply_config_sandbox_environment(sandboxes, isolated)
