#!/usr/bin/env python3
"""Closed-by-default verifier for current foundation runtime evidence."""

import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import zlib


EXPECTED_PROFILES = {
    "q35-tcg-smp1": ("q35", "tcg", 1, "basic"),
    "q35-tcg-smp2": ("q35", "tcg", 2, "basic"),
    "q35-kvm-smp4": ("q35", "kvm", 4, "full"),
    "q35-kvm-smp8": ("q35", "kvm", 8, "basic"),
    "q35-kvm-smp24": ("q35", "kvm", 24, "soak"),
    "pc-tcg-smp4": ("pc", "tcg", 4, "basic"),
}
FOCAL_PROFILES = {
    "q35-kvm-smp4": ("q35", "kvm", 4),
    "q35-kvm-smp8": ("q35", "kvm", 8),
    "q35-kvm-smp24": ("q35", "kvm", 24),
}
PRODUCTION_PROFILES = {
    "q35-tcg-smp1": ("q35", "tcg", 1, "smoke", (
        "irq check", "irq boot", "accounttest lapic-liveness 500")),
    "q35-kvm-smp4": ("q35", "kvm", 4, "full", (
        "irq check", "irq controllers", "irq routes", "irq boot",
        "accounttest lapic-config", "accounttest lapic-liveness 500",
        "accounttest lapic-rate 2000 5", "reaptest timer-ref",
        "reaptest timer-ref-competing", "accounttest check",
        "taskdiag check", "inputtest check", "modaltest check",
        "taskmantest check", "clear", "taskman 1000",
        "taskmantest stats")),
    "q35-kvm-smp8": ("q35", "kvm", 8, "diagnostics", (
        "irq check", "irq boot", "accounttest lapic-liveness 500",
        "reaptest timer-ref", "reaptest timer-ref-competing")),
    "q35-kvm-smp24": ("q35", "kvm", 24, "diagnostics", (
        "irq check", "irq boot", "accounttest lapic-liveness 500",
        "reaptest timer-ref", "reaptest timer-ref-competing",
        "accounttest lapic-rate 2000 5")),
}
BASIC_COMMANDS = (
    "irq check", "irq controllers", "irq routes", "irq boot",
    "accounttest lapic-config", "accounttest lapic-liveness 500",
)
FULL_COMMANDS = BASIC_COMMANDS + (
    "accounttest check", "accounttest lapic-rate 2000 5",
    "synctest sleep", "synctest timer-order",
    "synctest timer-cancel", "synctest timer-backlog",
    "reaptest timer-ref", "reaptest timer-ref-competing",
    "synctest check", "taskdiag check",
    "inputtest check", "modaltest check", "taskmantest check",
    "clear", "taskman 1000", "taskmantest stats",
)
FULL_COMMAND_COUNTS = {payload: 1 for payload in FULL_COMMANDS}
FULL_COMMAND_COUNTS["reaptest timer-ref"] = 3
FULL_COMMAND_COUNTS["reaptest timer-ref-competing"] = 3
ACCEPT_RE = re.compile(
    r"^\[HARNESS\]\[FRAME\] ACCEPT seq=([1-9][0-9]*) "
    r"crc=([0-9a-f]{8}) len=([0-9]+)$")
BEGIN_RE = re.compile(r"^\[HARNESS\]\[BEGIN\] seq=([1-9][0-9]*)$")
END_RE = re.compile(
    r"^\[HARNESS\]\[END\] seq=([1-9][0-9]*) status=(-?[0-9]+)$")
REPLAY_RE = re.compile(
    r"^\[HARNESS\]\[REPLAY\] seq=([1-9][0-9]*) status=(-?[0-9]+)$")
REJECT_RE = re.compile(
    r"^\[HARNESS\]\[FRAME\] REJECT seq=([1-9][0-9]*) (.+)$")
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


def read_env(path):
    values = {}
    for line in path.read_text(errors="replace").splitlines():
        if "=" in line:
            name, value = line.split("=", 1)
            values[name] = value.strip().strip("'")
    return values


def read_commands(path):
    rows = []
    if not path.is_file():
        return rows
    for number, line in enumerate(path.read_text(errors="replace").splitlines(),
                                  1):
        if not line:
            continue
        try:
            value = json.loads(line)
        except json.JSONDecodeError as error:
            rows.append({"malformed": f"line {number}: {error}"})
            continue
        if not isinstance(value, dict):
            rows.append({"malformed": f"line {number}: not an object"})
        else:
            rows.append(value)
    return rows


def line_matches(lines, regex, sequence=None):
    found = []
    for number, line in enumerate(lines, 1):
        match = regex.fullmatch(line)
        if match and (sequence is None or match.group(1) == str(sequence)):
            found.append((number, match))
    return found


def fields(line, prefix):
    if not line.startswith(prefix):
        return None
    suffix = line[len(prefix):]
    if suffix and not suffix.startswith(" "):
        return None
    result = {}
    for token in suffix.strip().split():
        if "=" not in token:
            return None
        name, value = token.split("=", 1)
        if not name or not value or name in result:
            return None
        result[name] = value
    return result


def exact_records(lines, prefix):
    return [(number, line, fields(line, prefix))
            for number, line in enumerate(lines, 1)
            if fields(line, prefix) is not None]


def int_field(values, name):
    try:
        return int(values[name], 0)
    except (KeyError, TypeError, ValueError):
        return None


def transaction_segment(transcript, row):
    try:
        start = int(row["start_line_count"])
        end = int(row["end_line_count"])
    except (KeyError, TypeError, ValueError):
        return None
    if start < 0 or end <= start or end > len(transcript):
        return None
    return transcript[start:end]


def command_recorded_in_range(transcript, row, marker=None):
    """Use a half-open line-count interval; never consume the next line."""
    lines = transcript.splitlines() if isinstance(transcript, str) else transcript
    segment = transaction_segment(lines, row)
    if segment is None:
        return False, "command line range is invalid"
    target = marker if marker is not None else row.get("marker", "")
    return (target in segment,
            "command result is absent from its causal transcript slice")


def verify_modal_timing(row, errors, production=False):
    if row.get("payload") != "taskman 1000":
        return
    required = ("modal_owner_line", "modal_settle_seconds",
                "modal_settle_started_monotonic", "modal_screenshot_monotonic",
                "modal_escape_monotonic", "modal_screenshot")
    if not all(name in row for name in required):
        errors.append("TASKMAN observation lacks settle or timing evidence")
        return
    try:
        trigger = float(row["trigger_monotonic"])
        settle_started = float(row["modal_settle_started_monotonic"])
        screenshot_time = float(row["modal_screenshot_monotonic"])
        escape_time = float(row["modal_escape_monotonic"])
        completed = float(row["completed_monotonic"])
        settle = float(row["modal_settle_seconds"])
    except (TypeError, ValueError):
        errors.append("TASKMAN observation timing is malformed")
        return
    if (settle < 2.0 or
            not (trigger <= settle_started and
                 screenshot_time - settle_started >= settle and
                 screenshot_time <= escape_time <= completed)):
        errors.append("TASKMAN screenshot/escape timing is not causal")
    if production:
        try:
            returned = float(row["handler_return_monotonic"])
        except (KeyError, TypeError, ValueError):
            errors.append("production TASKMAN return timing is malformed")
        else:
            if escape_time > returned:
                errors.append("production TASKMAN escaped after handler return")


def verify_modal_artifact(row, screenshot, errors):
    value = row.get("modal_screenshot")
    if not isinstance(value, dict):
        errors.append("TASKMAN screenshot receipt is absent")
        return
    if (not screenshot.is_file() or screenshot.stat().st_size < 100000 or
            not screenshot.read_bytes().startswith(b"P6")):
        errors.append("TASKMAN screendump is missing or invalid")
        return
    if (value.get("path") != screenshot.name or
            value.get("bytes") != screenshot.stat().st_size or
            value.get("sha256") != sha256(screenshot)):
        errors.append("TASKMAN screenshot receipt differs from the artifact")


def mi_memory_from_log(text, token, size):
    matches = re.findall(
        rf'(?m)^{token}\^done,[^\n]*contents="([0-9a-fA-F]+)"[^\n]*$',
        text)
    if len(matches) != 1:
        raise ValueError("GDB memory response is missing or duplicated")
    value = bytes.fromhex(matches[0])
    if len(value) != size:
        raise ValueError("GDB memory response size is invalid")
    return value


def validated_input_layout(row, profile_dir, errors):
    name = row.get("input_layout_artifact")
    if not isinstance(name, str) or Path(name).name != name or not name:
        errors.append("command input layout artifact name is unsafe")
        return None
    path = profile_dir / name
    if not path.is_file():
        errors.append("command input layout artifact is absent")
        return None
    if row.get("input_layout_sha256") != sha256(path):
        errors.append("command input layout artifact hash differs")
        return None
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError):
        errors.append("command input layout artifact is malformed")
        return None
    if value != row.get("input_layout") or set(value) != {
            "schema", "controller_size", "slot_count", "key_count",
            *(item for _label, offset, size in INPUT_LAYOUT_ARRAYS
              for item in (offset, size))}:
        errors.append("command input layout differs from the ledger")
        return None
    if (value.get("schema") != 1 or value.get("slot_count") != 64 or
            value.get("key_count") != 6 or
            not isinstance(value.get("controller_size"), int) or
            isinstance(value.get("controller_size"), bool) or
            value["controller_size"] <= 0):
        errors.append("command input layout dimensions are invalid")
        return None
    expected_sizes = {
        "prev_keys": value["slot_count"] * value["key_count"],
        "prev_mods": value["slot_count"],
        "repeat_key": value["slot_count"],
        "repeat_mods": value["slot_count"],
        "repeat_active": value["slot_count"],
    }
    ranges = []
    for label, offset_name, size_name in INPUT_LAYOUT_ARRAYS:
        offset, size = value.get(offset_name), value.get(size_name)
        if (not isinstance(offset, int) or isinstance(offset, bool) or
                not isinstance(size, int) or isinstance(size, bool) or
                offset < 0 or size != expected_sizes[label] or
                offset + size > value["controller_size"]):
            errors.append("command input layout range is invalid")
            return None
        ranges.append((offset, offset + size))
    ordered = sorted(ranges)
    if any(left[1] > right[0] for left, right in zip(ordered, ordered[1:])):
        errors.append("command input layout ranges overlap")
        return None
    return value


def keyboard_state_from_gdb(raw, sample, layout):
    tokens = sample.get("keyboard_response_tokens")
    if not isinstance(tokens, dict) or set(tokens) != {
            item[0] for item in INPUT_LAYOUT_ARRAYS}:
        raise ValueError("keyboard response token inventory differs")
    values = {}
    for label, _offset_name, size_name in INPUT_LAYOUT_ARRAYS:
        values[label] = mi_memory_from_log(
            raw, tokens[label], layout[size_name])
    slot_count, key_count = layout["slot_count"], layout["key_count"]
    active_slots = []
    for slot in range(slot_count):
        start = slot * key_count
        if (any(values["prev_keys"][start:start + key_count]) or
                values["prev_mods"][slot] or values["repeat_key"][slot] or
                values["repeat_mods"][slot] or
                values["repeat_active"][slot]):
            active_slots.append(slot)
    released = all(not any(value) for value in values.values())
    return values, active_slots, released


def keyboard_sample_errors(sample, raw, commands, parsed_symbols, layout):
    errors = []
    try:
        values, active_slots, released = keyboard_state_from_gdb(
            raw, sample, layout)
    except (TypeError, ValueError, KeyError):
        return ["keyboard release state cannot be derived from GDB"]
    expected_hex = {name: value.hex() for name, value in values.items()}
    if (sample.get("keyboard_state_hex") != expected_hex or
            sample.get("keyboard_active_slots") != active_slots or
            sample.get("keyboard_release_observed") is not released):
        errors.append("keyboard release summary differs from GDB")
    controller = parsed_symbols.get("xhci_driver")
    if not isinstance(controller, int):
        errors.append("xHCI input state symbol is absent")
    elif commands is not None:
        for _label, offset_name, size_name in INPUT_LAYOUT_ARRAYS:
            expected = (f"-data-read-memory-bytes 0x"
                        f"{controller + layout[offset_name]:x} "
                        f"{layout[size_name]}")
            if expected not in commands:
                errors.append("keyboard observer command uses another range")
    return errors


def input_address_map_errors(row, profile_dir, parsed_symbols):
    """Rebuild the virtual-to-physical map from the candidate ELF."""
    errors = []
    name = row.get("input_address_map_artifact")
    if not isinstance(name, str) or Path(name).name != name or not name:
        return None, ["command input address-map artifact name is unsafe"]
    path = profile_dir / name
    if not path.is_file():
        return None, ["command input address-map artifact is absent"]
    if row.get("input_address_map_sha256") != sha256(path):
        return None, ["command input address-map artifact hash differs"]
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError):
        return None, ["command input address-map artifact is malformed"]
    candidate = profile_dir.parent.parent / "candidate" / "kernel.elf"
    if not candidate.is_file() or sha256(candidate) != row.get(
            "candidate_sha256"):
        return None, ["command input address-map candidate is absent or differs"]
    nm_command = ["nm", "-n", str(candidate)]
    nm_result = subprocess.run(nm_command, text=True, capture_output=True,
                               check=False)
    readelf_command = ["readelf", "-lW", str(candidate)]
    readelf_result = subprocess.run(
        readelf_command, text=True, capture_output=True, check=False)
    if nm_result.returncode or readelf_result.returncode:
        return None, ["command input address-map tools failed"]
    wanted = set(parsed_symbols)
    found = {}
    for line in nm_result.stdout.splitlines():
        fields = line.split()
        if len(fields) >= 3 and fields[-1] in wanted:
            try:
                address = int(fields[0], 16)
            except ValueError:
                continue
            found.setdefault(fields[-1], []).append(address)
    if set(found) != wanted or any(len(items) != 1 for items in found.values()):
        return None, ["command input address-map symbols are not unique"]
    virtual = {name: values[0] for name, values in found.items()}
    if virtual != parsed_symbols:
        errors.append("command input virtual addresses differ from the ELF")
    segments = []
    for line in readelf_result.stdout.splitlines():
        fields = line.split()
        if len(fields) >= 6 and fields[0] == "LOAD":
            try:
                segments.append({
                    "offset": int(fields[1], 0),
                    "virtual_address": int(fields[2], 0),
                    "physical_address": int(fields[3], 0),
                    "file_size": int(fields[4], 0),
                    "memory_size": int(fields[5], 0),
                })
            except ValueError:
                errors.append("command input LOAD entry is malformed")
                return None, errors
    physical = {}
    for symbol, address in virtual.items():
        matches = [segment for segment in segments
                   if segment["virtual_address"] <= address <
                   segment["virtual_address"] + segment["memory_size"]]
        if len(matches) != 1:
            errors.append("command input symbol has no unique LOAD mapping")
            continue
        segment = matches[0]
        physical[symbol] = (segment["physical_address"] + address -
                            segment["virtual_address"])
    formatted_virtual = {key: f"0x{item:016x}" for key, item in virtual.items()}
    formatted_physical = {key: f"0x{item:016x}" for key, item in
                          physical.items()}
    expected = {
        "schema": 1,
        "kind": "elf-input-physical-address-map",
        "elf_sha256": sha256(candidate),
        "readelf_argv": value.get("readelf_argv"),
        "readelf_stdout": readelf_result.stdout,
        "load_segments": segments,
        "virtual_addresses": formatted_virtual,
        "physical_addresses": formatted_physical,
    }
    recorded_argv = value.get("readelf_argv")
    if (not isinstance(recorded_argv, list) or len(recorded_argv) != 3 or
            Path(str(recorded_argv[0])).name != "readelf" or
            recorded_argv[1] != "-lW" or
            Path(str(recorded_argv[2])).name != "kernel.elf"):
        errors.append("command input address-map readelf argv is invalid")
    if value != expected:
        errors.append("command input address-map artifact differs from the ELF")
    if (row.get("input_load_segments") != segments or
            row.get("input_symbol_physical_addresses") != formatted_physical):
        errors.append("command input physical-address ledger differs")
    return physical, errors


def decode_hmp_physical_bytes(value, physical_address, size):
    if not isinstance(value, str) or size <= 0:
        raise ValueError("invalid physical-memory response")
    result = bytearray()
    for line in value.splitlines():
        match = re.fullmatch(
            r"([0-9a-fA-F]{16}):((?: 0x[0-9a-fA-F]{2})+)", line)
        if match is None or int(match.group(1), 16) != \
                physical_address + len(result):
            raise ValueError("invalid physical-memory row")
        result.extend(int(item, 16) for item in
                      re.findall(r"0x([0-9a-fA-F]{2})", match.group(2)))
    if len(result) != size:
        raise ValueError("invalid physical-memory size")
    return bytes(result)


def qmp_memory_event_bytes(event, physical_address, size, qmp_records,
                           errors):
    if not isinstance(event, dict):
        errors.append("QMP physical-memory event is malformed")
        return None
    command_line = f"xp /{size}bx 0x{physical_address:x}"
    started, completed = (event.get("started_monotonic"),
                          event.get("completed_monotonic"))
    valid_time = (isinstance(started, (int, float)) and
                  not isinstance(started, bool) and math.isfinite(started) and
                  isinstance(completed, (int, float)) and
                  not isinstance(completed, bool) and
                  math.isfinite(completed) and started <= completed)
    if (not isinstance(event.get("request_id"), str) or
            event.get("qmp_execute") != "human-monitor-command" or
            event.get("arguments") != {"command-line": command_line} or
            event.get("physical_address") != physical_address or
            event.get("size") != size or not valid_time or
            event.get("expected_return") != event.get("response_return")):
        errors.append("QMP physical-memory event contract differs")
    qmp_records.append(event)
    try:
        data = decode_hmp_physical_bytes(
            event.get("response_return"), physical_address, size)
    except (TypeError, ValueError):
        errors.append("QMP physical-memory bytes cannot be decoded")
        return None
    if event.get("bytes_hex") != data.hex():
        errors.append("QMP physical-memory byte summary differs")
    return data


def qmp_snapshot_values(sample, virtual, physical, layout, qmp_records,
                        errors):
    if not isinstance(sample, dict) or sample.get("observer") not in (
            "qmp-physical-memory", "qmp-stable-physical-memory"):
        errors.append("QMP input snapshot identity is invalid")
        return None
    stable_observer = sample.get("observer") == "qmp-stable-physical-memory"
    shell_base = virtual["g_buffer"]
    shell_size = virtual["g_shell_active"] + 1 - shell_base
    shell_before_event = sample.get(
        "shell_memory_before_event" if stable_observer else
        "shell_memory_event")
    shell_before = qmp_memory_event_bytes(
        shell_before_event, physical["g_buffer"], shell_size, qmp_records,
        errors)
    keyboard_start = min(layout[item[1]] for item in INPUT_LAYOUT_ARRAYS)
    keyboard_end = max(layout[item[1]] + layout[item[2]]
                       for item in INPUT_LAYOUT_ARRAYS)
    keyboard_event = sample.get("keyboard_memory_event")
    keyboard_data = qmp_memory_event_bytes(
        keyboard_event,
        physical["xhci_driver"] + keyboard_start,
        keyboard_end - keyboard_start, qmp_records, errors)
    if stable_observer:
        shell_after_event = sample.get("shell_memory_after_event")
        shell_after = qmp_memory_event_bytes(
            shell_after_event, physical["g_buffer"], shell_size,
            qmp_records, errors)
    else:
        shell_after_event = shell_before_event
        shell_after = shell_before
    if shell_before is None or shell_after is None or keyboard_data is None:
        return None
    shell_snapshot_stable = shell_before == shell_after
    if (stable_observer and
            sample.get("shell_snapshot_stable") is not
            shell_snapshot_stable):
        errors.append("QMP shell stability summary differs from raw memory")
    if stable_observer:
        def event_time(event, field):
            return event.get(field) if isinstance(event, dict) else None

        event_times = (
            event_time(shell_before_event, "started_monotonic"),
            event_time(shell_before_event, "completed_monotonic"),
            event_time(keyboard_event, "started_monotonic"),
            event_time(keyboard_event, "completed_monotonic"),
            event_time(shell_after_event, "started_monotonic"),
            event_time(shell_after_event, "completed_monotonic"),
            sample.get("observed_monotonic"),
        )
        if (not all(isinstance(value, (int, float)) and
                    not isinstance(value, bool) and math.isfinite(value)
                    for value in event_times) or
                list(event_times) != sorted(event_times)):
            errors.append("QMP stable snapshot event order differs")
    shell_data = shell_after
    length_offset = virtual["g_len"] - shell_base
    position_offset = virtual["g_pos"] - shell_base
    active_offset = virtual["g_shell_active"] - shell_base
    if not (0 <= length_offset <= shell_size - 4 and
            0 <= position_offset <= shell_size - 4 and
            0 <= active_offset < shell_size):
        errors.append("QMP shell snapshot offsets are invalid")
        return None
    length = int.from_bytes(shell_data[length_offset:length_offset + 4],
                            "little", signed=True)
    position = int.from_bytes(shell_data[position_offset:position_offset + 4],
                              "little", signed=True)
    active = shell_data[active_offset]
    observed = shell_data[:length] if 0 <= length < length_offset else b""
    terminated = 0 <= length < length_offset and shell_data[length] == 0
    values = {}
    for label, offset_name, size_name in INPUT_LAYOUT_ARRAYS:
        start = layout[offset_name] - keyboard_start
        values[label] = keyboard_data[start:start + layout[size_name]]
    active_slots = []
    for slot in range(layout["slot_count"]):
        start = slot * layout["key_count"]
        if (any(values["prev_keys"][start:start + layout["key_count"]]) or
                values["prev_mods"][slot] or values["repeat_key"][slot] or
                values["repeat_mods"][slot] or
                values["repeat_active"][slot]):
            active_slots.append(slot)
    released = all(not any(item) for item in values.values())
    expected_hex = {name: item.hex() for name, item in values.items()}
    observed_text = observed.decode("ascii") if observed.isascii() else None
    if ((sample.get("active"), sample.get("length"),
         sample.get("position"), sample.get("terminated"),
         sample.get("observed_bytes_hex"), sample.get("observed_text")) !=
            (active, length, position, terminated, observed.hex(),
             observed_text)):
        errors.append("QMP shell summary differs from raw memory")
    if (sample.get("keyboard_state_hex") != expected_hex or
            sample.get("keyboard_active_slots") != active_slots or
            sample.get("keyboard_release_observed") is not released):
        errors.append("QMP keyboard-state summary differs from raw memory")
    return (active, length, position, terminated, observed, released,
            active_slots, shell_snapshot_stable)


def hmp_input_log_errors(path, expected_operation, expected_value,
                         expected_profile, require_release_boundary=False):
    errors = []
    if not path.is_file():
        return ["HMP input artifact is absent"]
    lines = path.read_text(errors="replace").splitlines()
    if len(lines) < 2 or not lines[0].startswith("argv="):
        return ["HMP input artifact lacks argv"]
    try:
        argv = json.loads(lines[0][5:])
    except (TypeError, ValueError, json.JSONDecodeError):
        return ["HMP input argv is malformed"]
    if (not isinstance(argv, list) or len(argv) < 8 or
            Path(str(argv[1])).name != "qemu_hmp.py"):
        errors.append("HMP input argv does not invoke qemu_hmp.py")
    try:
        operation_index = argv.index(expected_operation)
    except ValueError:
        errors.append("HMP input argv uses another operation")
    else:
        if (operation_index + 1 >= len(argv) or
                argv[operation_index + 1] != expected_value):
            errors.append("HMP input argv carries different bytes")
        suffix = argv[operation_index + 2:]
        if suffix != ["--profile", expected_profile]:
            errors.append("HMP input argv has unexpected framing options")
        if "--enter" in argv:
            errors.append("HMP text delivery included a terminator")
    if lines[1] != "return_code=0":
        errors.append("HMP input operation did not return zero")
    if require_release_boundary:
        expected = (f"[HMP][TEXT] chars={len(expected_value)} "
                    "release_key=shift hold_ms=30 delay_ms=100 "
                    "status=COMPLETE")
        if lines.count(expected) != 1:
            errors.append("HMP text release boundary is absent or duplicated")
    return errors


def expected_qmp_key_events(key, down):
    if len(key) == 1:
        if "a" <= key <= "z" or "0" <= key <= "9":
            qcode, modifiers = key, []
        elif key == " ":
            qcode, modifiers = "spc", []
        elif key == "-":
            qcode, modifiers = "minus", []
        elif key == ".":
            qcode, modifiers = "dot", []
        elif key == "+":
            qcode, modifiers = "equal", ["shift"]
        else:
            return None
    elif key in ("left", "right", "ret"):
        qcode, modifiers = key, []
    else:
        return None
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


def qmp_input_log_errors(path, event_records):
    errors = []
    try:
        entries = [json.loads(line) for line in path.read_text().splitlines()
                   if line.strip()]
    except (OSError, ValueError, json.JSONDecodeError):
        return ["QMP input log is malformed"]
    if not entries:
        return ["QMP input log is empty"]
    for entry in entries:
        if (not isinstance(entry, dict) or
                entry.get("direction") not in ("send", "receive") or
                not isinstance(entry.get("message"), dict) or
                not isinstance(entry.get("host_monotonic"), (int, float)) or
                isinstance(entry.get("host_monotonic"), bool) or
                not math.isfinite(entry.get("host_monotonic"))):
            errors.append("QMP input log row is malformed")
            return errors
    if (entries[0].get("direction") != "receive" or
            "QMP" not in entries[0].get("message", {})):
        errors.append("QMP input greeting is absent")

    sends = {}
    receives = {}
    for entry in entries:
        message = entry["message"]
        command_id = message.get("id")
        if not isinstance(command_id, str):
            continue
        target = sends if entry["direction"] == "send" else receives
        if command_id in target:
            errors.append("QMP input request or response ID is duplicated")
        target[command_id] = entry

    capability_ids = [command_id for command_id, entry in sends.items()
                      if entry["message"].get("execute") ==
                      "qmp_capabilities"]
    if len(capability_ids) != 1:
        errors.append("QMP capabilities handshake is not unique")
    elif (sends[capability_ids[0]]["message"] != {
            "execute": "qmp_capabilities", "id": capability_ids[0]} or
          receives.get(capability_ids[0], {}).get("message") != {
              "return": {}, "id": capability_ids[0]}):
        errors.append("QMP capabilities handshake is invalid")

    operation_ids = [command_id for command_id, entry in sends.items()
                     if entry["message"].get("execute") !=
                     "qmp_capabilities"]
    recorded_ids = []
    for record in event_records:
        if not isinstance(record, dict):
            errors.append("QMP input event record is malformed")
            continue
        command_id = record.get("request_id")
        recorded_ids.append(command_id)
        execute = record.get("qmp_execute", "input-send-event")
        arguments = record.get("arguments", {"events": record.get("events")})
        expected_return = record.get("expected_return", {})
        expected = {"execute": execute, "id": command_id}
        if arguments:
            expected["arguments"] = arguments
        send = sends.get(command_id)
        receive = receives.get(command_id)
        if not isinstance(command_id, str) or send is None:
            errors.append("QMP input request is absent")
            continue
        if send["message"] != expected:
            errors.append("QMP input request differs from its event record")
        if receive is None:
            errors.append("QMP input response is absent or failed")
            continue
        if receive["message"] != {
                "return": expected_return, "id": command_id}:
            errors.append("QMP input response is absent or failed")
        started = record.get("started_monotonic")
        completed = record.get("completed_monotonic")
        if (not isinstance(started, (int, float)) or
                isinstance(started, bool) or not math.isfinite(started) or
                not isinstance(completed, (int, float)) or
                isinstance(completed, bool) or not math.isfinite(completed) or
                not started <= send["host_monotonic"] <=
                receive["host_monotonic"] <= completed):
            errors.append("QMP input event timing is inconsistent")
    if (len(recorded_ids) != len(set(recorded_ids)) or
            set(operation_ids) != set(recorded_ids)):
        errors.append("QMP input request inventory differs from the ledger")
    return errors


