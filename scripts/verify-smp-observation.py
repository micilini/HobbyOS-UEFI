#!/usr/bin/env python3
"""Verify focused IRQ snapshot and pre-trigger boot observations offline."""

import argparse
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import zlib


READY_MARKER = "[BOOT][RUNTIME_READY] PASS"


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_json(path):
    try:
        value = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"{path}: cannot read JSON: {error}") from error
    if not isinstance(value, dict):
        raise ValueError(f"{path}: expected a JSON object")
    return value


def fields(line):
    values = {}
    for token in line.split():
        if "=" not in token:
            continue
        name, value = token.split("=", 1)
        if name in values:
            raise ValueError(f"duplicate field {name}: {line}")
        values[name] = value
    return values


def integer(value, name):
    if isinstance(value, bool):
        raise ValueError(f"{name} is not an integer")
    try:
        result = value if isinstance(value, int) else int(value, 0)
    except (TypeError, ValueError) as error:
        raise ValueError(f"{name} is not an integer") from error
    if result < 0:
        raise ValueError(f"{name} is negative")
    return result


def validate_irq_run(run_dir):
    command = load_json(run_dir / "command.json")
    serial_path = run_dir / "serial.log"
    if not serial_path.is_file():
        raise ValueError(f"{serial_path}: missing")
    raw = serial_path.read_bytes()
    if raw and not raw.endswith(b"\n"):
        raise ValueError("serial transcript ends with an incomplete record")
    lines = serial_path.read_text(errors="replace").splitlines()
    if command.get("payload") != "irq boot":
        raise ValueError("command payload is not irq boot")
    if command.get("handler_status") != 0 or command.get("classification") != "PASS":
        raise ValueError("irq boot handler did not complete with status zero")
    expected_crc = f"{zlib.crc32(b'irq boot') & 0xffffffff:08x}"
    if command.get("crc32") != expected_crc:
        raise ValueError("irq boot payload checksum is incorrect")
    sequence = integer(command.get("sequence"), "command sequence")
    begin_text = f"[HARNESS][BEGIN] seq={sequence}"
    end_text = f"[HARNESS][END] seq={sequence} status=0"
    begins = [index for index, line in enumerate(lines, 1)
              if line == begin_text]
    ends = [index for index, line in enumerate(lines, 1)
            if line == end_text]
    if len(begins) != 1 or len(ends) != 1 or begins[0] >= ends[0]:
        raise ValueError("command does not have one ordered BEGIN/END pair")
    if command.get("begin_line") not in (None, begins[0]):
        raise ValueError("declared BEGIN line differs from raw serial")
    if command.get("end_line") not in (None, ends[0]):
        raise ValueError("declared END line differs from raw serial")
    segment = lines[begins[0]:ends[0] - 1]
    boot = [line for line in segment if line.startswith("[IRQ][BOOT] ")]
    probes = [line for line in segment
              if line.startswith("[IRQ][BOOT_PROBES] ")]
    clocks = [line for line in segment
              if line.startswith("[IRQ][BOOT_CLOCK] ")]
    cpus = [line for line in segment
            if line.startswith("[IRQ][BOOT_CPU] ")]
    results = [line for line in segment
               if line.startswith("[IRQ][BOOT_RESULT] ")]
    failures = [line for line in segment
                if line.startswith("[IRQ][BOOT_SNAPSHOT] FAIL") or
                line.startswith("[IRQ][BOOT_ERROR]")]
    if (len(boot), len(probes), len(clocks), len(results)) != (1, 1, 1, 1):
        raise ValueError("irq boot record set is incomplete or duplicated")
    if failures:
        raise ValueError("irq boot contains a snapshot or construction failure")
    boot_fields = fields(boot[0])
    ready = boot_fields.get("runtime_ready", "").split("/")
    if len(ready) != 2:
        raise ValueError("runtime_ready is malformed")
    ready_count = integer(ready[0], "runtime_ready numerator")
    expected_count = integer(ready[1], "runtime_ready denominator")
    if ready_count != expected_count or not expected_count:
        raise ValueError("runtime_ready does not cover the topology")
    if len(cpus) != expected_count:
        raise ValueError("BOOT_CPU count differs from the discovered topology")
    seen = set()
    for expected_slot, line in enumerate(cpus):
        row = fields(line)
        slot = integer(row.get("slot"), "BOOT_CPU slot")
        if slot in seen or slot != expected_slot:
            raise ValueError("BOOT_CPU slots are missing, duplicated, or reordered")
        seen.add(slot)
        entered = integer(row.get("entered"), "BOOT_CPU entered")
        returned = integer(row.get("returned"), "BOOT_CPU returned")
        if entered != returned or integer(row.get("depth"), "BOOT_CPU depth") != 0:
            raise ValueError("BOOT_CPU was accepted while inside an interrupt")
        for name in ("handoff", "preempt", "ready"):
            if integer(row.get(name), f"BOOT_CPU {name}") != 1:
                raise ValueError(f"BOOT_CPU {name} is not complete")
    result = fields(results[0])
    if " PASS " not in results[0]:
        raise ValueError("BOOT_RESULT is not PASS")
    if integer(result.get("snapshots"), "BOOT_RESULT snapshots") != expected_count:
        raise ValueError("BOOT_RESULT snapshot count differs from topology")
    if integer(result.get("budget_ms"), "BOOT_RESULT budget_ms") != 5000:
        raise ValueError("BOOT_RESULT collection budget changed")
    acquisition = integer(result.get("acquisition_ms"),
                          "BOOT_RESULT acquisition_ms")
    publication = integer(result.get("publication_ms"),
                          "BOOT_RESULT publication_ms")
    elapsed = integer(result.get("elapsed_ms"), "BOOT_RESULT elapsed_ms")
    if acquisition > 5000:
        raise ValueError("snapshot acquisition exceeded its shared budget")
    if elapsed != acquisition + publication:
        raise ValueError("snapshot timing fields are inconsistent")
    return {
        "sequence": sequence,
        "candidate_sha256": command.get("candidate_sha256"),
        "snapshots": expected_count,
        "polls": integer(result.get("polls"), "BOOT_RESULT polls"),
        "acquisition_ms": acquisition,
        "publication_ms": publication,
        "elapsed_ms": elapsed,
    }


