#!/usr/bin/env python3
"""Offline FIX14 audit for modal ownership versus global heap noise."""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import pathlib
import re
import sys
from typing import Any


ROOT = pathlib.Path(__file__).resolve().parents[1]
ARTIFACT_DIR = ROOT / "artifacts" / "build"
EXPECTED_RUNTIME_MANIFEST = (
    "d8dd183b4b2ef247bd766464e2e09dc88addbeb08c66b373dd6e9fb199b01d09"
)
EXPECTED_RUN1 = "aaaa9cd4ae9fab81a7a32ad583497f117ef31fb4bfc5bbee8038810867c04c21"
EXPECTED_RUN2 = "ff08dc05d038156d547a7a1d99ad1fd4115aa7ee80db110fd99407f90a78496b"
FAULT_RE = re.compile(
    r"(?:\bpanic\b|#PF|#GP|\bFATAL\b|"
    r"\[REAPER\]\[STRUCTURAL_FAULT\]|\[SCHED\]\[FINISH_FAULT\])",
    re.IGNORECASE,
)


class ListHead(ctypes.Structure):
    _fields_ = [("next", ctypes.c_void_p), ("prev", ctypes.c_void_p)]


class TimerHandle(ctypes.Structure):
    _fields_ = [("id", ctypes.c_uint64), ("generation", ctypes.c_uint64)]


class TimerNodeAbi(ctypes.Structure):
    _fields_ = [
        ("handle", TimerHandle),
        ("deadline_ms", ctypes.c_uint64),
        ("state", ctypes.c_int),
        ("kind", ctypes.c_int),
        ("callback", ctypes.c_void_p),
        ("ctx", ctypes.c_void_p),
        ("task_id", ctypes.c_uint64),
        ("lifecycle_generation", ctypes.c_uint64),
        ("wait_generation", ctypes.c_uint64),
        ("wake_reason", ctypes.c_int),
        ("task_ref_held", ctypes.c_uint8),
        ("node", ListHead),
    ]


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read(path: str) -> tuple[pathlib.Path, str]:
    resolved = ROOT / path
    return resolved, resolved.read_text(encoding="utf-8", errors="replace")


def modal_record(text: str) -> tuple[str, dict[str, str]]:
    lines = [
        line for line in text.splitlines()
        if line.startswith("[MODALTEST][OPEN_CLOSE] ")
    ]
    if not lines:
        raise ValueError("missing modal open-close record")
    line = lines[-1]
    status = line.split()[1]
    fields = dict(re.findall(r"([a-z_]+)=([^ ]+)", line))
    return status, fields


def as_int(fields: dict[str, str], name: str) -> int:
    if name not in fields:
        raise ValueError(f"missing modal field: {name}")
    return int(fields[name])


def clean_fix13_record(fields: dict[str, str]) -> bool:
    return (
        as_int(fields, "cycles") == 1000
        and as_int(fields, "cleanup") == 1000
        and as_int(fields, "warmup_gone") == 1
        and as_int(fields, "handles") == 1000
        and as_int(fields, "handles_gone") == 1000
        and as_int(fields, "first_live_id") == 0
        and as_int(fields, "reaper_idle") == 1
        and as_int(fields, "zombies") == 0
        and as_int(fields, "free_inflight") == 0
        and as_int(fields, "modal_violations") == 0
        and as_int(fields, "session_violations") == 0
    )


