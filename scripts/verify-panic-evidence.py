#!/usr/bin/env python3
"""Verify panic evidence from the serial channel and an external observer."""

import argparse
import copy
import hashlib
import json
from pathlib import Path
import re
import shutil
import sys
import tempfile
import zlib


HISTORICAL_RUNS = {
    "simple-tcg-smp1", "exception-tcg-smp4", "console-lock-tcg-smp4",
    "clock-lock-tcg-smp4", "joint-lock-kvm-smp24",
    "reentry-tcg-smp4", "reentry-kvm-smp4",
    "second-reentry-kvm-smp24", "invalid-clock-tcg-smp4",
    "regressing-clock-tcg-smp4", "intermittent-clock-tcg-smp4",
    "hpet-frozen-tcg-smp4", "restart-tcg-smp4", "shutdown-tcg-smp4",
    "uart-unresponsive-tcg-smp4", "adversarial-lock-wait-tcg-smp4",
    "production-simple-tcg-smp1",
} | {f"joint-lock-{accel}-smp4-{attempt}"
     for accel in ("tcg", "kvm") for attempt in range(1, 4)} | {
    f"second-reentry-{accel}-smp4-{attempt}"
    for accel in ("tcg", "kvm") for attempt in range(1, 4)} | {
    f"constant-clock-{accel}-smp4-{attempt}"
    for accel in ("tcg", "kvm") for attempt in range(1, 4)}
CURRENT_RUNS = (HISTORICAL_RUNS - {"reentry-tcg-smp4", "reentry-kvm-smp4"}) | {
    f"reentry-{accel}-smp4-{attempt}"
    for accel in ("tcg", "kvm") for attempt in range(1, 4)}


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_state(run_dir):
    paths = (run_dir / "gdb-halt-2.log", run_dir / "gdb-negative.log")
    path = next((candidate for candidate in paths if candidate.is_file()),
                None)
    if path is None:
        return {}
    state = {}
    for line in path.read_text(errors="replace").splitlines():
        match = re.match(r"STATE_([A-Z0-9_]+)=(0x[0-9a-fA-F]+|[0-9]+)$",
                         line.strip())
        if match:
            state[match.group(1)] = int(match.group(2), 0)
        match = re.match(
            r"G_PANIC_TEST_UART_(UNRESPONSIVE|POLLS)="
            r"(0x[0-9a-fA-F]+|[0-9]+)$", line.strip())
        if match:
            state["UART_" + match.group(1)] = int(match.group(2), 0)
    return state


def read_json_lines(path):
    values = []
    errors = []
    if not path.is_file():
        return values, [f"{path.name} is missing"]
    for number, line in enumerate(path.read_text(errors="replace").splitlines(),
                                  1):
        if not line:
            continue
        try:
            value = json.loads(line)
        except json.JSONDecodeError as error:
            errors.append(f"{path.name} line {number} is invalid: {error}")
            continue
        if not isinstance(value, dict):
            errors.append(f"{path.name} line {number} is not an object")
        else:
            values.append(value)
    return values, errors


def launch_identity_errors(run_dir, result):
    path = run_dir / "launch.json"
    if not path.is_file():
        return ["launch.json is missing"]
    try:
        launch = json.loads(path.read_text())
    except (json.JSONDecodeError, OSError) as error:
        return [f"launch.json cannot be read: {error}"]
    errors = []
    profile = result.get("profile", {})
    for name in ("machine", "accel", "memory", "smp"):
        if launch.get(name) != profile.get(name):
            errors.append(f"launch {name} differs from result profile")
    if launch.get("elf_sha256") != result.get("elf_sha256"):
        errors.append("launch ELF identity differs from result")
    if launch.get("image_sha256") != result.get("image_sha256_before"):
        errors.append("launch image identity differs from result")
    command = launch.get("command")
    if not isinstance(command, list) or not all(isinstance(x, str)
                                                for x in command):
        errors.append("launch argv is absent or malformed")
    else:
        joined = "\n".join(command)
        if "-qmp" not in command or "qmp.sock,server=on,wait=off" not in joined:
            errors.append("launch did not attach a local QMP observer")
        if "-gdb" not in command or "gdb.sock" not in joined:
            errors.append("launch did not attach the local GDB observer")
        if "-serial" not in command or "serial-transcript.log" not in joined:
            errors.append("launch serial transcript identity is missing")
    if not isinstance(result.get("qemu_pid"), int) or result["qemu_pid"] <= 0:
        errors.append("result lacks the QEMU process identity")
    return errors


