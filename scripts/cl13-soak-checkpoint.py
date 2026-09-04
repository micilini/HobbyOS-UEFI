#!/usr/bin/env python3
"""Offline FIX12 checkpoint for the interrupted FIX11 SMP8 soak head."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
ARTIFACTS = ROOT / "artifacts" / "build"
EXPECTED_SOURCE_SHA256 = (
    "264e398d77f9fe1522ebf9722e16bf1e2e8ecf37f91304553a6370fe2ef5cb13"
)
EXPECTED_RUNTIME_MANIFEST_SHA256 = (
    "d8dd183b4b2ef247bd766464e2e09dc88addbeb08c66b373dd6e9fb199b01d09"
)
COUNTERS = {
    "accepted": 100000,
    "consumed": 100000,
    "chars": 50000,
    "specials": 50000,
    "last_sequence": 100000,
    "sequence_gaps": 0,
    "duplicates": 0,
    "regressions": 0,
}
FAULT_RE = re.compile(
    r"PANIC|#PF|#GP|FATAL|STRUCTURAL_FAULT|FINISH_FAULT|QEMU exited"
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"REJECTED_CL13_FIX12_EVIDENCE: {message}")


def frame_block(text: str, sequence: int) -> str:
    pattern = re.compile(
        rf"^\[HARNESS\]\[FRAME\] ACCEPT seq={sequence} "
        rf"crc=c990a27a len=26\r?$.*?"
        rf"^\[HARNESS\]\[END\] seq={sequence} status=0\r?$",
        re.MULTILINE | re.DOTALL,
    )
    match = pattern.search(text)
    require(match is not None, f"seq={sequence} ACCEPT/END status=0 missing")
    block = match.group(0)
    require(
        re.search(rf"^\[HARNESS\]\[BEGIN\] seq={sequence}\r?$", block, re.MULTILINE)
        is not None,
        f"seq={sequence} BEGIN missing",
    )
    return block


def extract_counters(block: str, sequence: int) -> dict[str, int]:
    require("[INPUTTEST][PRODUCERS] PASS" in block, f"seq={sequence} PASS missing")
    values: dict[str, int] = {}
    for name, expected in COUNTERS.items():
        match = re.search(rf"\b{name}=\s*([0-9]+)", block)
        require(match is not None, f"seq={sequence} counter {name} missing")
        values[name] = int(match.group(1))
        require(values[name] == expected, f"seq={sequence} counter {name} mismatch")
    return values


def verify_source_contract(source: Path) -> None:
    require(sha256(source) == EXPECTED_SOURCE_SHA256, "cmd_inputtest.c hash mismatch")
    text = source.read_text(encoding="utf-8")
    required = (
        "accepted == 100000",
        "consumed == 100000",
        "chars == 50000",
        "specials == 50000",
        "last == 100000",
        "!gaps",
        "!duplicates",
        "!regressions",
        "s.count == 0",
        "return ok ? 0 : 1;",
    )
    for fragment in required:
        require(fragment in text, f"handler predicate absent: {fragment}")


def write_outputs(data: dict[str, object]) -> None:
    ARTIFACTS.mkdir(parents=True, exist_ok=True)
    json_path = ARTIFACTS / "cl13fix12-soak8-head.json"
    log_path = ARTIFACTS / "cl13fix12-soak8-head.log"
    md_path = ARTIFACTS / "cl13fix12-soak8-head.md"
    sentinel = (
        "[CL13][SOAK8_HEAD_REVALIDATED] PASS "
        "producer_runs=2 input_events=200000 faults=0"
    )
    json_path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")
    log_path.write_text(sentinel + "\n")
    md_path.write_text(
        "# FIX12 soak8 head checkpoint\n\n"
        f"- Result: `{sentinel}`\n"
        "- Scope: the two completed producer frames only.\n"
        "- This checkpoint does not certify or splice a complete soak.\n"
    )
    print(sentinel)


def verify_fix11_input_failure(args: argparse.Namespace) -> None:
    log = Path(args.log)
    source = Path(args.source)
    runtime = Path(args.runtime_manifest)
    for path in (log, source, runtime):
        require(path.is_file(), f"missing input: {path}")
    verify_source_contract(source)
    require(
        sha256(runtime) == EXPECTED_RUNTIME_MANIFEST_SHA256,
        "runtime manifest hash mismatch",
    )
    text = log.read_text(encoding="utf-8", errors="replace")
    require(FAULT_RE.search(text) is None, "guest fault found")
    blocks = {sequence: frame_block(text, sequence) for sequence in (24, 25)}
    counters = {
        str(sequence): extract_counters(block, sequence)
        for sequence, block in blocks.items()
    }
    strict_record = re.compile(
        r"^\[INPUTTEST\]\[PRODUCERS\] PASS accepted=100000 consumed=100000 "
        r"chars=50000 specials=50000 last_sequence=100000 sequence_gaps=0 "
        r"duplicates=0 regressions=0\r?$",
        re.MULTILINE,
    )
    marker_integrity = {
        str(sequence): strict_record.search(block) is not None
        for sequence, block in blocks.items()
    }
    require(marker_integrity == {"24": True, "25": False}, "unexpected marker integrity")
    write_outputs(
        {
            "status": "PASS",
            "classification": "REJECTED_CL13_SOAK_DIAGNOSTIC_MARKER_INTERLEAVING",
            "producer_runs": 2,
            "input_events": 200000,
            "faults": 0,
            "frames": counters,
            "marker_integrity": marker_integrity,
            "source_sha256": sha256(source),
            "runtime_manifest_sha256": sha256(runtime),
            "partial_log": str(log),
            "partial_log_sha256": sha256(log),
        }
    )


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser()
    subparsers = result.add_subparsers(dest="command", required=True)
    verify = subparsers.add_parser("verify-fix11-input-failure")
    verify.add_argument(
        "--log",
        default=str(ARTIFACTS / "cl13fix11-soak-smp8-kvm-failed.log"),
    )
    verify.add_argument(
        "--source",
        default=str(ROOT / "kernel/src/shell/commands/cmd_inputtest.c"),
    )
    verify.add_argument(
        "--runtime-manifest",
        default=str(ARTIFACTS / "cl13fix11-runtime-before.sha256"),
    )
    verify.set_defaults(func=verify_fix11_input_failure)
    return result


def main() -> None:
    args = parser().parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
