#!/usr/bin/env python3
"""Offline verifier for the FIFO spinlock progress campaign."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
import zlib


RECORD_RE = re.compile(
    r"^\[LOCKTEST\]\[PROGRESS\] (?P<status>PASS|FAIL) "
    r"cpus=(?P<cpus>[0-9]+) iterations=(?P<iterations>[0-9]+) "
    r"acquisitions=(?P<acquisitions>[0-9]+) "
    r"exclusive_violations=(?P<exclusive_violations>[0-9]+) "
    r"if_violations=(?P<if_violations>[0-9]+) "
    r"max_queue_distance=(?P<max_queue_distance>[0-9]+) "
    r"limit=(?P<limit>[0-9]+) slots=(?P<slots>[0-9]+) "
    r"slot_mask=(?P<slot_mask>[0-9]+) "
    r"observer_samples=(?P<observer_samples>[0-9]+) "
    r"contended=(?P<contended>[0-9]+) "
    r"trylock_no_ticket=(?P<trylock_no_ticket>[01]) "
    r"static_zero=(?P<static_zero>[01]) reaped=(?P<reaped>[0-9]+) "
    r"max_active=(?P<max_active>[0-9]+)$"
)

HOST_RE = re.compile(
    r"^\[LOCKTEST\]\[HOST\] PASS static_zero=1 wrap=1 "
    r"trylock_no_ticket=1 irq_restore=1 exclusion=1 progress=1 "
    r"threads=8 iterations=20000 acquisitions=160000 "
    r"max_distance=([0-7])$"
)


class EvidenceError(Exception):
    def __init__(self, code: str, detail: str = "") -> None:
        super().__init__(code)
        self.code = code
        self.detail = detail


def require(condition: bool, code: str, detail: str = "") -> None:
    if not condition:
        raise EvidenceError(code, detail)


def sha256(path: Path) -> str:
    require(path.is_file(), "FILE_MISSING", str(path))
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_json(path: Path) -> dict:
    require(path.is_file(), "JSON_MISSING", str(path))
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise EvidenceError("JSON_INVALID", f"{path}: {error}") from error
    require(isinstance(value, dict), "JSON_TYPE_INVALID", str(path))
    return value


def read_jsonl(path: Path) -> list[dict]:
    require(path.is_file(), "JSONL_MISSING", str(path))
    rows: list[dict] = []
    try:
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            if not line:
                continue
            value = json.loads(line)
            require(isinstance(value, dict), "JSONL_ROW_INVALID",
                    f"{path}:{number}")
            rows.append(value)
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise EvidenceError("JSONL_INVALID", f"{path}: {error}") from error
    return rows


def read_env(path: Path) -> dict[str, str]:
    require(path.is_file(), "ENV_MISSING", str(path))
    values: dict[str, str] = {}
    for number, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        require("=" in line, "ENV_LINE_INVALID", f"{path}:{number}")
        key, value = line.split("=", 1)
        values[key] = value.replace("\\ ", " ").replace("\\(", "(").replace("\\)", ")")
    return values


def parse_progress(text: str, cpus: int, iterations: int) -> dict[str, int | str]:
    matches = [match for line in text.splitlines()
               if (match := RECORD_RE.fullmatch(line))]
    require(len(matches) == 1, "PROGRESS_RECORD_COUNT", str(len(matches)))
    fields: dict[str, int | str] = {"status": matches[0].group("status")}
    for key, value in matches[0].groupdict().items():
        if key != "status":
            fields[key] = int(value)
    require(fields["status"] == "PASS", "PROGRESS_STATUS_FAIL")
    require(fields["cpus"] == cpus, "CPU_COUNT_MISMATCH")
    require(fields["iterations"] == iterations, "ITERATION_COUNT_MISMATCH")
    expected = cpus * iterations
    require(fields["acquisitions"] == expected, "ACQUISITION_COUNT_MISMATCH")
    require(fields["exclusive_violations"] == 0, "EXCLUSION_VIOLATION")
    require(fields["if_violations"] == 0, "IF_RESTORE_VIOLATION")
    require(fields["limit"] == cpus - 1, "QUEUE_LIMIT_MISMATCH")
    require(fields["max_queue_distance"] <= fields["limit"],
            "QUEUE_DISTANCE_EXCEEDED")
    require(fields["slots"] == cpus, "CPU_COVERAGE_MISMATCH")
    require(fields["slot_mask"] == (1 << cpus) - 1, "CPU_MASK_MISMATCH")
    require(fields["observer_samples"] == expected, "OBSERVER_COUNT_MISMATCH")
    require(fields["contended"] > 0 if cpus > 1 else True,
            "REAL_CONTENTION_ABSENT")
    require(fields["trylock_no_ticket"] == 1, "TRYLOCK_CONSUMED_TICKET")
    require(fields["static_zero"] == 1, "STATIC_ZERO_INVALID")
    require(fields["reaped"] == cpus, "WORKER_REAP_MISMATCH")
    require(fields["max_active"] == cpus, "CONCURRENCY_NOT_OBSERVED")
    return fields


def write_fixture(path: Path, *, cpus: int = 4, iterations: int = 100,
                  exclusive: int = 0, distance: int = 3) -> None:
    acquisitions = cpus * iterations
    path.write_text(
        "[LOCKTEST][PROGRESS] PASS "
        f"cpus={cpus} iterations={iterations} acquisitions={acquisitions} "
        f"exclusive_violations={exclusive} if_violations=0 "
        f"max_queue_distance={distance} limit={cpus - 1} slots={cpus} "
        f"slot_mask={(1 << cpus) - 1} observer_samples={acquisitions} "
        "contended=200 trylock_no_ticket=1 static_zero=1 "
        f"reaped={cpus} max_active={cpus}\n",
        encoding="utf-8")


def fixture_error(path: Path, expected: str) -> dict[str, str]:
    try:
        parse_progress(path.read_text(encoding="utf-8"), 4, 100)
    except EvidenceError as error:
        require(error.code == expected, "FIXTURE_WRONG_REASON",
                f"{path.name}: {error.code}")
        return {"id": path.stem, "status": "NEGATIVE_DETECTED",
                "expected_code": expected, "observed_code": error.code}
    raise EvidenceError("FIXTURE_ACCEPTED", path.name)


def make_fixtures(output: Path) -> None:
    require(not output.exists(), "FIXTURE_OUTPUT_EXISTS", str(output))
    output.mkdir(parents=True)
    valid = output / "valid.log"
    unfair = output / "unfair.log"
    nonexclusive = output / "nonexclusive.log"
    write_fixture(valid)
    write_fixture(unfair, distance=4)
    write_fixture(nonexclusive, exclusive=1)
    parse_progress(valid.read_text(encoding="utf-8"), 4, 100)
    records = [
        {"id": "valid", "status": "PASS"},
        fixture_error(unfair, "QUEUE_DISTANCE_EXCEEDED"),
        fixture_error(nonexclusive, "EXCLUSION_VIOLATION"),
    ]
    (output / "result.json").write_text(json.dumps({
        "schema": 1, "status": "PASS", "records": records,
    }, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print("LOCK_PROGRESS_FIXTURES: PASS negatives=2")


def verify_hash_manifest(root: Path, path: Path) -> None:
    require(path.is_file(), "HASH_MANIFEST_MISSING", str(path))
    for number, line in enumerate(path.read_text().splitlines(), 1):
        if not line:
            continue
        match = re.fullmatch(r"([0-9a-f]{64})  (.+)", line)
        require(match is not None, "HASH_MANIFEST_INVALID",
                f"{path}:{number}")
        target = root / match.group(2)
        require(sha256(target) == match.group(1), "HASH_MISMATCH",
                str(target))


def transaction_segment(lines: list[str], row: dict, sequence: int,
                        payload: str) -> str:
    require(row.get("sequence") == sequence, "TRANSACTION_SEQUENCE_MISMATCH")
    require(row.get("payload") == payload, "TRANSACTION_PAYLOAD_MISMATCH")
    require(row.get("handler_status") == 0, "HANDLER_STATUS_NONZERO")
    require(row.get("classification") == "PASS", "TRANSACTION_NOT_PASS")
    require(row.get("transport_status") == "PASS", "TRANSPORT_NOT_PASS")
    require(row.get("disposition") == "EXECUTED", "TRANSACTION_NOT_EXECUTED")
    require(row.get("checksum_verified") is True, "TRANSACTION_CRC_UNVERIFIED")
    expected_crc = f"{zlib.crc32(payload.encode()) & 0xffffffff:08x}"
    require(row.get("crc32") == expected_crc, "TRANSACTION_CRC_MISMATCH")
    start = row.get("start_line_count")
    end = row.get("end_line_count")
    require(isinstance(start, int) and isinstance(end, int) and
            0 <= start < end <= len(lines), "TRANSACTION_BOUNDS_INVALID")
    segment = lines[start:end]
    require(segment.count(f"[HARNESS][BEGIN] seq={sequence}") == 1,
            "HARNESS_BEGIN_COUNT")
    require(segment.count(f"[HARNESS][END] seq={sequence} status=0") == 1,
            "HARNESS_END_COUNT")
    return "\n".join(segment) + "\n"


def verify_profile(evidence: Path, smp: int, iterations: int,
                   candidate_hash: str) -> dict:
    name = f"q35-kvm-smp{smp}"
    profile_dir = evidence / "runtime" / name
    profile = read_env(profile_dir / "profile.env")
    launch = read_env(profile_dir / "qemu" / "launch.env")
    required = {"schema": "1", "name": name, "machine": "q35",
                "accel": "kvm", "smp": str(smp), "cleanup": "PASS",
                "run_complete": "YES", "candidate_sha256": candidate_hash}
    for key, expected in required.items():
        require(profile.get(key) == expected, "PROFILE_FIELD_MISMATCH",
                f"{name}:{key}")
    for key, expected in (("MACHINE", "q35"), ("ACCEL", "kvm"),
                          ("SMP", str(smp)), ("KERNEL_SHA256", candidate_hash)):
        require(launch.get(key) == expected, "LAUNCH_FIELD_MISMATCH",
                f"{name}:{key}")
    serial_path = profile_dir / "qemu" / "qemu-serial.log"
    lines = serial_path.read_text(errors="replace").splitlines()
    rows = read_jsonl(profile_dir / "commands.jsonl")
    payloads = [f"locktest progress {smp} {iterations}"]
    if smp == 8:
        payloads.append("accounttest clock-smp 32 1000000")
    require(len(rows) == len(payloads), "TRANSACTION_COUNT_MISMATCH", name)
    progress = None
    for sequence, payload in enumerate(payloads, 1):
        segment = transaction_segment(lines, rows[sequence - 1], sequence,
                                      payload)
        if sequence == 1:
            progress = parse_progress(segment, smp, iterations)
        else:
            marker = re.findall(r"^\[ACCOUNT\]\[CLOCK_SMP\] PASS .+$",
                                segment, re.MULTILINE)
            require(len(marker) == 1, "CLOCK_SMP_PASS_COUNT", str(len(marker)))
            require("api_regressions=0" in marker[0] and
                    "local_regressions=0" in marker[0] and
                    "free_inflight=0" in marker[0], "CLOCK_SMP_REGRESSION")
    serial = "\n".join(lines)
    require("[KERNEL][PANIC]" not in serial and "[PANIC]" not in serial,
            "PANIC_PRESENT", name)
    require(progress is not None, "PROGRESS_RECORD_MISSING", name)
    return {"name": name, "status": "PASS", "cpus": smp,
            "iterations": iterations,
            "max_queue_distance": progress["max_queue_distance"],
            "fairness_authority": "AUTHORITATIVE" if smp == 8 else "FUNCTIONAL_ONLY"}


def verify_all(evidence: Path) -> None:
    require(evidence.is_dir(), "EVIDENCE_MISSING", str(evidence))
    verify_hash_manifest(evidence / "source",
                         evidence / "source" / "SHA256SUMS")
    for profile in ("normal", "ubsan"):
        run = evidence / "host" / profile / "run.log"
        require(run.is_file(), "HOST_LOG_MISSING", profile)
        matches = [line for line in run.read_text().splitlines()
                   if HOST_RE.fullmatch(line)]
        require(len(matches) == 1, "HOST_RESULT_INVALID", profile)
        require((evidence / "host" / profile / "exit-code.txt").read_text().strip() == "0",
                "HOST_EXIT_NONZERO", profile)

    for name in ("deps-check", "kernel-check", "stack-check", "image"):
        require((evidence / "build" / f"{name}.exit-code.txt").read_text().strip() == "0",
                "BUILD_GATE_FAILED", name)
    candidate_hash = sha256(evidence / "candidate" / "kernel.elf")
    verify_hash_manifest(evidence / "candidate", evidence / "candidate" / "SHA256SUMS")
    profiles = [verify_profile(evidence, 8, 9000, candidate_hash),
                verify_profile(evidence, 24, 1000, candidate_hash)]

    production = evidence / "production"
    require((production / "production-image.exit-code.txt").read_text().strip() == "0",
            "PRODUCTION_BUILD_FAILED")
    require((production / "policy.exit-code.txt").read_text().strip() == "0",
            "PRODUCTION_POLICY_FAILED")
    strings = (production / "kernel.strings.txt").read_text(errors="replace")
    symbols = (production / "kernel.symbols.txt").read_text(errors="replace")
    require("locktest" not in strings.lower(), "LOCKTEST_STRING_IN_PRODUCTION")
    require("locktest" not in symbols.lower(), "LOCKTEST_SYMBOL_IN_PRODUCTION")
    require("[BOOT][PRODUCTION_TEST_POLICY] PASS" in
            (production / "policy.log").read_text(errors="replace"),
            "PRODUCTION_POLICY_MARKER_MISSING")

    fixtures = read_json(evidence / "fixtures" / "result.json")
    require(fixtures.get("status") == "PASS", "FIXTURE_SUITE_FAILED")
    records = fixtures.get("records")
    require(isinstance(records, list) and len(records) == 3,
            "FIXTURE_RECORD_COUNT")
    negatives = [row for row in records
                 if isinstance(row, dict) and row.get("status") == "NEGATIVE_DETECTED"]
    require(len(negatives) == 2, "FIXTURE_NEGATIVE_COUNT")

    summary = {"schema": 1, "status": "PASS", "candidate_sha256": candidate_hash,
               "host_profiles": ["normal", "ubsan"], "runtime_profiles": profiles,
               "negative_fixtures": 2}
    (evidence / "verify-result.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print("LOCK_PROGRESS_VERIFY: PASS host=2 runtime=2 production=1 negatives=2")


def main() -> int:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="mode", required=True)
    record = subparsers.add_parser("record")
    record.add_argument("transcript", type=Path)
    record.add_argument("--cpus", type=int, required=True)
    record.add_argument("--iterations", type=int, required=True)
    fixtures = subparsers.add_parser("fixtures")
    fixtures.add_argument("output", type=Path)
    all_parser = subparsers.add_parser("all")
    all_parser.add_argument("evidence", type=Path)
    args = parser.parse_args()
    try:
        if args.mode == "record":
            parse_progress(args.transcript.read_text(encoding="utf-8"),
                           args.cpus, args.iterations)
            print("LOCK_PROGRESS_RECORD: PASS")
        elif args.mode == "fixtures":
            make_fixtures(args.output)
        else:
            verify_all(args.evidence)
    except (EvidenceError, OSError, UnicodeError) as error:
        code = error.code if isinstance(error, EvidenceError) else "IO_ERROR"
        detail = error.detail if isinstance(error, EvidenceError) else str(error)
        print(f"LOCK_PROGRESS_VERIFY: FAIL code={code} detail={detail}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