def qmp_terminal_errors(run_dir, result):
    action = result.get("action")
    if action not in ("restart", "shutdown"):
        return []
    entries, errors = read_json_lines(run_dir / "qmp-events.jsonl")
    terminal = result.get("terminal_evidence", {})
    expected_event = "RESET" if action == "restart" else "SHUTDOWN"
    expected_reason = "guest-reset" if action == "restart" else "guest-shutdown"
    declared_event = terminal.get("event")
    declared_time = terminal.get("event_monotonic")
    try:
        trigger = float(result["trigger_monotonic"])
        cleanup_start = float(result["cleanup"]["started"])
        event_time = float(declared_time)
    except (KeyError, TypeError, ValueError):
        errors.append("terminal trigger/event/cleanup timing is incomplete")
        return errors
    exact = [entry for entry in entries
             if entry.get("host_monotonic") == declared_time and
             entry.get("message") == declared_event]
    if len(exact) != 1:
        errors.append("declared terminal event is not unique in raw QMP")
    if not (trigger < event_time < cleanup_start):
        errors.append("terminal event is outside trigger-to-cleanup interval")
    if not isinstance(declared_event, dict):
        errors.append("declared terminal QMP event is malformed")
        return errors
    data = declared_event.get("data", {})
    if (declared_event.get("event") != expected_event or
            data.get("guest") is not True or data.get("reason") != expected_reason):
        errors.append("terminal QMP event type, origin, or reason is incompatible")
    raw_candidates = []
    for entry in entries:
        try:
            observed = float(entry.get("host_monotonic"))
        except (TypeError, ValueError):
            errors.append("raw QMP message has an invalid host timestamp")
            continue
        message = entry.get("message")
        if (isinstance(message, dict) and
                message.get("event") == expected_event and
                trigger < observed < cleanup_start):
            raw_candidates.append(entry)
    if len(raw_candidates) != 1:
        errors.append("raw QMP lacks one unique guest terminal event in the causal interval")
    elif raw_candidates[0].get("message", {}).get("data", {}).get("guest") is not True:
        errors.append("raw terminal event was initiated by the host")
    if result.get("qmp_messages") != len(entries):
        errors.append("QMP message count differs from raw stream")
    return errors


def production_preflight_errors(run_dir, result, transcript):
    if not result.get("production"):
        return []
    errors = []
    preflight = result.get("production_preflight", {})

    def require(condition, message):
        if not condition:
            errors.append(message)

    record_path = run_dir / "production-preflight-command.jsonl"
    records, record_errors = read_json_lines(record_path)
    errors.extend(record_errors)
    if len(records) != 1:
        errors.append("production preflight lacks one raw command record")
        return errors
    raw = records[0]
    for key, value in raw.items():
        require(preflight.get(key) == value,
                "production preflight result differs from raw command record")
    payload = preflight.get("payload")
    require(payload == "irq check" and
            preflight.get("command") == payload,
            "production preflight did not observe irq check")
    require(preflight.get("payload_crc32") ==
            f"{zlib.crc32(str(payload).encode()) & 0xffffffff:08x}",
            "production preflight checksum is incorrect")
    require(preflight.get("observer_return_code") == 0 and
            preflight.get("observer_status") == "COMPLETED" and
            preflight.get("classification") == "PASS" and
            preflight.get("handler_status") == 0 and
            preflight.get("gdb_return_code") == 0,
            "production preflight exact handler observation failed")
    try:
        connected = float(preflight["observer_connected_monotonic"])
        armed = float(preflight["observer_armed_monotonic"])
        trigger = float(preflight["trigger_monotonic"])
        breakpoint_time = float(preflight["breakpoint_monotonic"])
        return_time = float(preflight["handler_return_monotonic"])
        completed = float(preflight["completed_monotonic"])
        panic_trigger = float(result["trigger_monotonic"])
        require(connected <= armed <= trigger <= breakpoint_time <=
                return_time <= completed <= panic_trigger,
                "production preflight timing is not causal")
        start = int(preflight["start_line_count"])
        end = int(preflight["end_line_count"])
    except (KeyError, TypeError, ValueError):
        errors.append("production preflight timing or interval is malformed")
        return errors
    lines = transcript.splitlines()
    if not (0 <= start < end <= len(lines)):
        errors.append("production preflight serial interval is invalid")
        return errors
    segment = lines[start:end]
    marker = preflight.get("marker", "")
    markers = [start + index + 1 for index, line in enumerate(segment)
               if line == marker or line.startswith(marker + " ")]
    require(marker.startswith("[IRQ][CHECK] PASS ") and len(markers) == 1 and
            preflight.get("observed") is True and
            preflight.get("marker_line") == markers[0],
            "production irq result is absent from its command interval")

    gdb_path = run_dir / "gdb-production-preflight.log"
    if not gdb_path.is_file():
        errors.append("production preflight raw GDB log is missing")
        return errors
    gdb_text = gdb_path.read_text(errors="replace")
    require(sha256(gdb_path) == preflight.get("gdb_log_sha256"),
            "production preflight GDB hash differs")
    for key in ("breakpoint_stop", "payload_memory_response",
                "return_address_response", "return_breakpoint_response",
                "return_stop", "return_pc_response",
                "handler_status_response"):
        value = preflight.get(key)
        require(isinstance(value, str) and
                gdb_text.splitlines().count(value) == 1,
                f"production preflight {key} is absent from raw GDB")
    require('reason="breakpoint-hit"' in
            str(preflight.get("breakpoint_stop")) and
            'func="shell_dispatch_command_line"' in
            str(preflight.get("breakpoint_stop")),
            "production preflight did not stop in the shell dispatcher")
    contents = re.search(r'contents="([0-9a-fA-F]+)"',
                         str(preflight.get("payload_memory_response")))
    observed_payload = None
    if contents:
        try:
            observed_payload = bytes.fromhex(contents.group(1)).split(
                b"\0", 1)[0].decode("ascii")
        except (ValueError, UnicodeDecodeError):
            pass
    require(observed_payload == payload == preflight.get("observed_payload"),
            "production GDB observed another preflight payload")
    address = preflight.get("return_address")
    address_match = re.fullmatch(
        r'[1-9][0-9]*\^done,value="(0x[0-9a-fA-F]+|[0-9]+)"',
        str(preflight.get("return_address_response")))
    pc_match = re.fullmatch(
        r'[1-9][0-9]*\^done,value="(0x[0-9a-fA-F]+|[0-9]+)"',
        str(preflight.get("return_pc_response")))
    require(address_match is not None and pc_match is not None and
            int(address_match.group(1), 0) == address and
            int(pc_match.group(1), 0) == address,
            "production preflight return PC is not causal")
    breakpoint_number = preflight.get("return_breakpoint_number")
    require(f'bkptno="{breakpoint_number}"' in
            str(preflight.get("return_stop")) and
            'reason="breakpoint-hit"' in str(preflight.get("return_stop")),
            "production preflight did not stop at its return breakpoint")
    status_match = re.fullmatch(
        r'[1-9][0-9]*\^done,value="(-?(?:0x[0-9a-fA-F]+|[0-9]+))"',
        str(preflight.get("handler_status_response")))
    require(status_match is not None and
            int(status_match.group(1), 0) == preflight.get("handler_status"),
            "production preflight status differs from raw GDB")
    command_path = run_dir / "gdb-production-preflight.cmd"
    require(command_path.is_file() and sha256(command_path) ==
            preflight.get("gdb_command_sha256"),
            "production preflight GDB command transcript differs")
    return errors


