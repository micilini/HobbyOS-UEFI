#!/usr/bin/env python3
"""Run one assertion/list scenario in an isolated QEMU virtual machine."""

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time


SCRIPT_DIR = Path(__file__).resolve().parent
PANIC_CONTROLLER = SCRIPT_DIR / "panic-qemu.py"
spec = importlib.util.spec_from_file_location("hobbyos_panic_qemu",
                                              PANIC_CONTROLLER)
panic_qemu = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(panic_qemu)


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
UNCHANGED_FIXTURE = {
    "warn", "false", "double-insert", "insert-reciprocity",
    "remove-reciprocity", "bug", "locked-bug", "reentry",
    "second-reentry",
}

STATE_OFFSETS = {
    "armed": 0,
    "scenario": 4,
    "trigger_slot": 8,
    "queued": 12,
    "prepared": 16,
    "release": 20,
    "completed": 24,
    "result": 28,
    "observed_slot": 32,
    "observed_cpu_id": 36,
    "if_before": 40,
    "if_after": 44,
    "evaluations": 48,
    "continuation": 52,
    "lock_acquired": 56,
    "arm_error": 60,
    "lock_addr": 64,
    "fixture_addr": 72,
    "fixture_size": 80,
    "task_id": 88,
    "task_generation": 96,
    "context_ready": 104,
    "context_release": 108,
    "context_cpu_id": 112,
    "context_slot": 116,
    "context_valid_mask": 120,
    "context_reserved": 124,
    "context_task_id": 128,
    "context_task_generation": 136,
}
PANIC_STATE_SCENARIO = 4
PANIC_STATE_REENTRY_FAULTS = 60
MAX_CPUS = 32
SCHEDULER_CURRENT_OFFSET = 8
TASK_ID_OFFSET = 8
TASK_GENERATION_OFFSET = 168


def atomic_json(path, value):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")
    os.replace(temporary, path)


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def set_u32(address, value):
    return f"set {{unsigned int}}0x{address:x} = {value}"


def set_u64(address, value):
    return f"set {{unsigned long long}}0x{address:x} = {value}"


def state_address(base, field):
    return base + STATE_OFFSETS[field]


def marker_value(text, name, base=10):
    pattern = re.compile(rf"^{re.escape(name)}=([^\s]+)$", re.MULTILINE)
    match = pattern.search(text)
    if not match:
        return None
    try:
        return int(match.group(1), base)
    except ValueError:
        return None


def arm_commands(symbols, scenario, trigger_slot, lock_name):
    state = symbols["g_assertion_test_state"]["address"]
    commands = []
    for field in (
            "armed", "scenario", "trigger_slot", "queued", "prepared",
            "release", "completed", "result", "observed_slot",
            "observed_cpu_id", "if_before", "if_after", "evaluations",
            "continuation", "lock_acquired", "arm_error", "context_ready",
            "context_release", "context_cpu_id", "context_slot",
            "context_valid_mask", "context_reserved"):
        commands.append(set_u32(state_address(state, field), 0))
    for field in ("lock_addr", "fixture_addr", "fixture_size", "task_id",
                  "task_generation", "context_task_id",
                  "context_task_generation"):
        commands.append(set_u64(state_address(state, field), 0))
    commands.extend([
        set_u32(state_address(state, "scenario"), SCENARIOS[scenario]),
        set_u32(state_address(state, "trigger_slot"), trigger_slot),
    ])
    if lock_name:
        commands.append(set_u64(state_address(state, "lock_addr"),
                                symbols[lock_name]["address"]))
    if scenario in ("reentry", "second-reentry"):
        panic_state = symbols["g_panic_test_state"]["address"]
        panic_scenario = 4 if scenario == "reentry" else 5
        commands.extend([
            set_u32(panic_state + PANIC_STATE_SCENARIO, panic_scenario),
            set_u32(panic_state + PANIC_STATE_REENTRY_FAULTS, 0),
        ])
    commands.extend([
        set_u32(state_address(state, "armed"), 1),
        f'printf "ASSERT_ARMED=%u\\n", *(unsigned int*)0x{state:x}',
        f'printf "ASSERT_SCENARIO=%u\\n", '
        f'*(unsigned int*)0x{state_address(state, "scenario"):x}',
    ])
    return commands