def cursor_delivery_errors(row, profile_dir, expected, parsed_symbols):
    errors = []
    qmp_mode = row.get("schema") == 10

    def require(condition, message):
        if not condition:
            errors.append(message)

    def finite(value):
        return (isinstance(value, (int, float)) and
                not isinstance(value, bool) and math.isfinite(value))

    text_events = row.get("qmp_text_events" if qmp_mode else
                          "hmp_text_events")
    boundary_events = row.get("input_delivery_boundary_events")
    samples = row.get("input_delivery_samples")
    require(row.get("input_delivery_status") == "COMPLETE",
            "cursor-feedback delivery did not complete")
    require(row.get("qmp_text_status" if qmp_mode else
                    "hmp_text_status") == "PASS" and
            row.get("input_character_boundary_status") == "PASS" and
            row.get("input_character_boundary") ==
            ("qmp-keyup-cursor-left-right" if qmp_mode else
             "cursor-left-right"),
            "cursor-feedback text or character boundary did not complete")
    require(row.get("input_delivery_timeout_seconds") == 90.0 and
            row.get("input_delivery_stability_seconds") == 0.5 and
            row.get("input_delivery_sample_delay_seconds") == 0.05,
            "cursor-feedback delivery policy differs")
    require(row.get("input_delivery_character_count") == len(expected),
            "cursor-feedback byte count differs from the frame")
    require(isinstance(text_events, list) and
            len(text_events) == len(expected),
            "cursor-feedback text event count differs")
    require(isinstance(boundary_events, list) and
            len(boundary_events) == len(expected) * 2,
            "cursor-feedback boundary event count differs")
    require(isinstance(samples, list) and bool(samples),
            "cursor-feedback samples are absent")
    if not (isinstance(text_events, list) and
            isinstance(boundary_events, list) and
            isinstance(samples, list)):
        return errors
    if (len(text_events) != len(expected) or
            len(boundary_events) != len(expected) * 2 or not samples):
        return errors

    manifest_name = row.get("qmp_input_manifest" if qmp_mode else
                            "hmp_text_manifest")
    if (not isinstance(manifest_name, str) or
            Path(manifest_name).name != manifest_name or not manifest_name):
        errors.append("cursor-feedback manifest name is unsafe")
    else:
        manifest_path = profile_dir / manifest_name
        if not manifest_path.is_file():
            errors.append("cursor-feedback manifest is absent")
        elif row.get("qmp_input_manifest_sha256" if qmp_mode else
                     "hmp_text_manifest_sha256") != sha256(manifest_path):
            errors.append("cursor-feedback manifest hash differs")
        else:
            try:
                manifest = json.loads(manifest_path.read_text())
            except (OSError, ValueError, json.JSONDecodeError):
                errors.append("cursor-feedback manifest is malformed")
            else:
                expected_manifest = {
                    "schema": 1,
                    "kind": "cursor-feedback-transport",
                    "frame_bytes_hex": expected.hex(),
                    "text_events": text_events,
                    "boundary_events": boundary_events,
                }
                if qmp_mode:
                    expected_manifest = {
                        "schema": 2,
                        "kind": "qmp-key-transition-transport",
                        "frame_bytes_hex": expected.hex(),
                        "qmp_log": row.get("qmp_input_log"),
                        "text_events": text_events,
                        "boundary_events": boundary_events,
                        "final_release_event":
                            row.get("qmp_final_release_event"),
                        "enter_events": row.get("qmp_enter_events"),
                    }
                require(manifest == expected_manifest,
                        "cursor-feedback manifest differs from the ledger")
        if not qmp_mode:
            require(row.get("hmp_text_log") == manifest_name and
                    row.get("hmp_text_log_sha256") ==
                    row.get("hmp_text_manifest_sha256"),
                    "cursor-feedback manifest alias differs")

    artifact_names = []

    def validate_event(event, index, operation, value, label, step=None):
        if not isinstance(event, dict):
            errors.append(f"cursor-feedback {label} event is malformed")
            return
        require(event.get("character_index") == index and
                event.get("target_length") == index,
                f"cursor-feedback {label} index differs")
        if step is not None:
            require(event.get("step") == step and event.get("key") == value,
                    f"cursor-feedback {label} step differs")
            expected_pending = index if step == "left" else index - 1
            expected_position = index - 1 if step == "left" else index
            require(event.get("pending_position") == expected_pending and
                    event.get("expected_position") == expected_position,
                    f"cursor-feedback {label} cursor contract differs")
        started = event.get("started_monotonic")
        completed = event.get("completed_monotonic")
        require(finite(started) and finite(completed) and
                started <= completed,
                f"cursor-feedback {label} timing is invalid")
        if qmp_mode:
            require(isinstance(event.get("request_id"), str) and
                    isinstance(event.get("events"), list),
                    f"cursor-feedback {label} QMP event is malformed")
        else:
            name = event.get("log")
            if not isinstance(name, str) or Path(name).name != name or not name:
                errors.append(
                    f"cursor-feedback {label} artifact name is unsafe")
                return
            artifact_names.append(name)
            artifact = profile_dir / name
            if not artifact.is_file():
                errors.append(f"cursor-feedback {label} artifact is absent")
                return
            require(event.get("log_sha256") == sha256(artifact),
                    f"cursor-feedback {label} artifact hash differs")
            errors.extend(hmp_input_log_errors(
                artifact, operation, value, "framed"))

    qmp_records = []
    for index, byte in enumerate(expected, 1):
        text_event = text_events[index - 1]
        validate_event(text_event, index, "text", chr(byte), "text")
        if isinstance(text_event, dict):
            require(text_event.get("character_hex") == bytes([byte]).hex(),
                    "cursor-feedback text byte differs")
        left_event = boundary_events[(index - 1) * 2]
        right_event = boundary_events[(index - 1) * 2 + 1]
        validate_event(left_event, index, "key", "left", "left", "left")
        validate_event(right_event, index, "key", "right", "right", "right")
        if qmp_mode:
            character = chr(byte)
            expected_text = ([] if index == 1 else
                             expected_qmp_key_events("right", False))
            expected_text += expected_qmp_key_events(character, True)
            expected_left = (expected_qmp_key_events(character, False) +
                             expected_qmp_key_events("left", True))
            expected_right = (expected_qmp_key_events("left", False) +
                              expected_qmp_key_events("right", True))
            require(text_event.get("events") == expected_text,
                    "cursor-feedback QMP text transition differs")
            require(left_event.get("events") == expected_left,
                    "cursor-feedback QMP left transition differs")
            require(right_event.get("events") == expected_right,
                    "cursor-feedback QMP right transition differs")
            qmp_records.extend((text_event, left_event, right_event))
    if not qmp_mode:
        require(len(artifact_names) == len(set(artifact_names)),
                "cursor-feedback HMP artifacts are reused")
    else:
        final_release = row.get("qmp_final_release_event")
        enter_events = row.get("qmp_enter_events")
        require(isinstance(final_release, dict) and
                final_release.get("events") ==
                expected_qmp_key_events("right", False),
                "cursor-feedback final QMP key release differs")
        require(isinstance(enter_events, list) and len(enter_events) == 2,
                "cursor-feedback QMP terminator events differ")
        if isinstance(final_release, dict):
            qmp_records.append(final_release)
        if isinstance(enter_events, list) and len(enter_events) == 2:
            require(enter_events[0].get("step") == "down" and
                    enter_events[0].get("events") ==
                    expected_qmp_key_events("ret", True) and
                    enter_events[1].get("step") == "up" and
                    enter_events[1].get("events") ==
                    expected_qmp_key_events("ret", False),
                    "cursor-feedback QMP terminator transition differs")
            qmp_records.extend(enter_events)
        qmp_name = row.get("qmp_input_log")
        if (not isinstance(qmp_name, str) or
                Path(qmp_name).name != qmp_name or not qmp_name):
            errors.append("cursor-feedback QMP log name is unsafe")
        else:
            qmp_path = profile_dir / qmp_name
            if not qmp_path.is_file():
                errors.append("cursor-feedback QMP log is absent")
            elif row.get("qmp_input_log_sha256") != sha256(qmp_path):
                errors.append("cursor-feedback QMP log hash differs")
            else:
                errors.extend(qmp_input_log_errors(qmp_path, qmp_records))

    sample_names = []
    sample_times = []
    previous_length = -1
    character_samples = {index: [] for index in range(1, len(expected) + 1)}
    left_samples = {index: [] for index in range(1, len(expected) + 1)}
    right_samples = {index: [] for index in range(1, len(expected) + 1)}
    stability_samples = []
    require([sample.get("sample") if isinstance(sample, dict) else None
             for sample in samples] == list(range(1, len(samples) + 1)),
            "cursor-feedback sample sequence is invalid")
    for sample in samples:
        if not isinstance(sample, dict):
            errors.append("cursor-feedback sample is malformed")
            continue
        index = sample.get("character_index")
        target_length = sample.get("target_length")
        phase = sample.get("phase")
        valid_index = isinstance(index, int) and 1 <= index <= len(expected)
        require(valid_index and target_length == index,
                "cursor-feedback sample target is invalid")
        files = {}
        for name_field, hash_field in (("log", "log_sha256"),
                                       ("command", "command_sha256"),
                                       ("argv", "argv_sha256")):
            name = sample.get(name_field)
            if not isinstance(name, str) or Path(name).name != name or not name:
                errors.append("cursor-feedback sample artifact name is unsafe")
                continue
            sample_names.append(name)
            artifact = profile_dir / name
            files[name_field] = artifact
            if not artifact.is_file():
                errors.append("cursor-feedback sample artifact is absent")
            elif sample.get(hash_field) != sha256(artifact):
                errors.append("cursor-feedback sample artifact hash differs")
        log_path = files.get("log")
        command_path = files.get("command")
        if not log_path or not log_path.is_file() or not valid_index:
            continue
        raw = log_path.read_text(errors="replace")
        try:
            active = mi_memory_from_log(
                raw, sample.get("active_response_token"), 1)[0]
            length = int.from_bytes(mi_memory_from_log(
                raw, sample.get("length_response_token"), 4),
                "little", signed=True)
            position = int.from_bytes(mi_memory_from_log(
                raw, sample.get("position_response_token"), 4),
                "little", signed=True)
            memory = mi_memory_from_log(
                raw, sample.get("memory_response_token"), 256)
            observed = memory[:length] if 0 <= length < 256 else b""
            terminated = 0 <= length < 256 and memory[length] == 0
        except (TypeError, ValueError):
            errors.append("cursor-feedback values cannot be derived from GDB")
            continue
        target = expected[:index]
        if phase == "character":
            coherent = (active == 1 and position == length == len(observed) and
                        terminated)
            derived = ("EXACT" if coherent and observed == target else
                       ("PREFIX" if coherent and target.startswith(observed)
                        else "DIVERGED"))
            character_samples[index].append(sample)
        elif phase in ("cursor-left", "cursor-right"):
            expected_position = index - 1 if phase == "cursor-left" else index
            pending_position = index if phase == "cursor-left" else index - 1
            coherent = (active == 1 and length == len(observed) == len(target)
                        and terminated and observed == target)
            derived = ("EXACT" if coherent and position == expected_position
                       else ("PENDING" if coherent and
                             position == pending_position else "DIVERGED"))
            (left_samples if phase == "cursor-left" else
             right_samples)[index].append(sample)
        elif phase == "stability" and index == len(expected):
            coherent = (active == 1 and position == length == len(observed) and
                        terminated)
            derived = "EXACT" if coherent and observed == expected else "DIVERGED"
            stability_samples.append(sample)
        else:
            coherent = False
            derived = "DIVERGED"
            errors.append("cursor-feedback sample phase is invalid")
        observed_text = observed.decode("ascii") if observed.isascii() else None
        require(coherent and derived != "DIVERGED",
                "cursor-feedback sample is incoherent or diverged")
        require((sample.get("active"), sample.get("length"),
                 sample.get("position"), sample.get("terminated"),
                 sample.get("observed_bytes_hex"),
                 sample.get("observed_text"), sample.get("status")) ==
                (active, length, position, terminated, observed.hex(),
                 observed_text, derived),
                "cursor-feedback summary differs from GDB")
        require(length >= previous_length,
                "cursor-feedback buffer regressed")
        previous_length = length
        observed_time = sample.get("observed_monotonic")
        require(finite(observed_time),
                "cursor-feedback sample time is invalid")
        if finite(observed_time):
            sample_times.append(observed_time)
        if command_path and command_path.is_file() and parsed_symbols:
            commands = command_path.read_text(errors="replace")
            for name, size in (("g_shell_active", 1), ("g_len", 4),
                               ("g_pos", 4), ("g_buffer", 256)):
                require(
                    f"-data-read-memory-bytes 0x{parsed_symbols[name]:x} {size}" in
                    commands,
                    "cursor-feedback command uses another symbol")
            require("break-insert" not in commands and "$rdi" not in commands,
                    "cursor-feedback sample attempted dispatch inspection")

    require(len(sample_names) == len(set(sample_names)),
            "cursor-feedback sample artifacts are reused")
    require(sample_times == sorted(sample_times) and
            len(sample_times) == len(set(sample_times)),
            "cursor-feedback sample times are not increasing")

    def completed_once(group, pending):
        return (bool(group) and group[-1].get("status") == "EXACT" and
                all(item.get("status") == pending for item in group[:-1]))

    for index in range(1, len(expected) + 1):
        chars = character_samples[index]
        left = left_samples[index]
        right = right_samples[index]
        require(completed_once(chars, "PREFIX"),
                "cursor-feedback character prefix was not completed once")
        require(completed_once(left, "PENDING"),
                "cursor-feedback left boundary was not completed once")
        require(completed_once(right, "PENDING"),
                "cursor-feedback right boundary was not completed once")
        if not (chars and left and right):
            continue
        text_event = text_events[index - 1]
        left_event = boundary_events[(index - 1) * 2]
        right_event = boundary_events[(index - 1) * 2 + 1]
        values = (
            text_event.get("completed_monotonic") if
            isinstance(text_event, dict) else None,
            chars[-1].get("observed_monotonic"),
            left_event.get("started_monotonic") if
            isinstance(left_event, dict) else None,
            left_event.get("completed_monotonic") if
            isinstance(left_event, dict) else None,
            left[-1].get("observed_monotonic"),
            right_event.get("started_monotonic") if
            isinstance(right_event, dict) else None,
            right_event.get("completed_monotonic") if
            isinstance(right_event, dict) else None,
            right[-1].get("observed_monotonic"),
        )
        require(all(finite(value) for value in values) and
                list(values) == sorted(values),
                "cursor-feedback boundary is not causally ordered")
        if index < len(expected):
            next_started = (text_events[index].get("started_monotonic")
                            if isinstance(text_events[index], dict) else None)
            require(finite(next_started) and finite(values[-1]) and
                    values[-1] <= next_started,
                    "cursor-feedback next character preceded its boundary")

    require(len(stability_samples) == 1 and
            stability_samples[0].get("status") == "EXACT" and
            samples[-1] is stability_samples[0],
            "cursor-feedback final stability sample is absent")
    final_right = right_samples[len(expected)]
    if final_right and stability_samples:
        stable_time = stability_samples[0].get("observed_monotonic")
        boundary_time = (row.get("qmp_final_release_event", {}).get(
            "completed_monotonic") if qmp_mode else
            final_right[-1].get("observed_monotonic"))
        recorded = row.get("input_delivery_stable_span_seconds")
        require(finite(stable_time) and finite(boundary_time) and
                finite(recorded) and stable_time - boundary_time >= 0.5 and
                abs(recorded - (stable_time - boundary_time)) < 1e-9,
                "cursor-feedback stability interval differs")
    return errors


def confirmed_delivery_errors(row, profile_dir, expected, parsed_symbols,
                              input_layout, physical_addresses=None):
    """Reconcile press/consume/release delivery with QMP and GDB raw data."""
    errors = []

    def require(condition, message):
        if not condition:
            errors.append(message)

    def finite(value):
        return (isinstance(value, (int, float)) and
                not isinstance(value, bool) and math.isfinite(value))

    def validate_qmp_event(event, expected_events, label):
        if not isinstance(event, dict):
            errors.append(f"confirmed delivery {label} event is malformed")
            return False
        started = event.get("started_monotonic")
        completed = event.get("completed_monotonic")
        require(isinstance(event.get("request_id"), str) and
                event.get("events") == expected_events,
                f"confirmed delivery {label} transition differs")
        require(finite(started) and finite(completed) and started <= completed,
                f"confirmed delivery {label} timing is invalid")
        return True

    def validate_runstate(checks, label, qmp_records):
        require(isinstance(checks, list) and bool(checks),
                f"{label} runstate observations are absent")
        if not isinstance(checks, list) or not checks:
            return None
        for index, check in enumerate(checks):
            value = check.get("response_return") if isinstance(check, dict) \
                else None
            running = (isinstance(value, dict) and
                       value.get("running") is True and
                       value.get("status") == "running")
            require(isinstance(check, dict) and
                    check.get("qmp_execute") == "query-status" and
                    check.get("arguments") == {} and
                    check.get("expected_return") == value and
                    ((index == len(checks) - 1 and running) or
                     (index < len(checks) - 1 and not running)),
                    f"{label} runstate observation differs")
            if isinstance(check, dict):
                qmp_records.append(check)
        first = checks[0].get("started_monotonic")
        last = checks[-1].get("completed_monotonic")
        require(finite(first) and finite(last) and first <= last,
                f"{label} runstate timing is invalid")
        return first, last

    def sample_artifacts(sample, label):
        files = {}
        for name_field, hash_field in (("log", "log_sha256"),
                                       ("command", "command_sha256"),
                                       ("argv", "argv_sha256")):
            name = sample.get(name_field) if isinstance(sample, dict) else None
            if (not isinstance(name, str) or Path(name).name != name or
                    not name):
                errors.append(f"{label} artifact name is unsafe")
                continue
            path = profile_dir / name
            files[name_field] = path
            if not path.is_file():
                errors.append(f"{label} artifact is absent")
            elif sample.get(hash_field) != sha256(path):
                errors.append(f"{label} artifact hash differs")
        return files

    pulses = row.get("qmp_text_events")
    samples = row.get("input_delivery_samples")
    enter_events = row.get("qmp_enter_events")
    schema = row.get("schema")
    qmp_memory = schema in (24, 25, 26)
    qmp_atomic = schema in (25, 26)
    qmp_stable = schema == 26
    if qmp_memory and not isinstance(physical_addresses, dict):
        errors.append("QMP physical-address map is unavailable")
        return errors
    require(row.get("input_delivery_status") == "COMPLETE" and
            row.get("qmp_text_status") == "PASS" and
            row.get("input_character_boundary_status") == "PASS" and
            row.get("input_character_boundary") ==
            ("qmp-stable-physical-atomic-stroke-confirmed-key-delivery"
             if qmp_stable else
             ("qmp-physical-atomic-stroke-confirmed-key-delivery"
              if qmp_atomic else
             ("qmp-physical-press-release-confirmed-key-delivery"
              if qmp_memory else
              "qmp-guest-press-release-confirmed-key-delivery"))),
            "confirmed delivery did not complete")
    require(row.get("input_delivery_timeout_seconds") == 90.0 and
            row.get("input_delivery_stability_seconds") == 0.5 and
            row.get("input_delivery_sample_delay_seconds") == 0.05 and
            row.get("input_key_stroke_atomic") is qmp_atomic and
            (qmp_atomic or
             row.get("input_key_press_timeout_seconds") == 5.0) and
            row.get("input_key_release_timeout_seconds") == 5.0,
            "confirmed delivery policy differs")
    if qmp_stable:
        require(row.get("input_shell_snapshot_policy") ==
                "matching-physical-reads-bracketing-keyboard-state",
                "stable physical snapshot policy differs")
    require(row.get("input_delivery_character_count") == len(expected),
            "confirmed delivery byte count differs")
    require(isinstance(pulses, list) and len(pulses) == len(expected),
            "confirmed delivery event count differs")
    require(isinstance(samples, list) and bool(samples),
            "confirmed delivery samples are absent")
    require(isinstance(enter_events, list) and len(enter_events) == 2,
            "confirmed delivery terminator events differ")
    if (not isinstance(pulses, list) or len(pulses) != len(expected) or
            not isinstance(samples, list) or not samples or
            not isinstance(enter_events, list) or len(enter_events) != 2 or
            input_layout is None):
        return errors

    manifest_name = row.get("qmp_input_manifest")
    if (not isinstance(manifest_name, str) or
            Path(manifest_name).name != manifest_name or not manifest_name):
        errors.append("confirmed delivery manifest name is unsafe")
    else:
        manifest_path = profile_dir / manifest_name
        if not manifest_path.is_file():
            errors.append("confirmed delivery manifest is absent")
        elif row.get("qmp_input_manifest_sha256") != sha256(manifest_path):
            errors.append("confirmed delivery manifest hash differs")
        else:
            try:
                manifest = json.loads(manifest_path.read_text())
            except (OSError, ValueError, json.JSONDecodeError):
                errors.append("confirmed delivery manifest is malformed")
            else:
                expected_manifest = {
                    "schema": (17 if qmp_stable else
                               (16 if qmp_atomic else
                               (15 if qmp_memory else 14))),
                    "kind": (
                        "qmp-stable-physical-observed-atomic-stroke-transport"
                        if qmp_stable else
                        ("qmp-physical-observed-atomic-stroke-transport"
                         if qmp_atomic else
                             ("qmp-physical-observed-press-release-transport"
                              if qmp_memory else
                              "qmp-guest-press-release-confirmed-transport"))),
                    "frame_bytes_hex": expected.hex(),
                    "qmp_log": row.get("qmp_input_log"),
                    "input_layout_artifact":
                        row.get("input_layout_artifact"),
                    "input_layout_sha256": row.get("input_layout_sha256"),
                    "input_layout": row.get("input_layout"),
                    "key_pulses": pulses,
                    "resume_checks": row.get("qmp_resume_checks"),
                    "enter_events": enter_events,
                    "cleanup_checks": row.get("qmp_cleanup_checks"),
                }
                if qmp_stable:
                    expected_manifest["shell_snapshot_policy"] = \
                        "matching-physical-reads-bracketing-keyboard-state"
                if qmp_memory:
                    expected_manifest.update({
                        "input_address_map_artifact":
                            row.get("input_address_map_artifact"),
                        "input_address_map_sha256":
                            row.get("input_address_map_sha256"),
                    })
                require(manifest == expected_manifest,
                        "confirmed delivery manifest differs from the ledger")

    qmp_records = []
    if qmp_memory:
        boundary_samples = row.get("input_boundary_samples")
        require(isinstance(boundary_samples, list) and bool(boundary_samples),
                "QMP input boundary samples are absent")
        if isinstance(boundary_samples, list):
            require([item.get("sample") if isinstance(item, dict) else None
                     for item in boundary_samples] ==
                    list(range(1, len(boundary_samples) + 1)),
                    "QMP input boundary sample sequence is invalid")
            for sample_index, sample in enumerate(boundary_samples):
                values = qmp_snapshot_values(
                    sample, parsed_symbols, physical_addresses, input_layout,
                    qmp_records, errors)
                if values is None:
                    continue
                active, length, position, _terminated, _observed, released, \
                    _slots, snapshot_stable = values
                clean = (snapshot_stable and active == 1 and length == 0 and
                         position == 0 and released)
                require((sample.get("active"), sample.get("length"),
                         sample.get("position"), sample.get("clean")) ==
                        (active, length, position, clean),
                        "QMP input boundary summary differs from raw memory")
                require(clean == (sample_index == len(boundary_samples) - 1),
                        "only the final QMP input boundary may be clean")
        require(row.get("input_boundary_status") == "CLEAN" and
                row.get("input_boundary_active") == 1 and
                row.get("input_boundary_length") == 0 and
                row.get("input_boundary_position") == 0 and
                row.get("input_boundary_keyboard_release") is True,
                "QMP input boundary did not establish an empty released state")

    transitions = {}
    for index, byte in enumerate(expected, 1):
        pulse = pulses[index - 1]
        if not isinstance(pulse, dict):
            errors.append("confirmed delivery event is malformed")
            continue
        character = chr(byte)
        require(pulse.get("character_index") == index and
                pulse.get("target_length") == index and
                pulse.get("character_hex") == bytes([byte]).hex() and
                pulse.get("atomic_down_up") is qmp_atomic and
                (qmp_atomic or pulse.get("press_timeout_seconds") == 5.0) and
                pulse.get("release_timeout_seconds") == 5.0,
                "confirmed delivery character identity differs")
        checks = pulse.get("pre_pulse_runstate_checks")
        runstate = validate_runstate(
            checks, "confirmed delivery pre-press", qmp_records)
        if qmp_atomic:
            stroke = pulse.get("stroke_event")
            valid = validate_qmp_event(
                stroke, expected_qmp_key_events(character, True) +
                expected_qmp_key_events(character, False), "stroke")
            if valid:
                qmp_records.append(stroke)
                values = (stroke.get("started_monotonic"),
                          stroke.get("completed_monotonic"))
        else:
            down = pulse.get("down_event")
            up = pulse.get("up_event")
            down_valid = validate_qmp_event(
                down, expected_qmp_key_events(character, True), "down")
            up_valid = validate_qmp_event(
                up, expected_qmp_key_events(character, False), "up")
            valid = down_valid and up_valid
            if down_valid:
                qmp_records.append(down)
            if up_valid:
                qmp_records.append(up)
            if valid:
                values = (down.get("started_monotonic"),
                          down.get("completed_monotonic"),
                          up.get("started_monotonic"),
                          up.get("completed_monotonic"))
        if valid:
            require(all(finite(value) for value in values) and
                    list(values) == sorted(values),
                    "confirmed delivery transition order differs")
            if runstate is not None:
                started = pulse.get(
                    "pre_pulse_runstate_started_monotonic")
                observed = pulse.get(
                    "pre_pulse_runstate_observed_monotonic")
                require(finite(started) and finite(observed) and
                        started <= runstate[0] <= runstate[1] <= observed <=
                        values[0],
                        "confirmed delivery began before runstate confirmation")
            transitions[index] = values

    require([sample.get("sample") if isinstance(sample, dict) else None
             for sample in samples] == list(range(1, len(samples) + 1)),
            "confirmed delivery sample sequence is invalid")
    press_groups = {index: [] for index in range(1, len(expected) + 1)}
    release_groups = {index: [] for index in range(1, len(expected) + 1)}
    delivery_groups = {index: [] for index in range(1, len(expected) + 1)}
    stability = []
    artifact_names = []
    sample_times = []
    previous_length = -1
    for sample in samples:
        if not isinstance(sample, dict):
            errors.append("confirmed delivery sample is malformed")
            continue
        index = sample.get("character_index")
        phase = sample.get("phase")
        valid_index = isinstance(index, int) and 1 <= index <= len(expected)
        require(valid_index and sample.get("target_length") == index,
                "confirmed delivery sample target is invalid")
        commands = None
        if qmp_memory:
            values = qmp_snapshot_values(
                sample, parsed_symbols, physical_addresses, input_layout,
                qmp_records, errors)
            if values is None or not valid_index:
                continue
            active, length, position, terminated, observed, _released, \
                _slots, snapshot_stable = values
        else:
            snapshot_stable = True
            files = sample_artifacts(sample, "confirmed delivery sample")
            artifact_names.extend(path.name for path in files.values())
            log_path = files.get("log")
            command_path = files.get("command")
            if not log_path or not log_path.is_file() or not valid_index:
                continue
            raw = log_path.read_text(errors="replace")
            try:
                active = mi_memory_from_log(
                    raw, sample.get("active_response_token"), 1)[0]
                length = int.from_bytes(mi_memory_from_log(
                    raw, sample.get("length_response_token"), 4),
                    "little", signed=True)
                position = int.from_bytes(mi_memory_from_log(
                    raw, sample.get("position_response_token"), 4),
                    "little", signed=True)
                memory = mi_memory_from_log(
                    raw, sample.get("memory_response_token"), 256)
                observed = memory[:length] if 0 <= length < 256 else b""
                terminated = 0 <= length < 256 and memory[length] == 0
            except (TypeError, ValueError):
                errors.append(
                    "confirmed delivery values cannot be derived from GDB")
                continue
            commands = (command_path.read_text(errors="replace")
                        if command_path and command_path.is_file() else None)
            errors.extend(keyboard_sample_errors(
                sample, raw, commands, parsed_symbols, input_layout))
        coherent = (active == 1 and position == length == len(observed) and
                    terminated)
        target = expected[:index]
        text_status = (
            "UNSTABLE" if qmp_stable and not snapshot_stable else
            ("EXACT" if coherent and observed == target else
             ("PREFIX" if coherent and target.startswith(observed)
              else "DIVERGED")))
        released = sample.get("keyboard_release_observed") is True
        active_slots = sample.get("keyboard_active_slots")
        press_complete = (not qmp_atomic and phase == "press" and
                          text_status in ("PREFIX", "EXACT") and
                          not released and isinstance(active_slots, list) and
                          bool(active_slots))
        release_complete = (phase in (("delivery",) if qmp_atomic else
                                      ("release",)) and
                            text_status == "EXACT" and released)
        if qmp_atomic and phase == "delivery":
            derived = ("EXACT" if release_complete else
                       ("HELD" if text_status == "EXACT" else
                        (text_status if text_status in ("PREFIX", "UNSTABLE")
                         else
                         "DIVERGED")))
            delivery_groups[index].append(sample)
        elif phase == "press":
            derived = ("PRESSED" if press_complete else
                       ("PREFIX" if text_status in ("PREFIX", "EXACT") else
                        "DIVERGED"))
            press_groups[index].append(sample)
        elif phase == "release":
            derived = ("EXACT" if release_complete else
                       ("HELD" if text_status == "EXACT" else
                        ("PREFIX" if text_status == "PREFIX" else
                         "DIVERGED")))
            release_groups[index].append(sample)
        elif phase == "stability" and index == len(expected):
            derived = ("UNSTABLE" if text_status == "UNSTABLE" else
                       ("EXACT" if text_status == "EXACT" and released else
                        "DIVERGED"))
            press_complete = None
            release_complete = derived == "EXACT"
            stability.append(sample)
        else:
            derived = "DIVERGED"
            errors.append("confirmed delivery sample phase is invalid")
        observed_text = observed.decode("ascii") if observed.isascii() else None
        require((qmp_stable and not snapshot_stable and
                 derived == "UNSTABLE") or
                (snapshot_stable and coherent and derived != "DIVERGED"),
                "confirmed delivery sample is incoherent or diverged")
        require((sample.get("active"), sample.get("length"),
                 sample.get("position"), sample.get("terminated"),
                 sample.get("observed_bytes_hex"), sample.get("observed_text"),
                 sample.get("input_text_status"), sample.get("status"),
                 sample.get("press_complete"),
                 sample.get("character_complete")) ==
                (active, length, position, terminated, observed.hex(),
                 observed_text, text_status, derived, press_complete,
                 release_complete),
                ("confirmed delivery summary differs from raw memory"
                 if qmp_memory else
                 "confirmed delivery summary differs from GDB"))
        if snapshot_stable:
            require(length >= previous_length,
                    "confirmed delivery shell prefix regressed")
            previous_length = length
        observed_time = sample.get("observed_monotonic")
        require(finite(observed_time),
                "confirmed delivery sample time is invalid")
        if finite(observed_time):
            sample_times.append(observed_time)
        if commands is not None:
            for name, size in (("g_shell_active", 1), ("g_len", 4),
                               ("g_pos", 4), ("g_buffer", 256)):
                require(
                    f"-data-read-memory-bytes 0x{parsed_symbols[name]:x} {size}"
                    in commands,
                    "confirmed delivery command uses another shell symbol")
            require("break-insert" not in commands and "$rdi" not in commands,
                    "confirmed delivery sample attempted dispatch inspection")

    require(len(artifact_names) == len(set(artifact_names)),
            "confirmed delivery sample artifacts are reused")
    require(sample_times == sorted(sample_times) and
            len(sample_times) == len(set(sample_times)),
            "confirmed delivery sample times are not increasing")
    for index in range(1, len(expected) + 1):
        press = press_groups[index]
        release = release_groups[index]
        transition = transitions.get(index)
        if qmp_atomic:
            delivery = delivery_groups[index]
            require(bool(delivery) and
                    delivery[-1].get("status") == "EXACT" and
                    delivery[-1].get("character_complete") is True and
                    all(item.get("status") in (
                            ("PREFIX", "HELD", "UNSTABLE") if qmp_stable else
                            ("PREFIX", "HELD")) and
                        item.get("character_complete") is False
                        for item in delivery[:-1]),
                    "confirmed atomic delivery was not observed exactly once")
            if delivery and transition:
                release_time = delivery[-1].get("observed_monotonic")
                require(finite(release_time) and
                        transition[1] <= release_time,
                        "confirmed atomic observation preceded its stroke")
                if index < len(expected) and index + 1 in transitions:
                    require(release_time <= transitions[index + 1][0],
                            "next key began before prior delivery was observed")
        else:
            require(bool(press) and press[-1].get("status") == "PRESSED" and
                    press[-1].get("press_complete") is True and
                    all(item.get("status") == "PREFIX" and
                        item.get("press_complete") is False
                        for item in press[:-1]),
                    "confirmed delivery press was not observed exactly once")
            require(bool(release) and release[-1].get("status") == "EXACT" and
                    release[-1].get("character_complete") is True and
                    all(item.get("status") in ("PREFIX", "HELD") and
                        item.get("character_complete") is False
                        for item in release[:-1]),
                    "confirmed delivery release was not observed exactly once")
        if not qmp_atomic and press and release and transition:
            press_time = press[-1].get("observed_monotonic")
            release_time = release[-1].get("observed_monotonic")
            require(finite(press_time) and finite(release_time) and
                    transition[1] <= press_time <= transition[2] <=
                    transition[3] <= release_time,
                    "confirmed delivery observation lies outside its key state")
            if index < len(expected) and index + 1 in transitions:
                require(release_time <= transitions[index + 1][0],
                        "next key began before prior release was observed")

    require(bool(stability) and samples[-1] is stability[-1] and
            stability[-1].get("status") == "EXACT" and
            (len(stability) == 1 or
             (qmp_stable and all(item.get("status") == "UNSTABLE"
                                 for item in stability[:-1]))),
            "confirmed delivery final stability sample is absent")
    if stability and len(expected) in transitions:
        stable_time = stability[-1].get("observed_monotonic")
        release_time = transitions[len(expected)][1 if qmp_atomic else 3]
        recorded = row.get("input_delivery_stable_span_seconds")
        require(finite(stable_time) and finite(recorded) and
                stable_time - release_time >= 0.5 and
                abs(recorded - (stable_time - release_time)) < 1e-9,
                "confirmed delivery stability interval differs")

    expected_enter = (expected_qmp_key_events("ret", True),
                      expected_qmp_key_events("ret", False))
    require(enter_events[0].get("step") == "down" and
            enter_events[1].get("step") == "up",
            "confirmed delivery terminator steps differ")
    for event, expected_events, label in zip(
            enter_events, expected_enter, ("terminator-down", "terminator-up")):
        if validate_qmp_event(event, expected_events, label):
            qmp_records.append(event)

    resume_checks = row.get("qmp_resume_checks")
    resume_timing = validate_runstate(
        resume_checks, "dispatcher resume", qmp_records)
    cleanup_checks = row.get("qmp_cleanup_checks")
    cleanup_timing = validate_runstate(
        cleanup_checks, "observer cleanup", qmp_records)
    if qmp_memory:
        enter_order = (
            row.get("input_observer_resumed_monotonic"),
            row.get("qmp_resume_observed_monotonic"),
            enter_events[0].get("started_monotonic"),
            enter_events[0].get("completed_monotonic"),
            row.get("dispatcher_observed_monotonic"),
            row.get("input_observer_cleanup_started_monotonic"),
            row.get("input_observer_target_detached_monotonic"),
            row.get("input_observer_process_completed_monotonic"),
            row.get("qmp_cleanup_started_monotonic"),
            row.get("qmp_cleanup_observed_monotonic"),
            enter_events[1].get("started_monotonic"),
            enter_events[1].get("completed_monotonic"),
        )
    else:
        enter_order = (
            row.get("input_observer_resumed_monotonic"),
            row.get("qmp_resume_observed_monotonic"),
            enter_events[0].get("started_monotonic"),
            enter_events[0].get("completed_monotonic"),
            row.get("dispatcher_observed_monotonic"),
            enter_events[1].get("started_monotonic"),
            enter_events[1].get("completed_monotonic"),
            row.get("input_observer_cleanup_started_monotonic"),
            row.get("input_observer_target_detached_monotonic"),
            row.get("input_observer_process_completed_monotonic"),
            row.get("qmp_cleanup_started_monotonic"),
            row.get("qmp_cleanup_observed_monotonic"),
        )
    require(all(finite(value) for value in enter_order) and
            list(enter_order) == sorted(enter_order),
            "confirmed delivery dispatch/release ordering differs")
    require(row.get("qmp_resume_status") == "PASS" and
            row.get("qmp_resume_timeout_seconds") == 2.0 and
            row.get("qmp_resume_poll_seconds") == 0.01 and
            resume_timing is not None and
            row.get("qmp_resume_started_monotonic") <= resume_timing[0] <=
            resume_timing[1] <= row.get("qmp_resume_observed_monotonic"),
            "dispatcher resume policy differs")
    require(row.get("input_observer_cleanup_status") == "PASS" and
            row.get("input_observer_cleanup_method") ==
            "detach-stopped-private-observer" and
            row.get("input_observer_cleanup_return_code") == 0 and
            row.get("input_observer_cleanup_timeout_seconds") == 5.0 and
            cleanup_timing is not None and
            row.get("qmp_cleanup_started_monotonic") <= cleanup_timing[0] <=
            cleanup_timing[1] <= row.get("qmp_cleanup_observed_monotonic"),
            "observer cleanup policy differs")
    require(row.get("qmp_enter_guest_release_status") == "PASS" and
            isinstance(row.get("qmp_enter_guest_release_samples"), list) and
            bool(row.get("qmp_enter_guest_release_samples")),
            "Enter release was not confirmed in the guest")
    if qmp_memory and isinstance(
            row.get("qmp_enter_guest_release_samples"), list):
        release_samples = row["qmp_enter_guest_release_samples"]
        require([item.get("sample") if isinstance(item, dict) else None
                 for item in release_samples] ==
                list(range(1, len(release_samples) + 1)),
                "Enter release sample sequence is invalid")
        for sample_index, sample in enumerate(release_samples):
            values = qmp_snapshot_values(
                sample, parsed_symbols, physical_addresses, input_layout,
                qmp_records, errors)
            released = (values[5] and values[7]
                        if values is not None else False)
            require(released == (sample_index == len(release_samples) - 1),
                    "only the final Enter snapshot may be released")

    qmp_name = row.get("qmp_input_log")
    if (not isinstance(qmp_name, str) or Path(qmp_name).name != qmp_name or
            not qmp_name):
        errors.append("confirmed delivery QMP log name is unsafe")
    else:
        qmp_path = profile_dir / qmp_name
        if not qmp_path.is_file():
            errors.append("confirmed delivery QMP log is absent")
        elif row.get("qmp_input_log_sha256") != sha256(qmp_path):
            errors.append("confirmed delivery QMP log hash differs")
        else:
            errors.extend(qmp_input_log_errors(qmp_path, qmp_records))
    return errors


