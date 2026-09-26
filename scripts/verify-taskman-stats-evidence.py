#!/usr/bin/env python3
"""Closed-by-default verifier for atomic TASKMAN statistics records."""

import argparse
import copy
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import zlib


FIELD_NAMES = (
    "sessions", "frames", "full_frames", "fallback_frames",
    "auto_exit_sessions", "auto_exit_shortfalls", "max_frame_gap_ms",
    "shell_exit_scroll_delta", "last_session_shell_exit_scroll_delta",
    "last_session_full_frames", "last_session_fallback_frames", "last_mode",
    "pages", "captured", "total", "truncated", "render_failures",
    "page_changes", "selection_changes", "ignored", "scroll_delta",
    "last_session_scroll_delta", "clipped", "builder_truncations",
    "summary_failures", "footer_failures", "region_clear_failures",
    "present_calls", "selected_pid", "rows_examined", "rows_changed",
    "rows_unchanged", "cells_examined", "cells_changed", "cells_unchanged",
    "glyph_changes", "style_changes", "first_frame_presents",
    "cleanup_clears", "geometry_clears", "stable_frame_full_clears",
    "tail_rows_cleared", "separator_mismatches", "field_overflows",
    "field_truncations", "split_overlaps", "workspace_live",
    "workspace_allocations", "workspace_reallocations", "workspace_frees",
    "stale_cells", "auto_exit_pending",
)
NUMERIC_FIELDS = tuple(name for name in FIELD_NAMES if name != "last_mode")
MODES = {"WIDE", "COMPACT", "TOO_NARROW", "TOO_SHORT"}
HOST_PROFILES = ("normal", "ubsan", "asan")
HOST_CASES = (
    "field-compatibility", "capacity-and-guards", "layout-modes",
    "numeric-extremes", "single-emission", "storage-failure",
    "formatting-failure", "fragmented-control", "invalid-snapshot",
)
QEMU_PROFILES = tuple(
    f"q35-kvm-smp{smp}-run{run}"
    for smp in (4, 24)
    for run in range(1, 4)
)
SOURCE_PATHS = (
    "kernel/src/shell/commands/cmd_taskmantest.c",
    "kernel/link.ld",
    "scripts/taskman-stats-host.c",
    "scripts/test-taskman-stats-record.sh",
    "scripts/verify-taskman-stats-evidence.py",
    "scripts/verify-foundation-regression.py",
)
HISTORICAL_SERIAL_SHA256 = "df334cd069aaed1e9a948a4a4aca4cab83a857f97a41e21ff72fa220f986731d"
HISTORICAL_COMMAND_SHA256 = "6ddbe250984c69cd9d2abe2359dab0058c1bc5a0e758925dd0d7ccf45bb19cb7"
STATS_PREFIX = "[TASKMANTEST][STATS] "