def verify(args: argparse.Namespace) -> int:
    run1_path, run1_text = read(args.run1)
    run2_path, run2_text = read(args.run2)
    old_path, old_source = read(args.cmd_modaltest)
    smpstress_path, smpstress = read(args.cmd_smpstress)
    timers_path, timers = read(args.timers)
    heap_path, heap = read(args.heap)
    manifest_path, _ = read(args.runtime_manifest)
    run1_status, run1 = modal_record(run1_text)
    run2_status, run2 = modal_record(run2_text)

    timer_node_size = ctypes.sizeof(TimerNodeAbi)
    run1_block_delta = as_int(run1, "final_blocks") - as_int(
        run1, "baseline_blocks"
    )
    run2_block_delta = as_int(run2, "final_blocks") - as_int(
        run2, "baseline_blocks"
    )
    checks = {
        "run1_hash": sha256(run1_path) == EXPECTED_RUN1,
        "run2_hash": sha256(run2_path) == EXPECTED_RUN2,
        "runtime_manifest": sha256(manifest_path) == EXPECTED_RUNTIME_MANIFEST,
        "run1_status": run1_status == "PASS",
        "run1_zero": run1.get("direction") == "ZERO"
        and as_int(run1, "signed_delta") == 0
        and run1_block_delta == 0,
        "run2_status_is_old_false_rejection": run2_status == "FAIL",
        "run2_down_96": run2.get("direction") == "DOWN"
        and as_int(run2, "signed_delta") == -96
        and run2_block_delta == -1,
        "run1_ownership_clean": clean_fix13_record(run1),
        "run2_ownership_clean": clean_fix13_record(run2),
        "faults_zero": not FAULT_RE.search(run1_text)
        and not FAULT_RE.search(run2_text),
        "old_oracle_hard_gated_global_heap": "result.signed_drift == 0" in old_source
        and "result.baseline_used_blocks == result.final_used_blocks" in old_source,
        "smpstress_yield_one": re.search(r"uint32_t\s+yield_ms\s*=\s*1\s*;", smpstress)
        is not None,
        "smpstress_sleeps": "timer_sleep(c->yield_ms);" in smpstress,
        "timer_node_allocated": "kmalloc(sizeof(*node))" in timers,
        "timer_node_fields": all(
            marker in timers
            for marker in (
                "typedef struct timer_node",
                "timer_handle_t handle;",
                "task_wake_reason_t wake_reason;",
                "struct list_head node;",
            )
        ),
        "timer_node_size_96": timer_node_size == 96,
        "heap_align_16": re.search(r"#define\s+HEAP_ALIGN\s+16ull", heap)
        is not None,
    }
    passed = all(checks.values())
    result: dict[str, Any] = {
        "status": "PASS" if passed else "FAIL",
        "classification": "REJECTED_CL13_MODAL_HEAP_ORACLE_CONFOUNDS_TRANSIENT_TIMERS",
        "run1": {
            "path": str(run1_path),
            "sha256": sha256(run1_path),
            "status": run1_status,
            "direction": run1.get("direction"),
            "signed_delta": as_int(run1, "signed_delta"),
            "block_delta": run1_block_delta,
        },
        "run2": {
            "path": str(run2_path),
            "sha256": sha256(run2_path),
            "status": run2_status,
            "direction": run2.get("direction"),
            "signed_delta": as_int(run2, "signed_delta"),
            "block_delta": run2_block_delta,
        },
        "ownership_clean": 1
        if clean_fix13_record(run1) and clean_fix13_record(run2)
        else 0,
        "faults": len(FAULT_RE.findall(run1_text))
        + len(FAULT_RE.findall(run2_text)),
        "timer_node_size": timer_node_size,
        "heap_align": 16,
        "smpstress_yield_ms": 1,
        "sources": [
            str(old_path), str(smpstress_path), str(timers_path), str(heap_path)
        ],
        "runtime_manifest": str(manifest_path),
        "runtime_manifest_sha256": sha256(manifest_path),
        "checks": checks,
    }

    ARTIFACT_DIR.mkdir(parents=True, exist_ok=True)
    json_path = ARTIFACT_DIR / "cl13fix14-old-focused.json"
    log_path = ARTIFACT_DIR / "cl13fix14-old-focused.log"
    md_path = ARTIFACT_DIR / "cl13fix14-old-focused.md"
    json_path.write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    sentinel = (
        f"[CL13][MODAL_OWNERSHIP_OLD_RUN] {'PASS' if passed else 'FAIL'}\n"
        "run1=ZERO run2=DOWN delta=-96 "
        f"ownership_clean={result['ownership_clean']}\n"
    )
    log_path.write_text(
        sentinel + json.dumps(result, sort_keys=True) + "\n", encoding="utf-8"
    )
    md_path.write_text(
        "# CL-13 FIX14 old focused audit\n\n"
        f"- Status: {'PASS' if passed else 'FAIL'}\n"
        "- Run 1: ZERO, 0 bytes, 0 blocks\n"
        "- Run 2: DOWN, -96 bytes, -1 block\n"
        f"- `sizeof(timer_node_t)`: {timer_node_size}\n"
        "- `HEAP_ALIGN`: 16\n"
        "- `smpstress` default yield: 1 ms through `timer_sleep`\n"
        f"- Modal ownership clean: {result['ownership_clean']}\n"
        f"- Guest faults: {result['faults']}\n",
        encoding="utf-8",
    )
    sys.stdout.write(sentinel)
    return 0 if passed else 1