def pulse_delivery_errors(row, profile_dir, expected, parsed_symbols,
                          input_layout=None):
    errors = []

    def require(condition, message):
        if not condition:
            errors.append(message)

    def finite(value):
        return (isinstance(value, (int, float)) and
                not isinstance(value, bool) and math.isfinite(value))

    pulses = row.get("qmp_text_events")
    samples = row.get("input_delivery_samples")
    enter_events = row.get("qmp_enter_events")
    schema = row.get("schema")
    single_enter = schema in (12, 13, 18, 19, 20, 21)
    require(row.get("input_delivery_status") == "COMPLETE",
            "key-pulse delivery did not complete")
    expected_boundary = (
        "qmp-guest-release-confirmed-key-pulse" if schema == 22 else
        ("qmp-atomic-key-stroke" if schema in (18, 19, 20, 21) else
         "qmp-bounded-key-pulse"))
    require(row.get("qmp_text_status") == "PASS" and
            row.get("input_character_boundary_status") == "PASS" and
            row.get("input_character_boundary") == expected_boundary,
            "key-pulse text delivery did not complete")
    require(row.get("input_delivery_timeout_seconds") == 90.0 and
            row.get("input_delivery_stability_seconds") == 0.5 and
            row.get("input_delivery_sample_delay_seconds") == 0.05 and
            ((schema in (18, 19, 20, 21) and
              row.get("input_key_stroke_atomic") is True) or
             (schema not in (18, 19, 20, 21) and
              row.get("input_key_pulse_seconds") == 0.05)),
            "key-pulse delivery policy differs")
    require(row.get("input_delivery_character_count") == len(expected),
            "key-pulse byte count differs from the frame")
    require(isinstance(pulses, list) and len(pulses) == len(expected),
            "key-pulse event count differs")
    require(isinstance(samples, list) and bool(samples),
            "key-pulse samples are absent")
    expected_enter_count = 1 if single_enter else 2
    require(isinstance(enter_events, list) and
            len(enter_events) == expected_enter_count,
            "key-pulse terminator events differ")
    if not (isinstance(pulses, list) and len(pulses) == len(expected) and
            isinstance(samples, list) and isinstance(enter_events, list)):
        return errors

    manifest_name = row.get("qmp_input_manifest")
    if (not isinstance(manifest_name, str) or
            Path(manifest_name).name != manifest_name or not manifest_name):
        errors.append("key-pulse manifest name is unsafe")
    else:
        manifest_path = profile_dir / manifest_name
        if not manifest_path.is_file():
            errors.append("key-pulse manifest is absent")
        elif row.get("qmp_input_manifest_sha256") != sha256(manifest_path):
            errors.append("key-pulse manifest hash differs")
        else:
            try:
                manifest = json.loads(manifest_path.read_text())
            except (OSError, ValueError, json.JSONDecodeError):
                errors.append("key-pulse manifest is malformed")
            else:
                expected_manifest = {
                    "schema": (13 if schema == 22 else
                               (12 if schema == 21 else
                               (11 if schema == 20 else
                               (10 if schema == 19 else
                               (9 if schema == 18 else
                               (8 if schema == 17 else
                               (7 if row.get("schema") == 16 else
                               (6 if row.get("schema") == 15 else
                               (5 if row.get("schema") == 14 else
                                (4 if row.get("schema") == 13 else 3)))))))))),
                    "kind": ("qmp-guest-release-key-pulse-transport"
                             if schema == 22 else
                             ("qmp-atomic-key-stroke-transport"
                              if schema in (18, 19, 20, 21) else
                              "qmp-key-pulse-transport")),
                    "frame_bytes_hex": expected.hex(),
                    "qmp_log": row.get("qmp_input_log"),
                    "key_pulses": pulses,
                    **({"resume_checks": row.get("qmp_resume_checks")}
                       if row.get("schema") in
                       (15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26) else {}),
                    "enter_events": enter_events,
                    **({"cleanup_checks": row.get("qmp_cleanup_checks")}
                       if row.get("schema") in
                       (16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26) else {}),
                }
                if schema == 22:
                    expected_manifest.update({
                        "input_layout_artifact":
                            row.get("input_layout_artifact"),
                        "input_layout_sha256": row.get("input_layout_sha256"),
                        "input_layout": row.get("input_layout"),
                    })
                require(manifest == expected_manifest,
                        "key-pulse manifest differs from the ledger")

    def validate_qmp_event(event, expected_events, label):
        if not isinstance(event, dict):
            errors.append(f"key-pulse {label} event is malformed")
            return False
        started = event.get("started_monotonic")
        completed = event.get("completed_monotonic")
        require(isinstance(event.get("request_id"), str) and
                event.get("events") == expected_events,
                f"key-pulse {label} transition differs")
        require(finite(started) and finite(completed) and started <= completed,
                f"key-pulse {label} timing is invalid")
        return True

    qmp_records = []
    pulse_times = []
    for index, byte in enumerate(expected, 1):
        pulse = pulses[index - 1]
        if not isinstance(pulse, dict):
            errors.append("key-pulse record is malformed")
            continue
        character = chr(byte)
        require(pulse.get("character_index") == index and
                pulse.get("target_length") == index and
                pulse.get("character_hex") == bytes([byte]).hex() and
                  ((schema in (18, 19, 20, 21) and
                  pulse.get("atomic_down_up") is True) or
                 (schema not in (18, 19, 20, 21) and
                  pulse.get("pulse_seconds") == 0.05)),
                "key-pulse character identity differs")
        if schema in (17, 18, 19, 20, 21, 22, 23, 24, 25, 26):
            checks = pulse.get("pre_pulse_runstate_checks")
            require(isinstance(checks, list) and bool(checks),
                    "key-pulse pre-delivery runstate observations are absent")
            if isinstance(checks, list) and checks:
                for check_index, check in enumerate(checks):
                    value = check.get("response_return") if isinstance(
                        check, dict) else None
                    running = (isinstance(value, dict) and
                               value.get("running") is True and
                               value.get("status") == "running")
                    require(isinstance(check, dict) and
                            check.get("qmp_execute") == "query-status" and
                            check.get("arguments") == {} and
                            check.get("expected_return") == value and
                            ((check_index == len(checks) - 1 and running) or
                             (check_index < len(checks) - 1 and not running)),
                            "key-pulse pre-delivery runstate differs")
                    if isinstance(check, dict):
                        qmp_records.append(check)
                first = checks[0].get("started_monotonic")
                last = checks[-1].get("completed_monotonic")
                started = pulse.get("pre_pulse_runstate_started_monotonic")
                observed = pulse.get("pre_pulse_runstate_observed_monotonic")
                require(all(finite(value) for value in
                            (started, first, last, observed)) and
                        started <= first <= last <= observed,
                        "key-pulse pre-delivery runstate timing differs")
        if schema in (18, 19, 20, 21):
            stroke = pulse.get("stroke_event")
            if validate_qmp_event(
                    stroke,
                    expected_qmp_key_events(character, True) +
                    expected_qmp_key_events(character, False),
                    "atomic stroke"):
                qmp_records.append(stroke)
                values = (stroke.get("started_monotonic"),
                          stroke.get("completed_monotonic"))
                require(all(finite(value) for value in values) and
                        values[0] <= values[1],
                        "key-pulse atomic stroke timing is invalid")
                observed = pulse.get(
                    "pre_pulse_runstate_observed_monotonic")
                require(finite(observed) and observed <= values[0],
                        "key-pulse began before runstate confirmation")
                pulse_times.append(values)
        else:
            down = pulse.get("down_event")
            up = pulse.get("up_event")
            down_valid = validate_qmp_event(
                down, expected_qmp_key_events(character, True), "down")
            up_valid = validate_qmp_event(
                up, expected_qmp_key_events(character, False), "up")
            if down_valid and up_valid:
                qmp_records.extend((down, up))
                values = (down.get("started_monotonic"),
                          down.get("completed_monotonic"),
                          up.get("started_monotonic"),
                          up.get("completed_monotonic"))
                require(all(finite(value) for value in values) and
                        values[0] <= values[1] and
                        values[2] - values[1] >= 0.05 and
                        values[2] <= values[3],
                        "key-pulse hold interval or ordering differs")
                if schema == 17:
                    settled = pulse.get("release_settled_monotonic")
                    require(pulse.get("release_settle_seconds") == 0.1 and
                            finite(settled) and
                            settled - values[3] >= 0.1,
                            "key-pulse release interval differs")
                    observed = pulse.get(
                        "pre_pulse_runstate_observed_monotonic")
                    require(finite(observed) and observed <= values[0],
                            "key-pulse began before runstate confirmation")
                pulse_times.append(values)

    if single_enter and len(enter_events) == 1:
        enter = enter_events[0]
        if schema in (18, 19, 20, 21):
            require(isinstance(enter, dict) and
                    enter.get("step") == "stroke" and
                    enter.get("events") ==
                    expected_qmp_key_events("ret", True) +
                    expected_qmp_key_events("ret", False) and
                    isinstance(enter.get("request_id"), str) and
                    finite(enter.get("started_monotonic")) and
                    finite(enter.get("completed_monotonic")) and
                    enter.get("started_monotonic") <=
                    enter.get("completed_monotonic"),
                    "key-pulse atomic terminator differs")
        else:
            expected_step = ("post-resume-pulse" if schema == 13
                             else "scheduled-pulse")
            require(isinstance(enter, dict) and
                    enter.get("step") == expected_step and
                    enter.get("qmp_execute") ==
                    "human-monitor-command" and
                    enter.get("arguments") == {
                        "command-line": "sendkey ret 50"} and
                    enter.get("expected_return") == "" and
                    enter.get("response_return") == "" and
                    enter.get("hold_ms") == 50 and
                    isinstance(enter.get("request_id"), str) and
                    finite(enter.get("started_monotonic")) and
                    finite(enter.get("completed_monotonic")) and
                    enter.get("started_monotonic") <=
                    enter.get("completed_monotonic"),
                    "key-pulse scheduled terminator differs")
        if isinstance(enter, dict):
            qmp_records.append(enter)
    elif not single_enter and len(enter_events) == 2:
        expected_enter = (expected_qmp_key_events("ret", True),
                          expected_qmp_key_events("ret", False))
        require(enter_events[0].get("step") == "down" and
                enter_events[1].get("step") == "up",
                "key-pulse terminator step differs")
        for event, expected_events, label in zip(
                enter_events, expected_enter,
                ("terminator-down", "terminator-up")):
            if validate_qmp_event(event, expected_events, label):
                qmp_records.append(event)
        if row.get("schema") in (14, 15, 16, 17):
            down = enter_events[0]
            up = enter_events[1]
            ordered = (
                row.get("input_observer_resumed_monotonic"),
                down.get("started_monotonic"),
                down.get("completed_monotonic"),
                row.get("dispatcher_observed_monotonic"),
                row.get("input_observer_dispatch_resumed_monotonic"),
                up.get("started_monotonic"),
                up.get("completed_monotonic"),
            )
            require(all(finite(value) for value in ordered) and
                    list(ordered) == sorted(ordered),
                    "key-pulse terminator release ordering differs")
        elif row.get("schema") == 22:
            ordered = (
                row.get("input_observer_resumed_monotonic"),
                enter_events[0].get("started_monotonic"),
                enter_events[0].get("completed_monotonic"),
                enter_events[1].get("started_monotonic"),
                enter_events[1].get("completed_monotonic"),
                row.get("dispatcher_observed_monotonic"),
                row.get("input_observer_target_detached_monotonic"),
            )
            require(all(finite(value) for value in ordered) and
                    list(ordered) == sorted(ordered),
                    "key-pulse terminator guest-release ordering differs")

    resume_checks = row.get("qmp_resume_checks", [])
    if schema in (15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26):
        require(isinstance(resume_checks, list) and bool(resume_checks),
                "QMP resume observations are absent")
        if isinstance(resume_checks, list) and resume_checks:
            for index, check in enumerate(resume_checks):
                value = check.get("response_return") if isinstance(
                    check, dict) else None
                running = (isinstance(value, dict) and
                           value.get("running") is True and
                           value.get("status") == "running")
                require(isinstance(check, dict) and
                        check.get("qmp_execute") == "query-status" and
                        check.get("arguments") == {} and
                        check.get("expected_return") == value and
                        ((index == len(resume_checks) - 1 and running) or
                         (index < len(resume_checks) - 1 and not running)),
                        "QMP resume observation differs")
                if isinstance(check, dict):
                    qmp_records.append(check)
            first = resume_checks[0].get("started_monotonic")
            last = resume_checks[-1].get("completed_monotonic")
            resume_started = row.get("qmp_resume_started_monotonic")
            resume_observed = row.get("qmp_resume_observed_monotonic")
            timeout = row.get("qmp_resume_timeout_seconds")
            require(all(finite(value) for value in
                        (resume_started, first, last, resume_observed,
                         timeout)) and
                    resume_started <= first <= last <= resume_observed and
                    last - first <= timeout and timeout == 2.0 and
                    row.get("qmp_resume_poll_seconds") == 0.01 and
                    row.get("qmp_resume_status") == "PASS",
                    "QMP resume observation timing differs")

    cleanup_checks = row.get("qmp_cleanup_checks", [])
    if schema in (16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26):
        require(isinstance(cleanup_checks, list) and bool(cleanup_checks),
                "QMP observer-cleanup observations are absent")
        if isinstance(cleanup_checks, list) and cleanup_checks:
            for index, check in enumerate(cleanup_checks):
                value = check.get("response_return") if isinstance(
                    check, dict) else None
                running = (isinstance(value, dict) and
                           value.get("running") is True and
                           value.get("status") == "running")
                require(isinstance(check, dict) and
                        check.get("qmp_execute") == "query-status" and
                        check.get("arguments") == {} and
                        check.get("expected_return") == value and
                        ((index == len(cleanup_checks) - 1 and running) or
                         (index < len(cleanup_checks) - 1 and not running)),
                        "QMP observer-cleanup observation differs")
                if isinstance(check, dict):
                    qmp_records.append(check)
            first = cleanup_checks[0].get("started_monotonic")
            last = cleanup_checks[-1].get("completed_monotonic")
            cleanup_started = row.get("qmp_cleanup_started_monotonic")
            cleanup_observed = row.get("qmp_cleanup_observed_monotonic")
            timeout = row.get("qmp_resume_timeout_seconds")
            require(all(finite(value) for value in
                        (cleanup_started, first, last, cleanup_observed,
                         timeout)) and
                    cleanup_started <= first <= last <= cleanup_observed and
                    last - first <= timeout and timeout == 2.0,
                    "QMP observer-cleanup observation timing differs")

    qmp_name = row.get("qmp_input_log")
    if (not isinstance(qmp_name, str) or Path(qmp_name).name != qmp_name or
            not qmp_name):
        errors.append("key-pulse QMP log name is unsafe")
    else:
        qmp_path = profile_dir / qmp_name
        if not qmp_path.is_file():
            errors.append("key-pulse QMP log is absent")
        elif row.get("qmp_input_log_sha256") != sha256(qmp_path):
            errors.append("key-pulse QMP log hash differs")
        else:
            errors.extend(qmp_input_log_errors(qmp_path, qmp_records))

    require([sample.get("sample") if isinstance(sample, dict) else None
             for sample in samples] == list(range(1, len(samples) + 1)),
            "key-pulse sample sequence is invalid")
    groups = {index: [] for index in range(1, len(expected) + 1)}
    stability = []
    sample_names = []
    sample_times = []
    previous_length = -1
    for sample in samples:
        if not isinstance(sample, dict):
            errors.append("key-pulse sample is malformed")
            continue
        index = sample.get("character_index")
        phase = sample.get("phase")
        valid_index = isinstance(index, int) and 1 <= index <= len(expected)
        require(valid_index and sample.get("target_length") == index,
                "key-pulse sample target is invalid")
        files = {}
        for name_field, hash_field in (("log", "log_sha256"),
                                       ("command", "command_sha256"),
                                       ("argv", "argv_sha256")):
            name = sample.get(name_field)
            if not isinstance(name, str) or Path(name).name != name or not name:
                errors.append("key-pulse sample artifact name is unsafe")
                continue
            sample_names.append(name)
            artifact = profile_dir / name
            files[name_field] = artifact
            if not artifact.is_file():
                errors.append("key-pulse sample artifact is absent")
            elif sample.get(hash_field) != sha256(artifact):
                errors.append("key-pulse sample artifact hash differs")
        log_path = files.get("log")
        command_path = files.get("command")
        if not log_path or not log_path.is_file() or not valid_index:
            continue
        raw = log_path.read_text(errors="replace")
        try:
            active = mi_memory_from_log(
                raw, sample.get("active_response_token"), 1)[0]
            length = int.from_bytes(mi_memory_from_log(
                raw, sample.get("length_response_token"), 4),
                "little", signed=True)
            position = int.from_bytes(mi_memory_from_log(
                raw, sample.get("position_response_token"), 4),
                "little", signed=True)
            memory = mi_memory_from_log(
                raw, sample.get("memory_response_token"), 256)
            observed = memory[:length] if 0 <= length < 256 else b""
            terminated = 0 <= length < 256 and memory[length] == 0
        except (TypeError, ValueError):
            errors.append("key-pulse values cannot be derived from GDB")
            continue
        coherent = (active == 1 and position == length == len(observed) and
                    terminated)
        target = expected[:index]
        text_derived = "DIVERGED"
        if phase == "character":
            text_derived = ("EXACT" if coherent and observed == target else
                       ("PREFIX" if coherent and target.startswith(observed)
                        else "DIVERGED"))
            if schema == 22 and input_layout is not None:
                errors.extend(keyboard_sample_errors(
                    sample, raw,
                    command_path.read_text(errors="replace")
                    if command_path and command_path.is_file() else None,
                    parsed_symbols, input_layout))
                released = sample.get("keyboard_release_observed") is True
                derived = ("PREFIX" if text_derived == "EXACT" and
                           not released else text_derived)
            else:
                derived = text_derived
            groups[index].append(sample)
        elif phase == "stability" and index == len(expected):
            text_derived = ("EXACT" if coherent and observed == expected else
                            "DIVERGED")
            if schema == 22 and input_layout is not None:
                errors.extend(keyboard_sample_errors(
                    sample, raw,
                    command_path.read_text(errors="replace")
                    if command_path and command_path.is_file() else None,
                    parsed_symbols, input_layout))
                derived = ("EXACT" if text_derived == "EXACT" and
                           sample.get("keyboard_release_observed") is True
                           else "DIVERGED")
            else:
                derived = text_derived
            stability.append(sample)
        else:
            derived = "DIVERGED"
            errors.append("key-pulse sample phase is invalid")
        observed_text = observed.decode("ascii") if observed.isascii() else None
        require(coherent and derived != "DIVERGED",
                "key-pulse sample is incoherent or diverged")
        expected_summary = (
            active, length, position, terminated, observed.hex(),
            observed_text, derived)
        require((sample.get("active"), sample.get("length"),
                 sample.get("position"), sample.get("terminated"),
                 sample.get("observed_bytes_hex"), sample.get("observed_text"),
                 sample.get("status")) ==
                expected_summary,
                "key-pulse summary differs from GDB")
        if schema == 22:
            require(sample.get("input_text_status") == text_derived and
                    sample.get("character_complete") is
                    (derived == "EXACT"),
                    "key-pulse release completion summary differs")
        require(length >= previous_length, "key-pulse buffer regressed")
        previous_length = length
        observed_time = sample.get("observed_monotonic")
        require(finite(observed_time), "key-pulse sample time is invalid")
        if finite(observed_time):
            sample_times.append(observed_time)
        if command_path and command_path.is_file() and parsed_symbols:
            commands = command_path.read_text(errors="replace")
            for name, size in (("g_shell_active", 1), ("g_len", 4),
                               ("g_pos", 4), ("g_buffer", 256)):
                require(
                    f"-data-read-memory-bytes 0x{parsed_symbols[name]:x} {size}" in
                    commands, "key-pulse command uses another symbol")
            require("break-insert" not in commands and "$rdi" not in commands,
                    "key-pulse sample attempted dispatch inspection")

    require(len(sample_names) == len(set(sample_names)),
            "key-pulse sample artifacts are reused")
    require(sample_times == sorted(sample_times) and
            len(sample_times) == len(set(sample_times)),
            "key-pulse sample times are not increasing")

    def completed_once(group):
        return (bool(group) and group[-1].get("status") == "EXACT" and
                all(item.get("status") == "PREFIX" for item in group[:-1]))

    for index in range(1, len(expected) + 1):
        group = groups[index]
        require(completed_once(group),
                "key-pulse character prefix was not completed once")
        if not group or index > len(pulse_times):
            continue
        exact_time = group[-1].get("observed_monotonic")
        release_time = pulse_times[index - 1][-1]
        require(finite(exact_time) and release_time <= exact_time,
                "key-pulse character was observed before release")
        if index < len(expected) and index < len(pulse_times):
            require(exact_time <= pulse_times[index][0],
                    "key-pulse next character preceded observation")

    require(len(stability) == 1 and stability[0].get("status") == "EXACT" and
            samples[-1] is stability[0],
            "key-pulse final stability sample is absent")
    if pulse_times and stability:
        stable_time = stability[0].get("observed_monotonic")
        release_time = pulse_times[-1][-1]
        recorded = row.get("input_delivery_stable_span_seconds")
        require(finite(stable_time) and finite(recorded) and
                stable_time - release_time >= 0.5 and
                abs(recorded - (stable_time - release_time)) < 1e-9,
                "key-pulse stability interval differs")
    return errors


