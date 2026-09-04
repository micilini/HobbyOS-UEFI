#!/usr/bin/env python3
"""Offline FIX13 evidence and signed modal-heap oracle validation."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import pathlib
import re
import subprocess
import sys
from typing import Any


ROOT = pathlib.Path(__file__).resolve().parents[1]
ARTIFACT_DIR = ROOT / "artifacts" / "build"
BASE = "4e700a9b38653fb57ecb0f107a18f87508db0736"
EXPECTED_RUNTIME_MANIFEST = (
    "d8dd183b4b2ef247bd766464e2e09dc88addbeb08c66b373dd6e9fb199b01d09"
)
FAULT_RE = re.compile(
    r"(?:\bpanic\b|#PF|#GP|\bFATAL\b|"
    r"\[REAPER\]\[STRUCTURAL_FAULT\]|\[SCHED\]\[FINISH_FAULT\])",
    re.IGNORECASE,
)


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def source_at_base(path: pathlib.Path | None) -> tuple[str, str]:
    if path is not None:
        return path.read_text(encoding="utf-8"), str(path)
    source = subprocess.run(
        ["git", "show", f"{BASE}:kernel/src/shell/commands/cmd_modaltest.c"],
        cwd=ROOT,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
    ).stdout
    return source, f"git:{BASE}:kernel/src/shell/commands/cmd_modaltest.c"


def line_number(lines: list[str], marker: str, start: int = 0) -> int:
    for index in range(start, len(lines)):
        if marker in lines[index]:
            return index + 1
    raise ValueError(f"missing marker: {marker}")


def parse_stage_timestamp(tsv: pathlib.Path, stage: str) -> dt.datetime:
    rows = tsv.read_text(encoding="utf-8").splitlines()
    if not rows or rows[0].split("\t")[:3] != [
        "stage", "status", "host_timestamp"
    ]:
        raise ValueError("invalid stage TSV header")
    for row in rows[1:]:
        fields = row.split("\t")
        if fields and fields[0] == stage:
            return dt.datetime.fromisoformat(fields[2])
    raise ValueError(f"missing stage timestamp: {stage}")


def verify_old_run(args: argparse.Namespace) -> int:
    log_path = pathlib.Path(args.log)
    stage_path = pathlib.Path(args.stages)
    manifest_path = pathlib.Path(args.runtime_manifest)
    source, source_name = source_at_base(
        pathlib.Path(args.old_source) if args.old_source else None
    )
    lines = log_path.read_text(encoding="utf-8", errors="replace").splitlines()
    text = "\n".join(lines)

    begin_line = line_number(lines, "[HARNESS][BEGIN] seq=26")
    warmup_create_line = line_number(
        lines, "[TASK][CREATE] id=30054 name=modaltest-ui", begin_line - 1
    )
    warmup_free_line = line_number(
        lines, "[REAPER] FREE id=30054", warmup_create_line
    )
    last_worker_create_line = line_number(
        lines, "[TASK][CREATE] id=31054 name=modaltest-ui", warmup_create_line
    )
    last_worker_free_line = line_number(
        lines, "[REAPER] FREE id=31054", last_worker_create_line
    )
    fail_line = line_number(
        lines,
        "[MODALTEST][OPEN_CLOSE] FAIL cycles=1000 cleanup=1000 heap_drift=16640",
        last_worker_free_line,
    )
    end_line = line_number(
        lines, "[HARNESS][END] seq=26 status=1", fail_line
    )

    frame = "\n".join(lines[begin_line - 1 : end_line])
    create_markers = frame.count("[TASK][CREATE] id=")
    free_markers = frame.count("[REAPER] FREE id=")
    fault_markers = FAULT_RE.findall(text)
    boot_time = parse_stage_timestamp(stage_path, "boot")
    modal_time = parse_stage_timestamp(stage_path, "modal")
    elapsed_seconds = (modal_time - boot_time).total_seconds()
    manifest_hash = sha256(manifest_path)

    source_break = re.search(
        r"if\s*\(\s*!scheduler_reap_zombies\(0\)\s*\)\s*break\s*;",
        source,
        re.MULTILINE,
    ) is not None
    source_abs = (
        "drift < 0 ? (uint64_t)-drift : (uint64_t)drift" in source
        and 'serial_write_all(" heap_drift=")' in source
    )
    checks = {
        "begin_line": begin_line == 1657,
        "warmup_create_line": warmup_create_line == 1658,
        "warmup_free_line": warmup_free_line == 2256,
        "last_worker_free_line": last_worker_free_line == 3675,
        "modal_fail_line": fail_line == 3691,
        "end_line": end_line == 3693,
        "warmup_late_reap": warmup_free_line > warmup_create_line + 1,
        "cycle_range": last_worker_create_line > warmup_create_line,
        "create_marker_count": create_markers == 1001,
        "free_marker_count": free_markers == 1001,
        "cycles_cleanup": "cycles=1000 cleanup=1000" in lines[fail_line - 1],
        "absolute_drift": source_abs,
        "legacy_break_on_zero": source_break,
        "faults_zero": len(fault_markers) == 0,
        "elapsed_about_11m25": abs(elapsed_seconds - 685.0) <= 15.0,
        "runtime_manifest": manifest_hash == EXPECTED_RUNTIME_MANIFEST,
    }
    passed = all(checks.values())
    result: dict[str, Any] = {
        "status": "PASS" if passed else "FAIL",
        "classification": "REJECTED_CL13_MODALTEST_WARMUP_BASELINE_CONTAMINATED",
        "log": str(log_path),
        "log_sha256": sha256(log_path),
        "stages": str(stage_path),
        "stages_sha256": sha256(stage_path),
        "old_source": source_name,
        "runtime_manifest": str(manifest_path),
        "runtime_manifest_sha256": manifest_hash,
        "begin_line": begin_line,
        "warmup_id": 30054,
        "warmup_create_line": warmup_create_line,
        "warmup_free_line": warmup_free_line,
        "warmup_late_reap": 1 if warmup_free_line > warmup_create_line + 1 else 0,
        "cycle_first_id": 30055,
        "cycle_last_id": 31054,
        "last_worker_create_line": last_worker_create_line,
        "last_worker_free_line": last_worker_free_line,
        "cycles": 1000,
        "cleanup": 1000,
        "old_drift_abs": 16640,
        "old_drift_direction": "UNKNOWN",
        "faults": len(fault_markers),
        "boot_timestamp": boot_time.isoformat(),
        "modal_fail_timestamp": modal_time.isoformat(),
        "elapsed_seconds": elapsed_seconds,
        "checks": checks,
    }

    ARTIFACT_DIR.mkdir(parents=True, exist_ok=True)
    json_path = ARTIFACT_DIR / "cl13fix13-modal-old-run.json"
    log_out = ARTIFACT_DIR / "cl13fix13-modal-old-run.log"
    md_out = ARTIFACT_DIR / "cl13fix13-modal-old-run.md"
    json_path.write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    sentinel = (
        f"[CL13][MODAL_OLD_RUN_AUDIT] {'PASS' if passed else 'FAIL'}\n"
        f"warmup_late_reap={result['warmup_late_reap']} cycles=1000 "
        f"cleanup=1000 faults={result['faults']}\n"
    )
    log_out.write_text(sentinel + json.dumps(result, sort_keys=True) + "\n",
                       encoding="utf-8")
    md_out.write_text(
        "# CL-13 FIX13 old modal run audit\n\n"
        f"- Status: {'PASS' if passed else 'FAIL'}\n"
        "- Classification: `REJECTED_CL13_MODALTEST_WARMUP_BASELINE_CONTAMINATED`\n"
        f"- Warmup: ID 30054, create line {warmup_create_line}, "
        f"late free line {warmup_free_line}\n"
        f"- Cycle range: 30055..31054; last free line {last_worker_free_line}\n"
        "- Result: 1000 cycles, 1000 cleanups, absolute-only drift 16640\n"
        f"- Faults: {len(fault_markers)}\n"
        f"- Elapsed: {elapsed_seconds:.3f}s (~11m25s)\n",
        encoding="utf-8",
    )
    sys.stdout.write(sentinel)
    return 0 if passed else 1


def classify_heap(baseline: int, final: int, baseline_blocks: int,
                  final_blocks: int) -> str:
    delta = final - baseline
    if delta > 0:
        return "UP_RUNTIME_DEFECT"
    if delta < 0:
        return "DOWN_BASELINE_CONTAMINATION"
    if baseline_blocks != final_blocks:
        return "ZERO_BYTES_BLOCK_MISMATCH"
    return "ZERO_PASS"


def host_unit() -> int:
    down = classify_heap(1000, 900, 10, 9)
    up = classify_heap(900, 1000, 9, 10)
    zero = classify_heap(1000, 1000, 10, 10)
    reaped_return = 0
    busy_stats = {"current_zombies": 1, "free_inflight": 0}
    zero_implies_idle = (
        reaped_return == 0
        and busy_stats["current_zombies"] == 0
        and busy_stats["free_inflight"] == 0
    )
    passed = (
        down == "DOWN_BASELINE_CONTAMINATION"
        and up == "UP_RUNTIME_DEFECT"
        and zero == "ZERO_PASS"
        and not zero_implies_idle
    )
    ARTIFACT_DIR.mkdir(parents=True, exist_ok=True)
    output = ARTIFACT_DIR / "cl13fix13-modal-heap-host-unit.log"
    sentinel = f"[CL13][MODAL_HEAP_HOST_UNIT] {'PASS' if passed else 'FAIL'}"
    output.write_text(
        sentinel + "\n"
        f"down={down} up={up} zero={zero} "
        f"reaper_return_zero_implies_idle={int(zero_implies_idle)}\n",
        encoding="utf-8",
    )
    print(sentinel)
    return 0 if passed else 1


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser()
    subparsers = result.add_subparsers(dest="command", required=True)
    verify = subparsers.add_parser("verify-fix12-modal-failure")
    verify.add_argument(
        "--log", default="artifacts/build/cl13fix12-soak8-kvm-failed.log"
    )
    verify.add_argument(
        "--stages", default="artifacts/build/cl13fix12-soak8-stages.tsv"
    )
    verify.add_argument("--old-source")
    verify.add_argument(
        "--runtime-manifest",
        default="artifacts/build/cl13fix12-runtime-before.sha256",
    )
    subparsers.add_parser("host-unit")
    return result


def main() -> int:
    args = parser().parse_args()
    try:
        if args.command == "verify-fix12-modal-failure":
            return verify_old_run(args)
        if args.command == "host-unit":
            return host_unit()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"[CL13][MODAL_OLD_RUN_AUDIT] FAIL reason={error}", file=sys.stderr)
        return 1
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
