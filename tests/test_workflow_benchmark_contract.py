#!/usr/bin/env python3
"""Contract tests for the provider-neutral agent workflow benchmark."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "benchmark-agent-workflows.py"
FIXTURE_MANIFEST = (
    ROOT / "tests" / "fixtures" / "workflow-evidence" / "manifest.json"
)


def _task(task_id: str = "c-caller-001") -> dict[str, object]:
    prompt = "Identify the direct caller of add_one and name the narrowest regression test."
    return {
        "id": task_id,
        "language": "c",
        "category": "investigate",
        "risk_class": "high",
        "blast_radius": "small",
        "path_class": "hotspot",
        "prompt": prompt,
        "prompt_sha256": hashlib.sha256(prompt.encode("utf-8")).hexdigest(),
        "source_paths": ["tests/fixtures/workflow-evidence/c-small/src/math.c"],
        "time_limit_seconds": 300,
        "tool_call_budget": 12,
    }


def _manifest() -> dict[str, object]:
    return {
        "schema_version": 1,
        "manifest_state": "template",
        "suite_id": "workflow-evidence-v1",
        "arms": ["A", "B", "C"],
        "model": {"name": "fixture-model", "reasoning": "fixed"},
        "hardware": "fixture-hardware",
        "base_commit": "0" * 40,
        "tasks": [_task()],
    }


def _run_cli(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(SCRIPT), *args],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
    )


def _run_record(
    arm: str,
    task: dict[str, object] | None = None,
    manifest: dict[str, object] | None = None,
) -> dict[str, object]:
    selected_task = task or _task()
    selected_manifest = manifest or _manifest()
    model = selected_manifest["model"]
    assert isinstance(model, dict)
    return {
        "schema_version": 1,
        "suite_id": selected_manifest["suite_id"],
        "task_id": selected_task["id"],
        "run_id": f"{selected_task['id']}-{arm.lower()}",
        "arm": arm,
        "model": model["name"],
        "reasoning": model["reasoning"],
        "hardware": selected_manifest["hardware"],
        "language": selected_task["language"],
        "category": selected_task["category"],
        "risk_class": selected_task["risk_class"],
        "blast_radius": selected_task["blast_radius"],
        "path_class": selected_task["path_class"],
        "prompt_sha256": selected_task["prompt_sha256"],
        "base_commit": selected_manifest["base_commit"],
        "time_limit_seconds": selected_task["time_limit_seconds"],
        "tool_call_budget": selected_task["tool_call_budget"],
        "token_usage": {
            "accounting_method": "provider",
            "usage_source": "fixture-provider-usage",
            "tool_schema_tokens": 100,
            "tool_argument_tokens": 20,
            "tool_response_tokens": 200,
            "completion_tokens": 180,
            "total_tokens": 500,
        },
        "metrics": {
            "targeted_tests_pass": True,
            "hidden_tests_pass": True,
            "unnecessary_files": 0,
            "regression_escapes": 0,
            "wrong_project": False,
            "unsafe_empty": False,
            "contradiction": False,
            "latency_ms": 100.0,
            "single_primitive_latency_ms": 100.0,
            "compact_response_tokens": 200,
            "tool_calls_used": 5,
            "elapsed_seconds": 60.0,
            "timed_out": False,
        },
        "blind_score": {
            "reviewer_id_hash": "a" * 64,
            "score": 90,
        },
    }


def _promotion_manifest() -> dict[str, object]:
    manifest = _manifest()
    manifest["manifest_state"] = "frozen"
    manifest["base_commit"] = "1" * 40
    tasks: list[dict[str, object]] = []
    categories = ("fix", "refactor", "investigate")
    for index in range(30):
        task = _task(f"promotion-{index:02d}")
        prompt = f"Frozen reversible benchmark task {index:02d}."
        task.update(
            {
                "language": "c" if index % 2 == 0 else "typescript",
                "category": categories[index % len(categories)],
                "risk_class": "high" if index % 2 == 0 else "ordinary",
                "blast_radius": "medium" if index % 2 == 0 else "small",
                "path_class": "hotspot" if index % 2 == 0 else "ordinary",
                "prompt": prompt,
                "prompt_sha256": hashlib.sha256(prompt.encode("utf-8")).hexdigest(),
            }
        )
        tasks.append(task)
    manifest["tasks"] = tasks
    return manifest


def _promotion_records(manifest: dict[str, object]) -> list[dict[str, object]]:
    tasks = manifest["tasks"]
    assert isinstance(tasks, list)
    records: list[dict[str, object]] = []
    for task in tasks:
        assert isinstance(task, dict)
        for arm in ("A", "B", "C"):
            record = _run_record(arm, task, manifest)
            metrics = record["metrics"]
            token_usage = record["token_usage"]
            blind_score = record["blind_score"]
            assert isinstance(metrics, dict)
            assert isinstance(token_usage, dict)
            assert isinstance(blind_score, dict)
            if arm == "B":
                metrics.update(
                    {
                        "targeted_tests_pass": False,
                        "hidden_tests_pass": False,
                        "unnecessary_files": 2,
                        "regression_escapes": 1,
                        "latency_ms": 200.0,
                    }
                )
                token_usage["completion_tokens"] = 680
                token_usage["total_tokens"] = 1000
                blind_score["score"] = 70
            elif arm == "C":
                metrics.update(
                    {
                        "targeted_tests_pass": True,
                        "hidden_tests_pass": True,
                        "unnecessary_files": 1,
                        "regression_escapes": 0,
                        "latency_ms": 100.0,
                        "compact_response_tokens": 210,
                    }
                )
                token_usage["completion_tokens"] = 280
                token_usage["total_tokens"] = 600
                blind_score["score"] = 90
            records.append(record)
    return records


class ManifestContractTests(unittest.TestCase):
    def test_validate_manifest_accepts_minimal_reproducible_suite(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            manifest_path = Path(tmp) / "manifest.json"
            manifest_path.write_text(json.dumps(_manifest()), encoding="utf-8")

            result = _run_cli("validate-manifest", str(manifest_path))

        self.assertEqual(result.returncode, 0, result.stderr)
        payload = json.loads(result.stdout)
        self.assertEqual(payload["status"], "valid")
        self.assertEqual(payload["suite_id"], "workflow-evidence-v1")
        self.assertEqual(payload["task_count"], 1)

    def test_validate_manifest_rejects_absolute_source_path(self) -> None:
        manifest = _manifest()
        tasks = manifest["tasks"]
        assert isinstance(tasks, list)
        task = tasks[0]
        assert isinstance(task, dict)
        task["source_paths"] = [r"C:\private\service.c"]

        with tempfile.TemporaryDirectory() as tmp:
            manifest_path = Path(tmp) / "manifest.json"
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")

            result = _run_cli("validate-manifest", str(manifest_path))

        self.assertEqual(result.returncode, 2)
        payload = json.loads(result.stderr)
        self.assertEqual(payload["status"], "invalid")
        self.assertIn("repository-relative", payload["error"])

    def test_validate_manifest_rejects_absolute_labeled_paths(self) -> None:
        for field in ("expected_callers", "expected_tests"):
            with self.subTest(field=field), tempfile.TemporaryDirectory() as tmp:
                manifest = _manifest()
                tasks = manifest["tasks"]
                assert isinstance(tasks, list)
                task = tasks[0]
                assert isinstance(task, dict)
                task[field] = [r"C:\\private\\service.c"]
                manifest_path = Path(tmp) / "manifest.json"
                manifest_path.write_text(json.dumps(manifest), encoding="utf-8")

                result = _run_cli("validate-manifest", str(manifest_path))

                self.assertEqual(result.returncode, 2)
                self.assertIn("repository-relative", json.loads(result.stderr)["error"])

    def test_validate_manifest_rejects_prompt_hash_mismatch(self) -> None:
        manifest = _manifest()
        tasks = manifest["tasks"]
        assert isinstance(tasks, list)
        task = tasks[0]
        assert isinstance(task, dict)
        task["prompt_sha256"] = "f" * 64

        with tempfile.TemporaryDirectory() as tmp:
            manifest_path = Path(tmp) / "manifest.json"
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")

            result = _run_cli("validate-manifest", str(manifest_path))

        self.assertEqual(result.returncode, 2)
        payload = json.loads(result.stderr)
        self.assertIn("prompt_sha256", payload["error"])

    def test_validate_manifest_rejects_duplicate_task_id(self) -> None:
        manifest = _manifest()
        tasks = manifest["tasks"]
        assert isinstance(tasks, list)
        tasks.append(_task())
        with tempfile.TemporaryDirectory() as tmp:
            manifest_path = Path(tmp) / "manifest.json"
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")

            result = _run_cli("validate-manifest", str(manifest_path))

        self.assertEqual(result.returncode, 2)
        payload = json.loads(result.stderr)
        self.assertIn("duplicate task id", payload["error"])

    def test_validate_manifest_requires_frozen_protocol_metadata(self) -> None:
        for field in ("schema_version", "arms", "model", "base_commit"):
            with self.subTest(field=field), tempfile.TemporaryDirectory() as tmp:
                manifest = _manifest()
                del manifest[field]
                manifest_path = Path(tmp) / "manifest.json"
                manifest_path.write_text(json.dumps(manifest), encoding="utf-8")

                result = _run_cli("validate-manifest", str(manifest_path))

                self.assertEqual(result.returncode, 2)
                payload = json.loads(result.stderr)
                self.assertIn(field, payload["error"])

    def test_validate_manifest_requires_task_classification_and_budgets(self) -> None:
        for field in (
            "language",
            "category",
            "blast_radius",
            "path_class",
            "time_limit_seconds",
            "tool_call_budget",
        ):
            with self.subTest(field=field), tempfile.TemporaryDirectory() as tmp:
                manifest = _manifest()
                tasks = manifest["tasks"]
                assert isinstance(tasks, list)
                task = tasks[0]
                assert isinstance(task, dict)
                del task[field]
                manifest_path = Path(tmp) / "manifest.json"
                manifest_path.write_text(json.dumps(manifest), encoding="utf-8")

                result = _run_cli("validate-manifest", str(manifest_path))

                self.assertEqual(result.returncode, 2)
                payload = json.loads(result.stderr)
                self.assertIn(field, payload["error"])

    def test_validate_manifest_requires_hardware_and_risk_class(self) -> None:
        manifest = _manifest()
        del manifest["hardware"]
        with tempfile.TemporaryDirectory() as tmp:
            manifest_path = Path(tmp) / "manifest.json"
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            result = _run_cli("validate-manifest", str(manifest_path))
        self.assertEqual(result.returncode, 2)
        self.assertIn("hardware", json.loads(result.stderr)["error"])

        manifest = _manifest()
        tasks = manifest["tasks"]
        assert isinstance(tasks, list)
        task = tasks[0]
        assert isinstance(task, dict)
        del task["risk_class"]
        with tempfile.TemporaryDirectory() as tmp:
            manifest_path = Path(tmp) / "manifest.json"
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            result = _run_cli("validate-manifest", str(manifest_path))
        self.assertEqual(result.returncode, 2)
        self.assertIn("risk_class", json.loads(result.stderr)["error"])

    def test_repository_fixture_manifest_is_valid(self) -> None:
        result = _run_cli("validate-manifest", str(FIXTURE_MANIFEST))

        self.assertEqual(result.returncode, 0, result.stderr)
        payload = json.loads(result.stdout)
        self.assertEqual(payload["suite_id"], "workflow-evidence-public-v1")
        self.assertEqual(payload["manifest_state"], "template")
        self.assertGreaterEqual(payload["task_count"], 2)

    def test_canonical_test_harness_runs_workflow_benchmark_contract(self) -> None:
        canonical_harness = (ROOT / "scripts" / "test.sh").read_text(encoding="utf-8")

        self.assertIn(
            'python3 "$ROOT/tests/test_workflow_benchmark_contract.py"',
            canonical_harness,
        )


class RunContractTests(unittest.TestCase):
    def test_validate_runs_accepts_complete_comparable_abc_triplet(self) -> None:
        records = [_run_record(arm) for arm in ("A", "B", "C")]
        with tempfile.TemporaryDirectory() as tmp:
            runs_path = Path(tmp) / "runs.jsonl"
            runs_path.write_text(
                "".join(json.dumps(record) + "\n" for record in records),
                encoding="utf-8",
            )

            result = _run_cli("validate-runs", str(runs_path))

        self.assertEqual(result.returncode, 0, result.stderr)
        payload = json.loads(result.stdout)
        self.assertEqual(payload["status"], "valid")
        self.assertEqual(payload["record_count"], 3)
        self.assertEqual(payload["task_count"], 1)
        self.assertEqual(payload["arms"], ["A", "B", "C"])

    def test_validate_runs_rejects_missing_comparability_metadata(self) -> None:
        for field in ("model", "hardware", "risk_class"):
            with self.subTest(field=field), tempfile.TemporaryDirectory() as tmp:
                records = [_run_record(arm) for arm in ("A", "B", "C")]
                for record in records:
                    del record[field]
                runs_path = Path(tmp) / "runs.jsonl"
                runs_path.write_text(
                    "".join(json.dumps(record) + "\n" for record in records),
                    encoding="utf-8",
                )

                result = _run_cli("validate-runs", str(runs_path))

                self.assertEqual(result.returncode, 2)
                payload = json.loads(result.stderr)
                self.assertIn(field, payload["error"])

    def test_validate_runs_requires_token_accounting_method(self) -> None:
        records = [_run_record(arm) for arm in ("A", "B", "C")]
        for record in records:
            token_usage = record["token_usage"]
            assert isinstance(token_usage, dict)
            del token_usage["accounting_method"]
        with tempfile.TemporaryDirectory() as tmp:
            runs_path = Path(tmp) / "runs.jsonl"
            runs_path.write_text(
                "".join(json.dumps(record) + "\n" for record in records),
                encoding="utf-8",
            )
            result = _run_cli("validate-runs", str(runs_path))
        self.assertEqual(result.returncode, 2)
        self.assertIn("accounting_method", json.loads(result.stderr)["error"])

    def test_validate_runs_requires_execution_budget_evidence(self) -> None:
        records = [_run_record(arm) for arm in ("A", "B", "C")]
        for record in records:
            metrics = record["metrics"]
            assert isinstance(metrics, dict)
            del metrics["tool_calls_used"]
        with tempfile.TemporaryDirectory() as tmp:
            runs_path = Path(tmp) / "runs.jsonl"
            runs_path.write_text(
                "".join(json.dumps(record) + "\n" for record in records),
                encoding="utf-8",
            )
            result = _run_cli("validate-runs", str(runs_path))

        self.assertEqual(result.returncode, 2)
        self.assertIn("tool_calls_used", json.loads(result.stderr)["error"])

    def test_provider_token_accounting_requires_usage_source(self) -> None:
        records = [_run_record(arm) for arm in ("A", "B", "C")]
        for record in records:
            token_usage = record["token_usage"]
            assert isinstance(token_usage, dict)
            del token_usage["usage_source"]
        with tempfile.TemporaryDirectory() as tmp:
            runs_path = Path(tmp) / "runs.jsonl"
            runs_path.write_text(
                "".join(json.dumps(record) + "\n" for record in records),
                encoding="utf-8",
            )
            result = _run_cli("validate-runs", str(runs_path))

        self.assertEqual(result.returncode, 2)
        self.assertIn("usage_source", json.loads(result.stderr)["error"])

    def test_validate_runs_rejects_primitive_latency_mismatch(self) -> None:
        records = [_run_record(arm) for arm in ("A", "B", "C")]
        metrics = records[2]["metrics"]
        assert isinstance(metrics, dict)
        metrics["single_primitive_latency_ms"] = 120.0
        with tempfile.TemporaryDirectory() as tmp:
            runs_path = Path(tmp) / "runs.jsonl"
            runs_path.write_text(
                "".join(json.dumps(record) + "\n" for record in records),
                encoding="utf-8",
            )
            result = _run_cli("validate-runs", str(runs_path))
        self.assertEqual(result.returncode, 2)
        self.assertIn("single_primitive_latency_ms", json.loads(result.stderr)["error"])

    def test_validate_runs_rejects_non_finite_latency(self) -> None:
        records = [_run_record(arm) for arm in ("A", "B", "C")]
        metrics = records[2]["metrics"]
        assert isinstance(metrics, dict)
        metrics["latency_ms"] = float("nan")
        with tempfile.TemporaryDirectory() as tmp:
            runs_path = Path(tmp) / "runs.jsonl"
            runs_path.write_text(
                "".join(json.dumps(record) + "\n" for record in records),
                encoding="utf-8",
            )

            result = _run_cli("validate-runs", str(runs_path))

        self.assertEqual(result.returncode, 2)
        payload = json.loads(result.stderr)
        self.assertIn("finite", payload["error"])

    def test_validate_runs_accepts_manifest_bound_triplet(self) -> None:
        records = [_run_record(arm) for arm in ("A", "B", "C")]
        with tempfile.TemporaryDirectory() as tmp:
            manifest_path = Path(tmp) / "manifest.json"
            runs_path = Path(tmp) / "runs.jsonl"
            manifest_path.write_text(json.dumps(_manifest()), encoding="utf-8")
            runs_path.write_text(
                "".join(json.dumps(record) + "\n" for record in records),
                encoding="utf-8",
            )

            result = _run_cli(
                "validate-runs",
                str(runs_path),
                "--manifest",
                str(manifest_path),
            )

        self.assertEqual(result.returncode, 0, result.stderr)

    def test_validate_runs_rejects_prompt_drift_from_manifest(self) -> None:
        records = [_run_record(arm) for arm in ("A", "B", "C")]
        for record in records:
            record["prompt_sha256"] = "f" * 64
        with tempfile.TemporaryDirectory() as tmp:
            manifest_path = Path(tmp) / "manifest.json"
            runs_path = Path(tmp) / "runs.jsonl"
            manifest_path.write_text(json.dumps(_manifest()), encoding="utf-8")
            runs_path.write_text(
                "".join(json.dumps(record) + "\n" for record in records),
                encoding="utf-8",
            )

            result = _run_cli(
                "validate-runs",
                str(runs_path),
                "--manifest",
                str(manifest_path),
            )

        self.assertEqual(result.returncode, 2)
        payload = json.loads(result.stderr)
        self.assertIn("prompt_sha256", payload["error"])

    def test_validate_runs_rejects_cross_task_suite_metadata_drift(self) -> None:
        manifest = _promotion_manifest()
        tasks = manifest["tasks"]
        assert isinstance(tasks, list)
        first = tasks[0]
        second = tasks[1]
        assert isinstance(first, dict)
        assert isinstance(second, dict)
        records = [
            _run_record(arm, task, manifest)
            for task in (first, second)
            for arm in ("A", "B", "C")
        ]
        for record in records[3:]:
            record["model"] = "different-model"
        with tempfile.TemporaryDirectory() as tmp:
            runs_path = Path(tmp) / "runs.jsonl"
            runs_path.write_text(
                "".join(json.dumps(record) + "\n" for record in records),
                encoding="utf-8",
            )
            result = _run_cli("validate-runs", str(runs_path))

        self.assertEqual(result.returncode, 2)
        self.assertIn("benchmark suite", json.loads(result.stderr)["error"])

    def test_validate_runs_rejects_missing_safety_metric(self) -> None:
        records = [_run_record(arm) for arm in ("A", "B", "C")]
        metrics = records[2]["metrics"]
        assert isinstance(metrics, dict)
        del metrics["unsafe_empty"]
        with tempfile.TemporaryDirectory() as tmp:
            runs_path = Path(tmp) / "runs.jsonl"
            runs_path.write_text(
                "".join(json.dumps(record) + "\n" for record in records),
                encoding="utf-8",
            )

            result = _run_cli("validate-runs", str(runs_path))

        self.assertEqual(result.returncode, 2)
        payload = json.loads(result.stderr)
        self.assertIn("unsafe_empty", payload["error"])

    def test_validate_runs_rejects_missing_quality_metric(self) -> None:
        records = [_run_record(arm) for arm in ("A", "B", "C")]
        metrics = records[1]["metrics"]
        assert isinstance(metrics, dict)
        del metrics["hidden_tests_pass"]
        with tempfile.TemporaryDirectory() as tmp:
            runs_path = Path(tmp) / "runs.jsonl"
            runs_path.write_text(
                "".join(json.dumps(record) + "\n" for record in records),
                encoding="utf-8",
            )

            result = _run_cli("validate-runs", str(runs_path))

        self.assertEqual(result.returncode, 2)
        payload = json.loads(result.stderr)
        self.assertIn("hidden_tests_pass", payload["error"])

    def test_score_reports_quality_token_and_promotion_deltas(self) -> None:
        records = [_run_record(arm) for arm in ("A", "B", "C")]
        b_metrics = records[1]["metrics"]
        c_metrics = records[2]["metrics"]
        assert isinstance(b_metrics, dict)
        assert isinstance(c_metrics, dict)
        b_metrics.update(
            {
                "targeted_tests_pass": False,
                "hidden_tests_pass": False,
                "unnecessary_files": 2,
                "regression_escapes": 1,
                "latency_ms": 200.0,
                "compact_response_tokens": 200,
            }
        )
        c_metrics.update(
            {
                "hidden_tests_pass": True,
                "unnecessary_files": 1,
                "regression_escapes": 0,
                "latency_ms": 100.0,
                "compact_response_tokens": 210,
            }
        )
        b_tokens = records[1]["token_usage"]
        c_tokens = records[2]["token_usage"]
        assert isinstance(b_tokens, dict)
        assert isinstance(c_tokens, dict)
        b_tokens["completion_tokens"] = 680
        b_tokens["total_tokens"] = 1000
        c_tokens["completion_tokens"] = 280
        c_tokens["total_tokens"] = 600
        b_score = records[1]["blind_score"]
        c_score = records[2]["blind_score"]
        assert isinstance(b_score, dict)
        assert isinstance(c_score, dict)
        b_score["score"] = 70
        c_score["score"] = 90

        with tempfile.TemporaryDirectory() as tmp:
            runs_path = Path(tmp) / "runs.jsonl"
            summary_path = Path(tmp) / "summary.json"
            runs_path.write_text(
                "".join(json.dumps(record) + "\n" for record in records),
                encoding="utf-8",
            )

            result = _run_cli(
                "score",
                str(runs_path),
                "--output",
                str(summary_path),
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            summary = json.loads(summary_path.read_text(encoding="utf-8"))

        self.assertEqual(summary["arms"]["B"]["mean_total_tokens"], 1000.0)
        self.assertEqual(summary["arms"]["C"]["mean_total_tokens"], 600.0)
        comparison = summary["comparison"]["C_vs_B"]
        self.assertEqual(comparison["total_token_reduction_percent"], 40.0)
        self.assertEqual(comparison["targeted_test_pass_delta_points"], 100.0)
        self.assertEqual(comparison["hidden_test_pass_delta_points"], 100.0)
        self.assertEqual(comparison["mean_blind_score_delta_points"], 20.0)
        self.assertEqual(summary["arms"]["B"]["mean_tool_schema_tokens"], 100.0)
        gates = summary["promotion"]["gates"]
        self.assertTrue(gates["targeted_test_delta_at_least_10_points"])
        self.assertTrue(gates["hidden_test_delta_ci_nonnegative"])
        self.assertTrue(gates["high_risk_no_regression"])
        self.assertTrue(gates["latency_within_1_2x_single_primitive"])
        self.assertFalse(gates["manifest_bound"])
        self.assertFalse(summary["promotion"]["passed"])

    def test_score_promotes_only_frozen_stratified_manifest(self) -> None:
        manifest = _promotion_manifest()
        records = _promotion_records(manifest)
        with tempfile.TemporaryDirectory() as tmp:
            manifest_path = Path(tmp) / "manifest.json"
            runs_path = Path(tmp) / "runs.jsonl"
            summary_path = Path(tmp) / "summary.json"
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            runs_path.write_text(
                "".join(json.dumps(record) + "\n" for record in records),
                encoding="utf-8",
            )
            result = _run_cli(
                "score",
                str(runs_path),
                "--manifest",
                str(manifest_path),
                "--output",
                str(summary_path),
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            summary = json.loads(summary_path.read_text(encoding="utf-8"))

        self.assertTrue(summary["promotion"]["passed"])
        gates = summary["promotion"]["gates"]
        self.assertTrue(gates["manifest_bound"])
        self.assertTrue(gates["manifest_frozen"])
        self.assertTrue(gates["task_count_between_30_and_50"])
        self.assertTrue(gates["task_set_stratified"])
        provenance = summary["provenance"]
        self.assertEqual(provenance["manifest_state"], "frozen")
        self.assertRegex(provenance["manifest_sha256"], r"^[0-9a-f]{64}$")
        self.assertEqual(
            provenance["token_accounting"],
            [
                {
                    "accounting_method": "provider",
                    "source": "fixture-provider-usage",
                }
            ],
        )
        comparison = summary["comparison"]["C_vs_B"]
        self.assertGreaterEqual(comparison["blind_score_delta_ci95_points"]["low"], 0)
        self.assertGreaterEqual(
            comparison["unnecessary_file_reduction_ci95_count"]["low"], 0
        )
        self.assertGreaterEqual(
            comparison["regression_escape_reduction_ci95_count"]["low"], 0
        )

    def test_score_blocks_budget_overrun_even_when_quality_gates_pass(self) -> None:
        manifest = _promotion_manifest()
        records = _promotion_records(manifest)
        metrics = records[-1]["metrics"]
        assert isinstance(metrics, dict)
        metrics["tool_calls_used"] = 13
        with tempfile.TemporaryDirectory() as tmp:
            manifest_path = Path(tmp) / "manifest.json"
            runs_path = Path(tmp) / "runs.jsonl"
            summary_path = Path(tmp) / "summary.json"
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            runs_path.write_text(
                "".join(json.dumps(record) + "\n" for record in records),
                encoding="utf-8",
            )
            result = _run_cli(
                "score",
                str(runs_path),
                "--manifest",
                str(manifest_path),
                "--output",
                str(summary_path),
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            summary = json.loads(summary_path.read_text(encoding="utf-8"))

        self.assertFalse(
            summary["promotion"]["gates"]["all_runs_within_execution_budgets"]
        )
        self.assertFalse(summary["promotion"]["passed"])

    def test_score_rejects_thirty_unstratified_tasks(self) -> None:
        manifest = _promotion_manifest()
        tasks = manifest["tasks"]
        assert isinstance(tasks, list)
        for task in tasks:
            assert isinstance(task, dict)
            task["category"] = "fix"
        records = _promotion_records(manifest)
        with tempfile.TemporaryDirectory() as tmp:
            manifest_path = Path(tmp) / "manifest.json"
            runs_path = Path(tmp) / "runs.jsonl"
            summary_path = Path(tmp) / "summary.json"
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            runs_path.write_text(
                "".join(json.dumps(record) + "\n" for record in records),
                encoding="utf-8",
            )
            result = _run_cli(
                "score",
                str(runs_path),
                "--manifest",
                str(manifest_path),
                "--output",
                str(summary_path),
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            summary = json.loads(summary_path.read_text(encoding="utf-8"))

        gates = summary["promotion"]["gates"]
        self.assertTrue(gates["task_count_between_30_and_50"])
        self.assertFalse(gates["task_set_stratified"])
        self.assertFalse(summary["promotion"]["passed"])

    def test_bound_template_manifest_remains_non_promotable(self) -> None:
        manifest = _manifest()
        records = [_run_record(arm, _task(), manifest) for arm in ("A", "B", "C")]
        with tempfile.TemporaryDirectory() as tmp:
            manifest_path = Path(tmp) / "manifest.json"
            runs_path = Path(tmp) / "runs.jsonl"
            summary_path = Path(tmp) / "summary.json"
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            runs_path.write_text(
                "".join(json.dumps(record) + "\n" for record in records),
                encoding="utf-8",
            )
            result = _run_cli(
                "score",
                str(runs_path),
                "--manifest",
                str(manifest_path),
                "--output",
                str(summary_path),
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            summary = json.loads(summary_path.read_text(encoding="utf-8"))

        gates = summary["promotion"]["gates"]
        self.assertTrue(gates["manifest_bound"])
        self.assertFalse(gates["manifest_frozen"])
        self.assertFalse(summary["promotion"]["passed"])

    def test_score_without_frozen_manifest_is_never_promotable(self) -> None:
        records = [_run_record(arm) for arm in ("A", "B", "C")]
        with tempfile.TemporaryDirectory() as tmp:
            runs_path = Path(tmp) / "runs.jsonl"
            summary_path = Path(tmp) / "summary.json"
            runs_path.write_text(
                "".join(json.dumps(record) + "\n" for record in records),
                encoding="utf-8",
            )
            result = _run_cli(
                "score", str(runs_path), "--output", str(summary_path)
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            summary = json.loads(summary_path.read_text(encoding="utf-8"))

        gates = summary["promotion"]["gates"]
        self.assertFalse(gates["manifest_bound"])
        self.assertFalse(gates["manifest_frozen"])
        self.assertFalse(summary["promotion"]["passed"])

    def test_score_requires_promotion_sized_stratified_task_set(self) -> None:
        records = [_run_record(arm) for arm in ("A", "B", "C")]
        with tempfile.TemporaryDirectory() as tmp:
            runs_path = Path(tmp) / "runs.jsonl"
            summary_path = Path(tmp) / "summary.json"
            runs_path.write_text(
                "".join(json.dumps(record) + "\n" for record in records),
                encoding="utf-8",
            )
            result = _run_cli(
                "score", str(runs_path), "--output", str(summary_path)
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            summary = json.loads(summary_path.read_text(encoding="utf-8"))

        gates = summary["promotion"]["gates"]
        self.assertFalse(gates["task_count_between_30_and_50"])
        self.assertFalse(gates["task_set_stratified"])

    def test_high_risk_gate_rejects_file_and_blind_score_regressions(self) -> None:
        records = [_run_record(arm) for arm in ("A", "B", "C")]
        b_metrics = records[1]["metrics"]
        c_metrics = records[2]["metrics"]
        b_score = records[1]["blind_score"]
        c_score = records[2]["blind_score"]
        assert isinstance(b_metrics, dict)
        assert isinstance(c_metrics, dict)
        assert isinstance(b_score, dict)
        assert isinstance(c_score, dict)
        b_metrics["unnecessary_files"] = 1
        c_metrics["unnecessary_files"] = 2
        b_score["score"] = 90
        c_score["score"] = 80

        with tempfile.TemporaryDirectory() as tmp:
            runs_path = Path(tmp) / "runs.jsonl"
            summary_path = Path(tmp) / "summary.json"
            runs_path.write_text(
                "".join(json.dumps(record) + "\n" for record in records),
                encoding="utf-8",
            )
            result = _run_cli(
                "score", str(runs_path), "--output", str(summary_path)
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            summary = json.loads(summary_path.read_text(encoding="utf-8"))

        self.assertFalse(summary["promotion"]["gates"]["high_risk_no_regression"])

    def test_score_reports_order_invariant_paired_confidence_interval(self) -> None:
        records = [_run_record(arm) for arm in ("A", "B", "C")]
        b_metrics = records[1]["metrics"]
        assert isinstance(b_metrics, dict)
        b_metrics["hidden_tests_pass"] = False

        summaries: list[dict[str, object]] = []
        with tempfile.TemporaryDirectory() as tmp:
            for index, ordered_records in enumerate((records, list(reversed(records)))):
                runs_path = Path(tmp) / f"runs-{index}.jsonl"
                summary_path = Path(tmp) / f"summary-{index}.json"
                runs_path.write_text(
                    "".join(json.dumps(record) + "\n" for record in ordered_records),
                    encoding="utf-8",
                )
                result = _run_cli(
                    "score",
                    str(runs_path),
                    "--output",
                    str(summary_path),
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                summaries.append(json.loads(summary_path.read_text(encoding="utf-8")))

        first_comparison = summaries[0]["comparison"]
        second_comparison = summaries[1]["comparison"]
        assert isinstance(first_comparison, dict)
        assert isinstance(second_comparison, dict)
        first_delta = first_comparison["C_vs_B"]
        second_delta = second_comparison["C_vs_B"]
        assert isinstance(first_delta, dict)
        assert isinstance(second_delta, dict)
        self.assertEqual(
            first_delta.get("hidden_test_pass_delta_ci95_points"),
            {"low": 100.0, "high": 100.0},
        )
        self.assertEqual(first_delta, second_delta)

    def test_score_handles_zero_primitive_response_budget(self) -> None:
        records = [_run_record(arm) for arm in ("A", "B", "C")]
        for record in records:
            metrics = record["metrics"]
            assert isinstance(metrics, dict)
            metrics["compact_response_tokens"] = 0
        with tempfile.TemporaryDirectory() as tmp:
            runs_path = Path(tmp) / "runs.jsonl"
            summary_path = Path(tmp) / "summary.json"
            runs_path.write_text(
                "".join(json.dumps(record) + "\n" for record in records),
                encoding="utf-8",
            )

            result = _run_cli(
                "score",
                str(runs_path),
                "--output",
                str(summary_path),
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            summary = json.loads(summary_path.read_text(encoding="utf-8"))

        comparison = summary["comparison"]["C_vs_B"]
        self.assertEqual(comparison["compact_response_increase_percent"], 0.0)


if __name__ == "__main__":
    unittest.main()