def input_observation_errors(row, profile_dir, expected_frame, profile):
    errors = []

    def require(condition, message):
        if not condition:
            errors.append(message)

    required = {
        "guest_input_status", "guest_input_length", "guest_input_bytes_hex",
        "guest_input_text", "input_boundary_status", "input_boundary_active",
        "input_boundary_length", "input_boundary_position",
        "input_boundary_samples", "input_symbol_addresses",
        "pre_enter_status", "pre_enter_active", "pre_enter_length",
        "pre_enter_position", "pre_enter_bytes_hex", "pre_enter_text",
        "pre_enter_active_response_token", "pre_enter_length_response_token",
        "pre_enter_position_response_token", "pre_enter_memory_response_token",
        "input_observer_log", "input_observer_log_sha256",
        "input_observer_command", "input_observer_command_sha256",
        "input_observer_argv", "input_observer_argv_sha256",
        "input_observer_memory_response_token", "input_observer_armed_monotonic",
        "input_observer_resumed_monotonic",
        "pre_enter_observed_monotonic", "dispatcher_status",
        "dispatcher_stop",
    }
    schema = row.get("schema")
    if schema in (4, 5, 6, 7, 9):
        required.update({
            "hmp_input_status", "hmp_text_status", "hmp_enter_status",
            "hmp_text_started_monotonic", "hmp_text_completed_monotonic",
            "hmp_enter_completed_monotonic", "hmp_input_completed_monotonic",
            "hmp_text_log", "hmp_text_log_sha256", "hmp_enter_log",
            "hmp_enter_log_sha256",
        })
    if schema == 4:
        required.add("input_delivery_settle_seconds")
        expected_observation = "pre-enter-and-dispatcher-gdb"
    elif schema == 5:
        required.update({
            "input_delivery_status", "input_delivery_timeout_seconds",
            "input_delivery_sample_delay_seconds",
            "input_delivery_started_monotonic",
            "input_delivery_completed_monotonic",
            "input_delivery_samples",
        })
        expected_observation = "bounded-pre-enter-and-dispatcher-gdb"
    elif schema == 6:
        required.update({
            "input_delivery_status", "input_delivery_timeout_seconds",
            "input_delivery_initial_quiet_seconds",
            "input_delivery_stability_seconds",
            "input_delivery_sample_delay_seconds",
            "input_delivery_started_monotonic",
            "input_delivery_quiet_started_monotonic",
            "input_delivery_quiet_completed_monotonic",
            "input_delivery_completed_monotonic",
            "input_delivery_stable_span_seconds",
            "input_delivery_samples",
        })
        expected_observation = \
            "stable-pre-enter-and-dispatcher-gdb"
    elif schema == 7:
        required.update({
            "input_delivery_status", "input_delivery_timeout_seconds",
            "input_delivery_stability_seconds",
            "input_delivery_sample_delay_seconds",
            "input_delivery_started_monotonic",
            "input_delivery_completed_monotonic",
            "input_delivery_stable_span_seconds",
            "input_delivery_samples", "input_release_key",
            "input_release_status", "input_release_started_monotonic",
            "input_release_completed_monotonic", "input_release_log",
            "input_release_log_sha256",
        })
        expected_observation = \
            "release-stable-pre-enter-and-dispatcher-gdb"
    elif schema == 9:
        required.update({
            "input_delivery_status", "input_delivery_timeout_seconds",
            "input_delivery_stability_seconds",
            "input_delivery_sample_delay_seconds",
            "input_delivery_started_monotonic",
            "input_delivery_completed_monotonic",
            "input_delivery_stable_span_seconds", "input_delivery_samples",
            "input_delivery_character_count", "input_character_boundary",
            "input_character_boundary_status", "hmp_text_events",
            "input_delivery_boundary_events", "hmp_text_manifest",
            "hmp_text_manifest_sha256",
        })
        expected_observation = \
            "cursor-feedback-pre-enter-and-dispatcher-gdb"
    elif schema in (10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26):
        required.update({
            "qmp_input_status", "qmp_text_status", "qmp_enter_status",
            "qmp_text_started_monotonic", "qmp_text_completed_monotonic",
            "qmp_enter_completed_monotonic", "qmp_input_completed_monotonic",
            "qmp_input_log", "qmp_input_log_sha256", "qmp_input_manifest",
            "qmp_input_manifest_sha256", "qmp_text_events",
            "qmp_enter_events",
            "input_delivery_status", "input_delivery_timeout_seconds",
            "input_delivery_stability_seconds",
            "input_delivery_sample_delay_seconds",
            "input_delivery_started_monotonic",
            "input_delivery_completed_monotonic",
            "input_delivery_stable_span_seconds", "input_delivery_samples",
            "input_delivery_character_count", "input_character_boundary",
            "input_character_boundary_status",
            "dispatcher_observed_monotonic",
            "input_observer_dispatch_resumed_monotonic",
        })
        if schema == 10:
            required.update({"qmp_final_release_event",
                             "input_delivery_boundary_events"})
            expected_observation = \
                "qmp-keyup-feedback-pre-enter-and-dispatcher-gdb"
        elif schema in (23, 24, 25, 26):
            required.update({
                "input_key_stroke_atomic", "input_key_press_timeout_seconds",
                "input_key_release_timeout_seconds",
                "input_layout_artifact", "input_layout_sha256",
                "input_layout", "input_boundary_keyboard_release",
                "qmp_resume_status", "qmp_resume_timeout_seconds",
                "qmp_resume_poll_seconds", "qmp_resume_started_monotonic",
                "qmp_resume_observed_monotonic", "qmp_resume_checks",
                "qmp_enter_down_completed_monotonic",
                "qmp_enter_up_started_monotonic",
                "qmp_enter_guest_release_status",
                "qmp_enter_guest_release_samples",
                "input_observer_cleanup_status",
                "input_observer_cleanup_method",
                "input_observer_cleanup_return_code",
                "input_observer_cleanup_timeout_seconds",
                "input_observer_cleanup_started_monotonic",
                "input_observer_process_completed_monotonic",
                "input_observer_target_detached_monotonic",
                "input_observer_cleanup_detach_response_token",
                "input_observer_cleanup_exit_response_token",
                "qmp_cleanup_started_monotonic",
                "qmp_cleanup_observed_monotonic", "qmp_cleanup_checks",
                "input_observer_cleanup_completed_monotonic",
            })
            if schema in (25, 26):
                required.discard("input_key_press_timeout_seconds")
            if schema in (24, 25, 26):
                required.update({
                    "input_address_map_artifact",
                    "input_address_map_sha256",
                    "input_load_segments",
                    "input_symbol_physical_addresses",
                })
                if schema == 26:
                    required.add("input_shell_snapshot_policy")
                    expected_observation = \
                        "qmp-stable-physical-atomic-stroke-confirmed-bounded-stopped-detach-observer-gdb"
                else:
                    expected_observation = (
                        "qmp-physical-atomic-stroke-confirmed-bounded-stopped-detach-observer-gdb"
                        if schema == 25 else
                        "qmp-physical-press-release-confirmed-bounded-stopped-detach-observer-gdb")
            else:
                expected_observation = \
                    "qmp-guest-press-release-confirmed-bounded-stopped-detach-observer-gdb"
        elif schema not in (18, 19, 20, 21):
            required.add("input_key_pulse_seconds")
            if schema == 12:
                required.add("qmp_enter_scheduled_monotonic")
                expected_observation = \
                    "qmp-key-pulse-scheduled-enter-dispatcher-gdb"
            elif schema == 13:
                required.add("qmp_enter_started_monotonic")
                expected_observation = \
                    "qmp-key-pulse-post-resume-enter-dispatcher-gdb"
            elif schema == 14:
                required.update({"qmp_enter_down_completed_monotonic",
                                 "qmp_enter_up_started_monotonic"})
                expected_observation = \
                    "qmp-key-pulse-post-dispatch-release-gdb"
            elif schema == 15:
                required.update({
                    "qmp_resume_status", "qmp_resume_timeout_seconds",
                    "qmp_resume_poll_seconds", "qmp_resume_started_monotonic",
                    "qmp_resume_observed_monotonic", "qmp_resume_checks",
                    "qmp_enter_down_completed_monotonic",
                    "qmp_enter_up_started_monotonic",
                    "input_observer_cleanup_status",
                    "input_observer_cleanup_started_monotonic",
                    "input_observer_cleanup_completed_monotonic",
                    "input_observer_cleanup_stop",
                })
                expected_observation = \
                    "qmp-key-pulse-resume-confirmed-dispatch-release-gdb"
            elif schema == 16:
                required.update({
                    "qmp_resume_status", "qmp_resume_timeout_seconds",
                    "qmp_resume_poll_seconds", "qmp_resume_started_monotonic",
                    "qmp_resume_observed_monotonic", "qmp_resume_checks",
                    "qmp_enter_down_completed_monotonic",
                    "qmp_enter_up_started_monotonic",
                    "input_observer_cleanup_status",
                    "input_observer_cleanup_method",
                    "input_observer_cleanup_return_code",
                    "input_observer_cleanup_started_monotonic",
                    "input_observer_process_completed_monotonic",
                    "qmp_cleanup_started_monotonic",
                    "qmp_cleanup_observed_monotonic", "qmp_cleanup_checks",
                    "input_observer_cleanup_completed_monotonic",
                })
                expected_observation = \
                    "qmp-key-pulse-runstate-confirmed-private-observer-gdb"
            elif schema == 17:
                required.update({
                    "input_key_release_settle_seconds",
                    "qmp_resume_status", "qmp_resume_timeout_seconds",
                    "qmp_resume_poll_seconds", "qmp_resume_started_monotonic",
                    "qmp_resume_observed_monotonic", "qmp_resume_checks",
                    "qmp_enter_down_completed_monotonic",
                    "qmp_enter_up_started_monotonic",
                    "input_observer_cleanup_status",
                    "input_observer_cleanup_method",
                    "input_observer_cleanup_return_code",
                    "input_observer_cleanup_started_monotonic",
                    "input_observer_process_completed_monotonic",
                    "qmp_cleanup_started_monotonic",
                    "qmp_cleanup_observed_monotonic", "qmp_cleanup_checks",
                    "input_observer_cleanup_completed_monotonic",
                })
                expected_observation = \
                    "qmp-key-pulse-runstate-and-release-confirmed-private-observer-gdb"
            elif schema == 22:
                required.update({
                    "input_layout_artifact", "input_layout_sha256",
                    "input_layout", "input_boundary_keyboard_release",
                    "qmp_resume_status", "qmp_resume_timeout_seconds",
                    "qmp_resume_poll_seconds", "qmp_resume_started_monotonic",
                    "qmp_resume_observed_monotonic", "qmp_resume_checks",
                    "qmp_enter_down_completed_monotonic",
                    "qmp_enter_up_started_monotonic",
                    "qmp_enter_guest_release_status",
                    "qmp_enter_guest_release_samples",
                    "input_observer_cleanup_status",
                    "input_observer_cleanup_method",
                    "input_observer_cleanup_return_code",
                    "input_observer_cleanup_timeout_seconds",
                    "input_observer_cleanup_started_monotonic",
                    "input_observer_process_completed_monotonic",
                    "input_observer_target_detached_monotonic",
                    "input_observer_cleanup_detach_response_token",
                    "input_observer_cleanup_exit_response_token",
                    "qmp_cleanup_started_monotonic",
                    "qmp_cleanup_observed_monotonic", "qmp_cleanup_checks",
                    "input_observer_cleanup_completed_monotonic",
                })
                expected_observation = \
                    "qmp-guest-release-confirmed-bounded-stopped-detach-observer-gdb"
            else:
                expected_observation = \
                    "qmp-key-pulse-pre-enter-and-dispatcher-gdb"
        else:
            required.update({
                "input_key_stroke_atomic",
                "qmp_resume_status", "qmp_resume_timeout_seconds",
                "qmp_resume_poll_seconds", "qmp_resume_started_monotonic",
                "qmp_resume_observed_monotonic", "qmp_resume_checks",
                "qmp_enter_stroke_completed_monotonic",
                "input_observer_cleanup_status",
                "input_observer_cleanup_method",
                "input_observer_cleanup_return_code",
                "input_observer_cleanup_started_monotonic",
                "input_observer_process_completed_monotonic",
                "qmp_cleanup_started_monotonic",
                "qmp_cleanup_observed_monotonic", "qmp_cleanup_checks",
                "input_observer_cleanup_completed_monotonic",
            })
            if schema in (19, 20, 21):
                required.update({
                    "input_observer_cleanup_timeout_seconds",
                    "input_observer_cleanup_detach_response_token",
                    "input_observer_cleanup_exit_response_token",
                })
                if schema == 21:
                    required.add("input_observer_target_detached_monotonic")
                    expected_observation = \
                        "qmp-atomic-key-stroke-bounded-stopped-detach-observer-gdb"
                elif schema == 20:
                    required.update({
                        "input_observer_cleanup_interrupt_response_token",
                        "input_observer_cleanup_stop",
                    })
                    expected_observation = \
                        "qmp-atomic-key-stroke-bounded-interrupt-detach-observer-gdb"
                else:
                    expected_observation = \
                        "qmp-atomic-key-stroke-bounded-observer-cleanup-gdb"
            else:
                expected_observation = \
                    "qmp-atomic-key-stroke-runstate-confirmed-private-observer-gdb"
    else:
        expected_observation = None
    require(required.issubset(row),
            "command input observation lacks mandatory fields")
    if not required.issubset(row):
        return errors
    require(expected_observation is not None and
            profile.get("command_input_observation") == expected_observation and
            profile.get("input_profile") == "framed",
            "profile does not require pre-Enter framed observation")
    if schema in (10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26):
        require(row.get("qmp_text_status") == "PASS" and
                row.get("qmp_enter_status") == "PASS" and
                row.get("qmp_input_status") == "PASS",
                "QMP text or terminator operation did not complete")
    else:
        require(row.get("hmp_text_status") == "PASS" and
                row.get("hmp_enter_status") == "PASS" and
                row.get("hmp_input_status") == "PASS",
                "HMP text or terminator operation did not complete")
    require(row.get("pre_enter_status") == "PASS",
            "frame was not exact before the terminator")
    require(row.get("dispatcher_status") == "OBSERVED" and
            row.get("guest_input_status") == "PASS",
            "guest shell dispatcher input was not observed exactly")
    require(row.get("input_boundary_status") == "CLEAN" and
            row.get("input_boundary_active") == 1 and
            row.get("input_boundary_length") == 0 and
            row.get("input_boundary_position") == 0,
            "shell input boundary was not active and empty")
    expected = expected_frame.encode("ascii")
    try:
        pre_observed = bytes.fromhex(row.get("pre_enter_bytes_hex", ""))
    except (TypeError, ValueError):
        pre_observed = b""
        errors.append("pre-Enter hexadecimal bytes are invalid")
    try:
        observed = bytes.fromhex(row.get("guest_input_bytes_hex", ""))
    except (TypeError, ValueError):
        observed = b""
        errors.append("guest input hexadecimal bytes are invalid")
    require(pre_observed == expected and
            row.get("pre_enter_length") == len(expected) and
            row.get("pre_enter_position") == len(expected) and
            row.get("pre_enter_active") == 1 and
            row.get("pre_enter_text") == expected_frame,
            "pre-Enter shell buffer differs from the host frame")
    require(observed == expected and
            row.get("guest_input_length") == len(expected) and
            row.get("guest_input_text") == expected_frame,
            "dispatcher shell input differs from the host frame")
    input_layout = (validated_input_layout(row, profile_dir, errors)
                    if schema in (22, 23, 24, 25, 26) else None)
    symbols = row.get("input_symbol_addresses")
    parsed_symbols = {}
    expected_symbols = {"g_shell_active", "g_buffer", "g_len", "g_pos"}
    if schema in (22, 23, 24, 25, 26):
        expected_symbols.add("xhci_driver")
    require(isinstance(symbols, dict) and
            set(symbols) == expected_symbols,
            "shell input symbol inventory is invalid")
    if isinstance(symbols, dict):
        try:
            parsed_symbols = {name: int(value, 0)
                              for name, value in symbols.items()}
        except (TypeError, ValueError):
            errors.append("shell input symbol address is invalid")
        if parsed_symbols:
            require(all(value > 0 for value in parsed_symbols.values()) and
                    len(set(parsed_symbols.values())) == len(expected_symbols),
                    "shell input symbol addresses are zero or duplicated")

    physical_addresses = None
    if schema in (24, 25, 26) and parsed_symbols:
        physical_addresses, address_errors = input_address_map_errors(
            row, profile_dir, parsed_symbols)
        errors.extend(address_errors)

    samples = row.get("input_boundary_samples")
    require(isinstance(samples, list) and bool(samples),
            "shell input boundary samples are absent")
    if not isinstance(samples, list) or not samples:
        return errors
    require([sample.get("sample") for sample in samples] ==
            list(range(1, len(samples) + 1)),
            "shell input boundary sample sequence is invalid")
    for index, sample in enumerate(
            samples if schema not in (24, 25, 26) else []):
        if not isinstance(sample, dict):
            errors.append("shell input boundary sample is malformed")
            continue
        files = {}
        for name_field, hash_field in (("log", "log_sha256"),
                                       ("command", "command_sha256"),
                                       ("argv", "argv_sha256")):
            name = sample.get(name_field)
            if (not isinstance(name, str) or Path(name).name != name or
                    not name):
                errors.append("input boundary artifact name is unsafe")
                continue
            artifact = profile_dir / name
            files[name_field] = artifact
            if not artifact.is_file():
                errors.append("input boundary artifact is absent")
            elif sample.get(hash_field) != sha256(artifact):
                errors.append("input boundary artifact hash differs")
        log_path = files.get("log")
        command_path = files.get("command")
        if not log_path or not log_path.is_file():
            continue
        raw = log_path.read_text(errors="replace")
        try:
            active = mi_memory_from_log(
                raw, sample.get("active_response_token"), 1)[0]
            length = int.from_bytes(mi_memory_from_log(
                raw, sample.get("length_response_token"), 4),
                "little", signed=True)
            position = int.from_bytes(mi_memory_from_log(
                raw, sample.get("position_response_token"), 4),
                "little", signed=True)
        except (TypeError, ValueError):
            errors.append("input boundary values cannot be derived from GDB")
            continue
        release_observed = True
        if schema in (22, 23, 24, 25, 26) and input_layout is not None:
            sample_errors = keyboard_sample_errors(
                sample, raw,
                command_path.read_text(errors="replace")
                if command_path and command_path.is_file() else None,
                parsed_symbols, input_layout)
            errors.extend(sample_errors)
            release_observed = \
                sample.get("keyboard_release_observed") is True
        clean = (active == 1 and length == 0 and position == 0 and
                 release_observed)
        require((sample.get("active"), sample.get("length"),
                 sample.get("position"), sample.get("clean")) ==
                (active, length, position, clean),
                "input boundary summary differs from GDB")
        require(clean == (index == len(samples) - 1),
                "only the final input boundary sample may be clean")
        if command_path and command_path.is_file() and parsed_symbols:
            commands = command_path.read_text(errors="replace")
            for name, size in (("g_shell_active", 1), ("g_len", 4),
                               ("g_pos", 4)):
                require(
                    f"-data-read-memory-bytes 0x{parsed_symbols[name]:x} {size}" in
                    commands,
                    "input boundary command does not use recorded symbols")
            require("break-insert" not in commands and "$rdi" not in commands,
                    "input boundary observer attempted dispatch inspection")

    if schema in (22, 23, 24, 25, 26):
        require(row.get("input_boundary_keyboard_release") is True,
                "input boundary lacks a consumed keyboard release")
        release_samples = row.get("qmp_enter_guest_release_samples")
        require(row.get("qmp_enter_guest_release_status") == "PASS" and
                isinstance(release_samples, list) and bool(release_samples),
                "Enter release was not observed in the guest")
        if isinstance(release_samples, list) and schema not in (24, 25, 26):
            require([sample.get("sample") for sample in release_samples] ==
                    list(range(1, len(release_samples) + 1)),
                    "Enter release sample sequence is invalid")
            for sample_index, sample in enumerate(release_samples):
                if not isinstance(sample, dict):
                    errors.append("Enter release sample is malformed")
                    continue
                files = {}
                for name_field, hash_field in (
                        ("log", "log_sha256"),
                        ("command", "command_sha256"),
                        ("argv", "argv_sha256")):
                    name = sample.get(name_field)
                    if (not isinstance(name, str) or
                            Path(name).name != name or not name):
                        errors.append("Enter release artifact name is unsafe")
                        continue
                    artifact = profile_dir / name
                    files[name_field] = artifact
                    if not artifact.is_file():
                        errors.append("Enter release artifact is absent")
                    elif sample.get(hash_field) != sha256(artifact):
                        errors.append("Enter release artifact hash differs")
                log_path, command_path = files.get("log"), files.get("command")
                if (log_path and log_path.is_file() and input_layout is not None):
                    errors.extend(keyboard_sample_errors(
                        sample, log_path.read_text(errors="replace"),
                        command_path.read_text(errors="replace")
                        if command_path and command_path.is_file() else None,
                        parsed_symbols, input_layout))
                require(sample.get("keyboard_release_observed") is
                        (sample_index == len(release_samples) - 1),
                        "only the final Enter release sample may be released")

    if schema in (5, 6, 7):
        delivery_samples = row.get("input_delivery_samples")
        require(row.get("input_delivery_status") == "COMPLETE",
                "bounded input delivery did not complete")
        if schema == 5:
            require(row.get("input_delivery_timeout_seconds") == 30.0 and
                    row.get("input_delivery_sample_delay_seconds") == 0.25,
                    "bounded input delivery policy differs")
        elif schema == 6:
            require(row.get("input_delivery_timeout_seconds") == 30.0 and
                    row.get("input_delivery_initial_quiet_seconds") == 2.0 and
                    row.get("input_delivery_stability_seconds") == 0.5 and
                    row.get("input_delivery_sample_delay_seconds") == 0.5,
                    "stable input delivery policy differs")
        else:
            require(row.get("input_delivery_timeout_seconds") == 30.0 and
                    row.get("input_delivery_stability_seconds") == 0.5 and
                    row.get("input_delivery_sample_delay_seconds") == 0.25,
                    "release-stable input delivery policy differs")
        require(isinstance(delivery_samples, list) and bool(delivery_samples),
                "bounded input delivery samples are absent")
        previous_length = -1
        observed_times = []
        statuses = []
        if isinstance(delivery_samples, list):
            require([sample.get("sample") for sample in delivery_samples] ==
                    list(range(1, len(delivery_samples) + 1)),
                    "bounded input delivery sample sequence is invalid")
            for index, sample in enumerate(delivery_samples):
                if not isinstance(sample, dict):
                    errors.append("bounded input delivery sample is malformed")
                    continue
                files = {}
                for name_field, hash_field in (
                        ("log", "log_sha256"),
                        ("command", "command_sha256"),
                        ("argv", "argv_sha256")):
                    name = sample.get(name_field)
                    if (not isinstance(name, str) or Path(name).name != name or
                            not name):
                        errors.append(
                            "bounded input delivery artifact name is unsafe")
                        continue
                    artifact = profile_dir / name
                    files[name_field] = artifact
                    if not artifact.is_file():
                        errors.append(
                            "bounded input delivery artifact is absent")
                    elif sample.get(hash_field) != sha256(artifact):
                        errors.append(
                            "bounded input delivery artifact hash differs")
                log_path = files.get("log")
                command_path = files.get("command")
                if not log_path or not log_path.is_file():
                    continue
                raw = log_path.read_text(errors="replace")
                try:
                    active = mi_memory_from_log(
                        raw, sample.get("active_response_token"), 1)[0]
                    length = int.from_bytes(mi_memory_from_log(
                        raw, sample.get("length_response_token"), 4),
                        "little", signed=True)
                    position = int.from_bytes(mi_memory_from_log(
                        raw, sample.get("position_response_token"), 4),
                        "little", signed=True)
                    memory = mi_memory_from_log(
                        raw, sample.get("memory_response_token"), 256)
                    observed = memory[:length] if 0 <= length < 256 else b""
                    terminated = (0 <= length < 256 and memory[length] == 0)
                except (TypeError, ValueError):
                    errors.append(
                        "bounded input delivery values cannot be derived from GDB")
                    continue
                status = ("EXACT" if observed == expected else
                          ("PREFIX" if expected.startswith(observed) else
                           "DIVERGED"))
                observed_text = (observed.decode("ascii") if
                                 observed.isascii() else None)
                coherent = (active == 1 and position == length == len(observed)
                            and terminated)
                require(coherent and status != "DIVERGED",
                        "bounded input delivery sample is incoherent or diverged")
                require((sample.get("active"), sample.get("length"),
                         sample.get("position"), sample.get("terminated"),
                         sample.get("observed_bytes_hex"),
                         sample.get("observed_text"), sample.get("status")) ==
                        (active, length, position, terminated, observed.hex(),
                         observed_text, status),
                        "bounded input delivery summary differs from GDB")
                require(length >= previous_length,
                        "bounded input delivery regressed to a shorter prefix")
                previous_length = length
                statuses.append(status)
                observed_time = sample.get("observed_monotonic")
                require(isinstance(observed_time, (int, float)),
                        "bounded input delivery sample time is invalid")
                if isinstance(observed_time, (int, float)):
                    observed_times.append(observed_time)
                if schema == 5:
                    require((status == "EXACT") ==
                            (index == len(delivery_samples) - 1),
                            "only the final bounded delivery sample may be exact")
                if command_path and command_path.is_file() and parsed_symbols:
                    commands = command_path.read_text(errors="replace")
                    for name, size in (("g_shell_active", 1), ("g_len", 4),
                                       ("g_pos", 4), ("g_buffer", 256)):
                        require(
                            f"-data-read-memory-bytes 0x{parsed_symbols[name]:x} {size}" in
                            commands,
                            "bounded input delivery command uses another symbol")
                    require("break-insert" not in commands and
                            "$rdi" not in commands,
                            "bounded input delivery attempted dispatch inspection")
        require(observed_times == sorted(observed_times) and
                len(set(observed_times)) == len(observed_times),
                "bounded input delivery sample times are not increasing")
        if schema in (6, 7) and statuses:
            exact_indices = [index for index, status in enumerate(statuses)
                             if status == "EXACT"]
            require(len(exact_indices) >= 2,
                    "stable input delivery lacks two exact observations")
            if exact_indices:
                first_exact = exact_indices[0]
                require(all(status == "PREFIX"
                            for status in statuses[:first_exact]) and
                        all(status == "EXACT"
                            for status in statuses[first_exact:]),
                        "stable input delivery changed after becoming exact")
                if len(exact_indices) >= 2 and len(observed_times) == len(statuses):
                    stable_span = (observed_times[exact_indices[-1]] -
                                   observed_times[first_exact])
                    recorded_span = row.get(
                        "input_delivery_stable_span_seconds")
                    require(stable_span >= 0.5 and
                            isinstance(recorded_span, (int, float)) and
                            abs(recorded_span - stable_span) < 1e-9,
                            "stable input delivery interval is too short or differs")

    observer_files = {}
    for field, hash_field in (("input_observer_log",
                               "input_observer_log_sha256"),
                              ("input_observer_command",
                               "input_observer_command_sha256"),
                              ("input_observer_argv",
                               "input_observer_argv_sha256")):
        name = row.get(field)
        if not isinstance(name, str) or Path(name).name != name or not name:
            errors.append("input observer artifact name is unsafe")
            continue
        artifact = profile_dir / name
        observer_files[field] = artifact
        if not artifact.is_file():
            errors.append("input observer artifact is absent")
        elif row.get(hash_field) != sha256(artifact):
            errors.append("input observer artifact hash differs")
    final_log = observer_files.get("input_observer_log")
    final_command = observer_files.get("input_observer_command")
    raw = ""
    if final_log and final_log.is_file():
        raw = final_log.read_text(errors="replace")
        stops = [line for line in raw.splitlines()
                 if line.startswith("*stopped") and
                 'reason="breakpoint-hit"' in line and
                 'func="shell_dispatch_command_line"' in line]
        require(len(stops) == 1 and row.get("dispatcher_stop") == stops[0],
                "GDB did not observe the recorded dispatcher entry exactly once")
        try:
            raw_active = mi_memory_from_log(
                raw, row.get("pre_enter_active_response_token"), 1)[0]
            raw_length = int.from_bytes(mi_memory_from_log(
                raw, row.get("pre_enter_length_response_token"), 4),
                "little", signed=True)
            raw_position = int.from_bytes(mi_memory_from_log(
                raw, row.get("pre_enter_position_response_token"), 4),
                "little", signed=True)
            raw_pre_memory = mi_memory_from_log(
                raw, row.get("pre_enter_memory_response_token"), 256)
            raw_pre = raw_pre_memory.split(b"\0", 1)[0]
            raw_memory = mi_memory_from_log(
                raw, row.get("input_observer_memory_response_token"), 256)
            raw_observed = raw_memory.split(b"\0", 1)[0]
            require((raw_active, raw_length, raw_position, raw_pre) ==
                    (1, len(expected), len(expected), expected),
                    "GDB raw pre-Enter shell buffer differs")
            require(raw_pre == pre_observed,
                    "pre-Enter summary differs from GDB raw bytes")
            require(raw_observed == expected and raw_observed == observed,
                    "GDB raw dispatcher line differs from the host frame")
        except (TypeError, ValueError):
            errors.append("guest shell lines cannot be derived from GDB")
    if final_command and final_command.is_file() and parsed_symbols:
        commands = final_command.read_text(errors="replace")
        for name, size in (("g_shell_active", 1), ("g_len", 4),
                           ("g_pos", 4), ("g_buffer", 256)):
            require(
                f"-data-read-memory-bytes 0x{parsed_symbols[name]:x} {size}" in
                commands,
                "pre-Enter observer command does not use recorded symbols")
        require("-break-insert shell_dispatch_command_line" in commands and
                "-data-read-memory-bytes $rdi 256" in commands,
                "dispatcher observer commands are incomplete")
        if schema == 15:
            cleanup_stop = row.get("input_observer_cleanup_stop")
            require(row.get("input_observer_cleanup_status") == "PASS" and
                    "-exec-interrupt" in commands and
                    "-target-detach" in commands and
                    "-gdb-exit" in commands and
                    isinstance(cleanup_stop, str) and
                    cleanup_stop in raw and
                    cleanup_stop.startswith("*stopped"),
                    "GDB observer cleanup is incomplete")
        elif schema in (19, 20, 21, 22, 23, 24, 25, 26):
            return_code = row.get("input_observer_cleanup_return_code")
            interrupt_token = row.get(
                "input_observer_cleanup_interrupt_response_token")
            detach_token = row.get(
                "input_observer_cleanup_detach_response_token")
            exit_token = row.get(
                "input_observer_cleanup_exit_response_token")
            cleanup_stop = row.get("input_observer_cleanup_stop")
            transition_valid = (
                schema in (19, 21, 22, 23, 24, 25, 26) or
                (isinstance(interrupt_token, int) and
                 isinstance(detach_token, int) and
                 isinstance(cleanup_stop, str) and
                 cleanup_stop.startswith("*stopped") and
                 cleanup_stop in raw and
                 f"{interrupt_token}-exec-interrupt --all" in commands and
                 f"{interrupt_token}^done" in raw and
                 interrupt_token < detach_token))
            method = {
                19: "detach-running-private-observer",
                20: "interrupt-detach-private-observer",
                21: "detach-stopped-private-observer",
                22: "detach-stopped-private-observer",
                23: "detach-stopped-private-observer",
                24: "detach-stopped-private-observer",
                25: "detach-stopped-private-observer",
                26: "detach-stopped-private-observer",
            }.get(schema)
            stopped_detach_valid = (
                schema not in (21, 22, 23, 24, 25, 26) or
                ("-exec-interrupt" not in commands and
                 commands.count("-exec-continue") == 1 and
                 isinstance(row.get(
                     "input_observer_target_detached_monotonic"),
                            (int, float)) and
                 not isinstance(row.get(
                     "input_observer_target_detached_monotonic"), bool)))
            require(row.get("input_observer_cleanup_status") == "PASS" and
                    row.get("input_observer_cleanup_timeout_seconds") == 5.0 and
                    row.get("input_observer_cleanup_method") == method and
                    return_code == 0 and
                    isinstance(detach_token, int) and
                    isinstance(exit_token, int) and
                    detach_token < exit_token and
                    transition_valid and stopped_detach_valid and
                    f"{detach_token}-target-detach" in commands and
                    f"{exit_token}-gdb-exit" in commands and
                    f"{detach_token}^done" in raw and
                    f"{exit_token}^exit" in raw and
                    ((schema == 20 and "-exec-interrupt --all" in commands) or
                     (schema in (19, 21, 22, 23, 24, 25, 26) and
                      "-exec-interrupt" not in commands)),
                    "bounded GDB observer cleanup is incomplete")
        elif schema in (16, 17, 18):
            return_code = row.get("input_observer_cleanup_return_code")
            require(row.get("input_observer_cleanup_status") == "PASS" and
                    row.get("input_observer_cleanup_method") ==
                    "terminate-private-observer" and
                    isinstance(return_code, int) and return_code <= 0 and
                    "-exec-interrupt" not in commands and
                    "-target-detach" not in commands and
                    "-gdb-exit" not in commands,
                    "private GDB observer cleanup is incomplete")

    if schema in (9, 10):
        errors.extend(cursor_delivery_errors(
            row, profile_dir, expected, parsed_symbols))
    elif schema in (23, 24, 25, 26):
        errors.extend(confirmed_delivery_errors(
            row, profile_dir, expected, parsed_symbols, input_layout,
            physical_addresses))
    elif schema in (11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22):
        errors.extend(pulse_delivery_errors(
            row, profile_dir, expected, parsed_symbols, input_layout))

    if schema not in (10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26):
        hmp_operations = [
            ("hmp_enter_log", "hmp_enter_log_sha256", "key", "ret")]
        if schema != 9:
            hmp_operations.insert(
                0, ("hmp_text_log", "hmp_text_log_sha256", "text",
                    expected_frame))
        for name_field, hash_field, operation, value in hmp_operations:
            name = row.get(name_field)
            if not isinstance(name, str) or Path(name).name != name or not name:
                errors.append("HMP input artifact name is unsafe")
                continue
            artifact = profile_dir / name
            if not artifact.is_file():
                errors.append("HMP input artifact is absent")
                continue
            require(row.get(hash_field) == sha256(artifact),
                    "HMP input artifact hash differs")
            errors.extend(hmp_input_log_errors(
                artifact, operation, value, "framed",
                require_release_boundary=(schema == 6 and
                                          operation == "text")))

    if schema == 7:
        release_name = row.get("input_release_log")
        if (not isinstance(release_name, str) or
                Path(release_name).name != release_name or not release_name):
            errors.append("input release artifact name is unsafe")
        else:
            release_path = profile_dir / release_name
            if not release_path.is_file():
                errors.append("input release artifact is absent")
            else:
                require(row.get("input_release_log_sha256") ==
                        sha256(release_path),
                        "input release artifact hash differs")
                errors.extend(hmp_input_log_errors(
                    release_path, "key", "shift", "framed"))
        require(row.get("input_release_key") == "shift" and
                row.get("input_release_status") == "PASS",
                "printable-key release boundary did not complete")

    timing_prefix = ("qmp" if schema in
                     (10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26)
                     else "hmp")
    timing_fields = [f"{timing_prefix}_text_started_monotonic",
                     f"{timing_prefix}_text_completed_monotonic",
                     "pre_enter_observed_monotonic",
                     "input_observer_armed_monotonic"]
    if schema == 12:
        timing_fields.append("qmp_enter_scheduled_monotonic")
    timing_fields.append("input_observer_resumed_monotonic")
    if schema == 13:
        timing_fields.append("qmp_enter_started_monotonic")
    if schema in (15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26):
        timing_fields.extend(("qmp_resume_started_monotonic",
                              "qmp_resume_observed_monotonic"))
    if schema in (18, 19, 20, 21):
        timing_fields.extend(("qmp_enter_stroke_completed_monotonic",
                              "qmp_enter_completed_monotonic"))
    if schema in (14, 15, 16, 17):
        timing_fields.append("qmp_enter_down_completed_monotonic")
    if schema in (10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26):
        timing_fields.append("dispatcher_observed_monotonic")
        if schema != 21:
            timing_fields.append("input_observer_dispatch_resumed_monotonic")
    if schema in (14, 15, 16, 17):
        timing_fields.append("qmp_enter_up_started_monotonic")
    if schema not in (18, 19, 20, 21):
        timing_fields.append(f"{timing_prefix}_enter_completed_monotonic")
    if schema == 15:
        timing_fields.extend(("input_observer_cleanup_started_monotonic",
                              "input_observer_cleanup_completed_monotonic"))
    elif schema in (16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26):
        timing_fields.append("input_observer_cleanup_started_monotonic")
        if schema == 21:
            timing_fields.extend((
                "input_observer_target_detached_monotonic",
                "input_observer_dispatch_resumed_monotonic"))
        timing_fields.extend(("input_observer_process_completed_monotonic",
                              "qmp_cleanup_started_monotonic",
                              "qmp_cleanup_observed_monotonic",
                              "input_observer_cleanup_completed_monotonic"))
    timing_fields.append(f"{timing_prefix}_input_completed_monotonic")
    if schema in (22, 23, 24, 25, 26):
        timing_fields = [
            "qmp_text_started_monotonic", "qmp_text_completed_monotonic",
            "pre_enter_observed_monotonic", "input_observer_armed_monotonic",
            "input_observer_resumed_monotonic", "qmp_resume_started_monotonic",
            "qmp_resume_observed_monotonic",
            "qmp_enter_down_completed_monotonic",
            "qmp_enter_up_started_monotonic", "qmp_enter_completed_monotonic",
            "dispatcher_observed_monotonic",
            "input_observer_cleanup_started_monotonic",
            "input_observer_target_detached_monotonic",
            "input_observer_dispatch_resumed_monotonic",
            "input_observer_process_completed_monotonic",
            "qmp_cleanup_started_monotonic", "qmp_cleanup_observed_monotonic",
            "input_observer_cleanup_completed_monotonic",
            "qmp_input_completed_monotonic",
        ]
        if schema in (24, 25, 26):
            timing_fields = [
                "qmp_text_started_monotonic", "qmp_text_completed_monotonic",
                "pre_enter_observed_monotonic", "input_observer_armed_monotonic",
                "input_observer_resumed_monotonic", "qmp_resume_started_monotonic",
                "qmp_resume_observed_monotonic",
                "qmp_enter_down_completed_monotonic",
                "dispatcher_observed_monotonic",
                "input_observer_cleanup_started_monotonic",
                "input_observer_target_detached_monotonic",
                "input_observer_dispatch_resumed_monotonic",
                "input_observer_process_completed_monotonic",
                "qmp_cleanup_started_monotonic", "qmp_cleanup_observed_monotonic",
                "input_observer_cleanup_completed_monotonic",
                "qmp_enter_up_started_monotonic", "qmp_enter_completed_monotonic",
                "qmp_input_completed_monotonic",
            ]
        elif schema == 23:
            timing_fields = [
                "qmp_text_started_monotonic", "qmp_text_completed_monotonic",
                "pre_enter_observed_monotonic", "input_observer_armed_monotonic",
                "input_observer_resumed_monotonic", "qmp_resume_started_monotonic",
                "qmp_resume_observed_monotonic",
                "qmp_enter_down_completed_monotonic",
                "dispatcher_observed_monotonic", "qmp_enter_up_started_monotonic",
                "qmp_enter_completed_monotonic",
                "input_observer_cleanup_started_monotonic",
                "input_observer_target_detached_monotonic",
                "input_observer_dispatch_resumed_monotonic",
                "input_observer_process_completed_monotonic",
                "qmp_cleanup_started_monotonic", "qmp_cleanup_observed_monotonic",
                "input_observer_cleanup_completed_monotonic",
                "qmp_input_completed_monotonic",
            ]
    timing = [row.get(name) for name in timing_fields]
    require(all(isinstance(value, (int, float)) for value in timing) and
            timing == sorted(timing),
            "pre-Enter and dispatch observation timing is invalid")
    if schema == 4:
        require(row.get("input_delivery_settle_seconds") == 0.75,
                "input delivery settle interval differs from the fixed contract")
    elif schema == 5:
        started = row.get("input_delivery_started_monotonic")
        completed = row.get("input_delivery_completed_monotonic")
        require(isinstance(started, (int, float)) and
                isinstance(completed, (int, float)) and
                row.get("hmp_text_completed_monotonic") <= started <=
                completed <= row.get("pre_enter_observed_monotonic") and
                completed - started <=
                row.get("input_delivery_timeout_seconds"),
                "bounded input delivery timing is invalid")
    elif schema == 6:
        started = row.get("input_delivery_started_monotonic")
        quiet_started = row.get("input_delivery_quiet_started_monotonic")
        quiet_completed = row.get("input_delivery_quiet_completed_monotonic")
        completed = row.get("input_delivery_completed_monotonic")
        require(all(isinstance(value, (int, float)) for value in
                    (started, quiet_started, quiet_completed, completed)) and
                row.get("hmp_text_completed_monotonic") <= started ==
                quiet_started and quiet_completed - quiet_started >= 2.0 and
                quiet_completed <= completed <=
                row.get("pre_enter_observed_monotonic") and
                completed - started <=
                row.get("input_delivery_timeout_seconds"),
                "stable input delivery timing is invalid")
    elif schema == 7:
        started = row.get("input_delivery_started_monotonic")
        release_started = row.get("input_release_started_monotonic")
        release_completed = row.get("input_release_completed_monotonic")
        completed = row.get("input_delivery_completed_monotonic")
        exact_times = [sample.get("observed_monotonic") for sample in
                       row.get("input_delivery_samples", [])
                       if isinstance(sample, dict) and
                       sample.get("status") == "EXACT"]
        require(all(isinstance(value, (int, float)) for value in
                    (started, release_started, release_completed, completed)) and
                len(exact_times) >= 2 and
                row.get("hmp_text_completed_monotonic") <= started <=
                exact_times[0] <= release_started <= release_completed <=
                exact_times[-1] <= completed <=
                row.get("pre_enter_observed_monotonic") and
                completed - started <=
                row.get("input_delivery_timeout_seconds"),
                "release-stable input delivery timing is invalid")
    elif schema == 9:
        started = row.get("input_delivery_started_monotonic")
        text_completed = row.get("hmp_text_completed_monotonic")
        completed = row.get("input_delivery_completed_monotonic")
        pre_enter = row.get("pre_enter_observed_monotonic")
        timeout = row.get("input_delivery_timeout_seconds")
        require(all(isinstance(value, (int, float)) and
                    not isinstance(value, bool) and math.isfinite(value)
                    for value in
                    (started, text_completed, completed, pre_enter,
                     timeout)) and
                started <= text_completed <= completed <=
                pre_enter and
                completed - started <= timeout,
                "cursor-feedback input delivery timing is invalid")
    elif schema in (10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26):
        started = row.get("input_delivery_started_monotonic")
        text_completed = row.get("qmp_text_completed_monotonic")
        completed = row.get("input_delivery_completed_monotonic")
        pre_enter = row.get("pre_enter_observed_monotonic")
        timeout = row.get("input_delivery_timeout_seconds")
        require(all(isinstance(value, (int, float)) and
                    not isinstance(value, bool) and math.isfinite(value)
                    for value in
                    (started, text_completed, completed, pre_enter,
                     timeout)) and
                started <= text_completed <= completed <=
                pre_enter and completed - started <= timeout,
                "QMP bounded-key delivery timing is invalid")
    return errors


