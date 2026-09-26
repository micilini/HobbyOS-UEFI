#!/usr/bin/env python3
"""Offline verifier for the Foundation Stage 1 bare-metal handoff."""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import re
import subprocess
import sys
import tempfile


class EvidenceError(RuntimeError):
    def __init__(self, code: str, detail: str = "") -> None:
        super().__init__(code)
        self.code = code
        self.detail = detail


def require(condition: bool, code: str, detail: str = "") -> None:
    if not condition:
        raise EvidenceError(code, detail)


def sha256(path: pathlib.Path) -> str:
    require(path.is_file(), "FILE_MISSING", str(path))
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def single(pattern: re.Pattern[str], text: str, code: str) -> re.Match[str]:
    matches = list(pattern.finditer(text))
    require(len(matches) == 1, code, f"count={len(matches)}")
    return matches[0]


IRQ_CHECK_RE = re.compile(
    r"^\[IRQ\]\[CHECK\] (?P<status>PASS|FAIL) "
    r"clocksource=(?P<clocksource>\S+) clockevent=(?P<clockevent>\S+) "
    r"period_us=(?P<period>[0-9]+) hpet_timer0=(?P<hpet>\S+) "
    r"non_bsp_ticks=(?P<non_bsp>[0-9]+) stray_hpet=(?P<stray>[0-9]+) "
    r"cpus=(?P<ready>[0-9]+)/(?P<expected>[0-9]+) "
    r"controllers=(?P<controllers>[0-9]+) routes=(?P<routes>[0-9]+) "
    r"unexpected=(?P<unexpected>[0-9]+) imbalance=(?P<imbalance>[0-9]+) "
    r"snapshot_polls=(?P<snapshot_polls>[0-9]+) "
    r"validation_polls=(?P<validation_polls>[0-9]+) "
    r"elapsed_ms=(?P<elapsed>[0-9]+) reason=(?P<reason>\S+)$",
    re.MULTILINE,
)

IRQ_BOOT_RE = re.compile(
    r"^\[IRQ\]\[BOOT_RESULT\] (?P<status>PASS|FAIL) "
    r"snapshots=(?P<snapshots>[0-9]+) polls=(?P<polls>[0-9]+) "
    r"elapsed_ms=(?P<elapsed>[0-9]+) acquisition_ms=(?P<acquisition>[0-9]+) "
    r"publication_ms=(?P<publication>[0-9]+) budget_ms=(?P<budget>[0-9]+)$",
    re.MULTILINE,
)

MEMORY_RE = re.compile(
    r"^\[MEM\]\[SUMMARY\] (?P<status>PASS|FAIL) "
    r"pmm_total=(?P<pmm_total>[0-9]+) pmm_free=(?P<pmm_free>[0-9]+) "
    r"pmm_used=(?P<pmm_used>[0-9]+) heap_total=(?P<heap_total>[0-9]+) "
    r"heap_used=(?P<heap_used>[0-9]+) heap_free=(?P<heap_free>[0-9]+) "
    r"heap_blocks=(?P<heap_blocks>[0-9]+) "
    r"heap_free_blocks=(?P<heap_free_blocks>[0-9]+) "
    r"heap_largest_free=(?P<heap_largest_free>[0-9]+) "
    r"pmm_integrity=(?P<pmm_integrity>[01]) "
    r"heap_integrity=(?P<heap_integrity>[01])$",
    re.MULTILINE,
)

CPU_RE = re.compile(
    r"^\[CPU\]\[SUMMARY\] (?P<status>PASS|FAIL) "
    r"vendor=(?P<vendor>\S+) family=(?P<family>[0-9]+) "
    r"model=(?P<model>[0-9]+) stepping=(?P<stepping>[0-9]+) "
    r"max_basic=(?P<max_basic>[0-9]+) max_ext=(?P<max_ext>[0-9]+) "
    r"logical_cpus=(?P<logical_cpus>[0-9]+) "
    r"long_mode=(?P<long_mode>[01]) htt=(?P<htt>[01]) "
    r"hybrid=(?P<hybrid>[01]) core_type=(?P<core_type>[0-9]+)$",
    re.MULTILINE,
)