def validate_boot_run(run_dir, expected_elf=None):
    result = load_json(run_dir / "result.json")
    launch = load_json(run_dir / "launch.json")
    observation = load_json(run_dir / "boot-observation.json")
    serial_path = run_dir / "serial-transcript.log"
    qmp_path = run_dir / "qmp-events.jsonl"
    if not serial_path.is_file() or not qmp_path.is_file():
        raise ValueError("boot observation lacks serial or QMP raw evidence")
    if result.get("observe_boot_only") is not True:
        raise ValueError("run is not identified as boot-only observation")
    if result.get("status") != "BOOT_READY_OBSERVED" or \
            result.get("host_return_code") != 0:
        raise ValueError("boot-only observation did not reach readiness")
    profile = result.get("profile", {})
    required_profile = {"machine": "q35", "accel": "kvm", "smp": 24,
                        "memory": "2G", "cpu": "host"}
    if profile != required_profile:
        raise ValueError("boot observation profile is not Q35/KVM/SMP24/2G")
    if any(launch.get(name) != value for name, value in required_profile.items()):
        raise ValueError("launch profile differs from result profile")
    elf_hash = result.get("elf_sha256")
    if launch.get("elf_sha256") != elf_hash:
        raise ValueError("launch/result ELF identity differs")
    if expected_elf is not None and elf_hash != expected_elf:
        raise ValueError("boot run uses a different ELF from the planned candidate")
    if launch.get("image_sha256") != result.get("image_sha256_before"):
        raise ValueError("launch/result image identity differs")
    if not isinstance(result.get("image_sha256_after"), str):
        raise ValueError("boot result lacks the post-run working-image identity")
    if observation != result.get("boot_observation"):
        raise ValueError("raw boot observation differs from result summary")
    if observation.get("status") != "ready" or \
            observation.get("deadline_seconds") != 120 or \
            observation.get("checkpoint_seconds") != 45:
        raise ValueError("boot observation policy differs from 45/120 seconds")
    ready = observation.get("ready")
    checkpoint = observation.get("checkpoint")
    if not isinstance(ready, dict) or not isinstance(checkpoint, dict):
        raise ValueError("boot readiness or 45-second checkpoint is absent")
    ready_elapsed = float(ready.get("elapsed_seconds"))
    if not (0 <= ready_elapsed < 120):
        raise ValueError("ready observation is outside the absolute deadline")
    raw = serial_path.read_bytes()
    if raw and not raw.endswith(b"\n"):
        raise ValueError("boot serial ends with a partial record")
    lines = serial_path.read_text(errors="replace").splitlines()
    matches = [(index + 1, line) for index, line in enumerate(lines)
               if line == READY_MARKER or line.startswith(READY_MARKER + " ")]
    if len(matches) != 1:
        raise ValueError("boot serial lacks one unique complete READY record")
    if (ready.get("ready_line_number"), ready.get("ready_line")) != matches[0]:
        raise ValueError("boot observation READY identity differs from serial")
    checkpoint_count = integer(checkpoint.get("ready_count"),
                               "checkpoint ready_count")
    if (ready_elapsed <= 45 and checkpoint_count != 1) or \
            (ready_elapsed > 45 and checkpoint_count != 0):
        raise ValueError("45-second checkpoint contradicts READY timing")
    if "trigger_monotonic" in result or (run_dir / "gdb-arm.log").exists():
        raise ValueError("boot-only observation armed a panic trigger")
    qmp_entries = []
    for number, line in enumerate(qmp_path.read_text().splitlines(), 1):
        try:
            item = json.loads(line)
        except json.JSONDecodeError as error:
            raise ValueError(f"QMP line {number} is invalid: {error}") from error
        if not isinstance(item, dict):
            raise ValueError("QMP evidence contains a non-object")
        qmp_entries.append(item)
    if result.get("qmp_messages") != len(qmp_entries):
        raise ValueError("QMP message count differs from raw evidence")
    artifacts = result.get("artifacts", {})
    required = {"serial-transcript.log", "qmp-events.jsonl",
                "boot-observation.json", "debugcon.log", "qemu-trace.log",
                "qemu-stderr.log"}
    if not isinstance(artifacts, dict) or not required.issubset(artifacts):
        raise ValueError("boot result lacks required artifact identities")
    for name, identity in artifacts.items():
        path = run_dir / name
        if not path.is_file() or path.stat().st_size != identity.get("bytes") or \
                sha256(path) != identity.get("sha256"):
            raise ValueError(f"boot artifact identity differs: {name}")
    return {
        "elf_sha256": elf_hash,
        "image_sha256": result.get("image_sha256_before"),
        "ready_elapsed_seconds": ready_elapsed,
        "ready_after_45_seconds": ready_elapsed > 45,
        "checkpoint_last_line": checkpoint.get("last_complete_line"),
        "qmp_messages": len(qmp_entries),
    }


