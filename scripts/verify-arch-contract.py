#!/usr/bin/env python3
"""Verify architecture-contract evidence without building or starting QEMU."""

import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

XHCI_FENCE_COUNT = 48
XHCI_FENCE_SHA256 = \
    "ffbc1f4c163b7a4b60593b84871aa400fd0a2d1df9282c52bc9fb387403aacc2"


class ValidationError(Exception):
    def __init__(self, code: str, detail: str = "") -> None:
        super().__init__(code)
        self.code = code
        self.detail = detail


def require(condition: bool, code: str, detail: str = "") -> None:
    if not condition:
        raise ValidationError(code, detail)


def load_json(path: Path, code: str = "JSON_INVALID") -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ValidationError(code, f"{path}: {error}") from error
    require(isinstance(value, dict), code, f"{path}: root is not an object")
    return value


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def xhci_fence_inventory(source: bytes) -> bytes:
    return b"".join(
        line for line in source.splitlines(keepends=True)
        if re.search(rb"\b[mls]fence\b", line)
    )


def xhci_fences_match_contract(source: bytes) -> bool:
    inventory = xhci_fence_inventory(source)
    return (len(inventory.splitlines()) == XHCI_FENCE_COUNT and
            hashlib.sha256(inventory).hexdigest() == XHCI_FENCE_SHA256)


def checked(root: Path, value, code: str = "PATH_INVALID") -> Path:
    require(isinstance(value, str) and value != "", code, str(value))
    candidate = (root / value).resolve()
    try:
        candidate.relative_to(root.resolve())
    except ValueError as error:
        raise ValidationError(code, value) from error
    require(candidate.is_file(), "FILE_MISSING", value)
    return candidate


def require_file(root: Path, record: dict, prefix: str) -> Path:
    path = checked(root, record.get("path"), f"{prefix}_PATH_INVALID")
    require(record.get("bytes") == path.stat().st_size,
            f"{prefix}_SIZE_MISMATCH", str(path))
    require(record.get("sha256") == sha256(path),
            f"{prefix}_HASH_MISMATCH", str(path))
    return path


def parse_fields(line: str, prefix: str, expected: tuple[str, ...]) -> dict:
    require(line.startswith(prefix + " "), "MARKER_PREFIX_INVALID", line)
    fields = {}
    for token in line[len(prefix) + 1:].split(" "):
        require(token and "=" in token, "MARKER_FIELD_INVALID", token)
        key, value = token.split("=", 1)
        require(key and value, "MARKER_FIELD_INVALID", token)
        require(key not in fields, "MARKER_FIELD_DUPLICATE", key)
        fields[key] = value
    require(tuple(fields) == expected, "MARKER_FIELD_SET_INVALID",
            ",".join(fields))
    return fields


def one_marker(text: str, prefix: str, expected: tuple[str, ...]) -> dict:
    lines = [line for line in text.splitlines() if line.startswith(prefix)]
    require(len(lines) == 1, "MARKER_COUNT_INVALID",
            f"{prefix}: {len(lines)}")
    return parse_fields(lines[0], prefix, expected)


EARLY_FIELDS = (
    "status", "cpuid_checks", "msr_checks", "read_gp", "write_gp",
    "if_preserved", "invalid_msr", "rdmsr_limited",
)
TRACE_FIELDS = ("status", "cpus", "active", "target")
RUNTIME_FIELDS = (
    "status", "cpus", "joined", "complete", "overlap", "snapshots",
    "concurrent_samples", "concurrent_unavailable", "concurrent_active",
    "progress", "budget_ok",
)
BUDGET_FIELDS = ("status", "checks", "expected")
PIN_FIELDS = (
    "status", "if_before", "if_during", "if_after", "cpu_before",
    "cpu_during", "cpu_after", "slot", "snapshot_if1", "snapshot_if0",
)
CPUID_FIELDS = (
    "case", "domain", "cpu_id", "leaf", "subleaf", "accepted",
    "api_eax", "api_ebx", "api_ecx", "api_edx", "raw_valid",
    "raw_eax", "raw_ebx", "raw_ecx", "raw_edx", "legacy_valid",
    "legacy_eax", "legacy_ebx", "legacy_ecx", "legacy_edx",
)
MSR_READ_FIELDS = (
    "case", "cpu_id", "index", "result", "before", "after",
    "if_before", "if_after",
)
MSR_WRITE_FIELDS = (
    "case", "cpu_id", "index", "result", "value", "if_before",
    "if_after",
)
MSR_FS_FIELDS = (
    "case", "cpu_id", "index", "initial_read", "original",
    "temporary_write", "temporary", "temporary_read", "readback",
    "restore_write", "restore_read", "restored", "if_before", "if_after",
)


def integer(value: str, code: str, detail: str = "") -> int:
    require(isinstance(value, str) and re.fullmatch(
        r"(?:0x[0-9a-f]+|[0-9]+)", value) is not None, code,
        detail or str(value))
    return int(value, 0)


def marker_set(text: str, prefix: str, expected: tuple[str, ...]) -> list[dict]:
    lines = [line for line in text.splitlines() if line.startswith(prefix)]
    return [parse_fields(line, prefix, expected) for line in lines]


def validate_cpuid_records(serial: str, relative: str) -> tuple[list[dict], int]:
    records = marker_set(serial, "[ARCH_TEST][CPUID]", CPUID_FIELDS)
    require(len(records) == 7, "CPUID_RECORD_COUNT_INVALID", relative)
    normalized = []
    for fields in records:
        record = {"case": fields["case"], "domain": fields["domain"]}
        for name in CPUID_FIELDS[2:]:
            record[name] = integer(fields[name], "CPUID_VALUE_INVALID",
                                   f"{relative}: {name}")
        normalized.append(record)
    cpu_ids = {record["cpu_id"] for record in normalized}
    require(len(cpu_ids) == 1, "CPUID_CPU_ID_MIXED", relative)
    keyed = {(record["case"], record["leaf"], record["subleaf"]): record
             for record in normalized}
    require(len(keyed) == len(normalized), "CPUID_RECORD_DUPLICATE", relative)
    expected_keys = {
        ("basic-maximum", 0, 0), ("legacy-wrapper", 7, 0),
        ("extended-maximum", 0x80000000, 0),
        ("cache", 4, 0), ("cache", 4, 1),
    }
    require(expected_keys.issubset(keyed), "CPUID_RECORD_SET_INVALID", relative)
    for key in expected_keys:
        record = keyed[key]
        require(record["accepted"] == 1 and record["raw_valid"] == 1,
                "CPUID_RAW_RECORD_INVALID", f"{relative}: {key}")
        api = tuple(record[f"api_{name}"] for name in ("eax", "ebx", "ecx", "edx"))
        raw = tuple(record[f"raw_{name}"] for name in ("eax", "ebx", "ecx", "edx"))
        require(api == raw, "CPUID_RAW_MISMATCH", f"{relative}: {key}")
    legacy = keyed[("legacy-wrapper", 7, 0)]
    require(legacy["legacy_valid"] == 1 and
            tuple(legacy[f"legacy_{name}"] for name in
                  ("eax", "ebx", "ecx", "edx")) ==
            tuple(legacy[f"raw_{name}"] for name in
                  ("eax", "ebx", "ecx", "edx")),
            "CPUID_LEGACY_ECX_INVALID", relative)
    cache0 = keyed[("cache", 4, 0)]
    cache1 = keyed[("cache", 4, 1)]
    require(any(cache0[f"api_{name}"] != cache1[f"api_{name}"]
                for name in ("eax", "ebx", "ecx", "edx")),
            "CPUID_SUBLEAF_INVALID", relative)
    rejects = [record for record in normalized
               if record["case"] in ("basic-rejected", "extended-rejected")]
    require(len(rejects) == 2, "CPUID_REJECT_RECORD_INVALID", relative)
    maxima = {record["domain"]: record["api_eax"] for record in normalized
              if record["case"].endswith("maximum")}
    for record in rejects:
        require(record["accepted"] == 0 and record["raw_valid"] == 0 and
                record["leaf"] == maxima[record["domain"]] + 1 and
                all(record[f"api_{name}"] == 0 for name in
                    ("eax", "ebx", "ecx", "edx")),
                "CPUID_BOUNDS_INVALID", f"{relative}: {record['domain']}")
    return normalized, next(iter(cpu_ids))


def validate_msr_records(serial: str, accel: str, cpu_id: int,
                         relative: str) -> list[dict]:
    lines = [line for line in serial.splitlines()
             if line.startswith("[ARCH_TEST][MSR]")]
    require(len(lines) == 4, "MSR_RECORD_COUNT_INVALID", relative)
    records = []
    for line in lines:
        tokens = line[len("[ARCH_TEST][MSR]") + 1:].split(" ")
        case_token = tokens[0]
        require(case_token.startswith("case="), "MSR_RECORD_INVALID", line)
        case = case_token.split("=", 1)[1]
        schema = (MSR_FS_FIELDS if case == "fs-base" else
                  MSR_WRITE_FIELDS if case == "invalid-write" else
                  MSR_READ_FIELDS)
        fields = parse_fields(line, "[ARCH_TEST][MSR]", schema)
        record = {"case": case}
        for name in schema[1:]:
            record[name] = integer(fields[name], "MSR_VALUE_INVALID",
                                   f"{relative}: {case}:{name}")
        require(record["cpu_id"] == cpu_id, "MSR_CPU_ID_MISMATCH", case)
        require(record["if_before"] == record["if_after"] == 0,
                "MSR_IF_MISMATCH", case)
        records.append(record)
    keyed = {record["case"]: record for record in records}
    require(set(keyed) == {"tsc", "invalid-read", "invalid-write", "fs-base"},
            "MSR_RECORD_SET_INVALID", relative)
    require(keyed["tsc"]["index"] == 0x10 and
            keyed["tsc"]["result"] == 1,
            "MSR_VALID_READ_INVALID", relative)
    invalid_read = keyed["invalid-read"]
    sentinel = 0x9e3779b97f4a7c15
    if accel == "kvm":
        require(invalid_read["result"] == 0 and
                invalid_read["before"] == sentinel and
                invalid_read["after"] == sentinel,
                "MSR_REJECTED_READ_OUTPUT_CHANGED", relative)
    else:
        require(invalid_read["result"] == 1,
                "TCG_MSR_LIMIT_CLASSIFICATION_INVALID", relative)
    invalid_write = keyed["invalid-write"]
    require(invalid_write["index"] == 0x6e1 and
            invalid_write["result"] == 0 and
            invalid_write["value"] == 1 << 32,
            "MSR_INVALID_WRITE_RECORD_INVALID", relative)
    fs = keyed["fs-base"]
    require(fs["index"] == 0xc0000100 and
            all(fs[name] == 1 for name in
                ("initial_read", "temporary_write", "temporary_read",
                 "restore_write", "restore_read")) and
            fs["temporary"] == (fs["original"] ^ 1) and
            fs["readback"] == fs["temporary"] and
            fs["restored"] == fs["original"],
            "MSR_FS_RESTORE_INVALID", relative)
    return records