LOCK_RE = re.compile(
    r"^\[LOCKTEST\]\[PROGRESS\] (?P<status>PASS|FAIL) "
    r"cpus=(?P<cpus>[0-9]+) iterations=(?P<iterations>[0-9]+) "
    r"acquisitions=(?P<acquisitions>[0-9]+) "
    r"exclusive_violations=(?P<exclusive>[0-9]+) "
    r"if_violations=(?P<if_violations>[0-9]+) "
    r"max_queue_distance=(?P<distance>[0-9]+) limit=(?P<limit>[0-9]+) "
    r"slots=(?P<slots>[0-9]+) slot_mask=(?P<slot_mask>[0-9]+) "
    r"observer_samples=(?P<samples>[0-9]+) contended=(?P<contended>[0-9]+) "
    r"trylock_no_ticket=(?P<trylock>[01]) static_zero=(?P<static_zero>[01]) "
    r"reaped=(?P<reaped>[0-9]+) max_active=(?P<max_active>[0-9]+)$",
    re.MULTILINE,
)

ALLOC_RE = re.compile(
    r"^\[ALLOCTEST\]\[HARDENING\] (?P<status>PASS|FAIL) "
    r"heap_overflow=(?P<heap_overflow>[0-9]+) "
    r"heap_interior=(?P<heap_interior>[0-9]+) "
    r"heap_foreign=(?P<heap_foreign>[0-9]+) "
    r"heap_double=(?P<heap_double>[0-9]+) "
    r"pmm_null=(?P<pmm_null>[0-9]+) "
    r"pmm_unaligned=(?P<pmm_unaligned>[0-9]+) "
    r"pmm_outside=(?P<pmm_outside>[0-9]+) "
    r"pmm_reserved=(?P<pmm_reserved>[0-9]+) "
    r"pmm_double=(?P<pmm_double>[0-9]+) "
    r"bitmap_words=(?P<bitmap_words>[0-9]+) "
    r"pmm_word_calls=(?P<pmm_word_calls>[0-9]+) "
    r"pmm_word_iterations=(?P<pmm_word_iterations>[0-9]+) "
    r"heap_integrity=(?P<heap_integrity>[01]) "
    r"pmm_integrity=(?P<pmm_integrity>[01]) census=(?P<census>[01]) "
    r"heap_ok=(?P<heap_ok>[01]) pmm_ok=(?P<pmm_ok>[01]) "
    r"bitmap_ok=(?P<bitmap_ok>[01])$",
    re.MULTILINE,
)


def integer_fields(match: re.Match[str], excluded: set[str]) -> dict[str, int]:
    return {key: int(value) for key, value in match.groupdict().items()
            if key not in excluded}