def verify_transport_rejection(profile_dir, record_path, serial_path):
    errors = []

    def require(condition, message):
        if not condition:
            errors.append(message)

    try:
        rows = read_commands(record_path)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        return [f"rejection record cannot be read: {error}"]
    if len(rows) != 1 or rows[0].get("malformed"):
        return ["rejection evidence must contain exactly one valid record"]
    row = rows[0]
    sequence = row.get("sequence")
    payload = row.get("payload")
    supplied_crc = row.get("crc32")
    if (not isinstance(sequence, int) or sequence < 1 or
            not isinstance(payload, str) or
            not isinstance(supplied_crc, str)):
        return ["rejection record identity is malformed"]
    expected_crc = f"{zlib.crc32(payload.encode('ascii')) & 0xffffffff:08x}"
    frame = f"tasktest exec {sequence} {supplied_crc} {payload}"
    require(row.get("schema") in
            (4, 5, 6, 7, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26) and
            row.get("kind") == "framed-command-rejection",
            "rejection record schema is unsupported")
    require(re.fullmatch(r"[0-9a-f]{8}", supplied_crc) is not None and
            supplied_crc != expected_crc and
            row.get("expected_crc32") == expected_crc,
            "rejection checksum control is not actually invalid")
    require(row.get("expected_rejection") == "crc" and
            row.get("classification") == "EXPECTED_FRAME_REJECTED" and
            row.get("disposition") == "REJECTED",
            "rejection classification is not the expected CRC refusal")
    observation_modes = {
        4: "pre-enter-and-dispatcher-gdb",
        5: "bounded-pre-enter-and-dispatcher-gdb",
        6: "stable-pre-enter-and-dispatcher-gdb",
        7: "release-stable-pre-enter-and-dispatcher-gdb",
        9: "cursor-feedback-pre-enter-and-dispatcher-gdb",
        10: "qmp-keyup-feedback-pre-enter-and-dispatcher-gdb",
        11: "qmp-key-pulse-pre-enter-and-dispatcher-gdb",
        12: "qmp-key-pulse-scheduled-enter-dispatcher-gdb",
        13: "qmp-key-pulse-post-resume-enter-dispatcher-gdb",
        14: "qmp-key-pulse-post-dispatch-release-gdb",
        15: "qmp-key-pulse-resume-confirmed-dispatch-release-gdb",
        16: "qmp-key-pulse-runstate-confirmed-private-observer-gdb",
        17: "qmp-key-pulse-runstate-and-release-confirmed-private-observer-gdb",
        18: "qmp-atomic-key-stroke-runstate-confirmed-private-observer-gdb",
        19: "qmp-atomic-key-stroke-bounded-observer-cleanup-gdb",
        20: "qmp-atomic-key-stroke-bounded-interrupt-detach-observer-gdb",
        21: "qmp-atomic-key-stroke-bounded-stopped-detach-observer-gdb",
        22: "qmp-guest-release-confirmed-bounded-stopped-detach-observer-gdb",
        23: "qmp-guest-press-release-confirmed-bounded-stopped-detach-observer-gdb",
        24: "qmp-physical-press-release-confirmed-bounded-stopped-detach-observer-gdb",
        25: "qmp-physical-atomic-stroke-confirmed-bounded-stopped-detach-observer-gdb",
        26: "qmp-stable-physical-atomic-stroke-confirmed-bounded-stopped-detach-observer-gdb",
    }
    profile = {
        "command_input_observation": observation_modes.get(row.get("schema")),
        "input_profile": "framed",
    }
    errors.extend(input_observation_errors(row, profile_dir, frame, profile))
    require(row.get("transport_status") == "PASS",
            "malformed control was not delivered exactly")
    require((row.get("accept_count"), row.get("begin_count"),
             row.get("end_count"), row.get("replay_count"),
             row.get("reject_count"), row.get("handler_marker_count")) ==
            (0, 0, 0, 0, 1, 0),
            "rejected frame has dispatch, replay, duplicate, or handler evidence")
    if not serial_path.is_file():
        errors.append("rejection raw serial is absent")
        return errors
    transcript = [item.rstrip(b"\r\n").decode(errors="replace")
                  for item in serial_path.read_bytes().splitlines(keepends=True)
                  if item.endswith((b"\n", b"\r"))]
    start = row.get("start_line_count")
    if not isinstance(start, int) or start < 0 or start > len(transcript):
        errors.append("rejection start boundary is invalid")
        return errors
    end = row.get("end_line_count")
    if not isinstance(end, int) or end <= start or end > len(transcript):
        errors.append("rejection end boundary is invalid")
        return errors
    segment = transcript[start:end]
    rejects = [(start + index + 1, match)
               for index, line in enumerate(segment)
               if (match := REJECT_RE.fullmatch(line)) and
               match.group(1) == str(sequence)]
    accepts = line_matches(segment, ACCEPT_RE, sequence)
    begins = line_matches(segment, BEGIN_RE, sequence)
    endings = line_matches(segment, END_RE, sequence)
    replays = line_matches(segment, REPLAY_RE, sequence)
    marker = "[INPUTTEST][MARKER] name=" + payload.split()[-1]
    markers = [line for line in segment if line == marker]
    expected_reason = (f"reason=crc expected_seq={sequence} "
                       f"expected_crc={supplied_crc} actual_crc={expected_crc}")
    require(len(rejects) == 1 and rejects[0][1].group(2) == expected_reason,
            "raw serial lacks the exact CRC rejection")
    require(not accepts and not begins and not endings and not replays and
            not markers,
            "raw serial shows rejected-frame dispatch or handler execution")
    if len(rejects) == 1:
        require(row.get("rejection_line") == rejects[0][0] and
                row.get("end_line_count") == rejects[0][0],
                "rejection record boundary differs from raw serial")
    return errors


def verify_transport_command(profile_dir, record_path, serial_path):
    try:
        rows = read_commands(record_path)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        return [f"command record cannot be read: {error}"]
    if len(rows) != 1 or rows[0].get("malformed"):
        return ["command evidence must contain exactly one valid record"]
    if not serial_path.is_file():
        return ["command raw serial is absent"]
    transcript = []
    byte_ends = []
    offset = 0
    for item in serial_path.read_bytes().splitlines(keepends=True):
        offset += len(item)
        if not item.endswith((b"\n", b"\r")):
            break
        transcript.append(item.rstrip(b"\r\n").decode(errors="replace"))
        byte_ends.append(offset)
    row = rows[0]
    observation_modes = {
        4: "pre-enter-and-dispatcher-gdb",
        5: "bounded-pre-enter-and-dispatcher-gdb",
        6: "stable-pre-enter-and-dispatcher-gdb",
        7: "release-stable-pre-enter-and-dispatcher-gdb",
        9: "cursor-feedback-pre-enter-and-dispatcher-gdb",
        10: "qmp-keyup-feedback-pre-enter-and-dispatcher-gdb",
        11: "qmp-key-pulse-pre-enter-and-dispatcher-gdb",
        12: "qmp-key-pulse-scheduled-enter-dispatcher-gdb",
        13: "qmp-key-pulse-post-resume-enter-dispatcher-gdb",
        14: "qmp-key-pulse-post-dispatch-release-gdb",
        15: "qmp-key-pulse-resume-confirmed-dispatch-release-gdb",
        16: "qmp-key-pulse-runstate-confirmed-private-observer-gdb",
        17: "qmp-key-pulse-runstate-and-release-confirmed-private-observer-gdb",
        18: "qmp-atomic-key-stroke-runstate-confirmed-private-observer-gdb",
        19: "qmp-atomic-key-stroke-bounded-observer-cleanup-gdb",
        20: "qmp-atomic-key-stroke-bounded-interrupt-detach-observer-gdb",
        21: "qmp-atomic-key-stroke-bounded-stopped-detach-observer-gdb",
        22: "qmp-guest-release-confirmed-bounded-stopped-detach-observer-gdb",
        23: "qmp-guest-press-release-confirmed-bounded-stopped-detach-observer-gdb",
        24: "qmp-physical-press-release-confirmed-bounded-stopped-detach-observer-gdb",
        25: "qmp-physical-atomic-stroke-confirmed-bounded-stopped-detach-observer-gdb",
        26: "qmp-stable-physical-atomic-stroke-confirmed-bounded-stopped-detach-observer-gdb",
    }
    profile = {
        "name": row.get("profile_id"), "vm_id": row.get("vm_id"),
        "command_input_observation": observation_modes.get(row.get("schema")),
        "input_profile": "framed",
    }
    return validate_transaction(row, transcript, byte_ends, profile,
                                row.get("candidate_sha256"), profile_dir)


def verify_transport_input(profile_dir, record_path):
    """Verify the pre-dispatch bytes without treating handler status as input."""
    try:
        rows = read_commands(record_path)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        return [f"command record cannot be read: {error}"]
    if len(rows) != 1 or rows[0].get("malformed"):
        return ["command input evidence must contain exactly one valid record"]
    row = rows[0]
    sequence, payload = row.get("sequence"), row.get("payload")
    if (not isinstance(sequence, int) or isinstance(sequence, bool) or
            sequence < 1 or not isinstance(payload, str) or not payload):
        return ["command input identity is malformed"]
    try:
        payload_bytes = payload.encode("ascii")
    except UnicodeEncodeError:
        return ["command input payload is not ASCII"]
    expected_crc = f"{zlib.crc32(payload_bytes) & 0xffffffff:08x}"
    errors = []
    if (row.get("schema") not in (25, 26) or
            row.get("kind") != "framed-command" or
            row.get("crc32") != expected_crc):
        errors.append("command input frame identity differs")
    profile_path = profile_dir / "profile.env"
    if not profile_path.is_file():
        return errors + ["command input profile is absent"]
    profile = read_env(profile_path)
    frame = f"tasktest exec {sequence} {expected_crc} {payload}"
    errors.extend(input_observation_errors(row, profile_dir, frame, profile))
    return errors


def verify_transport_profile(root, profile_dir):
    errors = []
    candidate = root / "candidate" / "kernel.elf"
    paths = (profile_dir / "profile.env",
             profile_dir / "qemu" / "qemu-serial.log",
             profile_dir / "commands.jsonl",
             profile_dir / "qemu" / "launch.env")
    if not candidate.is_file() or not all(path.is_file() for path in paths):
        return ["transport profile evidence is incomplete"]
    candidate_hash = sha256(candidate)
    profile = read_env(paths[0])
    launch = read_env(paths[3])
    identity_profile = dict(profile)
    identity_profile["group"] = "soak"
    errors.extend(validate_profile_identity(
        profile_dir, identity_profile, launch, candidate_hash, focal=False))
    if profile.get("group") != "transport":
        errors.append("transport profile group is incorrect")
    transcript, byte_ends = complete_serial(paths[1])
    errors.extend(boot_errors(profile, transcript))
    commands = read_commands(paths[2])
    if [row.get("sequence") for row in commands] != list(
            range(1, len(commands) + 1)):
        errors.append("transport command sequence is incomplete or reordered")
    if [row.get("payload") for row in commands] != list(BASIC_COMMANDS):
        errors.append("transport profile command coverage or order differs")
    for row in commands:
        errors.extend(validate_transaction(
            row, transcript, byte_ends, profile, candidate_hash, profile_dir))
        segment = transaction_segment(transcript, row)
        if segment is not None:
            errors.extend(evidence_errors(
                row, segment, int(profile.get("smp", "0")), True))
    return errors

def validate_transaction(row, transcript, byte_ends, profile, candidate_hash,
                         profile_dir):
    errors = []

    def require(condition, message):
        if not condition:
            errors.append(message)

    required = {
        "schema", "kind", "profile_id", "vm_id", "candidate_sha256",
        "qemu_pid", "sequence", "payload", "crc32", "frame_length",
        "deadline_seconds", "start_line_count", "end_line_count",
        "start_byte_count", "end_byte_count",
        "serial_byte_count_at_trigger", "serial_byte_count_observed",
        "accepted_line", "begin_line", "end_line", "handler_status",
        "transport_status", "classification", "disposition",
        "accept_count", "begin_count", "end_count", "replay_count",
        "reject_count", "checksum_verified", "trigger_monotonic",
        "completed_monotonic",
    }
    require(not row.get("malformed"), row.get("malformed", "malformed row"))
    if row.get("malformed"):
        return errors
    require(required.issubset(row), "command record lacks mandatory fields")
    observation_mode = profile.get("command_input_observation", "")
    observation_schemas = {
        "pre-enter-and-dispatcher-gdb": 4,
        "bounded-pre-enter-and-dispatcher-gdb": 5,
        "stable-pre-enter-and-dispatcher-gdb": 6,
        "release-stable-pre-enter-and-dispatcher-gdb": 7,
        "cursor-feedback-pre-enter-and-dispatcher-gdb": 9,
        "qmp-keyup-feedback-pre-enter-and-dispatcher-gdb": 10,
        "qmp-key-pulse-pre-enter-and-dispatcher-gdb": 11,
        "qmp-key-pulse-scheduled-enter-dispatcher-gdb": 12,
        "qmp-key-pulse-post-resume-enter-dispatcher-gdb": 13,
        "qmp-key-pulse-post-dispatch-release-gdb": 14,
        "qmp-key-pulse-resume-confirmed-dispatch-release-gdb": 15,
        "qmp-key-pulse-runstate-confirmed-private-observer-gdb": 16,
        "qmp-key-pulse-runstate-and-release-confirmed-private-observer-gdb": 17,
        "qmp-atomic-key-stroke-runstate-confirmed-private-observer-gdb": 18,
        "qmp-atomic-key-stroke-bounded-observer-cleanup-gdb": 19,
        "qmp-atomic-key-stroke-bounded-interrupt-detach-observer-gdb": 20,
        "qmp-atomic-key-stroke-bounded-stopped-detach-observer-gdb": 21,
        "qmp-guest-release-confirmed-bounded-stopped-detach-observer-gdb": 22,
        "qmp-guest-press-release-confirmed-bounded-stopped-detach-observer-gdb": 23,
        "qmp-physical-press-release-confirmed-bounded-stopped-detach-observer-gdb": 24,
        "qmp-physical-atomic-stroke-confirmed-bounded-stopped-detach-observer-gdb": 25,
        "qmp-stable-physical-atomic-stroke-confirmed-bounded-stopped-detach-observer-gdb": 26,
    }
    observation_required = observation_mode in observation_schemas
    expected_schema = observation_schemas.get(observation_mode, 2)
    require(row.get("schema") == expected_schema and
            row.get("kind") == "framed-command",
            "command record schema is unsupported for the profile")
    require(row.get("profile_id") == profile.get("name"),
            "command profile identity differs")
    require(row.get("vm_id") == profile.get("vm_id"),
            "command VM identity differs")
    require(row.get("candidate_sha256") == candidate_hash,
            "command refers to another candidate")
    require(isinstance(row.get("qemu_pid"), int) and row.get("qemu_pid") > 0,
            "command lacks a valid QEMU pid")
    sequence = row.get("sequence")
    payload = row.get("payload")
    require(isinstance(sequence, int) and sequence > 0,
            "command sequence is invalid")
    require(isinstance(payload, str) and bool(payload),
            "command payload is invalid")
    if not isinstance(sequence, int) or not isinstance(payload, str):
        return errors
    expected_crc = f"{zlib.crc32(payload.encode()) & 0xffffffff:08x}"
    frame = f"tasktest exec {sequence} {expected_crc} {payload}"
    require(row.get("crc32") == expected_crc, "command checksum is incorrect")
    require(row.get("frame_length") == len(frame),
            "command frame length is incorrect")
    if observation_required and row.get("schema") in (
            4, 5, 6, 7, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18,
            19, 20, 21, 22, 23, 24, 25, 26):
        errors.extend(input_observation_errors(row, profile_dir, frame,
                                               profile))
    require(row.get("transport_status") == "PASS",
            "command input transport did not complete")
    require(row.get("classification") == "PASS",
            "command collector classification is not PASS")
    require(row.get("disposition") == "EXECUTED",
            "replay cannot stand in for a new command sample")
    require(row.get("accept_count") == 1 and row.get("begin_count") == 1 and
            row.get("end_count") == 1,
            "command frame records are incomplete or duplicated")
    require(row.get("replay_count") == 0 and row.get("reject_count") == 0,
            "command transaction contains replay or rejection")
    require(row.get("checksum_verified") is True,
            "collector did not verify the command checksum")
    require(isinstance(row.get("trigger_monotonic"), (int, float)) and
            isinstance(row.get("completed_monotonic"), (int, float)) and
            row.get("completed_monotonic", 0) >= row.get("trigger_monotonic", 1),
            "command host timing is invalid")
    verify_modal_timing(row, errors)

    segment = transaction_segment(transcript, row)
    require(segment is not None, "command line-count interval is invalid")
    if segment is None:
        return errors
    start_count = row.get("start_line_count")
    end_count = row.get("end_line_count")
    expected_start_byte = (0 if start_count == 0 else
                           byte_ends[start_count - 1])
    expected_end_byte = byte_ends[end_count - 1]
    require(row.get("start_byte_count") == expected_start_byte,
            "command start byte offset differs from raw serial")
    require(row.get("end_byte_count") == expected_end_byte,
            "command end byte offset differs from raw serial")
    require(row.get("serial_byte_count_at_trigger", -1) >= expected_start_byte,
            "serial trigger byte count precedes its complete-line boundary")
    require(row.get("serial_byte_count_observed", -1) >= expected_end_byte,
            "serial observed byte count precedes command completion")
    accepts = line_matches(transcript, ACCEPT_RE, sequence)
    begins = line_matches(transcript, BEGIN_RE, sequence)
    endings = line_matches(transcript, END_RE, sequence)
    replays = line_matches(transcript, REPLAY_RE, sequence)
    require(len(accepts) == 1, "raw serial lacks exactly one ACCEPT")
    require(len(begins) == 1, "raw serial lacks exactly one BEGIN")
    require(len(endings) == 1, "raw serial lacks exactly one END")
    require(not replays, "raw serial contains a replay for this sample")
    if len(accepts) == 1:
        accept_line, match = accepts[0]
        require(match.group(2) == expected_crc and
                int(match.group(3)) == len(payload),
                "raw ACCEPT checksum or payload length differs")
        require(row.get("accepted_line") == accept_line,
                "recorded ACCEPT line differs from raw serial")
    if len(begins) == 1:
        require(row.get("begin_line") == begins[0][0],
                "recorded BEGIN line differs from raw serial")
    if len(endings) == 1:
        end_line, match = endings[0]
        require(row.get("end_line") == end_line and
                row.get("end_line_count") == end_line,
                "recorded END boundary differs from raw serial")
        require(row.get("handler_status") == int(match.group(2)),
                "recorded handler status differs from raw END")
    if len(accepts) == len(begins) == len(endings) == 1:
        require(accepts[0][0] < begins[0][0] < endings[0][0],
                "raw frame order is invalid")
        start = row["start_line_count"]
        end = row["end_line_count"]
        require(start < accepts[0][0] <= end and
                start < begins[0][0] <= end and
                start < endings[0][0] <= end,
                "frame record lies outside its transaction")
    return errors


