#!/usr/bin/env python3
"""Run the CI benchmark subset and write normalized result files."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path


CHARACTERISTIC_BENCHMARKS = [
    "BM_IterateEightComponents/16384",
    "BM_IterateEightComponentsOwnedGroupView/16384",
    "BM_IterateSparseTwoComponentView/16384/10",
    "BM_IterateWriteOwnedGroupViewEightComponents/16384",
    "BM_AccessViewReadWrite/16384",
    "BM_AddRemoveComponents/16384",
    "BM_GroupMaintenanceAddRemove/16384",
    "BM_Snapshot/16384",
    "BM_DeltaSnapshotDirtyValues/16384",
    "BM_DeltaSnapshotStructuralChanges/16384",
    "BM_DeltaSnapshotRestoreStructuralChanges/16384",
    "BM_RuntimeEnsureExistingWriteRead/16384",
    "BM_JobScheduleReadOnly/256",
    "BM_RunJobsSingleThread/16384",
    "BM_RunJobsParallelThreadPool/16384",
    "BM_RunJobsParallelOverlappingWritesThreadPool/16384",
]

REQUIRED_HARDWARE_THREADS = 4


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--benchmark-filter", default="")
    parser.add_argument(
        "--subset",
        default="characteristic",
        choices=("characteristic", "all"),
    )
    parser.add_argument("--commit", default=os.environ.get("GITHUB_SHA", "unknown"))
    parser.add_argument("--ref", default=os.environ.get("GITHUB_REF_NAME", "unknown"))
    parser.add_argument("--run-id", default=os.environ.get("GITHUB_RUN_ID", ""))
    parser.add_argument("--run-attempt", default=os.environ.get("GITHUB_RUN_ATTEMPT", ""))
    parser.add_argument("--repository", default=os.environ.get("GITHUB_REPOSITORY", ""))
    return parser.parse_args()


def benchmark_path(build_dir: Path) -> Path:
    path = build_dir / "benchmarks" / "basic_operations_benchmark"
    if not path.exists():
        raise FileNotFoundError(f"benchmark executable not found: {path}")
    return path


def benchmark_filter(args: argparse.Namespace) -> str:
    if args.benchmark_filter:
        return args.benchmark_filter
    if args.subset == "all":
        return "."
    return "^(" + "|".join(CHARACTERISTIC_BENCHMARKS) + ")$"


def run_command(command: list[str]) -> None:
    print("+ " + " ".join(command), flush=True)
    subprocess.run(command, check=True)


def require_hardware_threads() -> None:
    hardware_threads = os.cpu_count() or 0
    if hardware_threads < REQUIRED_HARDWARE_THREADS:
        raise RuntimeError(
            f"benchmark CI requires at least {REQUIRED_HARDWARE_THREADS} hardware threads; found {hardware_threads}"
        )


def summarize_google_benchmark(path: Path) -> list[dict[str, object]]:
    data = json.loads(path.read_text(encoding="utf-8"))
    benchmarks = []
    for item in data.get("benchmarks", []):
        if item.get("run_type") != "iteration":
            continue
        benchmarks.append(
            {
                "name": item.get("name"),
                "real_time": item.get("real_time"),
                "cpu_time": item.get("cpu_time"),
                "time_unit": item.get("time_unit"),
                "items_per_second": item.get("items_per_second"),
                "bytes_per_second": item.get("bytes_per_second"),
                "iterations": item.get("iterations"),
            }
        )
    return benchmarks


def main() -> int:
    args = parse_args()
    require_hardware_threads()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    (args.output_dir / "cpu").mkdir(exist_ok=True)

    started_at = datetime.now(timezone.utc).isoformat()
    filter_value = benchmark_filter(args)
    google_benchmark_json = args.output_dir / "cpu" / "google-benchmark.json"

    run_command(
        [
            str(benchmark_path(args.build_dir)),
            f"--benchmark_filter={filter_value}",
            "--benchmark_format=json",
            f"--benchmark_out={google_benchmark_json}",
            "--benchmark_out_format=json",
            "--benchmark_min_time=0.2s",
        ]
    )

    cpu_summary = summarize_google_benchmark(google_benchmark_json)
    (args.output_dir / "cpu" / "summary.json").write_text(
        json.dumps(cpu_summary, indent=2) + "\n",
        encoding="utf-8",
    )

    result = {
        "schema_version": 1,
        "commit": args.commit,
        "ref": args.ref,
        "repository": args.repository,
        "run_id": args.run_id,
        "run_attempt": args.run_attempt,
        "started_at": started_at,
        "finished_at": datetime.now(timezone.utc).isoformat(),
        "subset": args.subset,
        "benchmark_filter": filter_value,
        "cpu": cpu_summary,
    }
    (args.output_dir / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    metadata_keys = (
        "schema_version",
        "commit",
        "ref",
        "repository",
        "run_id",
        "run_attempt",
        "started_at",
        "finished_at",
        "subset",
    )
    (args.output_dir / "metadata.json").write_text(
        json.dumps({key: result[key] for key in metadata_keys}, indent=2) + "\n",
        encoding="utf-8",
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
