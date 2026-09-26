#!/usr/bin/env python3
"""Offline verifier for allocator rejection and bitmap word-search evidence."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
import zlib


HOST_RE = re.compile(
    r"^\[ALLOCTEST\]\[HOST\] (?P<status>PASS|FAIL) sizes=7 patterns=3 "
    r"reference_match=(?P<reference_match>[01]) "
    r"word_search=(?P<word_search>[01]) ranges=(?P<ranges>[01]) "
    r"overflow=(?P<overflow>[01])$"
)

RECORD_RE = re.compile(
    r"^\[ALLOCTEST\]\[HARDENING\] (?P<status>PASS|FAIL) "
    r"heap_overflow=(?P<heap_overflow>[0-9]+) "
    r"heap_interior=(?P<heap_interior>[0-9]+) "
    r"heap_foreign=(?P<heap_foreign>[0-9]+) "
    r"heap_double=(?P<heap_double>[0-9]+) "
    r"pmm_null=(?P<pmm_null>[0-9]+) "
    r"pmm_unaligned=(?P<pmm_unaligned>[0-9]+) "
    r"pmm_outside=(?P<pmm_outside>[0-9]+) "
    r"pmm_reserved=(?P<pmm_reserved>[0-9]+) "
    r"pmm_double=(?P<pmm_double>[0-9]+) "
    r"bitmap_words=(?P<bitmap_words>[0-9]+) "
    r"pmm_word_calls=(?P<pmm_word_calls>[0-9]+) "
    r"pmm_word_iterations=(?P<pmm_word_iterations>[0-9]+) "
    r"heap_integrity=(?P<heap_integrity>[01]) "
    r"pmm_integrity=(?P<pmm_integrity>[01]) census=(?P<census>[01]) "
    r"heap_ok=(?P<heap_ok>[01]) pmm_ok=(?P<pmm_ok>[01]) "
    r"bitmap_ok=(?P<bitmap_ok>[01])$"
)

VALID_HOST = (
    "[ALLOCTEST][HOST] PASS sizes=7 patterns=3 reference_match=1 "
    "word_search=1 ranges=1 overflow=1\n"
)
VALID_RECORD = (
    "[ALLOCTEST][HARDENING] PASS heap_overflow=2 heap_interior=1 "
    "heap_foreign=1 heap_double=2 pmm_null=1 pmm_unaligned=1 "
    "pmm_outside=1 pmm_reserved=1 pmm_double=1 bitmap_words=3 "
    "pmm_word_calls=1 pmm_word_iterations=95 heap_integrity=1 "
    "pmm_integrity=1 census=1 heap_ok=1 pmm_ok=1 bitmap_ok=1\n"
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


def parse_host(text: str) -> dict[str, int | str]:
    matches = [match for line in text.splitlines()
               if (match := HOST_RE.fullmatch(line))]
    require(len(matches) == 1, "HOST_RECORD_COUNT", str(len(matches)))
    fields: dict[str, int | str] = {"status": matches[0].group("status")}
    for key, value in matches[0].groupdict().items():
        if key != "status":
            fields[key] = int(value)
    require(fields["status"] == "PASS", "HOST_STATUS_FAIL")
    require(fields["reference_match"] == 1, "BITMAP_REFERENCE_DIVERGENCE")
    require(fields["word_search"] == 1, "BITMAP_WORD_SEARCH_ABSENT")
    require(fields["ranges"] == 1, "BITMAP_RANGE_FAILURE")
    require(fields["overflow"] == 1, "BITMAP_OVERFLOW_ACCEPTED")
    return fields


def parse_runtime(text: str) -> dict[str, int | str]:
    matches = [match for line in text.splitlines()
               if (match := RECORD_RE.fullmatch(line))]
    require(len(matches) == 1, "HARDENING_RECORD_COUNT", str(len(matches)))
    fields: dict[str, int | str] = {"status": matches[0].group("status")}
    for key, value in matches[0].groupdict().items():
        if key != "status":
            fields[key] = int(value)
    require(fields["status"] == "PASS", "HARDENING_STATUS_FAIL")
    require(fields["heap_overflow"] == 2, "HEAP_OVERFLOW_NOT_REJECTED")
    require(fields["heap_interior"] == 1, "HEAP_INTERIOR_FREE_NOT_REJECTED")
    require(fields["heap_foreign"] == 1, "HEAP_FOREIGN_FREE_NOT_REJECTED")
    require(fields["heap_double"] == 2, "HEAP_DOUBLE_FREE_NOT_REJECTED")
    for key in ("pmm_null", "pmm_unaligned", "pmm_outside",
                "pmm_reserved", "pmm_double"):
        require(fields[key] == 1, f"{key.upper()}_NOT_REJECTED")
    require(fields["bitmap_words"] >= 3, "BITMAP_WORD_ADVANCE_NOT_PROVEN")
    require(fields["pmm_word_calls"] >= 1, "PMM_WORD_SEARCH_NOT_CALLED")
    require(fields["pmm_word_iterations"] >= fields["pmm_word_calls"],
            "PMM_WORD_ITERATION_INVALID")
    for key in ("heap_integrity", "pmm_integrity", "census",
                "heap_ok", "pmm_ok", "bitmap_ok"):
        require(fields[key] == 1, f"{key.upper()}_FAIL")
    return fields


def fixture_error(parser, text: str, expected: str, fixture_id: str) -> dict:
    try:
        parser(text)
    except EvidenceError as error:
        require(error.code == expected, "FIXTURE_WRONG_REASON",
                f"{fixture_id}: {error.code}")
        return {"id": fixture_id, "status": "NEGATIVE_DETECTED",
                "expected_code": expected, "observed_code": error.code}
    raise EvidenceError("FIXTURE_ACCEPTED", fixture_id)


def make_fixtures(output: Path) -> None:
    require(not output.exists(), "FIXTURE_OUTPUT_EXISTS", str(output))
    output.mkdir(parents=True)
    valid_runtime = output / "valid-runtime.log"
    silent_double = output / "silent-double-free.log"
    bitmap_divergence = output / "bitmap-reference-divergence.log"
    valid_runtime.write_text(VALID_RECORD, encoding="utf-8")
    silent_double.write_text(VALID_RECORD.replace("heap_double=2", "heap_double=0"),
                             encoding="utf-8")
    bitmap_divergence.write_text(
        VALID_HOST.replace("reference_match=1", "reference_match=0"),
        encoding="utf-8")
    parse_runtime(valid_runtime.read_text(encoding="utf-8"))
    records = [
        {"id": "valid-runtime", "status": "PASS"},
        fixture_error(parse_runtime, silent_double.read_text(encoding="utf-8"),
                      "HEAP_DOUBLE_FREE_NOT_REJECTED", "silent-double-free"),
        fixture_error(parse_host, bitmap_divergence.read_text(encoding="utf-8"),
                      "BITMAP_REFERENCE_DIVERGENCE",
                      "bitmap-reference-divergence"),
    ]
    (output / "result.json").write_text(json.dumps({
        "schema": 1, "status": "PASS", "records": records,
    }, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print("ALLOC_HARDENING_FIXTURES: PASS negatives=2")


def verify_hash_manifest(root: Path, path: Path) -> None:
    require(path.is_file(), "HASH_MANIFEST_MISSING", str(path))
    for number, line in enumerate(path.read_text().splitlines(), 1):
        if not line:
            continue
        match = re.fullmatch(r"([0-9a-f]{64})  (.+)", line)
        require(match is not None, "HASH_MANIFEST_INVALID", f"{path}:{number}")
        target = root / match.group(2)
        require(sha256(target) == match.group(1), "HASH_MISMATCH", str(target))


def transaction_segment(lines: list[str], row: dict) -> str:
    payload = "alloctest hardening"
    require(row.get("sequence") == 1, "TRANSACTION_SEQUENCE_MISMATCH")
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
    require(segment.count("[HARNESS][BEGIN] seq=1") == 1,
            "HARNESS_BEGIN_COUNT")
    require(segment.count("[HARNESS][END] seq=1 status=0") == 1,
            "HARNESS_END_COUNT")
    return "\n".join(segment) + "\n"


def verify_runtime(evidence: Path, candidate_hash: str) -> dict:
    name = "q35-kvm-smp4"
    profile_dir = evidence / "runtime" / name
    profile = read_env(profile_dir / "profile.env")
    launch = read_env(profile_dir / "qemu" / "launch.env")
    required = {"schema": "1", "name": name, "machine": "q35",
                "accel": "kvm", "smp": "4", "cleanup": "PASS",
                "run_complete": "YES", "candidate_sha256": candidate_hash}
    for key, expected in required.items():
        require(profile.get(key) == expected, "PROFILE_FIELD_MISMATCH",
                f"{name}:{key}")
    for key, expected in (("MACHINE", "q35"), ("ACCEL", "kvm"),
                          ("SMP", "4"), ("KERNEL_SHA256", candidate_hash)):
        require(launch.get(key) == expected, "LAUNCH_FIELD_MISMATCH",
                f"{name}:{key}")
    lines = (profile_dir / "qemu" / "qemu-serial.log").read_text(
        errors="replace").splitlines()
    rows = read_jsonl(profile_dir / "commands.jsonl")
    require(len(rows) == 1, "TRANSACTION_COUNT_MISMATCH", str(len(rows)))
    segment = transaction_segment(lines, rows[0])
    fields = parse_runtime(segment)
    require(segment.count("[ASSERT][WARN]") == 11,
            "REJECTION_WARNING_COUNT", str(segment.count("[ASSERT][WARN]")))
    serial = "\n".join(lines)
    require("[KERNEL][PANIC]" not in serial and "[PANIC]" not in serial,
            "PANIC_PRESENT", name)
    return {"name": name, "status": "PASS",
            "bitmap_words": fields["bitmap_words"],
            "pmm_word_iterations": fields["pmm_word_iterations"]}


def verify_all(evidence: Path) -> None:
    require(evidence.is_dir(), "EVIDENCE_MISSING", str(evidence))
    verify_hash_manifest(evidence / "source",
                         evidence / "source" / "SHA256SUMS")
    for profile in ("normal", "ubsan"):
        run = evidence / "host" / profile / "run.log"
        require(run.is_file(), "HOST_LOG_MISSING", profile)
        parse_host(run.read_text(encoding="utf-8"))
        require((evidence / "host" / profile / "exit-code.txt").read_text().strip() == "0",
                "HOST_EXIT_NONZERO", profile)

    for name in ("deps-check", "kernel-check", "stack-check", "image"):
        require((evidence / "build" / f"{name}.exit-code.txt").read_text().strip() == "0",
                "BUILD_GATE_FAILED", name)
    candidate_hash = sha256(evidence / "candidate" / "kernel.elf")
    verify_hash_manifest(evidence / "candidate",
                         evidence / "candidate" / "SHA256SUMS")
    runtime = verify_runtime(evidence, candidate_hash)

    production = evidence / "production"
    require((production / "production-image.exit-code.txt").read_text().strip() == "0",
            "PRODUCTION_BUILD_FAILED")
    require((production / "policy.exit-code.txt").read_text().strip() == "0",
            "PRODUCTION_POLICY_FAILED")
    strings = (production / "kernel.strings.txt").read_text(errors="replace")
    symbols = (production / "kernel.symbols.txt").read_text(errors="replace")
    require("alloctest" not in strings.lower(), "ALLOCTEST_STRING_IN_PRODUCTION")
    require("alloctest" not in symbols.lower(), "ALLOCTEST_SYMBOL_IN_PRODUCTION")
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

    summary = {"schema": 1, "status": "PASS",
               "candidate_sha256": candidate_hash,
               "host_profiles": ["normal", "ubsan"],
               "runtime_profile": runtime, "negative_fixtures": 2}
    (evidence / "verify-result.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print("ALLOC_HARDENING_VERIFY: PASS host=2 runtime=1 production=1 negatives=2")


def main() -> int:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="mode", required=True)
    record = subparsers.add_parser("record")
    record.add_argument("transcript", type=Path)
    fixtures = subparsers.add_parser("fixtures")
    fixtures.add_argument("output", type=Path)
    all_parser = subparsers.add_parser("all")
    all_parser.add_argument("evidence", type=Path)
    args = parser.parse_args()
    try:
        if args.mode == "record":
            parse_runtime(args.transcript.read_text(encoding="utf-8"))
            print("ALLOC_HARDENING_RECORD: PASS")
        elif args.mode == "fixtures":
            make_fixtures(args.output)
        else:
            verify_all(args.evidence)
    except (EvidenceError, OSError, UnicodeError) as error:
        code = error.code if isinstance(error, EvidenceError) else "IO_ERROR"
        detail = error.detail if isinstance(error, EvidenceError) else str(error)
        print(f"ALLOC_HARDENING_VERIFY: FAIL code={code} detail={detail}",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