def records_with_prefix(segment, prefix):
    return [(line, fields(line, prefix)) for line in segment
            if fields(line, prefix) is not None]


def require_single_record(segment, prefix, errors):
    observed = records_with_prefix(segment, prefix)
    if len(observed) != 1:
        errors.append(f"{prefix} count is {len(observed)}, expected 1")
        return None
    return observed[0][1]


def verify_irq_check(segment, smp, errors):
    value = require_single_record(segment, "[IRQ][CHECK] PASS", errors)
    if value is None:
        return
    expected = {
        "clocksource": "HPET", "clockevent": "BSP_LAPIC",
        "period_us": "1000", "hpet_timer0": "QUIESCENT",
        "non_bsp_ticks": "0", "stray_hpet": "0", "unexpected": "0",
        "imbalance": "0", "cpus": f"{smp}/{smp}", "reason": "ok",
    }
    if any(value.get(name) != item for name, item in expected.items()):
        errors.append("IRQ check fields violate the current runtime contract")
    if ((int_field(value, "snapshot_polls") or 0) < smp or
            (int_field(value, "validation_polls") or 0) < 1 or
            int_field(value, "elapsed_ms") is None):
        errors.append("IRQ check lacks bounded-sampling evidence")


def exact_slot_set(values, smp, label, errors):
    slots = [int_field(value, "slot") for value in values]
    if len(slots) != smp or any(slot is None for slot in slots):
        errors.append(f"{label} count differs from SMP")
    elif len(set(slots)) != len(slots):
        errors.append(f"{label} contains a duplicate slot")
    elif set(slots) != set(range(smp)):
        errors.append(f"{label} has a missing or out-of-range slot")


def verify_irq_controllers(segment, smp, errors):
    pic = require_single_record(segment, "[IRQ][PIC]", errors)
    hpet = require_single_record(segment, "[IRQ][HPET]", errors)
    if pic is not None and pic.get("quiescent") != "1":
        errors.append("PIC is not quiescent")
    if hpet is not None:
        expected = {"clocksource": "ACTIVE", "timer0": "QUIESCENT",
                    "irq_enabled": "0", "periodic": "0", "fsb": "0",
                    "route_prepared": "0", "route_enabled": "0",
                    "pending": "0", "legacy": "0", "stray": "0"}
        if any(hpet.get(name) != value for name, value in expected.items()):
            errors.append("HPET controller record violates quiescence")
    lapics = [value for _, value in records_with_prefix(segment,
                                                         "[IRQ][LAPIC]")]
    exact_slot_set(lapics, smp, "LAPIC snapshot", errors)


def verify_irq_boot(segment, smp, errors, require_terminal=True):
    boot = require_single_record(segment, "[IRQ][BOOT]", errors)
    clock = require_single_record(segment, "[IRQ][BOOT_CLOCK]", errors)
    require_single_record(segment, "[IRQ][BOOT_PROBES]", errors)
    if boot is not None:
        expected = {"state": "SERVICES_ACTIVE", "cpus_prepared": str(smp),
                    "cpus_verified": str(smp), "runtime_ready": f"{smp}/{smp}",
                    "handoffs": str(smp), "preemption": str(smp), "failed": "0"}
        if any(boot.get(name) != value for name, value in expected.items()):
            errors.append("IRQ boot summary is incomplete or inconsistent")
    if clock is not None:
        expected = {"clocksource": "HPET", "hpet_clocksource_verified": "1",
                    "hpet_timer0_quiescent": "1",
                    "hpet_timer0_irq_enabled": "0",
                    "hpet_timer0_route_enabled": "0",
                    "clockevent": "BSP_LAPIC", "clockevent_period_us": "1000",
                    "clockevent_non_bsp": "0", "hpet_stray_irqs": "0"}
        if any(clock.get(name) != value for name, value in expected.items()):
            errors.append("IRQ boot clock fields violate the runtime contract")
    cpu_values = [value for _, value in records_with_prefix(
        segment, "[IRQ][BOOT_CPU]")]
    exact_slot_set(cpu_values, smp, "BOOT_CPU snapshot", errors)
    for value in cpu_values:
        if (value.get("entered") != value.get("returned") or
                value.get("depth") != "0" or value.get("unexpected") != "0" or
                value.get("handoff") != "1" or value.get("preempt") != "1" or
                value.get("ready") != "1"):
            errors.append("BOOT_CPU snapshot is structurally inconsistent")
            break
    if require_terminal:
        terminal = require_single_record(segment, "[IRQ][BOOT_RESULT] PASS",
                                         errors)
        if terminal is not None and terminal.get("snapshots") != str(smp):
            errors.append("IRQ boot terminal snapshot count differs from SMP")


def verify_lapic_config(segment, smp, handler_status, errors):
    cpu_values = [value for _, value in records_with_prefix(
        segment, "[ACCOUNT][LAPIC_CONFIG_CPU]")]
    exact_slot_set(cpu_values, smp, "LAPIC configuration snapshot", errors)
    for value in cpu_values:
        if (value.get("failed_mask") != "0" or
                value.get("vector") != "34" or
                value.get("divisor") != "16" or
                (int_field(value, "initial") or 0) <= 0 or
                value.get("periodic") != "1" or value.get("masked") != "0"):
            errors.append("LAPIC configuration snapshot is structurally invalid")
            break
    terminals = []
    for line in segment:
        match = re.fullmatch(
            r"\[ACCOUNT\]\[LAPIC_CONFIG\] (PASS|FAIL) cpus=([0-9]+) "
            r"spread_x10=([0-9]+)", line)
        if match:
            terminals.append(match)
    if len(terminals) != 1:
        errors.append("LAPIC configuration lacks one terminal result")
        return
    result, cpus, spread = terminals[0].groups()
    if int(cpus) != smp:
        errors.append("LAPIC configuration terminal CPU count differs from SMP")
    expected_status = 0 if result == "PASS" else 1
    if handler_status != expected_status:
        errors.append("LAPIC configuration result and handler status disagree")
    if result != "PASS" or int(spread) > 200:
        errors.append(f"LAPIC configuration raw result failed spread_x10={spread}")


def verify_rate_transaction(segment, smp, handler_status, errors):
    summaries = []
    for line in segment:
        match = re.fullmatch(
            r"\[ACCOUNT\]\[LAPIC_RATE\] (PASS|FAIL) window_ms=([0-9]+) "
            r"rounds=([0-9]+) worst_median_x1000=([0-9]+) "
            r"min_round_x1000=([0-9]+) max_round_x1000=([0-9]+)", line)
        if match:
            summaries.append(match)
    if len(summaries) != 1:
        errors.append("LAPIC rate has no unique terminal result in its transaction")
        return None
    match = summaries[0]
    raw = match.group(1)
    if match.group(2) != "2000" or match.group(3) != "5":
        errors.append("LAPIC rate parameters differ from the required test")
    if (raw == "PASS" and handler_status != 0) or (
            raw == "FAIL" and handler_status != 1):
        errors.append("LAPIC rate raw result disagrees with handler status")
    samples = []
    for line in segment:
        hit = re.fullmatch(
            r"\[ACCOUNT\]\[LAPIC_RATE_CPU\] round=([1-5]) slot=([0-9]+) "
            r"delta=([0-9]+) expected=([0-9]+) ratio_x1000=([0-9]+)", line)
        if hit:
            samples.append((int(hit.group(1)), int(hit.group(2))))
    expected = {(round_id, slot) for round_id in range(1, 6)
                for slot in range(smp)}
    if len(samples) != len(expected) or set(samples) != expected:
        errors.append("LAPIC rate lacks exact per-round, per-CPU coverage")
    medians = records_with_prefix(segment, "[ACCOUNT][LAPIC_RATE_MEDIANS]")
    if len(medians) != 1 or set(medians[0][1]) != {
            f"cpu{slot}" for slot in range(smp)}:
        errors.append("LAPIC rate median coverage differs from SMP")
    return raw


def generic_result(segment, prefix, errors):
    require_single_record(segment, prefix, errors)


def verify_timer_reference(segment, competing, errors):
    value = require_single_record(segment, "[REAPTEST][TIMER_REF] PASS",
                                  errors)
    if value is None:
        return
    expected = {
        "mode": "competing" if competing else "target",
        "cleanup": "REAPED", "gate": "1", "hold_armed": "1",
        "hold_observed": "1", "sleeping": "1", "claimed": "1",
        "wake_claimed": "1", "killed": "1", "zombie": "1",
        "cancelled": "1", "ref": "1", "target_mask": "32768",
        "blocked": "1", "deferred": "1", "release_command": "1",
        "released": "1", "acquire_release": "1", "nodes_pending": "0",
        "nodes_claimed": "0", "node_refs": "0", "wrong_lifecycle": "0",
        "reaped": "1", "cleanup_reaped": "1", "residual_holds": "0",
        "competitor": "1" if competing else "0",
    }
    if any(value.get(name) != item for name, item in expected.items()):
        errors.append("timer-reference target proof is incomplete")
    for name in ("target_id", "lifecycle", "wait_generation",
                 "global_ref_release_delta"):
        if (int_field(value, name) or 0) < 1:
            errors.append(f"timer-reference field {name} is not positive")
    if competing:
        competitor = {
            "competitor_gone": "1", "competitor_target_present": "1",
            "legacy_blocked": "0",
        }
        if (any(value.get(name) != item for name, item in competitor.items()) or
                (int_field(value, "competitor_reaped") or 0) < 1):
            errors.append("competing zombie did not exercise the global-reap ambiguity")


def evidence_errors(row, segment, smp, require_irq_terminal=True):
    errors = []
    payload = row["payload"]
    status = row.get("handler_status")
    if payload == "irq check":
        verify_irq_check(segment, smp, errors)
    elif payload == "irq controllers":
        verify_irq_controllers(segment, smp, errors)
    elif payload == "irq routes":
        generic_result(segment, "[IRQ][ROUTES] PASS", errors)
    elif payload == "irq boot":
        verify_irq_boot(segment, smp, errors, require_irq_terminal)
    elif payload == "accounttest lapic-config":
        verify_lapic_config(segment, smp, status, errors)
    elif payload == "accounttest lapic-liveness 500":
        generic_result(segment, "[ACCOUNT][LAPIC_LIVENESS] PASS", errors)
    elif payload == "accounttest check":
        generic_result(segment, "[ACCOUNT][CHECK] PASS", errors)
    elif payload == "accounttest lapic-rate 2000 5":
        verify_rate_transaction(segment, smp, status, errors)
    elif payload == "synctest sleep":
        if len([line for line in segment if re.fullmatch(
                r"\[SYNC\]\[SLEEP\] START run=[1-9][0-9]* .*", line)]) != 1:
            errors.append("SLEEP launch lacks a unique run ID")
    elif payload == "synctest timer-cancel":
        if len([line for line in segment if re.fullmatch(
                r"\[SYNC\]\[TIMER_CANCEL\] START run=[1-9][0-9]* .*", line)]) != 1:
            errors.append("TIMER_CANCEL launch lacks a unique run ID")
    elif payload.startswith("synctest async-wait "):
        parts = payload.split()
        if len(parts) != 4 or not parts[2].isdigit():
            errors.append("async-wait payload is malformed")
        else:
            run = parts[2]
            matches = [line for line in segment if re.fullmatch(
                rf"\[SYNC\]\[ASYNC_WAIT\] PASS run={run} "
                r"kind=(SLEEP|TIMER_CANCEL) state=PASS .*", line)]
            if len(matches) != 1:
                errors.append("async-wait result has a wrong or missing run ID")
    elif payload == "synctest timer-order":
        generic_result(segment, "[SYNC][TIMER_ORDER] PASS", errors)
    elif payload == "synctest timer-backlog":
        generic_result(segment, "[SYNC][TIMER_BACKLOG] PASS", errors)
    elif payload == "reaptest timer-ref":
        verify_timer_reference(segment, False, errors)
    elif payload == "reaptest timer-ref-competing":
        verify_timer_reference(segment, True, errors)
    elif payload == "synctest check":
        generic_result(segment, "[SYNC][CHECK] PASS", errors)
    elif payload == "taskdiag check":
        generic_result(segment, "[TASKDIAG][CHECK] PASS", errors)
    elif payload == "inputtest check":
        generic_result(segment, "[INPUTTEST][CHECK] PASS", errors)
    elif payload == "modaltest check":
        generic_result(segment, "[MODALTEST][CHECK] PASS", errors)
    elif payload == "taskmantest check":
        generic_result(segment, "[TASKMANTEST][CHECK] PASS", errors)
    elif payload == "clear":
        pass
    elif payload == "taskman 1000":
        if segment.count("[TASKMAN][OWNER_STATE] shell=BLOCKED session=ACTIVE") != 1:
            errors.append("TASKMAN owner state is missing or duplicated")
        if segment.count("[MODAL] session_end OK") != 1:
            errors.append("TASKMAN modal completion is missing or duplicated")
    elif payload == "taskmantest stats":
        stats = require_single_record(segment, "[TASKMANTEST][STATS]", errors)
        if stats is not None:
            zero = (
                "stable_frame_full_clears", "stale_cells", "workspace_live",
                "last_session_fallback_frames", "render_failures", "clipped",
                "builder_truncations", "summary_failures", "footer_failures",
                "region_clear_failures", "separator_mismatches",
                "field_overflows", "field_truncations", "split_overlaps",
                "shell_exit_scroll_delta", "last_session_shell_exit_scroll_delta",
                "last_session_scroll_delta", "auto_exit_pending")
            if any(stats.get(name) != "0" for name in zero) or (
                    int_field(stats, "last_session_full_frames") or 0) < 1:
                errors.append("TASKMAN stats contain visual or workspace residue")
    else:
        errors.append(f"unrecognized command payload: {payload}")
    if payload not in ("accounttest lapic-rate 2000 5",
                       "accounttest lapic-config") and status != 0:
        errors.append("handler returned nonzero despite favorable diagnostics")
    return errors


def boot_errors(profile, transcript):
    errors = []
    try:
        smp = int(profile.get("smp", "0"))
    except ValueError:
        return ["profile SMP is invalid"]
    for prefix in ("[IRQ][CPU_TIMER_VERIFIED] PASS",
                   "[SCHED][BOOTSTRAP_HANDOFF] PASS",
                   "[IRQ][CPU_READY] PASS"):
        values = [value for _, _, value in exact_records(transcript, prefix)]
        exact_slot_set(values, smp, prefix, errors)
    ready = require_single_record(transcript, "[IRQ][CPU_READY_SUMMARY]", errors)
    if ready is not None:
        expected = {"expected": str(smp), "ready": str(smp), "failed": "0",
                    "waiting": "0"}
        if any(ready.get(name) != value for name, value in expected.items()):
            errors.append("CPU readiness summary is incomplete")
    runtime = require_single_record(transcript, "[BOOT][RUNTIME_READY] PASS",
                                    errors)
    if runtime is not None and runtime.get("cpus") != f"{smp}/{smp}":
        errors.append("runtime readiness does not cover the profile")
    timer0 = require_single_record(transcript, "[CLOCK][HPET_TIMER0] QUIESCENT",
                                   errors)
    if timer0 is not None:
        expected = {"irq_enabled": "0", "route_enabled": "0", "pending": "0",
                    "legacy": "0"}
        if any(timer0.get(name) != value for name, value in expected.items()):
            errors.append("HPET Timer0 boot record is not quiescent")
    clockevent = require_single_record(transcript, "[CLOCKEVENT][RUNTIME] ACTIVE",
                                      errors)
    if clockevent is not None and (clockevent.get("source") != "BSP_LAPIC" or
                                   clockevent.get("period_us") != "1000"):
        errors.append("runtime clockevent is not BSP LAPIC at 1000 us")
    fault = re.compile(r"PANIC|FATAL|DOUBLE FAULT|TRIPLE FAULT|STRUCTURAL_FAULT|"
                       r"FINISH_FAULT|CPU_READY_TIMEOUT|\[SMP\]\[CPU\] ERROR")
    if any(fault.search(line) for line in transcript):
        errors.append("guest fault or CPU readiness failure is present")
    return errors


def validate_profile_identity(profile_dir, profile, launch, candidate_hash,
                              focal=False):
    errors = []
    name = profile_dir.name
    expected = FOCAL_PROFILES.get(name) if focal else EXPECTED_PROFILES.get(name)
    if expected is None:
        return ["profile directory name is not in the required matrix"]
    machine, accel, smp = expected[:3]
    if (profile.get("name") != name or profile.get("machine") != machine or
            profile.get("accel") != accel or profile.get("smp") != str(smp)):
        errors.append("profile metadata differs from the required launch")
    if not focal and profile.get("group") != expected[3]:
        errors.append("profile group differs from the required matrix")
    if focal and profile.get("group") != "focal":
        errors.append("focal profile group is incorrect")
    if profile.get("kernel_sha256") != candidate_hash:
        errors.append("profile refers to another kernel binary")
    launch_kernel = launch.get("KERNEL_SHA256", "").replace("\\", "")
    if launch_kernel != candidate_hash:
        errors.append("QEMU launch kernel hash differs from candidate")
    for key, value in (("MACHINE", machine), ("ACCEL", accel),
                       ("SMP", str(smp))):
        if launch.get(key, "").replace("\\", "") != value:
            errors.append(f"QEMU launch {key} differs from profile")
    image = launch.get("IMAGE_SHA256", "").replace("\\", "")
    if image != profile.get("image_sha256_before"):
        errors.append("QEMU launch image hash differs from recorded input")
    if profile.get("cleanup") != "PASS" or profile.get("run_complete") != "YES":
        errors.append("QEMU cleanup or run completion is not proved")
    try:
        host_cpus = int(profile.get("host_schedulable_cpus", "0"))
    except ValueError:
        host_cpus = 0
    expected_authority = (
        "NOT_APPLICABLE" if accel != "kvm" else
        "NONAUTHORITATIVE_OVERSUBSCRIBED" if host_cpus < smp else
        "AUTHORITATIVE_CONTROL" if smp == 4 else "AUTHORITATIVE")
    if profile.get("rate_authority") != expected_authority:
        errors.append("rate authority is inconsistent with host capacity")
    return errors


def command_map(commands):
    mapping = {}
    for row in commands:
        mapping.setdefault(row.get("payload"), []).append(row)
    return mapping


def async_pair_errors(commands, transcript):
    errors = []
    for kind, payload in (("SLEEP", "synctest sleep"),
                          ("TIMER_CANCEL", "synctest timer-cancel")):
        launches = [row for row in commands if row.get("payload") == payload]
        if len(launches) != 1:
            errors.append(f"{kind} has no unique launch transaction")
            continue
        segment = transaction_segment(transcript, launches[0]) or []
        matches = []
        for line in segment:
            hit = re.fullmatch(
                rf"\[SYNC\]\[{kind}\] START run=([1-9][0-9]*) .*", line)
            if hit:
                matches.append(hit.group(1))
        if len(matches) != 1:
            continue
        run = matches[0]
        waits = [row for row in commands if isinstance(row.get("payload"), str)
                 and row["payload"].startswith(f"synctest async-wait {run} ")]
        if len(waits) != 1:
            errors.append(f"{kind} async wait does not use the launch run ID")
            continue
        wait_segment = transaction_segment(transcript, waits[0]) or []
        expected = re.compile(
            rf"^\[SYNC\]\[ASYNC_WAIT\] PASS run={run} kind={kind} "
            r"state=PASS .*$")
        if len([line for line in wait_segment if expected.fullmatch(line)]) != 1:
            errors.append(f"{kind} async completion is outside its wait transaction")
    return errors


def verify_profile(root, profile_dir, candidate_hash, focal=False):
    errors = []
    paths = (profile_dir / "profile.env", profile_dir / "qemu" /
             "qemu-serial.log", profile_dir / "commands.jsonl",
             profile_dir / "qemu" / "launch.env")
    if not all(path.is_file() for path in paths):
        return ["profile metadata, serial, ledger, or launch.env is missing"]
    profile = read_env(paths[0])
    serial_bytes = paths[1].read_bytes()
    transcript = []
    byte_ends = []
    offset = 0
    for item in serial_bytes.splitlines(keepends=True):
        offset += len(item)
        if not item.endswith((b"\n", b"\r")):
            break
        transcript.append(item.rstrip(b"\r\n").decode(errors="replace"))
        byte_ends.append(offset)
    commands = read_commands(paths[2])
    launch = read_env(paths[3])
    errors.extend(validate_profile_identity(profile_dir, profile, launch,
                                            candidate_hash, focal))
    errors.extend(boot_errors(profile, transcript))
    sequences = [row.get("sequence") for row in commands]
    if sequences != list(range(1, len(commands) + 1)):
        errors.append("command sequences are missing, duplicated, or reordered")
    for row in commands:
        errors.extend(validate_transaction(row, transcript, byte_ends, profile,
                                           candidate_hash, profile_dir))
        segment = transaction_segment(transcript, row)
        if segment is not None:
            require_terminal = not focal
            errors.extend(evidence_errors(row, segment,
                                          int(profile.get("smp", "0")),
                                          require_terminal))
    mapping = command_map(commands)
    if not focal:
        required = (dict(FULL_COMMAND_COUNTS) if profile.get("group") in
                    ("full", "soak") else
                    {payload: 1 for payload in BASIC_COMMANDS})
        if profile.get("group") == "soak":
            for payload in ("accounttest lapic-liveness 500", "irq check",
                            "synctest check"):
                required[payload] += 5
        for payload, expected_count in required.items():
            if len(mapping.get(payload, [])) != expected_count:
                errors.append(
                    f"required command count for {payload} is not "
                    f"{expected_count}")
        if profile.get("group") in ("full", "soak"):
            errors.extend(async_pair_errors(commands, transcript))
            screenshot = profile_dir / "taskman-screendump.ppm"
            modal_rows = mapping.get("taskman 1000", [])
            if len(modal_rows) == 1:
                verify_modal_artifact(modal_rows[0], screenshot, errors)
    return errors


def derive_rate(profile_dir):
    profile = read_env(profile_dir / "profile.env")
    transcript = (profile_dir / "qemu" / "qemu-serial.log").read_text(
        errors="replace").splitlines()
    commands = read_commands(profile_dir / "commands.jsonl")
    rates = [row for row in commands
             if row.get("payload") == "accounttest lapic-rate 2000 5"]
    if len(rates) != 1:
        return None, profile, ["rate transaction count differs from one"]
    errors = []
    raw = verify_rate_transaction(
        transaction_segment(transcript, rates[0]) or [],
        int(profile.get("smp", "0")), rates[0].get("handler_status"), errors)
    return raw, profile, errors


def verify_rate(root):
    errors = []
    summary_path = root / "rate-summary.tsv"
    if not summary_path.is_file():
        return ["rate-summary.tsv is missing"]
    rows = {}
    for line in summary_path.read_text().splitlines()[1:]:
        item = line.split("\t")
        if len(item) == 5:
            rows[item[0]] = item[1:]
    derived = {}
    for name in ("q35-kvm-smp4", "q35-kvm-smp24"):
        raw, profile, rate_errors = derive_rate(root / "profiles" / name)
        errors.extend(f"{name}: {error}" for error in rate_errors)
        try:
            host = int(profile.get("host_schedulable_cpus", "0"))
            guest = int(profile.get("smp", "0"))
        except ValueError:
            host = guest = 0
            errors.append(f"{name}: CPU capacity metadata is invalid")
        authority = ("NONAUTHORITATIVE_OVERSUBSCRIBED" if host < guest else
                     ("AUTHORITATIVE_CONTROL" if guest == 4 else
                      "AUTHORITATIVE"))
        derived[name] = [raw, authority, str(host), str(guest)]
        if profile.get("rate_authority") != authority:
            errors.append(f"{name}: collector rate authority is incorrect")
        if name == "q35-kvm-smp4" and (host < guest or raw != "PASS"):
            errors.append("non-oversubscribed SMP4 rate control is not valid")
        if host >= guest and raw != "PASS":
            errors.append(f"{name}: authoritative rate result failed")
        if raw not in ("PASS", "FAIL"):
            errors.append(f"{name}: raw rate result is incomplete")
    for name, expected in derived.items():
        if rows.get(name) != expected:
            errors.append(f"{name}: rate summary differs from raw transaction")
    return errors


def verify_soak(profile_dir):
    errors = []
    path = profile_dir / "soak-checkpoints.tsv"
    if not path.is_file():
        return ["soak checkpoints are missing"]
    rows = [line.split("\t") for line in path.read_text().splitlines()[1:] if line]
    if len(rows) != 5:
        errors.append("soak does not contain exactly five checkpoints")
        return errors
    commands = read_commands(profile_dir / "commands.jsonl")
    by_sequence = {row.get("sequence"): row for row in commands}
    elapsed_values = []
    for expected_checkpoint, row in enumerate(rows, 1):
        if len(row) != 6:
            errors.append("soak checkpoint row is malformed")
            continue
        checkpoint, _, elapsed, first, last, status = row
        try:
            first, last, elapsed = int(first), int(last), int(elapsed)
        except ValueError:
            errors.append("soak checkpoint numbers are invalid")
            continue
        elapsed_values.append(elapsed)
        if checkpoint != str(expected_checkpoint) or status != "PASS":
            errors.append("soak checkpoint identity or status is invalid")
        expected_payloads = ["accounttest lapic-liveness 500", "irq check",
                             "synctest check"]
        observed = [by_sequence.get(sequence, {}).get("payload")
                    for sequence in range(first, last + 1)]
        if observed != expected_payloads:
            errors.append("soak checkpoint is not backed by three causal commands")
    profile = read_env(profile_dir / "profile.env")
    try:
        total = int(profile.get("soak_elapsed_seconds", "0"))
    except ValueError:
        total = 0
    if total < 300 or not elapsed_values or elapsed_values[-1] < 300:
        errors.append("soak host duration is shorter than 300 seconds")
    return errors


def write_focal_summary(root, results):
    output = {"schema": 1, "profiles": results}
    (root / "focal-summary.json").write_text(
        json.dumps(output, indent=2, sort_keys=True) + "\n")


def complete_serial(path):
    lines, byte_ends, offset = [], [], 0
    for item in path.read_bytes().splitlines(keepends=True):
        offset += len(item)
        if not item.endswith((b"\n", b"\r")):
            break
        lines.append(item.rstrip(b"\r\n").decode(errors="replace"))
        byte_ends.append(offset)
    return lines, byte_ends


