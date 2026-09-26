#!/usr/bin/env python3
"""Run one architecture-contract candidate with QMP and GDB observers."""

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time


SCRIPT_DIR = Path(__file__).resolve().parent
PANIC_CONTROLLER = SCRIPT_DIR / "panic-qemu.py"
spec = importlib.util.spec_from_file_location("hobbyos_panic_qemu",
                                              PANIC_CONTROLLER)
panic_qemu = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(panic_qemu)

STATE_OFFSETS = {
    "early_done": 0,
    "early_pass": 4,
    "safe_read_failure": 8,
    "safe_write_failure": 12,
    "runtime_started": 16,
    "target_count": 20,
    "joined_mask": 24,
    "active_mask": 28,
    "complete_mask": 32,
    "overlap_mask": 36,
    "snapshot_mask": 40,
    "error": 44,
    "observer_ready": 1072,
    "observer_release": 1076,
    "publication_ready_mask": 1080,
    "sampling_release": 1084,
    "writers_started_mask": 1088,
    "writers_finished_mask": 1092,
    "concurrent_samples": 1096,
    "concurrent_unavailable": 1100,
    "concurrent_active_samples": 1104,
    "concurrent_progress_mask": 1108,
    "observer_budget_pass": 1112,
    "pin_task_started": 1116,
    "pin_task_done": 1120,
    "pin_task_pass": 1124,
    "pin_if_before": 1128,
    "pin_if_during": 1132,
    "pin_if_after": 1136,
    "pin_cpu_before": 1140,
    "pin_cpu_during": 1144,
    "pin_cpu_after": 1148,
    "pin_slot": 1152,
    "pin_snapshot_if1": 1156,
    "pin_snapshot_if0": 1160,
    "budget_checks": 1164,
    "concurrent_first_sequence": 1168,
    "concurrent_last_sequence": 1296,
}
TRACE_OFFSETS = {
    "sequence": 4,
    "address": 8,
    "value": 16,
    "cpu_id": 24,
    "slot": 28,
    "width": 32,
    "operation": 33,
    "phase": 34,
    "value_valid": 35,
}
MAX_CPUS = 32
UNHANDLED_GP_OFFSETS = {
    "count": 0,
    "context_valid": 4,
    "rip": 8,
    "error_code": 16,
    "cs": 24,
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def atomic_json(path: Path, value) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")
    os.replace(temporary, path)


def gdb_script_run(elf: Path, socket_path: Path, commands: list[str],
                   output_path: Path, timeout: float) -> str:
    command_path = output_path.with_suffix(".cmd")
    full = [
        "set confirm off", "set pagination off", "set print elements 0",
        f"target remote {socket_path}", *commands, "detach", "quit",
    ]
    command_path.write_text("\n".join(full) + "\n")
    result = subprocess.run(
        ["gdb", "-q", "-batch", str(elf), "-x", str(command_path)],
        text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=timeout)
    output_path.write_text(result.stdout)
    if result.returncode != 0:
        raise RuntimeError(
            f"gdb failed with {result.returncode}; see {output_path}")
    return result.stdout


def wait_serial(path: Path, marker: str, timeout: float,
                qmp=None) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists() and marker in path.read_text(errors="replace"):
            return True
        if qmp:
            qmp.poll(0)
        time.sleep(0.05)
    return False


def symbol_address(symbols: dict, name: str) -> int:
    if name not in symbols:
        raise RuntimeError(f"required ELF symbol is absent: {name}")
    return symbols[name]["address"]


def next_instruction_address(elf: Path, start: int, stop: int) -> int:
    result = subprocess.run(
        ["objdump", "-d", f"--start-address=0x{start:x}",
         f"--stop-address=0x{stop:x}", str(elf)],
        text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode != 0:
        raise RuntimeError("objdump failed while locating fixup probe")
    addresses = [int(value, 16) for value in re.findall(
        r"^\s*([0-9a-f]+):\s+(?:[0-9a-f]{2}\s+)+", result.stdout,
        re.MULTILINE)]
    if len(addresses) < 2 or addresses[0] != start:
        raise RuntimeError(
            f"cannot locate instruction after fixup 0x{start:x}")
    return addresses[1]


def early_observer_commands(elf: Path, symbols: dict) -> list[str]:
    state = symbol_address(symbols, "g_arch_test_state")
    read_site = symbol_address(symbols, "cpu_read_msr_safe_site")
    read_fixup = symbol_address(symbols, "cpu_read_msr_safe_fixup")
    write_site = symbol_address(symbols, "cpu_write_msr_safe_site")
    write_fixup = symbol_address(symbols, "cpu_write_msr_safe_fixup")
    read_probe = next_instruction_address(elf, read_fixup, write_site)
    write_probe = next_instruction_address(
        elf, write_fixup, symbol_address(symbols, "cpu_msr_fixup_lookup"))
    return [
        "set $gp_count = 0",
        "set $pending_gp = 0",
        "set $pending_gp_site = 0",
        "set $read_fixup_count = 0",
        "set $write_fixup_count = 0",
        "hbreak general_protection_msr_fixup",
        "commands",
        "silent",
        ('printf "GP_HIT=%u FRAME_RIP=0x%lx ERROR=0x%lx CS=0x%lx\\n", '
         '$gp_count, *(unsigned long long*)$rdi, $rsi, '
         '*(unsigned long long*)($rdi + 8)'),
        "set $pending_gp_site = *(unsigned long long*)$rdi",
        "set $pending_gp = 1",
        "set $gp_count = $gp_count + 1",
        "continue",
        "end",
        f"hbreak *0x{read_probe:x}",
        "commands",
        "silent",
        f"if $pending_gp && $pending_gp_site == 0x{read_site:x}",
        (f'printf "READ_FIXUP_HIT=%u RIP=0x%lx FIXUP=0x%lx '
         f'SITE=0x%lx\\n", $read_fixup_count, $rip, {read_fixup}, '
         '$pending_gp_site'),
        "set $read_fixup_count = $read_fixup_count + 1",
        "set $pending_gp = 0",
        "end",
        "continue",
        "end",
        f"hbreak *0x{write_probe:x}",
        "commands",
        "silent",
        f"if $pending_gp && $pending_gp_site == 0x{write_site:x}",
        (f'printf "WRITE_FIXUP_HIT=%u RIP=0x%lx FIXUP=0x%lx '
         f'SITE=0x%lx\\n", $write_fixup_count, $rip, {write_fixup}, '
         '$pending_gp_site'),
        "set $write_fixup_count = $write_fixup_count + 1",
        "set $pending_gp = 0",
        "end",
        "continue",
        "end",
        "hbreak arch_selftest_gdb_done",
        "commands",
        "silent",
        "end",
        "continue",
        'printf "GP_COUNT=%u\\n", $gp_count',
        'printf "READ_FIXUP_COUNT=%u\\n", $read_fixup_count',
        'printf "WRITE_FIXUP_COUNT=%u\\n", $write_fixup_count',
        'printf "EARLY_CONTINUATION_RIP=0x%lx\\n", $rip',
        f'printf "READ_SITE=0x%lx\\n", {read_site}',
        f'printf "READ_FIXUP=0x%lx\\n", {read_fixup}',
        f'printf "READ_FIXUP_PROBE=0x%lx\\n", {read_probe}',
        f'printf "WRITE_SITE=0x%lx\\n", {write_site}',
        f'printf "WRITE_FIXUP=0x%lx\\n", {write_fixup}',
        f'printf "WRITE_FIXUP_PROBE=0x%lx\\n", {write_probe}',
        f'printf "EARLY_DONE=%u\\n", *(unsigned int*)0x{state:x}',
        (f'printf "EARLY_PASS=%u\\n", '
         f'*(unsigned int*)0x{state + STATE_OFFSETS["early_pass"]:x}'),
        (f'printf "SAFE_READ_FAILURE=%u\\n", '
         f'*(unsigned int*)0x{state + STATE_OFFSETS["safe_read_failure"]:x}'),
        (f'printf "SAFE_WRITE_FAILURE=%u\\n", '
         f'*(unsigned int*)0x{state + STATE_OFFSETS["safe_write_failure"]:x}'),
        (f'printf "BUDGET_CHECKS=0x%x\\n", '
         f'*(unsigned int*)0x{state + STATE_OFFSETS["budget_checks"]:x}'),
    ]


def trace_observer_commands(symbols: dict, smp: int,
                            negative: bool) -> list[str]:
    state = symbol_address(symbols, "g_arch_test_state")
    records = symbol_address(symbols, "g_mmio_trace_records")
    cells = symbol_address(symbols, "g_arch_test_mmio_cells")
    topology = symbol_address(symbols, "g_cpus")
    record_size = symbols["g_mmio_trace_records"]["size"] // MAX_CPUS
    topology_size = symbols["g_cpus"]["size"] // MAX_CPUS
    if record_size != 64:
        raise RuntimeError(f"unexpected MMIO trace record size: {record_size}")
    if topology_size < 8:
        raise RuntimeError(f"unexpected topology record size: {topology_size}")
    commands = [
        (f'printf "OBSERVER_READY=%u\\n", '
         f'*(unsigned int*)0x{state + STATE_OFFSETS["observer_ready"]:x}'),
        (f'printf "TARGET_COUNT=%u\\n", '
         f'*(unsigned int*)0x{state + STATE_OFFSETS["target_count"]:x}'),
        (f'printf "JOINED_MASK=0x%x\\n", '
         f'*(unsigned int*)0x{state + STATE_OFFSETS["joined_mask"]:x}'),
        (f'printf "ACTIVE_MASK=0x%x\\n", '
         f'*(unsigned int*)0x{state + STATE_OFFSETS["active_mask"]:x}'),
        f'printf "TRACE_RECORD_STRIDE={record_size}\\n"',
        f'printf "TRACE_CELL_BASE=0x%lx\\n", {cells}',
        f'printf "TOPOLOGY_STRIDE={topology_size}\\n"',
    ]
    for slot in range(smp):
        topology_record = topology + slot * topology_size
        commands.append(
            f'printf "TOPOLOGY_INDEX={slot} TOPOLOGY_SLOT=%u '
            f'TOPOLOGY_CPU_ID=%u\\n", '
            f'*(unsigned int*)0x{topology_record:x}, '
            f'*(unsigned int*)0x{topology_record + 4:x}')
        base = records + slot * record_size
        commands.append(
            'printf "TRACE_SLOT=%u SEQUENCE=%u ADDRESS=0x%lx VALUE=0x%lx '
            'CPU_ID=%u RECORD_SLOT=%u WIDTH=%u OPERATION=%u PHASE=%u '
            'VALUE_VALID=%u\\n", '
            f'{slot}, *(unsigned int*)0x{base + TRACE_OFFSETS["sequence"]:x}, '
            f'*(unsigned long long*)0x{base + TRACE_OFFSETS["address"]:x}, '
            f'*(unsigned long long*)0x{base + TRACE_OFFSETS["value"]:x}, '
            f'*(unsigned int*)0x{base + TRACE_OFFSETS["cpu_id"]:x}, '
            f'*(unsigned int*)0x{base + TRACE_OFFSETS["slot"]:x}, '
            f'*(unsigned char*)0x{base + TRACE_OFFSETS["width"]:x}, '
            f'*(unsigned char*)0x{base + TRACE_OFFSETS["operation"]:x}, '
            f'*(unsigned char*)0x{base + TRACE_OFFSETS["phase"]:x}, '
            f'*(unsigned char*)0x{base + TRACE_OFFSETS["value_valid"]:x}')
    commands.append(
        f'set {{unsigned int}}0x{state + STATE_OFFSETS["observer_release"]:x} = 1')
    commands.append('printf "OBSERVER_RELEASED=1\\n"')
    if negative:
        unrelated_site = symbol_address(symbols, "arch_test_unrelated_gp_site")
        unhandled = symbol_address(
            symbols, "g_arch_test_unhandled_gp_observation")
        commands.extend([
            f'printf "UNRELATED_SITE=0x%lx\\n", {unrelated_site}',
            "set $unrelated_gp_count = 0",
            "hbreak general_protection_msr_fixup",
            "commands",
            "silent",
            ('printf "UNRELATED_GP_HIT=%u FRAME_RIP=0x%lx ERROR=0x%lx '
             'CS=0x%lx\\n", $unrelated_gp_count, '
             '*(unsigned long long*)$rdi, $rsi, '
             '*(unsigned long long*)($rdi + 8)'),
            "set $unrelated_gp_count = $unrelated_gp_count + 1",
            "continue",
            "end",
            "hbreak panic_halt_secondary",
            "commands",
            "silent",
            "end",
            "continue",
            'printf "UNRELATED_GP_COUNT=%u\\n", $unrelated_gp_count',
            (f'printf "UNRELATED_KERNEL_COUNT=%u\\n", '
             f'*(unsigned int*)0x{unhandled + UNHANDLED_GP_OFFSETS["count"]:x}'),
            (f'printf "UNRELATED_KERNEL_CONTEXT_VALID=%u\\n", '
             f'*(unsigned int*)0x{unhandled + UNHANDLED_GP_OFFSETS["context_valid"]:x}'),
            (f'printf "UNRELATED_KERNEL_RIP=0x%lx\\n", '
             f'*(unsigned long long*)0x{unhandled + UNHANDLED_GP_OFFSETS["rip"]:x}'),
            (f'printf "UNRELATED_KERNEL_ERROR=0x%lx\\n", '
             f'*(unsigned long long*)0x{unhandled + UNHANDLED_GP_OFFSETS["error_code"]:x}'),
            (f'printf "UNRELATED_KERNEL_CS=0x%lx\\n", '
             f'*(unsigned long long*)0x{unhandled + UNHANDLED_GP_OFFSETS["cs"]:x}'),
            'printf "TERMINAL_RIP=0x%lx\\n", $rip',
            'printf "TERMINAL_EFLAGS=0x%lx\\n", $eflags',
        ])
    return commands


def final_observer_commands(symbols: dict, smp: int) -> list[str]:
    state = symbol_address(symbols, "g_arch_test_state")
    commands = []
    for name in (
            "publication_ready_mask", "sampling_release",
            "writers_started_mask", "writers_finished_mask",
            "concurrent_samples", "concurrent_unavailable",
            "concurrent_active_samples", "concurrent_progress_mask",
            "observer_budget_pass", "pin_task_started", "pin_task_done",
            "pin_task_pass", "pin_if_before", "pin_if_during",
            "pin_if_after", "pin_cpu_before", "pin_cpu_during",
            "pin_cpu_after", "pin_slot", "pin_snapshot_if1",
            "pin_snapshot_if0", "budget_checks"):
        commands.append(
            f'printf "FINAL_{name.upper()}=0x%x\\n", '
            f'*(unsigned int*)0x{state + STATE_OFFSETS[name]:x}')
    for slot in range(smp):
        first = state + STATE_OFFSETS["concurrent_first_sequence"] + slot * 4
        last = state + STATE_OFFSETS["concurrent_last_sequence"] + slot * 4
        commands.append(
            f'printf "CONCURRENT_SLOT={slot} FIRST=%u LAST=%u\\n", '
            f'*(unsigned int*)0x{first:x}, *(unsigned int*)0x{last:x}')
    return commands


def parse_marker(text: str, name: str, base: int = 10):
    match = re.search(rf"(?:^|\s){re.escape(name)}=([^\s]+)", text)
    if not match:
        return None
    value = match.group(1)
    try:
        return int(value, base)
    except ValueError:
        return None


def trace_rows(text: str) -> list[dict]:
    pattern = re.compile(
        r"TRACE_SLOT=(\d+) SEQUENCE=(\d+) ADDRESS=0x([0-9a-f]+) "
        r"VALUE=0x([0-9a-f]+) CPU_ID=(\d+) RECORD_SLOT=(\d+) "
        r"WIDTH=(\d+) OPERATION=(\d+) PHASE=(\d+) VALUE_VALID=(\d+)")
    rows = []
    for match in pattern.finditer(text):
        values = [int(match.group(index), 16 if index in (3, 4) else 10)
                  for index in range(1, 11)]
        rows.append(dict(zip((
            "slot", "sequence", "address", "value", "cpu_id",
            "record_slot", "width", "operation", "phase",
            "value_valid"), values)))
    return rows


def topology_rows(text: str) -> list[dict]:
    pattern = re.compile(
        r"TOPOLOGY_INDEX=(\d+) TOPOLOGY_SLOT=(\d+) TOPOLOGY_CPU_ID=(\d+)")
    return [
        {"index": int(match.group(1)), "slot": int(match.group(2)),
         "cpu_id": int(match.group(3))}
        for match in pattern.finditer(text)
    ]


def concurrent_rows(text: str) -> list[dict]:
    pattern = re.compile(r"CONCURRENT_SLOT=(\d+) FIRST=(\d+) LAST=(\d+)")
    return [
        {"slot": int(match.group(1)), "first": int(match.group(2)),
         "last": int(match.group(3))}
        for match in pattern.finditer(text)
    ]


def run(args) -> int:
    run_dir = Path(args.run_dir).resolve()
    if run_dir.exists():
        raise RuntimeError(f"run directory already exists: {run_dir}")
    run_dir.mkdir(parents=True)
    image = Path(args.image).resolve()
    elf = Path(args.elf).resolve()
    if not image.is_file() or not elf.is_file():
        raise RuntimeError("--image and --elf must name regular files")
    if args.smp < 1 or args.smp > MAX_CPUS:
        raise RuntimeError("--smp is outside the supported test topology")
    if args.accel == "kvm" and not os.access("/dev/kvm", os.R_OK | os.W_OK):
        raise RuntimeError("KVM is unavailable")
    symbols = panic_qemu.elf_symbols(elf)
    required = {
        "cpu_read_msr_safe", "cpu_read_msr_safe_site",
        "cpu_read_msr_safe_fixup", "cpu_write_msr_safe",
        "cpu_write_msr_safe_site", "cpu_write_msr_safe_fixup",
    }
    if not args.production:
        required.update({
            "g_arch_test_state", "g_arch_test_mmio_cells",
            "g_mmio_trace_records", "general_protection_msr_fixup",
            "arch_selftest_gdb_done", "panic_halt_secondary", "g_cpus",
        })
    if args.negative:
        required.update({
            "arch_test_unrelated_gp_site",
            "g_arch_test_unhandled_gp_observation",
        })
    missing = sorted(required - symbols.keys())
    if missing:
        raise RuntimeError("required symbols absent: " + ",".join(missing))

    working = run_dir / "working.img"
    subprocess.run(["cp", "--reflink=auto", str(image), str(working)],
                   check=True)
    socket_dir = Path(tempfile.mkdtemp(prefix="hobbyos-arch-"))
    os.chmod(socket_dir, 0o700)
    firmware_args, firmware = panic_qemu.firmware_arguments(run_dir)
    qmp_path = socket_dir / "qmp.sock"
    hmp_path = socket_dir / "hmp.sock"
    gdb_path = socket_dir / "gdb.sock"
    cpu_model = "host" if args.accel == "kvm" else "Haswell"
    accel_arg = "kvm" if args.accel == "kvm" else "tcg,thread=multi"
    serial = run_dir / "serial-transcript.log"
    command = [
        args.qemu, "-machine", "q35", "-accel", accel_arg,
        "-cpu", cpu_model,
        "-smp", f"{args.smp},sockets=1,cores={args.smp},threads=1",
        "-m", "2G", *firmware_args, "-net", "none",
        "-drive", f"file={working},format=raw,cache=writeback",
        "-serial", f"file:{serial}",
        "-debugcon", f"file:{run_dir / 'debugcon.log'}",
        "-global", "isa-debugcon.iobase=0x402",
        "-d", "guest_errors", "-D", str(run_dir / "qemu-trace.log"),
        "-display", "none",
        "-monitor", f"unix:{hmp_path},server=on,wait=off",
        "-qmp", f"unix:{qmp_path},server=on,wait=off",
        "-chardev", f"socket,path={gdb_path},server=on,wait=off,id=gdb0",
        "-gdb", "chardev:gdb0", "-S", "-no-shutdown",
        "-device", "nec-usb-xhci,id=xhci,msi=on,msix=off",
        "-device", "usb-kbd,bus=xhci.0",
    ]
    version = subprocess.run([args.qemu, "--version"], text=True,
                             stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT,
                             check=True).stdout.splitlines()[0]
    launch = {
        "schema": 1,
        "command": command,
        "machine": "q35", "accel": args.accel,
        "cpu": cpu_model, "smp": args.smp, "memory": "2G",
        "negative": args.negative, "production": args.production,
        "firmware": firmware,
        "qemu_version": version,
        "image": str(image), "image_sha256": sha256(image),
        "elf": str(elf), "elf_sha256": sha256(elf),
        "socket_directory": str(socket_dir), "socket_mode": "0700",
    }
    atomic_json(run_dir / "launch.json", launch)
    (run_dir / "launch-command.txt").write_text("\0".join(command) + "\n")
    result = {
        "schema": 1, "status": "FAIL", "host_return_code": 1,
        "profile": {key: launch[key] for key in
                    ("machine", "accel", "cpu", "smp", "memory")},
        "negative": args.negative, "production": args.production,
        "elf_sha256": launch["elf_sha256"],
        "image_sha256": launch["image_sha256"],
        "host_started": time.time(),
    }
    process = None
    qmp = None
    stderr_handle = None
    try:
        stderr_handle = (run_dir / "qemu-stderr.log").open("w")
        process = subprocess.Popen(command, stdout=stderr_handle,
                                   stderr=subprocess.STDOUT)
        result["qemu_pid"] = process.pid
        deadline = time.monotonic() + 10
        if not panic_qemu.wait_path(qmp_path, deadline):
            raise RuntimeError("QMP socket did not appear")
        if not panic_qemu.wait_path(gdb_path, deadline):
            raise RuntimeError("GDB socket did not appear")
        qmp = panic_qemu.QmpClient(qmp_path, run_dir / "qmp-events.jsonl",
                                   deadline)
        result["qmp_connected_before_execution"] = True

        if args.production:
            if any(name in symbols for name in (
                    "g_arch_test_state", "g_arch_test_mmio_cells",
                    "g_mmio_trace_records", "mmio_trace_begin",
                    "mmio_trace_complete", "mmio_trace_snapshot_current")):
                raise RuntimeError("production ELF contains test or trace state")
            qmp.command("cont", {}, time.monotonic() + 3)
            if not wait_serial(serial, "[BOOT][RUNTIME_READY] PASS",
                               args.boot_timeout, qmp):
                raise RuntimeError("production runtime readiness timed out")
            transcript = serial.read_text(errors="replace")
            production_ok = "[ARCH_TEST]" not in transcript
            result["production_trace_elided"] = production_ok
            result["status_before_cleanup"] = qmp.command(
                "query-status", {}, time.monotonic() + 3)
            result["status"] = "PASS" if production_ok else "FAIL"
            result["host_return_code"] = 0 if production_ok else 1
            return result["host_return_code"]

        early_log = run_dir / "gdb-early.log"
        early_text = gdb_script_run(
            elf, gdb_path, early_observer_commands(elf, symbols), early_log,
            args.boot_timeout)
        result["early_observer"] = {
            "gp_count": parse_marker(early_text, "GP_COUNT"),
            "early_done": parse_marker(early_text, "EARLY_DONE"),
            "early_pass": parse_marker(early_text, "EARLY_PASS"),
            "safe_read_failure": parse_marker(
                early_text, "SAFE_READ_FAILURE"),
            "safe_write_failure": parse_marker(
                early_text, "SAFE_WRITE_FAILURE"),
            "budget_checks": parse_marker(early_text, "BUDGET_CHECKS", 16),
            "read_fixup_count": parse_marker(
                early_text, "READ_FIXUP_COUNT"),
            "write_fixup_count": parse_marker(
                early_text, "WRITE_FIXUP_COUNT"),
            "continuation_rip": parse_marker(
                early_text, "EARLY_CONTINUATION_RIP", 16),
            "gp_frame_rips": [int(value, 16) for value in re.findall(
                r"FRAME_RIP=0x([0-9a-f]+)", early_text)],
            "gp_events": [
                {"ordinal": int(match.group(1)),
                 "rip": int(match.group(2), 16),
                 "error": int(match.group(3), 16),
                 "cs": int(match.group(4), 16)}
                for match in re.finditer(
                    r"GP_HIT=(\d+) FRAME_RIP=0x([0-9a-f]+) "
                    r"ERROR=0x([0-9a-f]+) CS=0x([0-9a-f]+)", early_text)],
            "read_fixup_probe_rips": [int(value, 16) for value in re.findall(
                r"READ_FIXUP_HIT=\d+ RIP=0x([0-9a-f]+)", early_text)],
            "read_fixup_rips": [int(value, 16) for value in re.findall(
                r"READ_FIXUP_HIT=\d+ RIP=0x[0-9a-f]+ FIXUP=0x([0-9a-f]+)",
                early_text)],
            "read_fixup_sites": [int(value, 16) for value in re.findall(
                r"READ_FIXUP_HIT=\d+ RIP=0x[0-9a-f]+ FIXUP=0x[0-9a-f]+ "
                r"SITE=0x([0-9a-f]+)",
                early_text)],
            "write_fixup_probe_rips": [int(value, 16) for value in re.findall(
                r"WRITE_FIXUP_HIT=\d+ RIP=0x([0-9a-f]+)", early_text)],
            "write_fixup_rips": [int(value, 16) for value in re.findall(
                r"WRITE_FIXUP_HIT=\d+ RIP=0x[0-9a-f]+ FIXUP=0x([0-9a-f]+)",
                early_text)],
            "write_fixup_sites": [int(value, 16) for value in re.findall(
                r"WRITE_FIXUP_HIT=\d+ RIP=0x[0-9a-f]+ FIXUP=0x[0-9a-f]+ "
                r"SITE=0x([0-9a-f]+)",
                early_text)],
        }
        if not wait_serial(serial, "[ARCH_TEST][TRACE_ACTIVE]",
                           args.boot_timeout, qmp):
            raise RuntimeError("trace-active rendezvous timed out")

        trace_log = run_dir / "gdb-trace-active.log"
        trace_text = gdb_script_run(
            elf, gdb_path,
            trace_observer_commands(symbols, args.smp, args.negative),
            trace_log, args.boot_timeout)
        rows = trace_rows(trace_text)
        topology = topology_rows(trace_text)
        expected_mask = (1 << args.smp) - 1
        cells = symbol_address(symbols, "g_arch_test_mmio_cells")
        trace_valid = (
            parse_marker(trace_text, "OBSERVER_READY") == 1 and
            parse_marker(trace_text, "TARGET_COUNT") == args.smp and
            parse_marker(trace_text, "JOINED_MASK", 16) == expected_mask and
            parse_marker(trace_text, "ACTIVE_MASK", 16) == expected_mask and
            len(rows) == args.smp and len(topology) == args.smp)
        cpu_ids = set()
        for slot, row in enumerate(rows):
            expected_value = ((row["slot"] + 1) << 56) | 1
            trace_valid = trace_valid and (
                row["slot"] == slot and topology[slot]["index"] == slot and
                topology[slot]["slot"] == slot and
                row["cpu_id"] == topology[slot]["cpu_id"] and
                row["record_slot"] == row["slot"] and
                row["address"] == cells + row["slot"] * 8 and
                row["value"] == expected_value and row["width"] == 8 and
                row["operation"] == 1 and row["phase"] == 2 and
                row["value_valid"] == 1 and row["sequence"] != 0 and
                (row["sequence"] & 1) == 0)
            cpu_ids.add(row["cpu_id"])
        trace_valid = trace_valid and len(cpu_ids) == args.smp
        result["trace_observer"] = {
            "ready": parse_marker(trace_text, "OBSERVER_READY"),
            "target_count": parse_marker(trace_text, "TARGET_COUNT"),
            "joined_mask": parse_marker(trace_text, "JOINED_MASK", 16),
            "active_mask": parse_marker(trace_text, "ACTIVE_MASK", 16),
            "rows": rows, "topology": topology, "cell_base": cells,
            "valid": trace_valid,
        }
        early_state = result["early_observer"]
        early_common_valid = (
            early_state["early_done"] == 1 and
            early_state["early_pass"] == 1 and
            early_state["budget_checks"] == 0x3f and
            all(event["error"] == 0 and event["cs"] == 8
                for event in early_state["gp_events"]))

        if args.negative:
            if not wait_serial(serial, "[PANIC][ACTION] requested=halt",
                               args.post_timeout, qmp):
                raise RuntimeError("unrelated #GP did not reach panic halt")
            first = panic_qemu.halt_snapshot(
                elf, gdb_path, symbols, run_dir, 1, args.smp)
            time.sleep(0.25)
            second = panic_qemu.halt_snapshot(
                elf, gdb_path, symbols, run_dir, 2, args.smp)
            transcript = serial.read_text(errors="replace")
            site = symbol_address(symbols, "arch_test_unrelated_gp_site")
            unrelated_events = [
                {"ordinal": int(match.group(1)),
                 "rip": int(match.group(2), 16),
                 "error": int(match.group(3), 16),
                 "cs": int(match.group(4), 16)}
                for match in re.finditer(
                    r"UNRELATED_GP_HIT=(\d+) FRAME_RIP=0x([0-9a-f]+) "
                    r"ERROR=0x([0-9a-f]+) CS=0x([0-9a-f]+)", trace_text)]
            unrelated_rips = [event["rip"] for event in unrelated_events]
            gdb_notification_count = parse_marker(
                trace_text, "UNRELATED_GP_COUNT")
            panic_frame_rips = [int(value, 16) for value in re.findall(
                r"^\[PANIC\]\[FRAME\] available=1 rip=0x([0-9A-Fa-f]+) ",
                transcript, re.MULTILINE)]
            kernel_observer = {
                "count": parse_marker(trace_text, "UNRELATED_KERNEL_COUNT"),
                "context_valid": parse_marker(
                    trace_text, "UNRELATED_KERNEL_CONTEXT_VALID"),
                "rip": parse_marker(trace_text, "UNRELATED_KERNEL_RIP", 16),
                "error": parse_marker(
                    trace_text, "UNRELATED_KERNEL_ERROR", 16),
                "cs": parse_marker(trace_text, "UNRELATED_KERNEL_CS", 16),
            }
            terminal_ok = (
                len(unrelated_events) >= 1 and
                gdb_notification_count == len(unrelated_events) and
                all(event["ordinal"] == ordinal and
                    event["rip"] == site and event["error"] == 0 and
                    event["cs"] == 8
                    for ordinal, event in enumerate(unrelated_events)) and
                kernel_observer == {
                    "count": 1, "context_valid": 1, "rip": site,
                    "error": 0, "cs": 8} and
                "[ARCH_TEST][UNRELATED_GP] action=execute" in transcript and
                transcript.count("[PANIC][VECTOR] known=1 value=13") == 1 and
                panic_frame_rips == [site] and
                transcript.count("[PANIC][ACTION] requested=halt") == 1 and
                "[ARCH_TEST][UNRELATED_GP_RETURNED]" not in transcript and
                first["if_clear"] and second["if_clear"] and
                first["all_cpus_halted"] and second["all_cpus_halted"])
            result["negative_observer"] = {
                "site": site, "observed_rips": unrelated_rips,
                "observed_events": unrelated_events,
                "gdb_notification_count": gdb_notification_count,
                "kernel_observer": kernel_observer,
                "panic_frame_rips": panic_frame_rips,
                "first_halt": first, "second_halt": second,
                "valid": terminal_ok,
            }
            raw_records = (
                transcript.count("[ARCH_TEST][CPUID]") == 7 and
                transcript.count("[ARCH_TEST][MSR]") == 4 and
                transcript.count("[ARCH_TEST][BUDGET] status=PASS") == 1)
            run_ok = trace_valid and terminal_ok and early_common_valid and raw_records
        else:
            if not wait_serial(serial, "[ARCH_TEST][RUNTIME] status=PASS",
                               args.post_timeout, qmp):
                raise RuntimeError("runtime architecture test did not pass")
            if not wait_serial(serial, "[ARCH_TEST][PIN] status=PASS",
                               args.post_timeout, qmp):
                raise RuntimeError("runtime pinning task did not pass")
            final_log = run_dir / "gdb-final.log"
            final_text = gdb_script_run(
                elf, gdb_path, final_observer_commands(symbols, args.smp),
                final_log, args.post_timeout)
            final_values = {
                name: parse_marker(final_text, f"FINAL_{name.upper()}", 16)
                for name in (
                    "publication_ready_mask", "sampling_release",
                    "writers_started_mask", "writers_finished_mask",
                    "concurrent_samples", "concurrent_unavailable",
                    "concurrent_active_samples", "concurrent_progress_mask",
                    "observer_budget_pass", "pin_task_started",
                    "pin_task_done", "pin_task_pass", "pin_if_before",
                    "pin_if_during", "pin_if_after", "pin_cpu_before",
                    "pin_cpu_during", "pin_cpu_after", "pin_slot",
                    "pin_snapshot_if1", "pin_snapshot_if0", "budget_checks")}
            final_rows = concurrent_rows(final_text)
            final_valid = (
                final_values["publication_ready_mask"] == expected_mask and
                final_values["sampling_release"] == 1 and
                final_values["writers_started_mask"] == expected_mask and
                final_values["writers_finished_mask"] == expected_mask and
                final_values["concurrent_samples"] is not None and
                final_values["concurrent_samples"] >= args.smp and
                final_values["concurrent_progress_mask"] == expected_mask and
                final_values["observer_budget_pass"] == 1 and
                final_values["pin_task_started"] == 1 and
                final_values["pin_task_done"] == 1 and
                final_values["pin_task_pass"] == 1 and
                final_values["pin_if_before"] == 1 and
                final_values["pin_if_during"] == 0 and
                final_values["pin_if_after"] == 1 and
                final_values["pin_snapshot_if1"] == 1 and
                final_values["pin_snapshot_if0"] == 1 and
                final_values["budget_checks"] == 0x3f and
                len(final_rows) == args.smp and
                all(row["slot"] == slot and row["first"] != 0 and
                    row["last"] != 0 and row["first"] != row["last"]
                    for slot, row in enumerate(final_rows)) and
                (args.smp == 1 or
                 final_values["concurrent_active_samples"] > 0))
            result["final_observer"] = {
                "values": final_values, "rows": final_rows,
                "valid": final_valid}
            transcript = serial.read_text(errors="replace")
            early = result["early_observer"]
            read_site = symbol_address(symbols, "cpu_read_msr_safe_site")
            write_site = symbol_address(symbols, "cpu_write_msr_safe_site")
            sites = {read_site, write_site}
            read_fixup = symbol_address(symbols, "cpu_read_msr_safe_fixup")
            write_fixup = symbol_address(symbols, "cpu_write_msr_safe_fixup")
            read_probe = next_instruction_address(elf, read_fixup, write_site)
            write_probe = next_instruction_address(
                elf, write_fixup,
                symbol_address(symbols, "cpu_msr_fixup_lookup"))
            observed = set(early["gp_frame_rips"])
            gp_context_valid = all(
                event["error"] == 0 and event["cs"] == 8
                for event in early["gp_events"])
            if args.accel == "kvm":
                msr_valid = (early["safe_read_failure"] == 1 and
                             early["safe_write_failure"] == 1 and
                             sites == observed and
                             early["read_fixup_count"] == 1 and
                             early["write_fixup_count"] == 1 and
                             early["read_fixup_probe_rips"] == [read_probe] and
                             early["read_fixup_rips"] == [read_fixup] and
                             early["read_fixup_sites"] == [read_site] and
                             early["write_fixup_probe_rips"] == [write_probe] and
                             early["write_fixup_rips"] == [write_fixup] and
                             early["write_fixup_sites"] == [write_site] and
                             len(early["gp_events"]) == 2 and
                             gp_context_valid)
            else:
                msr_valid = (early["safe_read_failure"] == 0 and
                             early["safe_write_failure"] == 1 and
                             write_site in observed and
                             early["read_fixup_count"] == 0 and
                             early["write_fixup_count"] == 1 and
                             early["read_fixup_probe_rips"] == [] and
                             early["read_fixup_rips"] == [] and
                             early["read_fixup_sites"] == [] and
                             early["write_fixup_probe_rips"] == [write_probe] and
                             early["write_fixup_rips"] == [write_fixup] and
                             early["write_fixup_sites"] == [write_site] and
                             len(early["gp_events"]) == 1 and
                             gp_context_valid and
                             "rdmsr_limited=1" in transcript)
            raw_record_valid = (
                transcript.count("[ARCH_TEST][CPUID]") == 7 and
                transcript.count("[ARCH_TEST][MSR]") == 4 and
                transcript.count("[ARCH_TEST][BUDGET] status=PASS") == 1 and
                transcript.count("[ARCH_TEST][PIN] status=PASS") == 1)
            run_ok = (
                trace_valid and final_valid and raw_record_valid and
                early["early_done"] == 1 and
                early["early_pass"] == 1 and msr_valid and
                transcript.count("[ARCH_TEST][EARLY] status=PASS") == 1 and
                transcript.count("[ARCH_TEST][TRACE_ACTIVE] status=READY") == 1 and
                transcript.count("[ARCH_TEST][RUNTIME] status=PASS") == 1)
            result["msr_observation_valid"] = msr_valid
        result["status_before_cleanup"] = qmp.command(
            "query-status", {}, time.monotonic() + 3)
        result["status"] = "PASS" if run_ok else "FAIL"
        result["host_return_code"] = 0 if run_ok else 1
    except Exception as error:
        result["error"] = str(error)
    finally:
        cleanup = {"method": None, "started": time.monotonic()}
        if qmp and process and process.poll() is None:
            try:
                qmp.command("quit", {}, time.monotonic() + 3)
                cleanup["method"] = "qmp-quit"
            except Exception as error:
                cleanup["qmp_error"] = str(error)
        if process:
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.terminate()
                cleanup["method"] = cleanup["method"] or "sigterm"
                process.wait(timeout=3)
            result["qemu_process_return_code"] = process.returncode
        cleanup["completed"] = time.monotonic()
        result["cleanup"] = cleanup
        if qmp:
            result["qmp_messages"] = len(qmp.messages)
            qmp.close()
        if stderr_handle:
            stderr_handle.close()
        shutil.rmtree(socket_dir, ignore_errors=True)
        if working.exists():
            working.unlink()
        result["host_completed"] = time.time()
        artifacts = {}
        for path in run_dir.iterdir():
            if path.is_file() and path.name not in ("working.img",
                                                    "result.json"):
                artifacts[path.name] = {
                    "bytes": path.stat().st_size, "sha256": sha256(path)}
        result["artifacts"] = artifacts
        atomic_json(run_dir / "result.json", result)
    print(json.dumps({"status": result["status"],
                      "run_dir": str(run_dir)}, sort_keys=True))
    return result["host_return_code"]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-dir", required=True)
    parser.add_argument("--image", required=True)
    parser.add_argument("--elf", required=True)
    parser.add_argument("--accel", choices=("tcg", "kvm"), required=True)
    parser.add_argument("--smp", type=int, required=True)
    parser.add_argument("--negative", action="store_true")
    parser.add_argument("--production", action="store_true")
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--boot-timeout", type=float, default=60)
    parser.add_argument("--post-timeout", type=float, default=30)
    args = parser.parse_args()
    if args.negative and args.production:
        parser.error("--negative and --production are mutually exclusive")
    try:
        return run(args)
    except Exception as error:
        parser.error(str(error))


if __name__ == "__main__":
    raise SystemExit(main())