def artifact_path(run_root: Path, result: dict, name: str) -> Path:
    artifacts = result.get("artifacts")
    require(isinstance(artifacts, dict) and name in artifacts,
            "RUN_ARTIFACT_MISSING", name)
    record = dict(artifacts[name])
    record["path"] = name
    return require_file(run_root, record, "RUN_ARTIFACT")


def unique_number(text: str, name: str, base: int = 10,
                  code: str = "GDB_FIELD_INVALID") -> int:
    values = re.findall(rf"^{re.escape(name)}=([^\s]+)$", text, re.MULTILINE)
    require(len(values) == 1, code, f"{name}: count={len(values)}")
    try:
        return int(values[0], base)
    except ValueError as error:
        raise ValidationError(code, f"{name}: {values[0]}") from error


def parse_gp_events(text: str, prefix: str = "GP_HIT") -> list[dict]:
    pattern = re.compile(
        rf"^{re.escape(prefix)}=(\d+) FRAME_RIP=0x([0-9a-f]+) "
        r"ERROR=0x([0-9a-f]+) CS=0x([0-9a-f]+)$", re.MULTILINE)
    return [
        {"ordinal": int(match.group(1)), "rip": int(match.group(2), 16),
         "error": int(match.group(3), 16), "cs": int(match.group(4), 16)}
        for match in pattern.finditer(text)
    ]


def parse_trace_raw(text: str, smp: int, relative: str) -> dict:
    marker_names = ("OBSERVER_READY", "TARGET_COUNT", "JOINED_MASK",
                    "ACTIVE_MASK", "TRACE_RECORD_STRIDE", "TRACE_CELL_BASE",
                    "TOPOLOGY_STRIDE", "OBSERVER_RELEASED")
    bases = {"JOINED_MASK": 16, "ACTIVE_MASK": 16,
             "TRACE_CELL_BASE": 16}
    markers = {name: unique_number(
        text, name, bases.get(name, 10), "TRACE_GDB_FIELD_INVALID")
        for name in marker_names}
    row_pattern = re.compile(
        r"^TRACE_SLOT=(\d+) SEQUENCE=(\d+) ADDRESS=0x([0-9a-f]+) "
        r"VALUE=0x([0-9a-f]+) CPU_ID=(\d+) RECORD_SLOT=(\d+) "
        r"WIDTH=(\d+) OPERATION=(\d+) PHASE=(\d+) VALUE_VALID=(\d+)$",
        re.MULTILINE)
    rows = []
    for match in row_pattern.finditer(text):
        values = [int(match.group(index), 16 if index in (3, 4) else 10)
                  for index in range(1, 11)]
        rows.append(dict(zip((
            "slot", "sequence", "address", "value", "cpu_id",
            "record_slot", "width", "operation", "phase",
            "value_valid"), values)))
    topology_pattern = re.compile(
        r"^TOPOLOGY_INDEX=(\d+) TOPOLOGY_SLOT=(\d+) "
        r"TOPOLOGY_CPU_ID=(\d+)$", re.MULTILINE)
    topology = [
        {"index": int(match.group(1)), "slot": int(match.group(2)),
         "cpu_id": int(match.group(3))}
        for match in topology_pattern.finditer(text)]
    expected_mask = (1 << smp) - 1
    require(markers["OBSERVER_READY"] == 1 and
            markers["TARGET_COUNT"] == smp and
            markers["JOINED_MASK"] == expected_mask and
            markers["ACTIVE_MASK"] == expected_mask and
            markers["TRACE_RECORD_STRIDE"] == 64 and
            markers["TOPOLOGY_STRIDE"] >= 8 and
            markers["TRACE_CELL_BASE"] != 0 and
            markers["OBSERVER_RELEASED"] == 1,
            "TRACE_RENDEZVOUS_INVALID", relative)
    require(len(rows) == smp and len(topology) == smp,
            "TRACE_ROW_COUNT_INVALID", relative)
    for slot, (row, cpu) in enumerate(zip(rows, topology)):
        require(row["slot"] == slot and row["record_slot"] == slot and
                cpu["index"] == slot and cpu["slot"] == slot,
                "TRACE_SLOT_ASSOCIATION_INVALID", f"{relative}: {slot}")
        require(row["cpu_id"] == cpu["cpu_id"],
                "TRACE_CPU_ATTRIBUTION_INVALID", f"{relative}: {slot}")
        require(row["address"] == markers["TRACE_CELL_BASE"] + slot * 8,
                "TRACE_ADDRESS_INVALID", f"{relative}: {slot}")
        require(row["value"] == ((slot + 1) << 56) | 1 and
                row["width"] == 8 and row["operation"] == 1 and
                row["phase"] == 2 and row["value_valid"] == 1 and
                row["sequence"] > 0 and row["sequence"] % 2 == 0,
                "TRACE_SNAPSHOT_MIXED", f"{relative}: {slot}")
    require(len({row["cpu_id"] for row in rows}) == smp,
            "TRACE_CPU_ATTRIBUTION_INVALID", relative)
    return {"ready": markers["OBSERVER_READY"],
            "target_count": markers["TARGET_COUNT"],
            "joined_mask": markers["JOINED_MASK"],
            "active_mask": markers["ACTIVE_MASK"], "rows": rows,
            "topology": topology, "cell_base": markers["TRACE_CELL_BASE"]}


def parse_final_raw(text: str, smp: int, relative: str) -> dict:
    names = (
        "publication_ready_mask", "sampling_release", "writers_started_mask",
        "writers_finished_mask", "concurrent_samples",
        "concurrent_unavailable", "concurrent_active_samples",
        "concurrent_progress_mask", "observer_budget_pass",
        "pin_task_started", "pin_task_done", "pin_task_pass",
        "pin_if_before", "pin_if_during", "pin_if_after", "pin_cpu_before",
        "pin_cpu_during", "pin_cpu_after", "pin_slot", "pin_snapshot_if1",
        "pin_snapshot_if0", "budget_checks")
    values = {name: unique_number(
        text, f"FINAL_{name.upper()}", 16, "FINAL_GDB_FIELD_INVALID")
              for name in names}
    row_pattern = re.compile(
        r"^CONCURRENT_SLOT=(\d+) FIRST=(\d+) LAST=(\d+)$", re.MULTILINE)
    rows = [{"slot": int(match.group(1)), "first": int(match.group(2)),
             "last": int(match.group(3))}
            for match in row_pattern.finditer(text)]
    target = (1 << smp) - 1
    require(values["publication_ready_mask"] == target and
            values["sampling_release"] == 1 and
            values["writers_started_mask"] == target and
            values["writers_finished_mask"] == target,
            "TRACE_PUBLICATION_PROGRESS_INVALID", relative)
    require(values["concurrent_samples"] >= smp and
            values["concurrent_progress_mask"] == target and
            values["observer_budget_pass"] == 1 and
            (smp == 1 or values["concurrent_active_samples"] > 0),
            "TRACE_CONCURRENT_SNAPSHOT_INVALID", relative)
    require(len(rows) == smp, "TRACE_SEQUENCE_SET_INVALID", relative)
    for slot, row in enumerate(rows):
        require(row["slot"] == slot and row["first"] > 0 and
                row["last"] > 0 and row["first"] != row["last"] and
                row["first"] % 2 == 0 and row["last"] % 2 == 0,
                "TRACE_SEQUENCE_PROGRESS_INVALID", f"{relative}: {slot}")
    require(values["pin_task_started"] == 1 and
            values["pin_task_done"] == 1 and values["pin_task_pass"] == 1 and
            values["pin_if_before"] == 1 and
            values["pin_if_during"] == 0 and values["pin_if_after"] == 1 and
            values["pin_snapshot_if1"] == 1 and
            values["pin_snapshot_if0"] == 1 and
            values["budget_checks"] == 0x3f,
            "TRACE_PINNING_OBSERVATION_INVALID", relative)
    return {"values": values, "rows": rows}


def validate_launch_and_qmp(run_root: Path, result: dict, expected: tuple,
                            candidate_hash: str) -> dict:
    machine, accel, cpu, smp, negative, production = expected
    launch_path = artifact_path(run_root, result, "launch.json")
    command_path = artifact_path(run_root, result, "launch-command.txt")
    qmp_path = artifact_path(run_root, result, "qmp-events.jsonl")
    artifact_path(run_root, result, "qemu-stderr.log")
    launch = load_json(launch_path, "RUN_LAUNCH_INVALID")
    require(launch.get("machine") == machine and launch.get("accel") == accel and
            launch.get("cpu") == cpu and launch.get("smp") == smp and
            launch.get("negative") is negative and
            launch.get("production") is production and
            launch.get("elf_sha256") == candidate_hash,
            "RUN_LAUNCH_MISMATCH", str(launch_path))
    command = launch.get("command")
    require(isinstance(command, list) and all(isinstance(item, str)
                                               for item in command),
            "RUN_LAUNCH_COMMAND_INVALID", str(launch_path))
    require("-S" in command and "-display" in command and "none" in command and
            any(item.startswith("unix:") for item in command) and
            not any("tcp:" in item for item in command),
            "RUN_TRANSPORT_CONFIGURATION_INVALID", str(launch_path))
    expected_command = "\0".join(command) + "\n"
    require(command_path.read_text(errors="strict") == expected_command,
            "RUN_LAUNCH_COMMAND_MISMATCH", str(command_path))
    messages = []
    try:
        for line in qmp_path.read_text(encoding="utf-8").splitlines():
            messages.append(json.loads(line))
    except (UnicodeError, json.JSONDecodeError) as error:
        raise ValidationError("QMP_LOG_INVALID", str(error)) from error
    require(messages and "QMP" in messages[0].get("message", {}),
            "QMP_PRECONNECTION_MISSING", str(qmp_path))
    events = [item.get("message", {}).get("event") for item in messages]
    require("RESUME" in events and "SHUTDOWN" in events,
            "QMP_LIFECYCLE_INVALID", str(qmp_path))
    shutdowns = [item["message"] for item in messages
                 if item.get("message", {}).get("event") == "SHUTDOWN"]
    require(any(item.get("data", {}).get("reason") == "host-qmp-quit"
                for item in shutdowns), "QMP_CLEANUP_EVENT_INVALID",
            str(qmp_path))
    if not production:
        require("STOP" in events, "GDB_OBSERVATION_INTERVAL_MISSING",
                str(qmp_path))
    return launch