def halt_observation_errors(run_dir, result):
    terminal = result.get("terminal_evidence", {})
    if terminal.get("kind") != "halt" or result.get("negative_control"):
        return []
    errors = []
    try:
        smp = int(result.get("profile", {}).get("smp", 0))
    except (TypeError, ValueError):
        smp = 0
    expected_cpus = set(range(smp))
    observations = []
    owner_states = []
    expected_depth = (3 if result.get("scenario") == "second-reentry" else
                      2 if result.get("scenario") == "reentry" else 1)
    for index, key in ((1, "first"), (2, "second")):
        path = run_dir / f"gdb-halt-{index}.log"
        metadata = terminal.get(key, {})
        if not path.is_file():
            errors.append(f"halt observation {index} is missing")
            continue
        text = path.read_text(errors="replace")
        if metadata.get("sha256") != sha256(path):
            errors.append(f"halt observation {index} hash differs")
        halted = {int(value) for value in re.findall(
            r"CPU#([0-9]+) \[halted\s*\]", text)}
        if smp <= 0 or halted != expected_cpus:
            errors.append(
                f"halt observation {index} does not cover every vCPU")
        halt_rips = len(re.findall(
            r"^panic_halt_secondary(?: \+ [0-9]+)? in section \.text$",
            text, re.MULTILINE))
        if smp <= 0 or halt_rips < smp:
            errors.append(
                f"halt observation {index} has a CPU outside terminal halt")
        eflags = [int(value, 16) for value in re.findall(
            r"^eflags\s+0x([0-9a-fA-F]+)\b", text, re.MULTILINE)]
        if len(eflags) < smp or any(value & 0x200 for value in eflags):
            errors.append(f"halt observation {index} does not prove IF=0")
        owner = re.findall(r"^PANIC_OWNER_STATE=0x([0-9a-fA-F]+)$",
                           text, re.MULTILINE)
        if len(owner) != 1:
            errors.append(f"halt observation {index} lacks one owner state")
        else:
            owner_state = int(owner[0], 16)
            owner_states.append(owner_state)
            if (owner_state & 0xffffffff) != expected_depth or not (
                    owner_state >> 32):
                errors.append(
                    f"halt observation {index} has wrong owner depth or token")
            if metadata.get("owner_state") != owner_state or (
                    metadata.get("owner_depth") != expected_depth):
                errors.append(
                    f"halt observation {index} metadata differs from raw owner state")
        try:
            observations.append(float(metadata["host_monotonic"]))
        except (KeyError, TypeError, ValueError):
            errors.append(f"halt observation {index} lacks host time")
    if len(observations) == 2 and observations[1] - observations[0] < 0.2:
        errors.append("halt observations did not allow guest execution")
    if len(owner_states) == 2 and owner_states[0] != owner_states[1]:
        errors.append("panic owner state changed between halt observations")
    state = read_state(run_dir)
    expected_faults = (2 if result.get("scenario") == "second-reentry" else
                       1 if result.get("scenario") == "reentry" else 0)
    if result.get("production"):
        pass
    elif state.get("REENTRY_FAULTS") != expected_faults:
        errors.append("raw reentry fault count differs from scenario")
    return errors


def ordered(text, markers):
    position = -1
    for marker in markers:
        found = text.find(marker, position + 1)
        if found < 0:
            return False
        position = found
    return True