def verify_log_text(text: str, profile: str, cpus: int, iterations: int,
                    physical: bool) -> dict[str, int | str]:
    require(cpus > 0 and cpus <= 64, "CPU_ARGUMENT_INVALID", str(cpus))
    text = text.replace("\r", "")
    fault = re.search(
        r"PANIC|#PF|#GP|TRIPLE[ _-]?FAULT|STRUCTURAL_FAULT|FINISH_FAULT|"
        r"^\[[^\n]+\] FAIL(?: |$)", text, re.MULTILINE | re.IGNORECASE)
    require(fault is None, "FAULT_MARKER", fault.group(0) if fault else "")

    required = (
        "[IRQ][PIC] QUIESCENT",
        "[IRQ][IOAPIC] QUIESCENT",
        f"[IRQ][BOOTSTRAP] CPUS_PREPARED cpus={cpus}",
        "[IRQ][BSP_LAPIC_PROBE] PASS",
        "[CLOCK][HPET_CLOCKSOURCE_PROBE] PASS",
        "[CLOCK][HPET_TIMER0] QUIESCENT",
        "[CLOCKEVENT][RUNTIME] ACTIVE source=BSP_LAPIC bsp_slot=0 period_us=1000",
        f"[IRQ][BOOTSTRAP] CLOCKEVENT_ACTIVE cpus={cpus}",
        "=== xHCI DRIVER INITIALIZATION ===",
        "[XHCI] Starting Controller... OK (Running).",
        "[XHCI] Ports detected: USB2=",
        "[USB] Hotplug State Machine Initialized.",
        f"[BOOT][RUNTIME_READY] PASS cpus={cpus}/{cpus}",
        "[BOOT][TEST_READY] PASS autorun=0 selftests=0",
        "[KERNEL] Entering Main Loop.",
    )
    for marker in required:
        require(marker in text, "BOOT_MARKER_MISSING", marker)
    ready_count = text.count("[IRQ][CPU_READY] PASS")
    require(ready_count == cpus, "CPU_READY_COUNT", str(ready_count))

    irq = single(IRQ_CHECK_RE, text, "IRQ_CHECK_COUNT")
    require(irq["status"] == "PASS", "IRQ_CHECK_STATUS")
    require(irq["clocksource"] == "HPET" and
            irq["clockevent"] == "BSP_LAPIC" and
            irq["hpet"] == "QUIESCENT" and irq["reason"] == "ok",
            "IRQ_CLOCK_CONTRACT")
    irq_values = integer_fields(
        irq, {"status", "clocksource", "clockevent", "hpet", "reason"})
    require(irq_values["period"] == 1000 and
            irq_values["non_bsp"] == 0 and irq_values["stray"] == 0 and
            irq_values["ready"] == cpus and irq_values["expected"] == cpus and
            irq_values["controllers"] > 0 and irq_values["routes"] > 0 and
            irq_values["unexpected"] == 0 and irq_values["imbalance"] == 0,
            "IRQ_CHECK_FIELDS")

    boot = single(IRQ_BOOT_RE, text, "IRQ_BOOT_COUNT")
    boot_values = integer_fields(boot, {"status"})
    require(boot["status"] == "PASS" and
            boot_values["snapshots"] == cpus and
            boot_values["polls"] >= cpus and
            boot_values["elapsed"] <= boot_values["budget"] and
            boot_values["acquisition"] + boot_values["publication"] ==
            boot_values["elapsed"], "IRQ_BOOT_FIELDS")

    memory = single(MEMORY_RE, text, "MEMORY_RECORD_COUNT")
    memory_values = integer_fields(memory, {"status"})
    require(memory["status"] == "PASS", "MEMORY_STATUS")
    require(memory_values["pmm_total"] > 0 and
            memory_values["pmm_free"] <= memory_values["pmm_total"] and
            memory_values["pmm_free"] + memory_values["pmm_used"] ==
            memory_values["pmm_total"] and
            memory_values["heap_total"] > 0 and
            memory_values["heap_used"] + memory_values["heap_free"] ==
            memory_values["heap_total"] and
            memory_values["heap_free_blocks"] <= memory_values["heap_blocks"] and
            memory_values["heap_largest_free"] <= memory_values["heap_free"] and
            memory_values["pmm_integrity"] == 1 and
            memory_values["heap_integrity"] == 1, "MEMORY_FIELDS")
    if physical:
        require(memory_values["pmm_total"] > 4 * 1024 * 1024 * 1024,
                "PHYSICAL_MEMORY_NOT_ABOVE_4G")

    cpu = single(CPU_RE, text, "CPU_RECORD_COUNT")
    cpu_values = integer_fields(cpu, {"status", "vendor"})
    require(cpu["status"] == "PASS" and
            cpu_values["logical_cpus"] == cpus and
            cpu_values["long_mode"] == 1 and cpu_values["max_basic"] >= 1,
            "CPU_FIELDS")
    if physical:
        require(cpu["vendor"] == "GenuineIntel", "PHYSICAL_VENDOR")
        require(cpu_values["hybrid"] == 1 and cpu_values["core_type"] != 0,
                "PHYSICAL_HYBRID_TOPOLOGY")

    ordered = [
        "[BOOT][RUNTIME_READY] PASS",
        "[IRQ][CHECK] PASS",
        "[IRQ][BOOT_RESULT] PASS",
        "[MEM][SUMMARY] PASS",
        "[CPU][SUMMARY] PASS",
    ]

    if profile == "production":
        require("[LOCKTEST]" not in text and "[ALLOCTEST]" not in text and
                "[BUILD_PROFILE]" not in text,
                "SELFTEST_MARKER_IN_PRODUCTION")
    else:
        lock = single(LOCK_RE, text, "LOCK_RECORD_COUNT")
        lock_values = integer_fields(lock, {"status"})
        expected_acquisitions = cpus * iterations
        require(lock["status"] == "PASS" and
                lock_values["cpus"] == cpus and
                lock_values["iterations"] == iterations and
                lock_values["acquisitions"] == expected_acquisitions and
                lock_values["exclusive"] == 0 and
                lock_values["if_violations"] == 0 and
                lock_values["limit"] == cpus - 1 and
                lock_values["distance"] <= lock_values["limit"] and
                lock_values["slots"] == cpus and
                lock_values["slot_mask"] == (1 << cpus) - 1 and
                lock_values["samples"] == expected_acquisitions and
                lock_values["contended"] > 0 and
                lock_values["trylock"] == 1 and
                lock_values["static_zero"] == 1 and
                lock_values["reaped"] == cpus and
                lock_values["max_active"] == cpus,
                "LOCK_FIELDS")

        alloc = single(ALLOC_RE, text, "ALLOC_RECORD_COUNT")
        alloc_values = integer_fields(alloc, {"status"})
        expected_alloc = {
            "heap_overflow": 2, "heap_interior": 1, "heap_foreign": 1,
            "heap_double": 2, "pmm_null": 1, "pmm_unaligned": 1,
            "pmm_outside": 1, "pmm_reserved": 1, "pmm_double": 1,
            "bitmap_words": 3, "heap_integrity": 1, "pmm_integrity": 1,
            "census": 1, "heap_ok": 1, "pmm_ok": 1, "bitmap_ok": 1,
        }
        require(alloc["status"] == "PASS" and all(
            alloc_values[key] == value for key, value in expected_alloc.items()) and
            alloc_values["pmm_word_calls"] >= 1 and
            alloc_values["pmm_word_iterations"] >=
            alloc_values["pmm_word_calls"], "ALLOC_FIELDS")
        ordered.extend(("[LOCKTEST][PROGRESS] PASS",
                        "[ALLOCTEST][HARDENING] PASS"))

    positions = [text.find(marker) for marker in ordered]
    require(all(position >= 0 for position in positions) and
            positions == sorted(positions), "COMMAND_ORDER")
    return {
        "profile": profile,
        "cpus": cpus,
        "pmm_total": memory_values["pmm_total"],
        "irq_snapshots": boot_values["snapshots"],
    }