def validate_run(evidence: Path, relative: str, expected: tuple,
                 candidate_hash: str) -> dict:
    result_path = checked(evidence, relative, "RUN_RESULT_PATH_INVALID")
    result = load_json(result_path, "RUN_RESULT_INVALID")
    run_root = result_path.parent
    machine, accel, cpu, smp, negative, production = expected
    profile = result.get("profile", {})
    require(result.get("status") == "PASS" and
            result.get("host_return_code") == 0,
            "RUN_STATUS_INVALID", relative)
    require(profile.get("machine") == machine and
            profile.get("accel") == accel and profile.get("cpu") == cpu and
            profile.get("smp") == smp,
            "RUN_PROFILE_MISMATCH", relative)
    require(result.get("negative") is negative and
            result.get("production") is production,
            "RUN_MODE_MISMATCH", relative)
    require(result.get("elf_sha256") == candidate_hash,
            "RUN_CANDIDATE_MISMATCH", relative)
    require(result.get("qmp_connected_before_execution") is True,
            "QMP_PRECONNECTION_MISSING", relative)
    cleanup = result.get("cleanup", {})
    require(cleanup.get("method") == "qmp-quit" and
            result.get("qemu_process_return_code") == 0,
            "QEMU_CLEANUP_INVALID", relative)
    serial_path = artifact_path(run_root, result, "serial-transcript.log")
    serial = serial_path.read_text(errors="replace")
    validate_launch_and_qmp(run_root, result, expected, candidate_hash)

    if production:
        require(result.get("production_trace_elided") is True,
                "PRODUCTION_TRACE_ACTIVE", relative)
        require("[BOOT][RUNTIME_READY] PASS" in serial,
                "PRODUCTION_RUNTIME_NOT_READY", relative)
        require("[ARCH_TEST]" not in serial,
                "PRODUCTION_TEST_MARKER_PRESENT", relative)
        return {"profile": profile, "production": True}

    early = one_marker(serial, "[ARCH_TEST][EARLY]", EARLY_FIELDS)
    budget = one_marker(serial, "[ARCH_TEST][BUDGET]", BUDGET_FIELDS)
    trace = one_marker(serial, "[ARCH_TEST][TRACE_ACTIVE]", TRACE_FIELDS)
    runtime = one_marker(serial, "[ARCH_TEST][RUNTIME]", RUNTIME_FIELDS)
    cpuid_records, raw_cpu_id = validate_cpuid_records(serial, relative)
    msr_records = validate_msr_records(serial, accel, raw_cpu_id, relative)
    require(early["status"] == "PASS" and early["cpuid_checks"] == "63" and
            early["write_gp"] == "1" and early["if_preserved"] == "1",
            "EARLY_CONTRACT_INVALID", relative)
    require(budget == {"status": "PASS", "checks": "63", "expected": "63"},
            "OBSERVER_BUDGET_EXHAUSTION_INVALID", relative)
    require(trace["status"] == "READY" and int(trace["cpus"]) == smp and
            int(trace["active"]) == (1 << smp) - 1 and
            int(trace["target"]) == (1 << smp) - 1,
            "TRACE_RENDEZVOUS_INVALID", relative)
    require(runtime["status"] == "PASS" and int(runtime["cpus"]) == smp and
            int(runtime["joined"]) == (1 << smp) - 1 and
            int(runtime["complete"]) == (1 << smp) - 1 and
            int(runtime["snapshots"]) == (1 << smp) - 1 and
            int(runtime["progress"]) == (1 << smp) - 1 and
            int(runtime["budget_ok"]) == 1 and
            int(runtime["concurrent_samples"]) >= smp,
            "RUNTIME_CONTRACT_INVALID", relative)
    if smp > 1:
        require(int(runtime["overlap"]) != 0 and
                int(runtime["concurrent_active"]) > 0,
                "TRACE_CONCURRENCY_NOT_OBSERVED", relative)

    early_log = artifact_path(run_root, result, "gdb-early.log")
    artifact_path(run_root, result, "gdb-early.cmd")
    early_text = early_log.read_text(errors="replace")
    events = parse_gp_events(early_text)
    require([event["ordinal"] for event in events] == list(range(len(events))) and
            all(event["error"] == 0 and event["cs"] == 8 for event in events),
            "MSR_GP_FRAME_INVALID", relative)
    static = validate_run.static_result
    read_site = static["symbols"]["cpu_read_msr_safe_site"]
    read_fixup = static["symbols"]["cpu_read_msr_safe_fixup"]
    write_site = static["symbols"]["cpu_write_msr_safe_site"]
    write_fixup = static["symbols"]["cpu_write_msr_safe_fixup"]
    read_probe = static["fixup_probes"]["read"]
    write_probe = static["fixup_probes"]["write"]
    continuation = static["symbols"]["arch_selftest_gdb_done"]
    raw_early = {
        "gp_count": unique_number(early_text, "GP_COUNT"),
        "early_done": unique_number(early_text, "EARLY_DONE"),
        "early_pass": unique_number(early_text, "EARLY_PASS"),
        "safe_read_failure": unique_number(early_text, "SAFE_READ_FAILURE"),
        "safe_write_failure": unique_number(early_text, "SAFE_WRITE_FAILURE"),
        "budget_checks": unique_number(early_text, "BUDGET_CHECKS", 16),
        "read_fixup_count": unique_number(early_text, "READ_FIXUP_COUNT"),
        "write_fixup_count": unique_number(early_text, "WRITE_FIXUP_COUNT"),
        "continuation_rip": unique_number(
            early_text, "EARLY_CONTINUATION_RIP", 16),
        "gp_frame_rips": [event["rip"] for event in events],
        "gp_events": events,
        "read_fixup_probe_rips": [int(value, 16) for value in re.findall(
            r"^READ_FIXUP_HIT=\d+ RIP=0x([0-9a-f]+) FIXUP=0x[0-9a-f]+ "
            r"SITE=0x[0-9a-f]+$", early_text, re.MULTILINE)],
        "read_fixup_rips": [int(value, 16) for value in re.findall(
            r"^READ_FIXUP_HIT=\d+ RIP=0x[0-9a-f]+ FIXUP=0x([0-9a-f]+) "
            r"SITE=0x[0-9a-f]+$", early_text,
            re.MULTILINE)],
        "read_fixup_sites": [int(value, 16) for value in re.findall(
            r"^READ_FIXUP_HIT=\d+ RIP=0x[0-9a-f]+ FIXUP=0x[0-9a-f]+ "
            r"SITE=0x([0-9a-f]+)$",
            early_text, re.MULTILINE)],
        "write_fixup_probe_rips": [int(value, 16) for value in re.findall(
            r"^WRITE_FIXUP_HIT=\d+ RIP=0x([0-9a-f]+) FIXUP=0x[0-9a-f]+ "
            r"SITE=0x[0-9a-f]+$", early_text, re.MULTILINE)],
        "write_fixup_rips": [int(value, 16) for value in re.findall(
            r"^WRITE_FIXUP_HIT=\d+ RIP=0x[0-9a-f]+ FIXUP=0x([0-9a-f]+) "
            r"SITE=0x[0-9a-f]+$", early_text,
            re.MULTILINE)],
        "write_fixup_sites": [int(value, 16) for value in re.findall(
            r"^WRITE_FIXUP_HIT=\d+ RIP=0x[0-9a-f]+ FIXUP=0x[0-9a-f]+ "
            r"SITE=0x([0-9a-f]+)$",
            early_text, re.MULTILINE)],
    }
    require(raw_early["gp_count"] == len(events) and
            raw_early["early_done"] == raw_early["early_pass"] == 1 and
            raw_early["safe_write_failure"] == 1 and
            raw_early["budget_checks"] == 0x3f and
            raw_early["continuation_rip"] == continuation,
            "MSR_CONTINUATION_INVALID", relative)
    if accel == "tcg":
        require(early["msr_checks"] == "29" and early["read_gp"] == "0" and
                early["rdmsr_limited"] == "1" and
                raw_early["safe_read_failure"] == 0 and
                [event["rip"] for event in events] == [write_site] and
                raw_early["read_fixup_count"] == 0 and
                raw_early["write_fixup_count"] == 1 and
                raw_early["read_fixup_probe_rips"] == [] and
                raw_early["read_fixup_rips"] == [] and
                raw_early["read_fixup_sites"] == [] and
                raw_early["write_fixup_probe_rips"] == [write_probe] and
                raw_early["write_fixup_rips"] == [write_fixup] and
                raw_early["write_fixup_sites"] == [write_site],
                "TCG_MSR_LIMIT_CLASSIFICATION_INVALID", relative)
    else:
        require(early["msr_checks"] == "31" and early["read_gp"] == "1" and
                early["rdmsr_limited"] == "0" and
                raw_early["safe_read_failure"] == 1 and
                [event["rip"] for event in events] == [read_site, write_site] and
                raw_early["read_fixup_count"] == 1 and
                raw_early["write_fixup_count"] == 1 and
                raw_early["read_fixup_probe_rips"] == [read_probe] and
                raw_early["read_fixup_rips"] == [read_fixup] and
                raw_early["read_fixup_sites"] == [read_site] and
                raw_early["write_fixup_probe_rips"] == [write_probe] and
                raw_early["write_fixup_rips"] == [write_fixup] and
                raw_early["write_fixup_sites"] == [write_site],
                "MSR_GP_CAUSAL_EVIDENCE_INVALID", relative)
    derived_early = result.get("early_observer")
    require(isinstance(derived_early, dict) and all(
        derived_early.get(name) == value for name, value in raw_early.items()),
        "EARLY_OBSERVER_DERIVED_MISMATCH", relative)

    trace_log = artifact_path(run_root, result, "gdb-trace-active.log")
    artifact_path(run_root, result, "gdb-trace-active.cmd")
    raw_trace = parse_trace_raw(trace_log.read_text(errors="replace"), smp,
                                relative)
    derived_trace = result.get("trace_observer")
    require(isinstance(derived_trace, dict) and
            derived_trace.get("valid") is True and
            all(derived_trace.get(name) == raw_trace[name] for name in
                ("ready", "target_count", "joined_mask", "active_mask",
                 "rows", "topology", "cell_base")),
            "TRACE_OBSERVER_DERIVED_MISMATCH", relative)
    if negative:
        observer = result.get("negative_observer", {})
        trace_text = trace_log.read_text(errors="replace")
        unrelated_site = unique_number(trace_text, "UNRELATED_SITE", 16,
                                       "UNRELATED_GP_SITE_MISSING")
        unrelated = parse_gp_events(trace_text, "UNRELATED_GP_HIT")
        unrelated_count = unique_number(trace_text, "UNRELATED_GP_COUNT")
        kernel_observer = {
            "count": unique_number(
                trace_text, "UNRELATED_KERNEL_COUNT", code=
                "UNRELATED_GP_KERNEL_EVIDENCE_INVALID"),
            "context_valid": unique_number(
                trace_text, "UNRELATED_KERNEL_CONTEXT_VALID", code=
                "UNRELATED_GP_KERNEL_EVIDENCE_INVALID"),
            "rip": unique_number(
                trace_text, "UNRELATED_KERNEL_RIP", 16,
                "UNRELATED_GP_KERNEL_EVIDENCE_INVALID"),
            "error": unique_number(
                trace_text, "UNRELATED_KERNEL_ERROR", 16,
                "UNRELATED_GP_KERNEL_EVIDENCE_INVALID"),
            "cs": unique_number(
                trace_text, "UNRELATED_KERNEL_CS", 16,
                "UNRELATED_GP_KERNEL_EVIDENCE_INVALID"),
        }
        terminal_rip = unique_number(trace_text, "TERMINAL_RIP", 16)
        terminal_flags = unique_number(trace_text, "TERMINAL_EFLAGS", 16)
        panic_frame_rips = [int(value, 16) for value in re.findall(
            r"^\[PANIC\]\[FRAME\] available=1 rip=0x([0-9A-Fa-f]+) ",
            serial, re.MULTILINE)]
        require(len(unrelated) == unrelated_count and len(unrelated) >= 1 and
                all(event["ordinal"] == ordinal and
                    event["rip"] == unrelated_site and
                    event["error"] == 0 and event["cs"] == 8
                    for ordinal, event in enumerate(unrelated)) and
                terminal_rip != unrelated_site and (terminal_flags & 0x200) == 0,
                "UNRELATED_GP_CAUSAL_EVIDENCE_INVALID", relative)
        require(kernel_observer == {
                    "count": 1, "context_valid": 1,
                    "rip": unrelated_site, "error": 0, "cs": 8},
                "UNRELATED_GP_KERNEL_EVIDENCE_INVALID", relative)
        require(observer.get("valid") is True and
                observer.get("site") == unrelated_site and
                observer.get("observed_rips") == [
                    event["rip"] for event in unrelated] and
                observer.get("observed_events") == unrelated and
                observer.get("gdb_notification_count") == unrelated_count and
                observer.get("kernel_observer") == kernel_observer and
                observer.get("panic_frame_rips") == panic_frame_rips,
                "UNRELATED_GP_CAUSAL_EVIDENCE_INVALID", relative)
        for name in ("gdb-halt-1.log", "gdb-halt-2.log"):
            halt = artifact_path(run_root, result, name)
            require(halt.stat().st_size > 0, "UNRELATED_GP_HALT_RAW_MISSING",
                    str(halt))
        require(serial.count("[ARCH_TEST][UNRELATED_GP] action=execute") == 1 and
                serial.count("[PANIC][VECTOR] known=1 value=13") == 1 and
                panic_frame_rips == [unrelated_site] and
                serial.count("[PANIC][ACTION] requested=halt") == 1 and
                "[ARCH_TEST][UNRELATED_GP_RETURNED]" not in serial,
                "UNRELATED_GP_PANIC_INVALID", relative)
    else:
        pin = one_marker(serial, "[ARCH_TEST][PIN]", PIN_FIELDS)
        require(pin["status"] == "PASS" and pin["if_before"] == "1" and
                pin["if_during"] == "0" and pin["if_after"] == "1" and
                pin["snapshot_if1"] == "1" and pin["snapshot_if0"] == "1",
                "TRACE_PINNING_MARKER_INVALID", relative)
        final_log = artifact_path(run_root, result, "gdb-final.log")
        artifact_path(run_root, result, "gdb-final.cmd")
        raw_final = parse_final_raw(final_log.read_text(errors="replace"), smp,
                                    relative)
        derived_final = result.get("final_observer")
        require(isinstance(derived_final, dict) and
                derived_final.get("valid") is True and
                derived_final.get("values") == raw_final["values"] and
                derived_final.get("rows") == raw_final["rows"],
                "FINAL_OBSERVER_DERIVED_MISMATCH", relative)
        values = raw_final["values"]
        require(integer(pin["cpu_during"], "TRACE_PINNING_MARKER_INVALID") ==
                    values["pin_cpu_during"] and
                integer(pin["slot"], "TRACE_PINNING_MARKER_INVALID") ==
                    values["pin_slot"] and
                int(runtime["concurrent_samples"]) ==
                    values["concurrent_samples"] and
                int(runtime["concurrent_unavailable"]) ==
                    values["concurrent_unavailable"] and
                int(runtime["concurrent_active"]) ==
                    values["concurrent_active_samples"] and
                int(runtime["progress"]) ==
                    values["concurrent_progress_mask"],
                "SERIAL_GDB_RECONCILIATION_INVALID", relative)
    return {"profile": profile, "negative": negative,
            "trace_rows": len(raw_trace["rows"]), "gp_hits": len(events),
            "cpuid_records": len(cpuid_records),
            "msr_records": len(msr_records)}


