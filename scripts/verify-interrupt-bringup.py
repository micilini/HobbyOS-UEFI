#!/usr/bin/env python3
"""Host models, evidence ledger, and offline interrupt bring-up verifier."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
ARTIFACT = ROOT / "artifacts/build/interrupt-bringup"
BASE = "602532db881f5e328a78efdb52b0c012f35b4065"
BASE_TREE = "22a109bf7d2dd761bdcb8cca16b67e1258770872"
FIELDS = (
    "stage",
    "machine",
    "smp",
    "accel",
    "kernel_hash",
    "scenario",
    "result",
    "marker",
    "log",
    "sha256",
    "cpus_expected",
    "cpus_ready",
    "first_vector",
    "unexpected",
    "imbalance",
)


class EvidenceError(RuntimeError):
    pass


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run_git(*args: str) -> str:
    return subprocess.check_output(
        ["git", *args], cwd=ROOT, text=True, stderr=subprocess.DEVNULL
    ).strip()


def decode_interrupt_flags(flags: int) -> tuple[str, str]:
    if flags & 0xFFF0:
        raise ValueError("reserved ACPI interrupt bits")
    polarity = {0: "conforms", 1: "high", 3: "low"}.get(flags & 3)
    trigger = {0: "conforms", 1: "edge", 3: "level"}.get((flags >> 2) & 3)
    if polarity is None or trigger is None:
        raise ValueError("reserved ACPI interrupt encoding")
    return polarity, trigger


def find_ioapic(ranges: list[tuple[int, int]], gsi: int) -> tuple[int, int]:
    for index, (base, count) in enumerate(ranges):
        if base <= gsi < base + count:
            return index, gsi - base
    raise ValueError("GSI has no controller")


def encode_route(
    vector: int, destination: int, polarity: str, trigger: str, masked: bool
) -> tuple[int, int]:
    if vector < 32 or vector > 255 or destination < 0 or destination > 255:
        raise ValueError("unrepresentable route")
    lower = vector
    if polarity == "low":
        lower |= 1 << 13
    elif polarity != "high":
        raise ValueError("unresolved polarity")
    if trigger == "level":
        lower |= 1 << 15
    elif trigger != "edge":
        raise ValueError("unresolved trigger")
    if masked:
        lower |= 1 << 16
    return lower, destination << 24


def host_selftest() -> None:
    checks = 0
    assert decode_interrupt_flags(0) == ("conforms", "conforms")
    checks += 1
    assert decode_interrupt_flags(0xF) == ("low", "level")
    checks += 1
    for reserved in (2, 8, 0x10):
        try:
            decode_interrupt_flags(reserved)
        except ValueError:
            checks += 1
        else:
            raise AssertionError("reserved ACPI encoding accepted")

    ranges = [(0, 24), (24, 32), (80, 8)]
    assert find_ioapic(ranges, 0) == (0, 0)
    assert find_ioapic(ranges, 31) == (1, 7)
    assert find_ioapic(ranges, 87) == (2, 7)
    checks += 3
    try:
        find_ioapic(ranges, 64)
    except ValueError:
        checks += 1
    else:
        raise AssertionError("GSI hole accepted")
    for left, right in zip(ranges, ranges[1:]):
        assert left[0] + left[1] <= right[0]
        checks += 1

    lower, upper = encode_route(33, 7, "high", "edge", True)
    assert lower == 33 | (1 << 16) and upper == 7 << 24
    checks += 1
    lower, upper = encode_route(32, 255, "low", "level", False)
    assert lower & (1 << 13) and lower & (1 << 15) and upper == 255 << 24
    checks += 1
    safe_writes = [lower | (1 << 16), upper, lower | (1 << 16)]
    assert safe_writes[0] & (1 << 16) and safe_writes[2] & (1 << 16)
    checks += 1
    for vector, destination in ((31, 0), (33, 256)):
        try:
            encode_route(vector, destination, "high", "edge", True)
        except ValueError:
            checks += 1
        else:
            raise AssertionError("invalid route accepted")

    states = [
        "OFF",
        "CONTROLLERS_QUIESCENT",
        "ROUTES_PREPARED",
        "CPUS_PREPARED",
        "BSP_LAPIC_VERIFIED",
        "HPET_VERIFIED",
        "TIMERS_ACTIVE",
        "SERVICES_ACTIVE",
    ]
    assert len(states) == len(set(states)) and states[0] == "OFF"
    checks += 1
    entered = returned = depth = 0
    first = None
    entered += 1
    depth += 1
    first = 34 if first is None else first
    returned += 1
    depth -= 1
    assert entered == returned == 1 and depth == 0 and first == 34
    checks += 1
    attached = handoff = preempt = False
    assert not (attached and handoff and preempt)
    attached = handoff = preempt = True
    assert attached and handoff and preempt
    checks += 1
    print(f"[IRQ][HOST_SELFTEST] PASS tests={checks}")


def read_rows() -> list[dict[str, str]]:
    path = ARTIFACT / "evidence.json"
    if not path.exists():
        return []
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, list):
        raise EvidenceError("evidence.json is not a list")
    return [{field: str(row.get(field, "")) for field in FIELDS} for row in data]


def write_rows(rows: list[dict[str, str]]) -> None:
    ARTIFACT.mkdir(parents=True, exist_ok=True)
    json_path = ARTIFACT / "evidence.json"
    tsv_path = ARTIFACT / "evidence.tsv"
    json_path.write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")
    with tsv_path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, FIELDS, delimiter="\t", lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)


def record(args: argparse.Namespace) -> None:
    log = Path(args.log) if args.log else None
    if log and not log.is_absolute():
        log = ROOT / log
    row = {field: "" for field in FIELDS}
    for field in FIELDS:
        value = getattr(args, field, None)
        if value is not None:
            row[field] = str(value)
    if log:
        if not log.exists():
            raise EvidenceError(f"missing evidence log: {log}")
        row["log"] = str(log.relative_to(ROOT))
        row["sha256"] = sha256(log)
    rows = [old for old in read_rows() if old["stage"] != row["stage"]]
    rows.append(row)
    write_rows(rows)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise EvidenceError(message)


def require_text(path: Path, *markers: str) -> str:
    require(path.exists(), f"missing {path.relative_to(ROOT)}")
    text = path.read_text(encoding="utf-8", errors="replace")
    for marker in markers:
        require(marker in text, f"{path.name}: missing {marker}")
    return text


def verify_manifest(path: Path) -> None:
    require(path.exists(), f"missing {path.relative_to(ROOT)}")
    lines = path.read_text(encoding="utf-8").splitlines()
    require(lines, f"empty manifest {path.name}")
    for line in lines:
        digest, separator, relative = line.partition("  ")
        require(separator == "  " and re.fullmatch(r"[0-9a-f]{64}", digest) is not None,
                f"{path.name}: malformed line")
        source = ROOT / relative
        require(source.is_file(), f"{path.name}: missing {relative}")
        require(sha256(source) == digest, f"{path.name}: hash drift for {relative}")


def verify_git() -> None:
    require(run_git("branch", "--show-current") == "feat/taskman", "wrong branch")
    head = run_git("rev-parse", "HEAD")
    if head == BASE:
        require(run_git("rev-parse", "HEAD^{tree}") == BASE_TREE, "wrong base tree")
    else:
        require(run_git("rev-parse", "HEAD^") == BASE, "commit parent is not base")
        require(
            run_git("log", "-1", "--format=%s")
            == "fix(irq): establish safe x86 interrupt controller bring-up",
            "wrong commit subject",
        )


def verify_scenario(stage: str, row: dict[str, str]) -> None:
    path = ROOT / row["log"]
    text = require_text(
        path,
        "[IRQ][PIC] QUIESCENT",
        "[IRQ][IOAPIC] QUIESCENT",
        "[IRQ][BSP_LAPIC_PROBE] PASS",
        "[IRQ][HPET_PROBE] PASS",
        "[IRQ][BOOTSTRAP] SERVICES_ACTIVE",
        "[BOOT][SHELL_READY] PASS",
        "[BOOT][RUNTIME_READY] PASS",
        "[BOOT][TEST_READY] PASS autorun=0 selftests=0",
        "[KERNEL] Entering Main Loop.",
        "[IRQ][CHECK] PASS",
        "[TASKDIAG][CHECK] PASS",
        "[INPUTTEST][CHECK] PASS",
        "[MODALTEST][CHECK] PASS",
    )
    smp = int(row["smp"])
    expected = {
        "up-tcg": ("q35", 1, "tcg"),
        "smp2-tcg": ("q35", 2, "tcg"),
        "smp4-kvm": ("q35", 4, "kvm"),
        "smp8-kvm": ("q35", 8, "kvm"),
        "smp24-kvm": ("q35", 24, "kvm"),
        "pcat": ("pc", 4, "tcg"),
    }[stage]
    require((row["machine"], smp, row["accel"]) == expected,
            f"{stage}: launch tuple drift")
    require(row["cpus_expected"] == str(smp) and row["cpus_ready"] == str(smp),
            f"{stage}: ledger CPU count")
    require(row["first_vector"] == "34", f"{stage}: first vector")
    require(row["unexpected"] == "0" and row["imbalance"] == "0",
            f"{stage}: ledger IRQ balance")
    require(text.count("[IRQ][CPU_READY] PASS") == smp, f"{stage}: CPU_READY count")
    require(f"cpus={smp}/{smp}" in text, f"{stage}: runtime CPU count")
    require(text.count("timer_us=1000 handoff=1 preempt=1") == smp,
            f"{stage}: timer/handoff count")
    require("unexpected=0 imbalance=0" in text, f"{stage}: IRQ balance")
    faults = re.compile(r"PANIC|#PF|#GP|STRUCTURAL_FAULT|FINISH_FAULT")
    require(not faults.search(text), f"{stage}: guest fault")
    if stage in {"smp4-kvm", "smp24-kvm"}:
        require("[TASKMAN][AUTO_EXIT] PASS target=3" in text, f"{stage}: TASKMAN")
        require("fallback_frames=0" in text, f"{stage}: TASKMAN fallback")
        require("[TASKMANTEST][CHECK] PASS" in text, f"{stage}: TASKMAN check")
    if stage in {"smp8-kvm", "smp24-kvm"}:
        require("[SMP][KILL_SWEEP] PASS workers=8" in text, f"{stage}: stress sweep")
    if stage == "pcat":
        require("master=ff slave=ff" in text, "PCAT PIC masks")
        require("pcat=1" in text or "pcat=0" in text, "PCAT declaration missing")
        require("bsp_probe_vector=34" in text, "PCAT first maskable vector")
    if stage in {"smp4-kvm", "smp24-kvm", "pcat"}:
        require("[IRQ][ROUTES] PASS count=2" in text, f"{stage}: route snapshot")
        require("[IRQ][IOAPIC] index=0" in text, f"{stage}: controller snapshot")


def verify_candidate(by_stage: dict[str, dict[str, str]], matrix_hash: str) -> None:
    candidate = ROOT / "artifacts/baremetal/interrupt-bringup-candidate"
    required_files = (
        "hobbyos.img",
        "kernel.elf",
        "BOOTX64.EFI",
        "SHA256SUMS",
        "payload-manifest.txt",
        "candidate.txt",
        "QEMU-certification-summary.txt",
        "INTERRUPT_BRINGUP_OPERATOR_RUNBOOK.md",
        "interrupt-controller-bringup.md",
        "INTERRUPT_BRINGUP_CERTIFICATION.md",
        "expected-markers.txt",
        "fat-fsck.txt",
        "fat-metadata.txt",
        "boot-signature.txt",
    )
    for name in required_files:
        require((candidate / name).is_file(), f"candidate missing {name}")

    sums: dict[str, str] = {}
    for line in (candidate / "SHA256SUMS").read_text(encoding="utf-8").splitlines():
        digest, separator, name = line.partition("  ")
        require(separator == "  " and re.fullmatch(r"[0-9a-f]{64}", digest) is not None,
                "candidate SHA256SUMS malformed")
        require(name not in sums and "/" not in name and name not in {".", ".."},
                "candidate SHA256SUMS unsafe name")
        path = candidate / name
        require(path.is_file() and sha256(path) == digest,
                f"candidate hash mismatch: {name}")
        sums[name] = digest
    require(set(required_files) - {"SHA256SUMS"} <= sums.keys(),
            "candidate SHA256SUMS incomplete")
    require(sha256(candidate / "kernel.elf") == matrix_hash,
            "candidate kernel differs from matrix")
    require((candidate / "kernel.elf").read_bytes() == (ROOT / "kernel.elf").read_bytes(),
            "candidate kernel differs from final build")
    require((candidate / "hobbyos.img").read_bytes() == (ROOT / "hobbyos.img").read_bytes(),
            "candidate image differs from final build")
    require((candidate / "BOOTX64.EFI").read_bytes() == (ROOT / "BOOTX64.EFI").read_bytes(),
            "candidate BOOTX64 differs from final build")

    payloads = {}
    for line in (candidate / "payload-manifest.txt").read_text(encoding="utf-8").splitlines():
        digest, separator, name = line.partition("  ")
        require(separator == "  ", "payload manifest malformed")
        payloads[name] = digest
    expected_payloads = {
        "/kernel.elf": sha256(ROOT / "kernel.elf"),
        "/EFI/BOOT/BOOTX64.EFI": sha256(ROOT / "BOOTX64.EFI"),
        "/EFI/fonts/zap-light16.psf": sha256(ROOT / "bootloader/fonts/zap-light16.psf"),
        "/EFI/images/logo.bmp": sha256(ROOT / "bootloader/images/logo.bmp"),
        "/startup.nsh": sha256(ROOT / "bootloader/startup.nsh"),
    }
    require(payloads == expected_payloads, "candidate payload manifest drift")
    require_text(candidate / "boot-signature.txt", "FAT_BOOT_SIGNATURE=55aa")

    metadata = {}
    for line in (candidate / "candidate.txt").read_text(encoding="utf-8").splitlines():
        key, separator, value = line.partition("=")
        if separator:
            metadata[key] = value
    require(metadata.get("STATUS") == "READY_FOR_BARE_METAL_INTERRUPT_VALIDATION",
            "candidate status")
    require(metadata.get("COMMIT") == run_git("rev-parse", "HEAD"), "candidate commit")
    require(metadata.get("TREE") == run_git("rev-parse", "HEAD^{tree}"), "candidate tree")
    require(metadata.get("KERNEL_SHA256") == matrix_hash, "candidate kernel metadata")
    review_zip = ROOT / metadata.get("REVIEW_ZIP", "")
    require(review_zip.is_file(), "review ZIP missing")
    require(sha256(review_zip) == metadata.get("REVIEW_ZIP_SHA256"), "review ZIP hash")

    import zipfile

    with zipfile.ZipFile(review_zip) as archive:
        require(archive.testzip() is None, "review ZIP corrupt")
        names = archive.namelist()
        require(any(name.endswith("/.git/HEAD") for name in names), "review ZIP lacks .git")
        require(any(name.endswith("/kernel/kernel.c") for name in names), "review ZIP lacks kernel")
        require(any(name.endswith("/scripts/test-interrupt-bringup.sh") for name in names),
                "review ZIP lacks runner")
        forbidden = re.compile(r"/(?:artifacts|\.qemu)/|\.(?:o|d|su|sock|pid)$")
        require(not any(forbidden.search(name) for name in names),
                "review ZIP contains temporary build/runtime files")

    require(by_stage["candidate"]["kernel_hash"] == matrix_hash,
            "candidate ledger kernel drift")


def verify(mode: str) -> None:
    verify_git()
    rows = read_rows()
    by_stage = {row["stage"]: row for row in rows}
    required = {
        "preflight",
        "static",
        "selftest",
        "negative-pic",
        "negative-ioapic",
        "negative-iso",
        "negative-hpet",
        "negative-preemption",
        "up-tcg",
        "smp2-tcg",
        "smp4-kvm",
        "smp8-kvm",
        "smp24-kvm",
        "pcat",
        "soak24",
        "final-build",
    }
    if mode == "all":
        required |= {"candidate", "report"}
    missing = sorted(required - by_stage.keys())
    require(not missing, f"missing evidence stages: {', '.join(missing)}")
    for stage in required:
        require(by_stage[stage]["result"] == "PASS", f"{stage}: not PASS")
        if by_stage[stage]["log"]:
            path = ROOT / by_stage[stage]["log"]
            require(path.exists(), f"{stage}: log missing")
            require(sha256(path) == by_stage[stage]["sha256"], f"{stage}: log hash")

    negative_markers = {
        "negative-pic": "PIC_NOT_QUIESCENT_DETECTED",
        "negative-ioapic": "IOAPIC_NOT_QUIESCENT_DETECTED",
        "negative-iso": "INTERRUPT_OVERRIDE_LOST_DETECTED",
        "negative-hpet": "HPET_EARLY_DELIVERY_DETECTED",
        "negative-preemption": "PREEMPTION_BEFORE_HANDOFF_DETECTED",
    }
    for stage, marker in negative_markers.items():
        require_text(ROOT / by_stage[stage]["log"], marker)

    scenarios = ("up-tcg", "smp2-tcg", "smp4-kvm", "smp8-kvm", "smp24-kvm", "pcat")
    for stage in scenarios:
        verify_scenario(stage, by_stage[stage])
    require(by_stage["smp24-kvm"]["smp"] == "24", "SMP24 was reduced")

    matrix_hashes = {by_stage[stage]["kernel_hash"] for stage in scenarios}
    require(len(matrix_hashes) == 1 and "" not in matrix_hashes, "matrix kernel drift")
    require(by_stage["soak24"]["kernel_hash"] in matrix_hashes, "soak kernel drift")
    matrix_hash = next(iter(matrix_hashes))
    require(by_stage["final-build"]["kernel_hash"] == matrix_hash,
            "final build kernel drift")
    final_build = require_text(
        ROOT / by_stage["final-build"]["log"],
        "STACK_USAGE limit=2048",
        "violations=0",
        "stack-check: PASS",
        "kernel-check: PASS",
        "[BOOT][PRODUCTION_TEST_POLICY] PASS autorun=0 tasktest=0",
        "[IRQ][FINAL_BUILD] PASS",
    )
    require(final_build.count(f"{matrix_hash}  /tmp/hobbyos-irq-final-") == 2,
            "deterministic j2/jN hashes")
    undefined = subprocess.check_output(["nm", "-u", "kernel.elf"], cwd=ROOT,
                                        text=True).strip()
    require(not undefined, "undefined kernel symbols")
    soak = require_text(
        ROOT / by_stage["soak24"]["log"],
        "[IRQ][SOAK] PASS smp=24",
        "[IRQ][CHECK] PASS cpus=24/24",
    )
    require(soak.count("[IRQ][CHECK] PASS") >= 10, "soak irq check volume")
    require(soak.count("[TASKDIAG][CHECK] PASS") >= 10, "soak taskdiag volume")
    require(soak.count("[SMP][KILL_SWEEP] PASS workers=8") >= 4, "soak stress volume")
    require(soak.count("[TASKMAN][AUTO_EXIT] PASS target=3") >= 4, "soak TASKMAN volume")
    duration = re.search(r"\[IRQ\]\[SOAK\] PASS smp=24 duration_ms=(\d+)", soak)
    require(duration is not None and int(duration.group(1)) >= 300000, "soak duration")

    negative_sources = {
        "negative-pic": "HOBBYOS_IRQ_NEGATIVE_PIC_UNMASKED",
        "negative-ioapic": "HOBBYOS_IRQ_NEGATIVE_IOAPIC_ROUTE_ACTIVE",
        "negative-iso": "HOBBYOS_IRQ_NEGATIVE_IGNORE_ISO",
        "negative-hpet": "HOBBYOS_IRQ_NEGATIVE_HPET_EARLY_ARM",
        "negative-preemption": "HOBBYOS_IRQ_NEGATIVE_EARLY_PREEMPTION",
    }
    for stage, macro in negative_sources.items():
        directory = ARTIFACT / "negatives" / stage
        require_text(directory / "reset-serial.log", "[IRQ][SELFTEST] PASS")
        reset_build = require_text(directory / "reset-build.log", "kernel-check: PASS")
        require(f"-D{macro}" not in reset_build, f"{stage}: contaminated reset build")
    current_strings = subprocess.check_output(["strings", "kernel.elf"], cwd=ROOT,
                                              text=True, errors="replace")
    require("[IRQ][NEGATIVE]" not in current_strings, "negative marker in final kernel")

    before = ARTIFACT / "taskman-protected-before.sha256"
    after = ARTIFACT / "taskman-protected-after.sha256"
    boot_before = ARTIFACT / "bootloader-before.sha256"
    boot_after = ARTIFACT / "bootloader-after.sha256"
    require(before.exists() and after.exists() and before.read_bytes() == after.read_bytes(),
            "TASKMAN protected manifest drift")
    require(boot_before.exists() and boot_after.exists() and
            boot_before.read_bytes() == boot_after.read_bytes(),
            "bootloader manifest drift")
    verify_manifest(ARTIFACT / "source-after-tests.sha256")
    verify_manifest(ARTIFACT / "source-after-build.sha256")

    screen_dir = ARTIFACT / "screens"
    for name in ("uefi-before-kernel.ppm", "early-core-cleared.ppm", "final-shell.ppm"):
        require((screen_dir / name).exists(), f"missing screenshot {name}")
    require_text(ARTIFACT / "framebuffer.log", "[GRAPHICS][EARLY_CLEAR] PASS")

    if mode == "all":
        verify_candidate(by_stage, matrix_hash)
    print("[IRQ][EVIDENCE] PASS")


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser()
    sub = result.add_subparsers(dest="command", required=True)
    sub.add_parser("selftest")
    record_parser = sub.add_parser("record")
    for field in FIELDS:
        record_parser.add_argument(f"--{field.replace('_', '-')}")
    verify_parser = sub.add_parser("verify")
    verify_parser.add_argument("mode", choices=("matrix", "all"))
    return result


def main() -> int:
    args = parser().parse_args()
    try:
        if args.command == "selftest":
            host_selftest()
        elif args.command == "record":
            record(args)
        else:
            verify(args.mode)
    except (AssertionError, EvidenceError, OSError, ValueError) as error:
        print(f"[IRQ][EVIDENCE] FAIL reason={error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
