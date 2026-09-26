#!/usr/bin/env python3
"""Host checks for the framed command encoder and GDB byte decoder."""

import argparse
import importlib.util
import inspect
import json
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parent.parent
MODULE_PATH = ROOT / "scripts" / "foundation-qemu.py"
SPEC = importlib.util.spec_from_file_location("foundation_qemu", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)
VERIFIER_PATH = ROOT / "scripts" / "verify-foundation-regression.py"
VERIFIER_SPEC = importlib.util.spec_from_file_location(
    "verify_foundation_regression", VERIFIER_PATH)
VERIFIER = importlib.util.module_from_spec(VERIFIER_SPEC)
VERIFIER_SPEC.loader.exec_module(VERIFIER)


def independent_crc32(data):
    value = 0xffffffff
    for byte in data:
        value ^= byte
        for _ in range(8):
            value = ((value >> 1) ^ (0xedb88320 if value & 1 else 0))
    return value ^ 0xffffffff


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8")


def write_jsonl(path, values):
    path.write_text("".join(json.dumps(value, sort_keys=True) + "\n"
                            for value in values), encoding="utf-8")


def copy_evidence(source, destination):
    def ignore(_directory, names):
        return [name for name in names
                if name.endswith((".img", ".qcow2", ".iso"))]

    shutil.copytree(source, destination, ignore=ignore)


def command_paths(root):
    profile = root / "profiles" / "q35-kvm-smp24"
    return (profile, profile / "command-1.json",
            profile / "commands.jsonl", profile / "qemu" /
            "qemu-serial.log")


def read_rows(path):
    return [json.loads(line) for line in path.read_text().splitlines()
            if line]


def update_first_row(root, mutate, update_aggregate=True):
    profile, command, commands, _serial = command_paths(root)
    row = json.loads(command.read_text())
    mutate(row, profile)
    write_json(command, row)
    if update_aggregate:
        rows = read_rows(commands)
        rows[0] = row
        write_jsonl(commands, rows)
    return row


def refresh_manifest(row, profile):
    manifest_path = profile / row["qmp_input_manifest"]
    manifest = json.loads(manifest_path.read_text())
    manifest["key_pulses"] = row["qmp_text_events"]
    manifest["resume_checks"] = row["qmp_resume_checks"]
    manifest["enter_events"] = row["qmp_enter_events"]
    manifest["cleanup_checks"] = row["qmp_cleanup_checks"]
    write_json(manifest_path, manifest)
    row["qmp_input_manifest_sha256"] = VERIFIER.sha256(manifest_path)


def refresh_artifact_hash(row, profile, name_field, hash_field):
    row[hash_field] = VERIFIER.sha256(profile / row[name_field])