def validate_host(evidence: Path, record: dict) -> dict:
    log = require_file(evidence, record, "HOST_LOG")
    text = log.read_text(errors="replace")
    require(record.get("exit_code") == 0, "HOST_STATUS_INVALID",
            record.get("name", ""))
    match = re.search(
        r"^\[ARCH_HOST\] status=PASS checks=(\d+) debug_trace=([01])$",
        text, re.MULTILINE)
    require(match is not None, "HOST_MARKER_INVALID", str(log))
    expected_trace = 1 if record.get("debug_trace") else 0
    require(int(match.group(2)) == expected_trace,
            "HOST_PROFILE_MISMATCH", str(log))
    minimum = 30 if expected_trace else 15
    require(int(match.group(1)) >= minimum,
            "HOST_CHECK_COUNT_INVALID", str(log))
    return {"name": record.get("name"), "checks": int(match.group(1))}


def validate_static(evidence: Path, record_path: str) -> dict:
    path = checked(evidence, record_path, "STATIC_RESULT_PATH_INVALID")
    result = load_json(path, "STATIC_RESULT_INVALID")
    require(result.get("status") == "PASS", "STATIC_STATUS_INVALID")
    required = (
        "mmio_ordered_probe", "mmio_relaxed_probe",
        "trace_debug_symbols", "trace_production_elision",
        "msr_fixup_table", "cpuid_subleaf_instruction",
        "port_header_compatibility", "port_external_symbols_removed",
        "driver_fences_preserved", "undefined_helpers_absent",
        "section_layout_debug", "section_layout_production",
    )
    checks = result.get("checks")
    require(isinstance(checks, dict), "STATIC_CHECK_SET_INVALID")
    for name in required:
        require(checks.get(name) is True, "STATIC_CHECK_FAILED", name)
    artifacts = result.get("artifacts")
    require(isinstance(artifacts, list) and artifacts,
            "STATIC_ARTIFACT_SET_INVALID")
    raw = {}
    for artifact in artifacts:
        path = require_file(evidence, artifact, "STATIC_ARTIFACT")
        require(path.name not in raw, "STATIC_ARTIFACT_DUPLICATE", path.name)
        raw[path.name] = path
    required_raw = {
        "debug-nm.txt", "production-nm.txt", "production-global-nm.txt",
        "host-disassembly.txt", "debug-disassembly.txt",
        "debug-sections.txt", "production-strings.txt",
        "debug-mmio-undefined.txt", "section-layout-debug.json",
        "section-layout-production.json", "port-include-order.log",
    }
    require(required_raw.issubset(raw), "STATIC_RAW_SET_INCOMPLETE",
            ",".join(sorted(required_raw - raw.keys())))
    symbols = result.get("symbols")
    require(isinstance(symbols, dict), "STATIC_SYMBOLS_INVALID")
    for name in (
            "cpu_read_msr_safe_site", "cpu_read_msr_safe_fixup",
            "cpu_write_msr_safe_site", "cpu_write_msr_safe_fixup"):
        require(isinstance(symbols.get(name), int) and symbols[name] != 0,
                "STATIC_SYMBOLS_INVALID", name)

    debug_nm = raw["debug-nm.txt"].read_text(errors="replace")
    production_nm = raw["production-nm.txt"].read_text(errors="replace")
    debug_symbols = nm_symbols(debug_nm)
    production_symbols = nm_symbols(production_nm)
    require(all(debug_symbols.get(name) == value
                for name, value in symbols.items()),
            "STATIC_SYMBOL_DERIVATION_MISMATCH")
    debug_disassembly = raw["debug-disassembly.txt"].read_text(
        errors="replace")
    probes = {
        "read": fixup_probe_address(
            debug_disassembly, "cpu_read_msr_safe_fixup",
            debug_symbols["cpu_read_msr_safe_fixup"]),
        "write": fixup_probe_address(
            debug_disassembly, "cpu_write_msr_safe_fixup",
            debug_symbols["cpu_write_msr_safe_fixup"]),
    }
    require(result.get("fixup_probes") == probes,
            "MSR_FIXUP_PROBE_DERIVATION_MISMATCH")

    host_disassembly = raw["host-disassembly.txt"].read_text(errors="replace")
    ordered_read = disassembly_function(
        host_disassembly, "arch_probe_ordered_read64")
    relaxed_read = disassembly_function(
        host_disassembly, "arch_probe_relaxed_read64")
    ordered_write = disassembly_function(
        host_disassembly, "arch_probe_ordered_write64")
    relaxed_write = disassembly_function(
        host_disassembly, "arch_probe_relaxed_write64")
    ordered_ok = (ordered_read.count("mfence") == 2 and
                  ordered_write.count("mfence") == 2 and
                  len(re.findall(r"mov\s+\(%rdi\),%rax", ordered_read)) == 1 and
                  len(re.findall(r"mov\s+%rsi,\(%rdi\)", ordered_write)) == 1)
    relaxed_ok = ("mfence" not in relaxed_read and
                  "mfence" not in relaxed_write and
                  len(re.findall(r"mov\s+\(%rdi\),%rax", relaxed_read)) == 1 and
                  len(re.findall(r"mov\s+%rsi,\(%rdi\)", relaxed_write)) == 1)
    debug_trace_names = {
        "g_mmio_trace_records", "g_mmio_trace_slot_hints",
        "mmio_trace_begin", "mmio_trace_complete",
        "mmio_trace_snapshot_current",
    }
    production_strings = raw["production-strings.txt"].read_text(
        errors="replace")
    production_global = raw["production-global-nm.txt"].read_text(
        errors="replace")
    global_port_names = re.findall(
        r"^[0-9a-fA-F]+\s+[A-Z]\s+(inb|outb|io_wait)$",
        production_global, re.MULTILINE)
    cpuid_function = disassembly_function(debug_disassembly, "cpu_cpuid_raw")
    debug_sections = raw["debug-sections.txt"].read_text(errors="replace")
    debug_layout = load_json(raw["section-layout-debug.json"],
                             "STATIC_LAYOUT_RAW_INVALID")
    production_layout = load_json(raw["section-layout-production.json"],
                                  "STATIC_LAYOUT_RAW_INVALID")
    for role, layout in (("debug", debug_layout),
                         ("production", production_layout)):
        table = layout.get("msr_fixup", {})
        require(table.get("entry_count") == 2 and table.get("size") == 32 and
                len(table.get("entries", [])) == 2,
                "STATIC_LAYOUT_RAW_INVALID", role)
    xhci = evidence / "source/kernel/src/drivers/usb/xhci/xhci.c"
    require(xhci.is_file(), "STATIC_SOURCE_MISSING", str(xhci))
    recomputed = {
        "mmio_ordered_probe": ordered_ok,
        "mmio_relaxed_probe": relaxed_ok,
        "trace_debug_symbols": debug_trace_names.issubset(debug_symbols),
        "trace_production_elision": (
            not (debug_trace_names & production_symbols.keys()) and
            "[ARCH_TEST]" not in production_strings and
            "trace=per-cpu" not in production_strings),
        "msr_fixup_table": (
            ".cpu_msr_fixup" in debug_sections and
            debug_symbols["_cpu_msr_fixup_end"] -
            debug_symbols["_cpu_msr_fixup_start"] == 32),
        "cpuid_subleaf_instruction": disassembly_has_instruction(
            cpuid_function, "cpuid"),
        "port_header_compatibility": (
            "INCLUDE_ORDER_STATUS=PASS" in
            raw["port-include-order.log"].read_text(errors="replace")),
        "port_external_symbols_removed": not global_port_names,
        "driver_fences_preserved": xhci_fences_match_contract(
            xhci.read_bytes()),
        "undefined_helpers_absent": (
            "__atomic_" not in
            raw["debug-mmio-undefined.txt"].read_text(errors="replace")),
        "section_layout_debug": True,
        "section_layout_production": True,
    }
    require(checks == recomputed and all(recomputed.values()),
            "STATIC_RAW_DERIVED_MISMATCH",
            ",".join(name for name, value in recomputed.items() if not value))
    result["raw_layout_hashes"] = {
        "debug": debug_layout.get("elf_sha256"),
        "production": production_layout.get("elf_sha256"),
    }
    return result