class EvidenceError(Exception):
    pass


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_json(path, label):
    if not path.is_file():
        raise EvidenceError(f"{label} is missing")
    try:
        value = json.loads(path.read_text(errors="strict"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise EvidenceError(f"{label} is malformed: {error}") from None
    if not isinstance(value, dict):
        raise EvidenceError(f"{label} is not an object")
    return value


def read_json_lines(path, label):
    if not path.is_file():
        raise EvidenceError(f"{label} is missing")
    rows = []
    try:
        for number, line in enumerate(path.read_text(errors="strict").splitlines(), 1):
            if not line:
                continue
            value = json.loads(line)
            if not isinstance(value, dict):
                raise ValueError("row is not an object")
            rows.append(value)
    except (OSError, UnicodeError, json.JSONDecodeError, ValueError) as error:
        raise EvidenceError(f"{label} is malformed at row {number}: {error}") from None
    return rows


def read_env(path, label):
    if not path.is_file():
        raise EvidenceError(f"{label} is missing")
    result = {}
    for number, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            raise EvidenceError(f"{label} line {number} is malformed")
        key, value = line.split("=", 1)
        result[key] = value.replace("\\ ", " ").replace("\\(", "(").replace("\\)", ")")
    return result


def complete_lines(path):
    if not path.is_file():
        raise EvidenceError(f"serial transcript is missing: {path}")
    data = path.read_bytes()
    lines = []
    ends = []
    offset = 0
    for raw in data.splitlines(keepends=True):
        offset += len(raw)
        if not raw.endswith((b"\n", b"\r")):
            break
        lines.append(raw.rstrip(b"\r\n").decode(errors="replace"))
        ends.append(offset)
    return lines, ends, data


def parse_stats_record(line):
    if not line.startswith(STATS_PREFIX):
        raise EvidenceError("TASKMAN stats prefix is absent")
    tokens = line[len(STATS_PREFIX):].split(" ")
    if any(not token for token in tokens):
        raise EvidenceError("TASKMAN stats record has empty token spacing")
    pairs = []
    for token in tokens:
        if token.count("=") != 1:
            raise EvidenceError("TASKMAN stats token is not one key/value pair")
        key, value = token.split("=", 1)
        if not key or not value:
            raise EvidenceError(f"TASKMAN stats field {key or '<empty>'} lacks a value")
        pairs.append((key, value))
    names = tuple(key for key, _ in pairs)
    if names != FIELD_NAMES:
        missing = [name for name in FIELD_NAMES if name not in names]
        duplicate = sorted({name for name in names if names.count(name) > 1})
        extra = [name for name in names if name not in FIELD_NAMES]
        raise EvidenceError(
            "TASKMAN stats field order/set differs "
            f"missing={missing} duplicate={duplicate} extra={extra}")
    result = dict(pairs)
    for name in NUMERIC_FIELDS:
        if not re.fullmatch(r"[0-9]+", result[name]):
            raise EvidenceError(f"TASKMAN stats numeric field {name} is invalid")
        if int(result[name]) > (1 << 64) - 1:
            raise EvidenceError(f"TASKMAN stats numeric field {name} overflows u64")
    if result["last_mode"] not in MODES:
        raise EvidenceError("TASKMAN stats mode is invalid")
    return result


def transaction_errors(row, lines, byte_ends, profile, candidate_hash):
    errors = []
    required = {
        "schema": 2, "kind": "framed-command", "profile_id": profile["name"],
        "vm_id": profile["vm_id"], "candidate_sha256": candidate_hash,
        "transport_status": "PASS", "classification": "PASS",
        "disposition": "EXECUTED", "accept_count": 1, "begin_count": 1,
        "end_count": 1, "replay_count": 0, "reject_count": 0,
        "checksum_verified": True,
    }
    for key, expected in required.items():
        if row.get(key) != expected:
            errors.append(f"transaction {row.get('sequence')} field {key} differs")
    sequence = row.get("sequence")
    payload = row.get("payload")
    if not isinstance(sequence, int) or sequence < 1 or not isinstance(payload, str):
        return errors + ["transaction sequence or payload is invalid"]
    crc = f"{zlib.crc32(payload.encode()) & 0xffffffff:08x}"
    if row.get("crc32") != crc:
        errors.append(f"transaction {sequence} checksum differs")
    start = row.get("start_line_count")
    end = row.get("end_line_count")
    if not isinstance(start, int) or not isinstance(end, int) or not 0 <= start < end <= len(lines):
        return errors + [f"transaction {sequence} line bounds are invalid"]
    start_byte = row.get("start_byte_count")
    end_byte = row.get("end_byte_count")
    expected_start_byte = byte_ends[start - 1] if start else 0
    expected_end_byte = byte_ends[end - 1]
    if start_byte != expected_start_byte or end_byte != expected_end_byte:
        errors.append(f"transaction {sequence} byte bounds differ from serial")
    segment = lines[start:end]
    accept = f"[HARNESS][FRAME] ACCEPT seq={sequence} crc={crc} len="
    if len([line for line in segment if line.startswith(accept)]) != 1:
        errors.append(f"transaction {sequence} lacks one exact ACCEPT")
    if segment.count(f"[HARNESS][BEGIN] seq={sequence}") != 1:
        errors.append(f"transaction {sequence} lacks one exact BEGIN")
    terminal = f"[HARNESS][END] seq={sequence} status={row.get('handler_status')}"
    if segment.count(terminal) != 1:
        errors.append(f"transaction {sequence} lacks its exact END")
    if row.get("handler_status") != 0:
        errors.append(f"transaction {sequence} handler returned nonzero")
    return errors


def expected_payloads(smp):
    workers = min(smp * 2, 32)
    return (
        "taskmantest stats-reset",
        f"smpstress {workers} 0 1000",
        "taskmantest auto-session 50 5",
        "taskmantest stats",
        "taskdiag check",
        "killtest smpstress-sweep",
        "taskdiag check",
        "taskmantest check",
    )


def command_segment(lines, row):
    return lines[row["start_line_count"]:row["end_line_count"]]


def verify_qemu_profile(root, name, candidate_hash):
    errors = []
    profile_dir = root / "qemu" / name
    profile = read_env(profile_dir / "profile.env", f"{name} profile")
    serial_path = profile_dir / "qemu" / "qemu-serial.log"
    launch = read_env(profile_dir / "qemu" / "launch.env", f"{name} launch")
    lines, byte_ends, _ = complete_lines(serial_path)
    rows = read_json_lines(profile_dir / "commands.jsonl", f"{name} ledger")
    result = read_json(root / "result.json", "campaign result")
    synthetic = result.get("synthetic") is True
    match = re.fullmatch(r"q35-kvm-smp(4|24)-run([123])", name)
    if not match:
        return [f"unexpected QEMU profile name: {name}"]
    smp = int(match.group(1))
    expected_profile = {
        "schema": "1", "name": name, "machine": "q35", "accel": "kvm",
        "smp": str(smp), "candidate_sha256": candidate_hash,
        "cleanup": "PASS", "run_complete": "YES",
    }
    for key, expected in expected_profile.items():
        if profile.get(key) != expected:
            errors.append(f"{name}: profile field {key} differs")
    for key, expected in (("MACHINE", "q35"), ("ACCEL", "kvm"),
                          ("SMP", str(smp)), ("KERNEL_SHA256", candidate_hash)):
        if launch.get(key) != expected:
            errors.append(f"{name}: launch field {key} differs")
    if [row.get("sequence") for row in rows] != list(range(1, 9)):
        errors.append(f"{name}: command sequences are incomplete or reordered")
    if tuple(row.get("payload") for row in rows) != expected_payloads(smp):
        errors.append(f"{name}: command coverage or order differs")
    if not synthetic:
        transport_verifier = (root / "source/scripts/verify-foundation-regression.py")
        for sequence, row in enumerate(rows, 1):
            record_name = row.get("transport_record")
            record = profile_dir / record_name \
                if isinstance(record_name, str) else profile_dir / "<invalid>"
            if (not isinstance(record_name, str) or
                    Path(record_name).name != record_name or
                    not record.is_file()):
                errors.append(
                    f"{name}: transaction {sequence} transport record is absent")
                continue
            if row.get("transport_schema") != 26 or \
                    row.get("transport_record_sha256") != sha256(record):
                errors.append(
                    f"{name}: transaction {sequence} transport identity differs")
                continue
            raw = read_json(record, f"{name} transaction {sequence} transport")
            projected = {
                key: value for key, value in raw.items()
                if key in row and key not in (
                    "schema", "transport_schema", "transport_record",
                    "transport_record_sha256")
            }
            expected_projection = {
                key: value for key, value in row.items()
                if key not in (
                    "schema", "transport_schema", "transport_record",
                    "transport_record_sha256")
            }
            if raw.get("schema") != 26 or projected != expected_projection:
                errors.append(
                    f"{name}: transaction {sequence} projection differs")
                continue
            completed = subprocess.run(
                [sys.executable, str(transport_verifier),
                 "transport-command", str(profile_dir), str(record),
                 str(serial_path)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                check=False)
            if completed.returncode != 0:
                errors.append(
                    f"{name}: transaction {sequence} transport replay failed: " +
                    completed.stdout.strip().replace("\n", "; "))
    previous_end = 0
    for row in rows:
        if isinstance(row.get("start_line_count"), int) and row["start_line_count"] < previous_end:
            errors.append(f"{name}: command transactions overlap")
        if isinstance(row.get("end_line_count"), int):
            previous_end = row["end_line_count"]
        errors.extend(f"{name}: {error}" for error in
                      transaction_errors(row, lines, byte_ends, profile,
                                         candidate_hash))
    if len(rows) != 8:
        return errors

    reset = command_segment(lines, rows[0])
    if reset.count("[TASKMANTEST][STATS_RESET] PASS") != 1:
        errors.append(f"{name}: stats reset result is absent")
    workers = min(smp * 2, 32)
    stress = command_segment(lines, rows[1])
    if len([line for line in stress if line.startswith(
            f"[SMP] smpstress spawning workers={workers} ")]) != 1:
        errors.append(f"{name}: concurrent worker launch is not proved")
    session = command_segment(lines, rows[2])
    session_records = [line for line in session
                       if line.startswith("[TASKMANTEST][AUTO_SESSION] PASS ")]
    if len(session_records) != 1 or not all(token in session_records[0] for token in
            (" target=5 ", " full_delta=5 ", " fallback_delta=0 ",
             " valid=1")):
        errors.append(f"{name}: concurrent TASKMAN session is incomplete")

    stats_segment = command_segment(lines, rows[3])
    stats_lines = [line for line in stats_segment if line.startswith(STATS_PREFIX)]
    if len(stats_lines) != 1:
        errors.append(f"{name}: TASKMAN stats record count is {len(stats_lines)}, expected 1")
    else:
        try:
            stats = parse_stats_record(stats_lines[0])
            if int(stats["last_session_full_frames"]) != 5:
                errors.append(f"{name}: TASKMAN stats do not belong to the focal session")
            if int(stats["last_session_fallback_frames"]) != 0:
                errors.append(f"{name}: TASKMAN stats report fallback frames")
            if int(stats["captured"]) < workers:
                errors.append(f"{name}: TASKMAN stats did not observe concurrent workers")
        except EvidenceError as error:
            errors.append(f"{name}: {error}")
    sweep = command_segment(lines, rows[5])
    expected_sweep = re.compile(
        rf"^\[SMP\]\[KILL_SWEEP\] PASS workers={workers} killed={workers} "
        rf"cleaned={workers} reaped={workers}$")
    if len([line for line in sweep if expected_sweep.fullmatch(line)]) != 1:
        errors.append(f"{name}: worker lifetime through stats is not proved")
    final_check = command_segment(lines, rows[7])
    if len([line for line in final_check
            if line.startswith("[TASKMANTEST][CHECK] PASS")]) != 1:
        errors.append(f"{name}: final TASKMAN check is absent")
    return errors


def verify_host(root):
    errors = []
    result = read_json(root / "host" / "result.json", "host result")
    if result.get("profiles") != list(HOST_PROFILES):
        errors.append("host profile list differs")
    for name in HOST_PROFILES:
        run = root / "host" / name
        command = read_json(run / "command.json", f"host {name} command")
        if command.get("sanitizer") != {"normal": "none", "ubsan": "undefined",
                                             "asan": "address"}[name]:
            errors.append(f"host {name} sanitizer identity differs")
        if (run / "compile-exit-code.txt").read_text().strip() != "0":
            errors.append(f"host {name} compilation did not succeed")
        if (run / "run-exit-code.txt").read_text().strip() != "0":
            errors.append(f"host {name} execution did not succeed")
        lines = (run / "run.log").read_text(errors="replace").splitlines()
        expected_begin = "[STATS_HOST][SUITE_BEGIN] cases=9 fields=52 capacity=1953"
        if lines.count(expected_begin) != 1:
            errors.append(f"host {name} suite start differs")
        for case in HOST_CASES:
            marker = f"[STATS_HOST][CASE] id={case} status=PASS"
            if lines.count(marker) != 1:
                errors.append(f"host {name} case {case} is absent or duplicated")
        suite = [line for line in lines if line.startswith("[STATS_HOST][SUITE_END] ")]
        if len(suite) != 1 or not re.fullmatch(
                r"\[STATS_HOST\]\[SUITE_END\] status=PASS cases=9 "
                r"assertions=[1-9][0-9]* failures=0 fields=52 capacity=1953",
                suite[0]):
            errors.append(f"host {name} suite completion differs")
        binary = run / "taskman-stats-host"
        if not binary.is_file() or command.get("binary_sha256") != sha256(binary):
            errors.append(f"host {name} binary identity differs")
    return errors


def verify_historical(root):
    errors = []
    history = root / "historical"
    serial = history / "qemu-serial.log"
    command_path = history / "command-28.json"
    result = read_json(history / "result.json", "historical result")
    if sha256(serial) != HISTORICAL_SERIAL_SHA256:
        errors.append("historical serial hash differs")
    if sha256(command_path) != HISTORICAL_COMMAND_SHA256:
        errors.append("historical command hash differs")
    command = read_json(command_path, "historical command")
    lines, _, _ = complete_lines(serial)
    start = command.get("start_line_count")
    end = command.get("end_line_count")
    if not isinstance(start, int) or not isinstance(end, int) or not 0 <= start < end <= len(lines):
        errors.append("historical transaction bounds are invalid")
        return errors
    segment = lines[start:end]
    records = [line for line in segment if line.startswith(STATS_PREFIX)]
    if len(records) != 1 or not records[0].endswith(" page_changes="):
        errors.append("historical split prefix was not preserved")
    if len([line for line in segment
            if line.startswith("0 selection_changes=0")]) != 1:
        errors.append("historical split continuation was not preserved")
    if result.get("foundation_verifier_exit_code") != 1 or not result.get(
            "split_record_rejected"):
        errors.append("historical verifier rejection is not recorded")
    verification = history / "foundation-verifier.log"
    if not verification.is_file() or "[TASKMANTEST][STATS] count is 0, expected 1" not in verification.read_text(errors="replace"):
        errors.append("historical verifier did not reject the split record")
    return errors


def verify_sources(root, base_commit, synthetic):
    errors = []
    manifest = read_json(root / "source-manifest.json", "source manifest")
    rows = manifest.get("files")
    if not isinstance(rows, list):
        return ["source manifest file list is invalid"]
    found = {row.get("path"): row for row in rows if isinstance(row, dict)}
    if set(found) != set(SOURCE_PATHS):
        errors.append("source manifest paths differ")
    for name, row in found.items():
        copy_path = root / row.get("evidence_copy", "")
        if not copy_path.is_file() or row.get("sha256") != sha256(copy_path):
            errors.append(f"source evidence identity differs: {name}")
    linker = root / "source" / "kernel" / "link.ld"
    if synthetic:
        return errors
    environment = read_env(
        root / "preflight/environment.env", "preflight environment")
    if (not isinstance(base_commit, str) or
            re.fullmatch(r"[0-9a-f]{40}", base_commit) is None or
            environment.get("implementation_base_commit") != base_commit):
        errors.append("implementation base commit identity differs")
        return errors
    completed = subprocess.run(
        ["git", "show", f"{base_commit}:kernel/link.ld"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    base_linker = root / "static/base-link.ld"
    if (completed.returncode != 0 or not linker.is_file() or
            not base_linker.is_file() or
            completed.stdout != linker.read_bytes() or
            completed.stdout != base_linker.read_bytes()):
        errors.append("linker script differs from the audited base commit")
    return errors


def verify_all(root):
    errors = []
    result = read_json(root / "result.json", "aggregate result")
    if result.get("kind") != "taskman-stats-record-evidence" or result.get("schema") != 1:
        errors.append("aggregate result identity differs")
    if result.get("status") != "PASS" or result.get("field_count") != 52 or result.get("record_capacity") != 1953:
        errors.append("aggregate status, field count, or capacity differs")
    candidate = root / "candidate" / "kernel.elf"
    image = root / "candidate" / "hobbyos.img"
    if not candidate.is_file() or not image.is_file():
        errors.append("focal candidate ELF or image is missing")
        candidate_hash = ""
    else:
        candidate_hash = sha256(candidate)
        if result.get("candidate_sha256") != candidate_hash:
            errors.append("aggregate candidate hash differs")
    synthetic = result.get("synthetic") is True
    errors.extend(verify_sources(
        root, result.get("implementation_base_commit"), synthetic))
    errors.extend(verify_host(root))
    errors.extend(verify_historical(root))
    profiles = result.get("qemu_profiles")
    if profiles != list(QEMU_PROFILES):
        errors.append("QEMU profile matrix differs")
    for name in QEMU_PROFILES:
        try:
            errors.extend(verify_qemu_profile(root, name, candidate_hash))
        except EvidenceError as error:
            errors.append(f"{name}: {error}")
    return errors


def write_fixture(root):
    root.mkdir(parents=True, exist_ok=True)
    (root / "candidate").mkdir()
    (root / "candidate/kernel.elf").write_bytes(b"fixture-kernel")
    (root / "candidate/hobbyos.img").write_bytes(b"fixture-image")
    candidate_hash = sha256(root / "candidate/kernel.elf")
    source_rows = []
    for name in SOURCE_PATHS:
        destination = root / "source" / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(
            b"fixture" if name == "kernel/link.ld" else b"fixture source\n")
        source_rows.append({"path": name, "evidence_copy": f"source/{name}",
                            "sha256": sha256(destination)})
    # Fixture source verification uses its own declared linker identity below.
    for name in HOST_PROFILES:
        run = root / "host" / name
        run.mkdir(parents=True)
        binary = run / "taskman-stats-host"
        binary.write_bytes(b"fixture-host-" + name.encode())
        (run / "compile-exit-code.txt").write_text("0\n")
        (run / "run-exit-code.txt").write_text("0\n")
        lines = ["[STATS_HOST][SUITE_BEGIN] cases=9 fields=52 capacity=1953"]
        lines.extend(f"[STATS_HOST][CASE] id={case} status=PASS" for case in HOST_CASES)
        lines.append("[STATS_HOST][SUITE_END] status=PASS cases=9 assertions=60 failures=0 fields=52 capacity=1953")
        (run / "run.log").write_text("\n".join(lines) + "\n")
        (run / "command.json").write_text(json.dumps({
            "sanitizer": {"normal": "none", "ubsan": "undefined", "asan": "address"}[name],
            "binary_sha256": sha256(binary),
        }) + "\n")
    (root / "host/result.json").write_text(json.dumps({"profiles": list(HOST_PROFILES)}) + "\n")
    history = root / "historical"
    history.mkdir()
    # Synthetic fixtures cannot impersonate the frozen historical hashes; fixture
    # verification patches these two checks through an explicit synthetic marker.
    historical_lines = ["boot"] * 1386 + [
        "[HARNESS][FRAME] ACCEPT seq=28 crc=205331bc len=43", "transport",
        "[HARNESS][BEGIN] seq=28",
        "[TASKMANTEST][STATS] sessions=0 page_changes=",
        "0 selection_changes=0", "tail",
        "[HARNESS][END] seq=28 status=0",
    ]
    (history / "qemu-serial.log").write_text("\n".join(historical_lines) + "\n")
    command = {"start_line_count": 1386, "end_line_count": 1393}
    (history / "command-28.json").write_text(json.dumps(command) + "\n")
    (history / "foundation-verifier.log").write_text(
        "profile-error: taskmantest stats: [TASKMANTEST][STATS] count is 0, expected 1\n")
    (history / "result.json").write_text(json.dumps({
        "foundation_verifier_exit_code": 1, "split_record_rejected": True,
        "synthetic": True,
    }) + "\n")

    record = STATS_PREFIX + " ".join(
        f"{name}={'WIDE' if name == 'last_mode' else '0'}" for name in FIELD_NAMES)
    for name in QEMU_PROFILES:
        profile_dir = root / "qemu" / name
        qemu = profile_dir / "qemu"
        qemu.mkdir(parents=True)
        smp = 24 if "smp24" in name else 4
        vm_id = "fixture-" + name
        lines = []
        rows = []
        bodies = (
            ["[TASKMANTEST][STATS_RESET] PASS"],
            [f"[SMP] smpstress spawning workers={min(smp * 2, 32)} panic_pct=0 period_ms=1000 warmup_s=0 yield_ms=1 panic_check_ms=50"],
            ["[TASKMANTEST][AUTO_SESSION] PASS refresh=50 target=5 taskman_status=0 sessions_delta=1 auto_exit_delta=1 full_delta=5 fallback_delta=0 render_fail_delta=0 shortfall_delta=0 last_mode=WIDE pages=1 captured=64 modal_scroll=0 shell_exit_scroll=0 clipped=0 model_live=0 pending=0 controls_default=1 valid=1"],
            [record.replace("last_session_full_frames=0", "last_session_full_frames=5").replace("captured=0", f"captured={min(smp * 2, 32)}")],
            ["[TASKDIAG][CHECK] PASS current=1"],
            [f"[SMP][KILL_SWEEP] PASS workers={min(smp * 2, 32)} killed={min(smp * 2, 32)} cleaned={min(smp * 2, 32)} reaped={min(smp * 2, 32)}"],
            ["[TASKDIAG][CHECK] PASS current=1"],
            ["[TASKMANTEST][CHECK] PASS violations=0"],
        )
        for sequence, (payload, body) in enumerate(zip(expected_payloads(smp), bodies), 1):
            start = len(lines)
            crc = f"{zlib.crc32(payload.encode()) & 0xffffffff:08x}"
            lines.extend([
                f"[HARNESS][FRAME] ACCEPT seq={sequence} crc={crc} len={len(payload)}",
                f"[HARNESS][BEGIN] seq={sequence}", *body,
                f"[HARNESS][END] seq={sequence} status=0",
            ])
            end = len(lines)
            byte_ends = []
            offset = 0
            for line in lines:
                offset += len(line.encode()) + 1
                byte_ends.append(offset)
            rows.append({
                "schema": 2, "kind": "framed-command", "profile_id": name,
                "vm_id": vm_id, "candidate_sha256": candidate_hash,
                "sequence": sequence, "payload": payload, "crc32": crc,
                "start_line_count": start, "end_line_count": end,
                "start_byte_count": byte_ends[start - 1] if start else 0,
                "end_byte_count": byte_ends[end - 1], "handler_status": 0,
                "transport_status": "PASS", "classification": "PASS",
                "disposition": "EXECUTED", "accept_count": 1,
                "begin_count": 1, "end_count": 1, "replay_count": 0,
                "reject_count": 0, "checksum_verified": True,
            })
        (qemu / "qemu-serial.log").write_text("\n".join(lines) + "\n")
        (profile_dir / "commands.jsonl").write_text(
            "".join(json.dumps(row, sort_keys=True) + "\n" for row in rows))
        (profile_dir / "profile.env").write_text(
            f"schema=1\nname={name}\nmachine=q35\naccel=kvm\nsmp={smp}\n"
            f"vm_id={vm_id}\ncandidate_sha256={candidate_hash}\ncleanup=PASS\nrun_complete=YES\n")
        (qemu / "launch.env").write_text(
            f"MACHINE=q35\nACCEL=kvm\nSMP={smp}\nKERNEL_SHA256={candidate_hash}\n")
    (root / "result.json").write_text(json.dumps({
        "schema": 1, "kind": "taskman-stats-record-evidence", "status": "PASS",
        "field_count": 52, "record_capacity": 1953,
        "candidate_sha256": candidate_hash, "qemu_profiles": list(QEMU_PROFILES),
        "synthetic": True,
    }, indent=2) + "\n")
    (root / "source-manifest.json").write_text(json.dumps({"files": source_rows}) + "\n")


def synthetic_verify_all(root):
    """Fixture-only equivalent with frozen-input identity checks suppressed."""
    global HISTORICAL_SERIAL_SHA256, HISTORICAL_COMMAND_SHA256
    old = (HISTORICAL_SERIAL_SHA256, HISTORICAL_COMMAND_SHA256)
    HISTORICAL_SERIAL_SHA256 = sha256(root / "historical/qemu-serial.log")
    HISTORICAL_COMMAND_SHA256 = sha256(root / "historical/command-28.json")
    try:
        return verify_all(root)
    finally:
        HISTORICAL_SERIAL_SHA256, HISTORICAL_COMMAND_SHA256 = old


def run_fixtures(output):
    if output.exists():
        raise EvidenceError("fixture output directory already exists")
    valid = output / "valid"
    write_fixture(valid)
    valid_errors = synthetic_verify_all(valid)
    results = [{"name": "valid", "expected": "PASS",
                "observed": "PASS" if not valid_errors else "FAIL",
                "errors": valid_errors}]
    mutations = (
        ("split-record", "field page_changes lacks a value", lambda root: mutate_stats(root, "split")),
        ("missing-field", "field order/set differs", lambda root: mutate_stats(root, "missing")),
        ("duplicate-field", "field order/set differs", lambda root: mutate_stats(root, "duplicate")),
        ("record-after-end", "record count is 0", lambda root: mutate_stats(root, "late")),
        ("handler-nonzero", "handler returned nonzero", mutate_handler),
        ("wrong-candidate", "candidate_sha256 differs", mutate_candidate),
        ("missing-profile", "q35-kvm-smp24-run3", mutate_profile),
        ("host-case-missing", "case fragmented-control", mutate_host),
        ("worker-lifetime-missing", "worker lifetime through stats", mutate_sweep),
        ("historical-accepted", "historical verifier rejection", mutate_historical),
    )
    for name, expected, mutation in mutations:
        target = output / name
        shutil.copytree(valid, target)
        mutation(target)
        errors = synthetic_verify_all(target)
        matched = bool(errors) and any(expected in error for error in errors)
        results.append({"name": name, "expected": "REJECT", "observed":
                        "REJECT" if matched else "WRONG", "reason": expected,
                        "errors": errors})
    passed = results[0]["observed"] == "PASS" and all(
        row["observed"] == "REJECT" for row in results[1:])
    (output / "result.json").write_text(json.dumps({
        "schema": 1, "status": "PASS" if passed else "FAIL", "cases": results,
        "valid_controls": 1, "negative_cases": len(results) - 1,
    }, indent=2, sort_keys=True) + "\n")
    print(f"stats-fixtures status={'PASS' if passed else 'FAIL'} "
          f"valid=1 negatives={len(results) - 1}")
    return 0 if passed else 1


def load_profile_rows(root, profile="q35-kvm-smp4-run1"):
    path = root / "qemu" / profile / "commands.jsonl"
    return path, read_json_lines(path, "fixture ledger")


def rewrite_profile_serial(root, transform, profile="q35-kvm-smp4-run1"):
    profile_dir = root / "qemu" / profile
    serial = profile_dir / "qemu/qemu-serial.log"
    rows_path, rows = load_profile_rows(root, profile)
    lines = serial.read_text().splitlines()
    stats_row = rows[3]
    start, end = stats_row["start_line_count"], stats_row["end_line_count"]
    segment = lines[start:end]
    changed = transform(segment)
    lines[start:end] = changed
    serial.write_text("\n".join(lines) + "\n")
    # Reconcile every byte/line boundary after the focal mutation.
    offset = 0
    line_ends = []
    for line in lines:
        offset += len(line.encode()) + 1
        line_ends.append(offset)
    delta = len(changed) - (end - start)
    for row in rows:
        if row["sequence"] == 4:
            row["end_line_count"] += delta
        elif row["sequence"] > 4:
            row["start_line_count"] += delta
            row["end_line_count"] += delta
        row["start_byte_count"] = line_ends[row["start_line_count"] - 1] if row["start_line_count"] else 0
        row["end_byte_count"] = line_ends[row["end_line_count"] - 1]
    rows_path.write_text("".join(json.dumps(row, sort_keys=True) + "\n" for row in rows))


def mutate_stats(root, kind):
    def transform(segment):
        index = next(i for i, line in enumerate(segment) if line.startswith(STATS_PREFIX))
        line = segment[index]
        if kind == "split":
            needle = "page_changes="
            position = line.index(needle) + len(needle)
            segment[index:index + 1] = [line[:position], line[position:]]
        elif kind == "missing":
            segment[index] = line.replace(" page_changes=0", "", 1)
        elif kind == "duplicate":
            segment[index] = line.replace(" page_changes=0", " page_changes=0 page_changes=0", 1)
        elif kind == "late":
            moved = segment.pop(index)
            segment.append(moved)
            # Put the record after END while retaining the same transaction bound.
            end_index = next(i for i, value in enumerate(segment) if value.startswith("[HARNESS][END]"))
            segment.insert(end_index + 1, segment.pop())
        return segment
    rewrite_profile_serial(root, transform)
    if kind == "late":
        path, rows = load_profile_rows(root)
        rows[3]["end_line_count"] -= 1
        path.write_text("".join(json.dumps(row, sort_keys=True) + "\n" for row in rows))


def mutate_handler(root):
    path, rows = load_profile_rows(root)
    rows[3]["handler_status"] = 1
    path.write_text("".join(json.dumps(row, sort_keys=True) + "\n" for row in rows))


def mutate_candidate(root):
    path, rows = load_profile_rows(root)
    rows[3]["candidate_sha256"] = "0" * 64
    path.write_text("".join(json.dumps(row, sort_keys=True) + "\n" for row in rows))


def mutate_profile(root):
    shutil.rmtree(root / "qemu/q35-kvm-smp24-run3")


def mutate_host(root):
    path = root / "host/normal/run.log"
    lines = [line for line in path.read_text().splitlines()
             if "id=fragmented-control" not in line]
    path.write_text("\n".join(lines) + "\n")


def mutate_sweep(root):
    def transform(segment):
        return [line for line in segment if not line.startswith("[SMP][KILL_SWEEP]")]
    profile_dir = root / "qemu/q35-kvm-smp4-run1"
    serial = profile_dir / "qemu/qemu-serial.log"
    rows = read_json_lines(profile_dir / "commands.jsonl", "fixture ledger")
    lines = serial.read_text().splitlines()
    row = rows[5]
    lines[row["start_line_count"]:row["end_line_count"]] = transform(
        lines[row["start_line_count"]:row["end_line_count"]])
    serial.write_text("\n".join(lines) + "\n")
    # The fixture may also report boundary errors; the focal sweep reason remains required.


def mutate_historical(root):
    result = read_json(root / "historical/result.json", "fixture history")
    result["split_record_rejected"] = False
    (root / "historical/result.json").write_text(json.dumps(result) + "\n")


def main():
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    all_parser = subparsers.add_parser("all")
    all_parser.add_argument("evidence_root")
    fixtures = subparsers.add_parser("fixtures")
    fixtures.add_argument("--output-dir", required=True)
    args = parser.parse_args()
    try:
        if args.command == "fixtures":
            return run_fixtures(Path(args.output_dir).resolve())
        root = Path(args.evidence_root).resolve()
        errors = verify_all(root)
        if errors:
            for error in errors:
                print("stats-evidence-error: " + error)
            return 1
        print("stats-evidence status=PASS host_profiles=3 qemu_profiles=6 "
              "fields=52 historical_split=REJECTED")
        return 0
    except EvidenceError as error:
        print("stats-evidence-error: " + str(error))
        return 1


if __name__ == "__main__":
    sys.exit(main())