def semantic_errors(result, transcript, state):
    errors = []
    scenario = result.get("scenario")
    action = result.get("action")
    locks = result.get("locks", "none")
    terminal = result.get("terminal_evidence", {})
    production = bool(result.get("production"))

    def require(condition, message):
        if not condition:
            errors.append(message)

    require("[BOOT][RUNTIME_READY] PASS" in transcript,
            "runtime readiness marker is missing")
    require(result.get("qmp_connected_before_trigger") is True,
            "QMP observer was not connected before the trigger")
    try:
        require(float(result.get("runtime_ready_monotonic")) <=
                float(result.get("trigger_monotonic")),
                "panic trigger preceded runtime readiness")
    except (TypeError, ValueError):
        require(False, "runtime readiness or trigger time is missing")

    if production:
        preflight = result.get("production_preflight", {})
        marker = preflight.get("marker", "")
        require(preflight.get("command") == "irq check" and
                preflight.get("payload") == "irq check",
                "production smoke did not run irq check")
        require(preflight.get("observer_return_code") == 0 and
                preflight.get("handler_status") == 0 and
                preflight.get("observed") is True,
                "production irq preflight did not complete")
        require(marker.startswith("[IRQ][CHECK] PASS ") and
                marker in transcript,
                "production irq PASS marker is missing")
        require(preflight.get("completed_monotonic", float("inf")) <=
                result.get("trigger_monotonic", float("-inf")),
                "production irq preflight was not observed before panic")
        require(transcript.find(marker) < transcript.find("[PANIC][OWNER]"),
                "production panic preceded its irq preflight")

    if result.get("negative_control"):
        require(result.get("status") == "NEGATIVE_DETECTED",
                "negative control was not classified as detected")
        require("[PANIC_TEST][NEGATIVE] waiting_for_lock=console" in
                transcript, "adversarial lock wait marker is missing")
        require("[PANIC][DUMP_END]" not in transcript,
                "adversarial image unexpectedly completed the dump")
        require(terminal.get("kind") == "negative-timeout" and
                terminal.get("detected") is True,
                "negative timeout was not observed by the host")
        require(state.get("LOCK_ACK_MASK", 0) & 1 and
                state.get("CONSOLE_LOCK_VALUE") == 1,
                "negative control lacks an acquired console lock")
        return errors

    require(result.get("status") == "PASS", "run status is not PASS")
    require("[PANIC_TEST][TRIGGER]" in transcript or production,
            "causal panic trigger is missing")

    if scenario == "uart-unresponsive":
        require(state.get("UART_UNRESPONSIVE") == 1,
                "UART failure injection was not active")
        polls = state.get("UART_POLLS", 0)
        require(0 < polls <= 1000000,
                "UART polling did not remain inside its global budget")
    else:
        require(transcript.count("[PANIC][OWNER]") == 1,
                "owner marker count is not one")

    reentry = scenario in ("reentry", "second-reentry")
    full_dump = not reentry and scenario != "uart-unresponsive"
    if full_dump:
        require(transcript.count("[PANIC][DUMP_BEGIN]") == 1,
                "full dump did not begin exactly once")
        require(transcript.count("[PANIC][DUMP_END]") == 1,
                "full dump did not end exactly once")
        for marker in ("[PANIC][REASON]", "[PANIC][VECTOR]",
                       "[PANIC][FRAME]", "[PANIC][ERROR_CODE]",
                       "[PANIC][CR2]", "[PANIC][CR3]", "[PANIC][MMIO]"):
            require(marker in transcript, marker + " is missing")
        mmio_lines = [line for line in transcript.splitlines()
                      if line.startswith("[PANIC][MMIO] ")]
        require(len(mmio_lines) == 1,
                "panic MMIO record count is not one")
        if len(mmio_lines) == 1:
            mmio = mmio_lines[0]
            if production:
                require(mmio ==
                        "[PANIC][MMIO] available=0 trace=disabled",
                        "production panic MMIO trace was not elided")
            else:
                require(" trace=per-cpu coherence=sequence " in mmio,
                        "debug panic MMIO trace contract is missing")
                available = "available=1" in mmio
                if available:
                    for field in ("cpu_id=", "cpu_valid=", "cpu_slot=",
                                  "slot_valid=", "addr=", "operation=",
                                  "width=", "phase=", "value_valid="):
                        require(field in mmio,
                                "debug panic MMIO field is missing: " + field)
                    require("operation=read" in mmio or
                            "operation=write" in mmio,
                            "debug panic MMIO operation is invalid")
                    require("phase=attempt" in mmio or
                            "phase=complete" in mmio,
                            "debug panic MMIO phase is invalid")
                else:
                    require("available=0" in mmio,
                            "debug panic MMIO availability is invalid")
        require(ordered(transcript, ["[PANIC][OWNER]",
                                     "[PANIC][DUMP_BEGIN]",
                                     "[PANIC][DUMP_END]",
                                     "[PANIC][COUNTDOWN]"]),
                "owner, dump, and countdown ordering is invalid")

    if locks != "none":
        expected_mask = {"console": 1, "clock": 2, "both": 3}[locks]
        require("[PANIC_TEST][LOCK_HELD]" in transcript,
                "lock acquisition marker is missing")
        require(ordered(transcript, ["[PANIC_TEST][OWNER_READY]",
                                     "[PANIC_TEST][LOCK_HELD]",
                                     "[PANIC_TEST][TRIGGER]"]),
                "owner/peer lock rendezvous ordering is invalid")
        require(state.get("LOCK_MASK") == expected_mask and
                state.get("LOCK_ACK_MASK") == expected_mask,
                "lock acknowledgement does not match the requested mask")
        require(state.get("OWNER_OBSERVED_SLOT") !=
                state.get("PEER_OBSERVED_SLOT"),
                "owner and lock holder use the same topology slot")
        if expected_mask & 1:
            require(state.get("CONSOLE_LOCK_VALUE") == 1,
                    "console lock was not observed held")
        if expected_mask & 2:
            require(state.get("CLOCK_LOCK_VALUE") == 1,
                    "clock lock was not observed held")

    if scenario == "exception":
        require("[PANIC][VECTOR] known=1 value=6" in transcript,
                "controlled invalid-opcode vector is missing")
        match = re.search(r"\[PANIC\]\[FRAME\] available=1 rip=0x"
                          r"([0-9A-Fa-f]+)", transcript)
        require(match is not None, "exception frame is missing")
        expected_rip = result.get("expected_rip")
        require(match is not None and expected_rip is not None and
                int(match.group(1), 16) == expected_rip,
                "exception frame RIP does not match the injected ud2")

    if scenario == "reentry":
        require(transcript.count("[PANIC][REENTRY]") == 1,
                "first reentry marker count is not one")
        require("depth=2" in transcript,
                "first reentry depth is not two")
        require("[PANIC][DUMP_END]" not in transcript and
                "[PANIC][COUNTDOWN]" not in transcript,
                "reentry resumed the complex dump or countdown")
    if scenario == "second-reentry":
        require(transcript.count("[PANIC][REENTRY]") == 1,
                "third entry emitted output")
        require("depth=2" in transcript and
                "[PANIC][ACTION]" not in transcript and
                "[PANIC][COUNTDOWN]" not in transcript,
                "third entry did not take the silent terminal path")

    if result.get("clock_mode") == "constant":
        require("source=test-counter" in transcript and
                "state=clock-stalled" in transcript,
                "constant countdown source did not hit the stall budget")
        require(state.get("COUNTDOWN_SAMPLES", 0) >= 2000000,
                "constant source was not actually sampled to the stall limit")
    if result.get("clock_mode") == "invalid":
        require("source=test-counter" in transcript and
                "state=fallback-expired" in transcript,
                "invalid countdown source did not take bounded fallback")
    if result.get("clock_mode") == "regressing":
        require("state=clock-stalled" in transcript,
                "regressing source did not hit the no-progress budget")
    if result.get("clock_mode") == "intermittent":
        require("state=iteration-limit" in transcript,
                "intermittent source did not hit the absolute poll budget")

    if result.get("freeze_hpet"):
        hpet = result.get("hpet", {})
        require(hpet.get("disabled") is True and hpet.get("stable") is True,
                "HPET device was not disabled with stable post samples")

    if action == "halt":
        require(terminal.get("kind") == "halt" and
                terminal.get("if_clear") is True and
                terminal.get("depth_ok") is True,
                "external halt observation is incomplete")
        if scenario not in ("second-reentry", "uart-unresponsive"):
            require("[PANIC][ACTION] requested=halt" in transcript,
                    "halt action marker is missing")
    elif action in ("restart", "shutdown"):
        action_pattern = (
            r"^\[PANIC\]\[ACTION\] requested=restart attempt=cf9 "
            r"budget=[1-9][0-9]*$" if action == "restart" else
            r"^\[PANIC\]\[ACTION\] requested=shutdown attempt=acpi-s5 "
            r"pm1a=0x[0-9A-Fa-f]+ pm1b=0x[0-9A-Fa-f]+$")
        action_lines = re.findall(action_pattern, transcript, re.MULTILINE)
        require(len(action_lines) == 1,
                action + " lacks one structured terminal attempt")
        require(ordered(transcript, ["[PANIC][DUMP_END]",
                                     "[PANIC][COUNTDOWN]",
                                     f"[PANIC][ACTION] requested={action}"]),
                action + " diagnostic/action ordering is invalid")
        require(terminal.get("kind") == action and
                terminal.get("guest_origin") is True,
                action + " lacks a guest-originated QMP event")
        event = terminal.get("event") or {}
        expected_event = "RESET" if action == "restart" else "SHUTDOWN"
        require(event.get("event") == expected_event and
                event.get("data", {}).get("guest") is True,
                action + " event is absent or host-originated")

    return errors