def validate_sources(evidence: Path, path_value: str) -> int:
    manifest_path = checked(evidence, path_value,
                            "SOURCE_MANIFEST_PATH_INVALID")
    manifest = load_json(manifest_path, "SOURCE_MANIFEST_INVALID")
    files = manifest.get("files")
    require(isinstance(files, list) and files,
            "SOURCE_MANIFEST_INVALID", "empty")
    seen = set()
    for record in files:
        path = require_file(evidence, record, "SOURCE")
        require(record["path"] not in seen, "SOURCE_DUPLICATE",
                record["path"])
        seen.add(record["path"])
        require(path.is_file(), "SOURCE_TYPE_INVALID", str(path))
    return len(files)


def validate_contract_summary(summary: dict) -> None:
    requirements = (
        ("cpuid_subleaf", "CPUID_SUBLEAF_INVALID"),
        ("cpuid_bounds", "CPUID_BOUNDS_INVALID"),
        ("legacy_ecx_zero", "CPUID_LEGACY_ECX_INVALID"),
        ("msr_instruction_observed", "MSR_INSTRUCTION_NOT_OBSERVED"),
        ("msr_unrelated_gp_panics", "UNRELATED_GP_RECOVERED"),
        ("msr_table_unique", "MSR_TABLE_SITE_DUPLICATE"),
        ("msr_fixups_in_text", "MSR_FIXUP_OUTSIDE_TEXT"),
        ("trace_coherent", "TRACE_SNAPSHOT_MIXED"),
        ("trace_bounded", "TRACE_WAIT_UNBOUNDED"),
        ("production_trace_elided", "PRODUCTION_TRACE_ACTIVE"),
        ("records_complete", "ARCH_MARKER_FRAGMENTED"),
        ("recovery_continues", "MSR_CONTINUATION_MISSING"),
        ("port_headers_compatible", "PORT_HEADER_CONFLICT"),
        ("unexpected_orphan_rejected", "UNEXPECTED_ORPHAN_ACCEPTED"),
    )
    for field, code in requirements:
        require(summary.get(field) is True, code, field)


def validate_all(evidence: Path) -> dict:
    campaign = load_json(evidence / "campaign.json", "CAMPAIGN_INVALID")
    require(campaign.get("schema") == 1, "CAMPAIGN_SCHEMA_INVALID")
    source_count = validate_sources(evidence, campaign.get("source_manifest"))
    static_result = validate_static(evidence, campaign.get("static_result"))
    validate_run.static_result = static_result

    candidates = campaign.get("candidates")
    require(isinstance(candidates, dict), "CANDIDATE_SET_INVALID")
    candidate_hashes = {}
    for role in ("instrumented", "negative", "production"):
        record = candidates.get(role)
        require(isinstance(record, dict), "CANDIDATE_MISSING", role)
        elf = require_file(evidence, record["elf"], "CANDIDATE_ELF")
        image = require_file(evidence, record["image"], "CANDIDATE_IMAGE")
        candidate_hashes[role] = sha256(elf)
        require(record.get("elf_sha256") == candidate_hashes[role] and
                record.get("image_sha256") == sha256(image),
                "CANDIDATE_IDENTITY_MISMATCH", role)
    require(static_result["raw_layout_hashes"] == {
        "debug": candidate_hashes["instrumented"],
        "production": candidate_hashes["production"],
    }, "STATIC_LAYOUT_CANDIDATE_MISMATCH")

    host_records = campaign.get("host_runs")
    require(isinstance(host_records, list) and len(host_records) == 4,
            "HOST_MATRIX_INCOMPLETE")
    host_results = [validate_host(evidence, record)
                    for record in host_records]
    require({record.get("name") for record in host_records} == {
        "normal-debug", "ubsan-debug", "asan-debug", "normal-disabled"},
        "HOST_MATRIX_INCOMPLETE")

    expected_profiles = {
        ("q35", "tcg", "Haswell", 1, False, False),
        ("q35", "tcg", "Haswell", 4, False, False),
        ("q35", "kvm", "host", 4, False, False),
        ("q35", "kvm", "host", 24, False, False),
    }
    run_records = campaign.get("runtime_runs")
    require(isinstance(run_records, list) and len(run_records) == 4,
            "QEMU_MATRIX_INCOMPLETE")
    observed = set()
    runtime_results = []
    for record in run_records:
        profile = record.get("profile")
        require(isinstance(profile, list) and len(profile) == 4,
                "RUN_PROFILE_MISMATCH")
        expected = (profile[0], profile[1], profile[2], profile[3],
                    False, False)
        observed.add(expected)
        runtime_results.append(validate_run(
            evidence, record.get("result"), expected,
            candidate_hashes["instrumented"]))
    require(observed == expected_profiles, "QEMU_MATRIX_INCOMPLETE",
            str(sorted(observed)))

    negative = validate_run(
        evidence, campaign.get("negative_run"),
        ("q35", "tcg", "Haswell", 1, True, False),
        candidate_hashes["negative"])
    production = validate_run(
        evidence, campaign.get("production_run"),
        ("q35", "kvm", "host", 4, False, True),
        candidate_hashes["production"])

    summary = campaign.get("contract_summary")
    require(isinstance(summary, dict), "CONTRACT_SUMMARY_INVALID")
    validate_contract_summary(summary)
    result = {
        "schema": 1, "status": "PASS",
        "source_files": source_count,
        "host_runs": host_results,
        "runtime_runs": runtime_results,
        "negative_run": negative,
        "production_run": production,
        "tcg_rdmsr_limitation": (
            "QEMU 8.2.2 TCG returns zero for unimplemented RDMSR; "
            "KVM profiles provide causal read recovery evidence"),
    }
    return result


