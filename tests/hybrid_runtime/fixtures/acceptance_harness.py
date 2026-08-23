"""Assertions shared by the disposable three-client lifecycle acceptance tests."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Iterable, Mapping


@dataclass(frozen=True)
class ClientRuntimeObservation:
    """Runtime identity reported by one client after it acquires a lease."""

    client: str
    pid: int
    launch_id: str
    runtime_identity: str
    endpoint: str


SUPPORTED_CLIENTS = frozenset({"codex", "claude", "pi"})


def assert_shared_runtime(
    observations: Iterable[ClientRuntimeObservation],
) -> Mapping[str, ClientRuntimeObservation]:
    """Prove all three clients point at one owned runtime.

    This deliberately checks more than PID: Windows may reuse a PID, so the
    launch and runtime identities must also be stable across every client.
    """

    by_client = {observation.client: observation for observation in observations}
    if set(by_client) != SUPPORTED_CLIENTS:
        raise AssertionError(
            f"expected one observation for {sorted(SUPPORTED_CLIENTS)}, "
            f"got {sorted(by_client)}"
        )

    identities = {
        (
            observation.pid,
            observation.launch_id,
            observation.runtime_identity,
            observation.endpoint,
        )
        for observation in by_client.values()
    }
    if len(identities) != 1:
        raise AssertionError(f"clients do not share one runtime identity: {identities}")
    return by_client
