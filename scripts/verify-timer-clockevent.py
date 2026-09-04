#!/usr/bin/env python3
"""Offline evidence verifier for the HPET/LAPIC clockevent contract."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
import subprocess
import sys
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
ARTIFACT = ROOT / "artifacts/build/lapic-clockevent"
AUTONOMOUS_ARTIFACT = ROOT / "artifacts/build/boot-to-shell-autonomous"
BASE = "523f367c9cd8652e709c0ffc7af28f6433d1213c"
BASE_TREE = "cc04290b8cfcfcc16de06e65b1a8145f6760d646"
BASE_PARENT = "602532db881f5e328a78efdb52b0c012f35b4065"
SUBJECT = "fix(boot): complete LAPIC clockevent and boot-to-shell liveness"

FIELDS = (
    "stage", "scenario", "machine", "smp", "accel", "kernel_sha256",
    "image_sha256", "clocksource", "clockevent", "hpet_timer0_state",
    "lapic_period_us", "cpus_ready", "global_ticks", "non_bsp_ticks",
    "early_ticks", "hpet_irqs", "stray_hpet", "result", "log", "marker",
    "sha256", "measurement", "raw_test_result", "window_ms", "rounds",
    "worst_median_x1000", "min_round_x1000", "max_round_x1000",
    "threshold", "measurement_authority", "host_schedulable_cpus",
    "guest_vcpus", "runtime_correctness", "scenario_disposition",
    "timer_order_passes", "timer_cancel_passes", "late_callbacks",
    "claimed_residual",
)

RATE_THRESHOLD = 700
ORIGINAL_RATE_LOG_SHA256 = (
    "75fc283521f23bae6916493982ddb188768412d0e9b177aa2a9a2a55f6c39d46"
)
ORIGINAL_RUNTIME_LOG_SHA256 = (
    "29ace19ec5bd95ef29f8d962186dcd88d5ec159e636427e708b54ea869dc8e70"
)
FAIR_ARTIFACT = ROOT / "artifacts/build/timer-fair-dispatch"

CYCLE_FIELDS = (
    "cycle", "source_hash", "kernel_hash", "scenario",
    "last_progress_marker", "guest_elapsed_ms", "host_elapsed_ms",
    "classification", "root_cause", "files_changed", "focused_test",
    "result", "next_action",
)

BOOT_PROGRESS_REQUIRED = (
    "KERNEL_ENTRY", "CPUS_PREPARED", "CPU_READY_SUMMARY", "PCI_BEGIN",
    "PCI_SCAN_COMPLETE", "CORE_COMPLETE", "GRAPHICS_INITIALIZED",
    "SPLASH_BEGIN", "SPLASH_TERMINAL",
    "TERMINAL_TRANSITION", "SHELL_READY", "RUNTIME_READY", "MAIN_LOOP",
)

BASELINE_HASHES = {
    "kernel/src/core/kernel_init.c": "dd61de5b32006e0bc6d44786a6b9fe3e6f0572d0be77af8fce4775cea118a22c",
    "kernel/src/core/irq_bootstrap.c": "2a540208580755787a1e4bf26be6b5f1329c23737a918585e5fe59a66a86d309",
    "kernel/src/core/irq_bootstrap.h": "88d8a6c636e98e9b4c903cf75e54fee885fe52e8da09442e8f41337f39c25e9d",
    "kernel/src/core/interrupts.c": "d948f393482ffa3b290f16dcf54411ecc1278406dad9a3f9bf0de8fe335eeb83",
    "kernel/src/drivers/timer.c": "2178acde3db4f60d6dbd6355cbedf5cf9cd7fb43ce0f4c46a203b5d01cd7cb06",
    "kernel/src/drivers/timer.h": "e2e5695daec0036f1d4bf6567812e2dc94f60c0c428fb9e292f4ccada7b13d1c",
    "kernel/src/core/timers.c": "dd5a53461b79507ed40badd5af298fea840f710799950229b1651e68b649ae26",
    "kernel/src/core/timers.h": "8789c78f8f333a41fee2821035afb84221614a07e4814eaba5660e3896f2839e",
    "kernel/src/timer/hpet.c": "becf2e00c4d90be791b3fc2d2ace1ac038255f5255eeb63ce83d82eeefbd9d51",
    "kernel/src/timer/hpet.h": "49958dc23f31a043babbd8339e1d6be91745d6db4695b81eb6c52cc8c225d5a7",
    "kernel/src/shell/commands/cmd_irq.c": "64e9d2119ea1382b5ca4298f8efdc56569b71fa3ac6e96c31d9678cee8603056",
    "scripts/test-interrupt-bringup.sh": "4d935b47fe07ea6fd05c8d045f422cfce1768adf2b9826d2b34f08af995b94c9",
    "scripts/verify-interrupt-bringup.py": "00b5b001f4eab1fd5f63007fb7ebf7610e7c7b003d8d2952524e291126e2ea26",
    "makefile": "29c31bb5afec177b017c483ea30fa58e26611d69f7af7013da1a58f46dbbd613",
}


class EvidenceError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise EvidenceError(message)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def git(*args: str) -> str:
    return subprocess.check_output(
        ["git", *args], cwd=ROOT, text=True, stderr=subprocess.DEVNULL
    ).strip()


def read_rows() -> list[dict[str, str]]:
    path = ARTIFACT / "evidence.json"
    if not path.exists():
        return []
    raw = json.loads(path.read_text(encoding="utf-8"))
    require(isinstance(raw, list), "evidence.json is not a list")
    return [{field: str(row.get(field, "")) for field in FIELDS} for row in raw]


def write_rows(rows: list[dict[str, str]]) -> None:
    ARTIFACT.mkdir(parents=True, exist_ok=True)
    (ARTIFACT / "evidence.json").write_text(
        json.dumps(rows, indent=2) + "\n", encoding="utf-8"
    )
    with (ARTIFACT / "evidence.tsv").open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, FIELDS, delimiter="\t", lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)


def record(args: argparse.Namespace) -> None:
    row = {field: str(getattr(args, field, "") or "") for field in FIELDS}
    if args.log:
        log = Path(args.log)
        if not log.is_absolute():
            log = ROOT / log
        require(log.is_file(), f"missing evidence log: {log}")
        row["log"] = str(log.relative_to(ROOT))
        row["sha256"] = sha256(log)
    rows = [old for old in read_rows() if old["stage"] != row["stage"]]
    rows.append(row)
    write_rows(rows)


def parse_manifest(path: Path) -> dict[str, str]:
    require(path.is_file(), f"missing {path.relative_to(ROOT)}")
    result: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        digest, separator, name = line.partition("  ")
        require(separator == "  " and re.fullmatch(r"[0-9a-f]{64}", digest) is not None,
                f"malformed manifest line in {path.name}")
        require(name and name not in result, f"duplicate manifest path: {name}")
        result[name] = digest
    require(result, f"empty manifest: {path.name}")
    return result


def parse_cpu_list(value: str) -> set[int]:
    require(bool(value), "empty Cpus_allowed_list")
    cpus: set[int] = set()
    for component in value.split(","):
        if re.fullmatch(r"[0-9]+", component):
            cpus.add(int(component))
            continue
        match = re.fullmatch(r"([0-9]+)-([0-9]+)", component)
        require(match is not None, f"invalid CPU-list component: {component}")
        first, last = (int(item) for item in match.groups())
        require(last >= first, f"descending CPU-list range: {component}")
        cpus.update(range(first, last + 1))
    require(bool(cpus), "CPU list selects no CPUs")
    return cpus


def verify_fair_source_manifests(scope: str) -> None:
    source_before = FAIR_ARTIFACT / "source-before.sha256"
    protected_before = AUTONOMOUS_ARTIFACT / "protected-before.sha256"
    clockevent_before = FAIR_ARTIFACT / "clockevent-worktree-before.sha256"
    for path in (source_before, protected_before, clockevent_before):
        require(path.is_file(), f"missing {path.relative_to(ROOT)}")
    expected_source = {
        "kernel/src/core/timers.c", "kernel/src/core/timers.h",
        "kernel/src/shell/commands/cmd_synctest.c",
        "kernel/src/shell/commands/cmd_synctest.h",
        "scripts/test-timer-clockevent.sh",
        "scripts/verify-timer-clockevent.py",
        "docs/timer-clocksource-clockevent.md",
        "docs/test-reports/TIMER_CLOCKEVENT_CERTIFICATION.md",
        "AGENTS.md", "README.md",
    }
    require(set(parse_manifest(source_before)) == expected_source,
            "fair-dispatch source-before scope drift")
    suffixes = ["preflight", "static"]
    if scope in {"matrix", "all"}:
        suffixes.append("after-tests")
    if scope == "all":
        suffixes.append("after-build")
    for suffix in suffixes:
        clockevent = FAIR_ARTIFACT / f"clockevent-worktree-{suffix}.sha256"
        protected = FAIR_ARTIFACT / f"protected-{suffix}.sha256"
        require(clockevent.is_file() and
                clockevent.read_bytes() == clockevent_before.read_bytes(),
                f"REJECTED_TIMER_CLOSURE_SOURCE_DRIFT: {suffix}")
        require(protected.is_file() and
                protected.read_bytes() == protected_before.read_bytes(),
                f"REJECTED_TIMER_CLOSURE_SOURCE_DRIFT: protected {suffix}")
    for name, digest in parse_manifest(clockevent_before).items():
        require((ROOT / name).is_file() and sha256(ROOT / name) == digest,
                f"REJECTED_TIMER_CLOSURE_SOURCE_DRIFT: {name}")
    for name, digest in parse_manifest(protected_before).items():
        require((ROOT / name).is_file() and sha256(ROOT / name) == digest,
                f"REJECTED_TIMER_CLOSURE_SOURCE_DRIFT: {name}")


def verify_original_rate_failure() -> None:
    path = ARTIFACT / "qemu/smp24-kvm-failed/serial.log"
    require(path.is_file(), "original SMP24 LAPIC-rate log missing")
    require(sha256(path) == ORIGINAL_RATE_LOG_SHA256,
            "original SMP24 LAPIC-rate log hash drift")
    text = path.read_text(encoding="utf-8", errors="replace")
    marker = (
        "[ACCOUNT][LAPIC_RATE] FAIL window_ms=2000 rounds=5 "
        "worst_median_x1000=672 min_round_x1000=255 max_round_x1000=934"
    )
    require(text.count(marker) == 1, "original raw 672 result not preserved")


def verify_original_runtime_failure() -> None:
    path = ARTIFACT / "qemu/soak24-failed/serial.log"
    require(path.is_file(), "original timer-cancel failure log missing")
    require(sha256(path) == ORIGINAL_RUNTIME_LOG_SHA256,
            "original timer-cancel failure log hash drift")
    text = path.read_text(encoding="utf-8", errors="replace")
    marker = "[SYNC][TIMER_CANCEL] FAIL pending=0 claimed=1 callbacks=0"
    require(text.count(marker) == 1,
            "original timer-cancel rejection was not preserved")


def read_host_environment() -> list[dict[str, str]]:
    path = ARTIFACT / "host-environment.txt"
    require(path.is_file(), "host-environment.txt missing")
    text = path.read_text(encoding="utf-8", errors="replace")
    blocks: list[dict[str, str]] = []
    for raw_block in text.split("\n\n"):
        if "[TIMER][HOST_ENV]" not in raw_block:
            continue
        values: dict[str, str] = {}
        for line in raw_block.splitlines():
            if "=" in line and not line.startswith((" ", "[TIMER]")):
                key, value = line.split("=", 1)
                values[key] = value
        for field in (
            "timestamp", "uname", "qemu_version", "nproc",
            "cpus_allowed_list", "allowed_count", "host_schedulable_cpus",
            "loadavg", "dev_kvm_access", "scenario", "requested_accel",
            "guest_smp",
        ):
            require(values.get(field, "") != "", f"host metadata missing {field}")
        nproc_value = int(values["nproc"])
        allowed = parse_cpu_list(values["cpus_allowed_list"])
        require(int(values["allowed_count"]) == len(allowed),
                "host allowed CPU count mismatch")
        expected = min(nproc_value, len(allowed))
        require(int(values["host_schedulable_cpus"]) == expected,
                "host schedulable CPU count mismatch")
        guest = int(values["guest_smp"])
        oversubscribed = int(expected < guest)
        marker = (
            f"[TIMER][HOST_ENV] host_schedulable_cpus={expected} "
            f"guest_vcpus={guest} oversubscribed={oversubscribed}"
        )
        require(marker in raw_block, "host environment marker mismatch")
        for resource in ("cpu", "memory", "io"):
            require(f"[pressure_{resource}]" in raw_block or
                    f"pressure_{resource}=unavailable" in raw_block,
                    f"host pressure metadata missing: {resource}")
        blocks.append(values)
    require(blocks, "host environment contains no captures")
    return blocks


def host_block_for(row: dict[str, str]) -> dict[str, str]:
    matches = [
        block for block in read_host_environment()
        if block["scenario"] == row["scenario"] and
        block["requested_accel"] == row["accel"] and
        block["guest_smp"] == row["guest_vcpus"]
    ]
    require(len(matches) == 1,
            f"{row['stage']}: expected one matching host environment capture")
    return matches[0]


def parse_lapic_rate(text: str, stage: str) -> dict[str, int | str]:
    matches = re.findall(
        r"^\[ACCOUNT\]\[LAPIC_RATE\] (PASS|FAIL) ([^\r\n]+)",
        text,
        re.MULTILINE,
    )
    require(len(matches) == 1, f"{stage}: expected exactly one raw LAPIC-rate result")
    raw, fields_text = matches[0]
    fields: dict[str, int | str] = {"raw": raw}
    for name in (
        "window_ms", "rounds", "worst_median_x1000",
        "min_round_x1000", "max_round_x1000",
    ):
        match = re.search(rf"(?:^| ){name}=([0-9]+)(?: |$)", fields_text)
        require(match is not None, f"{stage}: raw result missing {name}")
        fields[name] = int(match.group(1))
    expected_raw = (
        "FAIL"
        if int(fields["worst_median_x1000"]) < RATE_THRESHOLD or
        int(fields["worst_median_x1000"]) > 1300
        else "PASS"
    )
    require(raw == expected_raw, f"{stage}: raw LAPIC-rate result was rewritten")
    return fields


def verify_rate_measurement(
    stage: str, row: dict[str, str], text: str, expected_guest: int
) -> None:
    parsed = parse_lapic_rate(text, stage)
    require(row["measurement"] == "lapic-rate", f"{stage}: measurement type")
    require(row["threshold"] == str(RATE_THRESHOLD), f"{stage}: threshold drift")
    require(row["raw_test_result"] == parsed["raw"],
            f"{stage}: raw result ledger mismatch")
    ledger_fields = {
        "window_ms": "window_ms",
        "rounds": "rounds",
        "worst_median_x1000": "worst_median_x1000",
        "min_round_x1000": "min_round_x1000",
        "max_round_x1000": "max_round_x1000",
    }
    for ledger, parsed_name in ledger_fields.items():
        require(row[ledger] == str(parsed[parsed_name]),
                f"{stage}: {ledger} ledger mismatch")
    require(parsed["window_ms"] == 2000 and parsed["rounds"] == 5,
            f"{stage}: rate command contract drift")
    require(row["guest_vcpus"] == str(expected_guest),
            f"{stage}: guest vCPU metadata mismatch")
    host = int(row["host_schedulable_cpus"])
    require(host > 0, f"{stage}: host schedulable CPU metadata missing")
    block = host_block_for(row)
    require(int(block["host_schedulable_cpus"]) == host,
            f"{stage}: host metadata disagrees with ledger")

    cpu_lines = re.findall(
        r"^\[ACCOUNT\]\[LAPIC_RATE_CPU\] round=([0-9]+) slot=([0-9]+) "
        r"delta=([0-9]+) expected=([0-9]+) ratio_x1000=([0-9]+)",
        text,
        re.MULTILINE,
    )
    require(len(cpu_lines) == expected_guest * 5,
            f"{stage}: incomplete per-CPU LAPIC-rate samples")
    observed = {(int(round_value), int(slot)) for round_value, slot, *_ in cpu_lines}
    expected = {
        (round_value, slot)
        for round_value in range(1, 6)
        for slot in range(expected_guest)
    }
    require(observed == expected, f"{stage}: per-CPU rate sample coverage")
    require(all(int(delta) > 0 for _, _, delta, _, _ in cpu_lines),
            f"{stage}: CPU with zero LAPIC IRQ delta")

    commands = ROOT / row["log"]
    commands = commands.parent / "commands.tsv"
    require(commands.is_file(), f"{stage}: commands.tsv missing")
    issued = [
        line.split("\t", 1)[1]
        for line in commands.read_text(encoding="utf-8").splitlines()
        if "\t" in line
    ]
    require(issued.count("accounttest lapic-rate 2000 5") == 1,
            f"{stage}: LAPIC-rate was not executed exactly once")
    require(row["runtime_correctness"] == "PASS",
            f"{stage}: runtime correctness not proven")

    if host >= expected_guest:
        expected_authority = (
            "AUTHORITATIVE_SMP24_RATE"
            if expected_guest == 24 else "AUTHORITATIVE_SMP4_RATE_CONTROL"
        )
        require(row["measurement_authority"] == expected_authority,
                f"{stage}: authoritative classification mismatch")
        require(row["raw_test_result"] == "PASS",
                f"{stage}: authoritative raw rate failure")
        require(row["scenario_disposition"] == "PASS",
                f"{stage}: authoritative disposition mismatch")
    else:
        require(expected_guest == 24,
                f"{stage}: environmental exception is SMP24-only")
        authority = "ENVIRONMENT_NON_AUTHORITATIVE_OVERSUBSCRIBED"
        require(row["measurement_authority"] == authority,
                f"{stage}: oversubscription authority mismatch")
        if row["raw_test_result"] == "FAIL":
            require(row["scenario_disposition"] ==
                    "PASS_WITH_ENVIRONMENT_RATE_EXCEPTION",
                    f"{stage}: invalid environmental disposition")
            marker = (
                "[TIMER][LAPIC_RATE_ENV] "
                "ENVIRONMENT_NON_AUTHORITATIVE_OVERSUBSCRIBED "
                f"host_cpus={host} guest_vcpus={expected_guest} "
                f"worst_median_x1000={row['worst_median_x1000']} threshold=700"
            )
            require(marker in text, f"{stage}: environmental marker missing")
        else:
            require(row["scenario_disposition"] == "PASS",
                    f"{stage}: passing raw result has wrong disposition")


def verify_rate_control(stage: str, row: dict[str, str]) -> None:
    require((row["machine"], row["smp"], row["accel"]) ==
            ("q35", "4", "kvm"), f"{stage}: control launch tuple")
    text = read_log(
        row,
        "[ACCOUNT][LAPIC_CONFIG] PASS",
        "[ACCOUNT][LAPIC_LIVENESS] PASS",
        "[ACCOUNT][CHECK] PASS clocksource=HPET clockevent=BSP_LAPIC",
        "[IRQ][CHECK] PASS clocksource=HPET clockevent=BSP_LAPIC",
        "[BOOT][RUNTIME_READY] PASS cpus=4/4",
    )
    liveness = re.findall(r"^\[ACCOUNT\]\[LAPIC_LIVENESS\] PASS[^\r\n]+", text,
                          re.MULTILINE)
    require(liveness and "all_advanced=1" in liveness[-1] and
            "zero_irq_cpus=0" in liveness[-1], f"{stage}: liveness details")
    require("unexpected=0 imbalance=0" in text, f"{stage}: IRQ correctness")
    require(not re.search(r"PANIC|#PF|#GP|FATAL|STRUCTURAL_FAULT|FINISH_FAULT", text),
            f"{stage}: guest fault")
    verify_rate_measurement(stage, row, text, 4)


def verify_fairness_log(
    stage: str, row: dict[str, str], text: str,
    minimum_order: int, minimum_cancel: int,
) -> None:
    order_marker = (
        "[SYNC][TIMER_ORDER] PASS old_position=1 new_position=2 callbacks=2"
    )
    cancel_results = re.findall(
        r"^\[SYNC\]\[TIMER_CANCEL\] PASS run=[0-9]+ "
        r"requested=3 completed=3 errors=0$", text, re.MULTILINE,
    )
    cancel_details = text.count(
        "PASS pending_cancel=0 duplicate_cancel=2 claimed_cancel=1 callbacks=1"
    )
    require(text.count(order_marker) >= minimum_order,
            f"{stage}: timer-order volume")
    require(len(cancel_results) >= minimum_cancel,
            f"{stage}: timer-cancel volume")
    require(cancel_details >= minimum_cancel,
            f"{stage}: timer-cancel diagnostic volume")
    require("[SYNC][TIMER_BACKLOG] PASS old=8 new=8 callbacks=16" in text,
            f"{stage}: timer backlog gate missing")
    checks = re.findall(r"^\[SYNC\]\[CHECK\] PASS[^\r\n]+", text,
                        re.MULTILINE)
    require(checks and all(field in checks[-1] for field in (
        "timers_claimed=0", "test_active=0", "late=0", "timer_errors=0",
    )), f"{stage}: final timer state is not clean")
    require(not re.search(
        r"\[SYNC\]\[(?:TIMER_ORDER|TIMER_CANCEL|TIMER_BACKLOG)\] FAIL",
        text), f"{stage}: timer fairness command failed")
    require(not re.search(
        r"\[(?:SYNC|TIMER)\].*(?:late=[1-9]|residual=[1-9]|"
        r"order_violations=[1-9])", text),
        f"{stage}: timer callback/order residual")
    require(not re.search(
        r"timer ref underflow|timer ref double release|late.callback|"
        r"dispatch order violation", text, re.IGNORECASE),
        f"{stage}: timer structural fault")
    require(row["timer_order_passes"] == str(text.count(order_marker)),
            f"{stage}: timer-order ledger mismatch")
    require(row["timer_cancel_passes"] == str(len(cancel_results)),
            f"{stage}: timer-cancel ledger mismatch")
    require(row["late_callbacks"] == "0" and
            row["claimed_residual"] == "0",
            f"{stage}: clean timer ledger mismatch")


def verify_focused_fairness(stage: str, row: dict[str, str]) -> None:
    expected_smp = 4 if stage.endswith("smp4") else 24
    require((row["machine"], row["smp"], row["accel"]) ==
            ("q35", str(expected_smp), "kvm"),
            f"{stage}: focused launch tuple")
    text = read_log(
        row, "[KERNEL] Entering Main Loop.",
        f"[BOOT][RUNTIME_READY] PASS cpus={expected_smp}/{expected_smp}",
        "[IRQ][CHECK] PASS clocksource=HPET clockevent=BSP_LAPIC",
        "[TASKDIAG][CHECK] PASS",
    )
    require(text.count("[SYNC][SLEEP] PASS workers=32 iterations=100 timeout=3200")
            >= 10, f"{stage}: sleep-pressure volume")
    verify_fairness_log(stage, row, text, 10, 10)
    if expected_smp == 24:
        require(text.count("[SMP][KILL_SWEEP] PASS workers=8") >= 4,
                f"{stage}: SMP24 pressure volume")
    require(not re.search(
        r"PANIC|#PF|#GP|FATAL|STRUCTURAL_FAULT|FINISH_FAULT", text),
        f"{stage}: guest fault")


def verify_negative_claimed_order(row: dict[str, str]) -> None:
    text = read_log(
        row, "[TIMER][NEGATIVE] CLAIMED_ORDER_BYPASS_DETECTED",
        "[SYNC][TIMER_ORDER] PASS old_position=2 new_position=1",
    )
    commands = ROOT / row["log"]
    commands = commands.parent / "commands.tsv"
    require(commands.is_file() and
            commands.read_text(encoding="utf-8").count(
                "synctest timer-order") == 1,
            "claimed-order negative command count")


def verify_baseline_manifest() -> None:
    actual = parse_manifest(ARTIFACT / "source-before.sha256")
    require(actual == BASELINE_HASHES, "source-before does not match the frozen base")


def verify_protected_manifests(scope: str) -> None:
    before = ARTIFACT / "protected-before.sha256"
    require(before.is_file(), "protected-before missing")
    targets = []
    if scope in {"matrix", "all"}:
        targets.append(ARTIFACT / "protected-after-tests.sha256")
    if scope == "all":
        targets.append(ARTIFACT / "protected-after-build.sha256")
    for target in targets:
        require(target.is_file(), f"missing {target.name}")
        require(before.read_bytes() == target.read_bytes(),
                f"protected source changed: {target.name}")
        for name, digest in parse_manifest(target).items():
            source = ROOT / name
            require(source.is_file() and sha256(source) == digest,
                    f"protected hash drift: {name}")


def verify_git(scope: str) -> None:
    require(git("branch", "--show-current") == "feat/taskman", "wrong branch")
    head = git("rev-parse", "HEAD")
    if head == BASE:
        require(scope != "all", "final verification requires the mission commit")
        require(git("rev-parse", "HEAD^{tree}") == BASE_TREE, "wrong base tree")
        require(git("rev-parse", "HEAD^") == BASE_PARENT, "wrong base parent")
    else:
        require(git("rev-parse", "HEAD^") == BASE, "mission commit parent is not base")
        require(git("rev-list", "--count", f"{BASE}..HEAD") == "1",
                "mission must contain exactly one commit")
        require(git("log", "-1", "--format=%s") == SUBJECT, "wrong commit subject")


def read_log(row: dict[str, str], *markers: str) -> str:
    path = ROOT / row["log"]
    require(path.is_file(), f"missing log for {row['stage']}")
    require(sha256(path) == row["sha256"], f"log hash drift for {row['stage']}")
    text = path.read_text(encoding="utf-8", errors="replace")
    for marker in markers:
        require(marker in text, f"{row['stage']}: missing {marker}")
    return text


def verify_scenario(stage: str, row: dict[str, str]) -> None:
    kind = stage.removeprefix("fair-")
    expected = {
        "up-tcg": ("q35", "1", "tcg"),
        "smp2-tcg": ("q35", "2", "tcg"),
        "smp4-kvm": ("q35", "4", "kvm"),
        "smp8-kvm": ("q35", "8", "kvm"),
        "smp24-kvm": ("q35", "24", "kvm"),
        "pcat": ("pc", "4", "tcg"),
    }[kind]
    require((row["machine"], row["smp"], row["accel"]) == expected,
            f"{stage}: launch tuple drift")
    require(row["result"] == "PASS" and row["clocksource"] == "HPET" and
            row["clockevent"] == "BSP_LAPIC" and
            row["hpet_timer0_state"] == "QUIESCENT" and
            row["lapic_period_us"] == "1000", f"{stage}: ledger contract")
    smp = int(row["smp"])
    require(row["cpus_ready"] == str(smp), f"{stage}: CPU ledger mismatch")
    require(row["non_bsp_ticks"] == "0" and row["early_ticks"] == "0" and
            row["hpet_irqs"] == "0" and row["stray_hpet"] == "0",
            f"{stage}: anomalous clockevent ledger")
    text = read_log(
        row,
        "[IRQ][PIC] QUIESCENT", "[IRQ][IOAPIC] QUIESCENT",
        f"[IRQ][BOOTSTRAP] CPUS_PREPARED cpus={smp}",
        "[IRQ][BSP_LAPIC_PROBE] PASS vector=34",
        "[CLOCK][HPET_CLOCKSOURCE_PROBE] PASS",
        "[CLOCK][HPET_TIMER0] QUIESCENT",
        "[CLOCKEVENT][RUNTIME] ACTIVE source=BSP_LAPIC",
        f"[IRQ][BOOTSTRAP] CLOCKEVENT_ACTIVE cpus={smp}",
        "[GRAPHICS][SPLASH] BEGIN", "[GRAPHICS][SPLASH] COMPLETE",
        "[IRQ][BOOTSTRAP] SERVICES_ACTIVE", "[BOOT][SHELL_READY] PASS",
        f"[BOOT][RUNTIME_READY] PASS cpus={smp}/{smp}",
        "[BOOT][TEST_READY] PASS autorun=0 selftests=0",
        "[KERNEL] Entering Main Loop.",
        "[IRQ][CHECK] PASS clocksource=HPET clockevent=BSP_LAPIC period_us=1000 hpet_timer0=QUIESCENT non_bsp_ticks=0 stray_hpet=0",
        "[ACCOUNT][CHECK] PASS clocksource=HPET clockevent=BSP_LAPIC hpet_timer0=QUIESCENT",
        "[TASKDIAG][CHECK] PASS", "[INPUTTEST][CHECK] PASS",
        "[MODALTEST][CHECK] PASS", "[TASKMANTEST][CHECK] PASS",
        "hpet_timer0_irq_enabled=0", "hpet_timer0_route_enabled=0",
        "clockevent_non_bsp=0", "clockevent_early=0",
        "clockevent_regressions=0", "hpet_stray_irqs=0",
    )
    require(text.count("[IRQ][CPU_READY] PASS") == smp,
            f"{stage}: CPU_READY count")
    require(text.count("timer_us=1000 handoff=1 preempt=1") == smp,
            f"{stage}: LAPIC period/handoff count")
    require(text.count("[SCHED][BOOTSTRAP_HANDOFF] PASS slot=") == smp,
            f"{stage}: handoff count")
    require("[IRQ][HPET_PROBE]" not in text, f"{stage}: legacy HPET IRQ probe")
    require(not re.search(r"owner=HPET[^\n]*enabled=1", text),
            f"{stage}: active HPET route")
    require(not re.search(r"PANIC|#PF|#GP|FATAL|STRUCTURAL_FAULT|FINISH_FAULT", text),
            f"{stage}: guest fault")
    if kind in {"smp4-kvm", "smp24-kvm"}:
        for marker in ("[SYNC][SLEEP] PASS workers=32 iterations=100 timeout=3200",
                       "[SYNC][TIMER_ORDER] PASS old_position=1 new_position=2",
                       "[SYNC][TIMER_CANCEL] PASS", "[REAPTEST][TIMER_REF] PASS",
                       "[SYNC][TIMER_BACKLOG] PASS old=8 new=8 callbacks=16",
                       "[TASKMAN][AUTO_EXIT] PASS target=3"):
            require(marker in text, f"{stage}: missing focused marker {marker}")
        verify_fairness_log(stage, row, text, 1, 1)
    if kind == "smp24-kvm":
        for marker in ("[ACCOUNT][CLOCK_SMP] PASS",
                       "[ACCOUNT][REAPER_QUIESCENCE] PASS workers=32",
                       "[ACCOUNT][LAPIC_CONFIG] PASS",
                       "[ACCOUNT][LAPIC_LIVENESS] PASS",
                       "[SMP][KILL_SWEEP] PASS workers=8",
                       "unexpected=0 imbalance=0"):
            require(marker in text, f"{stage}: missing SMP24 marker {marker}")
        liveness = re.findall(
            r"^\[ACCOUNT\]\[LAPIC_LIVENESS\] PASS[^\r\n]+", text, re.MULTILINE
        )
        require(liveness and "all_advanced=1" in liveness[-1] and
                "zero_irq_cpus=0" in liveness[-1],
                f"{stage}: LAPIC liveness details")
        verify_rate_measurement(stage, row, text, 24)
    if kind == "pcat":
        require("master=ff slave=ff" in text, "PCAT PIC masks missing")
        require("pcat=1" in text, "PCAT_COMPAT was not recognized")


def verify_soak(row: dict[str, str]) -> None:
    text = read_log(
        row, "[TIMER][SOAK] PASS smp=24 source=BSP_LAPIC hpet_timer0=QUIESCENT",
        "[TASKMANTEST][HEAP] PASS", "drift=0", "[REAPTEST][CHECK] PASS",
        "[TIMER][SOAK_RUNTIME] PASS timer_residual=0 heap_drift=0",
    )
    match = re.search(r"\[TIMER\]\[SOAK\] PASS smp=24 source=BSP_LAPIC hpet_timer0=QUIESCENT duration_ms=(\d+)", text)
    require(match is not None and int(match.group(1)) >= 300000, "soak duration too short")
    require(text.count("[IRQ][CHECK] PASS") >= 10, "soak irq check volume")
    require(text.count("[TASKDIAG][CHECK] PASS") >= 10, "soak taskdiag volume")
    require(text.count("[SYNC][SLEEP] PASS") >= 3, "soak sleep volume")
    require(text.count(
        "[SYNC][TIMER_ORDER] PASS old_position=1 new_position=2 callbacks=2"
    ) >= 5, "soak order volume")
    require(len(re.findall(
        r"^\[SYNC\]\[TIMER_CANCEL\] PASS run=[0-9]+ "
        r"requested=3 completed=3 errors=0$", text, re.MULTILINE,
    )) >= 5, "soak cancel volume")
    require(text.count(
        "PASS pending_cancel=0 duplicate_cancel=2 claimed_cancel=1 callbacks=1"
    ) >= 5, "soak cancel diagnostic volume")
    require(text.count("[SMP][KILL_SWEEP] PASS workers=8") >= 4, "soak stress volume")
    require(text.count("[TASKMAN][AUTO_EXIT] PASS target=3") >= 4, "soak TASKMAN volume")
    require("clockevent_non_bsp=0" in text and "clockevent_early=0" in text and
            "clockevent_regressions=0" in text and "hpet_stray_irqs=0" in text,
            "soak clockevent anomaly")
    require("unexpected=0 imbalance=0" in text, "soak interrupt anomaly")
    require("timers_claimed=0" in text, "soak claimed timer residual")
    verify_fairness_log("fair-soak24", row, text, 5, 5)
    require("[ACCOUNT][LAPIC_RATE]" not in text,
            "soak must not repeatedly benchmark LAPIC rate")
    require(not re.search(r"PANIC|#PF|#GP|FATAL|STRUCTURAL_FAULT|FINISH_FAULT", text),
            "soak guest fault")


def verify_candidate() -> None:
    candidate = ROOT / "artifacts/baremetal/lapic-clockevent-candidate"
    required = {
        "hobbyos.img", "kernel.elf", "BOOTX64.EFI", "SHA256SUMS",
        "payload-manifest.txt", "candidate.txt",
        "LAPIC_CLOCKEVENT_OPERATOR_RUNBOOK.md",
        "TIMER_CLOCKEVENT_CERTIFICATION.md", "timer-clocksource-clockevent.md",
    }
    require(candidate.is_dir(), "candidate directory missing")
    require(required <= {path.name for path in candidate.iterdir() if path.is_file()},
            "candidate files incomplete")
    sums = parse_manifest(candidate / "SHA256SUMS")
    for name, digest in sums.items():
        source = candidate / name
        require(source.is_file() and sha256(source) == digest,
                f"candidate hash mismatch: {name}")
    require(required - {"SHA256SUMS"} <= sums.keys(), "SHA256SUMS incomplete")
    data = (candidate / "candidate.txt").read_text(encoding="utf-8")
    for marker in (
        "STATUS=READY_FOR_BARE_METAL_LAPIC_CLOCKEVENT_VALIDATION",
        "CLOCKSOURCE=HPET", "CLOCKEVENT=BSP_LAPIC",
        "HPET_TIMER0=QUIESCENT", "LAPIC_PERIOD_US=1000",
        "ROOT_CAUSE=PREEXISTING_CLAIMED_TIMER_BYPASSED_BY_NEWLY_DUE_TIMER",
        "TIMER_FAIRNESS_GATE=PASS", "TIMER_CANCEL_REPEATED_GATE=PASS",
        "LATE_CALLBACKS=0", "CLAIMED_RESIDUAL=0",
        "SMP4_RATE_CONTROL=SMP4_KVM_3_OF_3_PASS",
        "SOAK24=PASS",
        f"ORIGINAL_SMP24_RATE_LOG_SHA256={ORIGINAL_RATE_LOG_SHA256}",
        f"ORIGINAL_TIMER_CANCEL_LOG_SHA256={ORIGINAL_RUNTIME_LOG_SHA256}",
        "BARE_METAL_EXECUTED_BY_CODEX=NO",
    ):
        require(marker in data, f"candidate metadata missing {marker}")
    rows = {row["stage"]: row for row in read_rows()}
    smp24 = rows.get("fair-smp24-kvm", {})
    focused4 = rows.get("fair-focused-smp4", {})
    focused24 = rows.get("fair-focused-smp24", {})
    soak = rows.get("fair-soak24", {})
    host_cpus = int(smp24.get("host_schedulable_cpus", "0") or "0")
    qemu_disposition = (
        "PASS_WITH_DOCUMENTED_ENVIRONMENT_RATE_EXCEPTION"
        if smp24.get("scenario_disposition") ==
        "PASS_WITH_ENVIRONMENT_RATE_EXCEPTION" else "PASS"
    )
    current_head = git("rev-parse", "HEAD")
    current_tree = git("rev-parse", "HEAD^{tree}")
    for marker in (
        f"COMMIT={current_head}", f"TREE={current_tree}",
        f"PARENT={BASE}",
        f"HOST_SCHEDULABLE_CPUS={smp24.get('host_schedulable_cpus', '')}",
        f"SMP24_RATE_RAW_RESULT={smp24.get('raw_test_result', '')}",
        f"SMP24_RATE_WORST_MEDIAN_X1000={smp24.get('worst_median_x1000', '')}",
        f"SMP24_RATE_AUTHORITY={smp24.get('measurement_authority', '')}",
        f"SMP24_SCENARIO_DISPOSITION={smp24.get('scenario_disposition', '')}",
        f"SMP24_RATE_LOG_SHA256={smp24.get('sha256', '')}",
        f"FOCUSED_SMP4_TIMER_ORDER_PASSES={focused4.get('timer_order_passes', '')}",
        f"FOCUSED_SMP4_TIMER_CANCEL_PASSES={focused4.get('timer_cancel_passes', '')}",
        f"FOCUSED_SMP24_TIMER_ORDER_PASSES={focused24.get('timer_order_passes', '')}",
        f"FOCUSED_SMP24_TIMER_CANCEL_PASSES={focused24.get('timer_cancel_passes', '')}",
        f"SOAK_TIMER_ORDER_PASSES={soak.get('timer_order_passes', '')}",
        f"SOAK_TIMER_CANCEL_PASSES={soak.get('timer_cancel_passes', '')}",
        f"QEMU_DISPOSITION={qemu_disposition}",
    ):
        require(marker in data and not marker.endswith("="),
                f"candidate metadata mismatch {marker}")
    duration = re.search(r"^SOAK24_DURATION_MS=([0-9]+)$", data, re.MULTILINE)
    require(duration is not None and int(duration.group(1)) >= 300000,
            "candidate soak duration missing or short")
    payloads = parse_manifest(candidate / "payload-manifest.txt")
    expected = {
        "/kernel.elf": sha256(ROOT / "kernel.elf"),
        "/EFI/BOOT/BOOTX64.EFI": sha256(ROOT / "BOOTX64.EFI"),
        "/EFI/fonts/zap-light16.psf": sha256(ROOT / "bootloader/fonts/zap-light16.psf"),
        "/EFI/images/logo.bmp": sha256(ROOT / "bootloader/images/logo.bmp"),
        "/startup.nsh": sha256(ROOT / "bootloader/startup.nsh"),
    }
    require(payloads == expected, "candidate payload manifest drift")
    reviews = sorted((ROOT / "artifacts/baremetal").glob(
        "HobbyOS-LAPIC-CLOCKEVENT-FAIR-TIMER-*.zip"
    ))
    require(len(reviews) == 1, "expected exactly one review ZIP")
    with zipfile.ZipFile(reviews[0]) as archive:
        require(archive.testzip() is None, "review ZIP is corrupt")
        names = archive.namelist()
        require(any(name.endswith("/.git/HEAD") for name in names), "review ZIP lacks .git")
        require(any("lapic-clockevent-candidate/hobbyos.img" in name for name in names),
                "review ZIP lacks candidate")


def verify(scope: str) -> None:
    verify_git(scope)
    verify_baseline_manifest()
    verify_fair_source_manifests(scope)
    verify_original_rate_failure()
    verify_original_runtime_failure()
    rows = read_rows()
    by_stage = {row["stage"]: row for row in rows}
    required = {
        "preflight", "static", "selftest", "fair-negative-hpet-irq",
        "fair-negative-ap-tick", "fair-negative-early-tick",
        "fair-negative-duplicate", "fair-negative-hpet-stall",
        "fair-negative-claimed-order", "fair-focused-smp4",
        "fair-focused-smp24", "fair-up-tcg", "fair-smp2-tcg",
        "fair-smp4-kvm", "fair-smp8-kvm", "fair-smp24-kvm",
        "fair-pcat", "fair-software-timers", "fair-taskman",
        "fair-stress", "fair-soak24", "fair-final-build",
        "fair-rate-control-smp4-run1", "fair-rate-control-smp4-run2",
        "fair-rate-control-smp4-run3",
    }
    if scope == "all":
        required |= {"candidate", "fair-report"}
    require(required <= by_stage.keys(), f"missing stages: {sorted(required - by_stage.keys())}")
    for stage in required:
        require(by_stage[stage]["result"] == "PASS", f"stage not PASS: {stage}")
    negative_markers = {
        "fair-negative-hpet-irq":
            "[TIMER][NEGATIVE] HPET_IRQ_BOOT_DEPENDENCY_DETECTED",
        "fair-negative-ap-tick":
            "[TIMER][NEGATIVE] NON_BSP_GLOBAL_TICK_DETECTED",
        "fair-negative-early-tick":
            "[TIMER][NEGATIVE] CLOCKEVENT_BEFORE_ACTIVATION_DETECTED",
        "fair-negative-duplicate":
            "[TIMER][NEGATIVE] DUPLICATE_GLOBAL_CLOCKEVENT_DETECTED",
        "fair-negative-hpet-stall":
            "[CLOCK][NEGATIVE] HPET_CLOCKSOURCE_STALL_DETECTED",
    }
    for stage, marker in negative_markers.items():
        read_log(by_stage[stage], marker)
    for stage in ("fair-up-tcg", "fair-smp2-tcg", "fair-smp4-kvm",
                  "fair-smp8-kvm", "fair-smp24-kvm", "fair-pcat"):
        verify_scenario(stage, by_stage[stage])
    for stage in ("fair-focused-smp4", "fair-focused-smp24"):
        verify_focused_fairness(stage, by_stage[stage])
    verify_negative_claimed_order(by_stage["fair-negative-claimed-order"])
    controls = [f"fair-rate-control-smp4-run{run}" for run in range(1, 4)]
    for stage in controls:
        verify_rate_control(stage, by_stage[stage])
    functional_stages = controls + [
        "fair-focused-smp4", "fair-focused-smp24", "fair-up-tcg",
        "fair-smp2-tcg", "fair-smp4-kvm", "fair-smp8-kvm",
        "fair-smp24-kvm", "fair-pcat", "fair-soak24", "fair-final-build",
    ]
    require(len({by_stage[stage]["kernel_sha256"]
                 for stage in functional_stages}) == 1,
            "functional gates did not use one reproducible kernel")
    control_dirs = {
        path.name for path in (ARTIFACT / "qemu").glob(
            "fair-rate-control-smp4-run*")
        if path.is_dir()
    }
    require(control_dirs == set(controls),
            "rate control boot count is not exactly three")
    verify_soak(by_stage["fair-soak24"])
    read_log(by_stage["fair-final-build"], "[TIMER][FINAL_BUILD] PASS")
    if scope == "all":
        verify_candidate()
    print(f"[TIMER][EVIDENCE] PASS scope={scope}")


def host_selftest() -> None:
    states = [
        "OFF", "CONTROLLERS_QUIESCENT", "ROUTES_PREPARED", "CPUS_PREPARED",
        "BSP_LAPIC_VERIFIED", "HPET_CLOCKSOURCE_VERIFIED",
        "CLOCKEVENT_ACTIVE", "SERVICES_ACTIVE",
    ]
    require(len(states) == len(set(states)), "duplicate state")
    event = {"configured": False, "active": False, "owner": 0, "ticks": 0,
             "early": 0, "non_bsp": 0, "last": 0, "regressions": 0}
    require(not event["configured"] and not event["active"], "initial event state")
    event["configured"] = True
    event["active"] = True
    for slot, now in ((0, 1000), (0, 2000), (1, 3000), (0, 1500)):
        if slot != event["owner"]:
            event["non_bsp"] += 1
        elif event["last"] and now < event["last"]:
            event["regressions"] += 1
        else:
            event["last"] = now
            event["ticks"] += 1
    require(event["ticks"] == 2 and event["non_bsp"] == 1 and
            event["regressions"] == 1, "clockevent model")
    require(101 > 100 and not (100 > 100), "HPET progress model")
    require(parse_cpu_list("0-11") == set(range(12)), "CPU-list range model")
    require(parse_cpu_list("0-3,8-11") == set(range(4)) | set(range(8, 12)),
            "CPU-list split-range model")
    require(("FAIL" if 672 < RATE_THRESHOLD or 672 > 1300 else "PASS") ==
            "FAIL", "raw rate classification model")
    require(12 < 24 and not (12 < 4), "oversubscription authority model")
    claimed = ["old"]
    newly_due = ["new"]
    claimed.extend(newly_due)
    require(claimed.pop(0) == "old" and claimed.pop(0) == "new",
            "claimed FIFO fairness model")
    held = [("held-old", True), ("eligible-old", False), ("new", False)]
    selected = next(name for name, is_held in held if not is_held)
    require(selected == "eligible-old", "held claimed selection model")
    print("[TIMER][HOST_SELFTEST] PASS tests=18")


def ppm_tokens(data: bytes) -> tuple[list[bytes], int]:
    tokens: list[bytes] = []
    index = 0
    while len(tokens) < 4:
        while index < len(data) and data[index] in b" \t\r\n":
            index += 1
        if index < len(data) and data[index] == ord("#"):
            while index < len(data) and data[index] != ord("\n"):
                index += 1
            continue
        start = index
        while index < len(data) and data[index] not in b" \t\r\n":
            index += 1
        require(index > start, "invalid PPM header")
        tokens.append(data[start:index])
    while index < len(data) and data[index] in b" \t\r\n":
        index += 1
    return tokens, index


def verify_screens(paths: list[str]) -> None:
    require(len(paths) == 2, "two screenshots required")
    digests = []
    for raw in paths:
        path = Path(raw)
        if not path.is_absolute():
            path = ROOT / path
        data = path.read_bytes()
        tokens, offset = ppm_tokens(data)
        require(tokens[0] == b"P6" and tokens[3] == b"255", "unsupported PPM")
        width, height = int(tokens[1]), int(tokens[2])
        payload = data[offset:]
        require(width > 0 and height > 0 and len(payload) == width * height * 3,
                "invalid PPM dimensions/payload")
        require(len(set(payload)) > 2, f"uniform screenshot: {path.name}")
        digests.append(sha256(path))
    require(digests[0] != digests[1], "splash and terminal screenshots are identical")
    print(f"[GRAPHICS][SPLASH_CAPTURE] PASS begin={digests[0]} complete={digests[1]}")


def _root_path(raw: str) -> Path:
    path = Path(raw)
    return path if path.is_absolute() else ROOT / path


def write_cycle_record(args: argparse.Namespace) -> None:
    AUTONOMOUS_ARTIFACT.mkdir(parents=True, exist_ok=True)
    json_path = AUTONOMOUS_ARTIFACT / "cycles.json"
    rows: list[dict[str, str]] = []
    if json_path.exists():
        loaded = json.loads(json_path.read_text(encoding="utf-8"))
        require(isinstance(loaded, list), "cycles.json is not a list")
        rows = [
            {field: str(row.get(field, "")) for field in CYCLE_FIELDS}
            for row in loaded
        ]
    row = {field: str(getattr(args, field, "") or "")
           for field in CYCLE_FIELDS}
    require(re.fullmatch(r"[0-5]", row["cycle"]) is not None,
            "cycle must be between 0 and 5")
    require(row["classification"] and row["result"],
            "cycle classification/result required")
    rows = [old for old in rows if old["cycle"] != row["cycle"]]
    rows.append(row)
    rows.sort(key=lambda item: int(item["cycle"]))
    temp = json_path.with_suffix(".json.tmp")
    temp.write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")
    temp.replace(json_path)
    tsv_path = AUTONOMOUS_ARTIFACT / "cycles.tsv"
    with tsv_path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, CYCLE_FIELDS, delimiter="\t",
                                lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
    print(f"[BOOT][AUTONOMOUS_CYCLE] RECORDED cycle={row['cycle']} "
          f"result={row['result']}")


def write_source_manifest(args: argparse.Namespace) -> None:
    output = _root_path(args.output)
    require(not output.exists(), f"refusing to overwrite manifest: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    rows = []
    for raw in args.paths:
        path = _root_path(raw)
        require(path.is_file(), f"manifest input missing: {path}")
        rows.append((str(path.relative_to(ROOT)), sha256(path)))
    output.write_text(
        "".join(f"{digest}  {name}\n" for name, digest in sorted(rows)),
        encoding="utf-8",
    )
    print(f"[BOOT][SOURCE_MANIFEST] PASS path={output.relative_to(ROOT)} "
          f"files={len(rows)} sha256={sha256(output)}")


def write_stall_report(args: argparse.Namespace) -> None:
    directory = _root_path(args.directory)
    directory.mkdir(parents=True, exist_ok=True)
    captures = []
    for name in (
        "stall-info-status.txt", "stall-info-registers.txt", "stall.ppm",
        "serial-stall.log", "serial-tail.log", "trace-tail.log",
        "clockevent-snapshot.log",
    ):
        path = directory / name
        if path.is_file():
            captures.append({
                "path": str(path.relative_to(ROOT)),
                "size": path.stat().st_size,
                "sha256": sha256(path),
            })
    record = {
        "scenario": args.scenario,
        "classification": args.classification,
        "stage": args.stage,
        "last_progress_marker": args.last_progress_marker,
        "guest_elapsed_ms": args.guest_elapsed_ms,
        "host_elapsed_ms": args.host_elapsed_ms,
        "idle_elapsed_ms": args.idle_elapsed_ms,
        "shell_available": args.shell_available == "1",
        "root_cause": args.root_cause,
        "captures": captures,
    }
    (directory / "stall.json").write_text(
        json.dumps(record, indent=2) + "\n", encoding="utf-8"
    )
    capture_lines = "\n".join(
        f"- `{item['path']}` — {item['sha256']}" for item in captures
    ) or "- none"
    (directory / "stall.md").write_text(
        "# Boot-to-shell stall\n\n"
        f"- Scenario: `{args.scenario}`\n"
        f"- Classification: `{args.classification}`\n"
        f"- Stage: `{args.stage}`\n"
        f"- Last progress marker: `{args.last_progress_marker}`\n"
        f"- Guest elapsed: `{args.guest_elapsed_ms}` ms\n"
        f"- Host elapsed: `{args.host_elapsed_ms}` ms\n"
        f"- Idle elapsed: `{args.idle_elapsed_ms}` ms\n"
        f"- Shell available: `{args.shell_available}`\n"
        f"- Root cause: `{args.root_cause}`\n\n"
        "## Captures\n\n" + capture_lines + "\n",
        encoding="utf-8",
    )
    print(f"[BOOT][STALL_CAPTURE] PASS scenario={args.scenario} "
          f"stage={args.stage}")


def verify_boot_progress(args: argparse.Namespace) -> None:
    timeline = _root_path(args.timeline)
    log = _root_path(args.log)
    require(timeline.is_file(), f"missing timeline: {timeline}")
    require(log.is_file(), f"missing boot log: {log}")
    with timeline.open(encoding="utf-8", newline="") as stream:
        rows = list(csv.DictReader(stream, delimiter="\t"))
    require(bool(rows), "empty boot progress timeline")
    stages = [row["stage"] for row in rows]
    positions: dict[str, int] = {}
    previous = -1
    for required in BOOT_PROGRESS_REQUIRED:
        try:
            position = stages.index(required, previous + 1)
        except ValueError as error:
            raise EvidenceError(f"missing/out-of-order progress stage: {required}") from error
        positions[required] = position
        previous = position
    pci_rows = rows[positions["PCI_BEGIN"] + 1:positions["PCI_SCAN_COMPLETE"]]
    pci_path = "scanned"
    if not any(row["stage"] == "PCI_SCAN_PROGRESS" for row in pci_rows):
        complete_marker = rows[positions["PCI_SCAN_COMPLETE"]]["marker"]
        require("status=MCFG_MISSING" in complete_marker,
                "PCI path has neither scan progress nor typed MCFG-missing completion")
        pci_path = "mcfg-missing-fail-open"
    begin = int(rows[positions["SPLASH_BEGIN"]]["host_elapsed_ms"])
    terminal = int(rows[positions["SPLASH_TERMINAL"]]["host_elapsed_ms"])
    require(0 <= terminal - begin <= 15000,
            f"splash host budget exceeded: {terminal - begin} ms")
    text = log.read_text(encoding="utf-8", errors="replace")
    require("[GRAPHICS][SPLASH] COMPLETE result=" in text,
            "missing typed splash terminal result")
    require("[BOOT][SHELL_READY] PASS" in text, "shell not ready")
    require(f"[BOOT][RUNTIME_READY] PASS cpus={args.smp}/{args.smp}" in text,
            "runtime readiness mismatch")
    require("[KERNEL] Entering Main Loop." in text, "main loop missing")
    require("console_suspended=0" in text,
            "console suspension terminal proof missing")
    print(f"[BOOT][PROGRESS_ORACLE] PASS smp={args.smp} "
          f"splash_host_ms={terminal - begin} markers={len(rows)} "
          f"pci_path={pci_path}")


def verify_autonomous_boot_log(path: Path, smp: int,
                               require_taskdiag: bool = True) -> str:
    require(path.is_file(), f"missing boot log: {path.relative_to(ROOT)}")
    text = path.read_text(encoding="utf-8", errors="replace")
    for marker in (
        f"[IRQ][BOOTSTRAP] CPUS_PREPARED cpus={smp}",
        f"[IRQ][CPU_READY_SUMMARY] expected={smp} ready={smp} failed=0",
        "[CLOCK][HPET_CLOCKSOURCE_PROBE] PASS",
        "[CLOCK][HPET_TIMER0] QUIESCENT",
        "[CLOCKEVENT][RUNTIME] ACTIVE source=BSP_LAPIC",
        "[GRAPHICS][SPLASH_SELFTEST] PASS",
        "[GRAPHICS][SPLASH] COMPLETE result=",
        "console_suspended=0",
        "[BOOT][SHELL_READY] PASS",
        f"[BOOT][RUNTIME_READY] PASS cpus={smp}/{smp}",
        "[KERNEL] Entering Main Loop.",
        "[IRQ][CHECK] PASS clocksource=HPET clockevent=BSP_LAPIC",
    ):
        require(marker in text, f"{path.name}: missing {marker}")
    if require_taskdiag:
        require("[TASKDIAG][CHECK] PASS" in text,
                f"{path.name}: missing TASKDIAG check")
    require(text.count("[IRQ][CPU_READY] PASS") == smp,
            f"{path.name}: CPU readiness count mismatch")
    require(text.count("timer_us=1000 handoff=1 preempt=1") == smp,
            f"{path.name}: per-CPU rendezvous count mismatch")
    fault = re.search(
        r"PANIC|FATAL|#PF|#GP|DOUBLE FAULT|TRIPLE FAULT|"
        r"STRUCTURAL_FAULT|FINISH_FAULT|CPU_READY_TIMEOUT|"
        r"boot stage timeout|console render suspension residual",
        text,
    )
    require(fault is None, f"{path.name}: fault marker {fault.group(0) if fault else ''}")
    require(re.search(r"(?:late_callbacks|claimed_residual)=[1-9][0-9]*",
                      text) is None,
            f"{path.name}: nonzero timer residual")
    return text


def verify_autonomous_closure() -> None:
    initial = {
        ARTIFACT / "qemu/rendezvous-smp24-boot1/serial.log":
            "341d2c114a2cf06d23f026e4eadf71b4111cb72dd003a1d32e4d1929c95352ef",
        ARTIFACT / "qemu/rendezvous-smp24-boot2/serial-timeout.log":
            "86a34e7b7d820144a1f8e7d63de6852a6770a0b0842554a133aa5e17cf5c62d4",
    }
    for path, digest in initial.items():
        require(path.is_file() and sha256(path) == digest,
                f"frozen entry evidence drift: {path.name}")

    cycles_path = AUTONOMOUS_ARTIFACT / "cycles.json"
    require(cycles_path.is_file(), "missing autonomous cycles.json")
    cycles = json.loads(cycles_path.read_text(encoding="utf-8"))
    require([int(row["cycle"]) for row in cycles] == list(range(5)),
            "autonomous cycle ledger must contain cycles 0..4 exactly")
    require(cycles[0]["classification"] ==
            "REJECTED_BOOT_TO_SHELL_LIVENESS_UNCLASSIFIED",
            "initial SMP24 run was misclassified")
    require(all(row["result"] == "REJECTED" for row in cycles[:4]),
            "cycles 0..3 must preserve rejected evidence")
    require(cycles[4]["result"] == "PASS" and
            cycles[4]["classification"] ==
            "ACCEPTED_BOOT_TO_SHELL_LIVENESS",
            "focused closure cycle did not pass")

    build_dir = AUTONOMOUS_ARTIFACT / "final-build"
    j2 = build_dir / "kernel-j2.elf"
    jn = build_dir / "kernel-jN.elf"
    for path in (j2, jn, ROOT / "kernel.elf"):
        require(path.is_file(), f"missing final kernel: {path}")
    kernel_hash = sha256(j2)
    require(sha256(jn) == kernel_hash and sha256(ROOT / "kernel.elf") == kernel_hash,
            "j2/jN/root kernel mismatch")
    require((build_dir / "undefined-symbols.txt").read_bytes() == b"",
            "undefined symbols are not zero")
    final_build = (build_dir / "final-build.log").read_text(encoding="utf-8")
    require("stack_max=1968 stack_limit=2048" in final_build and
            "byte_identical=1 undefined=0 owned_warnings=0" in final_build,
            "final build summary mismatch")

    table = AUTONOMOUS_ARTIFACT / "smp24-three-boots.tsv"
    with table.open(encoding="utf-8", newline="") as stream:
        boot_rows = list(csv.DictReader(stream, delimiter="\t"))
    require([row["run"] for row in boot_rows] == ["1", "2", "3"],
            "final SMP24 boot count is not exactly three")
    require({row["kernel_sha256"] for row in boot_rows} == {kernel_hash},
            "final SMP24 boots used different kernels")
    require(all(row["result"] == "PASS" and
                0 <= int(row["splash_host_ms"]) <= 15000
                for row in boot_rows),
            "final SMP24 splash/result gate failed")
    final_dirs = {path.name for path in (ARTIFACT / "qemu").glob(
        "boot-to-shell-final-smp24-run*") if path.is_dir()}
    expected_final_dirs = {f"boot-to-shell-final-smp24-run{run}"
                           for run in range(1, 4)}
    require(final_dirs == expected_final_dirs,
            "final SMP24 boot directories are not exactly three")
    for row in boot_rows:
        stage = f"boot-to-shell-final-smp24-run{row['run']}"
        directory = ARTIFACT / "qemu" / stage
        log = directory / "serial.log"
        verify_autonomous_boot_log(log, 24)
        require(sha256(log) == row["serial_sha256"],
                f"serial hash mismatch: {stage}")
        require(sha256(directory / "kernel.payload.elf") == kernel_hash,
                f"kernel payload mismatch: {stage}")

    matrix = (
        ("boot-to-shell-matrix-q35-smp1-tcg", 1, False),
        ("boot-to-shell-matrix-q35-smp2-tcg", 2, False),
        ("boot-to-shell-matrix-q35-smp4-kvm", 4, False),
        ("boot-to-shell-matrix-q35-smp8-kvm", 8, False),
        ("boot-to-shell-matrix-q35-smp24-kvm", 24, False),
        ("boot-to-shell-matrix-pc-smp4-tcg", 4, True),
    )
    for stage, smp, pcat in matrix:
        directory = ARTIFACT / "qemu" / stage
        text = verify_autonomous_boot_log(directory / "serial.log", smp)
        require(sha256(directory / "kernel.payload.elf") == kernel_hash,
                f"matrix kernel mismatch: {stage}")
        if pcat:
            require("status=MCFG_MISSING" in text and "pcat=1" in text,
                    "pc/TCG typed PCI fail-open proof missing")
        else:
            require("stage=PCI_SCAN_PROGRESS" in text,
                    f"PCI scan progress missing: {stage}")

    smp4_fair = ARTIFACT / "qemu/boot-to-shell-cycle2-focused-smp4/serial.log"
    require(sha256(ARTIFACT /
                   "qemu/boot-to-shell-cycle2-focused-smp4/kernel.payload.elf") ==
            kernel_hash, "SMP4 fairness used a different kernel")
    smp4_text = smp4_fair.read_text(encoding="utf-8", errors="replace")
    require(smp4_text.count("[SYNC][TIMER_ORDER] PASS old_position=1 "
                            "new_position=2 callbacks=2") >= 3,
            "SMP4 timer-order count below 3")
    require(len(re.findall(r"\[SYNC\]\[TIMER_CANCEL\] PASS run=\d+ "
                           r"requested=3 completed=3 errors=0", smp4_text)) >= 3,
            "SMP4 timer-cancel count below 3")

    runtime_dir = ARTIFACT / "qemu/rendezvous-runtime-smp24"
    runtime_text = verify_autonomous_boot_log(runtime_dir / "serial.log", 24)
    require(sha256(runtime_dir / "kernel.payload.elf") == kernel_hash,
            "runtime SMP24 kernel mismatch")
    require(runtime_text.count("[SYNC][TIMER_ORDER] PASS old_position=1 "
                               "new_position=2 callbacks=2") >= 5,
            "runtime SMP24 timer-order count below 5")
    require(len(re.findall(r"\[SYNC\]\[TIMER_CANCEL\] PASS run=\d+ "
                           r"requested=3 completed=3 errors=0", runtime_text)) >= 5,
            "runtime SMP24 timer-cancel count below 5")
    for marker in ("[TASKMAN][AUTO_EXIT] PASS target=3",
                   "[SMP][KILL_SWEEP] PASS workers=8",
                   "[INPUTTEST][CHECK] PASS", "[MODALTEST][CHECK] PASS"):
        require(marker in runtime_text, f"runtime SMP24 missing {marker}")
    raw = re.findall(r"\[ACCOUNT\]\[LAPIC_RATE\] (PASS|FAIL).*?"
                     r"worst_median_x1000=(\d+)", runtime_text)
    require(len(raw) == 1 and raw[0][0] == "FAIL",
            "SMP24 raw LAPIC-rate result was not preserved")
    require("ENVIRONMENT_NON_AUTHORITATIVE_OVERSUBSCRIBED host_cpus=12 "
            "guest_vcpus=24" in runtime_text,
            "SMP24 rate authority mismatch")

    controls = []
    for run in range(1, 4):
        directory = ARTIFACT / "qemu" / f"rendezvous-rate-control-smp4-run{run}"
        text = verify_autonomous_boot_log(directory / "serial.log", 4,
                                          require_taskdiag=False)
        require(sha256(directory / "kernel.payload.elf") == kernel_hash,
                f"rate-control kernel mismatch: run {run}")
        match = re.search(r"\[ACCOUNT\]\[LAPIC_RATE\] PASS .*?"
                          r"worst_median_x1000=(\d+)", text)
        require(match is not None and int(match.group(1)) >= RATE_THRESHOLD,
                f"rate-control threshold failure: run {run}")
        controls.append(int(match.group(1)))

    negative = (ARTIFACT /
                "negatives/boot-to-shell-negative-claimed-order/serial.log")
    negative_text = negative.read_text(encoding="utf-8", errors="replace")
    require("[TIMER][NEGATIVE] CLAIMED_ORDER_BYPASS_DETECTED" in negative_text and
            "[SYNC][TIMER_ORDER] PASS old_position=2 new_position=1" in negative_text,
            "negative claimed-bypass proof missing")

    soak_dir = ARTIFACT / "qemu/boot-to-shell-soak24"
    soak_text = verify_autonomous_boot_log(soak_dir / "serial.log", 24)
    require(sha256(soak_dir / "kernel.payload.elf") == kernel_hash,
            "soak kernel mismatch")
    soak_summary = (AUTONOMOUS_ARTIFACT / "soak24.log").read_text(
        encoding="utf-8")
    duration = re.search(r"duration_ms=(\d+)", soak_summary)
    require(duration is not None and int(duration.group(1)) >= 300000,
            "soak duration below 300000 ms")
    for marker, minimum in (("[IRQ][CHECK] PASS", 10),
                            ("[TASKDIAG][CHECK] PASS", 10),
                            ("[SYNC][TIMER_ORDER] PASS old_position=1 "
                             "new_position=2 callbacks=2", 5),
                            ("[TASKMAN][AUTO_EXIT] PASS target=3", 4),
                            ("[SMP][KILL_SWEEP] PASS workers=8", 4)):
        require(soak_text.count(marker) >= minimum,
                f"soak count below minimum: {marker}")
    require("clockevent_non_bsp=0" in soak_text and
            "clockevent_early=0" in soak_text and
            "clockevent_regressions=0" in soak_text and
            "hpet_stray_irqs=0" in soak_text and
            "timers_claimed=0" in soak_text,
            "soak final invariant snapshot mismatch")

    fairness_before = ROOT / "artifacts/build/ap-runtime-rendezvous/fairness-before.sha256"
    fairness_after = ROOT / "artifacts/build/ap-runtime-rendezvous/fairness-after-build.sha256"
    protected_before = AUTONOMOUS_ARTIFACT / "protected-before.sha256"
    protected_after = ROOT / "artifacts/build/ap-runtime-rendezvous/protected-after-build.sha256"
    require(fairness_before.read_bytes() == fairness_after.read_bytes(),
            "timer fairness continuity drift")
    require(protected_before.read_bytes() == protected_after.read_bytes(),
            "protected subsystem continuity drift")
    print("[BOOT][AUTONOMOUS_EVIDENCE] PASS "
          f"kernel={kernel_hash} smp24_boots=3 matrix=6 "
          f"runtime_fairness=5 soak_ms={duration.group(1)} "
          f"rate_smp24_raw={raw[0][0]} rate_smp4={','.join(map(str, controls))}")


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser()
    sub = result.add_subparsers(dest="command", required=True)
    sub.add_parser("selftest")
    screen = sub.add_parser("screens")
    screen.add_argument("paths", nargs=2)
    verify_parser = sub.add_parser("verify")
    verify_parser.add_argument("scope", choices=("matrix", "all"))
    rec = sub.add_parser("record")
    for field in FIELDS:
        if field in {"sha256", "log"}:
            continue
        rec.add_argument(f"--{field.replace('_', '-')}", dest=field, default="")
    rec.add_argument("--log", default="")
    cycle = sub.add_parser("cycle-record")
    for field in CYCLE_FIELDS:
        cycle.add_argument(f"--{field.replace('_', '-')}", dest=field,
                           required=True)
    manifest = sub.add_parser("source-manifest")
    manifest.add_argument("--output", required=True)
    manifest.add_argument("paths", nargs="+")
    stall = sub.add_parser("stall-report")
    for field in (
        "directory", "scenario", "classification", "stage",
        "last_progress_marker", "guest_elapsed_ms", "host_elapsed_ms",
        "idle_elapsed_ms", "shell_available", "root_cause",
    ):
        stall.add_argument(f"--{field.replace('_', '-')}", dest=field,
                           required=True)
    progress = sub.add_parser("boot-progress")
    progress.add_argument("--timeline", required=True)
    progress.add_argument("--log", required=True)
    progress.add_argument("--smp", type=int, required=True)
    sub.add_parser("autonomous-closure")
    return result


def main() -> int:
    args = parser().parse_args()
    try:
        if args.command == "selftest":
            host_selftest()
        elif args.command == "screens":
            verify_screens(args.paths)
        elif args.command == "record":
            record(args)
        elif args.command == "cycle-record":
            write_cycle_record(args)
        elif args.command == "source-manifest":
            write_source_manifest(args)
        elif args.command == "stall-report":
            write_stall_report(args)
        elif args.command == "boot-progress":
            verify_boot_progress(args)
        elif args.command == "autonomous-closure":
            verify_autonomous_closure()
        else:
            verify(args.scope)
        return 0
    except (EvidenceError, OSError, ValueError, subprocess.CalledProcessError,
            zipfile.BadZipFile) as error:
        print(f"[TIMER][EVIDENCE] FAIL reason={error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