def capture(argv: list[str], cwd: Path, path: Path) -> tuple[int, str]:
    completed = subprocess.run(argv, cwd=cwd, text=True,
                               stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(completed.stdout)
    return completed.returncode, completed.stdout


def artifact_record(evidence: Path, path: Path) -> dict:
    return {"path": str(path.resolve().relative_to(evidence.resolve())),
            "bytes": path.stat().st_size, "sha256": sha256(path)}


def nm_symbols(text: str) -> dict[str, int]:
    result = {}
    for line in text.splitlines():
        fields = line.split()
        if len(fields) >= 3 and re.fullmatch(r"[0-9a-fA-F]+", fields[0]):
            result[fields[-1]] = int(fields[0], 16)
    return result


def disassembly_function(text: str, name: str) -> str:
    match = re.search(
        rf"^[0-9a-f]+ <{re.escape(name)}>:\n(.*?)(?=\n\n|\Z)",
        text, re.MULTILINE | re.DOTALL)
    require(match is not None, "PROBE_DISASSEMBLY_MISSING", name)
    return match.group(1)


def disassembly_has_instruction(text: str, mnemonic: str) -> bool:
    return re.search(
        rf"^\s*[0-9a-f]+:\s+(?:[0-9a-f]{{2}}\s+)+"
        rf"{re.escape(mnemonic)}(?:\s|$)", text, re.MULTILINE) is not None


def fixup_probe_address(text: str, name: str, expected: int) -> int:
    body = disassembly_function(text, name)
    addresses = [int(value, 16) for value in re.findall(
        r"^\s*([0-9a-f]+):\s+(?:[0-9a-f]{2}\s+)+", body,
        re.MULTILINE)]
    require(len(addresses) >= 2 and addresses[0] == expected,
            "MSR_FIXUP_PROBE_INVALID", name)
    return addresses[1]


def collect_static(args) -> dict:
    root = Path(args.repo_root).resolve()
    evidence = Path(args.evidence_root).resolve()
    static_dir = evidence / "static"
    static_dir.mkdir(parents=True, exist_ok=True)
    debug_elf = Path(args.debug_elf).resolve()
    production_elf = Path(args.production_elf).resolve()
    host_disabled = Path(args.host_disabled).resolve()
    debug_mmio = Path(args.debug_mmio_object).resolve()
    for path in (debug_elf, production_elf, host_disabled, debug_mmio):
        require(path.is_file(), "STATIC_INPUT_MISSING", str(path))

    commands = [
        ("debug-nm.txt", ["nm", "-n", "-S", str(debug_elf)]),
        ("production-nm.txt", ["nm", "-n", "-S", str(production_elf)]),
        ("production-global-nm.txt", ["nm", "-g", str(production_elf)]),
        ("host-disassembly.txt", ["objdump", "-dr", str(host_disabled)]),
        ("debug-disassembly.txt", ["objdump", "-dr", str(debug_elf)]),
        ("debug-sections.txt", ["readelf", "-SW", str(debug_elf)]),
        ("debug-segments.txt", ["readelf", "-lW", str(debug_elf)]),
        ("production-sections.txt", ["readelf", "-SW", str(production_elf)]),
        ("production-segments.txt", ["readelf", "-lW", str(production_elf)]),
        ("production-strings.txt", ["strings", "-a", str(production_elf)]),
        ("debug-mmio-undefined.txt", ["nm", "-u", str(debug_mmio)]),
    ]
    outputs = {}
    artifacts = []
    for name, argv in commands:
        path = static_dir / name
        status, output = capture(argv, root, path)
        require(status == 0, "STATIC_COMMAND_FAILED", " ".join(argv))
        outputs[name] = output
        artifacts.append(artifact_record(evidence, path))

    section_script = root / "scripts/verify-section-layout.py"
    for role, elf in (("debug", debug_elf),
                      ("production", production_elf)):
        path = static_dir / f"section-layout-{role}.json"
        status, output = capture(
            [sys.executable, str(section_script), "elf", str(elf)], root,
            path)
        require(status == 0, "SECTION_LAYOUT_INVALID", role)
        outputs[path.name] = output
        artifacts.append(artifact_record(evidence, path))

    include_log = Path(args.include_order_log).resolve()
    require(include_log.is_file(), "PORT_INCLUDE_LOG_MISSING")
    copied_include = static_dir / "port-include-order.log"
    copied_include.write_bytes(include_log.read_bytes())
    artifacts.append(artifact_record(evidence, copied_include))

    debug_symbols = nm_symbols(outputs["debug-nm.txt"])
    production_symbols = nm_symbols(outputs["production-nm.txt"])
    product_symbols = (
        "cpu_read_msr_safe", "cpu_read_msr_safe_site",
        "cpu_read_msr_safe_fixup", "cpu_write_msr_safe",
        "cpu_write_msr_safe_site", "cpu_write_msr_safe_fixup",
        "_cpu_msr_fixup_start", "_cpu_msr_fixup_end",
    )
    required_symbols = product_symbols + ("arch_selftest_gdb_done",)
    require(all(name in debug_symbols for name in required_symbols),
            "MSR_SYMBOL_SET_INVALID")
    require(all(name in production_symbols for name in product_symbols),
            "PRODUCTION_MSR_SYMBOL_SET_INVALID")

    host_disassembly = outputs["host-disassembly.txt"]
    ordered_read = disassembly_function(
        host_disassembly, "arch_probe_ordered_read64")
    relaxed_read = disassembly_function(
        host_disassembly, "arch_probe_relaxed_read64")
    ordered_write = disassembly_function(
        host_disassembly, "arch_probe_ordered_write64")
    relaxed_write = disassembly_function(
        host_disassembly, "arch_probe_relaxed_write64")
    ordered_ok = (ordered_read.count("mfence") == 2 and
                  ordered_write.count("mfence") == 2 and
                  len(re.findall(r"mov\s+\(%rdi\),%rax", ordered_read)) == 1 and
                  len(re.findall(r"mov\s+%rsi,\(%rdi\)", ordered_write)) == 1)
    relaxed_ok = ("mfence" not in relaxed_read and
                  "mfence" not in relaxed_write and
                  len(re.findall(r"mov\s+\(%rdi\),%rax", relaxed_read)) == 1 and
                  len(re.findall(r"mov\s+%rsi,\(%rdi\)", relaxed_write)) == 1)

    debug_trace_names = {
        "g_mmio_trace_records", "g_mmio_trace_slot_hints",
        "mmio_trace_begin", "mmio_trace_complete",
        "mmio_trace_snapshot_current",
    }
    trace_debug = debug_trace_names.issubset(debug_symbols)
    trace_elided = (not (debug_trace_names & production_symbols.keys()) and
                    "[ARCH_TEST]" not in outputs["production-strings.txt"] and
                    "trace=per-cpu" not in outputs["production-strings.txt"])
    global_port_names = re.findall(
        r"^[0-9a-fA-F]+\s+[A-Z]\s+(inb|outb|io_wait)$",
        outputs["production-global-nm.txt"], re.MULTILINE)
    current_xhci = (root / "kernel/src/drivers/usb/xhci/xhci.c").read_bytes()
    baseline = subprocess.run(
        ["git", "show", f"{args.base_commit}:kernel/src/drivers/usb/xhci/xhci.c"],
        cwd=root, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    require(baseline.returncode == 0, "BASELINE_SOURCE_UNAVAILABLE",
            args.base_commit)
    fence_preserved = (
        xhci_fence_inventory(baseline.stdout) ==
        xhci_fence_inventory(current_xhci) and
        xhci_fences_match_contract(current_xhci)
    )
    debug_disassembly = outputs["debug-disassembly.txt"]
    fixup_probes = {
        "read": fixup_probe_address(
            debug_disassembly, "cpu_read_msr_safe_fixup",
            debug_symbols["cpu_read_msr_safe_fixup"]),
        "write": fixup_probe_address(
            debug_disassembly, "cpu_write_msr_safe_fixup",
            debug_symbols["cpu_write_msr_safe_fixup"]),
    }
    cpuid_function = disassembly_function(debug_disassembly,
                                          "cpu_cpuid_raw")
    msr_table_layout = (
        ".cpu_msr_fixup" in outputs["debug-sections.txt"] and
        debug_symbols["_cpu_msr_fixup_end"] -
        debug_symbols["_cpu_msr_fixup_start"] == 32)
    checks = {
        "mmio_ordered_probe": ordered_ok,
        "mmio_relaxed_probe": relaxed_ok,
        "trace_debug_symbols": trace_debug,
        "trace_production_elision": trace_elided,
        "msr_fixup_table": msr_table_layout,
        "cpuid_subleaf_instruction": disassembly_has_instruction(
            cpuid_function, "cpuid"),
        "port_header_compatibility": (
            "INCLUDE_ORDER_STATUS=PASS" in copied_include.read_text()),
        "port_external_symbols_removed": not global_port_names,
        "driver_fences_preserved": fence_preserved,
        "undefined_helpers_absent": (
            "__atomic_" not in outputs["debug-mmio-undefined.txt"]),
        "section_layout_debug": True,
        "section_layout_production": True,
    }
    result = {
        "schema": 1,
        "status": "PASS" if all(checks.values()) else "FAIL",
        "checks": checks,
        "symbols": {name: debug_symbols[name] for name in required_symbols},
        "fixup_probes": fixup_probes,
        "artifacts": artifacts,
    }
    result_path = static_dir / "result.json"
    result_path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    require(result["status"] == "PASS", "STATIC_COLLECTION_FAILED",
            ",".join(name for name, value in checks.items() if not value))
    return result


def expect_error(identifier: str, expected: str, operation,
                 output: Path) -> dict:
    case = output / identifier
    case.mkdir()
    try:
        operation()
    except ValidationError as error:
        status = "NEGATIVE_DETECTED" if error.code == expected else "FAIL"
        record = {"id": identifier, "status": status,
                  "expected_code": expected, "observed_code": error.code,
                  "detail": error.detail}
        (case / "result.json").write_text(
            json.dumps(record, indent=2, sort_keys=True) + "\n")
        require(status == "NEGATIVE_DETECTED", "FIXTURE_WRONG_REASON",
                identifier)
        return record
    raise ValidationError("FIXTURE_ACCEPTED_INVALID", identifier)


def run_fixtures(output: Path, source: Path) -> dict:
    require(not output.exists(), "FIXTURE_OUTPUT_EXISTS", str(output))
    output.mkdir(parents=True)
    valid = {
        "cpuid_subleaf": True, "cpuid_bounds": True,
        "legacy_ecx_zero": True, "msr_instruction_observed": True,
        "msr_unrelated_gp_panics": True, "msr_table_unique": True,
        "msr_fixups_in_text": True, "trace_coherent": True,
        "trace_bounded": True, "production_trace_elided": True,
        "records_complete": True, "recovery_continues": True,
        "port_headers_compatible": True,
        "unexpected_orphan_rejected": True,
    }
    validate_contract_summary(valid)
    records = [{"id": "valid", "status": "PASS"}]
    cases = [
        ("subleaf-ignored", "cpuid_subleaf", "CPUID_SUBLEAF_INVALID"),
        ("leaf-above-maximum", "cpuid_bounds", "CPUID_BOUNDS_INVALID"),
        ("legacy-ecx-residual", "legacy_ecx_zero", "CPUID_LEGACY_ECX_INVALID"),
        ("msr-no-instruction", "msr_instruction_observed", "MSR_INSTRUCTION_NOT_OBSERVED"),
        ("unrelated-gp-recovered", "msr_unrelated_gp_panics", "UNRELATED_GP_RECOVERED"),
        ("msr-site-duplicate", "msr_table_unique", "MSR_TABLE_SITE_DUPLICATE"),
        ("msr-fixup-outside-text", "msr_fixups_in_text", "MSR_FIXUP_OUTSIDE_TEXT"),
        ("trace-fields-mixed", "trace_coherent", "TRACE_SNAPSHOT_MIXED"),
        ("trace-wait-unbounded", "trace_bounded", "TRACE_WAIT_UNBOUNDED"),
        ("production-trace-active", "production_trace_elided", "PRODUCTION_TRACE_ACTIVE"),
        ("marker-fragmented", "records_complete", "ARCH_MARKER_FRAGMENTED"),
        ("recovery-no-continuation", "recovery_continues", "MSR_CONTINUATION_MISSING"),
        ("port-include-conflict", "port_headers_compatible", "PORT_HEADER_CONFLICT"),
        ("orphan-accepted", "unexpected_orphan_rejected", "UNEXPECTED_ORPHAN_ACCEPTED"),
    ]
    for identifier, field, code in cases:
        invalid = copy.deepcopy(valid)
        invalid[field] = False
        records.append(expect_error(
            identifier, code,
            lambda value=invalid: validate_contract_summary(value), output))

    source = source.resolve()
    require((source / "campaign.json").is_file(),
            "FIXTURE_SOURCE_INVALID", str(source))
    campaign = load_json(source / "campaign.json", "FIXTURE_SOURCE_INVALID")
    static_result = validate_static(source, campaign.get("static_result"))
    validate_run.static_result = static_result
    instrumented_hash = campaign["candidates"]["instrumented"]["elf_sha256"]
    negative_hash = campaign["candidates"]["negative"]["elf_sha256"]
    production_hash = campaign["candidates"]["production"]["elf_sha256"]
    positive_record = next(
        item for item in campaign["runtime_runs"]
        if item["profile"] == ["q35", "kvm", "host", 4])
    positive_relative = positive_record["result"]
    positive_expected = ("q35", "kvm", "host", 4, False, False)
    negative_relative = campaign["negative_run"]
    negative_expected = ("q35", "tcg", "Haswell", 1, True, False)
    production_relative = campaign["production_run"]
    production_expected = ("q35", "kvm", "host", 4, False, True)

    validate_run(source, positive_relative, positive_expected,
                 instrumented_hash)
    validate_run(source, negative_relative, negative_expected, negative_hash)
    validate_run(source, production_relative, production_expected,
                 production_hash)
    validate_all(source)
    records.extend([
        {"id": "valid-run-control", "status": "PASS"},
        {"id": "valid-negative-run-control", "status": "PASS"},
        {"id": "valid-aggregate-control", "status": "PASS"},
    ])

    excluded = set()
    try:
        excluded.add(output.resolve().relative_to(source).parts[0])
    except (ValueError, IndexError):
        pass

    def link_or_copy(src, dst):
        try:
            os.link(src, dst)
        except OSError:
            shutil.copy2(src, dst)
        return dst

    def clone_source(destination: Path) -> None:
        def ignored(_directory, names):
            return [name for name in names if name in excluded]
        shutil.copytree(source, destination, copy_function=link_or_copy,
                        ignore=ignored)

    def detached_write(path: Path, data: bytes) -> None:
        temporary = path.with_name(path.name + ".fixture-new")
        temporary.write_bytes(data)
        os.replace(temporary, path)

    def detached_json(path: Path, value: dict) -> None:
        detached_write(path, (json.dumps(
            value, indent=2, sort_keys=True) + "\n").encode())

    def modify_result(root: Path, relative: str, operation) -> list[str]:
        path = root / relative
        value = load_json(path)
        operation(value)
        detached_json(path, value)
        return [relative]

    def modify_artifact(root: Path, relative: str, name: str,
                        operation, update_hash: bool = True) -> list[str]:
        result_path = root / relative
        result = load_json(result_path)
        path = result_path.parent / name
        original = path.read_bytes()
        changed = operation(original)
        require(isinstance(changed, bytes) and changed != original,
                "FIXTURE_MUTATION_INEFFECTIVE", name)
        detached_write(path, changed)
        if update_hash:
            result["artifacts"][name] = {
                "bytes": path.stat().st_size, "sha256": sha256(path)}
            detached_json(result_path, result)
        return [str(path.relative_to(root)), relative]

    def remove_artifact(root: Path, relative: str, name: str) -> list[str]:
        path = (root / relative).parent / name
        path.unlink()
        return [str(path.relative_to(root)), relative]

    def modify_static_artifact(root: Path, name: str,
                               operation) -> list[str]:
        result_path = root / campaign["static_result"]
        result = load_json(result_path)
        record = next(item for item in result["artifacts"]
                      if Path(item["path"]).name == name)
        path = root / record["path"]
        original = path.read_bytes()
        changed = operation(original)
        require(isinstance(changed, bytes) and changed != original,
                "FIXTURE_MUTATION_INEFFECTIVE", name)
        detached_write(path, changed)
        record["bytes"] = path.stat().st_size
        record["sha256"] = sha256(path)
        detached_json(result_path, result)
        return [str(path.relative_to(root)), campaign["static_result"]]

    def update_text(pattern: bytes, replacement: bytes):
        def operation(value: bytes) -> bytes:
            require(value.count(pattern) == 1,
                    "FIXTURE_PATTERN_INVALID", pattern.decode(errors="replace"))
            return value.replace(pattern, replacement, 1)
        return operation

    def mutate_cache_subleaf(value: bytes) -> bytes:
        text = value.decode(errors="strict")
        lines = text.splitlines(keepends=True)
        cache = [index for index, line in enumerate(lines)
                 if line.startswith("[ARCH_TEST][CPUID] case=cache")]
        require(len(cache) == 2, "FIXTURE_PATTERN_INVALID", "cache records")
        first = dict(token.split("=", 1) for token in lines[cache[0]].split()[1:])
        second = lines[cache[1]]
        for prefix in ("api_", "raw_"):
            for register in ("eax", "ebx", "ecx", "edx"):
                name = prefix + register
                second = re.sub(rf"{name}=\S+", f"{name}={first[name]}",
                                second, count=1)
        lines[cache[1]] = second
        return "".join(lines).encode()

    def mutate_fs_restore(value: bytes) -> bytes:
        text = value.decode(errors="strict")
        lines = text.splitlines(keepends=True)
        index = next(i for i, line in enumerate(lines)
                     if line.startswith("[ARCH_TEST][MSR] case=fs-base"))
        fields = dict(token.split("=", 1) for token in lines[index].split()[1:])
        wrong = int(fields["original"], 0) ^ 1
        lines[index] = re.sub(r"restored=\S+", f"restored=0x{wrong:x}",
                              lines[index], count=1)
        return "".join(lines).encode()

    def mutate_invalid_read(value: bytes) -> bytes:
        text = value.decode(errors="strict")
        return re.sub(
            r"(\[ARCH_TEST\]\[MSR\] case=invalid-read[^\r\n]* after=)"
            r"0x[0-9a-f]+", r"\g<1>0x9e3779b97f4a7c16", text,
            count=1).encode()

    def mutate_cpuid_numeric_missing(value: bytes) -> bytes:
        text = value.decode(errors="strict")
        return re.sub(
            r"(\[ARCH_TEST\]\[CPUID\] case=basic-maximum[^\r\n]* "
            r"api_eax=)\S+", r"\g<1>", text, count=1).encode()

    def mutate_trace_cpu(value: bytes) -> bytes:
        text = value.decode(errors="strict")
        return re.sub(r"(^TRACE_SLOT=0 .* CPU_ID=)\d+", r"\g<1>1", text,
                      count=1, flags=re.MULTILINE).encode()

    def mutate_trace_slot_duplicate(value: bytes) -> bytes:
        text = value.decode(errors="strict")
        lines = text.splitlines(keepends=True)
        index = next(i for i, line in enumerate(lines)
                     if line.startswith("TRACE_SLOT=1 "))
        lines[index] = lines[index].replace("TRACE_SLOT=1 ", "TRACE_SLOT=0 ", 1)
        lines[index] = lines[index].replace(" RECORD_SLOT=1 ",
                                            " RECORD_SLOT=0 ", 1)
        return "".join(lines).encode()

    def mutate_trace_line_count(value: bytes, duplicate: bool) -> bytes:
        text = value.decode(errors="strict")
        lines = text.splitlines(keepends=True)
        index = next(i for i, line in enumerate(lines)
                     if line.startswith("TRACE_SLOT=3 "))
        if duplicate:
            lines.insert(index + 1, lines[index])
        else:
            del lines[index]
        return "".join(lines).encode()

    def mutate_derived_addresses(result: dict) -> None:
        for row in result["trace_observer"]["rows"]:
            row["address"] = 0

    def mutate_derived_cpu(result: dict) -> None:
        rows = result["trace_observer"]["rows"]
        values = [row["cpu_id"] for row in rows]
        for index, row in enumerate(rows):
            row["cpu_id"] = values[(index + 1) % len(values)]

    per_run = []
    serial_name = "serial-transcript.log"
    trace_name = "gdb-trace-active.log"
    early_name = "gdb-early.log"
    final_name = "gdb-final.log"
    per_run.extend([
        ("derived-address-zero", "TRACE_OBSERVER_DERIVED_MISMATCH",
         lambda root: modify_result(root, positive_relative,
                                    mutate_derived_addresses), "positive"),
        ("derived-cpu-permuted", "TRACE_OBSERVER_DERIVED_MISMATCH",
         lambda root: modify_result(root, positive_relative,
                                    mutate_derived_cpu), "positive"),
        ("gdb-raw-missing", "FILE_MISSING",
         lambda root: remove_artifact(root, positive_relative, trace_name),
         "positive"),
        ("launch-raw-missing", "FILE_MISSING",
         lambda root: remove_artifact(root, positive_relative, "launch.json"),
         "positive"),
        ("qmp-raw-missing", "FILE_MISSING",
         lambda root: remove_artifact(root, positive_relative,
                                      "qmp-events.jsonl"), "positive"),
        ("gdb-derived-contradiction", "MSR_CONTINUATION_INVALID",
         lambda root: modify_artifact(
             root, positive_relative, early_name,
             update_text(b"EARLY_PASS=1", b"EARLY_PASS=0")), "positive"),
        ("trace-address-zero", "TRACE_ADDRESS_INVALID",
         lambda root: modify_artifact(
             root, positive_relative, trace_name,
             lambda value: re.sub(
                 rb"(^TRACE_SLOT=0 .* ADDRESS=)0x[0-9a-f]+",
                 rb"\g<1>0x0", value, count=1, flags=re.MULTILINE)),
         "positive"),
        ("trace-cpu-permuted", "TRACE_CPU_ATTRIBUTION_INVALID",
         lambda root: modify_artifact(root, positive_relative, trace_name,
                                      mutate_trace_cpu), "positive"),
        ("gp-site-mismatch", "MSR_GP_CAUSAL_EVIDENCE_INVALID",
         lambda root: modify_artifact(
             root, positive_relative, early_name,
             lambda value: re.sub(
                 rb"(GP_HIT=0 FRAME_RIP=)0x[0-9a-f]+",
                 (rb"\g<1>0x%x" % static_result["symbols"][
                     "cpu_write_msr_safe_site"]), value, count=1)), "positive"),
        ("gp-error-code-mismatch", "MSR_GP_FRAME_INVALID",
         lambda root: modify_artifact(
             root, positive_relative, early_name,
             update_text(b"GP_HIT=0 FRAME_RIP=0x", b"GP_HIT=0 FRAME_RIP=0x")),
         "custom-gp-error"),
        ("gp-cs-mismatch", "MSR_GP_FRAME_INVALID",
         lambda root: modify_artifact(
             root, positive_relative, early_name,
             lambda value: re.sub(rb"(GP_HIT=0[^\r\n]* CS=)0x8",
                                  rb"\g<1>0x3", value, count=1)), "positive"),
        ("fixup-target-mismatch", "MSR_GP_CAUSAL_EVIDENCE_INVALID",
         lambda root: modify_artifact(
             root, positive_relative, early_name,
             lambda value: re.sub(
                 rb"(READ_FIXUP_HIT=0 RIP=0x[0-9a-f]+ FIXUP=)0x[0-9a-f]+",
                 (rb"\g<1>0x%x" % static_result["symbols"][
                     "cpu_write_msr_safe_fixup"]), value, count=1)), "positive"),
        ("continuation-missing", "MSR_CONTINUATION_INVALID",
         lambda root: modify_artifact(
             root, positive_relative, early_name,
             lambda value: re.sub(rb"EARLY_CONTINUATION_RIP=0x[0-9a-f]+",
                                  b"EARLY_CONTINUATION_RIP=0x0", value,
                                  count=1)), "positive"),
        ("cpuid-subleaf-response-reused", "CPUID_SUBLEAF_INVALID",
         lambda root: modify_artifact(root, positive_relative, serial_name,
                                      mutate_cache_subleaf), "positive"),
        ("cpuid-numeric-output-missing", "MARKER_FIELD_INVALID",
         lambda root: modify_artifact(root, positive_relative, serial_name,
                                      mutate_cpuid_numeric_missing), "positive"),
        ("cpuid-pass-mask-contradiction", "EARLY_CONTRACT_INVALID",
         lambda root: modify_artifact(
             root, positive_relative, serial_name,
             update_text(b"cpuid_checks=63", b"cpuid_checks=62")), "positive"),
        ("fs-base-not-restored", "MSR_FS_RESTORE_INVALID",
         lambda root: modify_artifact(root, positive_relative, serial_name,
                                      mutate_fs_restore), "positive"),
        ("rejected-read-output-changed", "MSR_REJECTED_READ_OUTPUT_CHANGED",
         lambda root: modify_artifact(root, positive_relative, serial_name,
                                      mutate_invalid_read), "positive"),
        ("trace-slot-missing", "TRACE_ROW_COUNT_INVALID",
         lambda root: modify_artifact(
             root, positive_relative, trace_name,
             lambda value: mutate_trace_line_count(value, False)), "positive"),
        ("trace-slot-extra", "TRACE_ROW_COUNT_INVALID",
         lambda root: modify_artifact(
             root, positive_relative, trace_name,
             lambda value: mutate_trace_line_count(value, True)), "positive"),
        ("trace-slot-duplicate", "TRACE_SLOT_ASSOCIATION_INVALID",
         lambda root: modify_artifact(root, positive_relative, trace_name,
                                      mutate_trace_slot_duplicate), "positive"),
        ("trace-fields-mixed-raw", "TRACE_SNAPSHOT_MIXED",
         lambda root: modify_artifact(
             root, positive_relative, trace_name,
             lambda value: re.sub(rb"(^TRACE_SLOT=0 .* VALUE=)0x[0-9a-f]+",
                                  rb"\g<1>0x2", value, count=1,
                                  flags=re.MULTILINE)), "positive"),
        ("observer-timeout-approved", "TRACE_CONCURRENT_SNAPSHOT_INVALID",
         lambda root: modify_artifact(
             root, positive_relative, final_name,
             update_text(b"FINAL_OBSERVER_BUDGET_PASS=0x1",
                         b"FINAL_OBSERVER_BUDGET_PASS=0x0")), "positive"),
        ("frozen-writers-labelled-concurrent", "TRACE_SEQUENCE_PROGRESS_INVALID",
         lambda root: modify_artifact(
             root, positive_relative, final_name,
             lambda value: re.sub(
                 rb"(CONCURRENT_SLOT=0 FIRST=)(\d+)( LAST=)\d+",
                 rb"\g<1>\g<2>\g<3>\g<2>", value, count=1)), "positive"),
        ("architecture-marker-fragmented", "MARKER_PREFIX_INVALID",
         lambda root: modify_artifact(
             root, positive_relative, serial_name,
             update_text(b"[ARCH_TEST][CPUID] case=basic-maximum",
                         b"[ARCH_TEST][CPUID]\ncase=basic-maximum")), "positive"),
        ("candidate-divergent", "RUN_CANDIDATE_MISMATCH",
         lambda root: modify_result(
             root, positive_relative,
             lambda result: result.__setitem__("elf_sha256", "0" * 64)),
         "positive"),
        ("qmp-event-from-other-run", "QMP_CLEANUP_EVENT_INVALID",
         lambda root: modify_artifact(
             root, positive_relative, "qmp-events.jsonl",
             update_text(b'"reason": "host-qmp-quit"',
                         b'"reason": "guest-reset"')), "positive"),
        ("production-trace-active-raw", "PRODUCTION_TRACE_ACTIVE",
         lambda root: modify_result(
             root, production_relative,
             lambda result: result.__setitem__(
                 "production_trace_elided", False)), "production"),
        ("unrelated-kernel-count-two",
         "UNRELATED_GP_KERNEL_EVIDENCE_INVALID",
         lambda root: modify_artifact(
             root, negative_relative, trace_name,
             update_text(b"UNRELATED_KERNEL_COUNT=1",
                         b"UNRELATED_KERNEL_COUNT=2")), "negative"),
    ])

    def gp_error_mutation(root: Path) -> list[str]:
        return modify_artifact(
            root, positive_relative, early_name,
            lambda value: re.sub(rb"(GP_HIT=0[^\r\n]* ERROR=)0x0",
                                 rb"\g<1>0x1", value, count=1))

    evidence_records = []
    for identifier, expected, mutate, kind in per_run:
        if kind == "custom-gp-error":
            mutate = gp_error_mutation
            kind = "positive"
        with tempfile.TemporaryDirectory(
                prefix="hobbyos-arch-fixture-", dir=str(source.parent)) as temp:
            root = Path(temp) / "evidence"
            clone_source(root)
            touched = mutate(root)
            validate_run.static_result = static_result
            if kind == "positive":
                operation = lambda: validate_run(
                    root, positive_relative, positive_expected,
                    instrumented_hash)
            elif kind == "negative":
                operation = lambda: validate_run(
                    root, negative_relative, negative_expected,
                    negative_hash)
            else:
                operation = lambda: validate_run(
                    root, production_relative, production_expected,
                    production_hash)
            try:
                operation()
            except ValidationError as error:
                require(error.code == expected, "FIXTURE_WRONG_REASON",
                        f"{identifier}: {error.code} != {expected}")
                case = output / identifier
                case.mkdir()
                preserved = []
                for relative in touched:
                    path = root / relative
                    if path.is_file():
                        destination = case / "mutated" / relative
                        destination.parent.mkdir(parents=True, exist_ok=True)
                        shutil.copy2(path, destination)
                        preserved.append({"path": str(destination.relative_to(output)),
                                          "bytes": destination.stat().st_size,
                                          "sha256": sha256(destination)})
                    else:
                        preserved.append({"path": relative, "missing": True})
                record = {"id": identifier, "status": "NEGATIVE_DETECTED",
                          "expected_code": expected,
                          "observed_code": error.code,
                          "detail": error.detail,
                          "mutated_files": preserved}
                (case / "result.json").write_text(
                    json.dumps(record, indent=2, sort_keys=True) + "\n")
                evidence_records.append(record)
            else:
                raise ValidationError("FIXTURE_ACCEPTED_INVALID", identifier)

    aggregate_cases = [
        ("static-cpuid-opcode-removed", "STATIC_RAW_DERIVED_MISMATCH",
         lambda root: modify_static_artifact(
             root, "debug-disassembly.txt",
             lambda value: re.sub(
                 rb"(<cpu_cpuid_raw>:\n.*?\t)cpuid(\s*\n)",
                 rb"\g<1>nop\g<2>", value, count=1, flags=re.DOTALL))),
        ("static-production-trace-string", "STATIC_RAW_DERIVED_MISMATCH",
         lambda root: modify_static_artifact(
             root, "production-strings.txt",
             lambda value: value + b"[ARCH_TEST]\n")),
        ("negative-control-promoted", "RUN_PROFILE_MISMATCH",
         lambda root: modify_result(
             root, "campaign.json",
             lambda value: value.__setitem__(
                 "negative_run", positive_relative))),
    ]
    for identifier, expected, mutate in aggregate_cases:
        with tempfile.TemporaryDirectory(
                prefix="hobbyos-arch-fixture-", dir=str(source.parent)) as temp:
            root = Path(temp) / "evidence"
            clone_source(root)
            touched = mutate(root)
            try:
                validate_all(root)
            except ValidationError as error:
                require(error.code == expected, "FIXTURE_WRONG_REASON",
                        f"{identifier}: {error.code} != {expected}")
                case = output / identifier
                case.mkdir()
                preserved = []
                for relative in touched:
                    path = root / relative
                    if path.is_file():
                        destination = case / "mutated" / relative
                        destination.parent.mkdir(parents=True, exist_ok=True)
                        shutil.copy2(path, destination)
                        preserved.append({"path": str(destination.relative_to(output)),
                                          "bytes": destination.stat().st_size,
                                          "sha256": sha256(destination)})
                record = {"id": identifier, "status": "NEGATIVE_DETECTED",
                          "expected_code": expected,
                          "observed_code": error.code,
                          "detail": error.detail,
                          "mutated_files": preserved}
                (case / "result.json").write_text(
                    json.dumps(record, indent=2, sort_keys=True) + "\n")
                evidence_records.append(record)
            else:
                raise ValidationError("FIXTURE_ACCEPTED_INVALID", identifier)

    records.extend(evidence_records)
    result = {"schema": 2, "status": "PASS", "valid_controls": 4,
              "summary_negative_controls": len(cases),
              "evidence_negative_controls": len(evidence_records),
              "negative_controls": len(cases) + len(evidence_records),
              "records": records}
    (output / "results.json").write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    all_parser = sub.add_parser("all")
    all_parser.add_argument("evidence_root")
    fixtures = sub.add_parser("fixtures")
    fixtures.add_argument("--output-dir", required=True)
    fixtures.add_argument("--source-evidence", required=True)
    collect = sub.add_parser("collect-static")
    collect.add_argument("--repo-root", required=True)
    collect.add_argument("--evidence-root", required=True)
    collect.add_argument("--debug-elf", required=True)
    collect.add_argument("--production-elf", required=True)
    collect.add_argument("--host-disabled", required=True)
    collect.add_argument("--debug-mmio-object", required=True)
    collect.add_argument("--include-order-log", required=True)
    collect.add_argument("--base-commit", required=True)
    args = parser.parse_args()
    try:
        if args.command == "all":
            result = validate_all(Path(args.evidence_root).resolve())
        elif args.command == "fixtures":
            result = run_fixtures(Path(args.output_dir).resolve(),
                                  Path(args.source_evidence).resolve())
        else:
            result = collect_static(args)
        print(json.dumps(result, indent=2, sort_keys=True))
        return 0
    except ValidationError as error:
        print(f"{error.code}: {error.detail}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