def run_evidence_fixtures(source, output_dir, check):
    source = source.resolve()
    required = (source / "candidate" / "kernel.elf",
                source / "profiles" / "q35-kvm-smp24" /
                "commands.jsonl")
    check(all(path.is_file() for path in required),
          "evidence-source-complete")
    output_dir.mkdir(parents=True, exist_ok=False)
    results = []

    with tempfile.TemporaryDirectory(prefix="transport-evidence-") as tmp:
        tmp_root = Path(tmp)

        def prepare(name):
            target = tmp_root / name
            copy_evidence(source, target)
            return target

        control = prepare("control")
        profile, command, _commands, serial = command_paths(control)
        command_errors = VERIFIER.verify_transport_command(
            profile, command, serial)
        profile_errors = VERIFIER.verify_transport_profile(control, profile)
        input_errors = VERIFIER.verify_transport_input(profile, command)
        check(not input_errors, "evidence-input-control-accepted")
        check(command_errors ==
              ["command collector classification is not PASS"],
              "evidence-handler-failure-preserved")
        check("handler returned nonzero despite favorable diagnostics" in
              profile_errors, "evidence-profile-handler-failure-preserved")
        baseline_errors = command_errors + profile_errors

        def negative(name, mutate, expected_fragment):
            target = prepare(name)
            profile_path, command_path, _commands_path, serial_path = \
                command_paths(target)
            mutate(target)
            input_result = VERIFIER.verify_transport_input(
                profile_path, command_path)
            command_result = VERIFIER.verify_transport_command(
                profile_path, command_path, serial_path)
            profile_result = VERIFIER.verify_transport_profile(
                target, profile_path)
            combined = input_result + command_result + profile_result
            check(combined != baseline_errors, f"{name}-mutation-detected")
            if not any(expected_fragment in item for item in combined):
                print(f"fixture={name} expected={expected_fragment!r} "
                      f"errors={combined!r}")
            check(any(expected_fragment in item for item in combined),
                  f"{name}-reason")
            results.append({
                "name": name,
                "expected_reason_fragment": expected_fragment,
                "input_errors": input_result,
                "command_errors": command_result,
                "profile_errors": profile_result,
            })

        def missing_qmp(target):
            profile_path, command_path, _commands, _serial = \
                command_paths(target)
            row = json.loads(command_path.read_text())
            (profile_path / row["qmp_input_log"]).unlink()

        negative("qmp-log-missing", missing_qmp,
                 "confirmed delivery QMP log is absent")

        def qmp_response_changed(target):
            def mutate(row, profile_path):
                path = profile_path / row["qmp_input_log"]
                entries = read_rows(path)
                for entry in reversed(entries):
                    message = entry.get("message", {})
                    returned = message.get("return")
                    if (entry.get("direction") == "receive" and
                            isinstance(returned, dict) and
                            returned.get("running") is True):
                        returned["running"] = False
                        returned["status"] = "paused"
                        break
                write_jsonl(path, entries)
                refresh_artifact_hash(row, profile_path, "qmp_input_log",
                                      "qmp_input_log_sha256")
            update_first_row(target, mutate)

        negative("qmp-response-semantic-change", qmp_response_changed,
                 "QMP input response is absent or failed")

        def qmp_physical_response_changed(target):
            def mutate(row, profile_path):
                sample = row["input_delivery_samples"][0]
                request_id = sample["shell_memory_event"]["request_id"]
                path = profile_path / row["qmp_input_log"]
                entries = read_rows(path)
                changed = False
                for entry in entries:
                    message = entry.get("message", {})
                    returned = message.get("return")
                    if (entry.get("direction") == "receive" and
                            message.get("id") == request_id and
                            isinstance(returned, str)):
                        replacement = "0x01" if "0x00" in returned else "0x00"
                        message["return"] = returned.replace(
                            "0x00" if "0x00" in returned else "0x01",
                            replacement, 1)
                        changed = True
                        break
                if not changed:
                    raise AssertionError("physical QMP response not found")
                write_jsonl(path, entries)
                refresh_artifact_hash(row, profile_path, "qmp_input_log",
                                      "qmp_input_log_sha256")
            update_first_row(target, mutate)

        negative("qmp-physical-response-changed",
                 qmp_physical_response_changed,
                 "QMP input response is absent or failed")

        def keyboard_summary_mixed(target):
            def mutate(row, _profile):
                row["input_delivery_samples"][0][
                    "keyboard_active_slots"] = [0]
            update_first_row(target, mutate)

        negative("keyboard-summary-mixed", keyboard_summary_mixed,
                 "QMP keyboard-state summary differs from raw memory")

        def physical_address_zero(target):
            def mutate(row, _profile):
                row["input_symbol_physical_addresses"]["g_buffer"] = \
                    "0x0000000000000000"
            update_first_row(target, mutate)

        negative("physical-address-zero", physical_address_zero,
                 "command input physical-address ledger differs")

        def observer_bytes_changed(target):
            def mutate(row, profile_path):
                path = profile_path / row["input_observer_log"]
                token = row["input_observer_memory_response_token"]
                lines = path.read_text().splitlines(keepends=True)
                prefix = f'{token}^done,'
                for index, line in enumerate(lines):
                    if line.startswith(prefix) and 'contents="' in line:
                        start = line.index('contents="') + len('contents="')
                        replacement = "00" if line[start:start + 2] != "00" \
                            else "01"
                        lines[index] = (line[:start] + replacement +
                                        line[start + 2:])
                        break
                path.write_text("".join(lines))
                refresh_artifact_hash(row, profile_path,
                                      "input_observer_log",
                                      "input_observer_log_sha256")
            update_first_row(target, mutate)

        negative("dispatcher-raw-bytes-changed", observer_bytes_changed,
                 "GDB raw dispatcher line differs from the host frame")

        def no_pre_pulse_checks(target):
            def mutate(row, profile_path):
                row["qmp_text_events"][0]["pre_pulse_runstate_checks"] = []
                refresh_manifest(row, profile_path)
            update_first_row(target, mutate)

        negative("pre-pulse-runstate-missing", no_pre_pulse_checks,
                 "confirmed delivery pre-press runstate observations are absent")

        def non_atomic_stroke(target):
            def mutate(row, profile_path):
                stroke = row["qmp_text_events"][0]["stroke_event"]
                stroke["events"] = stroke["events"][:1]
                refresh_manifest(row, profile_path)
            update_first_row(target, mutate)

        negative("stroke-release-missing", non_atomic_stroke,
                 "confirmed delivery stroke transition differs")

        def no_cleanup_checks(target):
            def mutate(row, profile_path):
                row["qmp_cleanup_checks"] = []
                refresh_manifest(row, profile_path)
            update_first_row(target, mutate)

        negative("cleanup-runstate-missing", no_cleanup_checks,
                 "observer cleanup runstate observations are absent")

        def cleanup_failed(target):
            update_first_row(target, lambda row, _profile:
                             row.__setitem__(
                                 "input_observer_cleanup_return_code", 1))

        negative("observer-cleanup-failed", cleanup_failed,
                 "bounded GDB observer cleanup is incomplete")

        def enter_release_missing(target):
            def mutate(row, profile_path):
                row["qmp_enter_events"][1]["events"] = []
                refresh_manifest(row, profile_path)
            update_first_row(target, mutate)

        negative("enter-release-missing", enter_release_missing,
                 "confirmed delivery terminator-up transition differs")

        def stale_manifest_hash(target):
            profile_path, command_path, commands_path, _serial = \
                command_paths(target)
            row = json.loads(command_path.read_text())
            manifest = profile_path / row["qmp_input_manifest"]
            manifest.write_text(manifest.read_text() + "\n")

        negative("manifest-integrity-stale", stale_manifest_hash,
                 "confirmed delivery manifest hash differs")

        def guest_summary_changed(target):
            def mutate(row, _profile):
                row["guest_input_text"] += "x"
                row["guest_input_bytes_hex"] += "78"
                row["guest_input_length"] += 1
            update_first_row(target, mutate)

        negative("guest-summary-wrong-frame", guest_summary_changed,
                 "dispatcher shell input differs from the host frame")

        def serial_end_missing(target):
            _profile, _command, _commands, serial_path = command_paths(target)
            data = serial_path.read_bytes()
            data = data.replace(b"[HARNESS][END] seq=1 status=1",
                                b"[HARNESS][END_MISSING] seq=1 status=1", 1)
            serial_path.write_bytes(data)

        negative("serial-end-missing", serial_end_missing,
                 "raw serial lacks exactly one END")

        def serial_accept_duplicated(target):
            _profile, _command, _commands, serial_path = command_paths(target)
            data = serial_path.read_bytes()
            marker = b"[HARNESS][FRAME] ACCEPT seq=1 crc=4fa3301b len=9"
            begin = b"[HARNESS][BEGIN] seq=1"
            data = data.replace(begin, marker, 1)
            serial_path.write_bytes(data)

        negative("serial-accept-duplicated", serial_accept_duplicated,
                 "raw serial lacks exactly one ACCEPT")

        def observer_missing(target):
            profile_path, command_path, _commands, _serial = \
                command_paths(target)
            row = json.loads(command_path.read_text())
            (profile_path / row["input_observer_log"]).unlink()

        negative("dispatcher-observer-missing", observer_missing,
                 "input observer artifact is absent")

        def qmp_record_missing(target):
            def mutate(row, profile_path):
                path = profile_path / row["qmp_input_log"]
                entries = read_rows(path)
                del entries[-1]
                write_jsonl(path, entries)
                refresh_artifact_hash(row, profile_path, "qmp_input_log",
                                      "qmp_input_log_sha256")
            update_first_row(target, mutate)

        negative("qmp-raw-record-missing", qmp_record_missing,
                 "QMP input response is absent or failed")

        def dispatcher_other_transaction(target):
            def mutate(row, profile_path):
                path = profile_path / row["input_observer_log"]
                token = row["input_observer_memory_response_token"]
                expected = bytes.fromhex(row["guest_input_bytes_hex"])
                replacement = bytearray(expected)
                replacement[-1] = ord("x")
                lines = path.read_text().splitlines(keepends=True)
                prefix = f'{token}^done,'
                for index, line in enumerate(lines):
                    if line.startswith(prefix) and 'contents="' in line:
                        start = line.index('contents="') + len('contents="')
                        content_end = line.index('"', start)
                        raw = bytes.fromhex(line[start:content_end])
                        changed = bytes(replacement) + raw[len(replacement):]
                        lines[index] = (line[:start] + changed.hex() +
                                        line[content_end:])
                        break
                path.write_text("".join(lines))
                row["guest_input_bytes_hex"] = bytes(replacement).hex()
                row["guest_input_text"] = bytes(replacement).decode()
                refresh_artifact_hash(row, profile_path,
                                      "input_observer_log",
                                      "input_observer_log_sha256")
            update_first_row(target, mutate)

        negative("dispatcher-other-transaction", dispatcher_other_transaction,
                 "GDB raw dispatcher line differs from the host frame")

        def aggregate_negative(name, mutate, expected_fragment):
            target = prepare(name)
            profile_path, _command, _commands, _serial = command_paths(target)
            mutate(target)
            errors = VERIFIER.verify_transport_profile(target, profile_path)
            check(bool(errors), f"{name}-aggregate-rejected")
            if not any(expected_fragment in item for item in errors):
                print(f"fixture={name} expected={expected_fragment!r} "
                      f"errors={errors!r}")
            check(any(expected_fragment in item for item in errors),
                  f"{name}-aggregate-reason")
            results.append({
                "name": name,
                "expected_reason_fragment": expected_fragment,
                "profile_errors": errors,
            })

        def missing_command(target):
            _profile, _command, commands, _serial = command_paths(target)
            rows = read_rows(commands)
            write_jsonl(commands, rows[:-1])

        aggregate_negative("aggregate-command-missing", missing_command,
                           "transport profile command coverage or order differs")

        def wrong_candidate(target):
            _profile, _command, commands, _serial = command_paths(target)
            rows = read_rows(commands)
            rows[0]["candidate_sha256"] = "0" * 64
            write_jsonl(commands, rows)

        aggregate_negative("aggregate-candidate-divergent", wrong_candidate,
                           "command refers to another candidate")

        def wrong_observation(target):
            profile_path, _command, _commands, _serial = command_paths(target)
            path = profile_path / "profile.env"
            path.write_text(path.read_text().replace(
                "command_input_observation="
                "qmp-stable-physical-atomic-stroke-confirmed-bounded-stopped-detach-observer-gdb",
                "command_input_observation=qmp-key-pulse-pre-enter-and-dispatcher-gdb"))

        aggregate_negative("aggregate-observation-profile-divergent",
                           wrong_observation,
                           "command record schema is unsupported for the profile")

        def wrong_launch_kernel(target):
            profile_path, _command, _commands, _serial = command_paths(target)
            path = profile_path / "qemu" / "launch.env"
            text = path.read_text()
            text = __import__("re").sub(r"(?m)^KERNEL_SHA256=.*$",
                                       "KERNEL_SHA256=" + "0" * 64, text)
            path.write_text(text)

        aggregate_negative("aggregate-launch-kernel-divergent",
                           wrong_launch_kernel,
                           "launch kernel hash differs")

        def duplicate_sequence(target):
            _profile, _command, commands, _serial = command_paths(target)
            rows = read_rows(commands)
            rows.append(dict(rows[0]))
            write_jsonl(commands, rows)

        aggregate_negative("aggregate-sequence-duplicate", duplicate_sequence,
                           "transport command sequence is incomplete or reordered")

    source_inventory = []
    for path in sorted(source.rglob("*")):
        if path.is_file() and not path.name.endswith((".img", ".qcow2", ".iso")):
            source_inventory.append({
                "path": path.relative_to(source).as_posix(),
                "bytes": path.stat().st_size,
                "sha256": VERIFIER.sha256(path),
            })
    write_json(output_dir / "result.json", {
        "schema": 1,
        "kind": "command-transport-evidence-fixtures",
        "source": str(source),
        "source_inventory": source_inventory,
        "valid_control": {
            "input_errors": input_errors,
            "command_errors": command_errors,
            "profile_errors": profile_errors,
        },
        "negative_count": len(results),
        "negatives": results,
    })
    (output_dir / "source-sha256.txt").write_text(
        "".join(f'{item["sha256"]}  {item["path"]}\n'
                for item in source_inventory), encoding="utf-8")
    print("COMMAND_TRANSPORT_EVIDENCE_FIXTURES: PASS "
          f"negatives={len(results)} source={source}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("all", "encoder", "observer",
                                         "evidence"),
                        nargs="?", default="all")
    parser.add_argument("--evidence-root", type=Path)
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    checks = 0

    def check(condition, name):
        nonlocal checks
        checks += 1
        if not condition:
            raise AssertionError(name)
        print(f"check={name} status=PASS")

    def rejected(call, name):
        try:
            call()
        except (ValueError, RuntimeError):
            check(True, name)
        else:
            check(False, name)

    if args.mode in ("all", "encoder"):
        with tempfile.TemporaryDirectory(prefix="transport-lines-") as tmp:
            serial = Path(tmp) / "serial.log"
            serial.write_bytes(b"alpha\r\nomega\r")
            lines, ends, total = MODULE.complete_lines(serial)
            check(lines == ["alpha"] and ends == [7] and total == 13,
                  "serial-bare-cr-is-incomplete")
            with serial.open("ab") as stream:
                stream.write(b"\n")
            lines, ends, total = MODULE.complete_lines(serial)
            check(lines == ["alpha", "omega"] and ends == [7, 14] and
                  total == 14,
                  "serial-crlf-boundary-is-atomic")

        payload = "synctest timer-cancel"
        crc, frame = MODULE.build_command_frame(12, payload)
        check(crc == f"{independent_crc32(payload.encode()):08x}",
              "crc-independent-oracle")
        check(crc == "4dde9fd7", "crc-known-command")
        check(frame == "tasktest exec 12 4dde9fd7 synctest timer-cancel",
              "frame-exact-bytes")
        check(len(frame.encode()) == 47, "frame-byte-length")
        max_payload = "a" * 96
        max_crc, max_frame = MODULE.build_command_frame(
            0xffffffffffffffff, max_payload)
        check(len(max_payload) == 96 and len(max_crc) == 8,
              "payload-maximum")
        check(len(max_frame.encode()) < 256, "maximum-frame-fits")
        for value, name in ((" leading", "leading-space"),
                            ("trailing ", "trailing-space"),
                            ("two  spaces", "double-space"),
                            ("tab\tvalue", "tab"),
                            ("line\nvalue", "newline"),
                            ("quoted'value", "quote"),
                            ("upper", "valid-lower-control")):
            if name == "valid-lower-control":
                check(MODULE.build_command_frame(1, value)[1].endswith(value),
                      name)
            else:
                rejected(lambda value=value:
                         MODULE.build_command_frame(1, value), name)
        rejected(lambda: MODULE.build_command_frame(1, "a" * 97),
                 "payload-too-long")
        rejected(lambda: MODULE.build_command_frame(1, "bad/character"),
                 "unsupported-character")
        rejected(lambda: MODULE.build_command_frame(
            1, "tasktest exec 1 00000000 ps"), "recursive-frame")
        for sequence, name in ((0, "sequence-zero"),
                               (-1, "sequence-negative"),
                               (0x10000000000000000, "sequence-overflow"),
                               (True, "sequence-bool"),
                               ("1", "sequence-type")):
            rejected(lambda sequence=sequence:
                     MODULE.build_command_frame(sequence, "irq check"), name)

    if args.mode in ("all", "observer"):
        check(MODULE.INPUT_DELIVERY_TIMEOUT_SECONDS == 90.0 and
              MODULE.INPUT_DELIVERY_STABILITY_SECONDS == 0.5 and
              MODULE.INPUT_DELIVERY_SAMPLE_DELAY_SECONDS == 0.05 and
              MODULE.INPUT_KEY_PRESS_TIMEOUT_SECONDS == 5.0 and
              MODULE.INPUT_KEY_RELEASE_TIMEOUT_SECONDS == 5.0,
              "qmp-confirmed-delivery-policy")
        key_down = MODULE.qmp_key_events("a", True)
        key_up = MODULE.qmp_key_events("a", False)
        check(key_down == [{"type": "key", "data": {
                  "down": True,
                  "key": {"type": "qcode", "data": "a"}}}],
              "qmp-letter-down-exact")
        check(key_up == [{"type": "key", "data": {
                  "down": False,
                  "key": {"type": "qcode", "data": "a"}}}],
              "qmp-letter-up-exact")
        check(MODULE.qmp_key_events("+", True) == [
                  {"type": "key", "data": {"down": True,
                   "key": {"type": "qcode", "data": "shift"}}},
                  {"type": "key", "data": {"down": True,
                   "key": {"type": "qcode", "data": "equal"}}}],
              "qmp-shifted-key-down-order")
        check(MODULE.qmp_key_events("+", False) == [
                  {"type": "key", "data": {"down": False,
                   "key": {"type": "qcode", "data": "equal"}}},
                  {"type": "key", "data": {"down": False,
                   "key": {"type": "qcode", "data": "shift"}}}],
              "qmp-shifted-key-up-order")
        rejected(lambda: MODULE.qmp_key_events("/", True),
                 "qmp-unsupported-key")
        class StatusQmp:
            def __init__(self, values):
                self.values = iter(values)
                self.calls = 0

            def command(self, name, arguments, deadline):
                self.calls += 1
                value = next(self.values)
                return f"status-{self.calls}", {
                    "id": f"status-{self.calls}", "return": value}

        status_qmp = StatusQmp((
            {"running": False, "singlestep": False, "status": "paused"},
            {"running": True, "singlestep": False, "status": "running"},
        ))
        resume_checks = MODULE.wait_qmp_running(
            status_qmp, time.monotonic() + 1.0)
        check(status_qmp.calls == 2 and len(resume_checks) == 2,
              "qmp-resume-waits-for-running")
        check(resume_checks[-1]["response_return"]["running"] is True and
              resume_checks[-1]["qmp_execute"] == "query-status",
              "qmp-resume-ledger")
        original_resume_timeout = MODULE.INPUT_RESUME_TIMEOUT_SECONDS
        original_resume_poll = MODULE.INPUT_RESUME_POLL_SECONDS
        MODULE.INPUT_RESUME_TIMEOUT_SECONDS = 0.01
        MODULE.INPUT_RESUME_POLL_SECONDS = 0.001
        try:
            rejected(lambda: MODULE.wait_qmp_running(
                StatusQmp(({"running": False, "status": "paused"}
                           for _ in range(10000))),
                time.monotonic() + 1.0), "qmp-resume-timeout")
        finally:
            MODULE.INPUT_RESUME_TIMEOUT_SECONDS = original_resume_timeout
            MODULE.INPUT_RESUME_POLL_SECONDS = original_resume_poll

        observer_source = inspect.getsource(MODULE.observe_framed_input)
        snapshot_source = inspect.getsource(MODULE.qmp_input_snapshot)
        frame_source = inspect.getsource(MODULE.command_frame)
        text_stroke = "stroke_event = qmp.input("
        enter_down = "enter_down = dict(qmp.input("
        enter_up = "enter_up = dict(qmp.input("
        check(observer_source.count("qmp_key_events(character, True)") == 1 and
              observer_source.count("qmp_key_events(character, False)") == 1 and
              observer_source.count("qmp_key_events(\"ret\", True)") == 1 and
              observer_source.count("qmp_key_events(\"ret\", False)") == 1,
              "qmp-each-key-state-is-sent-once")
        check(observer_source.index("runstate_checks = wait_qmp_running") <
              observer_source.index(text_stroke) <
              observer_source.index('"phase": "delivery"'),
              "qmp-text-uses-one-atomic-stroke-before-observation")
        check(observer_source.index('"-exec-continue"') <
              observer_source.index("resume_checks = wait_qmp_running") <
              observer_source.index(enter_down) <
              observer_source.index("mi_wait_stopped") <
              observer_source.index("detach_stopped_gdb_observer") <
              observer_source.index("cleanup_checks = wait_qmp_running") <
              observer_source.index(enter_up) <
              observer_source.rindex("qmp_input_snapshot"),
              "qmp-enter-release-follows-dispatch-observation")
        check("shell_input_snapshot" not in observer_source and
              observer_source.count("qmp_input_snapshot") == 4,
              "qmp-physical-memory-observer-replaces-gdb-sampling")
        check(snapshot_source.index("shell_before") <
              snapshot_source.index("keyboard_data") <
              snapshot_source.index("shell_after") and
              "shell_snapshot_stable = shell_before == shell_after" in
              snapshot_source,
              "qmp-shell-snapshot-brackets-keyboard-observation")
        check("send_qmp_key_pulse" not in observer_source and
              "send_qmp_key_stroke" not in observer_source,
              "qmp-fixed-pulse-path-is-absent")
        check("cleanup_started + INPUT_OBSERVER_CLEANUP_TIMEOUT_SECONDS" in
              observer_source and
              MODULE.INPUT_OBSERVER_CLEANUP_TIMEOUT_SECONDS == 5.0,
              "gdb-private-observer-cleanup-is-bounded")
        check('hmp_socket = runtime / "hmp.sock"' in frame_source and
              frame_source.count("run_hmp(root, hmp_socket") == 2 and
              '"normal", output_path=esc_log' in frame_source,
              "modal-output-uses-explicit-hmp-socket")
        with tempfile.TemporaryDirectory(prefix="transport-qmp-") as tmp:
            socket_path = Path(tmp) / "qmp.sock"
            log_path = Path(tmp) / "qmp.jsonl"
            listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            listener.bind(str(socket_path))
            listener.listen(1)
            captured = []

            def serve_qmp():
                connection, _ = listener.accept()
                with connection:
                    connection.sendall((json.dumps({
                        "QMP": {"version": {"qemu": {"major": 8}}}
                    }) + "\r\n").encode())
                    stream = connection.makefile("rb")
                    for _ in range(3):
                        request = json.loads(stream.readline())
                        captured.append(request)
                        connection.sendall((json.dumps({
                            "return": {}, "id": request["id"]
                        }) + "\r\n").encode())
                listener.close()

            server = threading.Thread(target=serve_qmp)
            server.start()
            client = MODULE.QmpInputClient(
                socket_path, log_path, time.monotonic() + 2.0)
            down_event = client.input(key_down, time.monotonic() + 2.0)
            up_event = client.input(key_up, time.monotonic() + 2.0)
            client.close()
            server.join(timeout=2.0)
            check(not server.is_alive(), "qmp-test-server-completed")
            check(captured[0]["execute"] == "qmp_capabilities",
                  "qmp-capabilities-request")
            check(captured[1]["execute"] == "input-send-event" and
                  captured[1]["arguments"]["events"] == key_down and
                  captured[2]["execute"] == "input-send-event" and
                  captured[2]["arguments"]["events"] == key_up,
                  "qmp-input-state-requests-exact")
            check(down_event["request_id"] == captured[1]["id"] and
                  down_event["events"] == key_down and
                  up_event["request_id"] == captured[2]["id"] and
                  up_event["events"] == key_up,
                  "qmp-input-state-ledger")
            raw_entries = [json.loads(line) for line in
                           log_path.read_text().splitlines()]
            check([entry["direction"] for entry in raw_entries] ==
                  ["receive", "send", "receive", "send", "receive",
                   "send", "receive"],
                  "qmp-raw-direction-order")
        original_mi_command = MODULE.mi_command
        dispatch_order = []

        def fake_mi_command(process, token, command, deadline, output,
                            commands):
            dispatch_order.append(("gdb", command))
            return f"{token}^running"

        MODULE.mi_command = fake_mi_command
        try:
            next_token, resumed = MODULE.resume_dispatch(
                object(), 7, time.monotonic() + 1.0, [], [])
        finally:
            MODULE.mi_command = original_mi_command
        check(dispatch_order == [("gdb", "-exec-continue")],
              "dispatcher-resume-order")
        check(next_token == 8 and isinstance(resumed, float),
              "dispatcher-resume-ledger")

        class FakeProcess:
            def __init__(self):
                self.waited = False
                self.killed = False

            def poll(self):
                return None

            def wait(self, timeout):
                self.waited = 0 < timeout <= 1.0
                return 0

            def kill(self):
                self.killed = True

        process = FakeProcess()
        cleanup_commands = []
        cleanup_output = []
        original_reader_finish = MODULE.mi_reader_finish
        original_mi_command = MODULE.mi_command
        finish_order = []
        MODULE.mi_reader_finish = lambda process, output: finish_order.append(
            "reader-finished")

        def fake_cleanup_command(process, token, command, deadline, output,
                                 commands):
            cleanup_commands.append((token, command))
            commands.append(f"{token}{command}")
            response = (f"{token}^exit" if command == "-gdb-exit" else
                        f"{token}^done")
            output.append(response + "\n")
            return response

        MODULE.mi_command = fake_cleanup_command
        try:
            next_token, cleanup = MODULE.detach_stopped_gdb_observer(
                process, 12, time.monotonic() + 1.0, cleanup_output, [])
        finally:
            MODULE.mi_reader_finish = original_reader_finish
            MODULE.mi_command = original_mi_command
        check(cleanup_commands == [(12, "-target-detach"),
                                   (13, "-gdb-exit")] and
              finish_order == ["reader-finished"] and not process.killed,
              "gdb-private-observer-stopped-detach-order")
        detached_time = cleanup.pop("input_observer_target_detached_monotonic")
        check(next_token == 14 and process.waited and
              isinstance(detached_time, float) and cleanup == {
                  "input_observer_cleanup_method":
                      "detach-stopped-private-observer",
                  "input_observer_cleanup_return_code": 0,
                  "input_observer_cleanup_detach_response_token": 12,
                  "input_observer_cleanup_exit_response_token": 13,
              }, "gdb-private-observer-stopped-detach-ledger")
        expected = b"tasktest exec 1 00000000 irq check"
        check(MODULE.classify_input_delivery(
            1, 0, 0, True, b"", expected) == "PREFIX",
            "bounded-delivery-empty-prefix")
        check(MODULE.classify_input_delivery(
            1, 20, 20, True, expected[:20], expected) == "PREFIX",
            "bounded-delivery-progress-prefix")
        check(MODULE.classify_input_delivery(
            1, len(expected), len(expected), True, expected, expected) ==
            "EXACT", "bounded-delivery-exact")
        check(MODULE.classify_input_delivery(
            1, 5, 5, True, b"wrong", expected) == "DIVERGED",
            "bounded-delivery-wrong-bytes")
        check(MODULE.classify_input_delivery(
            0, len(expected), len(expected), True, expected, expected) ==
            "DIVERGED", "bounded-delivery-inactive")
        check(MODULE.classify_input_delivery(
            1, len(expected), len(expected) - 1, True, expected, expected) ==
            "DIVERGED", "bounded-delivery-incoherent-position")
        check(MODULE.classify_input_delivery(
            1, len(expected), len(expected), False, expected, expected) ==
            "DIVERGED", "bounded-delivery-missing-nul")
        check(MODULE.classify_input_delivery(
            1, len(expected), len(expected), True, expected, expected,
            False) == "UNSTABLE", "bounded-delivery-torn-snapshot")
        first_target = expected[:1]
        check(MODULE.classify_input_delivery(
            1, 1, 1, True, first_target, first_target) == "EXACT",
            "character-first-byte-exact")
        check(MODULE.classify_input_delivery(
            1, 2, 2, True, first_target * 2, first_target) == "DIVERGED",
            "character-repeat-before-release-rejected")
        second_target = expected[:2]
        check(MODULE.classify_input_delivery(
            1, 1, 1, True, first_target, second_target) == "PREFIX",
            "character-await-without-resend")
        check(MODULE.classify_input_delivery(
            1, 2, 2, True, second_target, second_target) == "EXACT",
            "character-next-byte-exact")
        check(MODULE.classify_cursor_delivery(
            1, 2, 2, True, second_target, second_target, 2, 1) ==
              "PENDING", "cursor-left-await")
        check(MODULE.classify_cursor_delivery(
            1, 2, 1, True, second_target, second_target, 2, 1) ==
              "EXACT", "cursor-left-observed")
        check(MODULE.classify_cursor_delivery(
            1, 2, 1, True, second_target, second_target, 1, 2) ==
              "PENDING", "cursor-right-await")
        check(MODULE.classify_cursor_delivery(
            1, 2, 2, True, second_target, second_target, 1, 2) ==
              "EXACT", "cursor-right-observed")
        check(MODULE.classify_cursor_delivery(
            1, 3, 2, True, second_target + b"x", second_target, 2, 1) ==
              "DIVERGED", "cursor-boundary-extra-byte-rejected")
        frame_args = MODULE.build_parser().parse_args([
            "frame", "--root", "/r", "--runtime", "/v",
            "--gdb-socket", "/g", "--elf", "/e", "--input-layout", "/l",
            "--record", "/o",
            "--transport-log", "/t", "--profile-id", "p", "--vm-id", "v",
            "--candidate-sha256", "0" * 64, "--sequence", "1",
            "--payload", "irq check", "--timeout", "1",
            "--input-profile", "framed"])
        check(frame_args.input_profile == "framed",
              "framed-input-profile-accepted")
        response = ('9^done,memory=[{begin="0x1",offset="0",end="0x5",'
                    'contents="74657374"}]')
        check(MODULE.mi_memory_bytes(response, 4) == b"test",
              "observer-exact-memory")
        rejected(lambda: MODULE.mi_memory_bytes(response, 3),
                 "observer-size-mismatch")
        rejected(lambda: MODULE.mi_memory_bytes("9^done", 4),
                 "observer-contents-missing")
        rejected(lambda: MODULE.mi_memory_bytes(
            '9^done,memory=[{contents="xyz"}]', 1),
                 "observer-nonhex")
        hmp_bytes = ("0000000000001230: 0x74 0x65 0x73 0x74\r\n"
                     "0000000000001234: 0x00 0xff")
        check(MODULE.hmp_physical_bytes(hmp_bytes, 0x1230, 6) ==
              b"test\x00\xff", "qmp-physical-bytes-exact")
        check(VERIFIER.decode_hmp_physical_bytes(hmp_bytes, 0x1230, 6) ==
              b"test\x00\xff", "verifier-physical-bytes-independent")
        rejected(lambda: MODULE.hmp_physical_bytes(
            hmp_bytes.replace("0000000000001234", "0000000000001235"),
            0x1230, 6), "qmp-physical-address-gap")
        rejected(lambda: MODULE.hmp_physical_bytes(hmp_bytes, 0x1230, 5),
                 "qmp-physical-size-mismatch")
        with tempfile.TemporaryDirectory(prefix="transport-layout-") as tmp:
            binary = Path(tmp) / "layout"
            layout_path = Path(tmp) / "layout.json"
            compiled = subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I",
                str(ROOT), str(ROOT / "scripts" /
                               "command-transport-layout.c"),
                "-o", str(binary)], capture_output=True, text=True,
                check=False)
            check(compiled.returncode == 0,
                  "input-layout-real-header-build")
            executed = subprocess.run([str(binary)], capture_output=True,
                                      text=True, check=False)
            layout_path.write_text(executed.stdout, encoding="utf-8")
            loaded_path, layout = MODULE.load_input_layout(layout_path)
            check(executed.returncode == 0 and loaded_path == layout_path and
                  layout["slot_count"] == 64 and
                  layout["prev_keys_size"] == 384 and
                  layout["repeat_active_size"] == 64,
                  "input-layout-real-header-values")

            shell_virtual = {
                "g_buffer": 0x100000,
                "g_len": 0x100100,
                "g_pos": 0x100104,
                "g_shell_active": 0x100108,
                "xhci_driver": 0x200000,
            }
            shell_physical = {
                "g_buffer": 0x300000,
                "g_len": 0x300100,
                "g_pos": 0x300104,
                "g_shell_active": 0x300108,
                "xhci_driver": 0x400000,
            }
            shell_data = bytearray(265)
            shell_data[:4] = b"test"
            shell_data[4] = 0
            shell_data[256:260] = (4).to_bytes(4, "little", signed=True)
            shell_data[260:264] = (4).to_bytes(4, "little", signed=True)
            shell_data[264] = 1
            keyboard_start = min(layout[item[1]]
                                 for item in MODULE.INPUT_LAYOUT_ARRAYS)
            keyboard_end = max(layout[item[1]] + layout[item[2]]
                               for item in MODULE.INPUT_LAYOUT_ARRAYS)
            keyboard_data = bytearray(keyboard_end - keyboard_start)
            keyboard_data[0] = 0x17

            class MemoryQmp:
                def __init__(self, regions):
                    self.regions = regions
                    self.calls = 0
                    self.region_calls = {}

                def command(self, name, arguments, _deadline):
                    check(name == "human-monitor-command",
                          "qmp-memory-command-kind")
                    match = __import__("re").fullmatch(
                        r"xp /([1-9][0-9]*)bx 0x([0-9a-f]+)",
                        arguments["command-line"])
                    if match is None:
                        raise RuntimeError("unexpected test HMP command")
                    size, address = int(match.group(1)), int(match.group(2), 16)
                    key = (address, size)
                    data = self.regions[key]
                    if isinstance(data, list):
                        index = self.region_calls.get(key, 0)
                        data = data[min(index, len(data) - 1)]
                        self.region_calls[key] = index + 1
                    lines = []
                    for offset in range(0, size, 16):
                        chunk = data[offset:offset + 16]
                        lines.append(f"{address + offset:016x}:" +
                                     "".join(f" 0x{byte:02x}" for byte in chunk))
                    self.calls += 1
                    return f"memory-{self.calls}", {
                        "return": "\r\n".join(lines)}

            shell_size = shell_virtual["g_shell_active"] + 1 - \
                shell_virtual["g_buffer"]
            keyboard_address = shell_physical["xhci_driver"] + keyboard_start
            memory_qmp = MemoryQmp({
                (shell_physical["g_buffer"], shell_size): shell_data,
                (keyboard_address, len(keyboard_data)): keyboard_data,
            })
            memory_sample = MODULE.qmp_input_snapshot(
                memory_qmp, shell_virtual, shell_physical, layout,
                time.monotonic() + 1.0)
            check(memory_sample["observed_bytes_hex"] == b"test".hex() and
                  memory_sample["shell_snapshot_stable"] is True and
                  memory_sample["keyboard_active_slots"] == [0] and
                  memory_sample["keyboard_release_observed"] is False,
                  "qmp-memory-snapshot-derived-values")
            verifier_records = []
            verifier_errors = []
            verifier_values = VERIFIER.qmp_snapshot_values(
                memory_sample, shell_virtual, shell_physical, layout,
                verifier_records, verifier_errors)
            check(not verifier_errors and verifier_values[:5] ==
                  (1, 4, 4, True, b"test") and
                  verifier_values[7] is True and len(verifier_records) == 3,
                  "qmp-memory-snapshot-verifier-raw-reconciliation")
            torn_shell = bytearray(shell_data)
            torn_shell[3] = 0
            torn_qmp = MemoryQmp({
                (shell_physical["g_buffer"], shell_size):
                    [torn_shell, shell_data],
                (keyboard_address, len(keyboard_data)): keyboard_data,
            })
            torn_sample = MODULE.qmp_input_snapshot(
                torn_qmp, shell_virtual, shell_physical, layout,
                time.monotonic() + 1.0)
            torn_records = []
            torn_errors = []
            torn_values = VERIFIER.qmp_snapshot_values(
                torn_sample, shell_virtual, shell_physical, layout,
                torn_records, torn_errors)
            check(torn_sample["shell_snapshot_stable"] is False and
                  torn_sample["observed_bytes_hex"] == b"test".hex() and
                  not torn_errors and torn_values[7] is False and
                  len(torn_records) == 3,
                  "qmp-torn-shell-snapshot-is-explicit")
            memory_sample["observed_bytes_hex"] = b"best".hex()
            verifier_errors = []
            VERIFIER.qmp_snapshot_values(
                memory_sample, shell_virtual, shell_physical, layout, [],
                verifier_errors)
            check("QMP shell summary differs from raw memory" in verifier_errors,
                  "qmp-memory-summary-mutation-rejected")

            invalid = dict(layout, prev_keys_size=383)
            layout_path.write_text(json.dumps(invalid), encoding="utf-8")
            rejected(lambda: MODULE.load_input_layout(layout_path),
                     "input-layout-size-mismatch")

            tokens = {name: index + 1 for index, (name, _offset, _size) in
                      enumerate(MODULE.INPUT_LAYOUT_ARRAYS)}
            raw_lines = []
            commands = []
            xhci_address = 0x100000
            for name, offset_name, size_name in MODULE.INPUT_LAYOUT_ARRAYS:
                size = layout[size_name]
                token = tokens[name]
                raw_lines.append(
                    f'{token}^done,memory=[{{contents="{"00" * size}"}}]\n')
                commands.append(
                    f'{token}-data-read-memory-bytes 0x'
                    f'{xhci_address + layout[offset_name]:x} {size}')
            sample = {
                "keyboard_response_tokens": tokens,
                "keyboard_state_hex": {
                    name: "00" * layout[size_name]
                    for name, _offset, size_name in MODULE.INPUT_LAYOUT_ARRAYS
                },
                "keyboard_active_slots": [],
                "keyboard_release_observed": True,
            }
            check(not VERIFIER.keyboard_sample_errors(
                sample, "".join(raw_lines), "\n".join(commands),
                {"xhci_driver": xhci_address}, layout),
                "keyboard-release-derived-from-raw-gdb")
            sample["keyboard_release_observed"] = False
            check("keyboard release summary differs from GDB" in
                  VERIFIER.keyboard_sample_errors(
                      sample, "".join(raw_lines), "\n".join(commands),
                      {"xhci_driver": xhci_address}, layout),
                  "keyboard-release-summary-mismatch-rejected")
        with tempfile.TemporaryDirectory(prefix="transport-symbols-") as tmp:
            source = Path(tmp) / "symbols.c"
            obj = Path(tmp) / "symbols.o"
            source.write_text(
                "char g_shell_active; char g_buffer[256]; "
                "int g_len; int g_pos;\n", encoding="utf-8")
            compiled = subprocess.run(
                ["cc", "-c", str(source), "-o", str(obj)], check=False,
                capture_output=True, text=True)
            check(compiled.returncode == 0, "observer-symbol-probe-build")
            symbols = MODULE.elf_symbol_addresses(obj,
                                                   MODULE.SHELL_INPUT_SYMBOLS)
            check(set(symbols) == set(MODULE.SHELL_INPUT_SYMBOLS),
                  "observer-symbol-inventory")
            check(len(set(symbols.values())) == 4,
                  "observer-symbol-addresses-distinct")

    if args.mode == "evidence":
        if args.evidence_root is None or args.output_dir is None:
            parser.error("evidence mode requires --evidence-root and --output-dir")
        run_evidence_fixtures(args.evidence_root, args.output_dir, check)

    print(f"COMMAND_TRANSPORT_HOST: PASS mode={args.mode} checks={checks}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