def read_manifest(path: pathlib.Path) -> dict[str, str]:
    require(path.is_file(), "MANIFEST_MISSING", str(path))
    result: dict[str, str] = {}
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        digest, separator, name = line.partition("  ")
        require(separator == "  " and re.fullmatch(r"[0-9a-f]{64}", digest)
                is not None and name and name not in result,
                "MANIFEST_LINE", f"{path}:{number}")
        result[name] = digest
    require(bool(result), "MANIFEST_EMPTY", str(path))
    return result


def verify_payload_manifest(profile: pathlib.Path) -> None:
    manifest = read_manifest(profile / "payload-manifest.txt")
    expected_names = {
        "/kernel.elf", "/EFI/BOOT/BOOTX64.EFI",
        "/EFI/fonts/zap-light16.psf", "/EFI/images/logo.bmp", "/startup.nsh",
    }
    require(set(manifest) == expected_names, "PAYLOAD_MANIFEST_NAMES")
    image = profile / "hobbyos.img"
    with tempfile.TemporaryDirectory(prefix="hobbyos-bm-payload-") as raw:
        temporary = pathlib.Path(raw)
        for image_name in sorted(expected_names):
            destination = temporary / pathlib.PurePosixPath(image_name).name
            completed = subprocess.run(
                ["mcopy", "-i", str(image), f"::{image_name}", str(destination)],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
            )
            require(completed.returncode == 0, "PAYLOAD_EXTRACT",
                    image_name)
            require(sha256(destination) == manifest[image_name],
                    "PAYLOAD_HASH", image_name)
        require((temporary / "kernel.elf").read_bytes() ==
                (profile / "kernel.elf").read_bytes(), "EMBEDDED_KERNEL")
        require((temporary / "BOOTX64.EFI").read_bytes() ==
                (profile / "BOOTX64.EFI").read_bytes(), "EMBEDDED_BOOTX64")


