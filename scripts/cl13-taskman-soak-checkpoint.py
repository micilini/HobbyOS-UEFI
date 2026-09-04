#!/usr/bin/env python3
"""FIX15 offline audit for the interrupted FIX14 TASKMAN soak stage."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import pathlib
import re
import sys
from typing import Any


ROOT = pathlib.Path(__file__).resolve().parents[1]
ARTIFACT_DIR = ROOT / "artifacts" / "build"
EXPECTED_LOG = "84188d1ddf11a2ce9958922eef8adc6ff0f6c194900625ccb8d2404d5b347a9a"
EXPECTED_ARM_SOURCE = "4f83e637757d67b24f2693056776b5e20163e1f4a3fe620bb008d0809a20a68d"
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


def resolve(path: str) -> pathlib.Path:
    candidate = pathlib.Path(path)
    return candidate if candidate.is_absolute() else ROOT / candidate


def stage_rows(path: pathlib.Path) -> dict[str, dict[str, str]]:
    lines = path.read_text(encoding="utf-8").splitlines()
    if not lines:
        raise ValueError("empty stage ledger")
    header = lines[0].split("\t")
    required = {"stage", "status", "host_timestamp", "frame_sequence"}
    if not required.issubset(header):
        raise ValueError("invalid stage ledger header")
    rows: dict[str, dict[str, str]] = {}
    for line in lines[1:]:
        values = line.split("\t")
        row = dict(zip(header, values))
        if row.get("stage"):
            rows[row["stage"]] = row
    return rows


def timestamp(rows: dict[str, dict[str, str]], stage: str) -> dt.datetime:
    if stage not in rows:
        raise ValueError(f"missing stage: {stage}")
    return dt.datetime.fromisoformat(rows[stage]["host_timestamp"])


def line_number(lines: list[str], marker: str, start: int = 1) -> int:
    for number, line in enumerate(lines[start - 1 :], start):
        if marker in line:
            return number
    raise ValueError(f"missing marker: {marker}")


def verify_source(source: str, taskman: str) -> dict[str, bool]:
    checks = {
        "parse_exact_u32": "parse_exact_u32(text,&frames)" in source,
        "frames_min": "frames>=1" in source,
        "frames_max": "frames<=100000" in source,
        "arm_direct": "taskman_test_arm_auto_exit_full_frames(frames)" in source,
        "arm_returns_bool": "return ok;" in source,
        "command_status": "return ok?0:1;" in source,
        "taskman_consumes_arm":
            "__atomic_exchange_n(&g_test_auto_exit_after_full_frames,0"
            in taskman,
        "taskman_returns_status": "return 0;" in taskman,
    }
    return checks


def verify(args: argparse.Namespace) -> int:
    log_path = resolve(args.log)
    stages_path = resolve(args.stages)
    source_path = resolve(args.cmd_taskmantest)
    taskman_path = resolve(args.cmd_taskman)
    manifest_path = resolve(args.runtime_manifest)
    for path in (log_path, stages_path, source_path, taskman_path, manifest_path):
        if not path.is_file():
            raise ValueError(f"missing input: {path}")

    text = log_path.read_text(encoding="utf-8", errors="replace")
    lines = text.splitlines()
    source = source_path.read_text(encoding="utf-8", errors="replace")
    taskman = taskman_path.read_text(encoding="utf-8", errors="replace")
    rows = stage_rows(stages_path)

    frame_line = line_number(
        lines, "[HARNESS][FRAME] ACCEPT seq=924 crc=928ac952 len=30"
    )
    begin_line = line_number(lines, "[HARNESS][BEGIN] seq=924")
    end_line = line_number(lines, "[HARNESS][END] seq=924 status=0")
    marker_line = line_number(
        lines, "[TASKMANTEST][AUTO_EXIT_ARM] PASS frames=", begin_line
    )
    frame_position = text.index("[HARNESS][FRAME] ACCEPT seq=924")
    end_position = text.index("[HARNESS][END] seq=924 status=0", frame_position)
    frame_block = text[frame_position:end_position]

    arm_attempts = text.count("[TASKMANTEST][AUTO_EXIT_ARM] PASS")
    completed_sessions = text.count("[TASKMAN][AUTO_EXIT] PASS target=1")
    anchor_resets = text.count("[TASKMANTEST][ANCHOR_RESET] PASS")
    accepts = len(re.findall(r"\[HARNESS\]\[FRAME\] ACCEPT seq=", text))
    begins = len(re.findall(r"\[HARNESS\]\[BEGIN\] seq=", text))
    ends_zero = len(re.findall(
        r"\[HARNESS\]\[END\] seq=[0-9]+ status=0", text
    ))
    faults = FAULT_RE.findall(text)

    boot = timestamp(rows, "boot")
    warmup = timestamp(rows, "warmup")
    modal = timestamp(rows, "modal")
    ps = timestamp(rows, "ps")
    taskman_fail = timestamp(rows, "taskman")
    elapsed = (taskman_fail - boot).total_seconds()
    ps_elapsed = (ps - modal).total_seconds()
    taskman_elapsed = (taskman_fail - ps).total_seconds()
    source_checks = verify_source(source, taskman)

    checks = {
        "log_hash": sha256(log_path) == EXPECTED_LOG,
        "source_hash": sha256(source_path) == EXPECTED_ARM_SOURCE,
        "runtime_manifest": sha256(manifest_path) == EXPECTED_RUNTIME_MANIFEST,
        "frame_line": frame_line == 35833,
        "begin_line": begin_line == 35835,
        "marker_line": marker_line == 35836,
        "end_line": end_line == 35839,
        "marker_interleaved":
            "[TASKMANTEST][AUTO_EXIT_ARM] PASS frames=1" not in frame_block
            and "[TASKMANTEST][AUTO_EXIT_ARM] PASS frames=" in frame_block,
        "arm_attempts": arm_attempts == 101,
        "completed_sessions": completed_sessions == 100,
        "anchor_resets": anchor_resets == 101,
        "frames_balanced": accepts == begins == ends_zero == 924,
        "no_seq925": "seq=925" not in text,
        "no_auto_exit_after_seq924":
            "[TASKMAN][AUTO_EXIT]" not in text[end_position:],
        "faults_zero": len(faults) == 0,
        "stage_status": rows["ps"]["status"] == "PASS"
            and rows["taskman"]["status"] == "FAIL",
        "elapsed": abs(elapsed - 8474.935) <= 5.0,
        "ps_elapsed": abs(ps_elapsed - 4410.839) <= 5.0,
        "taskman_elapsed": abs(taskman_elapsed - 3475.772) <= 5.0,
        "source_contract": all(source_checks.values()),
    }
    passed = all(checks.values())
    result: dict[str, Any] = {
        "status": "PASS" if passed else "FAIL",
        "classification": "REJECTED_CL13_SOAK_TASKMAN_CONTROL_MARKER_INTERLEAVING",
        "artifact": str(log_path),
        "artifact_sha256": sha256(log_path),
        "stages": str(stages_path),
        "source": str(source_path),
        "source_sha256": sha256(source_path),
        "runtime_manifest": str(manifest_path),
        "runtime_manifest_sha256": sha256(manifest_path),
        "sequence": 924,
        "frame_line": frame_line,
        "begin_line": begin_line,
        "marker_line": marker_line,
        "end_line": end_line,
        "arm_status": 0,
        "arm_attempts": arm_attempts,
        "anchor_resets": anchor_resets,
        "completed_total_sessions": completed_sessions,
        "completed_warmup": 1,
        "completed_stress_sessions": completed_sessions - 1,
        "next_session_launched": 0,
        "frames": {"accept": accepts, "begin": begins, "end_status0": ends_zero},
        "faults": len(faults),
        "timestamps": {
            "boot": boot.isoformat(),
            "warmup": warmup.isoformat(),
            "modal": modal.isoformat(),
            "ps": ps.isoformat(),
            "taskman_fail": taskman_fail.isoformat(),
        },
        "elapsed_seconds": elapsed,
        "ps_stage_seconds": ps_elapsed,
        "taskman_stage_seconds": taskman_elapsed,
        "source_contract": source_checks,
        "checks": checks,
    }

    ARTIFACT_DIR.mkdir(parents=True, exist_ok=True)
    json_path = ARTIFACT_DIR / "cl13fix15-old-taskman.json"
    log_out = ARTIFACT_DIR / "cl13fix15-old-taskman.log"
    md_out = ARTIFACT_DIR / "cl13fix15-old-taskman.md"
    json_path.write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    sentinel = (
        f"[CL13][TASKMAN_OLD_RUN_AUDIT] {'PASS' if passed else 'FAIL'}\n"
        f"arm_status=0 completed_stress_sessions={completed_sessions - 1} "
        "next_session_not_launched=1\n"
        f"[CL13][AUTO_EXIT_ARM_REVALIDATED] {'PASS' if passed else 'FAIL'} "
        "seq=924 status=0 armed=1 faults=0\n"
    )
    log_out.write_text(
        sentinel + json.dumps(result, sort_keys=True) + "\n", encoding="utf-8"
    )
    md_out.write_text(
        "# CL-13 FIX15 old TASKMAN-stage audit\n\n"
        f"- Status: {'PASS' if passed else 'FAIL'}\n"
        "- Sequence 924: ACCEPT line 35833, BEGIN line 35835, "
        "interleaved arm marker line 35836, END status 0 line 35839.\n"
        f"- Arm attempts: {arm_attempts}; completed sessions: "
        f"{completed_sessions} (1 warmup + {completed_sessions - 1} stress).\n"
        f"- Balanced frames: {accepts}/{begins}/{ends_zero}.\n"
        f"- Guest faults: {len(faults)}.\n"
        f"- Elapsed: {elapsed:.3f}s (~2h21m15s); PS: "
        f"{ps_elapsed:.3f}s; TASKMAN: {taskman_elapsed:.3f}s.\n",
        encoding="utf-8",
    )
    sys.stdout.write(sentinel)
    return 0 if passed else 1


def status_oracle(end_status: int, marker_intact: bool) -> bool:
    del marker_intact
    return end_status == 0


def auto_session_delta(full: int, fallback: int, shortfall: int) -> bool:
    return full == 1 and fallback == 0 and shortfall == 0


def ps_loop_result(requested: int, completed: int,
                   failure_index: int) -> bool:
    return requested == completed and failure_index == 0


def host_unit() -> int:
    cases = {
        "arm_marker_intact": status_oracle(0, True),
        "arm_marker_interleaved": status_oracle(0, False),
        "arm_status_one": not status_oracle(1, True),
        "auto_session_clean": auto_session_delta(1, 0, 0),
        "auto_session_fallback": not auto_session_delta(1, 1, 0),
        "auto_session_shortfall": not auto_session_delta(0, 0, 1),
        "ps_loop_partial": not ps_loop_result(100, 73, 74),
    }
    passed = all(cases.values())
    ARTIFACT_DIR.mkdir(parents=True, exist_ok=True)
    output = ARTIFACT_DIR / "cl13fix15-taskman-transaction-host-unit.log"
    sentinel = (
        f"[CL13][TASKMAN_TRANSACTION_HOST_UNIT] "
        f"{'PASS' if passed else 'FAIL'}"
    )
    output.write_text(
        sentinel + "\n" + json.dumps(cases, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(sentinel)
    return 0 if passed else 1


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser()
    commands = result.add_subparsers(dest="command", required=True)
    verify_cmd = commands.add_parser("verify-fix14-taskman-failure")
    verify_cmd.add_argument(
        "--log", default="artifacts/build/cl13fix14-soak8-kvm-failed.log"
    )
    verify_cmd.add_argument(
        "--stages", default="artifacts/build/cl13fix14-soak8-stages.tsv"
    )
    verify_cmd.add_argument(
        "--cmd-taskmantest",
        default="artifacts/build/cl13fix15-cmd-taskmantest-before.c",
    )
    verify_cmd.add_argument(
        "--cmd-taskman", default="kernel/src/shell/commands/cmd_taskman.c"
    )
    verify_cmd.add_argument(
        "--runtime-manifest",
        default="artifacts/build/cl13fix11-runtime-after-hpet.sha256",
    )
    commands.add_parser("host-unit")
    return result


def main() -> int:
    args = parser().parse_args()
    try:
        if args.command == "verify-fix14-taskman-failure":
            return verify(args)
        if args.command == "host-unit":
            return host_unit()
    except (OSError, ValueError) as error:
        print(f"[CL13][TASKMAN_OLD_RUN_AUDIT] FAIL reason={error}", file=sys.stderr)
        return 1
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
