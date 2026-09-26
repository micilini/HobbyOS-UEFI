#!/usr/bin/env python3
"""Verify runtime-assertion and guarded-list evidence without running a VM."""

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


SCENARIOS = {
    "warn": 1,
    "false": 2,
    "valid-list": 3,
    "double-insert": 4,
    "insert-reciprocity": 5,
    "remove-reciprocity": 6,
    "bug": 7,
    "locked-bug": 8,
    "reentry": 9,
    "second-reentry": 10,
}
NONFATAL = {"warn", "false", "valid-list"}
UNCHANGED = {
    "warn", "false", "double-insert", "insert-reciprocity",
    "remove-reciprocity", "bug", "locked-bug", "reentry",
    "second-reentry",
}
HOST_CASES = {
    "macro-contract", "valid-list-states", "queue-fifo",
    "rejected-before-store", "deterministic-sequences",
}


class EvidenceError(Exception):
    """A classified evidence rejection."""


def reject(code, detail=""):
    message = code if not detail else f"{code}: {detail}"
    raise EvidenceError(message)


def require(condition, code, detail=""):
    if not condition:
        reject(code, detail)


def load_json(path):
    require(path.is_file() and not path.is_symlink(), "MISSING_JSON", str(path))
    try:
        value = json.loads(path.read_text())
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        reject("INVALID_JSON", f"{path}: {error}")
    require(isinstance(value, dict), "INVALID_JSON_ROOT", str(path))
    return value


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def require_file(path, expected_hash=None):
    require(path.is_file() and not path.is_symlink(), "MISSING_FILE", str(path))
    if expected_hash is not None:
        require(re.fullmatch(r"[0-9a-f]{64}", str(expected_hash)) is not None,
                "INVALID_HASH", str(path))
        require(sha256(path) == expected_hash, "HASH_MISMATCH", str(path))


def read_lines(path):
    require_file(path)
    try:
        return [line.removesuffix("\r") for line in
                path.read_text(errors="replace").splitlines()]
    except OSError as error:
        reject("READ_ERROR", f"{path}: {error}")


def unique_line(lines, prefix, code):
    matches = [(index, line) for index, line in enumerate(lines)
               if line.startswith(prefix)]
    require(len(matches) == 1, code, f"prefix={prefix!r} count={len(matches)}")
    return matches[0]


def fields(line, prefix, required=()):
    require(line.startswith(prefix), "LINE_PREFIX_MISMATCH", prefix)
    parsed = {}
    tail = line[len(prefix):].strip()
    for token in tail.split():
        require("=" in token, "MALFORMED_FIELD", token)
        key, value = token.split("=", 1)
        require(key and key not in parsed, "DUPLICATE_FIELD", key)
        parsed[key] = value
    for key in required:
        require(key in parsed, "MISSING_FIELD", f"{prefix}{key}")
    return parsed


def integer(value, code, base=10):
    try:
        return int(value, base)
    except (TypeError, ValueError):
        reject(code, str(value))


def decode_hex_context(line, prefix, early, observed=None):
    required = (
        "file_hex", "file_bytes", "file_valid", "file_truncated", "line",
        "expression_hex", "expression_bytes", "expression_valid",
        "expression_truncated", "cpu_id", "cpu_valid", "cpu_slot",
        "slot_valid", "task_id", "task_generation", "task_valid",
    )
    parsed = fields(line, prefix, required)
    for name in ("file_hex", "expression_hex"):
        require(re.fullmatch(r"[0-9a-f]*", parsed[name]) is not None,
                "CONTEXT_HEX_INVALID", name)
        byte_name = "file_bytes" if name == "file_hex" else "expression_bytes"
        require(len(parsed[name]) == integer(parsed[byte_name],
                                             "CONTEXT_SIZE_INVALID") * 2,
                "CONTEXT_SIZE_MISMATCH", name)
    try:
        file_text = bytes.fromhex(parsed["file_hex"]).decode()
        expression = bytes.fromhex(parsed["expression_hex"]).decode()
    except (ValueError, UnicodeError) as error:
        reject("CONTEXT_TEXT_INVALID", str(error))
    require(integer(parsed["file_valid"], "CONTEXT_FLAG_INVALID") == 1,
            "CONTEXT_FILE_INVALID")
    require(integer(parsed["expression_valid"], "CONTEXT_FLAG_INVALID") == 1,
            "CONTEXT_EXPRESSION_INVALID")
    require(integer(parsed["file_truncated"], "CONTEXT_FLAG_INVALID") == 0 and
            integer(parsed["expression_truncated"],
                    "CONTEXT_FLAG_INVALID") == 0,
            "CONTEXT_UNEXPECTED_TRUNCATION")
    require(file_text.endswith(("assert_selftest.c", "list.h")),
            "CONTEXT_CALLSITE_FILE_INVALID", file_text)
    require(expression != "", "CONTEXT_EXPRESSION_EMPTY")
    require(integer(parsed["line"], "CONTEXT_LINE_INVALID") > 0,
            "CONTEXT_LINE_INVALID")
    require(integer(parsed["cpu_valid"], "CONTEXT_FLAG_INVALID") == 1,
            "CONTEXT_CPU_INVALID")
    if early:
        require(integer(parsed["slot_valid"], "CONTEXT_FLAG_INVALID") == 0 and
                integer(parsed["task_valid"], "CONTEXT_FLAG_INVALID") == 0,
                "EARLY_CONTEXT_FABRICATED")
        require(integer(parsed["task_id"], "CONTEXT_TASK_INVALID") == 0 and
                integer(parsed["task_generation"],
                        "CONTEXT_TASK_INVALID") == 0,
                "EARLY_CONTEXT_TASK_NONZERO")
    else:
        require(integer(parsed["slot_valid"], "CONTEXT_FLAG_INVALID") == 1 and
                integer(parsed["task_valid"], "CONTEXT_FLAG_INVALID") == 1,
                "RUNTIME_CONTEXT_INVALID")
        require(observed is not None, "INTERNAL_OBSERVATION_MISSING")
        require(integer(parsed["cpu_id"], "CONTEXT_CPU_INVALID") ==
                observed["topology_cpu_id"] and
                integer(parsed["cpu_slot"], "CONTEXT_SLOT_INVALID") ==
                observed["state_slot"], "CONTEXT_CPU_MISMATCH")
        require(integer(parsed["task_id"], "CONTEXT_TASK_INVALID") ==
                observed["scheduler_task_id"] and
                integer(parsed["task_generation"], "CONTEXT_TASK_INVALID") ==
                observed["scheduler_task_generation"],
                "CONTEXT_TASK_MISMATCH")
    return {"fields": parsed, "file": file_text, "expression": expression}


def parse_gdb_values(path):
    values = {}
    for line in read_lines(path):
        match = re.fullmatch(r"([A-Z][A-Z0-9_]*)=(0x[0-9a-fA-F]+|[0-9]+)",
                             line)
        if not match:
            continue
        require(match.group(1) not in values, "DUPLICATE_GDB_VALUE",
                f"{path}:{match.group(1)}")
        values[match.group(1)] = int(match.group(2), 0)
    return values


def verify_artifacts(run_dir, result):
    artifacts = result.get("artifacts")
    require(isinstance(artifacts, dict) and artifacts,
            "ARTIFACT_LEDGER_MISSING", str(run_dir))
    mandatory = {
        "serial-transcript.log", "gdb-arm.log", "gdb-observer.log",
        "gdb-after.log", "fixture-before.bin", "fixture-after.bin",
        "launch.json", "qmp-events.jsonl",
    }
    if result.get("scenario") not in NONFATAL:
        mandatory |= {"gdb-halt-1.log", "gdb-halt-2.log"}
    if result.get("scenario") == "warn" or result.get("scenario") not in NONFATAL:
        mandatory.add("gdb-context-observer.log")
    require(mandatory <= set(artifacts), "ARTIFACT_LEDGER_INCOMPLETE",
            ",".join(sorted(mandatory - set(artifacts))))
    for name, receipt in artifacts.items():
        require(isinstance(receipt, dict), "ARTIFACT_RECEIPT_INVALID", name)
        path = run_dir / name
        require_file(path, receipt.get("sha256"))
        require(path.stat().st_size == receipt.get("bytes"),
                "ARTIFACT_SIZE_MISMATCH", str(path))


