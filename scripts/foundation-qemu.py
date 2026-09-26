#!/usr/bin/env python3
"""Causal host transport for foundation diagnostics in a QEMU guest."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import queue
import re
import socket
import subprocess
import sys
import threading
import time
import zlib


ACCEPT_RE = re.compile(
    r"^\[HARNESS\]\[FRAME\] ACCEPT seq=([1-9][0-9]*) "
    r"crc=([0-9a-f]{8}) len=([0-9]+)$")
BEGIN_RE = re.compile(r"^\[HARNESS\]\[BEGIN\] seq=([1-9][0-9]*)$")
END_RE = re.compile(
    r"^\[HARNESS\]\[END\] seq=([1-9][0-9]*) status=(-?[0-9]+)$")
REPLAY_RE = re.compile(
    r"^\[HARNESS\]\[REPLAY\] seq=([1-9][0-9]*) status=(-?[0-9]+)$")
REJECT_RE = re.compile(r"^\[HARNESS\]\[FRAME\] REJECT seq=([^ ]+) (.+)$")
OWNER_RECORD = "[TASKMAN][OWNER_STATE] shell=BLOCKED session=ACTIVE"
SESSION_END_RECORD = "[MODAL] session_end OK"
MODAL_SETTLE_SECONDS = 2.5
INPUT_BOUNDARY_TIMEOUT_SECONDS = 5.0
INPUT_BOUNDARY_SAMPLE_DELAY_SECONDS = 0.05
INPUT_DELIVERY_TIMEOUT_SECONDS = 90.0
INPUT_DELIVERY_STABILITY_SECONDS = 0.5
INPUT_DELIVERY_SAMPLE_DELAY_SECONDS = 0.05
INPUT_CHARACTER_BOUNDARY = ("left", "right")
INPUT_DISPATCH_TIMEOUT_SECONDS = 60.0
INPUT_RESUME_TIMEOUT_SECONDS = 2.0
INPUT_RESUME_POLL_SECONDS = 0.01
INPUT_OBSERVER_CLEANUP_TIMEOUT_SECONDS = 5.0
INPUT_KEY_PRESS_TIMEOUT_SECONDS = 5.0
INPUT_KEY_RELEASE_TIMEOUT_SECONDS = 5.0
SHELL_INPUT_SYMBOLS = ("g_shell_active", "g_buffer", "g_len", "g_pos")
XHCI_INPUT_SYMBOL = "xhci_driver"
INPUT_LAYOUT_ARRAYS = (
    ("prev_keys", "prev_keys_offset", "prev_keys_size"),
    ("prev_mods", "prev_mods_offset", "prev_mods_size"),
    ("repeat_key", "repeat_key_offset", "repeat_key_size"),
    ("repeat_mods", "repeat_mods_offset", "repeat_mods_size"),
    ("repeat_active", "repeat_active_offset", "repeat_active_size"),
)


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def complete_lines(path):
    """Return complete decoded lines and the byte end of every line."""
    data = path.read_bytes() if path.is_file() else b""
    lines = []
    ends = []
    start = 0
    while True:
        newline = data.find(b"\n", start)
        if newline < 0:
            break
        item = data[start:newline]
        if item.endswith(b"\r"):
            item = item[:-1]
        lines.append(item.decode(errors="replace"))
        ends.append(newline + 1)
        start = newline + 1
    return lines, ends, len(data)


def read_pid(runtime):
    path = runtime / "qemu.pid"
    try:
        value = int(path.read_text().strip())
        os.kill(value, 0)
    except (OSError, ValueError):
        raise RuntimeError("QEMU pid is absent or inactive")
    return value


def hmp_args(root, socket_path, operation, *values):
    return [sys.executable, str(root / "scripts" / "qemu_hmp.py"),
            "--socket", str(socket_path), operation, *values]


def run_hmp(root, socket_path, operation, *values, output_path=None):
    command = hmp_args(root, socket_path, operation, *values)
    completed = subprocess.run(command, text=True, capture_output=True,
                               check=False)
    if output_path:
        output_path.write_text(
            "argv=" + json.dumps(command) + "\n" +
            "return_code=" + str(completed.returncode) + "\n" +
            "stdout:\n" + completed.stdout + "\nstderr:\n" +
            completed.stderr)
    if completed.returncode:
        raise RuntimeError(
            f"HMP {operation} failed with status {completed.returncode}")
    return completed.stdout


def qmp_key_events(key, down):
    """Return the exact QMP input events for one supported key state."""
    if len(key) == 1:
        if "a" <= key <= "z" or "0" <= key <= "9":
            qcode = key
            modifiers = []
        elif key == " ":
            qcode = "spc"
            modifiers = []
        elif key == "-":
            qcode = "minus"
            modifiers = []
        elif key == ".":
            qcode = "dot"
            modifiers = []
        elif key == "+":
            qcode = "equal"
            modifiers = ["shift"]
        else:
            raise ValueError(f"unsupported QMP input key: {key!r}")
    elif key in ("left", "right", "ret"):
        qcode = key
        modifiers = []
    else:
        raise ValueError(f"unsupported QMP input key: {key!r}")

    ordered = [*modifiers, qcode]
    if not down:
        ordered.reverse()
    return [{
        "type": "key",
        "data": {
            "down": down,
            "key": {"type": "qcode", "data": item},
        },
    } for item in ordered]


def load_input_layout(path):
    """Validate the xHCI input-state layout emitted from the real header."""
    layout_path = Path(path).resolve()
    try:
        layout = json.loads(layout_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise RuntimeError("command input layout is unreadable") from error
    required = {
        "schema", "controller_size", "slot_count", "key_count",
        *(item for _name, offset, size in INPUT_LAYOUT_ARRAYS
          for item in (offset, size)),
    }
    if set(layout) != required or layout.get("schema") != 1:
        raise RuntimeError("command input layout schema is invalid")
    if (layout.get("slot_count") != 64 or layout.get("key_count") != 6 or
            not isinstance(layout.get("controller_size"), int) or
            layout["controller_size"] <= 0):
        raise RuntimeError("command input layout dimensions are invalid")
    ranges = []
    expected_sizes = {
        "prev_keys": layout["slot_count"] * layout["key_count"],
        "prev_mods": layout["slot_count"],
        "repeat_key": layout["slot_count"],
        "repeat_mods": layout["slot_count"],
        "repeat_active": layout["slot_count"],
    }
    for name, offset_field, size_field in INPUT_LAYOUT_ARRAYS:
        offset = layout.get(offset_field)
        size = layout.get(size_field)
        if (not isinstance(offset, int) or isinstance(offset, bool) or
                not isinstance(size, int) or isinstance(size, bool) or
                offset < 0 or size != expected_sizes[name] or
                offset + size > layout["controller_size"]):
            raise RuntimeError("command input layout range is invalid")
        ranges.append((offset, offset + size))
    if any(left[1] > right[0] for left, right in
           zip(sorted(ranges), sorted(ranges)[1:])):
        raise RuntimeError("command input layout ranges overlap")
    return layout_path, layout


def elf_symbol_physical_addresses(elf, virtual_addresses):
    """Map kernel virtual symbols through the ELF PT_LOAD declarations."""
    command = ["readelf", "-lW", str(elf)]
    completed = subprocess.run(command, text=True, capture_output=True,
                               check=False)
    if completed.returncode:
        raise RuntimeError("readelf could not inspect input mappings")
    segments = []
    for line in completed.stdout.splitlines():
        fields = line.split()
        if len(fields) >= 6 and fields[0] == "LOAD":
            try:
                segment = {
                    "offset": int(fields[1], 0),
                    "virtual_address": int(fields[2], 0),
                    "physical_address": int(fields[3], 0),
                    "file_size": int(fields[4], 0),
                    "memory_size": int(fields[5], 0),
                }
            except ValueError as error:
                raise RuntimeError("readelf returned an invalid LOAD entry") \
                    from error
            if segment["memory_size"] <= 0:
                raise RuntimeError("ELF LOAD entry has no mapped memory")
            segments.append(segment)
    if not segments:
        raise RuntimeError("ELF has no LOAD mappings")
    physical = {}
    for name, address in virtual_addresses.items():
        matches = [segment for segment in segments
                   if segment["virtual_address"] <= address <
                   segment["virtual_address"] + segment["memory_size"]]
        if len(matches) != 1:
            raise RuntimeError(f"symbol {name} has no unique LOAD mapping")
        segment = matches[0]
        physical[name] = (segment["physical_address"] + address -
                          segment["virtual_address"])
    return physical, segments, completed.stdout, command


def hmp_physical_bytes(value, physical_address, size):
    """Decode the exact bytes printed by HMP xp for a physical range."""
    if not isinstance(value, str) or size <= 0:
        raise RuntimeError("HMP physical-memory response is invalid")
    result = bytearray()
    for line in value.splitlines():
        match = re.fullmatch(
            r"([0-9a-fA-F]{16}):((?: 0x[0-9a-fA-F]{2})+)", line)
        if match is None:
            raise RuntimeError("HMP physical-memory row is malformed")
        row_address = int(match.group(1), 16)
        if row_address != physical_address + len(result):
            raise RuntimeError("HMP physical-memory row address differs")
        result.extend(int(item, 16) for item in
                      re.findall(r"0x([0-9a-fA-F]{2})", match.group(2)))
    if len(result) != size:
        raise RuntimeError("HMP physical-memory response size differs")
    return bytes(result)


def qmp_physical_read(qmp, physical_address, size, deadline):
    """Read and record one bounded physical-memory observation through QMP."""
    if (not isinstance(physical_address, int) or physical_address < 0 or
            not isinstance(size, int) or size <= 0 or size > 2048):
        raise RuntimeError("QMP physical-memory request is out of bounds")
    command_line = f"xp /{size}bx 0x{physical_address:x}"
    started = time.monotonic()
    command_id, response = qmp.command(
        "human-monitor-command", {"command-line": command_line}, deadline)
    completed = time.monotonic()
    value = response.get("return")
    data = hmp_physical_bytes(value, physical_address, size)
    return data, {
        "request_id": command_id,
        "qmp_execute": "human-monitor-command",
        "arguments": {"command-line": command_line},
        "expected_return": value,
        "response_return": value,
        "physical_address": physical_address,
        "size": size,
        "bytes_hex": data.hex(),
        "started_monotonic": started,
        "completed_monotonic": completed,
    }


def qmp_input_snapshot(qmp, virtual_addresses, physical_addresses,
                       input_layout, deadline):
    """Read a shell snapshot that brackets the keyboard-state observation."""
    shell_base = virtual_addresses["g_buffer"]
    shell_end = virtual_addresses["g_shell_active"] + 1
    if (not shell_base < virtual_addresses["g_len"] < shell_end or
            not shell_base < virtual_addresses["g_pos"] < shell_end or
            shell_end - shell_base > 512):
        raise RuntimeError("shell input symbols are not in one bounded span")
    shell_size = shell_end - shell_base
    shell_before, shell_before_event = qmp_physical_read(
        qmp, physical_addresses["g_buffer"], shell_size, deadline)
    keyboard_start = min(input_layout[item[1]] for item in INPUT_LAYOUT_ARRAYS)
    keyboard_end = max(input_layout[item[1]] + input_layout[item[2]]
                       for item in INPUT_LAYOUT_ARRAYS)
    keyboard_size = keyboard_end - keyboard_start
    keyboard_data, keyboard_event = qmp_physical_read(
        qmp, physical_addresses[XHCI_INPUT_SYMBOL] + keyboard_start,
        keyboard_size, deadline)
    shell_after, shell_after_event = qmp_physical_read(
        qmp, physical_addresses["g_buffer"], shell_size, deadline)
    shell_snapshot_stable = shell_before == shell_after
    length_offset = virtual_addresses["g_len"] - shell_base
    position_offset = virtual_addresses["g_pos"] - shell_base
    active_offset = virtual_addresses["g_shell_active"] - shell_base
    length = int.from_bytes(shell_after[length_offset:length_offset + 4],
                            "little", signed=True)
    position = int.from_bytes(shell_after[position_offset:position_offset + 4],
                              "little", signed=True)
    active = shell_after[active_offset]
    if 0 <= length < length_offset:
        observed = shell_after[:length]
        terminated = shell_after[length] == 0
    else:
        observed = b""
        terminated = False
    values = {}
    for name, offset_field, size_field in INPUT_LAYOUT_ARRAYS:
        start = input_layout[offset_field] - keyboard_start
        values[name] = keyboard_data[start:start + input_layout[size_field]]
    slot_count = input_layout["slot_count"]
    key_count = input_layout["key_count"]
    active_slots = []
    for slot in range(slot_count):
        key_start = slot * key_count
        if (any(values["prev_keys"][key_start:key_start + key_count]) or
                values["prev_mods"][slot] or values["repeat_key"][slot] or
                values["repeat_mods"][slot] or
                values["repeat_active"][slot]):
            active_slots.append(slot)
    released = all(not any(value) for value in values.values())
    return {
        "observer": "qmp-stable-physical-memory",
        "shell_snapshot_stable": shell_snapshot_stable,
        "active": active,
        "length": length,
        "position": position,
        "terminated": terminated,
        "observed_bytes_hex": observed.hex(),
        "observed_text": (observed.decode("ascii") if
                          observed.isascii() else None),
        "keyboard_release_observed": released,
        "keyboard_active_slots": active_slots,
        "keyboard_state_hex": {
            name: value.hex() for name, value in values.items()
        },
        "shell_memory_before_event": shell_before_event,
        "keyboard_memory_event": keyboard_event,
        "shell_memory_after_event": shell_after_event,
        "observed_monotonic": time.monotonic(),
    }


class QmpInputClient:
    """Small QMP client that records every request and response losslessly."""

    def __init__(self, path, log_path, deadline):
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.socket.settimeout(max(0.1, deadline - time.monotonic()))
        self.socket.connect(str(path))
        self.buffer = b""
        self.log_path = log_path
        self.log_path.write_text("", encoding="utf-8")
        self.next_id = 1
        greeting = self._receive(deadline)
        if not isinstance(greeting, dict) or "QMP" not in greeting:
            raise RuntimeError("QMP greeting was not received")
        self.command("qmp_capabilities", {}, deadline)

    def _record(self, direction, message):
        entry = {
            "direction": direction,
            "host_monotonic": time.monotonic(),
            "message": message,
        }
        with self.log_path.open("a", encoding="utf-8") as stream:
            stream.write(json.dumps(entry, sort_keys=True) + "\n")

    def _receive(self, deadline):
        while b"\n" not in self.buffer:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise RuntimeError("QMP response timed out")
            self.socket.settimeout(remaining)
            chunk = self.socket.recv(65536)
            if not chunk:
                raise RuntimeError("QMP connection closed")
            self.buffer += chunk
        raw, self.buffer = self.buffer.split(b"\n", 1)
        try:
            message = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise RuntimeError("QMP returned malformed JSON") from error
        self._record("receive", message)
        return message

    def command(self, name, arguments, deadline):
        command_id = f"input-{self.next_id}"
        self.next_id += 1
        request = {"execute": name, "id": command_id}
        if arguments:
            request["arguments"] = arguments
        self._record("send", request)
        encoded = (json.dumps(request, separators=(",", ":")) + "\n").encode()
        self.socket.sendall(encoded)
        while True:
            response = self._receive(deadline)
            if response.get("id") != command_id:
                continue
            if "error" in response or "return" not in response:
                raise RuntimeError(f"QMP command failed: {name}")
            return command_id, response

    def input(self, events, deadline):
        started = time.monotonic()
        command_id, _ = self.command(
            "input-send-event", {"events": events}, deadline)
        return {
            "request_id": command_id,
            "events": events,
            "started_monotonic": started,
            "completed_monotonic": time.monotonic(),
        }

    def close(self):
        self.socket.close()


def wait_qmp_running(qmp, command_deadline):
    """Require QMP to observe the resumed VM before sending Enter."""
    started = time.monotonic()
    deadline = min(command_deadline, started + INPUT_RESUME_TIMEOUT_SECONDS)
    checks = []
    while time.monotonic() < deadline:
        check_started = time.monotonic()
        command_id, response = qmp.command("query-status", {}, deadline)
        completed = time.monotonic()
        value = response.get("return")
        checks.append({
            "request_id": command_id,
            "qmp_execute": "query-status",
            "arguments": {},
            "expected_return": value,
            "response_return": value,
            "started_monotonic": check_started,
            "completed_monotonic": completed,
        })
        if (isinstance(value, dict) and value.get("running") is True and
                value.get("status") == "running"):
            return checks
        time.sleep(min(INPUT_RESUME_POLL_SECONDS,
                       max(0.0, deadline - time.monotonic())))
    raise RuntimeError("QMP did not observe the resumed VM")


def hmp_string(value):
    """Quote one HMP string argument without changing its byte spelling."""
    if any(char in value for char in "\x00\r\n"):
        raise RuntimeError("HMP string contains a control character")
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def append_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as stream:
        stream.write(json.dumps(value, sort_keys=True) + "\n")


def matching(lines, start_count, regex, sequence):
    found = []
    for index, line in enumerate(lines[start_count:], start_count + 1):
        match = regex.fullmatch(line)
        if match and match.group(1) == str(sequence):
            found.append((index, match))
    return found


def parse_allowed(text):
    try:
        values = {int(value) for value in text.split(",")}
    except ValueError as error:
        raise argparse.ArgumentTypeError("statuses must be comma-separated integers") from error
    if not values:
        raise argparse.ArgumentTypeError("at least one status is required")
    return values


def validate_payload(payload):
    if not payload or len(payload) > 96:
        raise ValueError("payload length must be 1..96")
    if payload.startswith(" ") or payload.endswith(" ") or "  " in payload:
        raise ValueError("payload must use canonical single spaces")
    if any(char in payload for char in "\r\n\t\"'"):
        raise ValueError("payload contains unsupported whitespace or quoting")
    if not re.fullmatch(r"[a-z0-9.+ -]+", payload):
        raise ValueError("payload contains a keymap-unsupported character")
    if payload == "tasktest exec" or payload.startswith("tasktest exec "):
        raise ValueError("recursive framed payload is forbidden")


def build_command_frame(sequence, payload):
    validate_payload(payload)
    if (not isinstance(sequence, int) or isinstance(sequence, bool) or
            sequence < 1 or sequence > 0xffffffffffffffff):
        raise ValueError("framed command sequence is outside uint64")
    payload_bytes = payload.encode("ascii")
    crc = f"{zlib.crc32(payload_bytes) & 0xffffffff:08x}"
    frame = f"tasktest exec {sequence} {crc} {payload}"
    if len(frame.encode("ascii")) >= 256:
        raise ValueError("framed command exceeds the shell buffer")
    return crc, frame


def wait_for_ready(serial_path, marker, timeout, pid):
    started = time.monotonic()
    deadline = started + timeout
    while time.monotonic() <= deadline:
        lines, _, _ = complete_lines(serial_path)
        matches = [index for index, line in enumerate(lines, 1)
                   if line == marker or line.startswith(marker + " ")]
        if matches:
            return {"status": "PASS", "line": matches[-1],
                    "host_monotonic": time.monotonic(),
                    "elapsed_seconds": time.monotonic() - started}
        try:
            os.kill(pid, 0)
        except OSError:
            return {"status": "QEMU_EXITED",
                    "elapsed_seconds": time.monotonic() - started}
        time.sleep(0.1)
    return {"status": "TIMEOUT",
            "elapsed_seconds": time.monotonic() - started}


def command_wait_ready(args):
    runtime = Path(args.runtime).resolve()
    serial_path = runtime / "qemu-serial.log"
    pid = read_pid(runtime)
    result = wait_for_ready(serial_path, args.marker, args.timeout, pid)
    result.update({"schema": 1, "kind": "runtime-ready", "qemu_pid": pid,
                   "marker": args.marker, "serial": str(serial_path)})
    Path(args.output).write_text(json.dumps(result, indent=2,
                                           sort_keys=True) + "\n")
    print(json.dumps(result, sort_keys=True))
    return 0 if result["status"] == "PASS" else 1


def frame_record_base(args, pid, crc, frame, start_lines, start_bytes):
    return {
        "schema": 26,
        "kind": "framed-command",
        "profile_id": args.profile_id,
        "vm_id": args.vm_id,
        "candidate_sha256": args.candidate_sha256,
        "qemu_pid": pid,
        "sequence": args.sequence,
        "payload": args.payload,
        "crc32": crc,
        "frame_length": len(frame),
        "deadline_seconds": args.timeout,
        "start_line_count": start_lines,
        "start_byte_count": start_bytes,
        "trigger_monotonic": time.monotonic(),
        "transport_status": "PENDING",
        "qmp_input_status": "PENDING",
        "guest_input_status": "PENDING",
        "classification": "PENDING",
        "disposition": "EXECUTED",
        "accepted_line": None,
        "begin_line": None,
        "end_line": None,
        "end_line_count": None,
        "end_byte_count": None,
        "handler_status": None,
        "accept_count": 0,
        "begin_count": 0,
        "end_count": 0,
        "replay_count": 0,
        "reject_count": 0,
        "checksum_verified": False,
    }


def command_frame(args):
    root = Path(args.root).resolve()
    runtime = Path(args.runtime).resolve()
    serial_path = runtime / "qemu-serial.log"
    hmp_socket = runtime / "hmp.sock"
    qmp_socket = runtime / "qmp.sock"
    record_path = Path(args.record).resolve()
    transport_log = Path(args.transport_log).resolve()
    gdb_socket = Path(args.gdb_socket).resolve()
    elf = Path(args.elf).resolve()
    allowed = parse_allowed(args.allowed_status)
    crc, frame = build_command_frame(args.sequence, args.payload)
    pid = read_pid(runtime)
    if (not hmp_socket.is_socket() or not qmp_socket.is_socket() or
            not gdb_socket.is_socket()):
        raise RuntimeError("HMP, QMP, or GDB observer socket is absent")
    if not elf.is_file():
        raise RuntimeError("command observer ELF is absent")
    initial_lines, initial_ends, initial_total_bytes = complete_lines(serial_path)
    start_count = len(initial_lines)
    initial_bytes = initial_ends[-1] if initial_ends else 0
    record = frame_record_base(args, pid, crc, frame, start_count,
                               initial_bytes)
    record["serial_byte_count_at_trigger"] = initial_total_bytes
    modal_owner_seen = False
    modal_escaped = False
    try:
        observe_framed_input(root, qmp_socket, gdb_socket, elf,
                             Path(args.input_layout), frame,
                             args.input_profile, transport_log, record,
                             record["trigger_monotonic"] + args.timeout)
    except RuntimeError as error:
        record["transport_status"] = "FAIL"
        record["classification"] = "INPUT_OBSERVER_FAILURE"
        record["error"] = str(error)
        record["completed_monotonic"] = time.monotonic()
        append_json(record_path, record)
        print(json.dumps(record, sort_keys=True))
        return 1

    deadline = record["trigger_monotonic"] + args.timeout
    if record["guest_input_status"] != "PASS":
        deadline = min(deadline, time.monotonic() + 5.0)
    while time.monotonic() <= deadline:
        lines, ends, total_bytes = complete_lines(serial_path)
        accepts = matching(lines, start_count, ACCEPT_RE, args.sequence)
        begins = matching(lines, start_count, BEGIN_RE, args.sequence)
        endings = matching(lines, start_count, END_RE, args.sequence)
        replays = matching(lines, start_count, REPLAY_RE, args.sequence)
        rejects = matching(lines, start_count, REJECT_RE, args.sequence)
        record["accept_count"] = len(accepts)
        record["begin_count"] = len(begins)
        record["end_count"] = len(endings)
        record["replay_count"] = len(replays)
        record["reject_count"] = len(rejects)
        if accepts:
            record["accepted_line"] = accepts[0][0]
            accepted = accepts[0][1]
            record["checksum_verified"] = (
                accepted.group(2) == crc and
                int(accepted.group(3)) == len(args.payload))
        if begins:
            record["begin_line"] = begins[0][0]

        if args.modal_screenshot and begins and not modal_owner_seen:
            owner_lines = [index for index, line in
                           enumerate(lines[start_count:], start_count + 1)
                           if line == OWNER_RECORD]
            if owner_lines:
                modal_owner_seen = True
                record["modal_owner_line"] = owner_lines[0]
                record["modal_settle_seconds"] = MODAL_SETTLE_SECONDS
                record["modal_settle_started_monotonic"] = time.monotonic()
                if record["modal_settle_started_monotonic"] + \
                        MODAL_SETTLE_SECONDS > deadline:
                    record["classification"] = "MODAL_SETTLE_TIMEOUT"
                    break
                time.sleep(MODAL_SETTLE_SECONDS)
                screenshot = Path(args.modal_screenshot).resolve()
                screenshot.parent.mkdir(parents=True, exist_ok=True)
                modal_log = transport_log.with_name(
                    transport_log.stem + "-screendump.log")
                try:
                    run_hmp(root, hmp_socket, "command",
                            f"screendump {hmp_string(str(screenshot))}",
                            output_path=modal_log)
                    if not screenshot.is_file():
                        raise RuntimeError(
                            "HMP screendump did not create output")
                    record["modal_screenshot"] = {
                        "path": screenshot.name,
                        "bytes": screenshot.stat().st_size,
                        "sha256": sha256(screenshot),
                    }
                    record["modal_screenshot_monotonic"] = time.monotonic()
                    esc_log = transport_log.with_name(
                        transport_log.stem + "-esc.log")
                    run_hmp(root, hmp_socket, "key", "esc", "--profile",
                            "normal", output_path=esc_log)
                    record["modal_escape_monotonic"] = time.monotonic()
                    modal_escaped = True
                except RuntimeError as error:
                    record["classification"] = "MODAL_HMP_FAILURE"
                    record["error"] = str(error)
                    break

        if rejects:
            record["classification"] = (
                "GUEST_INPUT_MISMATCH" if
                record["guest_input_status"] != "PASS" else
                "FRAME_REJECTED")
            break
        if replays:
            record["classification"] = "UNEXPECTED_REPLAY"
            break
        if len(accepts) > 1 or len(begins) > 1 or len(endings) > 1:
            record["classification"] = "DUPLICATE_FRAME_RECORD"
            break
        if endings:
            end_line, end_match = endings[0]
            record["end_line"] = end_line
            record["end_line_count"] = end_line
            record["end_byte_count"] = ends[end_line - 1]
            record["handler_status"] = int(end_match.group(2))
            if record["guest_input_status"] != "PASS":
                record["classification"] = "GUEST_INPUT_MISMATCH"
            elif len(accepts) != 1 or len(begins) != 1:
                record["classification"] = "INCOMPLETE_FRAME"
            elif not record["checksum_verified"]:
                record["classification"] = "CHECKSUM_MISMATCH"
            elif not (accepts[0][0] < begins[0][0] < end_line):
                record["classification"] = "FRAME_ORDER_INVALID"
            elif args.modal_screenshot and not (modal_owner_seen and
                                                 modal_escaped):
                record["classification"] = "MODAL_OBSERVATION_MISSING"
            elif record["handler_status"] not in allowed:
                record["classification"] = "HANDLER_STATUS_REJECTED"
            else:
                if args.modal_screenshot:
                    session_end = [index for index, line in
                                   enumerate(lines[start_count:end_line],
                                             start_count + 1)
                                   if line == SESSION_END_RECORD]
                    if len(session_end) != 1:
                        record["classification"] = \
                            "MODAL_SESSION_END_MISSING"
                    else:
                        record["modal_session_end_line"] = session_end[0]
                        record["classification"] = "PASS"
                else:
                    record["classification"] = "PASS"
            break
        try:
            os.kill(pid, 0)
        except OSError:
            record["classification"] = "QEMU_EXITED"
            break
        time.sleep(0.1)
    else:
        if record["guest_input_status"] != "PASS":
            record["classification"] = "GUEST_INPUT_MISMATCH"
        else:
            record["classification"] = (
                "COMMAND_NO_END" if record["begin_count"] else
                "FRAME_NO_BEGIN")

    record["completed_monotonic"] = time.monotonic()
    record["elapsed_seconds"] = (record["completed_monotonic"] -
                                 record["trigger_monotonic"])
    record["serial_byte_count_observed"] = (
        complete_lines(serial_path)[2])
    append_json(record_path, record)
    print(json.dumps(record, sort_keys=True))
    return 0 if record["classification"] == "PASS" else 1


def command_reject_frame(args):
    root = Path(args.root).resolve()
    runtime = Path(args.runtime).resolve()
    serial_path = runtime / "qemu-serial.log"
    qmp_socket = runtime / "qmp.sock"
    gdb_socket = Path(args.gdb_socket).resolve()
    elf = Path(args.elf).resolve()
    record_path = Path(args.record).resolve()
    transport_log = Path(args.transport_log).resolve()
    expected_crc, _ = build_command_frame(args.sequence, args.payload)
    if not re.fullmatch(r"[0-9a-f]{8}", args.crc):
        raise ValueError("rejection probe checksum must be eight lowercase hex digits")
    if args.crc == expected_crc:
        raise ValueError("rejection probe checksum must differ from the payload checksum")
    frame = f"tasktest exec {args.sequence} {args.crc} {args.payload}"
    if len(frame.encode("ascii")) >= 256:
        raise ValueError("rejection probe frame exceeds the shell buffer")
    pid = read_pid(runtime)
    if not qmp_socket.is_socket() or not gdb_socket.is_socket():
        raise RuntimeError("QMP or GDB observer socket is absent")
    if not elf.is_file():
        raise RuntimeError("command observer ELF is absent")
    initial_lines, initial_ends, initial_total = complete_lines(serial_path)
    start_count = len(initial_lines)
    start_bytes = initial_ends[-1] if initial_ends else 0
    record = frame_record_base(args, pid, args.crc, frame, start_count,
                               start_bytes)
    record.update({
        "kind": "framed-command-rejection",
        "expected_crc32": expected_crc,
        "expected_rejection": "crc",
        "serial_byte_count_at_trigger": initial_total,
        "disposition": "REJECTED",
    })
    try:
        observe_framed_input(root, qmp_socket, gdb_socket, elf,
                             Path(args.input_layout), frame,
                             args.input_profile, transport_log, record,
                             record["trigger_monotonic"] + args.timeout)
    except RuntimeError as error:
        record["transport_status"] = "FAIL"
        record["classification"] = "INPUT_OBSERVER_FAILURE"
        record["error"] = str(error)
    if record["transport_status"] == "PASS":
        deadline = min(record["trigger_monotonic"] + args.timeout,
                       time.monotonic() + 10.0)
        while time.monotonic() <= deadline:
            lines, ends, _ = complete_lines(serial_path)
            accepts = matching(lines, start_count, ACCEPT_RE, args.sequence)
            begins = matching(lines, start_count, BEGIN_RE, args.sequence)
            endings = matching(lines, start_count, END_RE, args.sequence)
            replays = matching(lines, start_count, REPLAY_RE, args.sequence)
            rejects = matching(lines, start_count, REJECT_RE, args.sequence)
            marker = "[INPUTTEST][MARKER] name=" + args.marker_name
            markers = [index for index, line in
                       enumerate(lines[start_count:], start_count + 1)
                       if line == marker]
            record.update({
                "accept_count": len(accepts), "begin_count": len(begins),
                "end_count": len(endings), "replay_count": len(replays),
                "reject_count": len(rejects),
                "handler_marker_count": len(markers),
            })
            if rejects:
                line_number, match = rejects[0]
                record["rejection_line"] = line_number
                record["rejection_reason"] = match.group(2)
                record["end_line_count"] = line_number
                record["end_byte_count"] = ends[line_number - 1]
                if (len(rejects) == 1 and not accepts and not begins and
                        not endings and not replays and not markers and
                        match.group(2) ==
                        (f"reason=crc expected_seq={args.sequence} "
                         f"expected_crc={args.crc} actual_crc={expected_crc}")):
                    record["classification"] = "EXPECTED_FRAME_REJECTED"
                else:
                    record["classification"] = "REJECTION_NOT_ISOLATED"
                break
            if accepts or begins or endings or replays or markers:
                record["classification"] = "REJECTED_FRAME_DISPATCHED"
                break
            time.sleep(0.05)
        else:
            record["classification"] = "EXPECTED_REJECTION_MISSING"
    record["completed_monotonic"] = time.monotonic()
    record["elapsed_seconds"] = (record["completed_monotonic"] -
                                 record["trigger_monotonic"])
    record["serial_byte_count_observed"] = complete_lines(serial_path)[2]
    append_json(record_path, record)
    print(json.dumps(record, sort_keys=True))
    return 0 if record["classification"] == "EXPECTED_FRAME_REJECTED" else 1


def mi_line(process, deadline, output):
    while time.monotonic() < deadline:
        try:
            line = process.mi_output.get(
                timeout=min(0.2, max(0.001, deadline - time.monotonic())))
        except queue.Empty:
            line = ""
        if line is None:
            raise RuntimeError("GDB MI output closed before observation completed")
        if line:
            output.append(line)
            return line.rstrip("\r\n")
        if process.poll() is not None:
            raise RuntimeError("GDB MI exited before completing observation")
    raise RuntimeError("GDB MI response timed out")


def mi_reader_start(process):
    output_queue = queue.Queue()

    def read_output():
        for line in process.stdout:
            output_queue.put(line)
        output_queue.put(None)

    reader = threading.Thread(target=read_output, daemon=True)
    process.mi_output = output_queue
    process.mi_reader = reader
    reader.start()


def mi_reader_finish(process, output):
    process.mi_reader.join(timeout=1)
    while True:
        try:
            line = process.mi_output.get_nowait()
        except queue.Empty:
            break
        if line is not None:
            output.append(line)


def mi_command(process, token, command, deadline, output, command_log):
    wire = f"{token}{command}"
    command_log.append(wire)
    process.stdin.write(wire + "\n")
    process.stdin.flush()
    prefix = f"{token}^"
    while True:
        line = mi_line(process, deadline, output)
        if line.startswith(prefix):
            if line.startswith(prefix + "error"):
                raise RuntimeError(f"GDB MI command failed: {line}")
            return line


def mi_wait_stopped(process, deadline, output):
    while True:
        line = mi_line(process, deadline, output)
        if line.startswith("*stopped"):
            return line


def mi_memory_bytes(response, expected_size):
    match = re.search(r'contents="([0-9a-fA-F]+)"', response)
    if match is None:
        raise RuntimeError("GDB MI memory response has no contents")
    try:
        value = bytes.fromhex(match.group(1))
    except ValueError as error:
        raise RuntimeError("GDB MI memory response is not hexadecimal") from error
    if len(value) != expected_size:
        raise RuntimeError("GDB MI memory response has an unexpected size")
    return value


def elf_symbol_addresses(elf, names):
    completed = subprocess.run(["nm", "-an", str(elf)], text=True,
                               capture_output=True, check=False)
    if completed.returncode:
        raise RuntimeError("nm could not inspect the command observer ELF")
    wanted = set(names)
    found = {name: [] for name in wanted}
    for line in completed.stdout.splitlines():
        fields = line.split()
        if len(fields) >= 3 and fields[-1] in wanted:
            try:
                address = int(fields[0], 16)
            except ValueError:
                continue
            found[fields[-1]].append(address)
    if any(len(found[name]) != 1 for name in wanted):
        raise RuntimeError("command observer ELF lacks unique shell symbols")
    return {name: found[name][0] for name in wanted}


def write_gdb_observer_files(base, argv, output, commands):
    log_path = base.with_suffix(".log")
    command_path = base.with_suffix(".cmd")
    argv_path = base.with_suffix(".argv.json")
    log_path.write_text("".join(output), encoding="utf-8")
    command_path.write_text("\n".join(commands) + "\n", encoding="utf-8")
    argv_path.write_text(json.dumps(argv, indent=2) + "\n", encoding="utf-8")
    return {
        "log": log_path.name,
        "log_sha256": sha256(log_path),
        "command": command_path.name,
        "command_sha256": sha256(command_path),
        "argv": argv_path.name,
        "argv_sha256": sha256(argv_path),
    }


def close_gdb_observer(process, token, deadline, output, commands):
    if process is None:
        return token
    if process.poll() is None:
        try:
            mi_command(process, token, "-target-detach", deadline, output,
                       commands)
            token += 1
        except (OSError, RuntimeError):
            pass
    if process.poll() is None:
        try:
            mi_command(process, token, "-gdb-exit", deadline, output,
                       commands)
            token += 1
        except (OSError, RuntimeError):
            process.terminate()
    if process.poll() is None:
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
    if hasattr(process, "mi_reader"):
        mi_reader_finish(process, output)
    return token


def resume_dispatch(process, token, deadline, output, commands):
    """Resume execution after inspecting the dispatcher argument."""
    continued = mi_command(process, token, "-exec-continue", deadline,
                           output, commands)
    token += 1
    if not continued.endswith("^running"):
        raise RuntimeError("GDB MI did not resume the shell dispatcher")
    resumed = time.monotonic()
    return token, resumed


def detach_stopped_gdb_observer(process, token, deadline, output, commands):
    """Detach a stopped private observer and close it within its deadline."""
    if process.poll() is not None:
        raise RuntimeError("GDB observer exited before cleanup")
    detach_token = token
    detached = mi_command(process, token, "-target-detach", deadline,
                          output, commands)
    token += 1
    if not detached.endswith("^done"):
        raise RuntimeError("GDB observer did not detach from the guest")
    detached_monotonic = time.monotonic()
    exit_token = token
    exited = mi_command(process, token, "-gdb-exit", deadline, output,
                        commands)
    token += 1
    if not exited.endswith("^exit"):
        raise RuntimeError("GDB observer did not acknowledge exit")
    try:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise subprocess.TimeoutExpired("gdb observer cleanup", 0)
        return_code = process.wait(timeout=remaining)
    except subprocess.TimeoutExpired as error:
        process.kill()
        process.wait(timeout=5)
        raise RuntimeError("GDB observer did not exit after detach") \
            from error
    mi_reader_finish(process, output)
    if return_code != 0:
        raise RuntimeError(
            f"GDB observer exited with status {return_code} after detach")
    return token, {
        "input_observer_cleanup_method":
            "detach-stopped-private-observer",
        "input_observer_cleanup_return_code": return_code,
        "input_observer_target_detached_monotonic": detached_monotonic,
        "input_observer_cleanup_detach_response_token": detach_token,
        "input_observer_cleanup_exit_response_token": exit_token,
    }


def keyboard_release_state(process, token, addresses, input_layout, deadline,
                           output, commands):
    """Read every guest keyboard state cell used to recognize key release."""
    values = {}
    response_tokens = {}
    controller = addresses[XHCI_INPUT_SYMBOL]
    for name, offset_field, size_field in INPUT_LAYOUT_ARRAYS:
        response = mi_command(
            process, token,
            (f"-data-read-memory-bytes 0x"
             f"{controller + input_layout[offset_field]:x} "
             f"{input_layout[size_field]}"),
            deadline, output, commands)
        response_tokens[name] = token
        token += 1
        values[name] = mi_memory_bytes(response, input_layout[size_field])

    slot_count = input_layout["slot_count"]
    key_count = input_layout["key_count"]
    active_slots = []
    for slot in range(slot_count):
        key_start = slot * key_count
        if (any(values["prev_keys"][key_start:key_start + key_count]) or
                values["prev_mods"][slot] or
                values["repeat_key"][slot] or
                values["repeat_mods"][slot] or
                values["repeat_active"][slot]):
            active_slots.append(slot)
    released = all(not any(value) for value in values.values())
    return token, {
        "keyboard_release_observed": released,
        "keyboard_active_slots": active_slots,
        "keyboard_state_hex": {
            name: value.hex() for name, value in values.items()
        },
        "keyboard_response_tokens": response_tokens,
    }


def shell_input_snapshot(gdb_socket, elf, addresses, input_layout, base,
                         deadline):
    argv = ["gdb", "-q", "-nx", "--interpreter=mi2", str(elf)]
    output = []
    commands = []
    process = None
    token = 1
    active = length = position = None
    observed = b""
    try:
        process = subprocess.Popen(
            argv, text=True, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, bufsize=1, encoding="utf-8",
            errors="replace")
        mi_reader_start(process)
        sample_deadline = min(deadline, time.monotonic() + 10.0)
        mi_command(process, token, "-gdb-set confirm off", sample_deadline,
                   output, commands)
        token += 1
        mi_command(process, token, "-gdb-set pagination off", sample_deadline,
                   output, commands)
        token += 1
        mi_command(process, token, "-target-select remote " +
                   str(gdb_socket), sample_deadline, output, commands)
        token += 1
        active_response = mi_command(
            process, token,
            f"-data-read-memory-bytes 0x{addresses['g_shell_active']:x} 1",
            sample_deadline, output, commands)
        active_token = token
        token += 1
        length_response = mi_command(
            process, token,
            f"-data-read-memory-bytes 0x{addresses['g_len']:x} 4",
            sample_deadline, output, commands)
        length_token = token
        token += 1
        position_response = mi_command(
            process, token,
            f"-data-read-memory-bytes 0x{addresses['g_pos']:x} 4",
            sample_deadline, output, commands)
        position_token = token
        token += 1
        memory_response = mi_command(
            process, token,
            f"-data-read-memory-bytes 0x{addresses['g_buffer']:x} 256",
            sample_deadline, output, commands)
        memory_token = token
        token += 1
        token, keyboard = keyboard_release_state(
            process, token, addresses, input_layout, sample_deadline,
            output, commands)
        active = mi_memory_bytes(active_response, 1)[0]
        length = int.from_bytes(mi_memory_bytes(length_response, 4),
                                "little", signed=True)
        position = int.from_bytes(mi_memory_bytes(position_response, 4),
                                  "little", signed=True)
        memory = mi_memory_bytes(memory_response, 256)
        if 0 <= length < len(memory):
            observed = memory[:length]
            terminated = memory[length] == 0
        else:
            terminated = False
        close_gdb_observer(process, token, sample_deadline, output, commands)
        process = None
        sample = {
            "active": active,
            "length": length,
            "position": position,
            "terminated": terminated,
            "observed_bytes_hex": observed.hex(),
            "observed_text": (observed.decode("ascii") if
                              observed.isascii() else None),
            "active_response_token": active_token,
            "length_response_token": length_token,
            "position_response_token": position_token,
            "memory_response_token": memory_token,
            "observed_monotonic": time.monotonic(),
            **keyboard,
        }
        sample.update(write_gdb_observer_files(
            base, argv, output, commands))
        return sample
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        close_gdb_observer(process, token,
                           min(deadline, time.monotonic() + 5.0),
                           output, commands)
        sample = {
            "active": active,
            "length": length,
            "position": position,
            "observed_bytes_hex": observed.hex(),
            "error": str(error),
            "observed_monotonic": time.monotonic(),
        }
        sample.update(write_gdb_observer_files(
            base, argv, output, commands))
        raise RuntimeError(
            f"shell input delivery observation failed: {error}") from error


def classify_input_delivery(active, length, position, terminated, observed,
                            expected, snapshot_stable=True):
    if snapshot_stable is not True:
        return "UNSTABLE"
    coherent = (active == 1 and length == len(observed) and
                position == len(observed) and terminated is True)
    if not coherent:
        return "DIVERGED"
    if observed == expected:
        return "EXACT"
    if expected.startswith(observed):
        return "PREFIX"
    return "DIVERGED"


def classify_cursor_delivery(active, length, position, terminated, observed,
                             expected, pending_position,
                             expected_position):
    coherent = (active == 1 and length == len(observed) == len(expected) and
                terminated is True and observed == expected)
    if not coherent:
        return "DIVERGED"
    if position == expected_position:
        return "EXACT"
    if position == pending_position:
        return "PENDING"
    return "DIVERGED"


def observe_framed_input(root, qmp_socket, gdb_socket, elf, input_layout_path,
                         frame, input_profile, transport_log, record,
                         command_deadline):
    input_layout_path, input_layout = load_input_layout(input_layout_path)
    addresses = elf_symbol_addresses(
        elf, (*SHELL_INPUT_SYMBOLS, XHCI_INPUT_SYMBOL))
    physical_addresses, load_segments, readelf_output, readelf_command = \
        elf_symbol_physical_addresses(elf, addresses)
    record["input_symbol_addresses"] = {
        name: f"0x{address:016x}" for name, address in addresses.items()
    }
    record["input_symbol_physical_addresses"] = {
        name: f"0x{address:016x}" for name, address in
        physical_addresses.items()
    }
    artifact_prefix = transport_log.stem
    address_map_path = transport_log.with_name(
        f"{artifact_prefix}-input-address-map.json")
    address_map_path.write_text(json.dumps({
        "schema": 1,
        "kind": "elf-input-physical-address-map",
        "elf_sha256": sha256(elf),
        "readelf_argv": readelf_command,
        "readelf_stdout": readelf_output,
        "load_segments": load_segments,
        "virtual_addresses": record["input_symbol_addresses"],
        "physical_addresses": record["input_symbol_physical_addresses"],
    }, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    record.update({
        "input_layout_artifact": input_layout_path.name,
        "input_layout_sha256": sha256(input_layout_path),
        "input_layout": input_layout,
        "input_address_map_artifact": address_map_path.name,
        "input_address_map_sha256": sha256(address_map_path),
        "input_load_segments": load_segments,
    })
    qmp_log = transport_log.with_name(f"{artifact_prefix}-qmp.jsonl")
    qmp = QmpInputClient(qmp_socket, qmp_log, command_deadline)
    boundary_deadline = min(command_deadline,
                            time.monotonic() + INPUT_BOUNDARY_TIMEOUT_SECONDS)
    boundary_samples = []
    sample_number = 0
    while time.monotonic() < boundary_deadline:
        sample_number += 1
        try:
            sample = qmp_input_snapshot(
                qmp, addresses, physical_addresses, input_layout,
                boundary_deadline)
            clean = (sample["shell_snapshot_stable"] is True and
                     sample["active"] == 1 and sample["length"] == 0 and
                     sample["position"] == 0 and
                     sample["keyboard_release_observed"] is True)
            sample.update({"sample": sample_number, "clean": clean})
            boundary_samples.append(sample)
            if clean:
                record["input_boundary_samples"] = boundary_samples
                record["input_boundary_status"] = "CLEAN"
                record["input_boundary_active"] = sample["active"]
                record["input_boundary_length"] = sample["length"]
                record["input_boundary_position"] = sample["position"]
                record["input_boundary_keyboard_release"] = True
                break
            time.sleep(INPUT_BOUNDARY_SAMPLE_DELAY_SECONDS)
        except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
            boundary_samples.append({
                "sample": sample_number,
                "clean": False,
                "error": str(error),
            })
            record["input_boundary_samples"] = boundary_samples
            qmp.close()
            raise RuntimeError(
                f"shell input boundary observation failed: {error}") from error
    if record.get("input_boundary_status") != "CLEAN":
        record["input_boundary_samples"] = boundary_samples
        record["input_boundary_status"] = "TIMEOUT"
        qmp.close()
        raise RuntimeError("shell input boundary did not become clean")

    expected = frame.encode("ascii")
    delivery_started = time.monotonic()
    delivery_deadline = min(
        command_deadline, delivery_started + INPUT_DELIVERY_TIMEOUT_SECONDS)
    delivery_samples = []
    text_events = []
    enter_events = []
    delivery_status = "TIMEOUT"
    record["qmp_text_started_monotonic"] = delivery_started
    last_release_completed = None

    for character_index, character in enumerate(frame, 1):
        if time.monotonic() >= delivery_deadline:
            break
        runstate_started = time.monotonic()
        runstate_checks = wait_qmp_running(qmp, delivery_deadline)
        runstate_observed = time.monotonic()
        stroke_event = qmp.input(
            qmp_key_events(character, True) + qmp_key_events(character, False),
            delivery_deadline)
        text_event = {
            "stroke_event": stroke_event,
            "atomic_down_up": True,
            "release_timeout_seconds": INPUT_KEY_RELEASE_TIMEOUT_SECONDS,
            "character_index": character_index,
            "character_hex": character.encode("ascii").hex(),
            "target_length": character_index,
            "pre_pulse_runstate_started_monotonic": runstate_started,
            "pre_pulse_runstate_observed_monotonic": runstate_observed,
            "pre_pulse_runstate_checks": runstate_checks,
        }
        text_events.append(text_event)
        target = expected[:character_index]
        release_complete = False
        release_deadline = min(
            delivery_deadline,
            time.monotonic() + INPUT_KEY_RELEASE_TIMEOUT_SECONDS)
        while time.monotonic() < release_deadline:
            sample_number = len(delivery_samples) + 1
            sample = qmp_input_snapshot(
                qmp, addresses, physical_addresses, input_layout,
                release_deadline)
            sample.update({
                "sample": sample_number,
                "phase": "delivery",
                "character_index": character_index,
                "target_length": character_index,
            })
            try:
                observed = bytes.fromhex(sample["observed_bytes_hex"])
            except ValueError:
                observed = b""
            input_text_status = classify_input_delivery(
                sample["active"], sample["length"], sample["position"],
                sample["terminated"], observed, target,
                sample["shell_snapshot_stable"])
            sample["input_text_status"] = input_text_status
            sample["press_complete"] = False
            sample["character_complete"] = (
                input_text_status == "EXACT" and
                sample["keyboard_release_observed"] is True)
            sample["status"] = (
                "EXACT" if sample["character_complete"] else
                ("HELD" if input_text_status == "EXACT" else
                 input_text_status))
            delivery_samples.append(sample)
            if input_text_status == "DIVERGED":
                delivery_status = "MISMATCH"
                break
            if sample["character_complete"]:
                release_complete = True
                last_release_completed = stroke_event["completed_monotonic"]
                break
            time.sleep(min(INPUT_DELIVERY_SAMPLE_DELAY_SECONDS,
                           max(0.0, release_deadline - time.monotonic())))
        if delivery_status == "MISMATCH" or not release_complete:
            break

    record["qmp_text_completed_monotonic"] = time.monotonic()
    text_complete = (len(text_events) == len(expected) and
                     all("stroke_event" in event for event in text_events))
    record["qmp_text_status"] = (
        "PASS" if text_complete else "PARTIAL")
    record["input_character_boundary_status"] = (
        "PASS" if text_complete else "PARTIAL")
    if (delivery_status != "MISMATCH" and
            text_complete and
            last_release_completed is not None):
        stability_not_before = (last_release_completed +
                                INPUT_DELIVERY_STABILITY_SECONDS)
        while time.monotonic() < delivery_deadline:
            remaining = stability_not_before - time.monotonic()
            if remaining > 0:
                time.sleep(min(remaining, max(
                    0.0, delivery_deadline - time.monotonic())))
            if time.monotonic() >= delivery_deadline:
                break
            sample_number = len(delivery_samples) + 1
            sample = qmp_input_snapshot(
                qmp, addresses, physical_addresses, input_layout,
                delivery_deadline)
            sample.update({
                "sample": sample_number,
                "phase": "stability",
                "character_index": len(expected),
                "target_length": len(expected),
            })
            try:
                observed = bytes.fromhex(sample["observed_bytes_hex"])
            except ValueError:
                observed = b""
            input_text_status = classify_input_delivery(
                sample["active"], sample["length"], sample["position"],
                sample["terminated"], observed, expected,
                sample["shell_snapshot_stable"])
            sample["input_text_status"] = input_text_status
            if input_text_status == "UNSTABLE":
                sample["status"] = "UNSTABLE"
            else:
                sample["status"] = (
                    "EXACT" if input_text_status == "EXACT" and
                    sample["keyboard_release_observed"] is True else
                    "DIVERGED")
            delivery_samples.append(sample)
            stable_span = (sample["observed_monotonic"] -
                           last_release_completed)
            sample["character_complete"] = (
                sample["status"] == "EXACT" and
                sample["keyboard_release_observed"] is True)
            if sample["character_complete"] and \
                    stable_span >= INPUT_DELIVERY_STABILITY_SECONDS:
                delivery_status = "COMPLETE"
                record["input_delivery_stable_span_seconds"] = stable_span
                break
            if input_text_status == "DIVERGED":
                delivery_status = "MISMATCH"
                break
            time.sleep(min(INPUT_DELIVERY_SAMPLE_DELAY_SECONDS,
                           max(0.0, delivery_deadline - time.monotonic())))

    def write_manifest():
        transport_log.write_text(json.dumps({
            "schema": 17,
            "kind": "qmp-stable-physical-observed-atomic-stroke-transport",
            "shell_snapshot_policy":
                "matching-physical-reads-bracketing-keyboard-state",
            "frame_bytes_hex": expected.hex(),
            "qmp_log": qmp_log.name,
            "input_layout_artifact": input_layout_path.name,
            "input_layout_sha256": sha256(input_layout_path),
            "input_layout": input_layout,
            "input_address_map_artifact": address_map_path.name,
            "input_address_map_sha256": sha256(address_map_path),
            "key_pulses": text_events,
            "resume_checks": record.get("qmp_resume_checks", []),
            "enter_events": enter_events,
            "cleanup_checks": record.get("qmp_cleanup_checks", []),
        }, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    record.update({
        "input_delivery_status": delivery_status,
        "input_delivery_timeout_seconds": INPUT_DELIVERY_TIMEOUT_SECONDS,
        "input_delivery_stability_seconds": INPUT_DELIVERY_STABILITY_SECONDS,
        "input_delivery_sample_delay_seconds":
            INPUT_DELIVERY_SAMPLE_DELAY_SECONDS,
        "input_key_stroke_atomic": True,
        "input_key_release_timeout_seconds": INPUT_KEY_RELEASE_TIMEOUT_SECONDS,
        "input_character_boundary":
            "qmp-stable-physical-atomic-stroke-confirmed-key-delivery",
        "input_delivery_started_monotonic": delivery_started,
        "input_delivery_completed_monotonic": time.monotonic(),
        "input_delivery_samples": delivery_samples,
        "input_delivery_character_count": len(expected),
        "input_shell_snapshot_policy":
            "matching-physical-reads-bracketing-keyboard-state",
        "qmp_text_events": text_events,
        "qmp_enter_events": enter_events,
        "qmp_input_log": qmp_log.name,
        "qmp_input_log_sha256": sha256(qmp_log),
        "qmp_input_manifest": transport_log.name,
    })
    write_manifest()
    record["qmp_input_manifest_sha256"] = sha256(transport_log)
    if delivery_status != "COMPLETE":
        last = delivery_samples[-1] if delivery_samples else {}
        observed_hex = last.get("observed_bytes_hex", "")
        try:
            observed = bytes.fromhex(observed_hex)
        except (TypeError, ValueError):
            observed = b""
        record.update({
            "pre_enter_status": delivery_status,
            "pre_enter_active": last.get("active"),
            "pre_enter_length": last.get("length"),
            "pre_enter_position": last.get("position"),
            "pre_enter_bytes_hex": observed_hex,
            "pre_enter_text": last.get("observed_text"),
            "pre_enter_observed_monotonic": last.get(
                "observed_monotonic", time.monotonic()),
            "qmp_enter_status": "NOT_SENT",
            "dispatcher_status": "NOT_ATTEMPTED",
            "guest_input_status": ("MISMATCH" if
                                   delivery_status == "MISMATCH" else
                                   "INCOMPLETE"),
            "guest_input_length": len(observed),
            "guest_input_bytes_hex": observed.hex(),
            "guest_input_text": (observed.decode("ascii") if
                                 observed.isascii() else None),
            "qmp_input_status": "FAIL",
            "qmp_input_completed_monotonic": time.monotonic(),
            "transport_status": "FAIL",
            "transport_completed_monotonic": time.monotonic(),
        })
        qmp.close()
        return

    base = transport_log.with_name(f"{artifact_prefix}-input-observer")
    argv = ["gdb", "-q", "-nx", "--interpreter=mi2", str(elf)]
    output = []
    commands = []
    process = None
    token = 1
    try:
        process = subprocess.Popen(
            argv, text=True, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, bufsize=1, encoding="utf-8",
            errors="replace")
        mi_reader_start(process)
        observer_deadline = min(command_deadline,
                                time.monotonic() +
                                INPUT_DISPATCH_TIMEOUT_SECONDS)
        mi_command(process, token, "-gdb-set confirm off", observer_deadline,
                   output, commands)
        token += 1
        mi_command(process, token, "-gdb-set pagination off", observer_deadline,
                   output, commands)
        token += 1
        mi_command(process, token, "-target-select remote " + str(gdb_socket),
                   observer_deadline, output, commands)
        token += 1
        active_response = mi_command(
            process, token,
            f"-data-read-memory-bytes 0x{addresses['g_shell_active']:x} 1",
            observer_deadline, output, commands)
        pre_active_token = token
        token += 1
        length_response = mi_command(
            process, token,
            f"-data-read-memory-bytes 0x{addresses['g_len']:x} 4",
            observer_deadline, output, commands)
        pre_length_token = token
        token += 1
        position_response = mi_command(
            process, token,
            f"-data-read-memory-bytes 0x{addresses['g_pos']:x} 4",
            observer_deadline, output, commands)
        pre_position_token = token
        token += 1
        pre_memory_response = mi_command(
            process, token,
            f"-data-read-memory-bytes 0x{addresses['g_buffer']:x} 256",
            observer_deadline, output, commands)
        pre_memory_token = token
        token += 1
        active = mi_memory_bytes(active_response, 1)[0]
        length = int.from_bytes(mi_memory_bytes(length_response, 4),
                                "little", signed=True)
        position = int.from_bytes(mi_memory_bytes(position_response, 4),
                                  "little", signed=True)
        pre_memory = mi_memory_bytes(pre_memory_response, 256)
        pre_observed = pre_memory.split(b"\0", 1)[0]
        pre_exact = (active == 1 and length == len(expected) and
                     position == len(expected) and pre_observed == expected)
        record.update({
            "pre_enter_status": "PASS" if pre_exact else "MISMATCH",
            "pre_enter_active": active,
            "pre_enter_length": length,
            "pre_enter_position": position,
            "pre_enter_bytes_hex": pre_observed.hex(),
            "pre_enter_text": (pre_observed.decode("ascii") if
                               pre_observed.isascii() else None),
            "pre_enter_active_response_token": pre_active_token,
            "pre_enter_length_response_token": pre_length_token,
            "pre_enter_position_response_token": pre_position_token,
            "pre_enter_memory_response_token": pre_memory_token,
            "pre_enter_observed_monotonic": time.monotonic(),
        })
        if not pre_exact:
            record["qmp_enter_status"] = "NOT_SENT"
            record["dispatcher_status"] = "NOT_ATTEMPTED"
            record["guest_input_status"] = "MISMATCH"
            record["guest_input_length"] = len(pre_observed)
            record["guest_input_bytes_hex"] = pre_observed.hex()
            record["guest_input_text"] = record["pre_enter_text"]
            record["transport_status"] = "FAIL"
            close_gdb_observer(process, token, observer_deadline, output,
                               commands)
            process = None
        else:
            breakpoint_response = mi_command(
                process, token, "-break-insert shell_dispatch_command_line",
                observer_deadline, output, commands)
            breakpoint_token = token
            token += 1
            number = re.search(r'number="([1-9][0-9]*)"',
                               breakpoint_response)
            if number is None:
                raise RuntimeError("GDB MI did not arm the shell dispatcher")
            record["input_observer_armed_monotonic"] = time.monotonic()
            continued = mi_command(process, token, "-exec-continue",
                                   observer_deadline, output, commands)
            token += 1
            if not continued.endswith("^running"):
                raise RuntimeError("GDB MI did not resume the shell")
            record["input_observer_resumed_monotonic"] = time.monotonic()
            resume_started = time.monotonic()
            resume_checks = wait_qmp_running(qmp, observer_deadline)
            record.update({
                "qmp_resume_status": "PASS",
                "qmp_resume_timeout_seconds": INPUT_RESUME_TIMEOUT_SECONDS,
                "qmp_resume_poll_seconds": INPUT_RESUME_POLL_SECONDS,
                "qmp_resume_started_monotonic": resume_started,
                "qmp_resume_observed_monotonic": time.monotonic(),
                "qmp_resume_checks": resume_checks,
            })
            enter_down = dict(qmp.input(
                qmp_key_events("ret", True), observer_deadline), step="down")
            enter_events.append(enter_down)
            record["qmp_enter_down_completed_monotonic"] = \
                enter_down["completed_monotonic"]
            stop = mi_wait_stopped(process, observer_deadline, output)
            if ("reason=\"breakpoint-hit\"" not in stop or
                    "shell_dispatch_command_line" not in stop):
                raise RuntimeError("GDB stopped outside the shell dispatcher")
            memory_response = mi_command(
                process, token, "-data-read-memory-bytes $rdi 256",
                observer_deadline, output, commands)
            memory_token = token
            token += 1
            memory = mi_memory_bytes(memory_response, 256)
            observed = memory.split(b"\0", 1)[0]
            record["dispatcher_observed_monotonic"] = time.monotonic()
            deleted = mi_command(
                process, token, f"-break-delete {number.group(1)}",
                observer_deadline, output, commands)
            token += 1
            if not deleted.endswith("^done"):
                raise RuntimeError(
                    "GDB MI did not remove the dispatcher breakpoint")
            cleanup_started = time.monotonic()
            cleanup_deadline = min(
                observer_deadline,
                cleanup_started + INPUT_OBSERVER_CLEANUP_TIMEOUT_SECONDS)
            token, cleanup_result = detach_stopped_gdb_observer(
                process, token, cleanup_deadline, output, commands)
            process_completed = time.monotonic()
            record["input_observer_dispatch_resumed_monotonic"] = \
                cleanup_result["input_observer_target_detached_monotonic"]
            cleanup_resume_started = time.monotonic()
            cleanup_checks = wait_qmp_running(qmp, cleanup_deadline)
            record.update({
                "input_observer_cleanup_status": "PASS",
                "input_observer_cleanup_timeout_seconds":
                    INPUT_OBSERVER_CLEANUP_TIMEOUT_SECONDS,
                "input_observer_cleanup_started_monotonic": cleanup_started,
                "input_observer_process_completed_monotonic":
                    process_completed,
                "qmp_cleanup_started_monotonic": cleanup_resume_started,
                "qmp_cleanup_observed_monotonic": time.monotonic(),
                "qmp_cleanup_checks": cleanup_checks,
                "input_observer_cleanup_completed_monotonic":
                    time.monotonic(),
                **cleanup_result,
            })
            process = None
            enter_up = dict(qmp.input(
                qmp_key_events("ret", False), observer_deadline), step="up")
            enter_events.append(enter_up)
            record["qmp_enter_up_started_monotonic"] = \
                enter_up["started_monotonic"]
            record["qmp_enter_status"] = "PASS"
            record["qmp_enter_completed_monotonic"] = \
                enter_up["completed_monotonic"]
            enter_release_deadline = min(
                observer_deadline, time.monotonic() +
                INPUT_OBSERVER_CLEANUP_TIMEOUT_SECONDS)
            enter_release_samples = []
            while time.monotonic() < enter_release_deadline:
                release_number = len(enter_release_samples) + 1
                release_sample = qmp_input_snapshot(
                    qmp, addresses, physical_addresses, input_layout,
                    enter_release_deadline)
                release_sample["sample"] = release_number
                enter_release_samples.append(release_sample)
                if (release_sample["shell_snapshot_stable"] is True and
                        release_sample["keyboard_release_observed"] is True):
                    break
                time.sleep(min(
                    INPUT_DELIVERY_SAMPLE_DELAY_SECONDS,
                    max(0.0, enter_release_deadline - time.monotonic())))
            record.update({
                "qmp_enter_guest_release_status": (
                    "PASS" if enter_release_samples and
                    enter_release_samples[-1].get(
                        "shell_snapshot_stable") is True and
                    enter_release_samples[-1].get(
                        "keyboard_release_observed") is True else "TIMEOUT"),
                "qmp_enter_guest_release_samples": enter_release_samples,
            })
            if record["qmp_enter_guest_release_status"] != "PASS":
                raise RuntimeError(
                    "guest did not consume the Enter key release")
            record.update({
                "breakpoint_response_token": breakpoint_token,
                "input_observer_memory_response_token": memory_token,
                "dispatcher_stop": stop,
                "dispatcher_status": "OBSERVED",
                "guest_input_length": len(observed),
                "guest_input_bytes_hex": observed.hex(),
                "guest_input_text": (observed.decode("ascii") if
                                     observed.isascii() else None),
            })
            exact = observed == expected
            record["guest_input_status"] = "PASS" if exact else "MISMATCH"
            record["transport_status"] = "PASS" if exact else "FAIL"
        artifacts = write_gdb_observer_files(base, argv, output, commands)
        record["input_observer_log"] = artifacts["log"]
        record["input_observer_log_sha256"] = artifacts["log_sha256"]
        record["input_observer_command"] = artifacts["command"]
        record["input_observer_command_sha256"] = artifacts["command_sha256"]
        record["input_observer_argv"] = artifacts["argv"]
        record["input_observer_argv_sha256"] = artifacts["argv_sha256"]
        record["qmp_enter_events"] = enter_events
        write_manifest()
        record["qmp_input_log_sha256"] = sha256(qmp_log)
        record["qmp_input_manifest_sha256"] = sha256(transport_log)
        record["qmp_input_status"] = (
            "PASS" if record.get("qmp_text_status") == "PASS" and
            record.get("qmp_enter_status") == "PASS" else "FAIL")
        record["qmp_input_completed_monotonic"] = time.monotonic()
        record["transport_completed_monotonic"] = time.monotonic()
        qmp.close()
        return
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        if record.get("qmp_text_status") == "PASS":
            record["guest_input_status"] = "NOT_OBSERVED"
        try:
            close_gdb_observer(process, token,
                               min(command_deadline,
                                   time.monotonic() + 5.0),
                               output, commands)
        finally:
            artifacts = write_gdb_observer_files(base, argv, output, commands)
            record["input_observer_log"] = artifacts["log"]
            record["input_observer_log_sha256"] = artifacts["log_sha256"]
            record["input_observer_command"] = artifacts["command"]
            record["input_observer_command_sha256"] = artifacts["command_sha256"]
            record["input_observer_argv"] = artifacts["argv"]
            record["input_observer_argv_sha256"] = artifacts["argv_sha256"]
            record["qmp_enter_events"] = enter_events
            write_manifest()
            record["qmp_input_log_sha256"] = sha256(qmp_log)
            record["qmp_input_manifest_sha256"] = sha256(transport_log)
            qmp.close()
        raise RuntimeError(f"framed input observation failed: {error}") \
            from error

def command_production(args):
    root = Path(args.root).resolve()
    runtime = Path(args.runtime).resolve()
    serial_path = (Path(args.serial_path).resolve() if args.serial_path else
                   runtime / "qemu-serial.log")
    hmp_socket = (Path(args.hmp_socket).resolve() if args.hmp_socket else
                  runtime / "hmp.sock")
    gdb_socket = Path(args.gdb_socket).resolve()
    elf = Path(args.elf).resolve()
    record_path = Path(args.record).resolve()
    gdb_log = Path(args.gdb_log).resolve()
    transport_log = Path(args.transport_log).resolve()
    allowed = parse_allowed(args.allowed_status)
    validate_payload(args.payload)
    pid = args.qemu_pid if args.qemu_pid is not None else read_pid(runtime)
    try:
        os.kill(pid, 0)
    except OSError as error:
        raise RuntimeError("QEMU pid is absent or inactive") from error
    if not hmp_socket.is_socket() or not gdb_socket.is_socket():
        raise RuntimeError("production observer socket is absent")
    if not elf.is_file():
        raise RuntimeError("production ELF is absent")
    initial_lines, initial_ends, initial_total = complete_lines(serial_path)
    start_count = len(initial_lines)
    start_byte = initial_ends[-1] if initial_ends else 0
    command_file = gdb_log.with_suffix(".cmd")
    armed_path = (Path(args.observer_arm_file).resolve()
                  if args.observer_arm_file else
                  runtime / f"gdb-observer-{args.sequence}.armed")
    armed_path.unlink(missing_ok=True)
    command_log = []
    argv = ["gdb", "-q", "-nx", "--interpreter=mi2", str(elf)]
    (gdb_log.with_suffix(".argv.json")).write_text(
        json.dumps(argv, indent=2) + "\n")
    record = {
        "schema": 3, "kind": "observed-command",
        "profile_id": args.profile_id, "vm_id": args.vm_id,
        "candidate_sha256": args.candidate_sha256, "qemu_pid": pid,
        "sequence": args.sequence, "payload": args.payload,
        "payload_crc32": f"{zlib.crc32(args.payload.encode()) & 0xffffffff:08x}",
        "deadline_seconds": args.timeout,
        "start_line_count": start_count, "start_byte_count": start_byte,
        "serial_byte_count_at_trigger": initial_total,
        "trigger_monotonic": None, "observer_status": "PENDING",
        "handler_status": None, "classification": "PENDING",
        "gdb_log": gdb_log.name, "gdb_command": command_file.name,
        "observer_armed_file": armed_path.name,
    }
    output = []
    process = None
    deadline = time.monotonic() + args.timeout
    try:
        process = subprocess.Popen(
            argv, text=True, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, bufsize=1, encoding="utf-8",
            errors="replace")
        mi_reader_start(process)
        arm_deadline = min(deadline, time.monotonic() + 20)
        mi_command(process, 1, "-gdb-set confirm off", arm_deadline,
                   output, command_log)
        mi_command(process, 2, "-gdb-set pagination off", arm_deadline,
                   output, command_log)
        mi_command(process, 3, "-target-select remote " + str(gdb_socket),
                   arm_deadline, output,
                   command_log)
        record["observer_connected_monotonic"] = time.monotonic()
        entry_breakpoint = mi_command(
            process, 4, "-break-insert shell_dispatch_command_line",
            arm_deadline, output, command_log)
        entry_number = re.search(r'number="([1-9][0-9]*)"',
                                 entry_breakpoint)
        if entry_number is None:
            raise RuntimeError("GDB MI did not arm the dispatcher entry")
        record["entry_breakpoint_response"] = entry_breakpoint
        record["entry_breakpoint_number"] = int(entry_number.group(1))
        continued = mi_command(process, 5, "-exec-continue", arm_deadline,
                               output, command_log)
        if not continued.startswith("5^running"):
            raise RuntimeError("GDB MI did not resume under the breakpoint")
        armed_path.touch()
        record["observer_status"] = "ARMED"
        record["observer_armed_monotonic"] = time.monotonic()
        record["trigger_monotonic"] = time.monotonic()
        run_hmp(root, hmp_socket, "text", args.payload, "--enter",
                "--profile", args.input_profile,
                output_path=transport_log)
        record["input_completed_monotonic"] = time.monotonic()

        breakpoint_stop = mi_wait_stopped(process, deadline, output)
        if ("reason=\"breakpoint-hit\"" not in breakpoint_stop or
                "shell_dispatch_command_line" not in breakpoint_stop):
            raise RuntimeError("GDB stopped outside the shell dispatcher")
        record["breakpoint_monotonic"] = time.monotonic()
        record["breakpoint_stop"] = breakpoint_stop
        payload_memory = mi_command(
            process, 6, "-data-read-memory-bytes $rdi 97", deadline,
            output, command_log)
        contents_match = re.search(r'contents="([0-9a-fA-F]+)"',
                                   payload_memory)
        if contents_match is None:
            raise RuntimeError("GDB MI did not expose the shell input")
        payload_bytes = bytes.fromhex(contents_match.group(1)).split(b"\0", 1)[0]
        try:
            observed_payload = payload_bytes.decode("ascii")
        except UnicodeDecodeError as error:
            raise RuntimeError("GDB observed a non-ASCII shell input") from error
        if observed_payload != args.payload:
            raise RuntimeError("GDB breakpoint observed another shell input")
        record["payload_memory_response"] = payload_memory
        record["observed_payload"] = observed_payload
        entry_deleted = mi_command(
            process, 7,
            f'-break-delete {record["entry_breakpoint_number"]}',
            deadline, output, command_log)
        if not entry_deleted.startswith("7^done"):
            raise RuntimeError("GDB MI did not remove the dispatcher entry")
        record["entry_breakpoint_delete_response"] = entry_deleted
        return_address_response = mi_command(
            process, 8,
            '-data-evaluate-expression "*(unsigned long long*)($rbp+8)"',
            deadline, output, command_log)
        return_match = re.search(
            r'value="(0x[0-9a-fA-F]+|[0-9]+)"', return_address_response)
        if return_match is None:
            raise RuntimeError("GDB MI did not expose the dispatcher return")
        return_address = int(return_match.group(1), 0)
        record["return_address_response"] = return_address_response
        record["return_address"] = return_address
        return_breakpoint = mi_command(
            process, 9, f"-break-insert -t *0x{return_address:x}", deadline,
            output, command_log)
        breakpoint_number = re.search(r'number="([1-9][0-9]*)"',
                                      return_breakpoint)
        if breakpoint_number is None:
            raise RuntimeError("GDB MI did not arm the dispatcher return")
        record["return_breakpoint_response"] = return_breakpoint
        record["return_breakpoint_number"] = int(breakpoint_number.group(1))
        continued_to_return = mi_command(
            process, 10, "-exec-continue", deadline, output, command_log)
        if not continued_to_return.startswith("10^running"):
            raise RuntimeError("GDB MI did not resume the handler")

        if args.modal_screenshot:
            modal_seen = False
            while time.monotonic() < deadline:
                lines, _, _ = complete_lines(serial_path)
                matches = [index for index, line in
                           enumerate(lines[start_count:], start_count + 1)
                           if line == OWNER_RECORD]
                if matches:
                    modal_seen = True
                    record["modal_owner_line"] = matches[0]
                    break
                if process.poll() is not None:
                    break
                time.sleep(0.1)
            if not modal_seen:
                raise RuntimeError("production modal owner was not observed")
            record["modal_settle_seconds"] = MODAL_SETTLE_SECONDS
            record["modal_settle_started_monotonic"] = time.monotonic()
            if record["modal_settle_started_monotonic"] + \
                    MODAL_SETTLE_SECONDS > deadline:
                raise RuntimeError("production modal settle exceeded deadline")
            time.sleep(MODAL_SETTLE_SECONDS)
            screenshot = Path(args.modal_screenshot).resolve()
            screenshot.parent.mkdir(parents=True, exist_ok=True)
            run_hmp(root, hmp_socket, "command",
                    f"screendump {hmp_string(str(screenshot))}",
                    output_path=transport_log.with_name(
                        transport_log.stem + "-screendump.log"))
            if not screenshot.is_file():
                raise RuntimeError("production screendump is absent")
            record["modal_screenshot"] = {
                "path": screenshot.name, "bytes": screenshot.stat().st_size,
                "sha256": sha256(screenshot),
            }
            record["modal_screenshot_monotonic"] = time.monotonic()
            run_hmp(root, hmp_socket, "key", "esc", "--profile",
                    args.input_profile,
                    output_path=transport_log.with_name(
                        transport_log.stem + "-esc.log"))
            record["modal_escape_monotonic"] = time.monotonic()

        finish_stop = mi_wait_stopped(process, deadline, output)
        if ("reason=\"breakpoint-hit\"" not in finish_stop or
                f'bkptno="{record["return_breakpoint_number"]}"' not in
                finish_stop):
            raise RuntimeError("GDB did not stop at the dispatcher return")
        record["handler_return_monotonic"] = time.monotonic()
        record["return_stop"] = finish_stop
        return_pc_response = mi_command(
            process, 11, '-data-evaluate-expression "(unsigned long long)$pc"',
            deadline, output, command_log)
        pc_match = re.search(r'value="(0x[0-9a-fA-F]+|[0-9]+)"',
                             return_pc_response)
        if pc_match is None or int(pc_match.group(1), 0) != return_address:
            raise RuntimeError("GDB stopped at an unexpected return address")
        record["return_pc_response"] = return_pc_response
        evaluated = mi_command(
            process, 12, '-data-evaluate-expression "(long)$rax"',
            deadline, output, command_log)
        value = re.search(r'value=\"(-?(?:0x[0-9a-fA-F]+|[0-9]+))\"',
                          evaluated)
        if value is None:
            raise RuntimeError("GDB MI did not expose the handler status")
        record["handler_status_response"] = evaluated
        record["handler_status"] = int(value.group(1), 0)
        if record["handler_status"] not in allowed:
            record["classification"] = "HANDLER_STATUS_REJECTED"
        else:
            record["classification"] = "PASS"
        record["observer_status"] = "COMPLETED"
        mi_command(process, 13, "-target-detach", deadline, output,
                   command_log)
        mi_command(process, 14, "-gdb-exit", deadline, output, command_log)
        remaining = max(0.1, deadline - time.monotonic())
        process.wait(timeout=remaining)
        mi_reader_finish(process, output)
        record["gdb_return_code"] = process.returncode
        if process.returncode != 0:
            raise RuntimeError("GDB MI exited with a nonzero status")
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        record["classification"] = (
            "OBSERVER_TIMEOUT" if isinstance(error, subprocess.TimeoutExpired)
            else "OBSERVER_FAILURE")
        record["error"] = str(error)
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
        if process is not None:
            if hasattr(process, "mi_reader"):
                mi_reader_finish(process, output)
            record["gdb_return_code"] = process.returncode
    time.sleep(0.2)
    lines, ends, total_bytes = complete_lines(serial_path)
    record["end_line_count"] = len(lines)
    record["end_byte_count"] = ends[-1] if ends else 0
    record["serial_byte_count_observed"] = total_bytes
    record["completed_monotonic"] = time.monotonic()
    record["elapsed_seconds"] = (
        record["completed_monotonic"] -
        (record["trigger_monotonic"] or record["completed_monotonic"]))
    gdb_log.write_text("".join(output))
    command_file.write_text("\n".join(command_log) + "\n")
    record["gdb_log_sha256"] = sha256(gdb_log)
    record["gdb_command_sha256"] = sha256(command_file)
    append_json(record_path, record)
    print(json.dumps(record, sort_keys=True))
    return 0 if record["classification"] == "PASS" else 1


def build_parser():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    ready = sub.add_parser("wait-ready")
    ready.add_argument("--runtime", required=True)
    ready.add_argument("--marker", required=True)
    ready.add_argument("--timeout", type=float, required=True)
    ready.add_argument("--output", required=True)
    ready.set_defaults(function=command_wait_ready)

    frame = sub.add_parser("frame")
    frame.add_argument("--root", required=True)
    frame.add_argument("--runtime", required=True)
    frame.add_argument("--gdb-socket", required=True)
    frame.add_argument("--elf", required=True)
    frame.add_argument("--input-layout", required=True)
    frame.add_argument("--record", required=True)
    frame.add_argument("--transport-log", required=True)
    frame.add_argument("--profile-id", required=True)
    frame.add_argument("--vm-id", required=True)
    frame.add_argument("--candidate-sha256", required=True)
    frame.add_argument("--sequence", type=int, required=True)
    frame.add_argument("--payload", required=True)
    frame.add_argument("--timeout", type=float, required=True)
    frame.add_argument("--allowed-status", default="0")
    frame.add_argument("--input-profile", choices=("normal", "stress",
                                                    "sync", "framed"),
                       default="normal")
    frame.add_argument("--modal-screenshot")
    frame.set_defaults(function=command_frame)

    rejection = sub.add_parser("reject-frame")
    rejection.add_argument("--root", required=True)
    rejection.add_argument("--runtime", required=True)
    rejection.add_argument("--gdb-socket", required=True)
    rejection.add_argument("--elf", required=True)
    rejection.add_argument("--input-layout", required=True)
    rejection.add_argument("--record", required=True)
    rejection.add_argument("--transport-log", required=True)
    rejection.add_argument("--profile-id", required=True)
    rejection.add_argument("--vm-id", required=True)
    rejection.add_argument("--candidate-sha256", required=True)
    rejection.add_argument("--sequence", type=int, required=True)
    rejection.add_argument("--payload", required=True)
    rejection.add_argument("--crc", required=True)
    rejection.add_argument("--marker-name", required=True)
    rejection.add_argument("--timeout", type=float, required=True)
    rejection.add_argument("--input-profile", choices=("normal", "stress",
                                                        "sync", "framed"),
                           default="framed")
    rejection.set_defaults(function=command_reject_frame)

    production = sub.add_parser("production-command")
    production.add_argument("--root", required=True)
    production.add_argument("--runtime", required=True)
    production.add_argument("--serial-path")
    production.add_argument("--hmp-socket")
    production.add_argument("--qemu-pid", type=int)
    production.add_argument("--observer-arm-file")
    production.add_argument("--gdb-socket", required=True)
    production.add_argument("--elf", required=True)
    production.add_argument("--record", required=True)
    production.add_argument("--gdb-log", required=True)
    production.add_argument("--transport-log", required=True)
    production.add_argument("--profile-id", required=True)
    production.add_argument("--vm-id", required=True)
    production.add_argument("--candidate-sha256", required=True)
    production.add_argument("--sequence", type=int, required=True)
    production.add_argument("--payload", required=True)
    production.add_argument("--timeout", type=float, required=True)
    production.add_argument("--allowed-status", default="0")
    production.add_argument("--input-profile", choices=("normal", "stress",
                                                         "sync", "framed"),
                            default="normal")
    production.add_argument("--modal-screenshot")
    production.set_defaults(function=command_production)
    return parser


def main():
    args = build_parser().parse_args()
    try:
        return args.function(args)
    except (OSError, RuntimeError, ValueError, socket.error) as error:
        print(f"foundation-qemu.py: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