def observer_commands(symbols, before_path, has_lock):
    state = symbols["g_assertion_test_state"]["address"]
    fixture = symbols["g_assertion_test_fixture"]
    scheduler = symbols["g_scheduler_cpus"]
    topology = symbols["g_cpus"]
    scheduler_stride = scheduler["size"] // MAX_CPUS
    topology_stride = topology["size"] // MAX_CPUS
    fixture_start = fixture["address"]
    fixture_end = fixture_start + fixture["size"]
    slot_address = state_address(state, "observed_slot")
    commands = [
        f"set $assert_slot = *(unsigned int*)0x{slot_address:x}",
        f"set $sched_stride = {scheduler_stride}",
        f"set $topology_stride = {topology_stride}",
        f"set $current = *(unsigned long long*)(0x{scheduler['address']:x} + "
        "$assert_slot * $sched_stride + 8)",
        'printf "STATE_OBSERVED_SLOT=%u\\n", $assert_slot',
        f'printf "STATE_OBSERVED_CPU_ID=%u\\n", '
        f'*(unsigned int*)0x{state_address(state, "observed_cpu_id"):x}',
        f'printf "TOPOLOGY_CPU_ID=%u\\n", '
        f'*(unsigned int*)(0x{topology["address"]:x} + '
        "$assert_slot * $topology_stride + 4)",
        f'printf "STATE_TASK_ID=%lu\\n", '
        f'*(unsigned long long*)0x{state_address(state, "task_id"):x}',
        f'printf "STATE_TASK_GENERATION=%lu\\n", '
        f'*(unsigned long long*)0x{state_address(state, "task_generation"):x}',
        'printf "SCHEDULER_CURRENT_PTR=0x%lx\\n", $current',
        f'printf "SCHEDULER_TASK_ID=%lu\\n", '
        f'*(unsigned long long*)($current + {TASK_ID_OFFSET})',
        f'printf "SCHEDULER_TASK_GENERATION=%lu\\n", '
        f'*(unsigned long long*)($current + {TASK_GENERATION_OFFSET})',
        f"dump binary memory {before_path} "
        f"0x{fixture_start:x} 0x{fixture_end:x}",
    ]
    lock_address = state_address(state, "lock_addr")
    commands.extend([
        f"set $assert_lock = *(unsigned long long*)0x{lock_address:x}",
        'printf "ASSERT_LOCK_ADDR=0x%lx\\n", $assert_lock',
    ])
    if has_lock:
        commands.append(
            'printf "ASSERT_LOCK_BEFORE=%u\\n", *(unsigned int*)$assert_lock')
    commands.extend([
        set_u32(state_address(state, "release"), 1),
        'printf "OBSERVER_RELEASED=1\\n"',
    ])
    return commands


def context_observer_commands(symbols):
    state = symbols["g_assertion_test_state"]["address"]
    scheduler = symbols["g_scheduler_cpus"]
    topology = symbols["g_cpus"]
    scheduler_stride = scheduler["size"] // MAX_CPUS
    topology_stride = topology["size"] // MAX_CPUS
    slot_address = state_address(state, "context_slot")
    return [
        f"set $context_slot = *(unsigned int*)0x{slot_address:x}",
        f"set $sched_stride = {scheduler_stride}",
        f"set $topology_stride = {topology_stride}",
        f"set $current = *(unsigned long long*)(0x{scheduler['address']:x} + "
        "$context_slot * $sched_stride + 8)",
        f'printf "CONTEXT_STATE_CPU_ID=%u\\n", '
        f'*(unsigned int*)0x{state_address(state, "context_cpu_id"):x}',
        f'printf "CONTEXT_STATE_SLOT=%u\\n", '
        f'*(unsigned int*)0x{slot_address:x}',
        f'printf "CONTEXT_VALID_MASK=%u\\n", '
        f'*(unsigned int*)0x{state_address(state, "context_valid_mask"):x}',
        f'printf "CONTEXT_STATE_TASK_ID=%lu\\n", '
        f'*(unsigned long long*)0x{state_address(state, "context_task_id"):x}',
        f'printf "CONTEXT_STATE_TASK_GENERATION=%lu\\n", '
        f'*(unsigned long long*)0x{state_address(state, "context_task_generation"):x}',
        f'printf "CONTEXT_TOPOLOGY_CPU_ID=%u\\n", '
        f'*(unsigned int*)(0x{topology["address"]:x} + '
        "$context_slot * $topology_stride + 4)",
        f'printf "CONTEXT_SCHEDULER_TASK_ID=%lu\\n", '
        f'*(unsigned long long*)($current + {TASK_ID_OFFSET})',
        f'printf "CONTEXT_SCHEDULER_TASK_GENERATION=%lu\\n", '
        f'*(unsigned long long*)($current + {TASK_GENERATION_OFFSET})',
        set_u32(state_address(state, "context_release"), 1),
        'printf "CONTEXT_RELEASED=1\\n"',
    ]


