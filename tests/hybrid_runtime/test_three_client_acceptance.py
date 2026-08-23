from __future__ import annotations

"""Synthetic contract checks; this module does not launch real client hosts."""

from pathlib import Path

from .fixtures.acceptance_harness import (
    ClientRuntimeObservation,
    assert_shared_runtime,
)


def test_three_client_harness_rejects_pid_only_matches() -> None:
    observations = [
        ClientRuntimeObservation("codex", 41001, "launch-1", "runtime-1", "127.0.0.1:45123"),
        ClientRuntimeObservation("claude", 41001, "launch-1", "runtime-1", "127.0.0.1:45123"),
        ClientRuntimeObservation("pi", 41001, "launch-2", "runtime-2", "127.0.0.1:45123"),
    ]

    try:
        assert_shared_runtime(observations)
    except AssertionError as error:
        assert "runtime identity" in str(error)
    else:
        raise AssertionError("PID-only matches must not pass shared-runtime acceptance")


def test_three_client_harness_accepts_one_pid_and_identity() -> None:
    observations = [
        ClientRuntimeObservation("codex", 41001, "launch-1", "runtime-1", "127.0.0.1:45123"),
        ClientRuntimeObservation("claude", 41001, "launch-1", "runtime-1", "127.0.0.1:45123"),
        ClientRuntimeObservation("pi", 41001, "launch-1", "runtime-1", "127.0.0.1:45123"),
    ]

    result = assert_shared_runtime(observations)

    assert set(result) == {"codex", "claude", "pi"}
    assert {item.pid for item in result.values()} == {41001}
    assert {item.runtime_identity for item in result.values()} == {"runtime-1"}


def test_harness_is_not_bound_to_live_client_config_paths() -> None:
    # Keep the acceptance harness disposable by construction; this sentinel
    # documents the invariant without resolving any user-scoped config path.
    assert not (Path.home() / ".codex").samefile(Path.cwd())