def verify_candidate(root: pathlib.Path) -> dict[str, str]:
    root = root.resolve()
    required = {
        "candidate.txt", "E1-BM-roteiro.md", "verify-bare-metal.py",
        "production/hobbyos.img", "production/kernel.elf",
        "production/BOOTX64.EFI", "production/payload-manifest.txt",
        "production/SHA256SUMS", "production/fat-fsck.txt",
        "production/fat-metadata.txt",
        "selftest/hobbyos.img", "selftest/kernel.elf",
        "selftest/BOOTX64.EFI", "selftest/payload-manifest.txt",
        "selftest/SHA256SUMS", "selftest/fat-fsck.txt",
        "selftest/fat-metadata.txt",
    }
    manifest = read_manifest(root / "SHA256SUMS")
    require(required <= set(manifest), "CANDIDATE_FILES")
    for name, digest in manifest.items():
        relative = pathlib.PurePosixPath(name)
        require(not relative.is_absolute() and ".." not in relative.parts,
                "MANIFEST_PATH", name)
        path = root.joinpath(*relative.parts).resolve()
        require(path.is_relative_to(root), "MANIFEST_ESCAPE", name)
        require(sha256(path) == digest, "CANDIDATE_HASH", name)

    metadata: dict[str, str] = {}
    for line in (root / "candidate.txt").read_text(encoding="utf-8").splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            metadata[key] = value
    expected_metadata = {
        "SCHEMA": "1",
        "STATUS": "READY_FOR_MAINTAINER_BARE_METAL_VALIDATION",
        "TARGET": "INTEL_CORE_I9_12900K_24_THREADS",
        "LAPIC_PERIOD_US": "1000",
        "BARE_METAL_EXECUTED_BY_CODEX": "NO",
        "AMD_MACHINE_AVAILABLE": "NO",
    }
    require(all(metadata.get(key) == value
                for key, value in expected_metadata.items()),
            "CANDIDATE_METADATA")
    for key in ("SOURCE_COMMIT", "SOURCE_TREE"):
        require(re.fullmatch(r"[0-9a-f]{40}", metadata.get(key, ""))
                is not None, "CANDIDATE_SOURCE_ID", key)
    for profile_name in ("production", "selftest"):
        profile = root / profile_name
        require(metadata.get(f"{profile_name.upper()}_KERNEL_SHA256") ==
                sha256(profile / "kernel.elf"), "METADATA_KERNEL", profile_name)
        require(metadata.get(f"{profile_name.upper()}_IMAGE_SHA256") ==
                sha256(profile / "hobbyos.img"), "METADATA_IMAGE", profile_name)
        verify_payload_manifest(profile)

    production = (root / "production/kernel.elf").read_bytes()
    selftest = (root / "selftest/kernel.elf").read_bytes()
    for marker in (b"locktest", b"alloctest", b"profiletest"):
        require(marker not in production.lower(), "PRODUCTION_SELFTEST_STRING",
                marker.decode())
    for marker in (b"[LOCKTEST][PROGRESS]", b"[ALLOCTEST][HARDENING]",
                   b"[BUILD_PROFILE][MEMORY]"):
        require(marker in selftest, "SELFTEST_MARKER_MISSING", marker.decode())
    for binary in (production, selftest):
        require(b"[MEM][SUMMARY]" in binary and b"[CPU][SUMMARY]" in binary,
                "DIAGNOSTIC_SUMMARY_MISSING")
    return metadata


