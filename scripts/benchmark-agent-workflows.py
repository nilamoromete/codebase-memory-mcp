#!/usr/bin/env python3
"""Validate and score provider-neutral coding-agent workflow benchmarks."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path, PurePosixPath, PureWindowsPath
import random
import re
from statistics import mean
import sys
from typing import Any


ARMS = ("A", "B", "C")
COMPARABILITY_FIELDS = (
    "suite_id",
    "task_id",
    "model",
    "reasoning",
    "prompt_sha256",
    "base_commit",
    "time_limit_seconds",
    "tool_call_budget",
)
TOKEN_COMPONENTS = (
    "tool_schema_tokens",
    "tool_argument_tokens",
    "tool_response_tokens",
    "completion_tokens",
)
BOOLEAN_METRICS = (
    "targeted_tests_pass",
    "hidden_tests_pass",
    "wrong_project",
    "unsafe_empty",
    "contradiction",
)
COUNT_METRICS = (
    "unnecessary_files",
    "regression_escapes",
    "compact_response_tokens",
)


def _load_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as handle:
        value = json.load(handle)
    if not isinstance(value, dict):
        raise ValueError("manifest root must be a JSON object")
    return value


def _load_jsonl(path: Path) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    with path.open("r", encoding="utf-8") as handle:
        for line_number, raw_line in enumerate(handle, start=1):
            line = raw_line.strip()
            if not line:
                continue
            value = json.loads(line)
            if not isinstance(value, dict):
                raise ValueError(f"line {line_number} must be a JSON object")
            records.append(value)
    if not records:
        raise ValueError("runs file must contain at least one record")
    return records


def _require_non_empty_string(value: Any, field: str) -> None:
    if not isinstance(value, str) or not value:
        raise ValueError(f"{field} must be a non-empty string")


def _require_positive_integer(value: Any, field: str) -> None:
    if not isinstance(value, int) or isinstance(value, bool) or value <= 0:
        raise ValueError(f"{field} must be a positive integer")


def _require_hex_digest(value: Any, length: int, field: str) -> None:
    if not isinstance(value, str) or not re.fullmatch(rf"[0-9a-f]{{{length}}}", value):
        raise ValueError(f"{field} must be {length} lowercase hex characters")


def validate_manifest(path: Path) -> dict[str, Any]:
    manifest = _load_json(path)
    if manifest.get("schema_version") != 1:
        raise ValueError("schema_version must be 1")
    if manifest.get("arms") != list(ARMS):
        raise ValueError(f"arms must be exactly {list(ARMS)}")
    model = manifest.get("model")
    if not isinstance(model, dict):
        raise ValueError("model must be an object")
    _require_non_empty_string(model.get("name"), "model.name")
    _require_non_empty_string(model.get("reasoning"), "model.reasoning")
    _require_hex_digest(manifest.get("base_commit"), 40, "base_commit")
    suite_id = manifest.get("suite_id")
    tasks = manifest.get("tasks")
    if not isinstance(suite_id, str) or not suite_id:
        raise ValueError("suite_id must be a non-empty string")
    if not isinstance(tasks, list) or not tasks:
        raise ValueError("tasks must be a non-empty array")
    seen_task_ids: set[str] = set()
    for index, task in enumerate(tasks):
        if not isinstance(task, dict):
            raise ValueError(f"tasks[{index}] must be an object")
        task_id = task.get("id")
        if not isinstance(task_id, str) or not task_id:
            raise ValueError(f"tasks[{index}].id must be a non-empty string")
        if task_id in seen_task_ids:
            raise ValueError(f"duplicate task id: {task_id}")
        seen_task_ids.add(task_id)
        prompt = task.get("prompt")
        prompt_sha256 = task.get("prompt_sha256")
        _require_non_empty_string(task.get("language"), f"tasks[{index}].language")
        category = task.get("category")
        if category not in ("fix", "refactor", "investigate"):
            raise ValueError(
                f"tasks[{index}].category must be fix, refactor, or investigate"
            )
        _require_positive_integer(
            task.get("time_limit_seconds"), f"tasks[{index}].time_limit_seconds"
        )
        _require_positive_integer(
            task.get("tool_call_budget"), f"tasks[{index}].tool_call_budget"
        )
        if not isinstance(prompt, str) or not prompt:
            raise ValueError(f"tasks[{index}].prompt must be a non-empty string")
        expected_prompt_hash = hashlib.sha256(prompt.encode("utf-8")).hexdigest()
        if prompt_sha256 != expected_prompt_hash:
            raise ValueError(f"tasks[{index}].prompt_sha256 does not match prompt")
        source_paths = task.get("source_paths")
        if not isinstance(source_paths, list) or not source_paths:
            raise ValueError(f"tasks[{index}].source_paths must be a non-empty array")
        for source_path in source_paths:
            if not isinstance(source_path, str):
                raise ValueError(f"tasks[{index}].source_paths must contain strings")
            normalized = PurePosixPath(source_path.replace("\\", "/"))
            if (
                PureWindowsPath(source_path).is_absolute()
                or normalized.is_absolute()
                or ".." in normalized.parts
            ):
                raise ValueError(
                    f"tasks[{index}].source_paths must be repository-relative"
                )
    return {"status": "valid", "suite_id": suite_id, "task_count": len(tasks)}


def validate_runs(path: Path) -> dict[str, Any]:
    records = _load_jsonl(path)
    grouped: dict[str, dict[str, dict[str, Any]]] = {}
    seen_run_ids: set[str] = set()
    for index, record in enumerate(records):
        if record.get("schema_version") != 1:
            raise ValueError(f"record {index}.schema_version must be 1")
        run_id = record.get("run_id")
        _require_non_empty_string(run_id, f"record {index}.run_id")
        if run_id in seen_run_ids:
            raise ValueError(f"duplicate run_id: {run_id}")
        seen_run_ids.add(run_id)
        task_id = record.get("task_id")
        arm = record.get("arm")
        if not isinstance(task_id, str) or not task_id:
            raise ValueError(f"record {index}.task_id must be a non-empty string")
        if arm not in ARMS:
            raise ValueError(f"record {index}.arm must be one of {ARMS}")
        by_arm = grouped.setdefault(task_id, {})
        if arm in by_arm:
            raise ValueError(f"duplicate record for task {task_id} arm {arm}")
        by_arm[arm] = record

        for field in ("suite_id", "model", "reasoning"):
            _require_non_empty_string(record.get(field), f"record {index}.{field}")
        _require_hex_digest(
            record.get("prompt_sha256"), 64, f"record {index}.prompt_sha256"
        )
        _require_hex_digest(record.get("base_commit"), 40, f"record {index}.base_commit")
        _require_positive_integer(
            record.get("time_limit_seconds"), f"record {index}.time_limit_seconds"
        )
        _require_positive_integer(
            record.get("tool_call_budget"), f"record {index}.tool_call_budget"
        )

        token_usage = record.get("token_usage")
        if not isinstance(token_usage, dict):
            raise ValueError(f"record {index}.token_usage must be an object")
        values: list[int] = []
        for field in TOKEN_COMPONENTS:
            value = token_usage.get(field)
            if not isinstance(value, int) or isinstance(value, bool) or value < 0:
                raise ValueError(
                    f"record {index}.token_usage.{field} must be a non-negative integer"
                )
            values.append(value)
        total = token_usage.get("total_tokens")
        if total != sum(values):
            raise ValueError(
                f"record {index}.token_usage.total_tokens must equal component sum"
            )
        metrics = record.get("metrics")
        if not isinstance(metrics, dict):
            raise ValueError(f"record {index}.metrics must be an object")
        for field in BOOLEAN_METRICS:
            if not isinstance(metrics.get(field), bool):
                raise ValueError(f"record {index}.metrics.{field} must be a boolean")
        for field in COUNT_METRICS:
            value = metrics.get(field)
            if not isinstance(value, int) or isinstance(value, bool) or value < 0:
                raise ValueError(
                    f"record {index}.metrics.{field} must be a non-negative integer"
                )
        latency_ms = metrics.get("latency_ms")
        if (
            not isinstance(latency_ms, (int, float))
            or isinstance(latency_ms, bool)
            or not math.isfinite(float(latency_ms))
            or latency_ms < 0
        ):
            raise ValueError(
                f"record {index}.metrics.latency_ms must be a finite non-negative number"
            )
        blind_score = record.get("blind_score")
        if not isinstance(blind_score, dict):
            raise ValueError(f"record {index}.blind_score must be an object")
        reviewer_hash = blind_score.get("reviewer_id_hash")
        if not isinstance(reviewer_hash, str) or not re.fullmatch(
            r"[0-9a-f]{64}", reviewer_hash
        ):
            raise ValueError(
                f"record {index}.blind_score.reviewer_id_hash must be 64 lowercase hex characters"
            )
        score = blind_score.get("score")
        if (
            not isinstance(score, (int, float))
            or isinstance(score, bool)
            or not math.isfinite(float(score))
            or not 0 <= score <= 100
        ):
            raise ValueError(
                f"record {index}.blind_score.score must be between 0 and 100"
            )

    for task_id, by_arm in grouped.items():
        if set(by_arm) != set(ARMS):
            raise ValueError(f"task {task_id} must contain exactly arms {ARMS}")
        reference = by_arm["A"]
        for arm in ARMS[1:]:
            candidate = by_arm[arm]
            for field in COMPARABILITY_FIELDS:
                if candidate.get(field) != reference.get(field):
                    raise ValueError(
                        f"task {task_id} field {field} differs between arms A and {arm}"
                    )

    return {
        "status": "valid",
        "record_count": len(records),
        "task_count": len(grouped),
        "arms": list(ARMS),
    }


def _percentile(values: list[float], percentile: float) -> float:
    ordered = sorted(values)
    index = max(0, math.ceil(percentile * len(ordered)) - 1)
    return float(ordered[index])


def _arm_summary(records: list[dict[str, Any]]) -> dict[str, Any]:
    metrics = [record["metrics"] for record in records]
    token_usage = [record["token_usage"] for record in records]
    return {
        "run_count": len(records),
        "targeted_test_pass_rate": round(
            mean(1.0 if item["targeted_tests_pass"] else 0.0 for item in metrics), 6
        ),
        "hidden_test_pass_rate": round(
            mean(1.0 if item["hidden_tests_pass"] else 0.0 for item in metrics), 6
        ),
        "mean_unnecessary_files": round(
            mean(float(item["unnecessary_files"]) for item in metrics), 6
        ),
        "mean_regression_escapes": round(
            mean(float(item["regression_escapes"]) for item in metrics), 6
        ),
        "wrong_project_count": sum(1 for item in metrics if item["wrong_project"]),
        "unsafe_empty_count": sum(1 for item in metrics if item["unsafe_empty"]),
        "contradiction_count": sum(1 for item in metrics if item["contradiction"]),
        "mean_total_tokens": round(
            mean(float(item["total_tokens"]) for item in token_usage), 6
        ),
        "p95_latency_ms": round(
            _percentile([float(item["latency_ms"]) for item in metrics], 0.95), 6
        ),
        "p95_compact_response_tokens": round(
            _percentile(
                [float(item["compact_response_tokens"]) for item in metrics], 0.95
            ),
            6,
        ),
        "mean_blind_score": round(
            mean(float(record["blind_score"]["score"]) for record in records), 6
        ),
    }


def _reduction_percent(baseline: float, candidate: float) -> float:
    if baseline == 0:
        return 0.0 if candidate == 0 else -100.0
    return round((baseline - candidate) * 100.0 / baseline, 6)


def _increase_percent(baseline: float, candidate: float) -> float:
    if baseline == 0:
        return 0.0 if candidate == 0 else 100.0
    return round((candidate - baseline) * 100.0 / baseline, 6)


def _paired_boolean_delta_ci(
    baseline_records: list[dict[str, Any]],
    candidate_records: list[dict[str, Any]],
    metric: str,
    iterations: int = 2000,
) -> dict[str, float]:
    baseline_by_task = {record["task_id"]: record for record in baseline_records}
    candidate_by_task = {record["task_id"]: record for record in candidate_records}
    task_ids = sorted(baseline_by_task)
    deltas = [
        (
            1.0
            if candidate_by_task[task_id]["metrics"][metric]
            else 0.0
        )
        - (
            1.0
            if baseline_by_task[task_id]["metrics"][metric]
            else 0.0
        )
        for task_id in task_ids
    ]
    rng = random.Random(1734)
    samples = [
        mean(deltas[rng.randrange(len(deltas))] for _ in deltas)
        for _ in range(iterations)
    ]
    return {
        "low": round(_percentile(samples, 0.025) * 100.0, 6),
        "high": round(_percentile(samples, 0.975) * 100.0, 6),
    }


def score_runs(path: Path) -> dict[str, Any]:
    validation = validate_runs(path)
    records = _load_jsonl(path)
    by_arm = {
        arm: [record for record in records if record["arm"] == arm] for arm in ARMS
    }
    summaries = {arm: _arm_summary(by_arm[arm]) for arm in ARMS}
    baseline = summaries["B"]
    candidate = summaries["C"]
    comparison = {
        "total_token_reduction_percent": _reduction_percent(
            baseline["mean_total_tokens"], candidate["mean_total_tokens"]
        ),
        "hidden_test_pass_delta_points": round(
            (candidate["hidden_test_pass_rate"] - baseline["hidden_test_pass_rate"])
            * 100.0,
            6,
        ),
        "hidden_test_pass_delta_ci95_points": _paired_boolean_delta_ci(
            by_arm["B"], by_arm["C"], "hidden_tests_pass"
        ),
        "unnecessary_file_reduction_percent": _reduction_percent(
            baseline["mean_unnecessary_files"], candidate["mean_unnecessary_files"]
        ),
        "regression_escape_reduction_percent": _reduction_percent(
            baseline["mean_regression_escapes"],
            candidate["mean_regression_escapes"],
        ),
        "latency_reduction_percent": _reduction_percent(
            baseline["p95_latency_ms"], candidate["p95_latency_ms"]
        ),
        "compact_response_increase_percent": _increase_percent(
            baseline["p95_compact_response_tokens"],
            candidate["p95_compact_response_tokens"],
        ),
    }
    gates = {
        "wrong_project_zero": candidate["wrong_project_count"] == 0,
        "unsafe_empty_zero": candidate["unsafe_empty_count"] == 0,
        "contradiction_zero": candidate["contradiction_count"] == 0,
        "hidden_test_delta_at_least_10_points": (
            comparison["hidden_test_pass_delta_points"] >= 10.0
        ),
        "unnecessary_files_reduced_at_least_20_percent": (
            candidate["mean_unnecessary_files"]
            <= baseline["mean_unnecessary_files"] * 0.8
        ),
        "regression_escapes_reduced_at_least_20_percent": (
            candidate["mean_regression_escapes"]
            <= baseline["mean_regression_escapes"] * 0.8
        ),
        "total_tokens_reduced_at_least_30_percent": (
            comparison["total_token_reduction_percent"] >= 30.0
        ),
        "compact_response_within_15_percent": (
            candidate["p95_compact_response_tokens"]
            <= baseline["p95_compact_response_tokens"] * 1.15
        ),
        "latency_reduced_at_least_40_percent": (
            comparison["latency_reduction_percent"] >= 40.0
        ),
    }
    return {
        "schema_version": 1,
        "status": "scored",
        "record_count": validation["record_count"],
        "task_count": validation["task_count"],
        "arms": summaries,
        "comparison": {"C_vs_B": comparison},
        "promotion": {"passed": all(gates.values()), "gates": gates},
    }


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    validate = subparsers.add_parser("validate-manifest")
    validate.add_argument("manifest", type=Path)
    validate_runs_parser = subparsers.add_parser("validate-runs")
    validate_runs_parser.add_argument("runs", type=Path)
    score = subparsers.add_parser("score")
    score.add_argument("runs", type=Path)
    score.add_argument("--output", type=Path, required=True)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command == "validate-manifest":
            result = validate_manifest(args.manifest)
        elif args.command == "validate-runs":
            result = validate_runs(args.runs)
        elif args.command == "score":
            result = score_runs(args.runs)
            args.output.write_text(
                json.dumps(result, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
        else:
            raise ValueError(f"unsupported command: {args.command}")
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(json.dumps({"status": "invalid", "error": str(exc)}), file=sys.stderr)
        return 2
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