def verify_run(run_dir):
    errors = []
    result_path = run_dir / "result.json"
    serial_path = run_dir / "serial-transcript.log"
    if not result_path.is_file() or not serial_path.is_file():
        return ["result.json or serial-transcript.log is missing"]
    try:
        result = json.loads(result_path.read_text())
    except (json.JSONDecodeError, OSError) as error:
        return [f"cannot read result.json: {error}"]
    transcript = serial_path.read_text(errors="replace")
    state = read_state(run_dir)
    errors.extend(semantic_errors(result, transcript, state))
    errors.extend(production_preflight_errors(run_dir, result, transcript))
    errors.extend(halt_observation_errors(run_dir, result))
    errors.extend(launch_identity_errors(run_dir, result))
    errors.extend(qmp_terminal_errors(run_dir, result))

    artifacts = result.get("artifacts")
    required_artifacts = {"serial-transcript.log", "debugcon.log",
                          "qemu-trace.log", "qmp-events.jsonl",
                          "qemu-stderr.log"}
    if result.get("production"):
        required_artifacts |= {
            "production-preflight-command.jsonl",
            "production-preflight-observer.log",
            "production-preflight-observer.argv.json",
            "production-preflight-observer.armed",
            "gdb-production-preflight.log",
            "gdb-production-preflight.cmd",
            "gdb-production-preflight.argv.json",
            "hmp-preflight.log",
        }
    if not isinstance(artifacts, dict) or not required_artifacts.issubset(
            artifacts):
        errors.append("mandatory run artifact identities are incomplete")
        artifacts = artifacts if isinstance(artifacts, dict) else {}
    for name, metadata in artifacts.items():
        if (not isinstance(name, str) or not isinstance(metadata, dict) or
                Path(name).is_absolute() or ".." in Path(name).parts):
            errors.append("recorded artifact path or metadata is unsafe")
            continue
        path = run_dir / name
        if not path.is_file():
            errors.append(f"recorded artifact is missing: {name}")
            continue
        if path.stat().st_size != metadata.get("bytes"):
            errors.append(f"recorded artifact size differs: {name}")
        if sha256(path) != metadata.get("sha256"):
            errors.append(f"recorded artifact hash differs: {name}")
    return errors