def after_commands(symbols, after_path, has_lock):
    state = symbols["g_assertion_test_state"]["address"]
    fixture = symbols["g_assertion_test_fixture"]
    commands = [
        f"dump binary memory {after_path} "
        f"0x{fixture['address']:x} 0x{fixture['address'] + fixture['size']:x}",
    ]
    for field in (
            "armed", "scenario", "trigger_slot", "queued", "prepared",
            "release", "completed", "result", "observed_slot",
            "observed_cpu_id", "if_before", "if_after", "evaluations",
            "continuation", "lock_acquired", "arm_error", "context_ready",
            "context_release", "context_cpu_id", "context_slot",
            "context_valid_mask", "context_reserved"):
        address = state_address(state, field)
        commands.append(
            f'printf "STATE_{field.upper()}=%u\\n", '
            f'*(unsigned int*)0x{address:x}')
    for field in ("lock_addr", "fixture_addr", "fixture_size", "task_id",
                  "task_generation", "context_task_id",
                  "context_task_generation"):
        address = state_address(state, field)
        commands.append(
            f'printf "STATE_{field.upper()}=0x%lx\\n", '
            f'*(unsigned long long*)0x{address:x}')
    commands.append(
        f"set $assert_lock = *(unsigned long long*)0x{state_address(state, 'lock_addr'):x}",
    )
    if has_lock:
        commands.append(
            'printf "ASSERT_LOCK_AFTER=%u\\n", *(unsigned int*)$assert_lock')
    return commands


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--run-dir", required=True)
    parser.add_argument("--image", required=True)
    parser.add_argument("--elf", required=True)
    parser.add_argument("--scenario", required=True, choices=SCENARIOS)
    parser.add_argument("--lock", choices=("none", "heap", "scheduler"),
                        default="none")
    parser.add_argument("--trigger-slot", type=int, default=0)
    parser.add_argument("--machine", choices=("q35", "pc"), default="q35")
    parser.add_argument("--smp", type=int, default=4)
    parser.add_argument("--accel", choices=("tcg", "kvm"), default="tcg")
    parser.add_argument("--memory", default="2G")
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--boot-timeout", type=float, default=75.0)
    parser.add_argument("--post-timeout", type=float, default=25.0)
    parser.add_argument("--cleanup-timeout", type=float, default=3.0)
    args = parser.parse_args()

    if args.smp < 1 or args.trigger_slot < 0 or args.trigger_slot >= args.smp:
        parser.error("trigger slot must be inside the virtual CPU topology")
    if args.accel == "kvm" and not os.access("/dev/kvm", os.R_OK | os.W_OK):
        parser.error("KVM was requested but /dev/kvm is unavailable")
    if args.scenario == "locked-bug" and args.lock == "none":
        parser.error("locked-bug requires --lock heap or scheduler")
    if args.scenario != "locked-bug" and args.lock != "none":
        parser.error("--lock applies only to locked-bug")

    run_dir = Path(args.run_dir).resolve()
    if run_dir.exists():
        parser.error(f"run directory already exists: {run_dir}")
    run_dir.mkdir(parents=True)
    image = Path(args.image).resolve()
    elf = Path(args.elf).resolve()
    if not image.is_file() or not elf.is_file():
        parser.error("image and ELF must exist")
    symbols = panic_qemu.elf_symbols(elf)
    required = {
        "g_assertion_test_state", "g_assertion_test_fixture",
        "g_panic_test_state", "g_panic_owner", "g_scheduler_cpus", "g_cpus",
        "g_heap_lock", "g_scheduler_lock", "assertion_warn_report",
        "assertion_bug_report",
    }
    missing = sorted(required.difference(symbols))
    if missing:
        parser.error("instrumentation symbols missing: " + ", ".join(missing))
    if symbols["g_assertion_test_state"]["size"] != 144:
        parser.error("unexpected assertion test state size")
    if symbols["g_scheduler_cpus"]["size"] % MAX_CPUS:
        parser.error("scheduler CPU array size is not divisible by max CPUs")
    if symbols["g_cpus"]["size"] % MAX_CPUS:
        parser.error("topology array size is not divisible by max CPUs")

    working_image = run_dir / "working.img"
    subprocess.run(["cp", "--reflink=auto", str(image), str(working_image)],
                   check=True)
    image_hash = sha256(working_image)
    elf_hash = sha256(elf)
    socket_dir = Path(tempfile.mkdtemp(prefix="hobbyos-assert-"))
    os.chmod(socket_dir, 0o700)
    command, firmware, qmp_path, hmp_path, gdb_path = (
        panic_qemu.launch_arguments(args, run_dir, socket_dir, working_image))
    del hmp_path
    qemu_version = subprocess.run(
        [args.qemu, "--version"], text=True, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, check=True).stdout.splitlines()[0]
    atomic_json(run_dir / "launch.json", {
        "schema": 1,
        "command": command,
        "qemu_version": qemu_version,
        "machine": args.machine,
        "accel": args.accel,
        "cpu": "host" if args.accel == "kvm" else "max",
        "smp": args.smp,
        "memory": args.memory,
        "firmware": firmware,
        "image_source": str(image),
        "image_sha256": image_hash,
        "elf": str(elf),
        "elf_sha256": elf_hash,
        "config": {"SELFTEST": 1, "SELFTEST_AUTORUN": 0,
                   "ASSERT_TEST": 1, "DEBUG_ASSERT": 1,
                   "HOBBYOS_PANIC_TEST": 1},
        "socket_directory": str(socket_dir),
        "socket_mode": "0700",
    })
    (run_dir / "launch-command.txt").write_text("\0".join(command) + "\n")

    result = {
        "schema": 1,
        "scenario": args.scenario,
        "scenario_id": SCENARIOS[args.scenario],
        "lock": args.lock,
        "profile": {"machine": args.machine, "accel": args.accel,
                    "smp": args.smp, "memory": args.memory,
                    "cpu": "host" if args.accel == "kvm" else "max"},
        "configuration": {"SELFTEST": 1, "SELFTEST_AUTORUN": 0,
                          "ASSERT_TEST": 1, "DEBUG_ASSERT": 1,
                          "HOBBYOS_PANIC_TEST": 1},
        "elf_sha256": elf_hash,
        "image_sha256_before": image_hash,
        "host_started": time.time(),
        "host_monotonic_started": time.monotonic(),
        "host_return_code": 1,
        "status": "FAIL",
    }
    process = None
    qmp = None
    stderr_handle = None
    cleanup = {"method": None, "started": None, "completed": None}
    try:
        stderr_handle = (run_dir / "qemu-stderr.log").open("w")
        process = subprocess.Popen(command, stdout=stderr_handle,
                                   stderr=subprocess.STDOUT)
        result["qemu_pid"] = process.pid
        socket_deadline = time.monotonic() + 10.0
        if not panic_qemu.wait_path(qmp_path, socket_deadline):
            raise RuntimeError("QMP socket did not appear")
        qmp = panic_qemu.QmpClient(qmp_path, run_dir / "qmp-events.jsonl",
                                   socket_deadline)
        result["qmp_connected_before_trigger"] = True
        serial_path = run_dir / "serial-transcript.log"
        boot_deadline = time.monotonic() + args.boot_timeout
        if not panic_qemu.wait_serial(
                serial_path, "[BOOT][RUNTIME_READY] PASS", boot_deadline, qmp):
            raise RuntimeError("boot/runtime readiness timeout")
        transcript = serial_path.read_text(errors="replace")
        if "[ASSERT_TEST][EARLY_END] status=PASS" not in transcript:
            raise RuntimeError("early assertion selftest did not pass")
        result["runtime_ready_monotonic"] = time.monotonic()
        result["status_before_trigger"] = qmp.command(
            "query-status", {}, time.monotonic() + 3.0)

        lock_name = None
        if args.lock == "heap":
            lock_name = "g_heap_lock"
        elif args.lock == "scheduler":
            lock_name = "g_scheduler_lock"
        result["trigger_monotonic"] = time.monotonic()
        panic_qemu.gdb_run(
            elf, gdb_path,
            arm_commands(symbols, args.scenario, args.trigger_slot, lock_name),
            run_dir / "gdb-arm.log")
        prepared_marker = (f"[ASSERT_TEST][PREPARED] "
                           f"scenario={SCENARIOS[args.scenario]}")
        if not panic_qemu.wait_serial(
                serial_path, prepared_marker,
                time.monotonic() + args.post_timeout, qmp):
            raise RuntimeError("guest did not publish prepared state")

        before_temporary = socket_dir / "fixture-before.bin"
        before_path = run_dir / "fixture-before.bin"
        observer_text = panic_qemu.gdb_run(
            elf, gdb_path, observer_commands(
                symbols, before_temporary, lock_name is not None),
            run_dir / "gdb-observer.log")
        shutil.copy2(before_temporary, before_path)
        result["observer_release_monotonic"] = time.monotonic()
        observed = {
            "state_slot": marker_value(observer_text,
                                       "STATE_OBSERVED_SLOT"),
            "state_cpu_id": marker_value(observer_text,
                                         "STATE_OBSERVED_CPU_ID"),
            "topology_cpu_id": marker_value(observer_text,
                                            "TOPOLOGY_CPU_ID"),
            "state_task_id": marker_value(observer_text, "STATE_TASK_ID"),
            "state_task_generation": marker_value(
                observer_text, "STATE_TASK_GENERATION"),
            "scheduler_task_id": marker_value(observer_text,
                                              "SCHEDULER_TASK_ID"),
            "scheduler_task_generation": marker_value(
                observer_text, "SCHEDULER_TASK_GENERATION"),
            "lock_address": marker_value(observer_text,
                                         "ASSERT_LOCK_ADDR", 16),
            "lock_before": marker_value(observer_text,
                                        "ASSERT_LOCK_BEFORE"),
        }
        observed["cpu_match"] = (
            observed["state_cpu_id"] is not None and
            observed["state_cpu_id"] == observed["topology_cpu_id"])
        observed["task_match"] = (
            observed["state_task_id"] is not None and
            observed["state_task_id"] != 0 and
            observed["state_task_id"] == observed["scheduler_task_id"] and
            observed["state_task_generation"] ==
            observed["scheduler_task_generation"])
        result["precondition_observation"] = observed

        terminal = args.scenario not in NONFATAL
        context_observed = None
        if args.scenario == "warn" or terminal:
            if not panic_qemu.wait_serial(
                    serial_path, "[ASSERT_TEST][CONTEXT_READY] valid_mask=7",
                    time.monotonic() + args.post_timeout, qmp):
                raise RuntimeError("assertion context observation timeout")
            context_text = panic_qemu.gdb_run(
                elf, gdb_path, context_observer_commands(symbols),
                run_dir / "gdb-context-observer.log")
            context_observed = {
                "state_cpu_id": marker_value(
                    context_text, "CONTEXT_STATE_CPU_ID"),
                "state_slot": marker_value(context_text,
                                           "CONTEXT_STATE_SLOT"),
                "valid_mask": marker_value(context_text,
                                             "CONTEXT_VALID_MASK"),
                "state_task_id": marker_value(
                    context_text, "CONTEXT_STATE_TASK_ID"),
                "state_task_generation": marker_value(
                    context_text, "CONTEXT_STATE_TASK_GENERATION"),
                "topology_cpu_id": marker_value(
                    context_text, "CONTEXT_TOPOLOGY_CPU_ID"),
                "scheduler_task_id": marker_value(
                    context_text, "CONTEXT_SCHEDULER_TASK_ID"),
                "scheduler_task_generation": marker_value(
                    context_text, "CONTEXT_SCHEDULER_TASK_GENERATION"),
            }
            context_observed["cpu_match"] = (
                context_observed["state_cpu_id"] is not None and
                context_observed["state_cpu_id"] ==
                context_observed["topology_cpu_id"])
            context_observed["task_match"] = (
                context_observed["state_task_id"] is not None and
                context_observed["state_task_id"] != 0 and
                context_observed["state_task_id"] ==
                context_observed["scheduler_task_id"] and
                context_observed["state_task_generation"] ==
                context_observed["scheduler_task_generation"])
            result["context_observation_monotonic"] = time.monotonic()
        result["independent_observation"] = (
            context_observed if context_observed is not None else observed)

        post_deadline = time.monotonic() + args.post_timeout
        wanted = (f"[ASSERT_TEST][COMPLETE] "
                  f"scenario={SCENARIOS[args.scenario]} status=PASS")
        if terminal:
            wanted = ("[PANIC][REENTRY]" if args.scenario in
                      ("reentry", "second-reentry") else
                      "[PANIC][ACTION] requested=halt")
        if not panic_qemu.wait_serial(serial_path, wanted, post_deadline, qmp):
            raise RuntimeError("guest did not reach the expected result")
        if terminal and args.scenario in ("reentry", "second-reentry"):
            time.sleep(0.35)
        elif terminal:
            if not panic_qemu.wait_serial(
                    serial_path, "[PANIC][ACTION] requested=halt",
                    post_deadline, qmp):
                raise RuntimeError("panic halt action was not reached")
            time.sleep(0.35)

        halt = None
        if terminal:
            first = panic_qemu.halt_snapshot(
                elf, gdb_path, symbols, run_dir, 1, args.smp)
            time.sleep(0.25)
            second = panic_qemu.halt_snapshot(
                elf, gdb_path, symbols, run_dir, 2, args.smp)
            expected_depth = 1
            if args.scenario == "reentry":
                expected_depth = 2
            elif args.scenario == "second-reentry":
                expected_depth = 3
            halt = {
                "first": first,
                "second": second,
                "expected_depth": expected_depth,
                "depth_ok": second["owner_depth"] == expected_depth,
                "observation_gap_seconds": (
                    second["host_monotonic"] - first["host_monotonic"]),
                "if_clear": first["if_clear"] and second["if_clear"],
                "all_cpus_halted": (first["all_cpus_halted"] and
                                    second["all_cpus_halted"]),
                "terminal_rips": (first["all_rips_in_terminal_halt"] and
                                  second["all_rips_in_terminal_halt"]),
            }
            result["terminal_evidence"] = halt

        after_temporary = socket_dir / "fixture-after.bin"
        after_path = run_dir / "fixture-after.bin"
        after_text = panic_qemu.gdb_run(
            elf, gdb_path, after_commands(
                symbols, after_temporary, lock_name is not None),
            run_dir / "gdb-after.log")
        shutil.copy2(after_temporary, after_path)
        state_after = {}
        for field in (
                "armed", "scenario", "trigger_slot", "queued", "prepared",
                "release", "completed", "result", "observed_slot",
                "observed_cpu_id", "if_before", "if_after", "evaluations",
                "continuation", "lock_acquired", "arm_error", "context_ready",
                "context_release", "context_cpu_id", "context_slot",
                "context_valid_mask", "context_reserved"):
            state_after[field] = marker_value(after_text,
                                             f"STATE_{field.upper()}")
        state_after["lock_after"] = marker_value(after_text,
                                                "ASSERT_LOCK_AFTER")
        result["state_after"] = state_after
        result["fixture"] = {
            "bytes": before_path.stat().st_size,
            "before_sha256": sha256(before_path),
            "after_sha256": sha256(after_path),
            "unchanged": before_path.read_bytes() == after_path.read_bytes(),
        }
        transcript = serial_path.read_text(errors="replace")
        result["serial_sha256"] = sha256(serial_path)

        semantic = [
            observed["cpu_match"],
            observed["task_match"],
            "[ASSERT_TEST][EARLY_END] status=PASS" in transcript,
            f"[ASSERT_TEST][EXECUTE] scenario={SCENARIOS[args.scenario]}" in
            transcript,
            state_after["arm_error"] == 0,
        ]
        if args.scenario in UNCHANGED_FIXTURE:
            semantic.append(result["fixture"]["unchanged"])
        if not terminal:
            semantic.extend([
                state_after["completed"] == 1,
                state_after["result"] == 1,
                "[PANIC][OWNER]" not in transcript,
            ])
            if args.scenario == "warn":
                semantic.extend([
                    state_after["evaluations"] == 1,
                    state_after["continuation"] == 1,
                    state_after["if_before"] == state_after["if_after"] == 1,
                ])
            if args.scenario == "false":
                semantic.extend([
                    state_after["evaluations"] == 2,
                    state_after["continuation"] == 1,
                ])
        else:
            assertion_line = next((line for line in transcript.splitlines()
                                   if line.startswith("[ASSERT][BUG] ")), None)
            semantic.extend([
                assertion_line is not None,
                "[PANIC][OWNER]" in transcript,
                "origin=assertion" in transcript,
                "[PANIC][DUMP_END] status=complete channel=serial" in transcript
                if args.scenario not in ("reentry", "second-reentry") else True,
                halt is not None and halt["depth_ok"] and halt["if_clear"] and
                halt["all_cpus_halted"] and halt["terminal_rips"] and
                halt["observation_gap_seconds"] >= 0.2,
            ])
            if args.scenario == "locked-bug":
                semantic.extend([
                    state_after["lock_acquired"] == 1,
                    state_after["lock_after"] == 1,
                    "[ASSERT_TEST][LOCK_HELD] acquired=1" in transcript,
                ])
        result["semantic_checks"] = semantic
        result["status"] = "PASS" if all(semantic) else "FAIL"
        result["host_return_code"] = 0 if result["status"] == "PASS" else 1
        result["status_before_cleanup"] = qmp.command(
            "query-status", {}, time.monotonic() + 3.0)
    except Exception as error:
        result["error"] = str(error)
        result["status"] = "FAIL"
        result["host_return_code"] = 1
    finally:
        cleanup["started"] = time.monotonic()
        if qmp and process and process.poll() is None:
            try:
                qmp.command("quit", {}, time.monotonic() + 2.0)
                cleanup["method"] = "qmp-quit"
            except Exception as error:
                cleanup["qmp_error"] = str(error)
        if process:
            try:
                process.wait(timeout=args.cleanup_timeout)
            except subprocess.TimeoutExpired:
                process.terminate()
                cleanup["method"] = cleanup["method"] or "sigterm"
                try:
                    process.wait(timeout=args.cleanup_timeout)
                except subprocess.TimeoutExpired:
                    process.kill()
                    cleanup["method"] = "sigkill"
                    process.wait(timeout=args.cleanup_timeout)
            result["qemu_process_return_code"] = process.returncode
        cleanup["completed"] = time.monotonic()
        result["cleanup"] = cleanup
        if qmp:
            result["qmp_messages"] = len(qmp.messages)
            qmp.close()
        if stderr_handle:
            stderr_handle.close()
        result["host_completed"] = time.time()
        result["host_elapsed_seconds"] = (
            time.monotonic() - result["host_monotonic_started"])
        if working_image.exists():
            result["image_sha256_after"] = sha256(working_image)
            result["image_unchanged"] = (
                result["image_sha256_after"] == image_hash)
        artifacts = {}
        for path in run_dir.iterdir():
            if path.is_file() and not path.is_symlink() and path.name != "result.json":
                artifacts[path.name] = {
                    "bytes": path.stat().st_size,
                    "sha256": sha256(path),
                }
        result["artifacts"] = artifacts
        atomic_json(run_dir / "result.json", result)
        shutil.rmtree(socket_dir, ignore_errors=True)

    print(json.dumps({"status": result["status"],
                      "host_return_code": result["host_return_code"],
                      "run_dir": str(run_dir)}, sort_keys=True))
    return result["host_return_code"]


if __name__ == "__main__":
    raise SystemExit(main())