def verify_precondition_observer(run_dir, result):
    raw = parse_gdb_values(run_dir / "gdb-observer.log")
    required = {
        "STATE_OBSERVED_SLOT", "STATE_OBSERVED_CPU_ID", "TOPOLOGY_CPU_ID",
        "STATE_TASK_ID", "STATE_TASK_GENERATION", "SCHEDULER_TASK_ID",
        "SCHEDULER_TASK_GENERATION", "ASSERT_LOCK_ADDR",
        "OBSERVER_RELEASED",
    }
    require(required <= set(raw), "OBSERVER_FIELDS_MISSING",
            ",".join(sorted(required - set(raw))))
    require(raw["OBSERVER_RELEASED"] == 1, "OBSERVER_NOT_RELEASED")
    require(raw["STATE_OBSERVED_CPU_ID"] == raw["TOPOLOGY_CPU_ID"],
            "OBSERVER_CPU_MISMATCH")
    require(raw["STATE_TASK_ID"] != 0 and
            raw["STATE_TASK_ID"] == raw["SCHEDULER_TASK_ID"] and
            raw["STATE_TASK_GENERATION"] ==
            raw["SCHEDULER_TASK_GENERATION"],
            "OBSERVER_TASK_MISMATCH")
    recorded = result.get("precondition_observation")
    require(isinstance(recorded, dict), "OBSERVER_LEDGER_MISSING")
    mapping = {
        "state_slot": "STATE_OBSERVED_SLOT",
        "state_cpu_id": "STATE_OBSERVED_CPU_ID",
        "topology_cpu_id": "TOPOLOGY_CPU_ID",
        "state_task_id": "STATE_TASK_ID",
        "state_task_generation": "STATE_TASK_GENERATION",
        "scheduler_task_id": "SCHEDULER_TASK_ID",
        "scheduler_task_generation": "SCHEDULER_TASK_GENERATION",
        "lock_address": "ASSERT_LOCK_ADDR",
    }
    for json_key, raw_key in mapping.items():
        require(recorded.get(json_key) == raw[raw_key],
                "OBSERVER_LEDGER_MISMATCH", json_key)
    return raw


def verify_context_observer(run_dir, result):
    raw = parse_gdb_values(run_dir / "gdb-context-observer.log")
    required = {
        "CONTEXT_STATE_CPU_ID", "CONTEXT_STATE_SLOT", "CONTEXT_VALID_MASK",
        "CONTEXT_STATE_TASK_ID", "CONTEXT_STATE_TASK_GENERATION",
        "CONTEXT_TOPOLOGY_CPU_ID", "CONTEXT_SCHEDULER_TASK_ID",
        "CONTEXT_SCHEDULER_TASK_GENERATION", "CONTEXT_RELEASED",
    }
    require(required <= set(raw), "CONTEXT_OBSERVER_FIELDS_MISSING",
            ",".join(sorted(required - set(raw))))
    require(raw["CONTEXT_RELEASED"] == 1 and raw["CONTEXT_VALID_MASK"] == 7,
            "CONTEXT_OBSERVER_NOT_RELEASED_OR_VALID")
    require(raw["CONTEXT_STATE_CPU_ID"] == raw["CONTEXT_TOPOLOGY_CPU_ID"],
            "OBSERVER_CPU_MISMATCH")
    require(raw["CONTEXT_STATE_TASK_ID"] != 0 and
            raw["CONTEXT_STATE_TASK_ID"] == raw["CONTEXT_SCHEDULER_TASK_ID"] and
            raw["CONTEXT_STATE_TASK_GENERATION"] ==
            raw["CONTEXT_SCHEDULER_TASK_GENERATION"],
            "OBSERVER_TASK_MISMATCH")
    recorded = result.get("independent_observation")
    require(isinstance(recorded, dict), "OBSERVER_LEDGER_MISSING")
    mapping = {
        "state_slot": "CONTEXT_STATE_SLOT",
        "state_cpu_id": "CONTEXT_STATE_CPU_ID",
        "valid_mask": "CONTEXT_VALID_MASK",
        "state_task_id": "CONTEXT_STATE_TASK_ID",
        "state_task_generation": "CONTEXT_STATE_TASK_GENERATION",
        "topology_cpu_id": "CONTEXT_TOPOLOGY_CPU_ID",
        "scheduler_task_id": "CONTEXT_SCHEDULER_TASK_ID",
        "scheduler_task_generation": "CONTEXT_SCHEDULER_TASK_GENERATION",
    }
    for json_key, raw_key in mapping.items():
        require(recorded.get(json_key) == raw[raw_key],
                "OBSERVER_LEDGER_MISMATCH", json_key)
    raw["state_slot"] = raw["CONTEXT_STATE_SLOT"]
    raw["topology_cpu_id"] = raw["CONTEXT_TOPOLOGY_CPU_ID"]
    raw["scheduler_task_id"] = raw["CONTEXT_SCHEDULER_TASK_ID"]
    raw["scheduler_task_generation"] = raw["CONTEXT_SCHEDULER_TASK_GENERATION"]
    return raw


def verify_halt_log(path, smp, expected_depth):
    lines = read_lines(path)
    text = "\n".join(lines)
    cpus = {int(value) for value in re.findall(r"CPU#([0-9]+) \[halted \]", text)}
    require(cpus == set(range(smp)), "HALT_CPU_SET_INVALID",
            f"{path}: observed={sorted(cpus)} expected={list(range(smp))}")
    eflags = [int(value, 16) for value in
              re.findall(r"^eflags\s+0x([0-9a-fA-F]+)", text, re.MULTILINE)]
    require(len(eflags) == smp and all((value & 0x200) == 0 for value in eflags),
            "HALT_IF_NOT_CLEAR", str(path))
    terminal = re.findall(r"^panic_halt_secondary \+ [0-9]+ in section \.text$",
                          text, re.MULTILINE)
    require(len(terminal) == smp, "HALT_RIP_NOT_TERMINAL", str(path))
    owner = re.findall(r"^PANIC_OWNER_STATE=(0x[0-9a-fA-F]+)$",
                       text, re.MULTILINE)
    require(len(owner) == 1, "HALT_OWNER_STATE_MISSING", str(path))
    state = int(owner[0], 16)
    require((state & 0xffffffff) == expected_depth,
            "HALT_OWNER_DEPTH_MISMATCH", str(path))
    return state


def verify_qmp_cleanup(run_dir, result):
    events = []
    for number, line in enumerate(read_lines(run_dir / "qmp-events.jsonl"), 1):
        if not line.strip():
            continue
        try:
            row = json.loads(line)
        except json.JSONDecodeError as error:
            reject("QMP_JSON_INVALID", f"line={number}: {error}")
        require(isinstance(row, dict) and isinstance(row.get("host_monotonic"),
                                                     (int, float)),
                "QMP_ROW_INVALID", f"line={number}")
        events.append(row)
    require(events, "QMP_STREAM_EMPTY")
    trigger = result.get("trigger_monotonic")
    cleanup = result.get("cleanup")
    require(isinstance(trigger, (int, float)) and isinstance(cleanup, dict),
            "TIMELINE_MISSING")
    require(result.get("qmp_connected_before_trigger") is True,
            "QMP_CONNECTED_TOO_LATE")
    require(isinstance(cleanup.get("started"), (int, float)) and
            cleanup["started"] > trigger, "CLEANUP_TIMELINE_INVALID")
    shutdowns = [row for row in events
                 if row.get("message", {}).get("event") == "SHUTDOWN"]
    for row in shutdowns:
        data = row.get("message", {}).get("data", {})
        when = row["host_monotonic"]
        require(not (when < cleanup["started"] and data.get("guest") is False),
                "HOST_ACTION_BEFORE_TERMINAL_OBSERVATION")