def refresh_fixture_artifacts(run_dir, result):
    artifacts = {}
    for name in ("serial-transcript.log", "debugcon.log", "qemu-trace.log",
                 "qmp-events.jsonl", "qemu-stderr.log"):
        path = run_dir / name
        artifacts[name] = {"bytes": path.stat().st_size,
                           "sha256": sha256(path)}
    result["artifacts"] = artifacts
    (run_dir / "result.json").write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n")


def build_qmp_fixture(run_dir):
    run_dir.mkdir(parents=True)
    transcript = "\n".join([
        "[BOOT][RUNTIME_READY] PASS cpus=4/4",
        "[PANIC_TEST][TRIGGER] owner_slot=0",
        "[PANIC][OWNER] token=1 cpu=0 depth=1",
        "[PANIC][DUMP_BEGIN] channel=serial kind=panic",
        "[PANIC][REASON] text=fixture", "[PANIC][VECTOR] known=0",
        "[PANIC][FRAME] available=0", "[PANIC][ERROR_CODE] valid=0",
        "[PANIC][CR2] valid=0", "[PANIC][CR3] value=0x1",
        "[PANIC][MMIO] available=0 trace=per-cpu coherence=sequence "
        "cpu_id=0 cpu_valid=0 cpu_slot=0 slot_valid=0",
        "[PANIC][DUMP_END] status=complete",
        "[PANIC][COUNTDOWN] state=completed",
        "[PANIC][ACTION] requested=restart attempt=cf9 budget=200000", "",
    ])
    (run_dir / "serial-transcript.log").write_text(transcript)
    entries = [
        {"host_monotonic": 1.0, "message": {"QMP": {"version": {}}}},
        {"host_monotonic": 11.0, "message": {
            "event": "RESET", "data": {"guest": True,
                                          "reason": "guest-reset"},
            "timestamp": {"seconds": 1, "microseconds": 0}}},
        {"host_monotonic": 12.1, "message": {
            "event": "SHUTDOWN", "data": {"guest": False,
                                             "reason": "host-qmp-quit"},
            "timestamp": {"seconds": 2, "microseconds": 0}}},
    ]
    (run_dir / "qmp-events.jsonl").write_text(
        "".join(json.dumps(entry, sort_keys=True) + "\n" for entry in entries))
    for name in ("debugcon.log", "qemu-trace.log", "qemu-stderr.log"):
        (run_dir / name).write_text("")
    elf_hash = "1" * 64
    image_hash = "2" * 64
    launch = {
        "machine": "q35", "accel": "tcg", "cpu": "max", "smp": 4,
        "memory": "2G", "elf_sha256": elf_hash,
        "image_sha256": image_hash,
        "command": ["qemu-system-x86_64", "-serial",
                    f"file:{run_dir / 'serial-transcript.log'}", "-qmp",
                    "unix:/tmp/fixture/qmp.sock,server=on,wait=off", "-gdb",
                    "chardev:gdb0", "-chardev",
                    "socket,path=/tmp/fixture/gdb.sock,server=on,wait=off,id=gdb0"],
    }
    (run_dir / "launch.json").write_text(
        json.dumps(launch, indent=2, sort_keys=True) + "\n")
    event = entries[1]
    result = {
        "schema": 1, "scenario": "simple", "action": "restart",
        "locks": "none", "clock_mode": "normal", "production": False,
        "negative_control": False, "freeze_hpet": False, "status": "PASS",
        "qmp_connected_before_trigger": True, "qmp_messages": len(entries),
        "runtime_ready_monotonic": 9.0, "trigger_monotonic": 10.0,
        "qemu_pid": 1234, "elf_sha256": elf_hash,
        "image_sha256_before": image_hash,
        "profile": {"machine": "q35", "accel": "tcg", "cpu": "max",
                    "memory": "2G", "smp": 4},
        "terminal_evidence": {"kind": "restart", "guest_origin": True,
                              "event_monotonic": event["host_monotonic"],
                              "event": event["message"]},
        "cleanup": {"started": 12.0, "completed": 12.2,
                    "method": "qmp-quit"},
    }
    refresh_fixture_artifacts(run_dir, result)
    return result


