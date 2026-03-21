#!/usr/bin/env bash
set -euo pipefail

# Benchmark the high-level pre-edit workflow against the older multi-call flow.
#
# Usage:
#   scripts/benchmark-edit-plan.sh <binary-path> [repo-path] [target-file]
#
# Examples:
#   scripts/benchmark-edit-plan.sh build/c/codebase-memory-mcp .
#   scripts/benchmark-edit-plan.sh build/c/codebase-memory-mcp . src/cli/cli.c

BINARY="${1:?usage: benchmark-edit-plan.sh <binary-path> [repo-path] [target-file]}"
REPO_PATH="${2:-$(pwd)}"
TARGET_FILE="${3:-}"

PYTHON_CMD=()
if command -v python3 >/dev/null 2>&1; then
    PYTHON_CMD=(python3)
elif command -v python >/dev/null 2>&1; then
    PYTHON_CMD=(python)
elif command -v py >/dev/null 2>&1; then
    PYTHON_CMD=(py -3)
else
    echo "error: python3/python/py is required"
    exit 1
fi

if [ ! -e "$BINARY" ]; then
    echo "error: binary not found: $BINARY"
    exit 1
fi

if [ ! -d "$REPO_PATH" ]; then
    echo "error: repo path not found: $REPO_PATH"
    exit 1
fi

if [ -z "$TARGET_FILE" ]; then
    if [ -f "$REPO_PATH/src/cli/cli.c" ]; then
        TARGET_FILE="src/cli/cli.c"
    elif [ -f "$REPO_PATH/src/main.py" ]; then
        TARGET_FILE="src/main.py"
    else
        TARGET_FILE="$(find "$REPO_PATH" -type f \( -name '*.c' -o -name '*.go' -o -name '*.py' -o -name '*.ts' -o -name '*.tsx' \) | sed "s#^$REPO_PATH/##" | head -n 1)"
    fi
fi

if [ -z "$TARGET_FILE" ] || [ ! -f "$REPO_PATH/$TARGET_FILE" ]; then
    echo "error: target file not found: $TARGET_FILE"
    exit 1
fi

TMP_JSON="$(mktemp)"
trap 'rm -f "$TMP_JSON"' EXIT

"$BINARY" cli index_repository "{\"repo_path\":\"$REPO_PATH\"}" > "$TMP_JSON"

PROJECT="$(
"${PYTHON_CMD[@]}" - "$TMP_JSON" <<'PY'
import json, sys
with open(sys.argv[1], 'r', encoding='utf-8') as f:
    outer = json.load(f)
inner = json.loads(outer["content"][0]["text"])
print(inner.get("project", ""))
PY
)"

if [ -z "$PROJECT" ]; then
    echo "error: failed to resolve indexed project name"
    exit 1
fi

echo "=== benchmark-edit-plan ==="
echo "binary: $BINARY"
echo "repo:   $REPO_PATH"
echo "project:$PROJECT"
echo "target: $TARGET_FILE"
echo ""

"${PYTHON_CMD[@]}" - "$BINARY" "$PROJECT" "$TARGET_FILE" <<'PY'
import json
import subprocess
import sys
import time

binary = sys.argv[1]
project = sys.argv[2]
target = sys.argv[3]

legacy_calls = [
    ("get_file_context", {"project": project, "path": target}),
    ("get_related_files", {"project": project, "path": target, "limit": 8}),
    ("get_tests", {"project": project, "paths": [target], "limit": 8}),
    ("get_change_risks", {"project": project, "paths": [target], "max_related_files": 8, "max_tests": 8}),
]

edit_plan_calls = [
    ("get_edit_plan", {"project": project, "path": target, "mode": "compact", "task_type": "fix"}),
]

detailed_calls = [
    ("get_edit_plan", {"project": project, "path": target, "mode": "detailed", "task_type": "fix"}),
]

def run_call(tool, payload):
    started = time.perf_counter()
    raw = subprocess.check_output(
        [binary, "cli", tool, json.dumps(payload, separators=(",", ":"))],
        text=True,
    )
    elapsed_ms = (time.perf_counter() - started) * 1000.0
    return elapsed_ms, raw

def run_suite(name, calls):
    total_ms = 0.0
    total_bytes = 0
    per_call = []
    for tool, payload in calls:
        elapsed_ms, raw = run_call(tool, payload)
        total_ms += elapsed_ms
        total_bytes += len(raw.encode("utf-8"))
        per_call.append((tool, elapsed_ms, len(raw.encode("utf-8"))))
    return {
        "name": name,
        "tool_calls": len(calls),
        "elapsed_ms": total_ms,
        "bytes": total_bytes,
        "per_call": per_call,
    }

legacy = run_suite("legacy", legacy_calls)
compact = run_suite("edit_plan_compact", edit_plan_calls)
detailed = run_suite("edit_plan_detailed", detailed_calls)

def pct_faster(base, candidate):
    if base <= 0:
        return 0.0
    return ((base - candidate) / base) * 100.0

def pct_smaller(base, candidate):
    if base <= 0:
        return 0.0
    return ((base - candidate) / base) * 100.0

print("suite                 calls  elapsed_ms  bytes")
for suite in (legacy, compact, detailed):
    print(f"{suite['name']:<20} {suite['tool_calls']:>5}  {suite['elapsed_ms']:>10.2f}  {suite['bytes']:>6}")

print("")
print(f"compact_vs_legacy_calls_saved: {legacy['tool_calls'] - compact['tool_calls']}")
print(f"compact_vs_legacy_elapsed_pct: {pct_faster(legacy['elapsed_ms'], compact['elapsed_ms']):.1f}%")
print(f"compact_vs_legacy_bytes_pct:   {pct_smaller(legacy['bytes'], compact['bytes']):.1f}%")
print(f"detailed_vs_legacy_calls_saved:{legacy['tool_calls'] - detailed['tool_calls']}")
print(f"detailed_vs_legacy_elapsed_pct:{pct_faster(legacy['elapsed_ms'], detailed['elapsed_ms']):.1f}%")
print(f"detailed_vs_legacy_bytes_pct:  {pct_smaller(legacy['bytes'], detailed['bytes']):.1f}%")

print("")
print("per_call_breakdown:")
for label, suite in (("legacy", legacy), ("compact", compact), ("detailed", detailed)):
    for tool, elapsed_ms, num_bytes in suite["per_call"]:
        print(f"  {label:<8} {tool:<18} {elapsed_ms:>10.2f} ms  {num_bytes:>6} bytes")
PY
