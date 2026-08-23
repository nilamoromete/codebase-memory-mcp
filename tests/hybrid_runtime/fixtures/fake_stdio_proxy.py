"""Minimal STDIO-to-HTTP proxy used to exercise the PowerShell bridge.

It intentionally validates the same argument and secret boundaries as the
pinned mcp-remote process, but leaves real mcp-remote verification to the
disposable staging and real-client acceptance gates.
"""

from __future__ import annotations

import json
import os
import sys
from pathlib import Path
from urllib.request import Request, urlopen


def main() -> int:
    arguments = sys.argv[1:]
    if len(arguments) < 6:
        return 91
    _entrypoint, endpoint, transport_flag, transport, header_flag, header = arguments[:6]
    token = os.environ.get("JCODEMUNCH_HTTP_TOKEN", "")
    if (
        transport_flag != "--transport"
        or transport != "http-only"
        or header_flag != "--header"
        or header != "Authorization: Bearer ${JCODEMUNCH_HTTP_TOKEN}"
        or not token
    ):
        return 92

    observation_path = os.environ.get("JCODEMUNCH_PROXY_OBSERVATION")
    if observation_path:
        Path(observation_path).write_text(
            json.dumps(
                {
                    "pid": os.getpid(),
                    "endpoint": endpoint,
                    "transport": transport,
                    "header_is_placeholder": "${JCODEMUNCH_HTTP_TOKEN}" in header,
                    "token_present_in_environment": bool(token),
                    "argv_contains_token": any(token in value for value in arguments),
                },
                sort_keys=True,
            ),
            encoding="utf-8",
        )

    for line in sys.stdin:
        payload = line.strip()
        if not payload:
            continue
        request = Request(
            endpoint,
            data=payload.encode("utf-8"),
            headers={
                "Authorization": f"Bearer {token}",
                "Accept": "application/json, text/event-stream",
                "Content-Type": "application/json",
            },
            method="POST",
        )
        with urlopen(request, timeout=10) as response:
            body = response.read().decode("utf-8")
        data_lines = [item[6:] for item in body.splitlines() if item.startswith("data: ")]
        if not data_lines:
            return 93
        sys.stdout.write(data_lines[-1] + "\n")
        sys.stdout.flush()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