def verify_run(run_dir, expected, candidate_hash):
    result = load_json(run_dir / "result.json")
    scenario = expected["scenario"]
    require(result.get("schema") == 1 and result.get("status") == "PASS" and
            result.get("host_return_code") == 0,
            "RUN_STATUS_INVALID", run_dir.name)
    require(result.get("scenario") == scenario and
            result.get("scenario_id") == SCENARIOS[scenario],
            "SCENARIO_ID_MISMATCH", run_dir.name)
    require(result.get("lock") == expected.get("lock", "none"),
            "LOCK_SCENARIO_MISMATCH", run_dir.name)
    require(result.get("profile") == expected["profile"],
            "PROFILE_MISMATCH", run_dir.name)
    config = result.get("configuration")
    require(config == {"SELFTEST": 1, "SELFTEST_AUTORUN": 0,
                       "ASSERT_TEST": 1, "DEBUG_ASSERT": 1,
                       "HOBBYOS_PANIC_TEST": 1},
            "DEBUG_CONFIGURATION_INVALID", run_dir.name)
    require(result.get("elf_sha256") == candidate_hash,
            "RESULT_FROM_OTHER_ELF", run_dir.name)
    launch = load_json(run_dir / "launch.json")
    require(launch.get("elf_sha256") == candidate_hash,
            "LAUNCH_FROM_OTHER_ELF", run_dir.name)
    require(launch.get("config") == config, "LAUNCH_CONFIGURATION_MISMATCH")
    for key in ("machine", "accel", "smp", "memory", "cpu"):
        require(launch.get(key) == expected["profile"][key],
                "LAUNCH_PROFILE_MISMATCH", f"{run_dir.name}:{key}")
    verify_artifacts(run_dir, result)
    serial = read_lines(run_dir / "serial-transcript.log")
    early_begin_index, _ = unique_line(
        serial, "[ASSERT_TEST][EARLY_BEGIN] ", "EARLY_BEGIN_COUNT_INVALID")
    early_end_index, early_end = unique_line(
        serial, "[ASSERT_TEST][EARLY_END] ", "EARLY_END_COUNT_INVALID")
    require(early_begin_index < early_end_index,
            "EARLY_MARKER_ORDER_INVALID")
    early_warns = [(index, line) for index, line in enumerate(serial)
                   if line.startswith("[ASSERT][WARN] ") and
                   early_begin_index < index < early_end_index]
    require(len(early_warns) == 1, "EARLY_WARNING_COUNT_INVALID")
    early = decode_hex_context(early_warns[0][1], "[ASSERT][WARN] ", True)
    require(fields(early_end, "[ASSERT_TEST][EARLY_END] ",
                   ("status", "evaluations", "continuation", "if_before",
                    "if_after", "task_expected")) == {
                        "status": "PASS", "evaluations": "3",
                        "continuation": "1", "if_before": "0",
                        "if_after": "0", "task_expected": "unknown"},
            "EARLY_RESULT_INVALID")
    require(early["fields"].get("if_enabled") == "0",
            "EARLY_WARNING_IF_INVALID")
    prepared_index, prepared = unique_line(
        serial, "[ASSERT_TEST][PREPARED] ", "PREPARED_COUNT_INVALID")
    execute_index, execute = unique_line(
        serial, "[ASSERT_TEST][EXECUTE] ", "EXECUTE_COUNT_INVALID")
    require(prepared_index < execute_index, "SCENARIO_MARKER_ORDER_INVALID")
    require(integer(fields(prepared, "[ASSERT_TEST][PREPARED] ",
                           ("scenario",))["scenario"],
                    "PREPARED_SCENARIO_INVALID") == SCENARIOS[scenario],
            "PREPARED_SCENARIO_MISMATCH")
    require(integer(fields(execute, "[ASSERT_TEST][EXECUTE] ",
                           ("scenario",))["scenario"],
                    "EXECUTE_SCENARIO_INVALID") == SCENARIOS[scenario],
            "EXECUTE_SCENARIO_MISMATCH")
    precondition = verify_precondition_observer(run_dir, result)
    observed = precondition
    context_ready = [(index, line) for index, line in enumerate(serial)
                     if line.startswith("[ASSERT_TEST][CONTEXT_READY] ")]
    if scenario == "warn" or scenario not in NONFATAL:
        require(len(context_ready) == 1, "CONTEXT_READY_COUNT_INVALID")
        require(fields(context_ready[0][1], "[ASSERT_TEST][CONTEXT_READY] ",
                       ("valid_mask",))["valid_mask"] == "7",
                "CONTEXT_READY_VALIDITY_INVALID")
        observed = verify_context_observer(run_dir, result)
    else:
        require(not context_ready, "UNEXPECTED_CONTEXT_OBSERVATION")
    before = run_dir / "fixture-before.bin"
    after = run_dir / "fixture-after.bin"
    require(before.stat().st_size == after.stat().st_size == 96,
            "FIXTURE_SIZE_INVALID", run_dir.name)
    same_fixture = before.read_bytes() == after.read_bytes()
    recorded_fixture = result.get("fixture")
    require(isinstance(recorded_fixture, dict) and
            recorded_fixture.get("before_sha256") == sha256(before) and
            recorded_fixture.get("after_sha256") == sha256(after) and
            recorded_fixture.get("unchanged") == same_fixture,
            "FIXTURE_LEDGER_MISMATCH", run_dir.name)
    if scenario in UNCHANGED:
        require(same_fixture, "REJECTED_OPERATION_MUTATED_FIXTURE", run_dir.name)
    after_values = parse_gdb_values(run_dir / "gdb-after.log")
    require(after_values.get("STATE_SCENARIO") == SCENARIOS[scenario] and
            after_values.get("STATE_ARM_ERROR") == 0,
            "STATE_SCENARIO_INVALID", run_dir.name)
    verify_qmp_cleanup(run_dir, result)
    if scenario in NONFATAL:
        complete_index, complete = unique_line(
            serial, "[ASSERT_TEST][COMPLETE] ", "COMPLETE_COUNT_INVALID")
        require(execute_index < complete_index,
                "NONFATAL_CONTINUATION_ORDER_INVALID")
        complete_fields = fields(complete, "[ASSERT_TEST][COMPLETE] ",
                                 ("scenario", "status"))
        require(complete_fields == {"scenario": str(SCENARIOS[scenario]),
                                    "status": "PASS"},
                "NONFATAL_COMPLETION_INVALID")
        require(after_values.get("STATE_COMPLETED") == 1 and
                after_values.get("STATE_RESULT") == 1,
                "NONFATAL_STATE_INCOMPLETE")
        require(not any(line.startswith("[PANIC][OWNER] ") for line in serial),
                "NONFATAL_ENTERED_PANIC")
        runtime_warns = [(index, line) for index, line in enumerate(serial)
                         if line.startswith("[ASSERT][WARN] ") and
                         execute_index < index < complete_index]
        if scenario == "warn":
            require(len(runtime_warns) == 1,
                    "WARNING_RUNTIME_COUNT_INVALID")
            require(execute_index < context_ready[0][0] < runtime_warns[0][0] <
                    complete_index, "WARNING_CONTEXT_ORDER_INVALID")
            context = decode_hex_context(runtime_warns[0][1],
                                         "[ASSERT][WARN] ", False, observed)
            require(context["fields"].get("if_enabled") == "1",
                    "WARNING_IF_NOT_ENABLED")
            require(after_values.get("STATE_EVALUATIONS") == 1 and
                    after_values.get("STATE_CONTINUATION") == 1 and
                    after_values.get("STATE_IF_BEFORE") == 1 and
                    after_values.get("STATE_IF_AFTER") == 1,
                    "WARNING_DID_NOT_CONTINUE_WITH_IF")
        elif scenario == "false":
            require(not runtime_warns, "FALSE_CONDITION_REPORTED")
            require(after_values.get("STATE_EVALUATIONS") == 2 and
                    after_values.get("STATE_CONTINUATION") == 1,
                    "FALSE_POLARITY_OR_EVALUATION_INVALID")
    else:
        require(not any(line.startswith("[ASSERT_TEST][COMPLETE] ")
                        for line in serial), "FATAL_RETURNED_TO_CALLER")
        owner_index, owner = unique_line(
            serial, "[PANIC][OWNER] ", "PANIC_OWNER_COUNT_INVALID")
        bug_index, bug = unique_line(
            serial, "[ASSERT][BUG] ", "BUG_CONTEXT_COUNT_INVALID")
        require(execute_index < owner_index < bug_index,
                "BUG_CONTEXT_BEFORE_OWNERSHIP")
        require(owner_index < context_ready[0][0] < bug_index,
                "BUG_CONTEXT_OBSERVATION_ORDER_INVALID")
        owner_fields = fields(owner, "[PANIC][OWNER] ",
                              ("origin", "action", "depth", "cpu_id",
                               "cpu_slot", "slot_valid"))
        require(owner_fields["origin"] == "assertion" and
                owner_fields["action"] == "halt" and
                owner_fields["depth"] == "1",
                "BUG_PANIC_OWNER_INVALID")
        decode_hex_context(bug, "[ASSERT][BUG] ", False, observed)
        expected_depth = 1
        if scenario == "reentry":
            expected_depth = 2
        elif scenario == "second-reentry":
            expected_depth = 3
        first_state = verify_halt_log(run_dir / "gdb-halt-1.log",
                                      expected["profile"]["smp"],
                                      expected_depth)
        second_state = verify_halt_log(run_dir / "gdb-halt-2.log",
                                       expected["profile"]["smp"],
                                       expected_depth)
        require(first_state == second_state, "HALT_OWNER_STATE_CHANGED")
        terminal = result.get("terminal_evidence")
        require(isinstance(terminal, dict), "TERMINAL_EVIDENCE_MISSING")
        first_time = terminal.get("first", {}).get("host_monotonic")
        second_time = terminal.get("second", {}).get("host_monotonic")
        trigger = result.get("trigger_monotonic")
        cleanup = result.get("cleanup", {}).get("started")
        require(all(isinstance(value, (int, float)) for value in
                    (first_time, second_time, trigger, cleanup)),
                "TERMINAL_TIMELINE_MISSING")
        require(trigger < first_time < second_time < cleanup,
                "TERMINAL_EVENT_OUTSIDE_TRIGGER_WINDOW")
        require(second_time - first_time >= 0.2,
                "HALT_EXECUTION_GAP_TOO_SHORT")
        if scenario in ("reentry", "second-reentry"):
            reentries = [line for line in serial
                         if line.startswith("[PANIC][REENTRY] ")]
            require(len(reentries) == 1, "REENTRY_MARKER_COUNT_INVALID")
            require(fields(reentries[0], "[PANIC][REENTRY] ",
                           ("depth",))["depth"] == "2",
                    "REENTRY_MINIMAL_DEPTH_INVALID")
            if scenario == "second-reentry":
                require(not any("depth=3" in line for line in serial),
                        "THIRD_ENTRY_WAS_NOT_SILENT")
        else:
            dump_end_index, _ = unique_line(
                serial, "[PANIC][DUMP_END] ", "DUMP_END_COUNT_INVALID")
            action_index, action = unique_line(
                serial, "[PANIC][ACTION] ", "PANIC_ACTION_COUNT_INVALID")
            require(bug_index < dump_end_index < action_index,
                    "PANIC_ACTION_ORDER_INVALID")
            require(fields(action, "[PANIC][ACTION] ",
                           ("requested", "terminal", "if")) == {
                               "requested": "halt", "terminal": "halt",
                               "if": "0"}, "PANIC_ACTION_INVALID")
        if scenario == "locked-bug":
            require(after_values.get("STATE_LOCK_ACQUIRED") == 1 and
                    after_values.get("ASSERT_LOCK_AFTER") == 1,
                    "REAL_LOCK_NOT_RETAINED")
            lock_lines = [line for line in serial
                          if line == "[ASSERT_TEST][LOCK_HELD] acquired=1"]
            require(len(lock_lines) == 1, "REAL_LOCK_ACK_MISSING")
    return {"scenario": scenario, "profile": expected["profile"]}