def validate_production_transaction(row, profile_dir, transcript, byte_ends,
                                    profile, candidate_hash):
    errors = []

    def require(condition, message):
        if not condition:
            errors.append(message)

    required = {
        "schema", "kind", "profile_id", "vm_id", "candidate_sha256",
        "qemu_pid", "sequence", "payload", "payload_crc32",
        "deadline_seconds", "start_line_count", "end_line_count",
        "start_byte_count", "end_byte_count", "serial_byte_count_at_trigger",
        "serial_byte_count_observed", "observer_status", "handler_status",
        "classification", "gdb_log", "gdb_log_sha256",
        "gdb_command", "gdb_command_sha256", "handler_status_response",
        "observer_connected_monotonic", "observer_armed_monotonic",
        "trigger_monotonic", "breakpoint_monotonic",
        "handler_return_monotonic", "completed_monotonic",
        "breakpoint_stop", "payload_memory_response", "observed_payload",
        "entry_breakpoint_response", "entry_breakpoint_number",
        "entry_breakpoint_delete_response",
        "return_address_response", "return_address",
        "return_breakpoint_response", "return_breakpoint_number",
        "return_stop", "return_pc_response", "gdb_return_code",
    }
    require(required.issubset(row), "production command lacks mandatory fields")
    require(row.get("schema") == 3 and row.get("kind") == "observed-command",
            "production command schema is unsupported")
    require(row.get("profile_id") == profile.get("name") and
            row.get("vm_id") == profile.get("vm_id"),
            "production command profile or VM identity differs")
    require(row.get("candidate_sha256") == candidate_hash,
            "production command refers to another candidate")
    payload = row.get("payload")
    if not isinstance(payload, str):
        return errors + ["production command payload is invalid"]
    require(row.get("payload_crc32") ==
            f"{zlib.crc32(payload.encode()) & 0xffffffff:08x}",
            "production command checksum is incorrect")
    require(row.get("observer_status") == "COMPLETED" and
            row.get("classification") == "PASS" and
            row.get("gdb_return_code") == 0,
            "production GDB observation did not complete")
    verify_modal_timing(row, errors, production=True)
    try:
        connected = float(row["observer_connected_monotonic"])
        armed = float(row["observer_armed_monotonic"])
        trigger = float(row["trigger_monotonic"])
        breakpoint_time = float(row["breakpoint_monotonic"])
        return_time = float(row["handler_return_monotonic"])
        completed = float(row["completed_monotonic"])
        require(connected <= armed <= trigger <= breakpoint_time <=
                return_time <= completed,
                "production observer timing is not causal")
        start, end = int(row["start_line_count"]), int(row["end_line_count"])
        require(0 <= start <= end <= len(transcript) and
                (start < end or payload == "clear"),
                "production command line interval is invalid")
    except (KeyError, TypeError, ValueError):
        return errors + ["production timing or line interval is malformed"]
    if not (0 <= start <= end <= len(transcript)) or (
            start == end and payload != "clear"):
        return errors
    expected_start = 0 if start == 0 else byte_ends[start - 1]
    expected_end = 0 if end == 0 else byte_ends[end - 1]
    require(row.get("start_byte_count") == expected_start and
            row.get("end_byte_count") == expected_end,
            "production byte offsets differ from raw serial")
    require(row.get("serial_byte_count_at_trigger", -1) >= expected_start and
            row.get("serial_byte_count_observed", -1) >= expected_end,
            "production serial byte counts do not cover the transaction")
    segment = transcript[start:end]
    require(not any(line.startswith("[HARNESS]") for line in segment),
            "production command improperly depends on framed transport")
    gdb_name = row.get("gdb_log")
    require(isinstance(gdb_name, str) and Path(gdb_name).name == gdb_name,
            "production GDB log path is unsafe")
    gdb_path = profile_dir / str(gdb_name)
    if gdb_path.is_file():
        text = gdb_path.read_text(errors="replace")
        breakpoint_stop = row.get("breakpoint_stop", "")
        entry_response = row.get("entry_breakpoint_response", "")
        entry_match = re.search(r'number="([1-9][0-9]*)"', entry_response)
        entry_number = row.get("entry_breakpoint_number")
        require(isinstance(entry_response, str) and
                text.splitlines().count(entry_response) == 1 and
                entry_match is not None and
                int(entry_match.group(1)) == entry_number,
                "production dispatcher entry breakpoint is not in raw GDB")
        require(isinstance(breakpoint_stop, str) and
                'reason="breakpoint-hit"' in breakpoint_stop and
                'func="shell_dispatch_command_line"' in breakpoint_stop and
                f'bkptno="{entry_number}"' in breakpoint_stop and
                text.splitlines().count(breakpoint_stop) == 1,
                "production dispatcher breakpoint is not in raw GDB")
        payload_response = row.get("payload_memory_response", "")
        contents = re.search(r'contents="([0-9a-fA-F]+)"',
                             payload_response)
        observed_payload = None
        if contents is not None:
            try:
                observed_payload = bytes.fromhex(contents.group(1)).split(
                    b"\0", 1)[0].decode("ascii")
            except (ValueError, UnicodeDecodeError):
                observed_payload = None
        require(isinstance(payload_response, str) and
                text.splitlines().count(payload_response) == 1 and
                observed_payload == payload == row.get("observed_payload"),
                "production GDB observed another shell payload")
        delete_response = row.get("entry_breakpoint_delete_response", "")
        require(isinstance(delete_response, str) and
                re.fullmatch(r'[1-9][0-9]*\^done', delete_response) is not None and
                text.splitlines().count(delete_response) == 1,
                "production dispatcher entry breakpoint was not removed")
        address_response = row.get("return_address_response", "")
        address_match = re.fullmatch(
            r'[1-9][0-9]*\^done,value="(0x[0-9a-fA-F]+|[0-9]+)"',
            address_response)
        recorded_address = row.get("return_address")
        require(address_match is not None and
                text.splitlines().count(address_response) == 1 and
                int(address_match.group(1), 0) == recorded_address,
                "production dispatcher return address is not in raw GDB")
        return_breakpoint = row.get("return_breakpoint_response", "")
        breakpoint_match = re.search(r'number="([1-9][0-9]*)"',
                                     return_breakpoint)
        breakpoint_number = row.get("return_breakpoint_number")
        require(isinstance(return_breakpoint, str) and
                text.splitlines().count(return_breakpoint) == 1 and
                breakpoint_match is not None and
                int(breakpoint_match.group(1)) == breakpoint_number and
                recorded_address is not None and
                f'original-location="*0x{recorded_address:x}"' in
                return_breakpoint,
                "production return breakpoint is not in raw GDB")
        return_stop = row.get("return_stop", "")
        require(isinstance(return_stop, str) and
                text.splitlines().count(return_stop) == 1 and
                'reason="breakpoint-hit"' in return_stop and
                f'bkptno="{breakpoint_number}"' in return_stop,
                "production handler return stop is not in raw GDB")
        pc_response = row.get("return_pc_response", "")
        pc_match = re.fullmatch(
            r'[1-9][0-9]*\^done,value="(0x[0-9a-fA-F]+|[0-9]+)"',
            pc_response)
        require(pc_match is not None and
                text.splitlines().count(pc_response) == 1 and
                int(pc_match.group(1), 0) == recorded_address,
                "production handler return PC differs from its caller")
        response = row.get("handler_status_response", "")
        match = re.fullmatch(
            r'[1-9][0-9]*\^done,value="(-?(?:0x[0-9a-fA-F]+|[0-9]+))"',
            response)
        require(match is not None and text.splitlines().count(response) == 1 and
                int(match.group(1), 0) == row.get("handler_status"),
                "production handler status differs from raw GDB")
        require(sha256(gdb_path) == row.get("gdb_log_sha256"),
                "production GDB log hash differs")
    else:
        errors.append("production GDB log is missing")
    armed_name = row.get("observer_armed_file")
    require(isinstance(armed_name, str) and Path(armed_name).name == armed_name
            and (profile_dir / "qemu" / str(armed_name)).is_file(),
            "production observer arm receipt is missing")
    command_name = row.get("gdb_command")
    command_path = profile_dir / str(command_name)
    if (not isinstance(command_name, str) or
            Path(command_name).name != command_name or
            not command_path.is_file() or
            sha256(command_path) != row.get("gdb_command_sha256")):
        errors.append("production GDB command transcript is missing or changed")
    errors.extend(evidence_errors(row, segment, int(profile.get("smp", "0")),
                                  True))
    return errors


def verify_production(root):
    errors = []
    candidate = root / "candidate" / "kernel.elf"
    image = root / "candidate" / "hobbyos.img"
    if not candidate.is_file() or not image.is_file():
        return ["production candidate ELF or image is missing"]
    candidate_hash = sha256(candidate)
    symbols = root / "candidate" / "symbols.txt"
    policy = root / "candidate" / "production-test-policy.log"
    if not symbols.is_file() or not policy.is_file():
        errors.append("production symbol or policy evidence is missing")
    else:
        symbol_text = symbols.read_text(errors="replace")
        forbidden = ("g_panic_test_state", "panic_test_",
                     "shell_execute_command_line_for_selftest",
                     "tasktest_transport_crc32", "timer_ref_no_release_negative")
        if any(item in symbol_text for item in forbidden):
            errors.append("production candidate contains a test-only symbol")
        if "[BOOT][PRODUCTION_TEST_POLICY] PASS" not in policy.read_text(
                errors="replace"):
            errors.append("production policy did not pass")
    profiles_root = root / "profiles"
    observed = {path.name for path in profiles_root.glob("*") if path.is_dir()}
    if observed != set(PRODUCTION_PROFILES):
        errors.append("production profile set is incomplete or has extras")
    rates = {}
    for name, (machine, accel, smp, group, required) in \
            PRODUCTION_PROFILES.items():
        profile_dir = profiles_root / name
        env_path = profile_dir / "profile.env"
        serial_path = profile_dir / "qemu" / "qemu-serial.log"
        ledger_path = profile_dir / "commands.jsonl"
        launch_path = profile_dir / "qemu" / "launch.env"
        argv_path = profile_dir / "qemu" / "qemu-argv.txt"
        if not all(path.is_file() for path in
                   (env_path, serial_path, ledger_path, launch_path, argv_path)):
            errors.append(f"{name}: production evidence files are incomplete")
            continue
        profile = read_env(env_path)
        launch = read_env(launch_path)
        expected_env = {"name": name, "machine": machine, "accel": accel,
                        "smp": str(smp), "group": group,
                        "candidate_kind": "production",
                        "kernel_sha256": candidate_hash, "cleanup": "PASS",
                        "run_complete": "YES"}
        if any(profile.get(key) != value for key, value in expected_env.items()):
            errors.append(f"{name}: production profile identity differs")
        if (launch.get("MACHINE") != machine or launch.get("ACCEL") != accel or
                launch.get("SMP") != str(smp) or
                launch.get("KERNEL_SHA256") != candidate_hash or
                launch.get("IMAGE_SHA256") != profile.get("image_sha256_before")):
            errors.append(f"{name}: production launch identity differs")
        try:
            argv = shlex.split(argv_path.read_text(errors="strict"))
        except (UnicodeDecodeError, ValueError):
            argv = []
        gdb_values = [argv[index + 1] for index, value in enumerate(argv[:-1])
                      if value == "-gdb"]
        chardev_values = [argv[index + 1]
                          for index, value in enumerate(argv[:-1])
                          if value == "-chardev"]
        local_gdb = False
        if (gdb_values == ["chardev:foundation_gdb"] and
                len(chardev_values) == 1):
            fields = chardev_values[0].split(",")
            options = set(fields[1:]) if fields and fields[0] == "socket" else set()
            paths = [item[5:] for item in options if item.startswith("path=")]
            if len(paths) == 1:
                socket_name = Path(paths[0])
                local_gdb = (
                    socket_name.is_absolute() and
                    socket_name.name == "gdb.sock" and
                    socket_name.parent.name == profile.get("vm_id") and
                    socket_name.parent.parent == Path("/tmp") and
                    {"server=on", "wait=off", "id=foundation_gdb"}.issubset(
                        options) and
                    not any(item.startswith(("host=", "port="))
                            for item in options))
        if not local_gdb:
            errors.append(f"{name}: local GDB observer launch is not proven")
        transcript, byte_ends = complete_serial(serial_path)
        rows = read_commands(ledger_path)
        if [row.get("sequence") for row in rows] != list(
                range(1, len(rows) + 1)):
            errors.append(f"{name}: production command sequence is invalid")
        payloads = [row.get("payload") for row in rows]
        if payloads != list(required):
            errors.append(f"{name}: production command coverage or order differs")
        previous_end = 0
        for row in rows:
            if (isinstance(row.get("start_line_count"), int) and
                    row["start_line_count"] < previous_end):
                errors.append(f"{name}: production transactions overlap")
            if isinstance(row.get("end_line_count"), int):
                previous_end = row["end_line_count"]
            errors.extend(f"{name}: {error}" for error in
                          validate_production_transaction(
                              row, profile_dir, transcript, byte_ends,
                              profile, candidate_hash))
            if row.get("payload") == "accounttest lapic-rate 2000 5":
                rates[name] = (row, transcript[
                    row.get("start_line_count", 0):row.get("end_line_count", 0)],
                    profile)
        if name == "q35-kvm-smp4":
            screenshot = profile_dir / "taskman-screendump.ppm"
            taskman_rows = [row for row in rows
                            if row.get("payload") == "taskman 1000"]
            if len(taskman_rows) == 1:
                verify_modal_artifact(taskman_rows[0], screenshot, errors)
    for name in ("q35-kvm-smp4", "q35-kvm-smp24"):
        if name not in rates:
            errors.append(f"{name}: production rate transaction is absent")
            continue
        row, segment, profile = rates[name]
        rate_errors = []
        raw = verify_rate_transaction(segment, int(profile["smp"]),
                                      row.get("handler_status"), rate_errors)
        errors.extend(f"{name}: {error}" for error in rate_errors)
        host = int(profile.get("host_schedulable_cpus", "0"))
        guest = int(profile["smp"])
        authority = ("NONAUTHORITATIVE_OVERSUBSCRIBED" if host < guest else
                     ("AUTHORITATIVE_CONTROL" if guest == 4 else
                      "AUTHORITATIVE"))
        if profile.get("rate_authority") != authority:
            errors.append(f"{name}: production rate authority differs")
        if (authority != "NONAUTHORITATIVE_OVERSUBSCRIBED" and raw != "PASS"):
            errors.append(f"{name}: authoritative production rate failed")
    if errors:
        for error in errors:
            print("production-error: " + error)
        return errors
    for name in sorted(PRODUCTION_PROFILES):
        print(f"production-profile={name} status=PASS")
    return []


def verify_diagnostic_negative(root):
    errors = []
    candidate = root / "candidate" / "kernel.elf"
    profile_dir = root / "profiles" / "timer-ref-no-release-kvm-smp4"
    env_path = profile_dir / "profile.env"
    serial_path = profile_dir / "qemu" / "qemu-serial.log"
    ledger_path = profile_dir / "commands.jsonl"
    launch_path = profile_dir / "qemu" / "launch.env"
    symbols_path = root / "candidate" / "symbols.txt"
    required_paths = (candidate, root / "candidate" / "hobbyos.img",
                      env_path, serial_path, ledger_path, launch_path,
                      symbols_path, root / "candidate" / "build-profile.env")
    if not all(path.is_file() for path in required_paths):
        return ["diagnostic negative evidence is incomplete"]
    candidate_hash = sha256(candidate)
    profile = read_env(env_path)
    launch = read_env(launch_path)
    expected = {"name": "timer-ref-no-release-kvm-smp4",
                "machine": "q35", "accel": "kvm", "smp": "4",
                "group": "negative", "kernel_sha256": candidate_hash,
                "cleanup": "PASS", "run_complete": "YES"}
    if any(profile.get(key) != value for key, value in expected.items()):
        errors.append("diagnostic negative profile identity differs")
    if (launch.get("MACHINE") != "q35" or launch.get("ACCEL") != "kvm" or
            launch.get("SMP") != "4" or
            launch.get("KERNEL_SHA256") != candidate_hash):
        errors.append("diagnostic negative launch identity differs")
    symbols = symbols_path.read_text(errors="replace")
    if (" timer_ref_no_release_negative" not in symbols or
            "g_panic_test_state" in symbols):
        errors.append("diagnostic negative build symbols are incorrect")
    build_profile = read_env(root / "candidate" / "build-profile.env")
    if (build_profile.get("SELFTEST") != "1" or
            build_profile.get("SELFTEST_AUTORUN") != "0" or
            build_profile.get("KERNEL_EXTRA_CFLAGS") !=
            "-DHOBBYOS_REAPTEST_NEGATIVE_NO_TIMER_RELEASE"):
        errors.append("diagnostic negative build flags differ")
    transcript, byte_ends = complete_serial(serial_path)
    errors.extend(boot_errors(profile, transcript))
    rows = read_commands(ledger_path)
    if len(rows) != 1:
        errors.append("diagnostic negative does not have one transaction")
        return errors
    row = rows[0]
    errors.extend(validate_transaction(row, transcript, byte_ends, profile,
                                       candidate_hash, profile_dir))
    if (row.get("payload") != "reaptest timer-ref-no-release-negative" or
            row.get("handler_status") != 1):
        errors.append("diagnostic negative handler status is not the expected FAIL")
    segment = transaction_segment(transcript, row) or []
    failures = records_with_prefix(segment, "[REAPTEST][TIMER_REF] FAIL")
    passes = records_with_prefix(segment, "[REAPTEST][TIMER_REF] PASS")
    if len(failures) != 1 or passes:
        errors.append("diagnostic negative lacks one unambiguous FAIL result")
    else:
        value = failures[0][1]
        expected_fields = {
            "failure_stage": "REF_RELEASED",
            "last_completed": "OBSERVATION_HOLD", "mode": "target",
            "cleanup": "REAPED", "gate": "1", "hold_armed": "1",
            "hold_observed": "1", "sleeping": "1", "claimed": "1",
            "wake_claimed": "1", "killed": "1", "zombie": "1",
            "cancelled": "1", "ref": "1", "target_mask": "32768",
            "blocked": "1", "deferred": "1", "release_command": "0",
            "released": "0", "acquire_release": "0", "reaped": "0",
            "cleanup_reaped": "1", "residual_holds": "0",
        }
        if any(value.get(key) != item for key, item in expected_fields.items()):
            errors.append("diagnostic negative did not isolate omitted timer release")
        for key in ("target_id", "lifecycle", "wait_generation"):
            if (int_field(value, key) or 0) < 1:
                errors.append(f"diagnostic negative field {key} is not positive")
    if errors:
        for error in errors:
            print("diagnostic-negative-error: " + error)
    else:
        print("diagnostic-negative status=PASS handler_status=1 reason=timer-reference-release-absent")
    return errors


def fixture_build(root):
    candidate = root / "candidate"
    profile_dir = root / "profiles" / "q35-kvm-smp4"
    qemu = profile_dir / "qemu"
    candidate.mkdir(parents=True)
    qemu.mkdir(parents=True)
    kernel = candidate / "kernel.elf"
    kernel.write_bytes(b"fixture-kernel")
    candidate_hash = sha256(kernel)
    (candidate / "hobbyos.img").write_bytes(b"fixture-image")
    image_hash = sha256(candidate / "hobbyos.img")
    lines = []
    for slot in range(4):
        lines.extend([
            f"[IRQ][CPU_TIMER_VERIFIED] PASS slot={slot}",
            f"[SCHED][BOOTSTRAP_HANDOFF] PASS slot={slot}",
            f"[IRQ][CPU_READY] PASS slot={slot}",
        ])
    lines.extend([
        "[IRQ][CPU_READY_SUMMARY] expected=4 ready=4 failed=0 waiting=0 abort=0",
        "[CLOCK][HPET_TIMER0] QUIESCENT irq_enabled=0 route_enabled=0 pending=0 legacy=0",
        "[CLOCKEVENT][RUNTIME] ACTIVE source=BSP_LAPIC bsp_slot=0 period_us=1000",
        "[BOOT][RUNTIME_READY] PASS cpus=4/4",
    ])
    rows = []

    def add(payload, body, status=0):
        sequence = len(rows) + 1
        crc = f"{zlib.crc32(payload.encode()) & 0xffffffff:08x}"
        start = len(lines)
        lines.extend([
            f"[HARNESS][FRAME] ACCEPT seq={sequence} crc={crc} len={len(payload)}",
            f"[HARNESS][BEGIN] seq={sequence}", *body,
            f"[HARNESS][END] seq={sequence} status={status}",
        ])
        end = len(lines)
        rows.append({
            "schema": 2, "kind": "framed-command",
            "profile_id": "q35-kvm-smp4", "vm_id": "fixture-vm",
            "candidate_sha256": candidate_hash, "qemu_pid": 1234,
            "sequence": sequence, "payload": payload, "crc32": crc,
            "frame_length": len(f"tasktest exec {sequence} {crc} {payload}"),
            "deadline_seconds": 60, "start_line_count": start,
            "end_line_count": end, "accepted_line": start + 1,
            "begin_line": start + 2, "end_line": end,
            "handler_status": status, "transport_status": "PASS",
            "classification": "PASS", "disposition": "EXECUTED",
            "accept_count": 1, "begin_count": 1, "end_count": 1,
            "replay_count": 0, "reject_count": 0,
            "checksum_verified": True, "trigger_monotonic": sequence * 10.0,
            "completed_monotonic": sequence * 10.0 + 1.0,
        })
        lines.append(f"[FIXTURE][BOUNDARY] seq={sequence}")

    check = ("[IRQ][CHECK] PASS clocksource=HPET clockevent=BSP_LAPIC "
             "period_us=1000 hpet_timer0=QUIESCENT non_bsp_ticks=0 "
             "stray_hpet=0 cpus=4/4 controllers=1 routes=2 unexpected=0 "
             "imbalance=0 snapshot_polls=4 validation_polls=1 elapsed_ms=0 "
             "reason=ok")
    add("irq check", [check])
    controller = ["[IRQ][PIC] master=0xFF slave=0xFF quiescent=1"]
    controller += [f"[IRQ][LAPIC] slot={slot} apic={slot} mode=xapic lvt_masked=7"
                   for slot in range(4)]
    controller += ["[IRQ][HPET] clocksource=ACTIVE timer0=QUIESCENT irq_enabled=0 periodic=0 fsb=0 route_prepared=0 route_enabled=0 pending=0 legacy=0 stray=0"]
    add("irq controllers", controller)
    add("irq routes", ["[IRQ][ROUTES] PASS count=2"])
    boot = [
        "[IRQ][BOOT] state=SERVICES_ACTIVE cpus_prepared=4 cpus_verified=4 runtime_ready=4/4 handoffs=4 preemption=4 failed=0",
        "[IRQ][BOOT_PROBES] bsp_probe_vector=240 bsp_probe_entered=1 bsp_probe_returned=1 hpet_clocksource_verified=1",
        "[IRQ][BOOT_CLOCK] clocksource=HPET hpet_clocksource_verified=1 hpet_timer0_quiescent=1 hpet_timer0_irq_enabled=0 hpet_timer0_route_enabled=0 clockevent=BSP_LAPIC clockevent_period_us=1000 clockevent_non_bsp=0 hpet_stray_irqs=0",
    ]
    boot += [f"[IRQ][BOOT_CPU] slot={slot} apic={slot} first_vector=32 entered=10 returned=10 depth=0 unexpected=0 handoff=1 preempt=1 ready=1"
             for slot in range(4)]
    boot += ["[IRQ][BOOT_RESULT] PASS snapshots=4"]
    add("irq boot", boot)
    lapic_config = [
        f"[ACCOUNT][LAPIC_CONFIG_CPU] slot={slot} failed_mask=0 vector=34 "
        "divisor=16 initial=62500 periodic=1 masked=0"
        for slot in range(4)]
    lapic_config.append("[ACCOUNT][LAPIC_CONFIG] PASS cpus=4 spread_x10=0")
    add("accounttest lapic-config", lapic_config)
    add("accounttest lapic-liveness 500", ["[ACCOUNT][LAPIC_LIVENESS] PASS window_ms=500"])
    add("accounttest check", ["[ACCOUNT][CHECK] PASS"])
    rate = [f"[ACCOUNT][LAPIC_RATE_CPU] round={round_id} slot={slot} delta=2000 expected=2000 ratio_x1000=1000"
            for round_id in range(1, 6) for slot in range(4)]
    rate += ["[ACCOUNT][LAPIC_RATE_MEDIANS] cpu0=1000 cpu1=1000 cpu2=1000 cpu3=1000",
             "[ACCOUNT][LAPIC_RATE] PASS window_ms=2000 rounds=5 worst_median_x1000=1000 min_round_x1000=1000 max_round_x1000=1000"]
    add("accounttest lapic-rate 2000 5", rate)
    add("synctest sleep", ["[SYNC][SLEEP] START run=1 requested=4 completed=0 errors=0"])
    add("synctest async-wait 1 300000", ["[SYNC][ASYNC_WAIT] PASS run=1 kind=SLEEP state=PASS requested=4 completed=4 errors=0"])
    add("synctest timer-order", ["[SYNC][TIMER_ORDER] PASS callbacks=2"])
    add("synctest timer-cancel", ["[SYNC][TIMER_CANCEL] START run=2 requested=3 completed=0 errors=0"])
    add("synctest async-wait 2 240000", ["[SYNC][ASYNC_WAIT] PASS run=2 kind=TIMER_CANCEL state=PASS requested=3 completed=3 errors=0"])
    add("synctest timer-backlog", ["[SYNC][TIMER_BACKLOG] PASS residual=0"])
    timer_ref_common = (
        "cleanup=REAPED target_id=40 lifecycle=400 wait_generation=7 "
        "gate=1 hold_armed=1 hold_observed=1 sleeping=1 claimed=1 "
        "wake_claimed=1 killed=1 zombie=1 cancelled=1 ref=1 "
        "target_mask=32768 blocked=1 scan_reaped=0 deferred=1")
    timer_ref_release = (
        "release_command=1 released=1 acquire_release=1 nodes_pending=0 "
        "nodes_claimed=0 node_refs=0 wrong_lifecycle=0 "
        "global_ref_release_delta=1 stale_delta=1 dispatched_delta=1 "
        "reaped=1 cleanup_reaped=1 residual_holds=0")
    for _ in range(3):
        add("reaptest timer-ref", [
            "[REAPTEST][TIMER_REF] PASS mode=target " + timer_ref_common +
            " competitor=0 competitor_reaped=0 competitor_gone=0 "
            "competitor_target_present=1 legacy_blocked=1 " +
            timer_ref_release])
        add("reaptest timer-ref-competing", [
            "[REAPTEST][TIMER_REF] PASS mode=competing " +
            timer_ref_common +
            " competitor=1 competitor_reaped=1 competitor_gone=1 "
            "competitor_target_present=1 legacy_blocked=0 " +
            timer_ref_release])
    add("synctest check", ["[SYNC][CHECK] PASS"])
    add("taskdiag check", ["[TASKDIAG][CHECK] PASS"])
    add("inputtest check", ["[INPUTTEST][CHECK] PASS"])
    add("modaltest check", ["[MODALTEST][CHECK] PASS"])
    add("taskmantest check", ["[TASKMANTEST][CHECK] PASS"])
    add("clear", [])
    add("taskman 1000", ["[TASKMAN][OWNER_STATE] shell=BLOCKED session=ACTIVE",
                          "[MODAL] session_end OK"])
    taskman_row = rows[-1]
    trigger = taskman_row["trigger_monotonic"]
    taskman_row.update({
        "modal_owner_line": taskman_row["begin_line"] + 1,
        "modal_settle_seconds": 2.5,
        "modal_settle_started_monotonic": trigger + 0.1,
        "modal_screenshot_monotonic": trigger + 2.7,
        "modal_escape_monotonic": trigger + 2.8,
        "completed_monotonic": trigger + 3.0,
    })
    add("taskmantest stats", ["[TASKMANTEST][STATS] stable_frame_full_clears=0 stale_cells=0 workspace_live=0 last_session_full_frames=1 last_session_fallback_frames=0 render_failures=0 clipped=0 builder_truncations=0 summary_failures=0 footer_failures=0 region_clear_failures=0 separator_mismatches=0 field_overflows=0 field_truncations=0 split_overlaps=0 shell_exit_scroll_delta=0 last_session_shell_exit_scroll_delta=0 last_session_scroll_delta=0 auto_exit_pending=0"])
    byte_ends = []
    byte_offset = 0
    for line in lines:
        byte_offset += len((line + "\n").encode())
        byte_ends.append(byte_offset)
    for row in rows:
        start = row["start_line_count"]
        end = row["end_line_count"]
        row["start_byte_count"] = 0 if start == 0 else byte_ends[start - 1]
        row["serial_byte_count_at_trigger"] = row["start_byte_count"]
        row["end_byte_count"] = byte_ends[end - 1]
        row["serial_byte_count_observed"] = row["end_byte_count"]
    (qemu / "qemu-serial.log").write_text("\n".join(lines) + "\n")
    (profile_dir / "profile.env").write_text(
        "schema=2\nname=q35-kvm-smp4\nvm_id=fixture-vm\nmachine=q35\n"
        "smp=4\naccel=kvm\ngroup=full\nkernel_sha256=" + candidate_hash +
        "\nimage_sha256_before=" + image_hash +
        "\nhost_schedulable_cpus=8\nrate_authority=AUTHORITATIVE_CONTROL\n"
        "input_profile=framed\n"
        "command_input_observation=release-stable-pre-enter-and-dispatcher-gdb\n"
        "cleanup=PASS\nrun_complete=YES\n")
    (qemu / "launch.env").write_text(
        "MACHINE=q35\nACCEL=kvm\nSMP=4\nIMAGE_SHA256=" + image_hash +
        "\nKERNEL_SHA256=" + candidate_hash + "\n")
    screenshot = profile_dir / "taskman-screendump.ppm"
    screenshot.write_bytes(b"P6\n200 200\n255\n" + b"\0" * 120000)
    taskman_row["modal_screenshot"] = {
        "path": screenshot.name,
        "bytes": screenshot.stat().st_size,
        "sha256": sha256(screenshot),
    }
    fixture_symbols = {
        "g_shell_active": "0xffffffff82001000",
        "g_buffer": "0xffffffff82001100",
        "g_len": "0xffffffff82001200",
        "g_pos": "0xffffffff82001204",
    }
    for row in rows:
        sequence = row["sequence"]
        frame = (f"tasktest exec {sequence} {row['crc32']} "
                 f"{row['payload']}")
        expected = frame.encode("ascii")
        memory = expected + b"\0" * (256 - len(expected))
        length_hex = len(expected).to_bytes(4, "little", signed=True).hex()

        boundary_log = profile_dir / f"input-boundary-{sequence}.log"
        boundary_command = profile_dir / f"input-boundary-{sequence}.cmd"
        boundary_argv = profile_dir / f"input-boundary-{sequence}.argv.json"
        boundary_log.write_text(
            '4^done,memory=[{begin="0xffffffff82001000",offset="0",'
            'end="0xffffffff82001001",contents="01"}]\n'
            '5^done,memory=[{begin="0xffffffff82001200",offset="0",'
            'end="0xffffffff82001204",contents="00000000"}]\n'
            '6^done,memory=[{begin="0xffffffff82001204",offset="0",'
            'end="0xffffffff82001208",contents="00000000"}]\n',
            encoding="utf-8")
        boundary_command.write_text(
            '1-gdb-set confirm off\n2-gdb-set pagination off\n'
            '3-target-select remote /fixture/gdb.sock\n'
            '4-data-read-memory-bytes 0xffffffff82001000 1\n'
            '5-data-read-memory-bytes 0xffffffff82001200 4\n'
            '6-data-read-memory-bytes 0xffffffff82001204 4\n'
            '7-target-detach\n8-gdb-exit\n', encoding="utf-8")
        boundary_argv.write_text(json.dumps(
            ["gdb", "-q", "-nx", "--interpreter=mi2",
             "/fixture/kernel.elf"], indent=2) + "\n", encoding="utf-8")

        delivery_log = profile_dir / f"input-delivery-{sequence}-1.log"
        delivery_command = profile_dir / f"input-delivery-{sequence}-1.cmd"
        delivery_argv = profile_dir / f"input-delivery-{sequence}-1.argv.json"
        delivery_log.write_text(
            '4^done,memory=[{begin="0xffffffff82001000",offset="0",'
            'end="0xffffffff82001001",contents="01"}]\n'
            '5^done,memory=[{begin="0xffffffff82001200",offset="0",'
            f'end="0xffffffff82001204",contents="{length_hex}"}}]\n'
            '6^done,memory=[{begin="0xffffffff82001204",offset="0",'
            f'end="0xffffffff82001208",contents="{length_hex}"}}]\n'
            '7^done,memory=[{begin="0xffffffff82001100",offset="0",'
            f'end="0xffffffff82001200",contents="{memory.hex()}"}}]\n',
            encoding="utf-8")
        delivery_command.write_text(
            '1-gdb-set confirm off\n2-gdb-set pagination off\n'
            '3-target-select remote /fixture/gdb.sock\n'
            '4-data-read-memory-bytes 0xffffffff82001000 1\n'
            '5-data-read-memory-bytes 0xffffffff82001200 4\n'
            '6-data-read-memory-bytes 0xffffffff82001204 4\n'
            '7-data-read-memory-bytes 0xffffffff82001100 256\n'
            '8-target-detach\n9-gdb-exit\n', encoding="utf-8")
        delivery_argv.write_text(json.dumps(
            ["gdb", "-q", "-nx", "--interpreter=mi2",
             "/fixture/kernel.elf"], indent=2) + "\n", encoding="utf-8")
        delivery_log_second = profile_dir / \
            f"input-delivery-{sequence}-2.log"
        delivery_command_second = profile_dir / \
            f"input-delivery-{sequence}-2.cmd"
        delivery_argv_second = profile_dir / \
            f"input-delivery-{sequence}-2.argv.json"
        delivery_log_second.write_bytes(delivery_log.read_bytes())
        delivery_command_second.write_bytes(delivery_command.read_bytes())
        delivery_argv_second.write_bytes(delivery_argv.read_bytes())

        log_path = profile_dir / f"input-observer-{sequence}.log"
        command_path = profile_dir / f"input-observer-{sequence}.cmd"
        argv_path = profile_dir / f"input-observer-{sequence}.argv.json"
        log_path.write_text(
            '4^done,memory=[{begin="0xffffffff82001000",offset="0",'
            'end="0xffffffff82001001",contents="01"}]\n'
            '5^done,memory=[{begin="0xffffffff82001200",offset="0",'
            f'end="0xffffffff82001204",contents="{length_hex}"}}]\n'
            '6^done,memory=[{begin="0xffffffff82001204",offset="0",'
            f'end="0xffffffff82001208",contents="{length_hex}"}}]\n'
            '7^done,memory=[{begin="0xffffffff82001100",offset="0",'
            f'end="0xffffffff82001200",contents="{memory.hex()}"}}]\n'
            '*stopped,reason="breakpoint-hit",bkptno="1",'
            'frame={func="shell_dispatch_command_line"}\n'
            f'10^done,memory=[{{begin="0x1",offset="0",end="0x101",'
            f'contents="{memory.hex()}"}}]\n', encoding="utf-8")
        command_path.write_text(
            '1-gdb-set confirm off\n2-gdb-set pagination off\n'
            '3-target-select remote /fixture/gdb.sock\n'
            '4-data-read-memory-bytes 0xffffffff82001000 1\n'
            '5-data-read-memory-bytes 0xffffffff82001200 4\n'
            '6-data-read-memory-bytes 0xffffffff82001204 4\n'
            '7-data-read-memory-bytes 0xffffffff82001100 256\n'
            '8-break-insert shell_dispatch_command_line\n'
            '9-exec-continue\n10-data-read-memory-bytes $rdi 256\n'
            '11-break-delete 1\n12-target-detach\n13-gdb-exit\n',
            encoding="utf-8")
        argv_path.write_text(json.dumps(
            ["gdb", "-q", "-nx", "--interpreter=mi2",
             "/fixture/kernel.elf"], indent=2) + "\n", encoding="utf-8")

        text_log = profile_dir / f"transport-{sequence}.log"
        enter_log = profile_dir / f"transport-{sequence}-enter.log"
        release_log = profile_dir / f"transport-{sequence}-input-release.log"
        text_argv = ["/usr/bin/python3", "/fixture/scripts/qemu_hmp.py",
                     "--socket", "/fixture/hmp.sock", "text", frame,
                     "--profile", "framed"]
        enter_argv = ["/usr/bin/python3", "/fixture/scripts/qemu_hmp.py",
                      "--socket", "/fixture/hmp.sock", "key", "ret",
                      "--profile", "framed"]
        release_argv = ["/usr/bin/python3", "/fixture/scripts/qemu_hmp.py",
                        "--socket", "/fixture/hmp.sock", "key", "shift",
                        "--profile", "framed"]
        text_log.write_text("argv=" + json.dumps(text_argv) +
                            "\nreturn_code=0\nstdout:\n\nstderr:\n",
                            encoding="utf-8")
        enter_log.write_text("argv=" + json.dumps(enter_argv) +
                             "\nreturn_code=0\nstdout:\n\nstderr:\n",
                             encoding="utf-8")
        release_log.write_text("argv=" + json.dumps(release_argv) +
                               "\nreturn_code=0\nstdout:\n\nstderr:\n",
                               encoding="utf-8")
        sample = {
            "sample": 1, "active": 1, "length": 0, "position": 0,
            "clean": True, "active_response_token": 4,
            "length_response_token": 5, "position_response_token": 6,
            "log": boundary_log.name, "log_sha256": sha256(boundary_log),
            "command": boundary_command.name,
            "command_sha256": sha256(boundary_command),
            "argv": boundary_argv.name, "argv_sha256": sha256(boundary_argv),
        }
        trigger = row["trigger_monotonic"]
        delivery_sample = {
            "sample": 1, "active": 1, "length": len(expected),
            "position": len(expected), "terminated": True,
            "observed_bytes_hex": expected.hex(), "observed_text": frame,
            "status": "EXACT", "active_response_token": 4,
            "length_response_token": 5, "position_response_token": 6,
            "memory_response_token": 7,
            "observed_monotonic": trigger + 0.40,
            "log": delivery_log.name,
            "log_sha256": sha256(delivery_log),
            "command": delivery_command.name,
            "command_sha256": sha256(delivery_command),
            "argv": delivery_argv.name,
            "argv_sha256": sha256(delivery_argv),
        }
        delivery_sample_second = dict(delivery_sample)
        delivery_sample_second.update({
            "sample": 2,
            "observed_monotonic": trigger + 1.00,
            "log": delivery_log_second.name,
            "log_sha256": sha256(delivery_log_second),
            "command": delivery_command_second.name,
            "command_sha256": sha256(delivery_command_second),
            "argv": delivery_argv_second.name,
            "argv_sha256": sha256(delivery_argv_second),
        })
        row.update({
            "schema": 7, "hmp_input_status": "PASS",
            "hmp_text_status": "PASS", "hmp_enter_status": "PASS",
            "guest_input_status": "PASS", "dispatcher_status": "OBSERVED",
            "dispatcher_stop": ('*stopped,reason="breakpoint-hit",'
                                'bkptno="1",frame={'
                                'func="shell_dispatch_command_line"}'),
            "guest_input_length": len(expected),
            "guest_input_bytes_hex": expected.hex(),
            "guest_input_text": frame,
            "input_boundary_status": "CLEAN",
            "input_boundary_active": 1, "input_boundary_length": 0,
            "input_boundary_position": 0,
            "input_boundary_samples": [sample],
            "input_symbol_addresses": fixture_symbols,
            "pre_enter_status": "PASS", "pre_enter_active": 1,
            "pre_enter_length": len(expected),
            "pre_enter_position": len(expected),
            "pre_enter_bytes_hex": expected.hex(), "pre_enter_text": frame,
            "pre_enter_active_response_token": 4,
            "pre_enter_length_response_token": 5,
            "pre_enter_position_response_token": 6,
            "pre_enter_memory_response_token": 7,
            "input_observer_log": log_path.name,
            "input_observer_log_sha256": sha256(log_path),
            "input_observer_command": command_path.name,
            "input_observer_command_sha256": sha256(command_path),
            "input_observer_argv": argv_path.name,
            "input_observer_argv_sha256": sha256(argv_path),
            "input_observer_memory_response_token": 10,
            "hmp_text_log": text_log.name,
            "hmp_text_log_sha256": sha256(text_log),
            "hmp_enter_log": enter_log.name,
            "hmp_enter_log_sha256": sha256(enter_log),
            "input_delivery_status": "COMPLETE",
            "input_delivery_timeout_seconds": 30.0,
            "input_delivery_stability_seconds": 0.5,
            "input_delivery_sample_delay_seconds": 0.25,
            "input_delivery_started_monotonic": trigger + 0.10,
            "input_delivery_completed_monotonic": trigger + 1.01,
            "input_delivery_stable_span_seconds":
                delivery_sample_second["observed_monotonic"] -
                delivery_sample["observed_monotonic"],
            "input_delivery_samples": [delivery_sample,
                                       delivery_sample_second],
            "input_release_key": "shift",
            "input_release_status": "PASS",
            "input_release_started_monotonic": trigger + 0.41,
            "input_release_completed_monotonic": trigger + 0.42,
            "input_release_log": release_log.name,
            "input_release_log_sha256": sha256(release_log),
            "hmp_text_started_monotonic": trigger + 0.01,
            "hmp_text_completed_monotonic": trigger + 0.10,
            "pre_enter_observed_monotonic": trigger + 1.10,
            "input_observer_armed_monotonic": trigger + 1.11,
            "input_observer_resumed_monotonic": trigger + 1.12,
            "hmp_enter_completed_monotonic": trigger + 1.13,
            "hmp_input_completed_monotonic": trigger + 1.14,
            "completed_monotonic": trigger + 3.00,
        })
    (profile_dir / "commands.jsonl").write_text(
        "".join(json.dumps(row, sort_keys=True) + "\n" for row in rows))
    return profile_dir, candidate_hash