def import_panic_qemu():
    path = Path(__file__).resolve().parent / "panic-qemu.py"
    spec = importlib.util.spec_from_file_location("panic_qemu", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class FakeClock:
    def __init__(self, path, schedule):
        self.value = 0.0
        self.path = path
        self.schedule = sorted(schedule)
        self._publish()

    def _publish(self):
        payload = b""
        for observed, value in self.schedule:
            if self.value >= observed:
                payload = value
        self.path.write_bytes(payload)

    def __call__(self):
        return self.value

    def sleep(self, duration):
        self.value += max(duration, 1.0)
        self._publish()


class FakeQmp:
    def __init__(self):
        self.polls = 0

    def poll(self, timeout):
        del timeout
        self.polls += 1


class FakeProcess:
    def poll(self):
        return None


def observer_case(module, root, name, schedule, expected,
                  ready_elapsed=None):
    directory = root / name
    directory.mkdir()
    serial = directory / "serial.log"
    clock = FakeClock(serial, schedule)
    qmp = FakeQmp()
    result = module.observe_boot_readiness(
        serial, READY_MARKER, 0.0, 120.0, 45.0, qmp, FakeProcess(), True,
        clock, clock.sleep)
    (directory / "result.json").write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n")
    if result.get("status") != expected:
        raise ValueError(f"{name}: expected {expected}, got {result.get('status')}")
    if ready_elapsed is not None:
        actual = result.get("ready", {}).get("elapsed_seconds")
        if actual != ready_elapsed:
            raise ValueError(f"{name}: expected ready at {ready_elapsed}, got {actual}")
    return result


def write_irq_fixture(directory):
    directory.mkdir()
    lines = ["[HARNESS][FRAME] ACCEPT seq=4 crc=b7672543 len=8", "",
             "[HARNESS][BEGIN] seq=4",
             "[IRQ][BOOT] state=SERVICES_ACTIVE cpus_prepared=24 cpus_verified=24 runtime_ready=24/24 handoffs=24 preemption=24 failed=0",
             "[IRQ][BOOT_PROBES] bsp_probe_vector=34 bsp_probe_entered=1 bsp_probe_returned=1 hpet_clocksource_verified=1 hpet_samples=1 hpet_counter_before=10 hpet_counter_after=20 hpet_counter_delta=10 keyboard_gsi=1",
             "[IRQ][BOOT_CLOCK] clocksource=HPET hpet_clocksource_verified=1 hpet_timer0_quiescent=1 hpet_timer0_irq_enabled=0 hpet_timer0_route_enabled=0 clockevent=BSP_LAPIC clockevent_bsp_slot=0 clockevent_period_us=1000 clockevent_ticks=100 clockevent_non_bsp=0 clockevent_early=0 clockevent_regressions=0 hpet_stray_irqs=0"]
    for slot in range(24):
        lines.append(
            f"[IRQ][BOOT_CPU] slot={slot} apic={slot} first_vector=34 "
            f"entered={100 + slot} returned={100 + slot} depth=0 "
            "unexpected=0 handoff=1 preempt=1 ready=1")
    lines.extend([
        "[IRQ][BOOT_RESULT] PASS snapshots=24 polls=25 elapsed_ms=8101 acquisition_ms=1 publication_ms=8100 budget_ms=5000",
        "", "[HARNESS][END] seq=4 status=0"])
    (directory / "serial.log").write_text("\n".join(lines) + "\n")
    command = {
        "schema": 2, "payload": "irq boot", "sequence": 4,
        "crc32": "b7672543", "classification": "PASS",
        "handler_status": 0, "begin_line": 3, "end_line": len(lines),
        "candidate_sha256": "a" * 64,
    }
    (directory / "command.json").write_text(
        json.dumps(command, indent=2, sort_keys=True) + "\n")


def run_fixtures(output_dir):
    if output_dir.exists():
        raise ValueError(f"fixture output already exists: {output_dir}")
    output_dir.mkdir(parents=True)
    module = import_panic_qemu()
    ready_line = (READY_MARKER +
                  " cpus=24/24 scheduler=1 dpc=1 shell=1 input=1 modal=1 pci=1\n").encode()
    early = observer_case(module, output_dir, "ready-before-45",
                          [(10, ready_line)], "ready", 10.0)
    if early["checkpoint"]["ready_count"] != 1:
        raise ValueError("early READY was not retained through checkpoint")
    late = observer_case(module, output_dir, "ready-between-45-and-120",
                         [(20, b"[BOOT][PROGRESS] stage=PCI_SCAN_PROGRESS\n"),
                          (60, ready_line)], "ready", 60.0)
    if late["checkpoint"]["ready_count"] != 0:
        raise ValueError("late READY appeared in the 45-second checkpoint")
    deadline = observer_case(
        module, output_dir, "no-ready-by-120", [], "deadline")
    if deadline["final"]["elapsed_seconds"] != 120.0:
        raise ValueError("absolute deadline was not enforced")
    observer_case(
        module, output_dir, "progress-without-ready",
        [(30, b"[BOOT][PROGRESS] stage=PCI_SCAN_PROGRESS\n")], "deadline")
    observer_case(
        module, output_dir, "truncated-ready",
        [(10, READY_MARKER.encode())], "deadline")
    observer_case(
        module, output_dir, "other-marker",
        [(10, b"[BOOT][RUNTIME_READY] FAIL cpus=24/24\n")], "deadline")
    observer_case(
        module, output_dir, "duplicate-ready",
        [(10, ready_line + ready_line)], "duplicate-ready-marker")
    observer_case(
        module, output_dir, "host-qmp-quit-without-ready",
        [(20, b"[BOOT][PROGRESS] stage=PCI_SCAN_PROGRESS\n")], "deadline")

    valid = output_dir / "irq-valid"
    write_irq_fixture(valid)
    validate_irq_run(valid)
    mutations = {
        "irq-handler-nonzero": lambda lines, command: command.update(
            {"handler_status": 1, "classification": "HANDLER_STATUS_REJECTED"}),
        "irq-slot-missing": lambda lines, command: lines.pop(7),
        "irq-slot-duplicated": lambda lines, command: lines.insert(7, lines[6]),
        "irq-slot-mixed": lambda lines, command: lines.__setitem__(6,
            lines[6].replace("slot=0 apic=0", "slot=1 apic=0")),
        "irq-budget-missing": lambda lines, command: lines.__setitem__(
            -3, lines[-3].replace(" budget_ms=5000", "")),
        "irq-status-fail": lambda lines, command: lines.__setitem__(
            -3, lines[-3].replace("BOOT_RESULT] PASS", "BOOT_RESULT] FAIL")),
        "irq-partial-serial": lambda lines, command: None,
    }
    rejected = []
    valid_lines = (valid / "serial.log").read_text().splitlines()
    valid_command = load_json(valid / "command.json")
    for name, mutate in mutations.items():
        target = output_dir / name
        target.mkdir()
        lines = list(valid_lines)
        command = copy.deepcopy(valid_command)
        mutate(lines, command)
        command["end_line"] = len(lines)
        raw = "\n".join(lines) + "\n"
        if name == "irq-partial-serial":
            raw = raw.rstrip("\n")
        (target / "serial.log").write_text(raw)
        (target / "command.json").write_text(
            json.dumps(command, indent=2, sort_keys=True) + "\n")
        try:
            validate_irq_run(target)
        except ValueError as error:
            rejected.append({"name": name, "reason": str(error)})
        else:
            raise ValueError(f"fixture was accepted unexpectedly: {name}")
    summary = {
        "schema": 1,
        "status": "PASS",
        "boot_controls": 8,
        "irq_valid_controls": 1,
        "irq_negatives_rejected": len(rejected),
        "irq_negatives": rejected,
    }
    (output_dir / "summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n")
    return summary


def verify_focal(root):
    summaries = []
    for profile in ("normal", "ubsan", "asan"):
        run = root / "host" / profile / "run.log"
        code = root / "host" / profile / "run-exit-code.txt"
        if not run.is_file() or not code.is_file() or code.read_text().strip() != "0":
            raise ValueError(f"host profile is incomplete: {profile}")
        text = run.read_text()
        expected = ("[IRQ_SNAPSHOT_HOST][SUMMARY] status=PASS cases=13 "
                    "assertions=90 failures=0")
        if text.splitlines().count(expected) != 1:
            raise ValueError(f"host profile summary differs: {profile}")
        summaries.append({"profile": profile, "sha256": sha256(run)})
    fixture = load_json(root / "fixtures" / "summary.json")
    if fixture.get("status") != "PASS" or \
            fixture.get("irq_negatives_rejected") != 7:
        raise ValueError("focused verifier fixtures are incomplete")
    return {"host_profiles": summaries, "fixtures": fixture}


def verify_series(root, kind):
    parent = root / kind
    runs = sorted(path for path in parent.iterdir() if path.is_dir())
    if len(runs) != 2:
        raise ValueError(f"{kind} must contain exactly two planned runs")
    if kind == "boot-observation":
        first_result = load_json(runs[0] / "result.json")
        expected = first_result.get("elf_sha256")
        results = [validate_boot_run(run, expected) for run in runs]
        if len({item["image_sha256"] for item in results}) != 1:
            raise ValueError("boot observations used different images")
    else:
        results = [validate_irq_run(run) for run in runs]
        if len({item["candidate_sha256"] for item in results}) != 1:
            raise ValueError("IRQ observations used different candidates")
    return results


def main():
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    fixture_parser = subparsers.add_parser("fixtures")
    fixture_parser.add_argument("--output-dir", type=Path, required=True)
    for name in ("focal", "boot-series", "irq-series", "all"):
        child = subparsers.add_parser(name)
        child.add_argument("root", type=Path)
    args = parser.parse_args()
    try:
        if args.command == "fixtures":
            result = run_fixtures(args.output_dir)
        elif args.command == "focal":
            result = verify_focal(args.root)
        elif args.command == "boot-series":
            result = {"boot_observation": verify_series(
                args.root, "boot-observation")}
        elif args.command == "irq-series":
            result = {"irq_runtime": verify_series(args.root, "irq-runtime")}
        else:
            result = verify_focal(args.root)
            result["boot_observation"] = verify_series(
                args.root, "boot-observation")
            result["irq_runtime"] = verify_series(args.root, "irq-runtime")
    except (OSError, TypeError, ValueError) as error:
        print(f"SMP_OBSERVATION_VERIFY: FAIL {error}", file=sys.stderr)
        return 1
    print("SMP_OBSERVATION_VERIFY: PASS " +
          json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