def internal_modal_pass(ownership_clean: bool, heap_delta: int) -> bool:
    del heap_delta
    return ownership_clean


def scenario_pass(ownership_clean: bool, outer_measured: bool,
                  outer_delta: int) -> bool:
    return ownership_clean and outer_measured and outer_delta == 0


def host_unit() -> int:
    cases = {
        "clean_zero": internal_modal_pass(True, 0),
        "clean_down": internal_modal_pass(True, -96),
        "clean_up": internal_modal_pass(True, 96),
        "dirty_zero": not internal_modal_pass(False, 0),
        "outer_required": not scenario_pass(True, False, 0),
        "outer_zero": scenario_pass(True, True, 0),
        "outer_nonzero": not scenario_pass(True, True, -96),
        "timer_minus_one_diagnostic": internal_modal_pass(True, -96),
    }
    passed = all(cases.values())
    ARTIFACT_DIR.mkdir(parents=True, exist_ok=True)
    output = ARTIFACT_DIR / "cl13fix14-modal-ownership-host-unit.log"
    sentinel = f"[CL13][MODAL_OWNERSHIP_HOST_UNIT] {'PASS' if passed else 'FAIL'}"
    output.write_text(
        sentinel + "\n" + json.dumps(cases, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(sentinel)
    return 0 if passed else 1


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser()
    commands = result.add_subparsers(dest="command", required=True)
    verify_cmd = commands.add_parser("verify-fix13-focused")
    verify_cmd.add_argument(
        "--run1", default="artifacts/build/cl13fix13-modal-smp4-kvm-run1.log"
    )
    verify_cmd.add_argument(
        "--run2", default="artifacts/build/cl13fix13-modal-smp4-kvm-run2-failed.log"
    )
    verify_cmd.add_argument(
        "--cmd-modaltest", default="artifacts/build/cl13fix14-cmd-modaltest-fix13.c"
    )
    verify_cmd.add_argument(
        "--cmd-smpstress", default="kernel/src/shell/commands/cmd_smpstress.c"
    )
    verify_cmd.add_argument("--timers", default="kernel/src/core/timers.c")
    verify_cmd.add_argument("--heap", default="kernel/src/memory/heap.c")
    verify_cmd.add_argument(
        "--runtime-manifest",
        default="artifacts/build/cl13fix11-runtime-after-hpet.sha256",
    )
    commands.add_parser("host-unit")
    return result


def main() -> int:
    args = parser().parse_args()
    try:
        if args.command == "verify-fix13-focused":
            return verify(args)
        if args.command == "host-unit":
            return host_unit()
    except (OSError, ValueError) as error:
        print(
            f"[CL13][MODAL_OWNERSHIP_OLD_RUN] FAIL reason={error}",
            file=sys.stderr,
        )
        return 1
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