def verify_host(root):
    summary = load_json(root / "host" / "summary.json")
    require(summary.get("schema") == 1 and summary.get("status") == "PASS",
            "HOST_SUMMARY_INVALID")
    expected = {"normal": 1, "ubsan": 1, "asan": 1, "disabled": 0}
    profiles = summary.get("profiles")
    require(isinstance(profiles, dict) and set(profiles) == set(expected),
            "HOST_PROFILE_SET_INVALID")
    for name, debug in expected.items():
        entry = profiles[name]
        run = root / "host" / name
        log = run / "run.log"
        require_file(log, entry.get("log_sha256"))
        require((run / "exit-code.txt").read_text().strip() == "0",
                "HOST_EXIT_NONZERO", name)
        lines = read_lines(log)
        cases = [fields(line, "[ASSERT_HOST][CASE] ", ("id", "status"))
                 for line in lines if line.startswith("[ASSERT_HOST][CASE] ")]
        expected_cases = HOST_CASES if debug else HOST_CASES - {
            "rejected-before-store"}
        require({case["id"] for case in cases} == expected_cases and
                len(cases) == len(expected_cases) and
                all(case["status"] == "PASS" for case in cases),
                "HOST_CASE_SET_INVALID", name)
        _, suite = unique_line(lines, "[ASSERT_HOST][SUITE] ",
                               "HOST_SUITE_COUNT_INVALID")
        suite_fields = fields(suite, "[ASSERT_HOST][SUITE] ",
                              ("status", "assertions", "failures", "seed",
                               "sequences", "debug"))
        require(suite_fields["status"] == "PASS" and
                suite_fields["failures"] == "0" and
                suite_fields["seed"] == "0x6173736572746c69" and
                suite_fields["sequences"] == "2048" and
                suite_fields["debug"] == str(debug) and
                integer(suite_fields["assertions"],
                        "HOST_ASSERTION_COUNT_INVALID") > 1000,
                "HOST_SUITE_INVALID", name)
        text = "\n".join(lines)
        require("runtime error:" not in text and
                "ERROR: AddressSanitizer" not in text,
                "HOST_SANITIZER_ERROR", name)
    adversarial = summary.get("adversarial")
    require(isinstance(adversarial, dict) and
            adversarial.get("status") == "NEGATIVE_DETECTED" and
            adversarial.get("exit_code") not in (None, 0),
            "ADVERSARIAL_CONTROL_NOT_DETECTED")
    adversarial_log = root / "host" / "adversarial" / "run.log"
    require_file(adversarial_log, adversarial.get("log_sha256"))
    require("broken previous reciprocity accepted" in
            adversarial_log.read_text(errors="replace"),
            "ADVERSARIAL_FAILURE_REASON_MISSING")
    disabled = root / "host" / "disabled-object"
    require((disabled / "helper-references.txt").is_file() and
            not (disabled / "helper-references.txt").read_text().strip(),
            "DISABLED_OBJECT_HAS_ASSERT_HELPER")
    require((disabled / "callsite-strings.txt").is_file() and
            not (disabled / "callsite-strings.txt").read_text().strip(),
            "DISABLED_OBJECT_HAS_CALLSITE")


def verify_build(root, candidate_hash, image_hash):
    result = load_json(root / "build" / "result.json")
    require(result.get("schema") == 1 and result.get("status") == "PASS",
            "BUILD_RESULT_INVALID")
    require(result.get("kernel_check_exit") == 0 and
            result.get("stack_check_exit") == 0 and
            result.get("assert_stack_check_exit") == 0,
            "BUILD_GATE_EXIT_INVALID")
    frame = result.get("max_automatic_frame")
    require(isinstance(frame, int) and 0 < frame <= 2048,
            "STACK_FRAME_LIMIT_INVALID", str(frame))
    require(result.get("candidate_elf_sha256") == candidate_hash and
            result.get("candidate_image_sha256") == image_hash,
            "BUILD_CANDIDATE_HASH_MISMATCH")
    require((root / "build" / "instrumented-undefined.txt").is_file() and
            not (root / "build" / "instrumented-undefined.txt").read_text().strip(),
            "INSTRUMENTED_UNDEFINED_SYMBOLS")
    require("kernel-check: PASS" in
            (root / "build" / "kernel-check-command.log").read_text(errors="replace"),
            "KERNEL_CHECK_MARKER_MISSING")
    require("stack-check: PASS" in
            (root / "build" / "stack-check-command.log").read_text(errors="replace"),
            "STACK_CHECK_MARKER_MISSING")


def symbol_names(path):
    try:
        output = subprocess.check_output(["nm", "-a", str(path)], text=True,
                                         stderr=subprocess.STDOUT)
    except (OSError, subprocess.CalledProcessError) as error:
        reject("ELF_SYMBOL_READ_FAILED", f"{path}: {error}")
    return {line.split()[-1] for line in output.splitlines() if line.split()}