def fixture_selftest(output_dir=None):
    base_result = {
        "scenario": "contention", "action": "halt", "locks": "both",
        "clock_mode": "normal", "production": False,
        "negative_control": False, "freeze_hpet": False, "status": "PASS",
        "qmp_connected_before_trigger": True,
        "runtime_ready_monotonic": 1.0, "trigger_monotonic": 2.0,
        "terminal_evidence": {"kind": "halt", "if_clear": True,
                              "depth_ok": True},
    }
    base_text = "\n".join([
        "[BOOT][RUNTIME_READY] PASS",
        "[PANIC_TEST][OWNER_READY] owner_slot=0 lock_mask=3",
        "[PANIC_TEST][LOCK_HELD] peer_slot=1 mask=3 console_locked=1 clock_locked=1",
        "[PANIC_TEST][TRIGGER] owner_slot=0",
        "[PANIC][OWNER] depth=1",
        "[PANIC][DUMP_BEGIN] channel=serial",
        "[PANIC][REASON] text=x", "[PANIC][VECTOR] known=0",
        "[PANIC][FRAME] available=0", "[PANIC][ERROR_CODE] valid=0",
        "[PANIC][CR2] valid=0", "[PANIC][CR3] value=0x1",
        "[PANIC][MMIO] available=1 trace=per-cpu coherence=sequence "
        "cpu_id=0 cpu_valid=1 cpu_slot=0 slot_valid=1 addr=0x1 "
        "operation=read width=4 phase=complete value_valid=1 value=0x1",
        "[PANIC][DUMP_END] status=complete",
        "[PANIC][COUNTDOWN] state=fallback-expired",
        "[PANIC][ACTION] requested=halt terminal=halt if=0", "",
    ])
    base_state = {"LOCK_MASK": 3, "LOCK_ACK_MASK": 3,
                  "OWNER_OBSERVED_SLOT": 0, "PEER_OBSERVED_SLOT": 1,
                  "CONSOLE_LOCK_VALUE": 1, "CLOCK_LOCK_VALUE": 1}
    fixtures = {}
    fixtures["dump-incomplete"] = (
        base_result, base_text.replace("[PANIC][DUMP_END] status=complete\n", ""),
        base_state)
    missing_lock = copy.deepcopy(base_state)
    missing_lock["CONSOLE_LOCK_VALUE"] = 0
    fixtures["lock-not-acquired"] = (base_result, base_text, missing_lock)
    same_slot = copy.deepcopy(base_state)
    same_slot["PEER_OBSERVED_SLOT"] = 0
    fixtures["same-owner-peer"] = (base_result, base_text, same_slot)
    announced = copy.deepcopy(base_result)
    announced["terminal_evidence"] = {"kind": "halt", "if_clear": False,
                                       "depth_ok": False}
    fixtures["action-only-announced"] = (announced, base_text, base_state)
    host_reset = copy.deepcopy(base_result)
    host_reset.update({"scenario": "simple", "action": "restart",
                       "locks": "none"})
    host_reset["terminal_evidence"] = {
        "kind": "restart", "guest_origin": False,
        "event": {"event": "RESET", "data": {"guest": False}}}
    fixtures["host-reset"] = (host_reset, base_text, {})
    stalled = copy.deepcopy(base_result)
    stalled.update({"scenario": "clock-stalled", "locks": "none",
                    "clock_mode": "constant"})
    no_fallback = base_text.replace(
        "[PANIC][COUNTDOWN] state=fallback-expired",
        "[PANIC][COUNTDOWN] state=start source=test-counter")
    fixtures["fallback-missing"] = (stalled, no_fallback,
                                     {"COUNTDOWN_SAMPLES": 2000001})
    third = copy.deepcopy(base_result)
    third.update({"scenario": "second-reentry", "locks": "none"})
    third_text = "\n".join([
        "[BOOT][RUNTIME_READY] PASS", "[PANIC_TEST][TRIGGER]",
        "[PANIC][OWNER] depth=1", "[PANIC][DUMP_BEGIN]",
        "[PANIC][REENTRY] depth=2", "[PANIC][REENTRY] depth=3",
        "[PANIC][ACTION] requested=halt", "",
    ])
    fixtures["third-entry-output"] = (third, third_text, {})

    failed = []
    for name, (result, transcript, state) in fixtures.items():
        errors = semantic_errors(copy.deepcopy(result), transcript,
                                 copy.deepcopy(state))
        if not errors:
            failed.append(name)
        else:
            print(f"fixture={name} result=REJECTED reason={errors[0]}")

    destination = Path(output_dir).resolve() if output_dir else None
    if destination:
        destination.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="panic-verifier-") as temporary:
        temporary = Path(temporary)
        control = temporary / "valid-restart"
        build_qmp_fixture(control)
        control_errors = verify_run(control)
        if control_errors:
            print("full fixture control rejected: " +
                  "; ".join(control_errors), file=sys.stderr)
            return 1
        print("fixture=valid-restart-full result=ACCEPTED")
        full_fixtures = {}
        for name in ("qmp-before-trigger", "qmp-metadata-only",
                     "restart-without-action", "cleanup-host-shutdown",
                     "qmp-reason-incompatible", "artifact-identities-empty"):
            target = temporary / name
            shutil.copytree(control, target)
            result = json.loads((target / "result.json").read_text())
            if name == "qmp-before-trigger":
                entries, _ = read_json_lines(target / "qmp-events.jsonl")
                entries[1]["host_monotonic"] = 9.5
                result["terminal_evidence"]["event_monotonic"] = 9.5
                (target / "qmp-events.jsonl").write_text("".join(
                    json.dumps(entry, sort_keys=True) + "\n" for entry in entries))
            elif name == "qmp-metadata-only":
                entries, _ = read_json_lines(target / "qmp-events.jsonl")
                entries.pop(1)
                result["qmp_messages"] = len(entries)
                (target / "qmp-events.jsonl").write_text("".join(
                    json.dumps(entry, sort_keys=True) + "\n" for entry in entries))
            elif name == "restart-without-action":
                path = target / "serial-transcript.log"
                path.write_text(path.read_text().replace(
                    "[PANIC][ACTION] requested=restart attempt=cf9 budget=200000",
                    "[FIXTURE][ACTION_REMOVED]"))
            elif name == "cleanup-host-shutdown":
                entries, _ = read_json_lines(target / "qmp-events.jsonl")
                host = entries[2]
                result["action"] = "shutdown"
                result["terminal_evidence"] = {
                    "kind": "shutdown", "guest_origin": False,
                    "event_monotonic": host["host_monotonic"],
                    "event": host["message"]}
                path = target / "serial-transcript.log"
                path.write_text(path.read_text().replace(
                    "requested=restart attempt=cf9 budget=200000",
                    "requested=shutdown attempt=acpi-s5 pm1a=0x604 pm1b=0x0"))
            elif name == "qmp-reason-incompatible":
                entries, _ = read_json_lines(target / "qmp-events.jsonl")
                entries[1]["message"]["data"]["reason"] = "host-reset"
                result["terminal_evidence"]["event"] = entries[1]["message"]
                (target / "qmp-events.jsonl").write_text("".join(
                    json.dumps(entry, sort_keys=True) + "\n" for entry in entries))
            refresh_fixture_artifacts(target, result)
            if name == "artifact-identities-empty":
                result["artifacts"] = {}
                (target / "result.json").write_text(
                    json.dumps(result, indent=2, sort_keys=True) + "\n")
            errors = verify_run(target)
            full_fixtures[name] = errors
            if not errors:
                failed.append(name)
                print(f"fixture={name} result=ACCEPTED_INCORRECTLY")
            else:
                print(f"fixture={name} result=REJECTED reason={errors[0]}")
            if destination:
                stored = destination / name
                stored.mkdir()
                for filename in ("result.json", "serial-transcript.log",
                                 "qmp-events.jsonl", "launch.json"):
                    shutil.copy2(target / filename, stored / filename)
                (stored / "expectation.json").write_text(json.dumps({
                    "mutation": name, "expected": "REJECTED",
                    "observed": "REJECTED" if errors else "ACCEPTED",
                    "reason": errors[0] if errors else "accepted invalid evidence",
                }, indent=2, sort_keys=True) + "\n")
        if destination:
            (destination / "valid-control.json").write_text(json.dumps({
                "expected": "ACCEPTED", "observed": "ACCEPTED",
            }, indent=2, sort_keys=True) + "\n")
    if failed:
        print("fixture selftest accepted invalid evidence: " + ", ".join(failed),
              file=sys.stderr)
        return 1
    print(f"fixture-selftest: PASS rejected={len(fixtures) + 6}")
    return 0