def mutate_fixture(root, name):
    profile = root / "profiles" / "q35-kvm-smp4"
    serial_path = profile / "qemu" / "qemu-serial.log"
    ledger_path = profile / "commands.jsonl"
    lines = serial_path.read_text().splitlines()
    rows = read_commands(ledger_path)

    def observer_log(row):
        return profile / row["input_observer_log"]

    def refresh_observer_log_hash(row):
        value = sha256(observer_log(row))
        row["input_observer_log_sha256"] = value

    def boundary_log(row):
        return profile / row["input_boundary_samples"][-1]["log"]

    def refresh_boundary_log_hash(row):
        row["input_boundary_samples"][-1]["log_sha256"] = \
            sha256(boundary_log(row))

    def delivery_log(row):
        return profile / row["input_delivery_samples"][-1]["log"]

    def refresh_delivery_log_hash(row):
        row["input_delivery_samples"][-1]["log_sha256"] = \
            sha256(delivery_log(row))

    def replace_delivery_snapshot(row, observed, status):
        sample = row["input_delivery_samples"][-1]
        path = delivery_log(row)
        length_hex = len(observed).to_bytes(
            4, "little", signed=True).hex()
        memory = observed + b"\0" * (256 - len(observed))
        text = path.read_text()
        for token in (5, 6):
            text, count = re.subn(
                rf'(?m)^({token}\^done,[^\n]*contents=")[0-9a-fA-F]+'
                r'("[^\n]*)$',
                lambda match: match.group(1) + length_hex + match.group(2),
                text)
            if count != 1:
                raise RuntimeError(
                    "delivery fixture length response is not unique")
        text, count = re.subn(
            r'(?m)^(7\^done,[^\n]*contents=")[0-9a-fA-F]+("[^\n]*)$',
            lambda match: match.group(1) + memory.hex() + match.group(2),
            text)
        if count != 1:
            raise RuntimeError("delivery fixture memory response is not unique")
        path.write_text(text)
        sample.update({
            "length": len(observed), "position": len(observed),
            "terminated": True, "observed_bytes_hex": observed.hex(),
            "observed_text": (observed.decode("ascii") if
                              observed.isascii() else None),
            "status": status,
        })
        refresh_delivery_log_hash(row)

    def replace_observed_memory(row, observed):
        memory = observed + b"\0" * (256 - len(observed))
        path = observer_log(row)
        changed, count = re.subn(
            r'(?m)^(10\^done,[^\n]*contents=")[0-9a-fA-F]+("[^\n]*)$',
            lambda match: match.group(1) + memory.hex() + match.group(2),
            path.read_text())
        if count != 1:
            raise RuntimeError("observer fixture memory response is not unique")
        path.write_text(changed)
        refresh_observer_log_hash(row)

    def replace_pre_enter_memory(row, observed):
        memory = observed + b"\0" * (256 - len(observed))
        path = observer_log(row)
        changed, count = re.subn(
            r'(?m)^(7\^done,[^\n]*contents=")[0-9a-fA-F]+("[^\n]*)$',
            lambda match: match.group(1) + memory.hex() + match.group(2),
            path.read_text())
        if count != 1:
            raise RuntimeError("pre-Enter fixture memory response is not unique")
        path.write_text(changed)
        refresh_observer_log_hash(row)

    def replace_once(old, new):
        matches = [index for index, line in enumerate(lines) if old in line]
        if len(matches) != 1:
            raise RuntimeError(f"fixture mutation target count for {old}: {len(matches)}")
        lines[matches[0]] = lines[matches[0]].replace(old, new)

    if name == "boot-cpu-intermediate-missing":
        replace_once("[IRQ][BOOT_CPU] slot=1", "[FIXTURE][REMOVED] slot=1")
    elif name == "boot-cpu-duplicate":
        replace_once("[IRQ][BOOT_CPU] slot=1", "[IRQ][BOOT_CPU] slot=0")
    elif name in ("boot-cpu-out-of-range", "boot-cpu-prefix-confusion"):
        replace_once("[IRQ][BOOT_CPU] slot=3", "[IRQ][BOOT_CPU] slot=30")
    elif name in ("marker-after-end", "late-result-next-transaction"):
        rate_index = next(index for index, row in enumerate(rows)
                          if row["payload"] == "accounttest lapic-rate 2000 5")
        marker = "[ACCOUNT][LAPIC_RATE] PASS window_ms=2000 rounds=5 worst_median_x1000=1000 min_round_x1000=1000 max_round_x1000=1000"
        replace_once(marker, "[FIXTURE][REMOVED_RATE_RESULT]")
        boundary = rows[rate_index]["end_line"]
        lines[boundary] = marker
    elif name == "end-missing":
        replace_once("[HARNESS][END] seq=1 status=0", "[FIXTURE][END_REMOVED] seq=1")
    elif name == "end-status-wrong":
        replace_once("[HARNESS][END] seq=1 status=0", "[HARNESS][END] seq=1 status=1")
    elif name == "end-duplicate":
        end = rows[0]["end_line"]
        lines[end] = "[HARNESS][END] seq=1 status=0"
    elif name == "handler-fail-human-pass":
        row = rows[0]
        row["handler_status"] = 1
        replace_once("[HARNESS][END] seq=1 status=0", "[HARNESS][END] seq=1 status=1")
    elif name == "replay":
        rows[0]["disposition"] = "REPLAY"
        rows[0]["replay_count"] = 1
        lines[rows[0]["end_line"]] = "[HARNESS][REPLAY] seq=1 status=0"
    elif name == "checksum-incorrect":
        row = rows[0]
        row["crc32"] = "deadbeef"
        row["checksum_verified"] = True
        old = f"crc={zlib.crc32(row['payload'].encode()) & 0xffffffff:08x}"
        replace_once(old, "crc=deadbeef")
    elif name == "run-id-wrong":
        replace_once("[SYNC][ASYNC_WAIT] PASS run=1 kind=SLEEP",
                     "[SYNC][ASYNC_WAIT] PASS run=9 kind=SLEEP")
    elif name == "false-authoritative-control":
        env = read_env(profile / "profile.env")
        text = (profile / "profile.env").read_text().replace(
            "host_schedulable_cpus=8", "host_schedulable_cpus=2")
        (profile / "profile.env").write_text(text)
    elif name == "other-binary":
        rows[0]["candidate_sha256"] = "0" * 64
    elif name == "irq-unexpected":
        replace_once("unexpected=0 imbalance=0", "unexpected=1 imbalance=0")
    elif name == "irq-bounded-sampling-missing":
        replace_once("snapshot_polls=4 validation_polls=1",
                     "snapshot_polls=0 validation_polls=0")
    elif name == "timer0-active":
        replace_once("timer0=QUIESCENT irq_enabled=0 periodic=0",
                     "timer0=ACTIVE irq_enabled=1 periodic=0")
    elif name == "timer-ref-release-missing":
        matches = [index for index, line in enumerate(lines)
                   if line.startswith(
                       "[REAPTEST][TIMER_REF] PASS mode=target ")]
        if len(matches) != 3:
            raise RuntimeError("timer-reference fixture count is not three")
        lines[matches[0]] = lines[matches[0]].replace(
            " release_command=1 released=1 acquire_release=1 ",
            " release_command=1 released=0 acquire_release=0 ", 1)
    elif name == "taskman-without-settle":
        row = next(row for row in rows if row["payload"] == "taskman 1000")
        row.pop("modal_settle_started_monotonic")
    elif name == "guest-input-byte-changed":
        row = rows[0]
        observed = bytearray(row["guest_input_text"].encode("ascii"))
        observed[17] = ord("0") if observed[17] != ord("0") else ord("1")
        replace_observed_memory(row, bytes(observed))
        row["guest_input_bytes_hex"] = bytes(observed).hex()
        row["guest_input_text"] = bytes(observed).decode("ascii")
        row["guest_input_length"] = len(observed)
    elif name == "guest-input-summary-diverges":
        row = rows[0]
        observed = bytearray(row["guest_input_text"].encode("ascii"))
        observed[-1] = ord("x")
        row["guest_input_bytes_hex"] = bytes(observed).hex()
        row["guest_input_text"] = bytes(observed).decode("ascii")
    elif name == "input-observer-missing":
        observer_log(rows[0]).unlink()
    elif name == "input-boundary-dirty":
        row = rows[0]
        path = boundary_log(row)
        text, count = re.subn(
            r'(?m)^(4\^done,[^\n]*contents=")01("[^\n]*)$',
            r'\g<1>00\g<2>', path.read_text())
        if count != 1:
            raise RuntimeError("boundary active response is not unique")
        path.write_text(text)
        row["input_boundary_active"] = 0
        row["input_boundary_samples"][-1]["active"] = 0
        row["input_boundary_samples"][-1]["clean"] = False
        refresh_boundary_log_hash(row)
    elif name == "input-observer-other-transaction":
        row = rows[0]
        other = (f"tasktest exec 9 {row['crc32']} "
                 f"{row['payload']}").encode("ascii")
        replace_observed_memory(row, other)
    elif name == "input-observer-hash-stale":
        path = observer_log(rows[0])
        path.write_text(path.read_text() + "~diagnostic-noise\n")
    elif name == "input-delivery-artifact-missing":
        delivery_log(rows[0]).unlink()
    elif name == "input-delivery-hash-stale":
        path = delivery_log(rows[0])
        path.write_text(path.read_text() + "~diagnostic-noise\n")
    elif name == "input-delivery-raw-diverges":
        row = rows[0]
        path = delivery_log(row)
        memory = bytearray.fromhex(
            row["input_delivery_samples"][-1]["observed_bytes_hex"])
        memory[-1] = ord("x")
        padded = bytes(memory) + b"\0" * (256 - len(memory))
        changed, count = re.subn(
            r'(?m)^(7\^done,[^\n]*contents=")[0-9a-fA-F]+("[^\n]*)$',
            lambda match: match.group(1) + padded.hex() + match.group(2),
            path.read_text())
        if count != 1:
            raise RuntimeError("delivery fixture memory response is not unique")
        path.write_text(changed)
        refresh_delivery_log_hash(row)
    elif name == "input-delivery-summary-diverges":
        rows[0]["input_delivery_samples"][-1]["observed_text"] = "wrong"
    elif name == "input-delivery-timeout-promoted":
        rows[0]["input_delivery_status"] = "TIMEOUT"
    elif name == "input-delivery-sample-order":
        rows[0]["input_delivery_samples"][-1]["sample"] = 3
    elif name == "input-delivery-single-exact":
        row = rows[0]
        row["input_delivery_samples"] = row["input_delivery_samples"][:1]
        row["input_delivery_stable_span_seconds"] = 0.0
    elif name == "input-delivery-stability-short":
        row = rows[0]
        first = row["input_delivery_samples"][0]["observed_monotonic"]
        row["input_delivery_samples"][1]["observed_monotonic"] = first + 0.49
        row["input_delivery_stable_span_seconds"] = 0.49
    elif name == "input-release-artifact-missing":
        row = rows[0]
        (profile / row["input_release_log"]).unlink()
    elif name == "input-release-key-wrong":
        rows[0]["input_release_key"] = "ctrl"
    elif name == "input-delivery-late-extra":
        row = rows[0]
        expected = row["input_delivery_samples"][-1][
            "observed_text"].encode("ascii")
        replace_delivery_snapshot(row, expected + b"r", "DIVERGED")
    elif name == "hmp-ack-without-guest-proof":
        row = rows[0]
        row["guest_input_status"] = "NOT_OBSERVED"
        row["transport_status"] = "PASS"
    elif name == "pre-enter-byte-changed":
        row = rows[0]
        observed = bytearray(row["pre_enter_text"].encode("ascii"))
        observed[-1] = ord("x")
        replace_pre_enter_memory(row, bytes(observed))
        row["pre_enter_bytes_hex"] = bytes(observed).hex()
        row["pre_enter_text"] = bytes(observed).decode("ascii")
    elif name == "pre-enter-summary-diverges":
        row = rows[0]
        row["pre_enter_bytes_hex"] = b"wrong".hex()
        row["pre_enter_text"] = "wrong"
        row["pre_enter_length"] = 5
        row["pre_enter_position"] = 5
    elif name == "terminator-in-text-delivery":
        row = rows[0]
        path = profile / row["hmp_text_log"]
        content = path.read_text().replace(
            '"--profile", "framed"]',
            '"--enter", "--profile", "framed"]', 1)
        path.write_text(content)
        row["hmp_text_log_sha256"] = sha256(path)
    elif name == "enter-artifact-missing":
        row = rows[0]
        (profile / row["hmp_enter_log"]).unlink()
    elif name == "dispatcher-observation-missing":
        row = rows[0]
        path = observer_log(row)
        content = path.read_text().replace(
            '*stopped,reason="breakpoint-hit",bkptno="1",'
            'frame={func="shell_dispatch_command_line"}\n', '', 1)
        path.write_text(content)
        refresh_observer_log_hash(row)
    elif name == "input-profile-not-framed":
        path = profile / "profile.env"
        path.write_text(path.read_text().replace(
            "input_profile=framed", "input_profile=sync", 1))
    elif name == "hmp-text-other-frame":
        row = rows[0]
        path = profile / row["hmp_text_log"]
        path.write_text(path.read_text().replace(
            row["payload"], "irq routes", 1))
        row["hmp_text_log_sha256"] = sha256(path)
    else:
        raise RuntimeError(f"unknown mutation {name}")
    byte_ends = []
    byte_offset = 0
    for line in lines:
        byte_offset += len((line + "\n").encode())
        byte_ends.append(byte_offset)
    for row in rows:
        start = row["start_line_count"]
        end = row["end_line_count"]
        row["start_byte_count"] = 0 if start == 0 else byte_ends[start - 1]
        row["serial_byte_count_at_trigger"] = row["start_byte_count"]
        row["end_byte_count"] = byte_ends[end - 1]
        row["serial_byte_count_observed"] = row["end_byte_count"]
    serial_path.write_text("\n".join(lines) + "\n")
    ledger_path.write_text("".join(json.dumps(row, sort_keys=True) + "\n"
                                   for row in rows))


def fixture_selftest(output_dir=None):
    names = (
        "boot-cpu-intermediate-missing", "boot-cpu-duplicate",
        "boot-cpu-out-of-range", "boot-cpu-prefix-confusion",
        "marker-after-end", "end-missing", "end-status-wrong",
        "end-duplicate", "handler-fail-human-pass", "replay",
        "checksum-incorrect", "run-id-wrong", "late-result-next-transaction",
        "false-authoritative-control", "other-binary", "irq-unexpected",
        "irq-bounded-sampling-missing", "timer0-active",
        "timer-ref-release-missing", "taskman-without-settle",
        "guest-input-byte-changed", "guest-input-summary-diverges",
        "input-observer-missing", "input-boundary-dirty",
        "input-observer-other-transaction", "input-observer-hash-stale",
        "input-delivery-artifact-missing", "input-delivery-hash-stale",
        "input-delivery-raw-diverges", "input-delivery-summary-diverges",
        "input-delivery-timeout-promoted", "input-delivery-sample-order",
        "input-delivery-single-exact", "input-delivery-stability-short",
        "input-release-artifact-missing", "input-release-key-wrong",
        "input-delivery-late-extra",
        "hmp-ack-without-guest-proof", "pre-enter-byte-changed",
        "pre-enter-summary-diverges", "terminator-in-text-delivery",
        "enter-artifact-missing", "dispatcher-observation-missing",
        "input-profile-not-framed", "hmp-text-other-frame",
    )
    destination = Path(output_dir).resolve() if output_dir else None
    if destination:
        destination.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="foundation-verifier-") as temporary:
        control = Path(temporary) / "control"
        profile, candidate_hash = fixture_build(control)
        errors = verify_profile(control, profile, candidate_hash)
        if errors:
            print("fixture control rejected: " + "; ".join(errors), file=sys.stderr)
            return 1
        print("fixture=valid-control result=ACCEPTED")
        failed = []
        for name in names:
            target = Path(temporary) / name
            shutil.copytree(control, target)
            mutate_fixture(target, name)
            profile = target / "profiles" / "q35-kvm-smp4"
            errors = verify_profile(target, profile,
                                    sha256(target / "candidate" / "kernel.elf"))
            if not errors:
                failed.append(name)
                reason = "accepted invalid evidence"
                print(f"fixture={name} result=ACCEPTED_INCORRECTLY")
            else:
                reason = errors[0]
                print(f"fixture={name} result=REJECTED reason={reason}")
            if destination:
                fixture_dir = destination / name
                fixture_dir.mkdir()
                shutil.copy2(profile / "qemu" / "qemu-serial.log",
                             fixture_dir / "serial.log")
                shutil.copy2(profile / "commands.jsonl",
                             fixture_dir / "commands.jsonl")
                for pattern in ("input-boundary-1.*", "input-delivery-1-*",
                                "input-observer-1.*",
                                "transport-1*.log"):
                    for observer in sorted(profile.glob(pattern)):
                        if observer.is_file():
                            shutil.copy2(observer, fixture_dir / observer.name)
                (fixture_dir / "expectation.json").write_text(json.dumps({
                    "mutation": name, "expected": "REJECTED",
                    "observed": "ACCEPTED" if not errors else "REJECTED",
                    "reason": reason,
                }, indent=2, sort_keys=True) + "\n")
        if destination:
            (destination / "control.json").write_text(json.dumps({
                "expected": "ACCEPTED", "observed": "ACCEPTED",
                "candidate_sha256": candidate_hash,
            }, indent=2, sort_keys=True) + "\n")
        if failed:
            print("verifier accepted invalid fixtures: " + ", ".join(failed),
                  file=sys.stderr)
            return 1
    print(f"fixture-selftest: PASS rejected={len(names)}")
    return 0


def main():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("all", "focal", "production", "diagnostic-negative"):
        item = sub.add_parser(name)
        item.add_argument("root")
    profile_parser = sub.add_parser("profile")
    profile_parser.add_argument("root")
    profile_parser.add_argument("profile_dir")
    fixtures = sub.add_parser("fixtures")
    fixtures.add_argument("--output-dir")
    transport_rejection = sub.add_parser("transport-rejection")
    transport_rejection.add_argument("profile_dir")
    transport_rejection.add_argument("record")
    transport_rejection.add_argument("serial")
    transport_command = sub.add_parser("transport-command")
    transport_command.add_argument("profile_dir")
    transport_command.add_argument("record")
    transport_command.add_argument("serial")
    transport_input = sub.add_parser("transport-input")
    transport_input.add_argument("profile_dir")
    transport_input.add_argument("record")
    transport_profile = sub.add_parser("transport-profile")
    transport_profile.add_argument("root")
    transport_profile.add_argument("profile_dir")
    args = parser.parse_args()
    if args.command == "fixtures":
        return fixture_selftest(args.output_dir)
    if args.command == "transport-rejection":
        errors = verify_transport_rejection(
            Path(args.profile_dir).resolve(), Path(args.record).resolve(),
            Path(args.serial).resolve())
        if errors:
            print("transport-rejection status=FAIL")
            for error in errors:
                print(f"  {error}")
            return 1
        print("transport-rejection status=PASS")
        return 0
    if args.command == "transport-command":
        errors = verify_transport_command(
            Path(args.profile_dir).resolve(), Path(args.record).resolve(),
            Path(args.serial).resolve())
        if errors:
            print("transport-command status=FAIL")
            for error in errors:
                print(f"  {error}")
            return 1
        print("transport-command status=PASS")
        return 0
    if args.command == "transport-input":
        errors = verify_transport_input(
            Path(args.profile_dir).resolve(), Path(args.record).resolve())
        if errors:
            print("transport-input status=FAIL")
            for error in errors:
                print(f"  {error}")
            return 1
        print("transport-input status=PASS")
        return 0
    if args.command == "transport-profile":
        errors = verify_transport_profile(
            Path(args.root).resolve(), Path(args.profile_dir).resolve())
        if errors:
            print("transport-profile status=FAIL")
            for error in errors:
                print(f"  {error}")
            return 1
        print("transport-profile status=PASS")
        return 0

    root = Path(args.root)
    if args.command == "production":
        return 1 if verify_production(root) else 0
    if args.command == "diagnostic-negative":
        return 1 if verify_diagnostic_negative(root) else 0
    candidate = root / "candidate" / "kernel.elf"
    if not candidate.is_file():
        print("candidate kernel.elf is missing", file=sys.stderr)
        return 1
    candidate_hash = sha256(candidate)
    focal = args.command == "focal"
    if args.command == "profile":
        targets = [Path(args.profile_dir)]
    else:
        targets = sorted((root / "profiles").glob("*"))
        expected = set(FOCAL_PROFILES if focal else EXPECTED_PROFILES)
        observed = {target.name for target in targets}
        if observed != expected:
            print("profile-matrix status=FAIL missing=" +
                  ",".join(sorted(expected - observed)) + " unexpected=" +
                  ",".join(sorted(observed - expected)))
            return 1
    failed = 0
    focal_results = {}
    for target in targets:
        errors = verify_profile(root, target, candidate_hash, focal=focal)
        if errors:
            failed += 1
            print(f"profile={target.name} status=FAIL")
            for error in errors:
                print(f"  {error}")
        else:
            print(f"profile={target.name} status=PASS")
        if focal:
            commands = read_commands(target / "commands.jsonl")
            focal_results[target.name] = {
                "verifier_errors": errors,
                "commands": [{"payload": row.get("payload"),
                              "handler_status": row.get("handler_status"),
                              "classification": row.get("classification")}
                             for row in commands],
            }
    if args.command == "all":
        rate_errors = verify_rate(root)
        for target in targets:
            if target.name == "q35-kvm-smp24":
                rate_errors.extend(verify_soak(target))
        if rate_errors:
            failed += 1
            print("campaign-derived-status=FAIL")
            for error in rate_errors:
                print(f"  {error}")
        else:
            print("campaign-derived-status=PASS")
    if focal:
        write_focal_summary(root, focal_results)
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