def verify_production(root):
    result = load_json(root / "production" / "result.json")
    require(result.get("schema") == 1 and result.get("status") == "PASS",
            "PRODUCTION_RESULT_INVALID")
    on = root / "production" / "debug-on" / "kernel.elf"
    off = root / "production" / "debug-off" / "kernel.elf"
    final = root / "production" / "final" / "kernel.elf"
    for path in (on, off, final):
        require_file(path)
    require(result.get("debug_on_sha256") == sha256(on) and
            result.get("debug_off_sha256") == sha256(off) and
            result.get("final_sha256") == sha256(final),
            "PRODUCTION_HASH_LEDGER_MISMATCH")
    require(on.read_bytes() != off.read_bytes(),
            "PAIRED_BUILD_NO_BINARY_DIFFERENCE")
    require(final.read_bytes() == off.read_bytes(),
            "PRODUCTION_NOT_DEBUG_OFF_BINARY")
    on_symbols = symbol_names(on)
    off_symbols = symbol_names(off)
    require({"assertion_warn_report", "assertion_bug_report"} <= on_symbols,
            "DEBUG_BUILD_ASSERT_HELPERS_MISSING")
    forbidden = {"assertion_warn_report", "assertion_bug_report",
                 "assertion_test_timer_hook", "g_assertion_test_state",
                 "g_assertion_test_fixture"}
    require(not (forbidden & off_symbols),
            "PRODUCTION_ASSERT_HELPER_PRESENT",
            ",".join(sorted(forbidden & off_symbols)))
    try:
        off_strings = subprocess.check_output(["strings", str(off)], text=True,
                                              stderr=subprocess.STDOUT)
        on_strings = subprocess.check_output(["strings", str(on)], text=True,
                                             stderr=subprocess.STDOUT)
    except (OSError, subprocess.CalledProcessError) as error:
        reject("ELF_STRING_READ_FAILED", str(error))
    callsites = ("new_node == NULL", "prev->next != next",
                 "[ASSERT][WARN]", "[ASSERT][BUG]")
    require(any(item in on_strings for item in callsites),
            "DEBUG_BUILD_CALLSITE_TEXT_MISSING")
    require(not any(item in off_strings for item in callsites),
            "PRODUCTION_ASSERT_CALLSITE_PRESENT")
    section = result.get("section_comparison")
    require(isinstance(section, dict) and section.get("status") == "PASS" and
            isinstance(section.get("debug_on_allocated_bytes"), int) and
            isinstance(section.get("debug_off_allocated_bytes"), int) and
            section["debug_on_allocated_bytes"] >
            section["debug_off_allocated_bytes"],
            "PAIRED_SECTION_COMPARISON_INVALID")
    final_dir = root / "production" / "final"
    for name in ("hobbyos.img", "BOOTX64.EFI"):
        require_file(final_dir / name,
                     result.get("final_artifacts", {}).get(name, {}).get(
                         "sha256"))
    payloads = final_dir / "payloads"
    for name in ("kernel.elf", "BOOTX64.EFI", "zap-light16.psf", "logo.bmp",
                 "startup.nsh"):
        receipt = result.get("payloads", {}).get(name, {})
        require_file(payloads / name, receipt.get("sha256"))
    require((payloads / "kernel.elf").read_bytes() == final.read_bytes(),
            "PRODUCTION_IMAGE_KERNEL_MISMATCH")


def verify_all(root):
    root = root.resolve()
    require(root.is_dir() and not root.is_symlink(),
            "EVIDENCE_ROOT_INVALID", str(root))
    campaign = load_json(root / "campaign.json")
    require(campaign.get("schema") == 1 and campaign.get("status") == "PASS",
            "CAMPAIGN_STATUS_INVALID")
    candidate = campaign.get("candidate")
    require(isinstance(candidate, dict), "CANDIDATE_LEDGER_MISSING")
    elf = root / candidate.get("elf_path", "")
    image = root / candidate.get("image_path", "")
    require_file(elf, candidate.get("elf_sha256"))
    require_file(image, candidate.get("image_sha256"))
    verify_build(root, candidate["elf_sha256"], candidate["image_sha256"])
    planned = campaign.get("planned_runs")
    require(isinstance(planned, list) and planned,
            "PLANNED_RUNS_MISSING")
    names = [row.get("name") for row in planned if isinstance(row, dict)]
    require(len(names) == len(planned) and all(isinstance(name, str) and name
                                               for name in names),
            "PLANNED_RUN_NAME_INVALID")
    require(len(set(names)) == len(names), "DUPLICATE_PLANNED_CASE")
    runtime = root / "runtime"
    actual = sorted(path.name for path in runtime.iterdir()
                    if path.is_dir() and not path.is_symlink())
    require(sorted(names) == actual, "RUNTIME_CASE_SET_MISMATCH",
            f"planned={len(names)} actual={len(actual)}")
    seen_keys = set()
    coverage = {scenario: 0 for scenario in SCENARIOS}
    profiles = set()
    for row in planned:
        require(row.get("scenario") in SCENARIOS,
                "PLANNED_SCENARIO_INVALID", str(row.get("scenario")))
        profile = row.get("profile")
        require(isinstance(profile, dict) and set(profile) == {
            "machine", "accel", "smp", "memory", "cpu"},
            "PLANNED_PROFILE_INVALID", row["name"])
        key = (profile["machine"], profile["accel"], profile["smp"],
               row["scenario"], row.get("lock", "none"))
        require(key not in seen_keys, "DUPLICATE_RUNTIME_CASE", row["name"])
        seen_keys.add(key)
        profiles.add((profile["machine"], profile["accel"], profile["smp"]))
        try:
            verify_run(runtime / row["name"], row, candidate["elf_sha256"])
        except EvidenceError as error:
            code = str(error).split(":", 1)[0]
            reject(code, f"run={row['name']} detail={error}")
        coverage[row["scenario"]] += 1
    require(all(value > 0 for value in coverage.values()),
            "SCENARIO_COVERAGE_INCOMPLETE",
            ",".join(name for name, value in coverage.items() if value == 0))
    require({("q35", "tcg", 4), ("q35", "kvm", 4),
             ("q35", "tcg", 1), ("q35", "kvm", 24)} <= profiles,
            "PROFILE_COVERAGE_INCOMPLETE")
    central = {(row["scenario"], row.get("lock", "none"),
                row["profile"]["accel"]) for row in planned
               if row["profile"]["smp"] == 4}
    for accel in ("tcg", "kvm"):
        for scenario in ("warn", "bug", "double-insert",
                         "insert-reciprocity", "remove-reciprocity"):
            require((scenario, "none", accel) in central,
                    "CENTRAL_MATRIX_INCOMPLETE", f"{accel}:{scenario}")
    require(any(row["profile"]["smp"] == 24 and
                row["scenario"] == "locked-bug" and
                row.get("lock") == "scheduler" for row in planned),
            "SMP24_LOCK_CONTENTION_MISSING")
    verify_host(root)
    verify_production(root)
    return {"runs": len(planned), "profiles": len(profiles),
            "coverage": coverage}


