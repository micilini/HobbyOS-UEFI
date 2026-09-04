#!/usr/bin/env python3
"""Hash-anchored CL-13 evidence checkpoints for selective negative resumes."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import pathlib
import re
import sys
from typing import Any


ROOT = pathlib.Path(__file__).resolve().parent.parent
ARTIFACTS = ROOT / "artifacts" / "build"
CHECKPOINT_JSON = ARTIFACTS / "cl13fix11-checkpoint.json"
CHECKPOINT_LOG = ARTIFACTS / "cl13fix11-checkpoint.log"
CHECKPOINT_MD = ARTIFACTS / "cl13fix11-checkpoint.md"
NEGATIVE_CHECKPOINTS = ARTIFACTS / "cl13-negative-checkpoints"
RUNTIME_BEFORE = ARTIFACTS / "cl13fix11-runtime-before.sha256"
HPET_FAILED = ARTIFACTS / "cl13-negative-hpet-torn-read-failed.log"
HPET_BUILD = ARTIFACTS / "kernel-check-j2.log"
HPET_REVALIDATED = ARTIFACTS / "cl13fix11-hpet-boot-negative-revalidated.log"

FAULT_RE = re.compile(
    r"PANIC|#PF|#GP|FATAL|STRUCTURAL_FAULT|FINISH_FAULT|QEMU exited"
)
STATUS_RE = re.compile(r"\[HARNESS\]\[STATUS\] accepted=(\d+) completed=(\d+)")

MATRIX = {
    "smp4-tcg": (
        "f9d400cbb62143492a3a1792a68a623f073ab47692005c03c4ce479e32c3df7d",
        "cl13-matrix-smp4-tcg.log",
    ),
    "smp4-kvm": (
        "d8f5fb7914bfd7a046a9344014cbc3e4ee181af24c1b06ea5e149d8d1ce5d4f2",
        "cl13-matrix-smp4-kvm.log",
    ),
    "smp8-tcg": (
        "8fd82c146dd6cd47f13cfe77010b5f3b049e2c509badb79fef91615e36a3b53e",
        "cl13-matrix-smp8-tcg.log",
    ),
    "smp8-kvm": (
        "5dc9f18d9de9e7678660cbf791e18100f1cfd0a9c2da81158de6f25d65805af6",
        "cl13-matrix-smp8-kvm.log",
    ),
}

QUANTUM = {
    "q1": (
        "93834f5acf522a5d95a3060d5ed84106a49bca74eb17f6fdc4f538a8ade70759",
        "cl13-quantum-1-smp2-tcg.log",
    ),
    "q7": (
        "720e01afba92cf2173b1cd7c069ff9c5e5de50ddd2d63a2df60ca688974eabf3",
        "cl13-quantum-7-smp8-tcg.log",
    ),
}

PRE_HPET = {
    "scheduler-stale-slot": (
        "e532c1c733ff840aca31871669d690b0e90427d812b4ea3c07f2fa2e42cc41e7",
        "[SCHED][NEGATIVE] STALE_SLOT_DETECTED",
    ),
    "sync-old-sem-gap": (
        "94b0fdc6897dfe97373c6f0c383e2b7330abd2e045e90454ca52e3a20a309e2b",
        "[SYNC][NEGATIVE] OLD_SEM_GAP_DETECTED",
    ),
    "accounting-irq-ticks": (
        "0c5e0301ad794460e0a3bf418d6a0c44458eac608d281c281088d8bd8cd9669a",
        "[ACCOUNT][NEGATIVE] IRQ_TICK_MODEL_DETECTED",
    ),
    "accounting-unsafe-muldiv": (
        "a3a1939f2c5a15d988a8632adbd6ef7c7820379ea86e6ce5150a0e53d73f65b5",
        "[ACCOUNT][NEGATIVE] UNSAFE_MULDIV_DETECTED",
    ),
    "clock-global-only": (
        "2f816045e21961931c3555852a12cdc5aa5113eae6307ff89a4d671e85eda81c",
        "[CLOCK][NEGATIVE] GLOBAL_ONLY_FALSE_REGRESSION_DETECTED",
    ),
    "clock-local-backward": (
        "dc91537a20dfe237f8f26eb7f3e4f4c04333b3be03bfa91a2f2c13f567a00121",
        "[CLOCK][NEGATIVE] LOCAL_SOURCE_REGRESSION_DETECTED",
    ),
}

RESETS = {
    "scheduler-stale-slot": (
        "c6027642e74c35875bb804191360a5428f62becc6948955bcdd36f979c8dff65",
        "[SCHED][CPU_PIN] PASS",
    ),
    "sync-old-sem-gap": (
        "c0ccf8583797c1a5dbc58b793053e63c7e8b9cf3c5f7114ca6466d643a9374fa",
        "[SYNC][BOUNDARY] PASS",
    ),
    "accounting-irq-ticks": (
        "59dfc8883656a7bb1d0025c6bab21cebdaefdce6feedebd63386c322d1cb00f7",
        "[ACCOUNT][BUSY] PASS",
    ),
    "accounting-unsafe-muldiv": (
        "4d36abd3679ee5e1ca99c23e1d2880d001a60543ef57f1fadf0899189039f38e",
        "[ACCOUNT][LONG] PASS",
    ),
    "clock-global-only": (
        "c442131235787e76361a780ebedfb8c75e7ff92600b839b66490cde00c68ff48",
        "[ACCOUNT][CLOCK_CLASSIFIER] PASS",
    ),
    "clock-local-backward": (
        "093e880a6b279069cd851c970db94502e15a71c285c43484c121c1e96a33ae5a",
        "[ACCOUNT][CLOCK_CLASSIFIER] PASS",
    ),
}

REMAINING = (
    "hpet-torn-read",
    "kill-ignore-killable",
    "kill-accept-exiting",
    "reaper-ignore-timer-ref",
    "reaper-on-cpu",
    "reaper-duplicate-notification",
    "input-phantom-permit",
    "input-route-after-unlock",
    "modal-stale-token",
    "modal-owner-recovery",
    "wait-preblock",
    "taskman-render-all",
    "taskman-exit-any-key",
    "taskman-fixed-128",
    "taskman-no-page-clamp",
    "clock-sleep-late-diagnostic",
)


class EvidenceError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise EvidenceError(message)


def sha256(path: pathlib.Path) -> str:
    require(path.is_file(), f"missing artifact: {path.relative_to(ROOT)}")
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def text(path: pathlib.Path) -> str:
    require(path.is_file(), f"missing artifact: {path.relative_to(ROOT)}")
    return path.read_text(encoding="utf-8", errors="replace").replace("\r", "")


def clean_guest_log(data: str, label: str) -> None:
    require(not FAULT_RE.search(data), f"guest fault in {label}")


def balanced_transport(data: str, label: str) -> None:
    matches = STATUS_RE.findall(data)
    require(matches, f"missing HARNESS status in {label}")
    accepted, completed = map(int, matches[-1])
    require(accepted == completed, f"unbalanced HARNESS in {label}")


def artifact_record(path: pathlib.Path, expected: str) -> dict[str, Any]:
    actual = sha256(path)
    require(actual == expected, f"hash mismatch: {path.name}")
    return {"path": str(path.relative_to(ROOT)), "sha256": actual}


def verify_runtime_manifest(path: pathlib.Path = RUNTIME_BEFORE) -> dict[str, Any]:
    rows = []
    for raw in text(path).splitlines():
        if not raw.strip():
            continue
        parts = raw.split(maxsplit=1)
        require(len(parts) == 2, f"invalid runtime manifest row: {raw}")
        expected, name = parts
        target = ROOT / name.strip()
        actual = sha256(target)
        require(actual == expected, f"runtime changed: {name.strip()}")
        rows.append(name.strip())
    require(rows, "empty runtime manifest")
    return {
        "path": str(path.relative_to(ROOT)),
        "sha256": sha256(path),
        "files": len(rows),
        "unchanged": True,
    }


def find_line(lines: list[str], marker: str, start: int = 0) -> int:
    for index in range(start, len(lines)):
        if marker in lines[index]:
            return index + 1
    raise EvidenceError(f"missing marker: {marker}")


def verify_hpet_artifact() -> dict[str, Any]:
    data = text(HPET_FAILED)
    lines = data.splitlines()
    marker = "[CLOCK][NEGATIVE] TORN_64BIT_COUNTER_READ_DETECTED"
    require(data.count(marker) == 1, "HPET marker count is not one")
    require("TORN_64BIT_COUNTER_READ_MISSED" not in data, "HPET MISSED marker found")
    clean_guest_log(data, HPET_FAILED.name)
    build = text(HPET_BUILD)
    macro = "HOBBYOS_HPET_NEGATIVE_TORN_COUNTER_READ"
    require(macro in build, "HPET macro absent from build log")

    marker_line = find_line(lines, marker)
    runtime_line = find_line(lines, "[BOOT][RUNTIME_READY] PASS")
    test_line = find_line(lines, "[BOOT][TEST_READY] PASS autorun=0")
    begin_line = find_line(lines, "[HARNESS][BEGIN] seq=1")
    health_line = find_line(lines, "[ACCOUNT][HPET_STATS]", begin_line - 1)
    end_line = find_line(lines, "[HARNESS][END] seq=1 status=0", health_line - 1)
    require(
        marker_line < runtime_line < test_line < begin_line < health_line < end_line,
        "HPET BOOT_NEGATIVE ordering mismatch",
    )
    result = {
        "contract": "BOOT",
        "macro": macro,
        "detected": 1,
        "missed": 0,
        "faults": 0,
        "health": "PASS",
        "marker_line": marker_line,
        "runtime_ready_line": runtime_line,
        "test_ready_line": test_line,
        "frame_begin_line": begin_line,
        "hpet_stats_line": health_line,
        "end_line": end_line,
        "end_status": 0,
        "artifact": str(HPET_FAILED.relative_to(ROOT)),
        "artifact_sha256": sha256(HPET_FAILED),
    }
    HPET_REVALIDATED.write_text(
        "[CL13][HPET_BOOT_NEGATIVE_REVALIDATED] PASS\n"
        "macro=1 detected=1 health=PASS faults=0\n"
        f"marker_line={marker_line} runtime_ready_line={runtime_line} "
        f"test_ready_line={test_line} frame_begin_line={begin_line} "
        f"hpet_stats_line={health_line} end_line={end_line} end_status=0\n"
        f"source={result['artifact']} sha256={result['artifact_sha256']}\n",
        encoding="utf-8",
    )
    return result


def load_checkpoint() -> dict[str, Any]:
    if not CHECKPOINT_JSON.is_file():
        return {"version": 1, "negatives": {}}
    value = json.loads(CHECKPOINT_JSON.read_text(encoding="utf-8"))
    require(isinstance(value, dict), "checkpoint JSON is not an object")
    value.setdefault("negatives", {})
    return value


def write_outputs(checkpoint: dict[str, Any], message: str) -> None:
    ARTIFACTS.mkdir(parents=True, exist_ok=True)
    checkpoint["updated_at"] = dt.datetime.now(dt.timezone.utc).isoformat()
    CHECKPOINT_JSON.write_text(
        json.dumps(checkpoint, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    CHECKPOINT_LOG.write_text(message.rstrip() + "\n", encoding="utf-8")
    pre = checkpoint.get("pre_hpet", {})
    negatives = checkpoint.get("negatives", {})
    rows = [
        "# CL-13 FIX11 evidence checkpoint",
        "",
        f"- Status: `{checkpoint.get('status', 'INCOMPLETE')}`",
        f"- Matrix: `{len(pre.get('matrix', {}))}/4`",
        f"- Quantum: `{len(pre.get('quantum', {}))}/2`",
        f"- Pre-HPET negatives: `{len(pre.get('negatives', {}))}/6`",
        f"- Reset logs: `{len(pre.get('resets', {}))}/6`",
        f"- Runtime unchanged: `{int(bool(pre.get('runtime', {}).get('unchanged')))}`",
        f"- Negative checkpoints: `{len(negatives)}`",
        "",
        "```text",
        message.rstrip(),
        "```",
        "",
    ]
    CHECKPOINT_MD.write_text("\n".join(rows), encoding="utf-8")


def verify_pre_hpet(_: argparse.Namespace) -> None:
    checkpoint = load_checkpoint()
    matrix: dict[str, Any] = {}
    for name, (expected, filename) in MATRIX.items():
        path = ARTIFACTS / filename
        data = text(path)
        record = artifact_record(path, expected)
        require("[TASKMANTEST][CHECK] PASS" in data, f"TASKMANTEST missing: {name}")
        require("[TASKDIAG][CHECK] PASS" in data, f"TASKDIAG missing: {name}")
        balanced_transport(data, name)
        clean_guest_log(data, name)
        matrix[name] = record

    quantum: dict[str, Any] = {}
    for name, (expected, filename) in QUANTUM.items():
        path = ARTIFACTS / filename
        data = text(path)
        record = artifact_record(path, expected)
        require("[SCHED][STRESS] YIELD_COMPLETE" in data, f"yield missing: {name}")
        require("[SELFTEST][AUTORUN] PASS" in data, f"autorun missing: {name}")
        clean_guest_log(data, name)
        quantum[name] = record

    negatives: dict[str, Any] = {}
    resets: dict[str, Any] = {}
    for name, (expected, marker) in PRE_HPET.items():
        path = ARTIFACTS / f"cl13-negative-{name}.log"
        data = text(path)
        record = artifact_record(path, expected)
        require(marker in data, f"negative marker missing: {name}")
        require("MISSED" not in data, f"MISSED in negative: {name}")
        clean_guest_log(data, name)
        negatives[name] = record

        reset_expected, reset_marker = RESETS[name]
        reset_path = ARTIFACTS / f"cl13-negative-{name}-reset.log"
        reset_data = text(reset_path)
        reset_record = artifact_record(reset_path, reset_expected)
        require(reset_marker in reset_data, f"reset marker missing: {name}")
        require("[SELFTEST][AUTORUN] PASS" in reset_data, f"reset autorun missing: {name}")
        balanced_transport(reset_data, f"{name}-reset")
        clean_guest_log(reset_data, f"{name}-reset")
        resets[name] = reset_record

    runtime = verify_runtime_manifest()
    hpet = verify_hpet_artifact()
    checkpoint["pre_hpet"] = {
        "status": "PASS",
        "matrix": matrix,
        "quantum": quantum,
        "negatives": negatives,
        "resets": resets,
        "runtime": runtime,
    }
    checkpoint["hpet_offline_revalidation"] = hpet
    checkpoint["status"] = "PRE_HPET_PASS"
    message = (
        "[CL13][CHECKPOINT] PASS\n"
        "matrix=4 quantum=2 negatives_pre_hpet=6 runtime_unchanged=1\n"
        "[CL13][HPET_BOOT_NEGATIVE_REVALIDATED] PASS\n"
        "macro=1 detected=1 health=PASS faults=0"
    )
    write_outputs(checkpoint, message)
    print(message)


def record_negative(args: argparse.Namespace) -> None:
    checkpoint = load_checkpoint()
    negative_path = (ROOT / args.negative_log).resolve()
    reset_path = (ROOT / args.reset_log).resolve()
    runtime_path = (ROOT / args.runtime_manifest).resolve()
    negative_data = text(negative_path)
    reset_data = text(reset_path)
    require(args.marker in negative_data, f"marker missing for {args.id}")
    require("MISSED" not in negative_data, f"MISSED in {args.id}")
    clean_guest_log(negative_data, args.id)
    clean_guest_log(reset_data, f"{args.id}-reset")
    runtime = verify_runtime_manifest(runtime_path)
    record = {
        "id": args.id,
        "macro": args.macro,
        "contract": args.contract,
        "marker": args.marker,
        "negative_log": str(negative_path.relative_to(ROOT)),
        "negative_sha256": sha256(negative_path),
        "reset_log": str(reset_path.relative_to(ROOT)),
        "reset_sha256": sha256(reset_path),
        "detected": True,
        "missed": False,
        "faults": 0,
        "runtime_manifest": str(runtime_path.relative_to(ROOT)),
        "runtime_manifest_sha256": runtime["sha256"],
    }
    NEGATIVE_CHECKPOINTS.mkdir(parents=True, exist_ok=True)
    target = NEGATIVE_CHECKPOINTS / f"{args.id}.json"
    target.write_text(json.dumps(record, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    checkpoint.setdefault("negatives", {})[args.id] = record
    checkpoint["status"] = "IN_PROGRESS"
    message = f"[CL13][NEGATIVE_CHECKPOINT] PASS id={args.id} contract={args.contract}"
    write_outputs(checkpoint, message)
    print(message)


def verify_negative(args: argparse.Namespace) -> None:
    path = NEGATIVE_CHECKPOINTS / f"{args.id}.json"
    record = json.loads(text(path))
    require(record.get("id") == args.id, "negative checkpoint ID mismatch")
    negative_path = ROOT / record["negative_log"]
    reset_path = ROOT / record["reset_log"]
    require(sha256(negative_path) == record["negative_sha256"], "negative hash mismatch")
    require(sha256(reset_path) == record["reset_sha256"], "reset hash mismatch")
    require(record["marker"] in text(negative_path), "negative marker missing")
    verify_runtime_manifest(ROOT / record["runtime_manifest"])
    print(f"[CL13][NEGATIVE_CHECKPOINT] VERIFIED id={args.id}")


def summary(_: argparse.Namespace) -> None:
    checkpoint = load_checkpoint()
    require(checkpoint.get("pre_hpet", {}).get("status") == "PASS", "pre-HPET checkpoint absent")
    for identifier in REMAINING:
        require(identifier in checkpoint.get("negatives", {}), f"missing negative checkpoint: {identifier}")
        verify_negative(argparse.Namespace(id=identifier))
    checkpoint["status"] = "PASS"
    message = (
        "[CL13][CHECKPOINT] PASS\n"
        "matrix=4 quantum=2 negatives_pre_hpet=6 negatives_remaining=16 "
        "runtime_unchanged=1"
    )
    write_outputs(checkpoint, message)
    print(message)


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser()
    sub = result.add_subparsers(dest="command", required=True)
    pre = sub.add_parser("verify-pre-hpet")
    pre.set_defaults(func=verify_pre_hpet)
    record = sub.add_parser("record-negative")
    record.add_argument("--id", required=True)
    record.add_argument("--macro", required=True)
    record.add_argument("--contract", required=True, choices=("BOOT", "COMMAND", "ASYNC"))
    record.add_argument("--marker", required=True)
    record.add_argument("--negative-log", required=True)
    record.add_argument("--reset-log", required=True)
    record.add_argument("--runtime-manifest", required=True)
    record.set_defaults(func=record_negative)
    verify = sub.add_parser("verify-negative")
    verify.add_argument("--id", required=True)
    verify.set_defaults(func=verify_negative)
    report = sub.add_parser("summary")
    report.set_defaults(func=summary)
    return result


def main() -> int:
    args = parser().parse_args()
    try:
        args.func(args)
        return 0
    except (EvidenceError, OSError, ValueError, KeyError) as error:
        message = f"REJECTED_CL13_FIX11_EVIDENCE: {error}"
        checkpoint = load_checkpoint()
        checkpoint["status"] = "FAIL"
        checkpoint["error"] = str(error)
        write_outputs(checkpoint, message)
        print(message, file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