def fixture_log(profile: str) -> str:
    cpus = 24
    ready = "\n".join(
        f"[IRQ][CPU_READY] PASS slot={slot} apic={slot} timer_us=1000 "
        "handoff=1 preempt=1 stage=READY" for slot in range(cpus))
    common = f"""[IRQ][PIC] QUIESCENT master=ff slave=ff pcat=1
[IRQ][IOAPIC] QUIESCENT controllers=1 entries=24 masked=24
[IRQ][BOOTSTRAP] CPUS_PREPARED cpus={cpus}
[IRQ][BSP_LAPIC_PROBE] PASS vector=34 entered=1 returned=1 period_us=1000
[CLOCK][HPET_CLOCKSOURCE_PROBE] PASS samples=1 delta_ticks=20 delta_ns=200 timer0=QUIESCENT
[CLOCK][HPET_TIMER0] QUIESCENT irq_enabled=0 route_enabled=0 pending=0 legacy=0
[CLOCKEVENT][RUNTIME] ACTIVE source=BSP_LAPIC bsp_slot=0 period_us=1000
{ready}
[IRQ][BOOTSTRAP] CLOCKEVENT_ACTIVE cpus={cpus}
=== xHCI DRIVER INITIALIZATION ===
[XHCI] Starting Controller... OK (Running).
[XHCI] Ports detected: USB2=4 USB3=4
[USB] Hotplug State Machine Initialized.
[BOOT][RUNTIME_READY] PASS cpus={cpus}/{cpus} scheduler=1 dpc=1 shell=1 input=1 modal=1 pci=1
[BOOT][TEST_READY] PASS autorun=0 selftests=0
[KERNEL] Entering Main Loop.
[IRQ][CHECK] PASS clocksource=HPET clockevent=BSP_LAPIC period_us=1000 hpet_timer0=QUIESCENT non_bsp_ticks=0 stray_hpet=0 cpus=24/24 controllers=1 routes=1 unexpected=0 imbalance=0 snapshot_polls=24 validation_polls=1 elapsed_ms=8 reason=ok
[IRQ][BOOT_RESULT] PASS snapshots=24 polls=24 elapsed_ms=20 acquisition_ms=5 publication_ms=15 budget_ms=5000
[MEM][SUMMARY] PASS pmm_total=19327352832 pmm_free=16770453504 pmm_used=2556899328 heap_total=33554432 heap_used=3886528 heap_free=29667904 heap_blocks=331 heap_free_blocks=4 heap_largest_free=29656048 pmm_integrity=1 heap_integrity=1
[CPU][SUMMARY] PASS vendor=GenuineIntel family=6 model=151 stepping=2 max_basic=32 max_ext=2147483656 logical_cpus=24 long_mode=1 htt=1 hybrid=1 core_type=64
"""
    if profile == "selftest":
        common += """[LOCKTEST][PROGRESS] PASS cpus=24 iterations=2000 acquisitions=48000 exclusive_violations=0 if_violations=0 max_queue_distance=23 limit=23 slots=24 slot_mask=16777215 observer_samples=48000 contended=40000 trylock_no_ticket=1 static_zero=1 reaped=24 max_active=24
[ALLOCTEST][HARDENING] PASS heap_overflow=2 heap_interior=1 heap_foreign=1 heap_double=2 pmm_null=1 pmm_unaligned=1 pmm_outside=1 pmm_reserved=1 pmm_double=1 bitmap_words=3 pmm_word_calls=1 pmm_word_iterations=95 heap_integrity=1 pmm_integrity=1 census=1 heap_ok=1 pmm_ok=1 bitmap_ok=1
"""
    return common