def compile_fixture_elf(path, enabled):
    source = path.with_suffix(".c")
    body = ["int main(void){return 0;}"]
    if enabled:
        body += [
            "void assertion_warn_report(void){}",
            "void assertion_bug_report(void){}",
            "const char *assert_text=\"[ASSERT][BUG] new_node == NULL\";",
        ]
    source.write_text("\n".join(body) + "\n")
    subprocess.run(["gcc", "-g", "-o", str(path), str(source)], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


def receipt(path):
    return {"bytes": path.stat().st_size, "sha256": sha256(path)}


def synthetic_run(root, row, candidate_hash, counter):
    run = root / "runtime" / row["name"]
    run.mkdir(parents=True)
    scenario = row["scenario"]
    scenario_id = SCENARIOS[scenario]
    cpu = min(row["profile"]["smp"] - 1, counter % row["profile"]["smp"])
    task = 5 + counter
    early_file = "kernel/src/core/assert_selftest.c".encode().hex()
    early_expr = "++evaluations == 3u".encode().hex()
    runtime_file = ("kernel/src/core/list.h" if scenario in {
        "double-insert", "insert-reciprocity", "remove-reciprocity"}
                    else "kernel/src/core/assert_selftest.c")
    runtime_expr = ("prev->next != next" if scenario in {
        "insert-reciprocity", "remove-reciprocity"}
                    else "scenario != ASSERTION_TEST_SCENARIO_NONE")
    lines = [
        "[ASSERT_TEST][EARLY_BEGIN] debug=1 stage=post-idt",
        f"[ASSERT][WARN] file_hex={early_file} file_bytes={len(bytes.fromhex(early_file))} file_valid=1 file_truncated=0 line=67 expression_hex={early_expr} expression_bytes={len(bytes.fromhex(early_expr))} expression_valid=1 expression_truncated=0 cpu_id=0 cpu_valid=1 cpu_slot=4294967295 slot_valid=0 task_id=0 task_generation=0 task_valid=0 if_enabled=0",
        "[ASSERT_TEST][EARLY_END] status=PASS evaluations=3 continuation=1 if_before=0 if_after=0 task_expected=unknown",
        "[BOOT][RUNTIME_READY] PASS",
        f"[ASSERT_TEST][PREPARED] scenario={scenario_id} slot={cpu} cpu_id={cpu}",
        f"[ASSERT_TEST][EXECUTE] scenario={scenario_id}",
    ]
    if scenario == "warn":
        lines.append("[ASSERT_TEST][CONTEXT_READY] valid_mask=7")
        fhex = runtime_file.encode().hex()
        ehex = "++evaluations == 1u".encode().hex()
        lines.append(f"[ASSERT][WARN] file_hex={fhex} file_bytes={len(runtime_file.encode())} file_valid=1 file_truncated=0 line=187 expression_hex={ehex} expression_bytes={len(bytes.fromhex(ehex))} expression_valid=1 expression_truncated=0 cpu_id={cpu} cpu_valid=1 cpu_slot={cpu} slot_valid=1 task_id={task} task_generation={task} task_valid=1 if_enabled=1")
    if scenario in NONFATAL:
        lines.append(f"[ASSERT_TEST][COMPLETE] scenario={scenario_id} status=PASS")
    else:
        lines.append(f"[PANIC][OWNER] cpu_id={cpu} cpu_slot={cpu} slot_valid=1 identity=cpuid-0b depth=1 origin=assertion stage=owner-established action=halt timeout_s=1")
        lines.append("[ASSERT_TEST][CONTEXT_READY] valid_mask=7")
        fhex = runtime_file.encode().hex()
        ehex = runtime_expr.encode().hex()
        lines.append(f"[ASSERT][BUG] file_hex={fhex} file_bytes={len(runtime_file.encode())} file_valid=1 file_truncated=0 line=211 expression_hex={ehex} expression_bytes={len(runtime_expr.encode())} expression_valid=1 expression_truncated=0 cpu_id={cpu} cpu_valid=1 cpu_slot={cpu} slot_valid=1 task_id={task} task_generation={task} task_valid=1")
        if scenario in ("reentry", "second-reentry"):
            lines.append(f"[PANIC][REENTRY] cpu_id={cpu} cpu_slot=unknown depth=2 vector_known=1 vector=6 path=minimal action=halt")
        else:
            if scenario == "locked-bug":
                lines.insert(-2, "[ASSERT_TEST][LOCK_HELD] acquired=1")
            lines += ["[PANIC][DUMP_END] status=complete channel=serial",
                      "[PANIC][ACTION] requested=halt terminal=halt if=0"]
    serial = run / "serial-transcript.log"
    serial.write_text("\n".join(lines) + "\n")
    before = bytes((index + counter) & 0xff for index in range(96))
    (run / "fixture-before.bin").write_bytes(before)
    (run / "fixture-after.bin").write_bytes(before)
    (run / "gdb-arm.log").write_text(
        f"ASSERT_ARMED=1\nASSERT_SCENARIO={scenario_id}\n")
    lock_address = 0x1000 if scenario == "locked-bug" else 0
    (run / "gdb-observer.log").write_text(
        f"STATE_OBSERVED_SLOT={cpu}\nSTATE_OBSERVED_CPU_ID={cpu}\n"
        f"TOPOLOGY_CPU_ID={cpu}\nSTATE_TASK_ID={task}\n"
        f"STATE_TASK_GENERATION={task}\nSCHEDULER_TASK_ID={task}\n"
        f"SCHEDULER_TASK_GENERATION={task}\nASSERT_LOCK_ADDR=0x{lock_address:x}\n"
        "OBSERVER_RELEASED=1\n")
    if scenario == "warn" or scenario not in NONFATAL:
        (run / "gdb-context-observer.log").write_text(
            f"CONTEXT_STATE_CPU_ID={cpu}\nCONTEXT_STATE_SLOT={cpu}\n"
            "CONTEXT_VALID_MASK=7\n"
            f"CONTEXT_STATE_TASK_ID={task}\n"
            f"CONTEXT_STATE_TASK_GENERATION={task}\n"
            f"CONTEXT_TOPOLOGY_CPU_ID={cpu}\n"
            f"CONTEXT_SCHEDULER_TASK_ID={task}\n"
            f"CONTEXT_SCHEDULER_TASK_GENERATION={task}\n"
            "CONTEXT_RELEASED=1\n")
    after_values = {
        "STATE_SCENARIO": scenario_id, "STATE_ARM_ERROR": 0,
        "STATE_COMPLETED": 1 if scenario in NONFATAL else 0,
        "STATE_RESULT": 1 if scenario in NONFATAL else 0,
        "STATE_EVALUATIONS": 1 if scenario == "warn" else
                             (2 if scenario == "false" else 0),
        "STATE_CONTINUATION": 1 if scenario in ("warn", "false") else 0,
        "STATE_IF_BEFORE": 1 if scenario == "warn" else 0,
        "STATE_IF_AFTER": 1 if scenario == "warn" else 0,
        "STATE_LOCK_ACQUIRED": 1 if scenario == "locked-bug" else 0,
        "ASSERT_LOCK_AFTER": 1 if scenario == "locked-bug" else 0,
    }
    (run / "gdb-after.log").write_text("".join(
        f"{key}={value}\n" for key, value in after_values.items()))
    trigger = 1000.0 + counter * 10
    cleanup = trigger + 3.0
    if scenario not in NONFATAL:
        depth = 2 if scenario == "reentry" else (
            3 if scenario == "second-reentry" else 1)
        owner_state = ((cpu + 1) << 32) | depth
        halt_text = "".join(
            f"  {index + 1} Thread 1.{index + 1} (CPU#{index} [halted ]) panic_halt_secondary\n"
            f"eflags         0x2 [ IOPL=0 ]\n"
            f"panic_halt_secondary + 10 in section .text\n"
            for index in range(row["profile"]["smp"]))
        halt_text += f"PANIC_OWNER_STATE=0x{owner_state:x}\n"
        (run / "gdb-halt-1.log").write_text(halt_text)
        (run / "gdb-halt-2.log").write_text(halt_text)
    qmp = [
        {"host_monotonic": trigger - 1,
         "message": {"QMP": {"version": {"qemu": {"major": 8}}}}},
        {"host_monotonic": cleanup + 0.01,
         "message": {"event": "SHUTDOWN",
                     "data": {"guest": False, "reason": "host-qmp-quit"}}},
    ]
    (run / "qmp-events.jsonl").write_text("".join(
        json.dumps(item) + "\n" for item in qmp))
    launch = {
        "schema": 1, "elf_sha256": candidate_hash,
        "config": {"SELFTEST": 1, "SELFTEST_AUTORUN": 0,
                   "ASSERT_TEST": 1, "DEBUG_ASSERT": 1,
                   "HOBBYOS_PANIC_TEST": 1}, **row["profile"],
    }
    write_json(run / "launch.json", launch)
    artifacts = {path.name: receipt(path) for path in run.iterdir()
                 if path.is_file() and path.name != "result.json"}
    observed = {
        "state_slot": cpu, "state_cpu_id": cpu, "topology_cpu_id": cpu,
        "state_task_id": task, "state_task_generation": task,
        "scheduler_task_id": task, "scheduler_task_generation": task,
        "lock_address": lock_address,
    }
    context_observed = {
        "state_slot": cpu, "state_cpu_id": cpu, "valid_mask": 7,
        "state_task_id": task, "state_task_generation": task,
        "topology_cpu_id": cpu, "scheduler_task_id": task,
        "scheduler_task_generation": task,
    }
    result = {
        "schema": 1, "status": "PASS", "host_return_code": 0,
        "scenario": scenario, "scenario_id": scenario_id,
        "lock": row.get("lock", "none"), "profile": row["profile"],
        "configuration": launch["config"], "elf_sha256": candidate_hash,
        "qmp_connected_before_trigger": True, "trigger_monotonic": trigger,
        "cleanup": {"started": cleanup, "completed": cleanup + 0.1,
                    "method": "qmp-quit"},
        "precondition_observation": observed,
        "independent_observation": (context_observed if scenario == "warn" or
                                    scenario not in NONFATAL else observed),
        "fixture": {"bytes": 96, "before_sha256": sha256(run / "fixture-before.bin"),
                    "after_sha256": sha256(run / "fixture-after.bin"),
                    "unchanged": True}, "artifacts": artifacts,
    }
    if scenario not in NONFATAL:
        result["terminal_evidence"] = {
            "first": {"host_monotonic": trigger + 1.0},
            "second": {"host_monotonic": trigger + 1.3},
            "expected_depth": 2 if scenario == "reentry" else
                              (3 if scenario == "second-reentry" else 1),
        }
    write_json(run / "result.json", result)


def create_synthetic(root):
    root.mkdir(parents=True)
    candidate = root / "builds" / "instrumented"
    candidate.mkdir(parents=True)
    compile_fixture_elf(candidate / "kernel.elf", True)
    (candidate / "hobbyos.img").write_bytes(b"synthetic image")
    planned = []
    profiles = [
        ("q35", "tcg", 4, "max"), ("q35", "kvm", 4, "host"),
    ]
    counter = 0
    for machine, accel, smp, cpu in profiles:
        for scenario in SCENARIOS:
            lock = "heap" if scenario == "locked-bug" else "none"
            name = f"{machine}-{accel}-smp{smp}-{scenario}-{lock}"
            planned.append({"name": name, "scenario": scenario, "lock": lock,
                            "profile": {"machine": machine, "accel": accel,
                                        "smp": smp, "memory": "2G",
                                        "cpu": cpu}})
    for machine, accel, smp, cpu, scenario, lock in [
            ("q35", "tcg", 1, "max", "bug", "none"),
            ("q35", "kvm", 24, "host", "warn", "none"),
            ("q35", "kvm", 24, "host", "valid-list", "none"),
            ("q35", "kvm", 24, "host", "locked-bug", "scheduler")]:
        name = f"{machine}-{accel}-smp{smp}-{scenario}-{lock}"
        planned.append({"name": name, "scenario": scenario, "lock": lock,
                        "profile": {"machine": machine, "accel": accel,
                                    "smp": smp, "memory": "2G", "cpu": cpu}})
    candidate_hash = sha256(candidate / "kernel.elf")
    build = root / "build"
    build.mkdir()
    (build / "instrumented-undefined.txt").write_text("")
    (build / "kernel-check-command.log").write_text("kernel-check: PASS\n")
    (build / "stack-check-command.log").write_text("stack-check: PASS\n")
    write_json(build / "result.json", {
        "schema": 1, "status": "PASS", "kernel_check_exit": 0,
        "stack_check_exit": 0, "assert_stack_check_exit": 0,
        "max_automatic_frame": 1024,
        "candidate_elf_sha256": candidate_hash,
        "candidate_image_sha256": sha256(candidate / "hobbyos.img"),
    })
    for row in planned:
        synthetic_run(root, row, candidate_hash, counter)
        counter += 1
    host = root / "host"
    profiles_json = {}
    for name, debug in (("normal", 1), ("ubsan", 1), ("asan", 1),
                        ("disabled", 0)):
        run = host / name
        run.mkdir(parents=True)
        cases = HOST_CASES if debug else HOST_CASES - {"rejected-before-store"}
        text = "".join(f"[ASSERT_HOST][CASE] id={case} status=PASS assertions=1\n"
                       for case in sorted(cases))
        text += (f"[ASSERT_HOST][SUITE] status=PASS assertions=2000 failures=0 "
                 f"seed=0x6173736572746c69 sequences=2048 debug={debug}\n")
        (run / "run.log").write_text(text)
        (run / "exit-code.txt").write_text("0\n")
        profiles_json[name] = {"log_sha256": sha256(run / "run.log")}
    adversarial = host / "adversarial"
    adversarial.mkdir(parents=True)
    (adversarial / "run.log").write_text(
        "[ASSERT_HOST][FAIL] broken previous reciprocity accepted\n")
    disabled = host / "disabled-object"
    disabled.mkdir(parents=True)
    (disabled / "helper-references.txt").write_text("")
    (disabled / "callsite-strings.txt").write_text("")
    write_json(host / "summary.json", {
        "schema": 1, "status": "PASS", "profiles": profiles_json,
        "adversarial": {"status": "NEGATIVE_DETECTED", "exit_code": 1,
                        "log_sha256": sha256(adversarial / "run.log")},
    })
    production = root / "production"
    for name, enabled in (("debug-on", True), ("debug-off", False)):
        directory = production / name
        directory.mkdir(parents=True)
        compile_fixture_elf(directory / "kernel.elf", enabled)
    final = production / "final"
    final.mkdir(parents=True)
    shutil.copy2(production / "debug-off" / "kernel.elf", final / "kernel.elf")
    (final / "hobbyos.img").write_bytes(b"production image")
    (final / "BOOTX64.EFI").write_bytes(b"efi")
    payloads = final / "payloads"
    payloads.mkdir()
    shutil.copy2(final / "kernel.elf", payloads / "kernel.elf")
    shutil.copy2(final / "BOOTX64.EFI", payloads / "BOOTX64.EFI")
    for name in ("zap-light16.psf", "logo.bmp", "startup.nsh"):
        (payloads / name).write_bytes(name.encode())
    write_json(production / "result.json", {
        "schema": 1, "status": "PASS",
        "debug_on_sha256": sha256(production / "debug-on" / "kernel.elf"),
        "debug_off_sha256": sha256(production / "debug-off" / "kernel.elf"),
        "final_sha256": sha256(final / "kernel.elf"),
        "section_comparison": {"status": "PASS",
                               "debug_on_allocated_bytes": 200,
                               "debug_off_allocated_bytes": 100},
        "final_artifacts": {name: receipt(final / name) for name in
                            ("hobbyos.img", "BOOTX64.EFI")},
        "payloads": {name: receipt(payloads / name) for name in
                     ("kernel.elf", "BOOTX64.EFI", "zap-light16.psf",
                      "logo.bmp", "startup.nsh")},
    })
    write_json(root / "campaign.json", {
        "schema": 1, "status": "PASS", "planned_runs": planned,
        "candidate": {"elf_path": "builds/instrumented/kernel.elf",
                      "image_path": "builds/instrumented/hobbyos.img",
                      "elf_sha256": candidate_hash,
                      "image_sha256": sha256(candidate / "hobbyos.img")},
    })


def refresh_artifact(run, name):
    result = load_json(run / "result.json")
    result["artifacts"][name] = receipt(run / name)
    if name == "fixture-before.bin":
        result["fixture"]["before_sha256"] = sha256(run / name)
        result["fixture"]["unchanged"] = ((run / "fixture-before.bin").read_bytes() ==
                                           (run / "fixture-after.bin").read_bytes())
    if name == "fixture-after.bin":
        result["fixture"]["after_sha256"] = sha256(run / name)
        result["fixture"]["unchanged"] = ((run / "fixture-before.bin").read_bytes() ==
                                           (run / "fixture-after.bin").read_bytes())
    write_json(run / "result.json", result)


def fixtures(output):
    output = output.resolve()
    require(not output.exists(), "FIXTURE_OUTPUT_EXISTS", str(output))
    output.mkdir(parents=True)
    records = []
    with tempfile.TemporaryDirectory(prefix="hobbyos-assert-fixtures-") as temp:
        base = Path(temp) / "base"
        create_synthetic(base)
        verify_all(base)
        records.append({"id": "valid-control", "expected": "PASS",
                        "observed": "PASS"})
        campaign = load_json(base / "campaign.json")
        by_scenario = {}
        for row in campaign["planned_runs"]:
            by_scenario.setdefault(row["scenario"], base / "runtime" / row["name"])

        def run_negative(identifier, expected_code, mutate):
            case = Path(temp) / identifier
            shutil.copytree(base, case)
            mutate(case)
            try:
                verify_all(case)
            except EvidenceError as error:
                observed = str(error).split(":", 1)[0]
                require(observed == expected_code, "FIXTURE_WRONG_REJECTION",
                        f"{identifier}: expected={expected_code} observed={error}")
                records.append({"id": identifier, "expected": expected_code,
                                "observed": str(error), "status": "REJECTED"})
            else:
                reject("FIXTURE_ACCEPTED_INVALID", identifier)

        def run_for(case, scenario):
            camp = load_json(case / "campaign.json")
            row = next(item for item in camp["planned_runs"]
                       if item["scenario"] == scenario)
            return case / "runtime" / row["name"], camp, row

        run_negative("missing-case", "RUNTIME_CASE_SET_MISMATCH",
                     lambda case: shutil.rmtree(run_for(case, "valid-list")[0]))

        def duplicate(case):
            camp = load_json(case / "campaign.json")
            camp["planned_runs"].append(copy.deepcopy(camp["planned_runs"][0]))
            write_json(case / "campaign.json", camp)
        run_negative("duplicate-case", "DUPLICATE_PLANNED_CASE", duplicate)

        def warn_no_continue(case):
            run, _, _ = run_for(case, "warn")
            text = (run / "serial-transcript.log").read_text().replace(
                f"[ASSERT_TEST][COMPLETE] scenario={SCENARIOS['warn']} status=PASS\n", "")
            (run / "serial-transcript.log").write_text(text)
            refresh_artifact(run, "serial-transcript.log")
        run_negative("warning-no-continuation", "COMPLETE_COUNT_INVALID",
                     warn_no_continue)

        def inverted(case):
            run, _, _ = run_for(case, "false")
            text = (run / "serial-transcript.log").read_text().replace(
                "[ASSERT_TEST][COMPLETE]", "[ASSERT][WARN] file_hex=00\n[ASSERT_TEST][COMPLETE]")
            (run / "serial-transcript.log").write_text(text)
            refresh_artifact(run, "serial-transcript.log")
        run_negative("inverted-polarity", "FALSE_CONDITION_REPORTED", inverted)

        def context_missing(case):
            run, _, _ = run_for(case, "bug")
            text = (run / "serial-transcript.log").read_text().replace(
                " expression_hex=", " expression_removed=", 1)
            (run / "serial-transcript.log").write_text(text)
            refresh_artifact(run, "serial-transcript.log")
        run_negative("callsite-field-missing", "MISSING_FIELD", context_missing)

        def identity_mismatch(case):
            run, _, _ = run_for(case, "warn")
            text = (run / "gdb-context-observer.log").read_text().replace(
                "CONTEXT_TOPOLOGY_CPU_ID=0",
                "CONTEXT_TOPOLOGY_CPU_ID=30", 1)
            (run / "gdb-context-observer.log").write_text(text)
            refresh_artifact(run, "gdb-context-observer.log")
        run_negative("cpu-task-mismatch", "OBSERVER_CPU_MISMATCH",
                     identity_mismatch)

        def mutate_fixture(case):
            run, _, _ = run_for(case, "double-insert")
            data = bytearray((run / "fixture-after.bin").read_bytes())
            data[0] ^= 0xff
            (run / "fixture-after.bin").write_bytes(data)
            refresh_artifact(run, "fixture-after.bin")
        run_negative("mutation-after-rejection",
                     "REJECTED_OPERATION_MUTATED_FIXTURE", mutate_fixture)

        def action_only(case):
            run, _, _ = run_for(case, "bug")
            (run / "gdb-halt-1.log").write_text("action announced only\n")
            refresh_artifact(run, "gdb-halt-1.log")
        run_negative("action-marker-only", "HALT_CPU_SET_INVALID", action_only)

        def old_event(case):
            run, _, _ = run_for(case, "bug")
            result = load_json(run / "result.json")
            result["terminal_evidence"]["first"]["host_monotonic"] = (
                result["trigger_monotonic"] - 1)
            write_json(run / "result.json", result)
        run_negative("terminal-event-before-trigger",
                     "TERMINAL_EVENT_OUTSIDE_TRIGGER_WINDOW", old_event)

        def host_cleanup(case):
            run, _, _ = run_for(case, "bug")
            for name in ("gdb-halt-1.log", "gdb-halt-2.log"):
                (run / name).write_text("host QMP shutdown is not guest halt\n")
                refresh_artifact(run, name)
        run_negative("host-cleanup-as-terminal", "HALT_CPU_SET_INVALID",
                     host_cleanup)

        def debug_off_runtime(case):
            run, _, _ = run_for(case, "warn")
            launch = load_json(run / "launch.json")
            launch["config"]["DEBUG_ASSERT"] = 0
            write_json(run / "launch.json", launch)
            refresh_artifact(run, "launch.json")
        run_negative("debug-disabled-runtime", "LAUNCH_CONFIGURATION_MISMATCH",
                     debug_off_runtime)

        def production_helper(case):
            source = case / "production" / "debug-off" / "kernel.c"
            source.write_text("int main(void){return 0;}\nvoid assertion_bug_report(void){}\n")
            subprocess.run(["gcc", "-o", str(source.with_suffix(".elf")),
                            str(source)], check=True, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
            off = case / "production" / "debug-off" / "kernel.elf"
            final = case / "production" / "final" / "kernel.elf"
            payload = case / "production" / "final" / "payloads" / "kernel.elf"
            shutil.copy2(off, final)
            shutil.copy2(off, payload)
            result = load_json(case / "production" / "result.json")
            result["debug_off_sha256"] = sha256(off)
            result["final_sha256"] = sha256(final)
            result["payloads"]["kernel.elf"] = receipt(payload)
            write_json(case / "production" / "result.json", result)
        run_negative("production-helper-active",
                     "PRODUCTION_ASSERT_HELPER_PRESENT", production_helper)

        def other_elf(case):
            run, _, _ = run_for(case, "warn")
            launch = load_json(run / "launch.json")
            launch["elf_sha256"] = "0" * 64
            write_json(run / "launch.json", launch)
            refresh_artifact(run, "launch.json")
        run_negative("result-from-other-elf", "LAUNCH_FROM_OTHER_ELF", other_elf)

        def adversarial_accepted(case):
            summary = load_json(case / "host" / "summary.json")
            summary["adversarial"]["status"] = "PASS"
            summary["adversarial"]["exit_code"] = 0
            write_json(case / "host" / "summary.json", summary)
        run_negative("adversarial-checks-removed",
                     "ADVERSARIAL_CONTROL_NOT_DETECTED", adversarial_accepted)

    write_json(output / "results.json", {
        "schema": 1, "status": "PASS", "valid_controls": 1,
        "negative_controls": len(records) - 1, "records": records,
    })
    print(f"ASSERT_FIXTURES: PASS controls=1 negatives={len(records) - 1}")


def main():
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    all_parser = subparsers.add_parser("all")
    all_parser.add_argument("evidence_root")
    fixture_parser = subparsers.add_parser("fixtures")
    fixture_parser.add_argument("--output-dir", required=True)
    args = parser.parse_args()
    try:
        if args.command == "fixtures":
            fixtures(Path(args.output_dir))
            return 0
        summary = verify_all(Path(args.evidence_root))
        print("ASSERT_EVIDENCE: PASS "
              f"runs={summary['runs']} profiles={summary['profiles']} "
              f"scenarios={len(summary['coverage'])}")
        return 0
    except EvidenceError as error:
        print(f"ASSERT_EVIDENCE: FAIL {error}", file=sys.stderr)
        return 1
    except Exception as error:  # Keep malformed inputs classified.
        print(f"ASSERT_EVIDENCE: FAIL INTERNAL_ERROR: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