def main():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    run_parser = sub.add_parser("run")
    run_parser.add_argument("run_dir")
    tree_parser = sub.add_parser("tree")
    tree_parser.add_argument("root")
    tree_parser.add_argument("--layout", choices=("current", "historical"),
                             default="current")
    fixtures_parser = sub.add_parser("fixtures")
    fixtures_parser.add_argument("--output-dir")
    args = parser.parse_args()

    if args.command == "fixtures":
        return fixture_selftest(args.output_dir)
    if args.command == "run":
        targets = [Path(args.run_dir)]
    else:
        targets = sorted(path.parent for path in Path(args.root).rglob(
                         "result.json"))
        if not targets:
            print("panic evidence tree contains no runs", file=sys.stderr)
            return 1

    failed = 0
    if args.command == "tree":
        expected = CURRENT_RUNS if args.layout == "current" else HISTORICAL_RUNS
        observed = {target.name for target in targets}
        if observed != expected:
            failed += 1
            print("panic-matrix status=FAIL missing=" +
                  ",".join(sorted(expected - observed)) + " unexpected=" +
                  ",".join(sorted(observed - expected)))
        else:
            print(f"panic-matrix status=PASS layout={args.layout} "
                  f"runs={len(observed)}")
    for target in targets:
        errors = verify_run(target)
        if errors:
            failed += 1
            print(f"run={target} status=FAIL")
            for error in errors:
                print(f"  {error}")
        else:
            print(f"run={target} status=PASS")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