def verify_fixtures() -> int:
    production = fixture_log("production")
    selftest = fixture_log("selftest")
    verify_log_text(production, "production", 24, 2000, True)
    verify_log_text(selftest, "selftest", 24, 2000, True)
    negatives = (
        ("cpu-ready", selftest.replace(
            "[IRQ][CPU_READY] PASS slot=23", "[IRQ][CPU_READY] LOST slot=23", 1)),
        ("memory-arithmetic", selftest.replace(
            "pmm_used=2556899328", "pmm_used=1", 1)),
        ("irq-status", selftest.replace(
            "[IRQ][CHECK] PASS", "[IRQ][CHECK] FAIL", 1)),
        ("queue-distance", selftest.replace(
            "max_queue_distance=23", "max_queue_distance=24", 1)),
        ("allocator-integrity", selftest.replace(
            "heap_integrity=1 pmm_integrity=1 census=1",
            "heap_integrity=0 pmm_integrity=1 census=1", 1)),
        ("fault", selftest + "PANIC synthetic\n"),
    )
    for name, value in negatives:
        try:
            verify_log_text(value, "selftest", 24, 2000, True)
        except EvidenceError:
            continue
        raise EvidenceError("NEGATIVE_ACCEPTED", name)
    try:
        verify_log_text(selftest, "production", 24, 2000, True)
    except EvidenceError:
        pass
    else:
        raise EvidenceError("NEGATIVE_ACCEPTED", "selftest-in-production")
    return len(negatives) + 1


def main() -> int:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    log_parser = subparsers.add_parser("log")
    log_parser.add_argument("profile", choices=("production", "selftest"))
    log_parser.add_argument("serial", type=pathlib.Path)
    log_parser.add_argument("--cpus", type=int, default=24)
    log_parser.add_argument("--iterations", type=int, default=2000)
    log_parser.add_argument("--environment", choices=("physical", "qemu"),
                            default="physical")
    candidate_parser = subparsers.add_parser("candidate")
    candidate_parser.add_argument("directory", type=pathlib.Path)
    subparsers.add_parser("fixtures")
    args = parser.parse_args()
    try:
        if args.command == "log":
            require(args.serial.is_file(), "SERIAL_MISSING", str(args.serial))
            raw = args.serial.read_text(encoding="utf-8", errors="replace")
            result = verify_log_text(
                raw, args.profile, args.cpus, args.iterations,
                args.environment == "physical")
            print("[BARE_METAL][VERIFY] PASS "
                  f"profile={result['profile']} cpus={result['cpus']} "
                  f"pmm_total={result['pmm_total']} "
                  f"serial_sha256={sha256(args.serial)}")
        elif args.command == "candidate":
            metadata = verify_candidate(args.directory)
            print("[BARE_METAL][CANDIDATE] PASS "
                  f"source_commit={metadata.get('SOURCE_COMMIT', '')} "
                  "profiles=2 payloads=10")
        else:
            negatives = verify_fixtures()
            print(f"[BARE_METAL][FIXTURES] PASS negatives={negatives}")
    except (EvidenceError, OSError, UnicodeError) as error:
        code = error.code if isinstance(error, EvidenceError) else "IO_ERROR"
        detail = error.detail if isinstance(error, EvidenceError) else str(error)
        print(f"[BARE_METAL][VERIFY] FAIL code={code} detail={detail}",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
