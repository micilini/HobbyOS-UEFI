#!/usr/bin/env python3
"""Run one isolated panic scenario with QMP and the QEMU gdbstub."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import select
import shutil
import socket
import subprocess
import sys
import tempfile
import time


SCENARIOS = {
    "simple": 1,
    "exception": 2,
    "contention": 3,
    "reentry": 4,
    "second-reentry": 5,
    "clock-stalled": 6,
    "clock-invalid": 7,
    "uart-unresponsive": 8,
}
ACTIONS = {"halt": 0, "restart": 1, "shutdown": 2}
CLOCK_MODES = {
    "normal": 0,
    "constant": 1,
    "invalid": 2,
    "regressing": 3,
    "intermittent": 4,
}
LOCK_MASKS = {"none": 0, "console": 1, "clock": 2, "both": 3}


STATE_OFFSETS = {
    "armed": 0,
    "scenario": 4,
    "owner_slot": 8,
    "peer_slot": 12,
    "lock_mask": 16,
    "lock_ack_mask": 20,
    "fired": 24,
    "arm_error": 28,
    "action": 32,
    "timeout_seconds": 36,
    "clock_mode": 40,
    "owner_observed_slot": 44,
    "peer_observed_slot": 48,
    "owner_cpu_id": 52,
    "peer_cpu_id": 56,
    "reentry_faults": 60,
    "console_lock_addr": 64,
    "clock_lock_addr": 72,
    "console_lock_value": 80,
    "clock_lock_value": 88,
    "counter_value": 96,
    "countdown_samples": 104,
}


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def atomic_json(path, value):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")
    os.replace(temporary, path)


def wait_path(path, deadline):
    while time.monotonic() < deadline:
        if path.exists():
            return True
        time.sleep(0.05)
    return False


def wait_serial(path, marker, deadline, qmp=None):
    while time.monotonic() < deadline:
        if path.exists() and marker in path.read_text(errors="replace"):
            return True
        if qmp:
            qmp.poll(0)
        time.sleep(0.05)
    return False


def complete_serial_records(path):
    if not path.exists():
        return b"", []
    raw = path.read_bytes()
    complete = raw.split(b"\n")
    if not raw.endswith(b"\n"):
        complete = complete[:-1]
    elif complete and complete[-1] == b"":
        complete = complete[:-1]
    return raw, [line.rstrip(b"\r").decode(errors="replace")
                 for line in complete]


def observe_boot_readiness(serial_path, marker, started, deadline,
                           checkpoint_seconds=0.0, qmp=None, process=None,
                           hold_ready_until_checkpoint=False,
                           clock=time.monotonic, sleeper=time.sleep):
    checkpoint_deadline = (started + checkpoint_seconds
                           if checkpoint_seconds > 0 else None)
    checkpoint = None
    ready = None

    def sample(now):
        raw, lines = complete_serial_records(serial_path)
        matches = [(index + 1, line) for index, line in enumerate(lines)
                   if line == marker or line.startswith(marker + " ")]
        return {
            "host_monotonic": now,
            "elapsed_seconds": now - started,
            "serial_bytes": len(raw),
            "complete_lines": len(lines),
            "last_complete_line": lines[-1] if lines else None,
            "ready_count": len(matches),
            "ready_line_number": matches[0][0] if len(matches) == 1 else None,
            "ready_line": matches[0][1] if len(matches) == 1 else None,
        }

    while True:
        now = clock()
        current = sample(now)
        if current["ready_count"] > 1:
            return {
                "schema": 1,
                "status": "duplicate-ready-marker",
                "marker": marker,
                "started_monotonic": started,
                "deadline_monotonic": deadline,
                "deadline_seconds": deadline - started,
                "checkpoint_seconds": checkpoint_seconds,
                "checkpoint": checkpoint,
                "ready": ready,
                "final": current,
            }
        if current["ready_count"] == 1 and ready is None:
            ready = dict(current)
        if checkpoint_deadline is not None and checkpoint is None and (
                now >= checkpoint_deadline):
            checkpoint = dict(current)
        checkpoint_complete = (checkpoint_deadline is None or
                               checkpoint is not None)
        if ready is not None and (
                not hold_ready_until_checkpoint or checkpoint_complete):
            return {
                "schema": 1,
                "status": "ready",
                "marker": marker,
                "started_monotonic": started,
                "deadline_monotonic": deadline,
                "deadline_seconds": deadline - started,
                "checkpoint_seconds": checkpoint_seconds,
                "checkpoint": checkpoint,
                "ready": ready,
                "final": current,
            }
        if process is not None and process.poll() is not None:
            return {
                "schema": 1,
                "status": "qemu-exited-before-ready",
                "marker": marker,
                "started_monotonic": started,
                "deadline_monotonic": deadline,
                "deadline_seconds": deadline - started,
                "checkpoint_seconds": checkpoint_seconds,
                "checkpoint": checkpoint,
                "ready": ready,
                "final": current,
                "qemu_return_code": process.poll(),
            }
        if now >= deadline:
            return {
                "schema": 1,
                "status": "deadline",
                "marker": marker,
                "started_monotonic": started,
                "deadline_monotonic": deadline,
                "deadline_seconds": deadline - started,
                "checkpoint_seconds": checkpoint_seconds,
                "checkpoint": checkpoint,
                "ready": ready,
                "final": current,
            }
        if qmp:
            qmp.poll(0)
        sleeper(min(0.05, max(0.0, deadline - now)))


def elf_symbols(elf):
    result = subprocess.run(
        ["nm", "-n", "-S", str(elf)], text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
    symbols = {}
    for line in result.stdout.splitlines():
        fields = line.split()
        if len(fields) >= 4:
            try:
                address = int(fields[0], 16)
                size = int(fields[1], 16)
            except ValueError:
                continue
            symbols[fields[3]] = {"address": address, "size": size,
                                  "type": fields[2]}
        elif len(fields) == 3:
            try:
                address = int(fields[0], 16)
            except ValueError:
                continue
            symbols[fields[2]] = {"address": address, "size": 0,
                                  "type": fields[1]}
    return symbols


class QmpClient:
    def __init__(self, path, log_path, deadline):
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        while True:
            try:
                self.socket.connect(str(path))
                break
            except OSError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.05)
        self.socket.setblocking(False)
        self.buffer = b""
        self.messages = []
        self.log_path = log_path
        greeting = self.wait_message(lambda item: "QMP" in item, deadline)
        if not greeting:
            raise RuntimeError("QMP greeting was not received")
        self.command("qmp_capabilities", {}, deadline)

    def _record(self, item):
        entry = {"host_monotonic": time.monotonic(), "message": item}
        self.messages.append(entry)
        with self.log_path.open("a") as stream:
            stream.write(json.dumps(entry, sort_keys=True) + "\n")

    def poll(self, timeout):
        ready, _, _ = select.select([self.socket], [], [], timeout)
        if not ready:
            return []
        try:
            chunk = self.socket.recv(65536)
        except BlockingIOError:
            return []
        if not chunk:
            return []
        self.buffer += chunk
        received = []
        while b"\n" in self.buffer:
            raw, self.buffer = self.buffer.split(b"\n", 1)
            if not raw.strip():
                continue
            item = json.loads(raw.decode(errors="strict"))
            self._record(item)
            received.append(item)
        return received

    def wait_message(self, predicate, deadline):
        checked = 0
        while time.monotonic() < deadline:
            while checked < len(self.messages):
                item = self.messages[checked]["message"]
                checked += 1
                if predicate(item):
                    return item
            self.poll(min(0.1, max(0, deadline - time.monotonic())))
        return None

    def command(self, name, arguments, deadline):
        command_id = f"cmd-{len(self.messages)}-{time.monotonic_ns()}"
        request = {"execute": name, "id": command_id}
        if arguments:
            request["arguments"] = arguments
        self.socket.sendall((json.dumps(request) + "\n").encode())
        response = self.wait_message(
            lambda item: item.get("id") == command_id, deadline)
        if response is None:
            raise RuntimeError(f"QMP command timed out: {name}")
        if "error" in response:
            raise RuntimeError(f"QMP command failed: {response['error']}")
        return response.get("return")

    def events(self, name):
        return [entry for entry in self.messages
                if entry["message"].get("event") == name]

    def close(self):
        self.socket.close()


def gdb_run(elf, socket_path, commands, output_path, timeout=15):
    command_path = output_path.with_suffix(".cmd")
    full = [
        "set confirm off",
        "set pagination off",
        "set print elements 0",
        f"target remote {socket_path}",
        *commands,
        "detach",
        "quit",
    ]
    command_path.write_text("\n".join(full) + "\n")
    argv = ["gdb", "-q", "-batch", str(elf)]
    for command in full:
        argv.extend(["-ex", command])
    result = subprocess.run(argv, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=timeout)
    output_path.write_text(result.stdout)
    if result.returncode != 0:
        raise RuntimeError(
            f"gdb failed with {result.returncode}; see {output_path}")
    return result.stdout


def set_u32(address, value):
    return f"set {{unsigned int}}0x{address:x} = {value}"


def set_u64(address, value):
    return f"set {{unsigned long long}}0x{address:x} = {value}"


def state_address(base, name):
    return base + STATE_OFFSETS[name]


def arm_commands(symbols, args):
    required = ["g_panic_test_state", "g_console_lock",
                "g_clock_event_lock"]
    missing = [name for name in required if name not in symbols]
    if missing:
        raise RuntimeError("instrumentation symbols missing: " +
                           ", ".join(missing))
    state = symbols["g_panic_test_state"]["address"]
    values32 = {
        "scenario": SCENARIOS[args.scenario],
        "owner_slot": args.owner_slot,
        "peer_slot": args.peer_slot,
        "lock_mask": LOCK_MASKS[args.locks],
        "action": ACTIONS[args.action],
        "timeout_seconds": args.countdown,
        "clock_mode": CLOCK_MODES[args.clock_mode],
    }
    commands = []
    for name, value in values32.items():
        commands.append(set_u32(state_address(state, name), value))
    commands.extend([
        set_u64(state_address(state, "console_lock_addr"),
                symbols["g_console_lock"]["address"]),
        set_u64(state_address(state, "clock_lock_addr"),
                symbols["g_clock_event_lock"]["address"]),
        set_u64(state_address(state, "counter_value"),
                args.counter_value),
        set_u32(state_address(state, "armed"), 1),
        f'printf "ARMED_STATE=0x%x\\n", *(unsigned int*)0x{state:x}',
        f'printf "CONSOLE_LOCK_ADDR=0x%lx\\n", '
        f'*(unsigned long long*)0x{state_address(state, "console_lock_addr"):x}',
        f'printf "CLOCK_LOCK_ADDR=0x%lx\\n", '
        f'*(unsigned long long*)0x{state_address(state, "clock_lock_addr"):x}',
    ])
    return commands


def state_snapshot_commands(symbols):
    commands = ["info threads", "thread apply all info registers rip eflags",
                "thread apply all info symbol $rip"]
    state = symbols.get("g_panic_test_state", {}).get("address")
    owner = symbols.get("g_panic_owner", {}).get("address")
    if owner is not None:
        commands.append(
            f'printf "PANIC_OWNER_STATE=0x%lx\\n", '
            f'*(unsigned long long*)0x{owner:x}')
    if state is not None:
        for name in (
            "armed", "scenario", "owner_slot", "peer_slot", "lock_mask",
            "lock_ack_mask", "fired", "arm_error", "action",
            "clock_mode", "owner_observed_slot", "peer_observed_slot",
            "owner_cpu_id", "peer_cpu_id", "reentry_faults"):
            address = state_address(state, name)
            commands.append(
                f'printf "STATE_{name.upper()}=%u\\n", '
                f'*(unsigned int*)0x{address:x}')
        for name in ("console_lock_addr", "clock_lock_addr",
                     "console_lock_value", "clock_lock_value",
                     "counter_value", "countdown_samples"):
            address = state_address(state, name)
            commands.append(
                f'printf "STATE_{name.upper()}=0x%lx\\n", '
                f'*(unsigned long long*)0x{address:x}')
    for name in ("g_panic_test_uart_unresponsive",
                 "g_panic_test_uart_polls"):
        item = symbols.get(name)
        if item:
            if item["size"] <= 4:
                commands.append(
                    f'printf "{name.upper()}=%u\\n", '
                    f'*(unsigned int*)0x{item["address"]:x}')
            else:
                commands.append(
                    f'printf "{name.upper()}=0x%lx\\n", '
                    f'*(unsigned long long*)0x{item["address"]:x}')
    return commands


def hpet_freeze_commands(symbols):
    item = symbols.get("g_hpet_base")
    if not item:
        raise RuntimeError("g_hpet_base symbol is missing")
    address = item["address"]
    return [
        f"set $hpet = *(unsigned long long*)0x{address:x}",
        "maintenance packet Qqemu.PhyMemMode:1",
        "set $hpet_cfg = *(unsigned long long*)($hpet + 0x10)",
        "set $hpet_low = *(unsigned int*)($hpet + 0xf0)",
        "set $hpet_high = *(unsigned int*)($hpet + 0xf4)",
        'printf "HPET_BASE=0x%lx\\n", $hpet',
        'printf "HPET_CONFIG_BEFORE=0x%lx\\n", $hpet_cfg',
        'printf "HPET_COUNTER=0x%lx\\n", '
        "((unsigned long long)$hpet_high << 32) | $hpet_low",
        "set {unsigned long long}($hpet + 0x10) = $hpet_cfg & ~1",
        'printf "HPET_CONFIG_AFTER=0x%lx\\n", '
        "*(unsigned long long*)($hpet + 0x10)",
        "maintenance packet Qqemu.PhyMemMode:0",
    ]


def hpet_sample_commands(symbols):
    address = symbols["g_hpet_base"]["address"]
    return [
        f"set $hpet = *(unsigned long long*)0x{address:x}",
        "maintenance packet Qqemu.PhyMemMode:1",
        "set $hpet_low = *(unsigned int*)($hpet + 0xf0)",
        "set $hpet_high = *(unsigned int*)($hpet + 0xf4)",
        'printf "HPET_CONFIG=0x%lx\\n", '
        "*(unsigned long long*)($hpet + 0x10)",
        'printf "HPET_COUNTER=0x%lx\\n", '
        "((unsigned long long)$hpet_high << 32) | $hpet_low",
        "maintenance packet Qqemu.PhyMemMode:0",
    ]


def firmware_arguments(run_dir):
    explicit = os.environ.get("OVMF_FD")
    if explicit:
        path = Path(explicit).resolve()
        if not path.is_file():
            raise RuntimeError(f"OVMF_FD does not exist: {path}")
        return ["-bios", str(path)], {"kind": "combined", "path": str(path)}
    for candidate in ("/usr/share/ovmf/OVMF.fd",
                      "/usr/share/OVMF/OVMF.fd",
                      "/usr/share/qemu/OVMF.fd"):
        path = Path(candidate)
        if path.is_file():
            return ["-bios", str(path)], {
                "kind": "combined", "path": str(path)}
    for directory in ("/usr/share/OVMF", "/usr/share/ovmf",
                      "/usr/share/edk2/x64"):
        code = Path(directory) / "OVMF_CODE.fd"
        variables = Path(directory) / "OVMF_VARS.fd"
        if code.is_file() and variables.is_file():
            copy = run_dir / "OVMF_VARS.fd"
            shutil.copy2(variables, copy)
            return [
                "-drive", f"if=pflash,format=raw,readonly=on,file={code}",
                "-drive", f"if=pflash,format=raw,file={copy}",
            ], {"kind": "split", "code": str(code),
                "vars_source": str(variables), "vars_copy": str(copy)}
    raise RuntimeError("OVMF firmware was not found")


def event_after(qmp, name, started):
    for entry in qmp.events(name):
        if entry["host_monotonic"] >= started:
            return entry
    return None


def parse_hex_marker(text, name):
    prefix = name + "=0x"
    for line in text.splitlines():
        if line.startswith(prefix):
            try:
                return int(line[len(prefix):].strip(), 16)
            except ValueError:
                return None
    return None


def halt_snapshot(elf, gdb_socket, symbols, run_dir, index, smp):
    output_path = run_dir / f"gdb-halt-{index}.log"
    started = time.monotonic()
    text = gdb_run(elf, gdb_socket, state_snapshot_commands(symbols),
                   output_path)
    eflags = []
    for line in text.splitlines():
        fields = line.strip().split()
        if len(fields) >= 2 and fields[0] == "eflags" and fields[1].startswith("0x"):
            try:
                eflags.append(int(fields[1], 16))
            except ValueError:
                pass
    owner_state = parse_hex_marker(text, "PANIC_OWNER_STATE")
    halted_cpus = sorted({int(value) for value in re.findall(
        r"CPU#([0-9]+) \[halted\s*\]", text)})
    halt_rips = len(re.findall(
        r"^panic_halt_secondary(?: \+ [0-9]+)? in section \.text$",
        text, re.MULTILINE))
    return {
        "host_monotonic": started,
        "path": str(output_path),
        "sha256": sha256(output_path),
        "eflags": eflags,
        "if_clear": len(eflags) >= smp and
                    all((value & 0x200) == 0 for value in eflags),
        "halted_cpus": halted_cpus,
        "all_cpus_halted": halted_cpus == list(range(smp)),
        "halt_rip_count": halt_rips,
        "all_rips_in_terminal_halt": halt_rips >= smp,
        "owner_state": owner_state,
        "owner_depth": None if owner_state is None else owner_state & 0xFFFFFFFF,
        "halt_symbols": text.count("panic_halt_secondary"),
    }


def launch_arguments(args, run_dir, socket_dir, working_image):
    firmware, firmware_info = firmware_arguments(run_dir)
    accelerator = args.accel
    accel_arg = accelerator if accelerator == "kvm" else "tcg,thread=multi"
    cpu = "host" if accelerator == "kvm" else "max"
    qmp_socket = socket_dir / "qmp.sock"
    hmp_socket = socket_dir / "hmp.sock"
    gdb_socket = socket_dir / "gdb.sock"
    command = [
        args.qemu,
        "-machine", args.machine,
        "-accel", accel_arg,
        "-cpu", cpu,
        "-smp", f"{args.smp},sockets=1,cores={args.smp},threads=1",
        "-m", args.memory,
        *firmware,
        "-net", "none",
        "-drive", f"file={working_image},format=raw,cache=writeback",
        "-serial", f"file:{run_dir / 'serial-transcript.log'}",
        "-debugcon", f"file:{run_dir / 'debugcon.log'}",
        "-global", "isa-debugcon.iobase=0x402",
        "-d", "guest_errors",
        "-D", str(run_dir / "qemu-trace.log"),
        "-display", "none",
        "-monitor", f"unix:{hmp_socket},server=on,wait=off",
        "-qmp", f"unix:{qmp_socket},server=on,wait=off",
        "-chardev", f"socket,path={gdb_socket},server=on,wait=off,id=gdb0",
        "-gdb", "chardev:gdb0",
        "-no-shutdown",
    ]
    if args.machine == "q35":
        command.extend([
            "-device", "nec-usb-xhci,id=xhci,msi=on,msix=off",
            "-device", "usb-kbd,bus=xhci.0",
        ])
    return command, firmware_info, qmp_socket, hmp_socket, gdb_socket


def validate_arguments(args):
    if args.smp < 1:
        raise ValueError("--smp must be positive")
    if args.owner_slot < 0 or args.owner_slot >= args.smp:
        raise ValueError("--owner-slot is outside the virtual CPU topology")
    if args.locks != "none" and (
            args.peer_slot < 0 or args.peer_slot >= args.smp or
            args.peer_slot == args.owner_slot):
        raise ValueError("lock scenarios require a distinct valid peer slot")
    if args.accel == "kvm" and not (
            os.access("/dev/kvm", os.R_OK | os.W_OK)):
        raise ValueError("KVM was requested but /dev/kvm is unavailable")
    if args.production and args.scenario != "simple":
        raise ValueError("production mode supports only the simple scenario")
    if args.negative_control and args.production:
        raise ValueError("negative control requires an instrumented image")
    if bool(args.production_preflight_command) != bool(
            args.production_preflight_marker):
        raise ValueError("production preflight command and marker must be used together")
    if args.production_preflight_command and not args.production:
        raise ValueError("production preflight is available only in production mode")
    if args.boot_checkpoint < 0 or args.boot_checkpoint >= args.boot_timeout:
        raise ValueError("boot checkpoint must be non-negative and below boot timeout")
    if args.observe_boot_only and (
            args.production or args.negative_control or args.freeze_hpet or
            args.production_preflight_command):
        raise ValueError("boot-only observation requires an instrumented normal boot")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--run-dir", required=True)
    parser.add_argument("--image", required=True)
    parser.add_argument("--elf", required=True)
    parser.add_argument("--scenario", choices=SCENARIOS, default="simple")
    parser.add_argument("--action", choices=ACTIONS, default="halt")
    parser.add_argument("--locks", choices=LOCK_MASKS, default="none")
    parser.add_argument("--clock-mode", choices=CLOCK_MODES, default="normal")
    parser.add_argument("--counter-value", type=lambda value: int(value, 0),
                        default=0x123456789)
    parser.add_argument("--countdown", type=int, default=1)
    parser.add_argument("--owner-slot", type=int, default=0)
    parser.add_argument("--peer-slot", type=int, default=1)
    parser.add_argument("--machine", choices=("q35", "pc"), default="q35")
    parser.add_argument("--smp", type=int, default=4)
    parser.add_argument("--accel", choices=("tcg", "kvm"), default="tcg")
    parser.add_argument("--memory", default="2G")
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--boot-timeout", type=float, default=45.0)
    parser.add_argument("--boot-checkpoint", type=float, default=0.0)
    parser.add_argument("--post-timeout", type=float, default=20.0)
    parser.add_argument("--cleanup-timeout", type=float, default=3.0)
    parser.add_argument("--freeze-hpet", action="store_true")
    parser.add_argument("--negative-control", action="store_true")
    parser.add_argument("--production", action="store_true")
    parser.add_argument("--observe-boot-only", action="store_true")
    parser.add_argument("--production-command",
                        default="panic normal image smoke")
    parser.add_argument("--production-preflight-command", default="")
    parser.add_argument("--production-preflight-marker", default="")
    args = parser.parse_args()

    try:
        validate_arguments(args)
    except ValueError as error:
        parser.error(str(error))

    run_dir = Path(args.run_dir).resolve()
    if run_dir.exists():
        parser.error(f"run directory already exists: {run_dir}")
    run_dir.mkdir(parents=True)
    image = Path(args.image).resolve()
    elf = Path(args.elf).resolve()
    if not image.is_file() or not elf.is_file():
        parser.error("--image and --elf must name existing files")

    symbols = elf_symbols(elf)
    if not args.production:
        required = {"g_panic_test_state", "g_console_lock",
                    "g_clock_event_lock"}
        if not required.issubset(symbols):
            parser.error("the ELF does not contain panic instrumentation")

    working_image = run_dir / "working.img"
    subprocess.run(["cp", "--reflink=auto", str(image), str(working_image)],
                   check=True)
    image_before = sha256(working_image)
    elf_hash = sha256(elf)
    version = subprocess.run([args.qemu, "--version"], text=True,
                             stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT,
                             check=True).stdout.splitlines()[0]
    socket_dir = Path(tempfile.mkdtemp(prefix="hobbyos-panic-"))
    os.chmod(socket_dir, 0o700)

    command, firmware, qmp_path, hmp_path, gdb_path = launch_arguments(
        args, run_dir, socket_dir, working_image)
    launch = {
        "command": command,
        "qemu_version": version,
        "machine": args.machine,
        "accel": args.accel,
        "cpu": "host" if args.accel == "kvm" else "max",
        "smp": args.smp,
        "memory": args.memory,
        "firmware": firmware,
        "image_source": str(image),
        "image_sha256": image_before,
        "elf": str(elf),
        "elf_sha256": elf_hash,
        "socket_directory": str(socket_dir),
        "socket_mode": "0700",
    }
    atomic_json(run_dir / "launch.json", launch)
    (run_dir / "launch-command.txt").write_text(
        "\0".join(command) + "\n")

    result = {
        "schema": 1,
        "scenario": args.scenario,
        "action": args.action,
        "locks": args.locks,
        "clock_mode": args.clock_mode,
        "freeze_hpet": args.freeze_hpet,
        "negative_control": args.negative_control,
        "production": args.production,
        "observe_boot_only": args.observe_boot_only,
        "profile": {"machine": args.machine, "accel": args.accel,
                    "smp": args.smp, "memory": args.memory,
                    "cpu": launch["cpu"]},
        "image_sha256_before": image_before,
        "elf_sha256": elf_hash,
        "expected_rip": None,
        "host_started": time.time(),
        "host_monotonic_started": time.monotonic(),
        "host_return_code": 1,
        "status": "FAIL",
    }
    if args.scenario == "exception" and "panic_test_exception_ud2_site" in symbols:
        result["expected_rip"] = symbols["panic_test_exception_ud2_site"]["address"]

    qmp = None
    process = None
    stderr_handle = None
    cleanup = {"method": None, "started": None, "completed": None}
    try:
        stderr_handle = (run_dir / "qemu-stderr.log").open("w")
        process = subprocess.Popen(command, stdout=stderr_handle,
                                   stderr=subprocess.STDOUT)
        result["qemu_pid"] = process.pid
        socket_deadline = time.monotonic() + 10.0
        if not wait_path(qmp_path, socket_deadline):
            raise RuntimeError("QMP socket did not appear")
        qmp = QmpClient(qmp_path, run_dir / "qmp-events.jsonl",
                        socket_deadline)
        result["qmp_connected_before_trigger"] = True
        boot_started = time.monotonic()
        boot_deadline = boot_started + args.boot_timeout
        serial_path = run_dir / "serial-transcript.log"
        boot_observation = observe_boot_readiness(
            serial_path, "[BOOT][RUNTIME_READY] PASS", boot_started,
            boot_deadline, args.boot_checkpoint, qmp, process,
            args.observe_boot_only)
        atomic_json(run_dir / "boot-observation.json", boot_observation)
        result["boot_observation"] = boot_observation
        if boot_observation["status"] != "ready":
            raise RuntimeError(
                "boot/runtime readiness " + boot_observation["status"])
        result["runtime_ready_monotonic"] = (
            boot_observation["ready"]["host_monotonic"])
        result["status_before_trigger"] = qmp.command(
            "query-status", {}, time.monotonic() + 3.0)

        if args.observe_boot_only:
            result["status"] = "BOOT_READY_OBSERVED"
            result["host_return_code"] = 0
            result["status_before_cleanup"] = result["status_before_trigger"]
            print(json.dumps({"status": result["status"],
                              "host_return_code": 0,
                              "run_dir": str(run_dir)}, sort_keys=True))
            return 0

        hpet = None
        if args.freeze_hpet:
            first_path = run_dir / "gdb-hpet-freeze.log"
            first = gdb_run(elf, gdb_path, hpet_freeze_commands(symbols),
                            first_path)
            time.sleep(0.25)
            second_path = run_dir / "gdb-hpet-sample-1.log"
            second = gdb_run(elf, gdb_path, hpet_sample_commands(symbols),
                             second_path)
            time.sleep(0.25)
            third_path = run_dir / "gdb-hpet-sample-2.log"
            third = gdb_run(elf, gdb_path, hpet_sample_commands(symbols),
                            third_path)
            hpet = {
                "freeze_log": str(first_path),
                "sample_logs": [str(second_path), str(third_path)],
                "counter_before": parse_hex_marker(first, "HPET_COUNTER"),
                "counter_after_1": parse_hex_marker(second, "HPET_COUNTER"),
                "counter_after_2": parse_hex_marker(third, "HPET_COUNTER"),
                "config_before": parse_hex_marker(first,
                                                   "HPET_CONFIG_BEFORE"),
                "config_after": parse_hex_marker(first,
                                                  "HPET_CONFIG_AFTER"),
                "config_observed_1": parse_hex_marker(second,
                                                       "HPET_CONFIG"),
                "config_observed_2": parse_hex_marker(third,
                                                       "HPET_CONFIG"),
            }
            hpet["stable"] = (
                hpet["counter_after_1"] == hpet["counter_after_2"])
            hpet["disabled"] = (hpet["config_after"] is not None and
                                (hpet["config_after"] & 1) == 0 and
                                hpet["config_observed_1"] ==
                                hpet["config_after"] and
                                hpet["config_observed_2"] ==
                                hpet["config_after"])
            result["hpet"] = hpet

        if args.production_preflight_command:
            if not wait_path(hmp_path, time.monotonic() + 3.0):
                raise RuntimeError("HMP socket did not appear")
            preflight_record = run_dir / "production-preflight-command.jsonl"
            observer_output = run_dir / "production-preflight-observer.log"
            observer_command = [
                sys.executable,
                str(Path(__file__).resolve().parent / "foundation-qemu.py"),
                "production-command", "--root",
                str(Path(__file__).resolve().parent.parent), "--runtime",
                str(socket_dir), "--serial-path", str(serial_path),
                "--hmp-socket", str(hmp_path), "--qemu-pid",
                str(process.pid), "--observer-arm-file",
                str(run_dir / "production-preflight-observer.armed"),
                "--gdb-socket", str(gdb_path), "--elf", str(elf),
                "--record", str(preflight_record), "--gdb-log",
                str(run_dir / "gdb-production-preflight.log"),
                "--transport-log", str(run_dir / "hmp-preflight.log"),
                "--profile-id", run_dir.name, "--vm-id", socket_dir.name,
                "--candidate-sha256", elf_hash, "--sequence", "1",
                "--payload", args.production_preflight_command,
                "--timeout", "30", "--allowed-status", "0",
                "--input-profile", "sync",
            ]
            (run_dir / "production-preflight-observer.argv.json").write_text(
                json.dumps(observer_command, indent=2) + "\n")
            observed = subprocess.run(
                observer_command, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, timeout=40)
            observer_output.write_text(observed.stdout)
            rows = [json.loads(line) for line in
                    preflight_record.read_text().splitlines() if line.strip()]
            if len(rows) != 1:
                raise RuntimeError("production preflight has no unique record")
            preflight = rows[0]
            preflight["command"] = args.production_preflight_command
            preflight["marker"] = args.production_preflight_marker
            preflight["observer_return_code"] = observed.returncode
            lines = serial_path.read_text(errors="replace").splitlines()
            start = preflight.get("start_line_count", -1)
            end = preflight.get("end_line_count", -1)
            segment = lines[start:end] if (isinstance(start, int) and
                                           isinstance(end, int) and
                                           0 <= start <= end <= len(lines)) else []
            markers = [start + index + 1 for index, line in enumerate(segment)
                       if (line == args.production_preflight_marker or
                           line.startswith(args.production_preflight_marker + " "))]
            preflight["observed"] = len(markers) == 1
            preflight["marker_line"] = markers[0] if markers else None
            result["production_preflight"] = preflight
            if (observed.returncode != 0 or
                    preflight.get("classification") != "PASS" or
                    preflight.get("handler_status") != 0 or
                    not preflight["observed"]):
                raise RuntimeError(
                    "production preflight exact handler observation failed")

        trigger_started = time.monotonic()
        result["trigger_monotonic"] = trigger_started
        if args.production:
            if not wait_path(hmp_path, time.monotonic() + 3.0):
                raise RuntimeError("HMP socket did not appear")
            hmp = subprocess.run([
                sys.executable, "scripts/qemu_hmp.py", "--socket",
                str(hmp_path), "text", args.production_command, "--enter",
                "--profile", "sync"], text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, timeout=15)
            (run_dir / "hmp-trigger.log").write_text(hmp.stdout)
            if hmp.returncode != 0:
                raise RuntimeError("HMP production trigger failed")
        else:
            arm_path = run_dir / "gdb-arm.log"
            gdb_run(elf, gdb_path, arm_commands(symbols, args), arm_path)

        terminal_event = None
        post_deadline = time.monotonic() + args.post_timeout
        while time.monotonic() < post_deadline:
            qmp.poll(0.05)
            if args.action == "restart":
                terminal_event = event_after(qmp, "RESET", trigger_started)
            elif args.action == "shutdown":
                terminal_event = event_after(qmp, "SHUTDOWN", trigger_started)
            else:
                transcript = serial_path.read_text(errors="replace")
                if args.scenario == "second-reentry":
                    ready = "[PANIC][REENTRY]" in transcript
                elif args.scenario == "uart-unresponsive":
                    ready = "[PANIC_TEST][TRIGGER]" in transcript
                elif args.negative_control:
                    ready = False
                else:
                    ready = "[PANIC][ACTION] requested=halt" in transcript
                if ready:
                    time.sleep(0.35)
                    break
            if terminal_event:
                break
            if process.poll() is not None:
                qmp.poll(0)
                break

        transcript = serial_path.read_text(errors="replace")
        if args.negative_control:
            negative_path = run_dir / "gdb-negative.log"
            negative_text = gdb_run(
                elf, gdb_path, state_snapshot_commands(symbols),
                negative_path)
            detected = (
                "[PANIC_TEST][NEGATIVE] waiting_for_lock=console" in transcript
                and "[PANIC][DUMP_END]" not in transcript
                and event_after(qmp, "RESET", trigger_started) is None
                and event_after(qmp, "SHUTDOWN", trigger_started) is None)
            result["terminal_evidence"] = {
                "kind": "negative-timeout", "detected": detected,
                "deadline_seconds": args.post_timeout,
                "snapshot": str(negative_path),
                "snapshot_sha256": sha256(negative_path),
                "owner_state": parse_hex_marker(
                    negative_text, "PANIC_OWNER_STATE")}
            result["status"] = "NEGATIVE_DETECTED" if detected else "FAIL"
            result["host_return_code"] = 0 if detected else 1
        elif args.action in ("restart", "shutdown"):
            name = "RESET" if args.action == "restart" else "SHUTDOWN"
            terminal_event = event_after(qmp, name, trigger_started)
            message = terminal_event["message"] if terminal_event else None
            guest = bool(message and message.get("data", {}).get("guest"))
            result["terminal_evidence"] = {
                "kind": args.action, "event": message,
                "event_monotonic": terminal_event["host_monotonic"]
                    if terminal_event else None,
                "guest_origin": guest}
            result["status"] = "PASS" if terminal_event and guest else "FAIL"
            result["host_return_code"] = 0 if result["status"] == "PASS" else 1
        else:
            first = halt_snapshot(elf, gdb_path, symbols, run_dir, 1,
                                  args.smp)
            time.sleep(0.25)
            second = halt_snapshot(elf, gdb_path, symbols, run_dir, 2,
                                   args.smp)
            if args.scenario == "second-reentry":
                expected_depth = 3
            elif args.scenario == "reentry":
                expected_depth = 2
            else:
                expected_depth = 1
            depth_ok = second["owner_depth"] == expected_depth
            if args.scenario == "uart-unresponsive":
                marker_ok = "[PANIC_TEST][TRIGGER]" in transcript
            elif args.scenario == "second-reentry":
                marker_ok = transcript.count("[PANIC][REENTRY]") == 1
            else:
                marker_ok = "[PANIC][ACTION] requested=halt" in transcript
            observation_gap = (second["host_monotonic"] -
                               first["host_monotonic"])
            halt_ok = (first["if_clear"] and second["if_clear"] and depth_ok
                       and marker_ok and first["all_cpus_halted"] and
                       second["all_cpus_halted"] and
                       first["all_rips_in_terminal_halt"] and
                       second["all_rips_in_terminal_halt"] and
                       observation_gap >= 0.2)
            result["terminal_evidence"] = {
                "kind": "halt", "first": first, "second": second,
                "if_clear": first["if_clear"] and second["if_clear"],
                "expected_depth": expected_depth,
                "depth_ok": depth_ok,
                "marker_before_inspection": marker_ok,
                "observation_gap_seconds": observation_gap,
            }
            result["status"] = "PASS" if halt_ok else "FAIL"
            result["host_return_code"] = 0 if halt_ok else 1

        try:
            result["status_before_cleanup"] = qmp.command(
                "query-status", {}, time.monotonic() + 3.0)
        except Exception as error:
            result["status_before_cleanup_error"] = str(error)
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
                result["image_sha256_after"] == image_before)
        artifact_names = ["serial-transcript.log", "debugcon.log",
                          "qemu-trace.log", "qmp-events.jsonl",
                          "qemu-stderr.log", "boot-observation.json"]
        if args.production_preflight_command:
            artifact_names.extend([
                "production-preflight-command.jsonl",
                "production-preflight-observer.log",
                "production-preflight-observer.argv.json",
                "production-preflight-observer.armed",
                "gdb-production-preflight.log",
                "gdb-production-preflight.cmd",
                "gdb-production-preflight.argv.json",
                "hmp-preflight.log",
            ])
        for name in artifact_names:
            path = run_dir / name
            if path.exists():
                result.setdefault("artifacts", {})[name] = {
                    "bytes": path.stat().st_size, "sha256": sha256(path)}
        atomic_json(run_dir / "result.json", result)
        shutil.rmtree(socket_dir, ignore_errors=True)

    print(json.dumps({"status": result["status"],
                      "host_return_code": result["host_return_code"],
                      "run_dir": str(run_dir)}, sort_keys=True))
    return result["host_return_code"]


if __name__ == "__main__":
    raise SystemExit(main())
