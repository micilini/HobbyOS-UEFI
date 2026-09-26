#!/usr/bin/env python3
"""Closed-by-default verifier for bounded formatting evidence."""

import argparse
import copy
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import zlib


HOST_CASES = (
    "capacity", "truncation", "integer-extremes", "conversions",
    "padding", "pointer", "va-list", "errors-and-budget",
    "deterministic-matrix", "legacy",
)
GUEST_CASES = (
    "capacity-zero", "capacity-one", "exact-boundaries", "truncation",
    "int-extremes", "long-long-extremes", "unsigned-extremes",
    "conversions", "embedded-zero", "padding", "pointer", "null-string",
    "va-list-preserved", "invalid-format", "width-bounds",
    "required-overflow", "memory-guards", "legacy-itoa", "legacy-hex",
    "breadcrumb-consumer", "pmem-consumer", "breadcrumb-fallback",
)
HOST_PROFILES = ("normal", "ubsan", "asan")
QEMU_PROFILES = {
    "q35-tcg-smp1": ("q35", "tcg", 1),
    "q35-kvm-smp4": ("q35", "kvm", 4),
}
SOURCE_FILES = (
    "kernel/src/libc/string.c",
    "kernel/src/libc/string.h",
    "kernel/src/libc/format_selftest.c",
    "kernel/src/libc/format_selftest.h",
    "kernel/src/core/kernel_init.c",
    "kernel/src/memory/pmem.c",
    "kernel/src/memory/heap.c",
    "kernel/src/memory/gdt.c",
    "kernel/src/memory/paging.c",
    "kernel/src/graphics/console.c",
    "kernel/src/graphics/terminal.c",
    "makefile",
    "README.md",
    "scripts/libc-format-host.c",
    "scripts/test-libc-format.sh",
    "scripts/verify-libc-format-evidence.py",
)
REMOVED_FILES = (
    "kernel/src/utils/utils.c",
    "kernel/src/utils/utils.h",
)
PAYLOADS = (
    "BOOTX64.EFI", "kernel.elf", "zap-light16.psf", "logo.bmp",
    "startup.nsh",
)


class EvidenceError(Exception):
    pass


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_json(path, label):
    if not path.is_file():
        raise EvidenceError(f"{label} is missing")
    try:
        value = json.loads(path.read_text(errors="strict"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise EvidenceError(f"{label} is malformed: {error}") from None
    if not isinstance(value, dict):
        raise EvidenceError(f"{label} is not an object")
    return value


def safe_relative(value, label):
    if not isinstance(value, str) or not value:
        raise EvidenceError(f"{label} path is invalid")
    path = Path(value)
    if path.is_absolute() or ".." in path.parts:
        raise EvidenceError(f"{label} path escapes the evidence root")
    return path


def repository_root():
    return Path(__file__).resolve().parent.parent


def collect_evidence(root):
    repo = repository_root()
    root.mkdir(parents=True, exist_ok=True)
    source_root = root / "source"
    source_rows = []
    for relative_text in SOURCE_FILES:
        relative = Path(relative_text)
        source = repo / relative
        if not source.is_file():
            raise EvidenceError(f"source required for collection is missing: {relative}")
        destination = source_root / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
        source_rows.append({
            "path": relative.as_posix(),
            "evidence_copy": (Path("source") / relative).as_posix(),
            "bytes": source.stat().st_size,
            "sha256": sha256(source),
        })
    source_manifest = {
        "schema": 1,
        "files": source_rows,
        "removed_files": list(REMOVED_FILES),
    }
    (root / "source-manifest.json").write_text(
        json.dumps(source_manifest, indent=2, sort_keys=True) + "\n")

    candidate = root / "candidate"
    candidate_files = ("kernel.elf", "hobbyos.img", "BOOTX64.EFI")
    candidate_rows = {}
    for name in candidate_files:
        path = candidate / name
        if not path.is_file() or path.stat().st_size == 0:
            raise EvidenceError(f"candidate artifact is missing: {name}")
        candidate_rows[name] = {
            "path": (Path("candidate") / name).as_posix(),
            "bytes": path.stat().st_size,
            "sha256": sha256(path),
        }
    payload_rows = {}
    for name in PAYLOADS:
        path = candidate / "payloads" / name
        if not path.is_file() or path.stat().st_size == 0:
            raise EvidenceError(f"candidate payload is missing: {name}")
        payload_rows[name] = {
            "path": (Path("candidate/payloads") / name).as_posix(),
            "bytes": path.stat().st_size,
            "sha256": sha256(path),
        }
    if payload_rows["kernel.elf"]["sha256"] != candidate_rows["kernel.elf"]["sha256"]:
        raise EvidenceError("kernel payload differs from the candidate ELF")
    candidate_manifest = {
        "schema": 1,
        "files": candidate_rows,
        "payloads": payload_rows,
    }
    (candidate / "manifest.json").write_text(
        json.dumps(candidate_manifest, indent=2, sort_keys=True) + "\n")

    host_rows = []
    for name in HOST_PROFILES:
        run = root / "host" / name
        command = read_json(run / "command.json", f"host {name} command")
        binary = run / "libc-format-host"
        if not binary.is_file() or binary.stat().st_size == 0:
            raise EvidenceError(f"host {name} executable is missing")
        host_rows.append({
            "name": name,
            "directory": (Path("host") / name).as_posix(),
            "binary_sha256": sha256(binary),
            "implementation_sha256": source_rows[0]["sha256"],
            "sanitizer": command.get("sanitizer"),
        })

    qemu_rows = []
    for name in QEMU_PROFILES:
        profile = read_json(root / "qemu" / name / "profile.json",
                            f"QEMU {name} profile")
        qemu_rows.append(profile)

    try:
        head = subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip()
        diff = subprocess.check_output(
            ["git", "diff", "--binary", "--", *SOURCE_FILES],
            cwd=repo)
        diff_sha = hashlib.sha256(diff).hexdigest()
    except (OSError, subprocess.CalledProcessError) as error:
        raise EvidenceError(f"Git identity collection failed: {error}") from None

    result = {
        "schema": 1,
        "kind": "bounded-format-evidence",
        "adversarial": False,
        "collection_head": head,
        "source_diff_sha256": diff_sha,
        "source_manifest": "source-manifest.json",
        "candidate_manifest": "candidate/manifest.json",
        "candidate": candidate_rows,
        "consumer_result": ("consumers/result.json"
                            if (root / "consumers/result.json").is_file()
                            else None),
        "host_runs": host_rows,
        "qemu_profiles": qemu_rows,
        "configuration": {
            "SELFTEST": 1,
            "SELFTEST_AUTORUN": 0,
            "FORMAT_TEST": 1,
            "HOBBYOS_FORMAT_TEST": 1,
        },
        "matrix": {
            "host_seed": "0x5eedf04a7c9b312d",
            "host_iterations": 4096,
            "host_cases": list(HOST_CASES),
            "guest_cases": list(GUEST_CASES),
        },
    }
    (root / "result.json").write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n")
    return result


def verify_source(root, result, errors):
    repo = repository_root()
    manifest_path = root / safe_relative(result.get("source_manifest"),
                                         "source manifest")
    try:
        manifest = read_json(manifest_path, "source manifest")
    except EvidenceError as error:
        errors.append(str(error))
        return
    rows = manifest.get("files")
    if not isinstance(rows, list):
        errors.append("source manifest file list is invalid")
        return
    by_path = {}
    for row in rows:
        if not isinstance(row, dict):
            errors.append("source manifest row is invalid")
            continue
        path_text = row.get("path")
        if path_text in by_path:
            errors.append("source manifest contains duplicate paths")
            continue
        by_path[path_text] = row
        try:
            evidence_path = root / safe_relative(
                row.get("evidence_copy"), "source evidence copy")
        except EvidenceError as error:
            errors.append(str(error))
            continue
        if (not evidence_path.is_file() or
                evidence_path.stat().st_size != row.get("bytes") or
                sha256(evidence_path) != row.get("sha256")):
            errors.append(f"source evidence copy differs: {path_text}")
            continue
        checkout = repo / str(path_text)
        if not checkout.is_file() or sha256(checkout) != row.get("sha256"):
            errors.append(f"evidence belongs to another source: {path_text}")
    if set(by_path) != set(SOURCE_FILES):
        errors.append("source manifest coverage differs from the required set")
    if manifest.get("removed_files") != list(REMOVED_FILES):
        errors.append("removed utility inventory differs")
    for relative in REMOVED_FILES:
        if (repo / relative).exists():
            errors.append(f"dead utility remains in the checkout: {relative}")

    def text(relative):
        path = repo / relative
        try:
            return path.read_text(errors="strict")
        except (OSError, UnicodeError):
            errors.append(f"source cannot be read: {relative}")
            return ""

    string_c = text("kernel/src/libc/string.c")
    string_h = text("kernel/src/libc/string.h")
    kernel_init = text("kernel/src/core/kernel_init.c")
    pmem = text("kernel/src/memory/pmem.c")
    heap = text("kernel/src/memory/heap.c")
    gdt = text("kernel/src/memory/gdt.c")
    paging = text("kernel/src/memory/paging.c")
    makefile = text("makefile")
    readme = text("README.md")
    if "int kvsnprintf(char *dst, size_t size" not in string_h or \
            "int ksnprintf(char *dst, size_t size" not in string_h:
        errors.append("bounded format declarations are absent")
    for required in ("va_copy", "va_end", "INT_MAX", "format_writer"):
        if required not in string_c:
            errors.append(f"formatter implementation lacks {required}")
    if re.search(r"\bn\s*=\s*-n\b", string_c):
        errors.append("legacy itoa still negates a signed minimum")
    forbidden = ("spin_lock", "kmalloc", "kfree", "serial_write",
                 "console_", "clock_", "scheduler_", "kpanic")
    for token in forbidden:
        if token in string_c:
            errors.append(f"formatter gained forbidden dependency: {token}")
    if "kinit_progress_append_" in kernel_init or "ksnprintf(" not in kernel_init:
        errors.append("boot progress consumer migration is incomplete")
    if kernel_init.count("serial_write_all(line);") != 1:
        errors.append("boot progress no longer emits one complete line")
    if "pmem_print_hex" in pmem or "ksnprintf(" not in pmem:
        errors.append("PMM diagnostic migration is incomplete")
    if "heap_print_hex" in heap or "heap_print_dec" in heap:
        errors.append("dead heap diagnostic helpers remain")
    if "static void serial_print_hex" in gdt:
        errors.append("dead GDT diagnostic helper remains")
    if "ksnprintf(" in paging:
        errors.append("constant-only paging diagnostics were needlessly migrated")
    if "$(KERNEL_DIR)/src/utils/utils.c" in makefile:
        errors.append("dead kernel utility remains in the build")
    if ("FORMAT_TEST=1 requires SELFTEST=1" not in makefile or
            "HOBBYOS_FORMAT_TEST=1" not in makefile):
        errors.append("conditional format selftest build contract is absent")
    for relative in ("kernel/src/graphics/console.c",
                     "kernel/src/graphics/terminal.c"):
        if "../utils/utils.h" in text(relative):
            errors.append(f"dead utility include remains: {relative}")
    for source in repo.glob("kernel/**/*"):
        if source.is_file() and source.suffix in (".c", ".h", ".S"):
            content = source.read_text(errors="replace")
            if re.search(r"\b(k_memset|k_memcpy|k_delay)\s*\(", content):
                errors.append(f"dead utility call remains: {source.relative_to(repo)}")
    if "strncpy" in readme or "`memcmp`) and `memory.h`" in readme:
        errors.append("README LibC catalog remains inaccurate")
    if "`ksnprintf`, `kvsnprintf`" not in readme or "`memcmp`" not in readme:
        errors.append("README does not describe the bounded formatter catalog")


def verify_candidate(root, result, errors):
    try:
        manifest_path = root / safe_relative(result.get("candidate_manifest"),
                                             "candidate manifest")
        manifest = read_json(manifest_path, "candidate manifest")
    except EvidenceError as error:
        errors.append(str(error))
        return None
    files = manifest.get("files")
    payloads = manifest.get("payloads")
    if not isinstance(files, dict) or not isinstance(payloads, dict):
        errors.append("candidate manifest entries are invalid")
        return None
    if set(files) != {"kernel.elf", "hobbyos.img", "BOOTX64.EFI"}:
        errors.append("candidate manifest file coverage differs")
    if set(payloads) != set(PAYLOADS):
        errors.append("candidate payload coverage differs")
    for group_name, rows in (("candidate", files), ("payload", payloads)):
        for name, row in rows.items():
            if not isinstance(row, dict):
                errors.append(f"{group_name} receipt is malformed: {name}")
                continue
            try:
                path = root / safe_relative(row.get("path"), group_name)
            except EvidenceError as error:
                errors.append(str(error))
                continue
            if (not path.is_file() or path.stat().st_size != row.get("bytes") or
                    sha256(path) != row.get("sha256")):
                errors.append(f"{group_name} receipt differs: {name}")
    if (isinstance(files, dict) and isinstance(payloads, dict) and
            files.get("kernel.elf", {}).get("sha256") !=
            payloads.get("kernel.elf", {}).get("sha256")):
        errors.append("kernel payload differs from candidate ELF")
    if result.get("candidate") != files:
        errors.append("result candidate identity differs from its manifest")
    return files


def exact_count(lines, value):
    return sum(1 for line in lines if line == value)


def verify_host(root, result, candidate, errors):
    rows = result.get("host_runs")
    if not isinstance(rows, list) or {row.get("name") for row in rows
                                      if isinstance(row, dict)} != set(HOST_PROFILES):
        errors.append("host test profile coverage differs")
        return
    source_hash = result.get("candidate", {}).get("kernel.elf", {}).get("sha256")
    del source_hash
    for row in rows:
        if not isinstance(row, dict):
            errors.append("host run record is malformed")
            continue
        name = row.get("name")
        try:
            directory = root / safe_relative(row.get("directory"), "host run")
        except EvidenceError as error:
            errors.append(str(error))
            continue
        log = directory / "run.log"
        binary = directory / "libc-format-host"
        exit_code = directory / "exit-code.txt"
        command_path = directory / "command.json"
        if not log.is_file():
            errors.append(f"host {name} log is missing")
            continue
        lines = log.read_text(errors="replace").splitlines()
        for case in HOST_CASES:
            if exact_count(lines, f"[FORMAT_HOST][CASE] id={case} status=PASS") != 1:
                errors.append(f"host {name} case coverage differs: {case}")
        if any("status=FAIL" in line for line in lines):
            errors.append(f"host {name} contains a failed assertion")
        contracts = [line for line in lines
                     if line.startswith("[FORMAT_HOST][CONTRACT] ")]
        expected_contract = (
            "[FORMAT_HOST][CONTRACT] canary=PASS nul=PASS "
            "truncation_required=PASS source_preserved=PASS "
            "width_budget=PASS va_list_preserved=PASS")
        if contracts != [expected_contract]:
            errors.append(f"host {name} memory/return contract differs")
        suites = [line for line in lines
                  if line.startswith("[FORMAT_HOST][SUITE] ")]
        if len(suites) != 1 or not re.fullmatch(
                r"\[FORMAT_HOST\]\[SUITE\] status=PASS cases=10 "
                r"assertions=[1-9][0-9]* failures=0 "
                r"seed=0x5eedf04a7c9b312d matrix=4096", suites[0]):
            errors.append(f"host {name} suite did not complete")
        if not exit_code.is_file() or exit_code.read_text().strip() != "0":
            errors.append(f"host {name} process exit is not zero")
        if (not binary.is_file() or sha256(binary) != row.get("binary_sha256")):
            errors.append(f"host {name} executable identity differs")
        try:
            command = read_json(command_path, f"host {name} command")
        except EvidenceError as error:
            errors.append(str(error))
            continue
        if (command.get("implementation_path") != "kernel/src/libc/string.c" or
                command.get("implementation_sha256") !=
                row.get("implementation_sha256")):
            errors.append(f"host {name} did not identify the formatter source")
        sanitizer = row.get("sanitizer")
        expected = {"normal": "none", "ubsan": "undefined",
                    "asan": "address"}.get(name)
        if sanitizer != expected or command.get("sanitizer") != expected:
            errors.append(f"host {name} sanitizer identity differs")


def parse_launch_env(path):
    values = {}
    if not path.is_file():
        return values
    for line in path.read_text(errors="replace").splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            values[key] = value.strip("'")
    return values


def verify_qemu(root, result, candidate, errors):
    rows = result.get("qemu_profiles")
    if not isinstance(rows, list) or {row.get("name") for row in rows
                                      if isinstance(row, dict)} != set(QEMU_PROFILES):
        errors.append("QEMU format profile coverage differs")
        return
    if not isinstance(candidate, dict):
        return
    for row in rows:
        if not isinstance(row, dict):
            errors.append("QEMU profile record is malformed")
            continue
        name = row.get("name")
        if name not in QEMU_PROFILES:
            errors.append("QEMU profile name is unexpected")
            continue
        machine, accel, smp = QEMU_PROFILES[name]
        directory = root / "qemu" / name
        if (row.get("machine"), row.get("accel"), row.get("smp")) != \
                (machine, accel, smp):
            errors.append(f"QEMU {name} configuration differs")
        if (row.get("kernel_sha256") != candidate["kernel.elf"]["sha256"] or
                row.get("image_sha256") != candidate["hobbyos.img"]["sha256"]):
            errors.append(f"QEMU {name} refers to another binary")
        run_image_value = row.get("run_image_path")
        try:
            run_image_relative = safe_relative(run_image_value,
                                               f"QEMU {name} run image")
        except EvidenceError:
            errors.append(f"QEMU {name} run image receipt is absent")
        else:
            run_image = root / run_image_relative
            if (not run_image.is_file() or
                    sha256(run_image) != row.get("run_image_final_sha256")):
                errors.append(f"QEMU {name} final run image differs")
        if row.get("run_image_initial_sha256") != candidate["hobbyos.img"]["sha256"]:
            errors.append(f"QEMU {name} run image did not start from the candidate")
        if (row.get("completion_observed") is not True or
                row.get("qemu_alive_after_cleanup") is not False or
                not isinstance(row.get("qemu_pid"), int) or
                row.get("qemu_pid") <= 0):
            errors.append(f"QEMU {name} completion/cleanup receipt differs")
        serial = directory / "serial.log"
        if not serial.is_file():
            errors.append(f"QEMU {name} serial is missing")
            continue
        lines = serial.read_text(errors="replace").splitlines()
        if exact_count(lines, "[FORMAT][SUITE_BEGIN] cases=22") != 1:
            errors.append(f"QEMU {name} suite start differs")
        for case in GUEST_CASES:
            if exact_count(lines, f"[FORMAT][CASE] id={case} status=PASS") != 1:
                errors.append(f"QEMU {name} guest case coverage differs: {case}")
        if exact_count(lines,
                       "[FORMAT][SUITE_END] status=PASS completed=1 cases=22") != 1:
            errors.append(f"QEMU {name} test status is not completed")
        if any(line.startswith("[FORMAT][CASE]") and "status=FAIL" in line
               for line in lines):
            errors.append(f"QEMU {name} contains a failed format case")
        try:
            suite_end = lines.index(
                "[FORMAT][SUITE_END] status=PASS completed=1 cases=22")
            pmm_begin = lines.index("[CORE] Init PMM...")
            if suite_end >= pmm_begin:
                errors.append(f"QEMU {name} selftest did not run before PMM")
        except ValueError:
            pass
        if not any(re.fullmatch(
                r"\[PMM\] Highest RAM: 0x[0-9A-F]{16} \| "
                r"Total Frames: 0x[0-9A-F]{16}", line) for line in lines):
            errors.append(f"QEMU {name} PMM consumer diagnostic is absent")
        for stage in ("CPU_RELEASE", "PCI_BEGIN", "PCI_SCAN_COMPLETE",
                      "CORE_COMPLETE"):
            pattern = re.compile(
                rf"\[BOOT\]\[PROGRESS\] stage={stage} "
                r"monotonic_ms=[0-9]+ clockevent_ticks=[0-9]+")
            if sum(1 for line in lines if pattern.fullmatch(line)) != 1:
                errors.append(f"QEMU {name} breadcrumb differs: {stage}")
        if (not any(line.startswith("[BOOT][RUNTIME_READY] PASS ")
                    for line in lines) or
                "[KERNEL] Entering Main Loop." not in lines):
            errors.append(f"QEMU {name} did not reach runtime readiness")
        if any(token in line for line in lines
               for token in ("FORMAT_ERROR", "PANIC", "#PF", "#GP", "FATAL")):
            errors.append(f"QEMU {name} contains a fatal/format error")

        launch = parse_launch_env(directory / "launch.env")
        if (launch.get("MACHINE") != machine or launch.get("ACCEL") != accel or
                launch.get("SMP") != str(smp) or
                launch.get("KERNEL_SHA256") != candidate["kernel.elf"]["sha256"] or
                launch.get("IMAGE_SHA256") != candidate["hobbyos.img"]["sha256"]):
            errors.append(f"QEMU {name} launch receipt differs")
        try:
            argv_value = json.loads((directory / "argv.json").read_text())
        except (OSError, json.JSONDecodeError):
            errors.append(f"QEMU {name} argv receipt is missing or malformed")
            argv_value = None
        if not isinstance(argv_value, list) or not all(
                isinstance(item, str) for item in argv_value):
            errors.append(f"QEMU {name} argv is invalid")
        else:
            joined = "\n".join(argv_value)
            if (machine not in joined or accel not in joined or
                    f"{smp},sockets=1,cores={smp},threads=1" not in joined):
                errors.append(f"QEMU {name} argv profile differs")


def read_jsonl(path, label, errors):
    if not path.is_file():
        errors.append(f"{label} is missing")
        return []
    rows = []
    for number, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
        if not line:
            continue
        try:
            value = json.loads(line)
        except json.JSONDecodeError:
            errors.append(f"{label} line {number} is malformed")
            continue
        if not isinstance(value, dict):
            errors.append(f"{label} line {number} is not an object")
            continue
        rows.append(value)
    return rows


def verify_receipt(root, row, label, errors):
    if not isinstance(row, dict):
        errors.append(f"{label} receipt is malformed")
        return None
    try:
        path = root / safe_relative(row.get("path"), label)
    except EvidenceError as error:
        errors.append(str(error))
        return None
    if (not path.is_file() or path.stat().st_size != row.get("bytes") or
            sha256(path) != row.get("sha256")):
        errors.append(f"{label} receipt differs")
        return None
    return path


def validate_consumer_transactions(rows, lines, expected_payloads, label, errors):
    if [row.get("payload") for row in rows] != list(expected_payloads):
        errors.append(f"{label} command coverage/order differs")
        return
    previous_sequence = 0
    for row in rows:
        required = {"schema", "payload", "marker", "start_line_count",
                    "end_line_count", "sequence", "handler_status", "crc32"}
        if not required.issubset(row):
            errors.append(f"{label} command receipt lacks fields")
            continue
        sequence = row.get("sequence")
        start = row.get("start_line_count")
        end = row.get("end_line_count")
        payload = row.get("payload")
        marker = row.get("marker")
        if (not isinstance(sequence, int) or sequence <= previous_sequence or
                not isinstance(start, int) or not isinstance(end, int) or
                start < 0 or end <= start or end > len(lines)):
            errors.append(f"{label} command range/sequence is invalid")
            continue
        previous_sequence = sequence
        segment = lines[start:end]
        crc = f"{zlib.crc32(payload.encode()) & 0xffffffff:08x}"
        accept = (f"[HARNESS][FRAME] ACCEPT seq={sequence} crc={crc} "
                  f"len={len(payload)}")
        begin = f"[HARNESS][BEGIN] seq={sequence}"
        ending = f"[HARNESS][END] seq={sequence} status=0"
        if (row.get("schema") != 1 or row.get("handler_status") != 0 or
                row.get("crc32") != crc or exact_count(segment, accept) != 1 or
                exact_count(segment, begin) != 1 or
                exact_count(segment, ending) != 1 or
                not any(marker in line for line in segment)):
            errors.append(f"{label} command transaction is not causal: {payload}")


def record_fields(line, prefix):
    if not line.startswith(prefix):
        return None
    fields = {}
    for token in line[len(prefix):].split():
        if "=" not in token:
            return None
        key, value = token.split("=", 1)
        if key in fields:
            return None
        fields[key] = value
    return fields


def modal_record_valid(line):
    prefix = "[MODALTEST][OPEN_CLOSE] PASS "
    fields = record_fields(line, prefix)
    if fields is None:
        return False
    integer_expected = {
        "cycles": 1000, "cleanup": 1000, "warmup_gone": 1,
        "handles": 1000, "handles_gone": 1000, "first_live_id": 0,
        "baseline_heap_stable": 1, "heap_stable": 1,
        "reaper_idle": 1, "zombies": 0, "free_inflight": 0,
        "modal_violations": 0, "session_violations": 0,
        "runs_delta": 1001, "workers_delta": 1001,
        "normal_delta": 1001, "killed_delta": 0,
        "cleanup_delta": 1001, "signals_delta": 1001,
        "duplicates_delta": 0, "alloc_fail_delta": 0,
        "create_fail_delta": 0, "begin_fail_delta": 0,
        "recovery_delta": 0, "second_session_delta": 0,
        "contexts_live": 0, "contexts_quarantined": 0,
    }
    try:
        if any(int(fields.get(key, "-1"), 0) != value
               for key, value in integer_expected.items()):
            return False
        if int(fields.get("warmup_id", "0"), 0) <= 0:
            return False
        baseline = int(fields["baseline_used"], 0)
        final = int(fields["final_used"], 0)
        signed_delta = int(fields["signed_delta"], 0)
        if signed_delta != final - baseline or \
                int(fields["drift_abs"], 0) != abs(signed_delta):
            return False
        for key in ("baseline_blocks", "final_blocks", "timer_pending_before",
                    "timer_pending_after", "timer_claimed_before",
                    "timer_claimed_after", "timer_nodes_before",
                    "timer_nodes_after"):
            if int(fields[key], 0) < 0:
                return False
    except (KeyError, ValueError):
        return False
    expected_direction = "UP" if signed_delta > 0 else \
        "DOWN" if signed_delta < 0 else "ZERO"
    if (fields.get("ownership") != "PASS" or fields.get("reason") != "none" or
            fields.get("heap_scope") != "GLOBAL_DIAGNOSTIC" or
            fields.get("heap_gate") != "OUTER_SCENARIO" or
            fields.get("direction") != expected_direction):
        return False
    return True


def checkpoint_fields(segment, prefix, expected_names, label, errors):
    records = [line for line in segment if line.startswith(prefix)]
    if len(records) != 1:
        errors.append(f"{label} count is {len(records)}, expected 1")
        return None
    fields = record_fields(records[0], prefix)
    if fields is None or tuple(fields) != expected_names:
        errors.append(f"{label} fields/order differ")
        return None
    return fields


def checkpoint_integer_fields(fields, names, label, errors):
    values = {}
    for name in names:
        try:
            value = int(fields[name], 10)
        except (KeyError, TypeError, ValueError):
            errors.append(f"{label} numeric field {name} is invalid")
            return None
        if value < 0 or value > (1 << 64) - 1:
            errors.append(f"{label} numeric field {name} is out of range")
            return None
        values[name] = value
    return values


def verify_checkpoint_resources(segment, marker, checkpoint, workers,
                                expected_status, label, errors):
    prefix = f"[TASKMANTEST][{marker}] "
    expected_names = (
        "checkpoint", "index", "id", "generation", "schedule",
        "runtime_ns", "bytes", "status",
    )
    records = [line for line in segment if line.startswith(prefix)]
    if len(records) != workers:
        errors.append(f"{label} resource count is {len(records)}, expected {workers}")
        return None
    resources = {}
    handles = set()
    ids = set()
    for line in records:
        fields = record_fields(line, prefix)
        if fields is None or tuple(fields) != expected_names:
            errors.append(f"{label} resource fields/order differ")
            return None
        values = checkpoint_integer_fields(
            fields,
            ("checkpoint", "index", "id", "generation", "schedule",
             "runtime_ns", "bytes"), label, errors)
        if values is None:
            return None
        index = values["index"]
        handle = (values["id"], values["generation"])
        if (values["checkpoint"] != checkpoint or index >= workers or
                index in resources or values["id"] == 0 or
                values["generation"] == 0 or handle in handles or
                values["id"] in ids or values["schedule"] == 0 or
                values["runtime_ns"] == 0 or values["bytes"] == 0 or
                fields["status"] != expected_status):
            errors.append(f"{label} resource identity/status differs")
            return None
        resources[index] = values
        handles.add(handle)
        ids.add(values["id"])
    if set(resources) != set(range(workers)):
        errors.append(f"{label} resource indexes are incomplete")
        return None
    return resources


def verify_memory_checkpoint(rows, lines, workers, label, errors):
    expected_payloads = (
        f"taskmantest memory-checkpoint-begin 1000 {workers}",
        "taskmantest memory-checkpoint-track",
        "taskmantest memory-checkpoint-progress",
        "taskmantest memory-checkpoint-end",
    )
    by_payload = {}
    bounds = {}
    for row in rows:
        payload = row.get("payload")
        if payload in expected_payloads:
            if payload in by_payload:
                errors.append(f"{label} checkpoint command is duplicated: {payload}")
                return
            start = row.get("start_line_count")
            end = row.get("end_line_count")
            if not isinstance(start, int) or not isinstance(end, int) or \
                    not 0 <= start < end <= len(lines):
                errors.append(f"{label} checkpoint command range is invalid")
                return
            by_payload[payload] = lines[start:end]
            bounds[payload] = (start, end)
    if set(by_payload) != set(expected_payloads):
        errors.append(f"{label} checkpoint commands are incomplete")
        return
    activity = lines[bounds[expected_payloads[1]][1]:
                     bounds[expected_payloads[2]][0]]
    if not any(line.startswith("[SMP] Task ") for line in activity):
        errors.append(f"{label} lacks worker activity between track and progress")
        return

    begin_names = (
        "checkpoint", "scope", "cycles", "workers", "attempts",
        "initial_workers", "modal_active", "contexts_live",
        "contexts_quarantined", "session_state", "route_state",
        "shell_paused", "model_live", "reaper_free_inflight",
        "global_used", "global_blocks", "global_comparable", "reason",
    )
    begin = checkpoint_fields(
        by_payload[expected_payloads[0]],
        "[TASKMANTEST][MEMORY_CHECKPOINT_BEGIN] PASS ", begin_names,
        f"{label} checkpoint begin", errors)
    if begin is None:
        return
    begin_values = checkpoint_integer_fields(
        begin,
        ("checkpoint", "cycles", "workers", "attempts", "initial_workers",
         "modal_active", "contexts_live", "contexts_quarantined",
         "session_state", "route_state", "shell_paused", "model_live",
         "reaper_free_inflight", "global_used", "global_blocks",
         "global_comparable"), f"{label} checkpoint begin", errors)
    if begin_values is None:
        return
    checkpoint = begin_values["checkpoint"]
    if (checkpoint == 0 or begin.get("scope") != "TARGET_IDENTITIES" or
            begin.get("reason") != "none" or begin_values["cycles"] != 1000 or
            begin_values["workers"] != workers or
            begin_values["attempts"] == 0 or
            any(begin_values[name] != 0 for name in
                ("initial_workers", "modal_active", "contexts_live",
                 "contexts_quarantined", "session_state", "route_state",
                 "shell_paused", "model_live", "global_comparable"))):
        errors.append(f"{label} initial checkpoint is not established")
        return

    track_names = (
        "checkpoint", "expected", "captured", "generations", "scheduled",
        "duplicates", "out_of_range", "attempts", "registry_generation",
        "reason",
    )
    track = checkpoint_fields(
        by_payload[expected_payloads[1]],
        "[TASKMANTEST][MEMORY_CHECKPOINT_TRACK] PASS ", track_names,
        f"{label} checkpoint track", errors)
    if track is None:
        return
    track_values = checkpoint_integer_fields(
        track, tuple(name for name in track_names if name != "reason"),
        f"{label} checkpoint track", errors)
    if track_values is None:
        return
    if (track_values["checkpoint"] != checkpoint or
            any(track_values[name] != workers for name in
                ("expected", "captured", "generations", "scheduled")) or
            track_values["duplicates"] != 0 or
            track_values["out_of_range"] != 0 or
            track_values["attempts"] == 0 or
            track_values["registry_generation"] == 0 or
            track.get("reason") != "none"):
        errors.append(f"{label} tracked resource summary differs")
        return
    tracked = verify_checkpoint_resources(
        by_payload[expected_payloads[1]], "MEMORY_RESOURCE_TRACK",
        checkpoint, workers, "CAPTURED", f"{label} track", errors)
    if tracked is None:
        return

    progress_names = (
        "checkpoint", "expected", "progressed", "schedule_delta",
        "attempts", "reason",
    )
    progress = checkpoint_fields(
        by_payload[expected_payloads[2]],
        "[TASKMANTEST][MEMORY_CHECKPOINT_PROGRESS] PASS ", progress_names,
        f"{label} checkpoint progress", errors)
    if progress is None:
        return
    progress_values = checkpoint_integer_fields(
        progress, tuple(name for name in progress_names if name != "reason"),
        f"{label} checkpoint progress", errors)
    if progress_values is None:
        return
    if (progress_values["checkpoint"] != checkpoint or
            progress_values["expected"] != workers or
            progress_values["progressed"] != workers or
            progress_values["schedule_delta"] == 0 or
            progress_values["attempts"] == 0 or
            progress.get("reason") != "none"):
        errors.append(f"{label} progress summary differs")
        return
    progressed = verify_checkpoint_resources(
        by_payload[expected_payloads[2]], "MEMORY_RESOURCE_PROGRESS",
        checkpoint, workers, "PROGRESSED", f"{label} progress", errors)
    if progressed is None:
        return
    computed_schedule_delta = 0
    for index in range(workers):
        before = tracked[index]
        after = progressed[index]
        if ((before["id"], before["generation"]) !=
                (after["id"], after["generation"]) or
                after["bytes"] != before["bytes"] or
                after["schedule"] <= before["schedule"] or
                after["runtime_ns"] <= before["runtime_ns"]):
            errors.append(f"{label} resource {index} did not progress coherently")
            return
        computed_schedule_delta += after["schedule"] - before["schedule"]
    if computed_schedule_delta != progress_values["schedule_delta"]:
        errors.append(f"{label} progress delta differs from resource rows")
        return

    end_names = (
        "checkpoint", "scope", "expected", "captured", "progressed",
        "released", "retained", "retained_bytes", "release_attempts",
        "drain_free_inflight", "modal_expected", "modal_runs",
        "modal_cleanup", "modal_signals",
        "modal_contexts_live", "modal_contexts_quarantined", "modal_active",
        "modal_violations", "session_state", "route_state", "shell_paused",
        "session_violations", "model_live", "reaper_zombies",
        "reaper_free_inflight", "global_baseline_used", "global_final_used",
        "global_direction", "global_drift", "global_baseline_blocks",
        "global_final_blocks", "global_comparable", "endpoint", "cleanup",
        "reason",
    )
    end = checkpoint_fields(
        by_payload[expected_payloads[3]],
        "[TASKMANTEST][MEMORY_CHECKPOINT_END] PASS ", end_names,
        f"{label} checkpoint end", errors)
    if end is None:
        return
    end_numeric = tuple(name for name in end_names if name not in
                        ("scope", "global_direction", "endpoint", "cleanup",
                         "reason"))
    end_values = checkpoint_integer_fields(
        end, end_numeric, f"{label} checkpoint end", errors)
    if end_values is None:
        return
    modal_expected = 1001
    if (end_values["checkpoint"] != checkpoint or
            end.get("scope") != "TARGET_IDENTITIES" or
            any(end_values[name] != workers for name in
                ("expected", "captured", "progressed", "released")) or
            end_values["retained"] != 0 or
            end_values["retained_bytes"] != 0 or
            end_values["release_attempts"] == 0 or
            end_values["drain_free_inflight"] != 0 or
            any(end_values[name] != modal_expected for name in
                ("modal_expected", "modal_runs", "modal_cleanup",
                 "modal_signals")) or
            any(end_values[name] != 0 for name in
                ("modal_contexts_live", "modal_contexts_quarantined",
                 "modal_active", "modal_violations", "session_state",
                 "route_state", "shell_paused", "session_violations",
                 "model_live", "global_comparable")) or
            end.get("endpoint") != "ESTABLISHED" or
            end.get("cleanup") != "COMPLETE" or end.get("reason") != "none"):
        errors.append(f"{label} final target-lifetime checkpoint differs")
        return
    baseline = end_values["global_baseline_used"]
    final = end_values["global_final_used"]
    expected_direction = "UP" if final > baseline else \
        "DOWN" if final < baseline else "ZERO"
    if (end.get("global_direction") != expected_direction or
            end_values["global_drift"] != abs(final - baseline)):
        errors.append(f"{label} raw global heap observation is inconsistent")
        return
    final_resources = verify_checkpoint_resources(
        by_payload[expected_payloads[3]], "MEMORY_RESOURCE_FINAL",
        checkpoint, workers, "RELEASED", f"{label} final", errors)
    if final_resources is None:
        return
    for index in range(workers):
        if ((final_resources[index]["id"], final_resources[index]["generation"]) !=
                (progressed[index]["id"], progressed[index]["generation"]) or
                final_resources[index]["schedule"] != progressed[index]["schedule"] or
                final_resources[index]["runtime_ns"] != progressed[index]["runtime_ns"] or
                final_resources[index]["bytes"] != progressed[index]["bytes"]):
            errors.append(f"{label} final resource {index} identity differs")
            return


def verify_consumer_profile(root, directory, profile, expected_name,
                            expected_smp, expected_kind, candidate, errors):
    label = f"consumer {expected_name}"
    if (profile.get("name") != expected_name or
            profile.get("machine") != "q35" or
            profile.get("accel") != "kvm" or
            profile.get("smp") != expected_smp or
            profile.get("kind") != expected_kind):
        errors.append(f"{label} profile identity differs")
    if (profile.get("kernel_sha256") != candidate["kernel.elf"]["sha256"] or
            profile.get("image_sha256") != candidate["hobbyos.img"]["sha256"] or
            profile.get("run_image_initial_sha256") !=
            candidate["hobbyos.img"]["sha256"]):
        errors.append(f"{label} refers to another candidate")
    try:
        run_image = root / safe_relative(profile.get("run_image_path"),
                                         f"{label} run image")
    except EvidenceError as error:
        errors.append(str(error))
    else:
        if (not run_image.is_file() or
                sha256(run_image) != profile.get("run_image_final_sha256")):
            errors.append(f"{label} final run image differs")
    if (profile.get("completion_observed") is not True or
            profile.get("qemu_alive_after_cleanup") is not False or
            not isinstance(profile.get("qemu_pid"), int) or
            profile.get("qemu_pid") <= 0):
        errors.append(f"{label} completion/cleanup differs")
    launch = parse_launch_env(directory / "launch.env")
    if (launch.get("MACHINE") != "q35" or launch.get("ACCEL") != "kvm" or
            launch.get("SMP") != str(expected_smp) or
            launch.get("KERNEL_SHA256") != candidate["kernel.elf"]["sha256"] or
            launch.get("IMAGE_SHA256") != candidate["hobbyos.img"]["sha256"]):
        errors.append(f"{label} launch receipt differs")
    try:
        argv = json.loads((directory / "argv.json").read_text())
    except (OSError, json.JSONDecodeError):
        argv = None
    if (not isinstance(argv, list) or
            f"{expected_smp},sockets=1,cores={expected_smp},threads=1" not in argv):
        errors.append(f"{label} argv receipt differs")
    serial = directory / "serial.log"
    if not serial.is_file():
        errors.append(f"{label} serial is missing")
        return []
    lines = serial.read_text(errors="replace").splitlines()
    if ("[SELFTEST][AUTORUN] PASS" not in lines or
            not any(line.startswith("[BOOT][RUNTIME_READY] PASS ")
                    for line in lines) or
            "[KERNEL] Entering Main Loop." not in lines):
        errors.append(f"{label} did not reach selftest/runtime readiness")
    if any(token in line for line in lines
           for token in ("PANIC", "#PF", "#GP", "FATAL", "STRUCTURAL_FAULT")):
        errors.append(f"{label} contains a guest fault")
    return lines


def verify_consumers(root, result, errors):
    value = result.get("consumer_result")
    if value is None:
        errors.append("current modal/visual consumer evidence is absent")
        return
    try:
        consumer = read_json(root / safe_relative(value, "consumer result"),
                             "consumer result")
    except EvidenceError as error:
        errors.append(str(error))
        return
    if consumer.get("schema") != 1 or consumer.get("status") != "PASS":
        errors.append("consumer result schema/status differs")
    if consumer.get("configuration") != {
            "SELFTEST": 1, "SELFTEST_AUTORUN": 1, "FORMAT_TEST": 0}:
        errors.append("consumer candidate configuration differs")
    candidate = consumer.get("candidate")
    payloads = consumer.get("payloads")
    if not isinstance(candidate, dict) or set(candidate) != {
            "kernel.elf", "hobbyos.img", "BOOTX64.EFI"}:
        errors.append("consumer candidate coverage differs")
        return
    if not isinstance(payloads, dict) or set(payloads) != set(PAYLOADS):
        errors.append("consumer payload coverage differs")
        return
    for name, row in candidate.items():
        verify_receipt(root, row, f"consumer candidate {name}", errors)
    for name, row in payloads.items():
        verify_receipt(root, row, f"consumer payload {name}", errors)
    if candidate["kernel.elf"].get("sha256") != payloads["kernel.elf"].get("sha256"):
        errors.append("consumer kernel payload differs")

    expected_modal = [f"q35-kvm-smp{smp}-run{run}"
                      for smp, count in ((4, 2), (8, 3))
                      for run in range(1, count + 1)]
    if consumer.get("modal_heap_profiles") != expected_modal:
        errors.append("modal/heap profile declaration differs")
    for name in expected_modal:
        match = re.fullmatch(r"q35-kvm-smp(4|8)-run([1-3])", name)
        smp = int(match.group(1))
        directory = root / "consumers/modal-heap" / name
        try:
            profile = read_json(directory / "profile.json", f"consumer {name} profile")
        except EvidenceError as error:
            errors.append(str(error))
            continue
        lines = verify_consumer_profile(root, directory, profile, name, smp,
                                        "modal-heap", candidate, errors)
        expected_commands = (
            f"taskmantest memory-checkpoint-begin 1000 {smp * 2}",
            f"smpstress {smp * 2} 0 1000",
            "taskmantest memory-checkpoint-track",
            "modaltest open-close 1000",
            "modaltest check", "taskdiag check",
            "taskmantest memory-checkpoint-progress",
            "killtest smpstress-sweep",
            "modaltest check", "taskdiag check",
            "taskmantest memory-checkpoint-end",
            "tasktest transport-status",
        )
        rows = read_jsonl(directory / "commands.jsonl",
                          f"consumer {name} commands", errors)
        validate_consumer_transactions(rows, lines, expected_commands,
                                       f"consumer {name}", errors)
        records = [line for line in lines
                   if line.startswith("[MODALTEST][OPEN_CLOSE] PASS ")]
        if len(records) != 1 or not modal_record_valid(records[0]):
            errors.append(f"consumer {name} modal/heap record differs")
        verify_memory_checkpoint(rows, lines, smp * 2,
                                 f"consumer {name}", errors)
        if not any(line.startswith("[SMP][KILL_SWEEP] PASS ") and
                   f"workers={smp * 2}" in line for line in lines):
            errors.append(f"consumer {name} sweep evidence differs")
        if not any("[SMP] Task " in line for line in lines):
            errors.append(f"consumer {name} lacks concurrent worker progress")
        transport = [line for line in lines if line.startswith("[HARNESS][STATUS] ")]
        if len(transport) != 1:
            errors.append(f"consumer {name} transport status differs")
        else:
            fields = record_fields(transport[0], "[HARNESS][STATUS] ")
            try:
                if int(fields["accepted"], 0) != int(fields["completed"], 0):
                    errors.append(f"consumer {name} transport is unbalanced")
            except (TypeError, KeyError, ValueError):
                errors.append(f"consumer {name} transport status is malformed")

    visual_name = consumer.get("visual_profile")
    if visual_name != "q35-kvm-smp4":
        errors.append("visual profile declaration differs")
        return
    directory = root / "consumers/visual/q35-kvm-smp4"
    try:
        profile = read_json(directory / "profile.json", "visual profile")
    except EvidenceError as error:
        errors.append(str(error))
        return
    lines = verify_consumer_profile(root, directory, profile, visual_name, 4,
                                    "visual", candidate, errors)
    expected_commands = (
        "taskmantest stats-reset", "taskmantest geometry-runtime 118 40",
        "taskmantest anchor-reset", "taskmantest stats",
        "taskmantest geometry-runtime 76 40", "taskmantest anchor-reset",
        "taskmantest stats", "taskmantest geometry-clear", "inputtest check",
        "modaltest check", "taskdiag check", "taskmantest check",
    )
    rows = read_jsonl(directory / "commands.jsonl", "visual commands", errors)
    validate_consumer_transactions(rows, lines, expected_commands,
                                   "visual", errors)
    observations = read_jsonl(directory / "visual-observations.jsonl",
                              "visual observations", errors)
    if [(row.get("mode"), row.get("columns")) for row in observations] != [
            ("WIDE", 118), ("COMPACT", 76)]:
        errors.append("visual mode/geometry coverage differs")
    for row in observations:
        screenshot = directory / str(row.get("screenshot", ""))
        if (not screenshot.is_file() or screenshot.stat().st_size < 100000 or
                not screenshot.read_bytes().startswith(b"P6") or
                screenshot.stat().st_size != row.get("screenshot_bytes") or
                sha256(screenshot) != row.get("screenshot_sha256")):
            errors.append(f"visual {row.get('mode')} screendump differs")
        stats = row.get("stats_line", "")
        mode = row.get("mode")
        if (not isinstance(stats, str) or f"last_mode={mode}" not in stats or
                not isinstance(row.get("full_frames"), int) or
                row.get("full_frames") < 1):
            errors.append(f"visual {mode} frame evidence differs")
        for field in ("last_session_fallback_frames", "stable_frame_full_clears",
                      "stale_cells", "clipped", "last_session_scroll_delta",
                      "workspace_live"):
            if not re.search(rf"(?:^| ){field}=0(?: |$)", stats):
                errors.append(f"visual {mode} {field} is nonzero or absent")
        try:
            start = int(row["start_line_count"])
            end = int(row["end_line_count"])
        except (KeyError, TypeError, ValueError):
            errors.append(f"visual {mode} observation range is invalid")
        else:
            segment = lines[start:end] if 0 <= start < end <= len(lines) else []
            if (not any("[MODAL] session_begin OK" in line for line in segment) or
                    not any("[TASKMAN][OWNER_STATE] shell=BLOCKED "
                            "session=ACTIVE" in line for line in segment) or
                    not any("[MODAL] session_end OK" in line for line in segment)):
                errors.append(f"visual {mode} modal observation is not causal")


def verify_evidence(root, require_consumers=True):
    errors = []
    try:
        result = read_json(root / "result.json", "format result")
    except EvidenceError as error:
        return [str(error)]
    if (result.get("schema") != 1 or
            result.get("kind") != "bounded-format-evidence"):
        errors.append("format result schema is unsupported")
    if result.get("adversarial") is not False:
        errors.append("expected and adversarial evidence are confused")
    expected_config = {
        "SELFTEST": 1, "SELFTEST_AUTORUN": 0,
        "FORMAT_TEST": 1, "HOBBYOS_FORMAT_TEST": 1,
    }
    if result.get("configuration") != expected_config:
        errors.append("format test configuration differs")
    matrix = result.get("matrix")
    if (not isinstance(matrix, dict) or
            matrix.get("host_seed") != "0x5eedf04a7c9b312d" or
            matrix.get("host_iterations") != 4096 or
            matrix.get("host_cases") != list(HOST_CASES) or
            matrix.get("guest_cases") != list(GUEST_CASES)):
        errors.append("deterministic format matrix declaration differs")
    verify_source(root, result, errors)
    candidate = verify_candidate(root, result, errors)
    verify_host(root, result, candidate, errors)
    verify_qemu(root, result, candidate, errors)
    if require_consumers:
        verify_consumers(root, result, errors)
    return errors


def write_consumer_fixture(root):
    candidate = root / "consumers/candidate"
    payload_dir = candidate / "payloads"
    payload_dir.mkdir(parents=True, exist_ok=True)
    candidate_content = {
        "kernel.elf": b"fixture-consumer-kernel",
        "hobbyos.img": b"fixture-consumer-image",
        "BOOTX64.EFI": b"fixture-consumer-efi",
    }
    for name, content in candidate_content.items():
        (candidate / name).write_bytes(content)
    payload_content = {
        "kernel.elf": candidate_content["kernel.elf"],
        "BOOTX64.EFI": candidate_content["BOOTX64.EFI"],
        "zap-light16.psf": b"fixture-consumer-font",
        "logo.bmp": b"fixture-consumer-logo",
        "startup.nsh": b"fixture-consumer-startup",
    }
    for name, content in payload_content.items():
        (payload_dir / name).write_bytes(content)

    kernel_hash = sha256(candidate / "kernel.elf")
    image_hash = sha256(candidate / "hobbyos.img")

    def add_transaction(lines, rows, payload, marker, extra=()):
        sequence = len(rows) + 1
        start = len(lines)
        crc = f"{zlib.crc32(payload.encode()) & 0xffffffff:08x}"
        lines.extend((
            f"[HARNESS][FRAME] ACCEPT seq={sequence} crc={crc} "
            f"len={len(payload)}",
            f"[HARNESS][BEGIN] seq={sequence}",
            marker,
        ))
        lines.extend(extra)
        lines.append(f"[HARNESS][END] seq={sequence} status=0")
        rows.append({
            "schema": 1,
            "payload": payload,
            "marker": marker,
            "start_line_count": start,
            "end_line_count": len(lines),
            "sequence": sequence,
            "handler_status": 0,
            "crc32": crc,
        })

    def write_profile(directory, name, smp, kind, lines, rows):
        directory.mkdir(parents=True, exist_ok=True)
        run_image = directory / "hobbyos-run.img"
        run_image.write_bytes(candidate_content["hobbyos.img"])
        relative = run_image.relative_to(root).as_posix()
        profile = {
            "schema": 1,
            "name": name,
            "machine": "q35",
            "accel": "kvm",
            "smp": smp,
            "kind": kind,
            "qemu_pid": 4100 + smp,
            "started_at": "2026-09-05T20:00:00-03:00",
            "ended_at": "2026-09-05T20:01:00-03:00",
            "kernel_sha256": kernel_hash,
            "image_sha256": image_hash,
            "run_image_path": relative,
            "run_image_initial_sha256": image_hash,
            "run_image_final_sha256": image_hash,
            "completion_observed": True,
            "qemu_alive_after_cleanup": False,
        }
        (directory / "profile.json").write_text(
            json.dumps(profile, indent=2, sort_keys=True) + "\n")
        (directory / "serial.log").write_text("\n".join(lines) + "\n")
        (directory / "launch.env").write_text(
            f"MACHINE=q35\nACCEL=kvm\nSMP={smp}\n"
            f"IMAGE_SHA256={image_hash}\nKERNEL_SHA256={kernel_hash}\n")
        argv = ["qemu-system-x86_64", "-machine", "q35", "-accel", "kvm",
                "-smp", f"{smp},sockets=1,cores={smp},threads=1"]
        (directory / "argv.json").write_text(json.dumps(argv) + "\n")
        with (directory / "commands.jsonl").open("w") as stream:
            for row in rows:
                stream.write(json.dumps(row, sort_keys=True) + "\n")

    modal_profiles = []
    for smp, run_count in ((4, 2), (8, 3)):
        for run in range(1, run_count + 1):
            name = f"q35-kvm-smp{smp}-run{run}"
            modal_profiles.append(name)
            lines = [
                "[SELFTEST][AUTORUN] PASS",
                f"[BOOT][RUNTIME_READY] PASS runtime_ready={smp}/{smp}",
                "[KERNEL] Entering Main Loop.",
            ]
            rows = []
            workers = smp * 2
            checkpoint = 7
            add_transaction(
                lines, rows,
                f"taskmantest memory-checkpoint-begin 1000 {workers}",
                "[TASKMANTEST][MEMORY_CHECKPOINT_BEGIN] PASS "
                f"checkpoint={checkpoint} scope=TARGET_IDENTITIES cycles=1000 "
                f"workers={workers} attempts=1 initial_workers=0 modal_active=0 "
                "contexts_live=0 contexts_quarantined=0 session_state=0 "
                "route_state=0 shell_paused=0 model_live=0 "
                "reaper_free_inflight=0 global_used=2048 global_blocks=16 "
                "global_comparable=0 reason=none")
            add_transaction(lines, rows, f"smpstress {workers} 0 1000",
                            f"[SMP] smpstress spawning workers={workers}")
            tracked = tuple(
                "[TASKMANTEST][MEMORY_RESOURCE_TRACK] "
                f"checkpoint={checkpoint} index={index} id={1000 + index} "
                f"generation={2000 + index} schedule={10 + index} "
                f"runtime_ns={10000 + index * 100} bytes={20000 + index} "
                "status=CAPTURED"
                for index in range(workers))
            add_transaction(
                lines, rows, "taskmantest memory-checkpoint-track",
                "[TASKMANTEST][MEMORY_CHECKPOINT_TRACK] PASS "
                f"checkpoint={checkpoint} expected={workers} captured={workers} "
                f"generations={workers} scheduled={workers} duplicates=0 "
                "out_of_range=0 attempts=1 registry_generation=99 reason=none",
                tracked)
            lines.append("[SMP] Task 0 cpu=0 test=A iter=1 acc=0x1")
            modal = (
                "[MODALTEST][OPEN_CLOSE] PASS cycles=1000 cleanup=1000 "
                "ownership=PASS reason=none "
                "warmup_id=41 warmup_gone=1 handles=1000 handles_gone=1000 "
                "first_live_id=0 baseline_used=2048 final_used=2048 "
                "direction=ZERO signed_delta=0 drift_abs=0 "
                "baseline_blocks=16 final_blocks=16 "
                "heap_scope=GLOBAL_DIAGNOSTIC heap_gate=OUTER_SCENARIO "
                "baseline_heap_stable=1 heap_stable=1 "
                "reaper_idle=1 zombies=0 free_inflight=0 "
                "modal_violations=0 session_violations=0 "
                "runs_delta=1001 workers_delta=1001 normal_delta=1001 "
                "killed_delta=0 cleanup_delta=1001 signals_delta=1001 "
                "duplicates_delta=0 alloc_fail_delta=0 create_fail_delta=0 "
                "begin_fail_delta=0 recovery_delta=0 second_session_delta=0 "
                "contexts_live=0 contexts_quarantined=0 "
                "timer_pending_before=2 timer_pending_after=3 "
                "timer_claimed_before=4 timer_claimed_after=5 "
                "timer_nodes_before=6 timer_nodes_after=8")
            add_transaction(lines, rows, "modaltest open-close 1000", modal)
            add_transaction(lines, rows, "modaltest check",
                            "[MODALTEST][CHECK] PASS")
            add_transaction(lines, rows, "taskdiag check",
                            "[TASKDIAG][CHECK] PASS")
            progressed = tuple(
                "[TASKMANTEST][MEMORY_RESOURCE_PROGRESS] "
                f"checkpoint={checkpoint} index={index} id={1000 + index} "
                f"generation={2000 + index} schedule={11 + index} "
                f"runtime_ns={10100 + index * 100} bytes={20000 + index} "
                "status=PROGRESSED"
                for index in range(workers))
            add_transaction(
                lines, rows, "taskmantest memory-checkpoint-progress",
                "[TASKMANTEST][MEMORY_CHECKPOINT_PROGRESS] PASS "
                f"checkpoint={checkpoint} expected={workers} progressed={workers} "
                f"schedule_delta={workers} attempts=1 reason=none",
                progressed)
            add_transaction(lines, rows, "killtest smpstress-sweep",
                            f"[SMP][KILL_SWEEP] PASS workers={workers} "
                            f"killed={workers} cleaned={workers} reaped={workers}")
            add_transaction(lines, rows, "modaltest check",
                            "[MODALTEST][CHECK] PASS")
            add_transaction(lines, rows, "taskdiag check",
                            "[TASKDIAG][CHECK] PASS")
            released = tuple(
                "[TASKMANTEST][MEMORY_RESOURCE_FINAL] "
                f"checkpoint={checkpoint} index={index} id={1000 + index} "
                f"generation={2000 + index} schedule={11 + index} "
                f"runtime_ns={10100 + index * 100} bytes={20000 + index} "
                "status=RELEASED"
                for index in range(workers))
            add_transaction(
                lines, rows, "taskmantest memory-checkpoint-end",
                "[TASKMANTEST][MEMORY_CHECKPOINT_END] PASS "
                f"checkpoint={checkpoint} scope=TARGET_IDENTITIES "
                f"expected={workers} captured={workers} progressed={workers} "
                f"released={workers} retained=0 retained_bytes=0 "
                "release_attempts=1 drain_free_inflight=0 "
                "modal_expected=1001 modal_runs=1001 modal_cleanup=1001 "
                "modal_signals=1001 modal_contexts_live=0 "
                "modal_contexts_quarantined=0 modal_active=0 "
                "modal_violations=0 session_state=0 route_state=0 "
                "shell_paused=0 session_violations=0 model_live=0 "
                "reaper_zombies=0 reaper_free_inflight=0 "
                "global_baseline_used=2048 global_final_used=2112 "
                "global_direction=UP global_drift=64 "
                "global_baseline_blocks=16 global_final_blocks=17 "
                "global_comparable=0 endpoint=ESTABLISHED cleanup=COMPLETE "
                "reason=none",
                released)
            add_transaction(lines, rows, "tasktest transport-status",
                            "[HARNESS][STATUS] accepted=12 completed=12")
            write_profile(root / "consumers/modal-heap" / name,
                          name, smp, "modal-heap", lines, rows)

    visual = root / "consumers/visual/q35-kvm-smp4"
    lines = [
        "[SELFTEST][AUTORUN] PASS",
        "[BOOT][RUNTIME_READY] PASS runtime_ready=4/4",
        "[KERNEL] Entering Main Loop.",
    ]
    rows = []
    add_transaction(lines, rows, "taskmantest stats-reset",
                    "[TASKMANTEST][STATS_RESET] PASS")
    observations = []
    for mode, columns in (("WIDE", 118), ("COMPACT", 76)):
        add_transaction(lines, rows, f"taskmantest geometry-runtime {columns} 40",
                        "[TASKMANTEST][GEOMETRY_RUNTIME] PASS")
        add_transaction(lines, rows, "taskmantest anchor-reset",
                        "[TASKMANTEST][ANCHOR_RESET] PASS")
        start = len(lines)
        lines.extend((
            "[MODAL] session_begin OK owner=fixture",
            "[TASKMAN][OWNER_STATE] shell=BLOCKED session=ACTIVE",
            "[MODAL] session_end OK owner=fixture",
        ))
        stats = (
            f"[TASKMANTEST][STATS] last_mode={mode} "
            "last_session_full_frames=1 last_session_fallback_frames=0 "
            "stable_frame_full_clears=0 stale_cells=0 clipped=0 "
            "last_session_scroll_delta=0 workspace_live=0")
        add_transaction(lines, rows, "taskmantest stats", stats)
        screenshot = visual / f"{mode.lower()}-screendump.ppm"
        visual.mkdir(parents=True, exist_ok=True)
        screenshot.write_bytes(b"P6\n400 100\n255\n" + bytes(120000))
        observations.append({
            "schema": 1,
            "mode": mode,
            "columns": columns,
            "start_line_count": start,
            "end_line_count": len(lines),
            "begin_count_before": 0,
            "end_count_before": 0,
            "owner_count_before": 0,
            "screenshot": screenshot.name,
            "screenshot_bytes": screenshot.stat().st_size,
            "screenshot_sha256": sha256(screenshot),
            "full_frames": 1,
            "stats_line": stats,
        })
    add_transaction(lines, rows, "taskmantest geometry-clear",
                    "[TASKMANTEST][GEOMETRY_RUNTIME] CLEARED")
    add_transaction(lines, rows, "inputtest check", "[INPUTTEST][CHECK] PASS")
    add_transaction(lines, rows, "modaltest check", "[MODALTEST][CHECK] PASS")
    add_transaction(lines, rows, "taskdiag check", "[TASKDIAG][CHECK] PASS")
    add_transaction(lines, rows, "taskmantest check", "[TASKMANTEST][CHECK] PASS")
    write_profile(visual, "q35-kvm-smp4", 4, "visual", lines, rows)
    with (visual / "visual-observations.jsonl").open("w") as stream:
        for row in observations:
            stream.write(json.dumps(row, sort_keys=True) + "\n")

    def receipt(path):
        return {
            "path": path.relative_to(root).as_posix(),
            "bytes": path.stat().st_size,
            "sha256": sha256(path),
        }

    consumer = {
        "schema": 1,
        "status": "PASS",
        "configuration": {
            "SELFTEST": 1, "SELFTEST_AUTORUN": 1, "FORMAT_TEST": 0},
        "candidate": {name: receipt(candidate / name)
                      for name in candidate_content},
        "payloads": {name: receipt(payload_dir / name)
                     for name in payload_content},
        "modal_heap_profiles": modal_profiles,
        "visual_profile": "q35-kvm-smp4",
    }
    (root / "consumers/result.json").write_text(
        json.dumps(consumer, indent=2, sort_keys=True) + "\n")


def write_fixture(root):
    repo = repository_root()
    (root / "candidate/payloads").mkdir(parents=True, exist_ok=True)
    (root / "candidate/kernel.elf").write_bytes(b"fixture-kernel")
    (root / "candidate/hobbyos.img").write_bytes(b"fixture-image")
    (root / "candidate/BOOTX64.EFI").write_bytes(b"fixture-efi")
    payload_content = {
        "kernel.elf": b"fixture-kernel",
        "BOOTX64.EFI": b"fixture-efi",
        "zap-light16.psf": b"fixture-font",
        "logo.bmp": b"fixture-logo",
        "startup.nsh": b"fixture-startup",
    }
    for name, content in payload_content.items():
        (root / "candidate/payloads" / name).write_bytes(content)

    host_lines = ["[FORMAT_HOST][BEGIN] seed=0x5eedf04a7c9b312d matrix=4096"]
    host_lines.extend(
        f"[FORMAT_HOST][CASE] id={case} status=PASS" for case in HOST_CASES)
    host_lines.append(
        "[FORMAT_HOST][CONTRACT] canary=PASS nul=PASS "
        "truncation_required=PASS source_preserved=PASS "
        "width_budget=PASS va_list_preserved=PASS")
    host_lines.append(
        "[FORMAT_HOST][SUITE] status=PASS cases=10 assertions=4179 "
        "failures=0 seed=0x5eedf04a7c9b312d matrix=4096")
    sanitizers = {"normal": "none", "ubsan": "undefined",
                  "asan": "address"}
    implementation_hash = sha256(repo / "kernel/src/libc/string.c")
    for name in HOST_PROFILES:
        run = root / "host" / name
        run.mkdir(parents=True, exist_ok=True)
        (run / "run.log").write_text("\n".join(host_lines) + "\n")
        (run / "exit-code.txt").write_text("0\n")
        (run / "libc-format-host").write_bytes(("fixture-" + name).encode())
        (run / "command.json").write_text(json.dumps({
            "argv": ["gcc", "kernel/src/libc/string.c",
                     "scripts/libc-format-host.c"],
            "implementation_path": "kernel/src/libc/string.c",
            "implementation_sha256": implementation_hash,
            "sanitizer": sanitizers[name],
        }, indent=2) + "\n")

    guest_lines = [
        "[CORE] Serial init OK (dynamic ports from BootInfo)",
        "[CORE] Init GDT...", "[CORE] Init IDT...",
        "[FORMAT][SUITE_BEGIN] cases=22",
    ]
    guest_lines.extend(
        f"[FORMAT][CASE] id={case} status=PASS" for case in GUEST_CASES)
    guest_lines.extend((
        "[FORMAT][SUITE_END] status=PASS completed=1 cases=22",
        "[CORE] Init PMM...",
        "[PMM] Highest RAM: 0x0000000040000000 | "
        "Total Frames: 0x0000000000040000",
        "[BOOT][PROGRESS] stage=CPU_RELEASE monotonic_ms=1 "
        "clockevent_ticks=1",
        "[BOOT][PROGRESS] stage=PCI_BEGIN monotonic_ms=2 "
        "clockevent_ticks=2",
        "[BOOT][PROGRESS] stage=PCI_SCAN_COMPLETE monotonic_ms=3 "
        "clockevent_ticks=3",
        "[BOOT][PROGRESS] stage=CORE_COMPLETE monotonic_ms=4 "
        "clockevent_ticks=4",
        "[BOOT][RUNTIME_READY] PASS runtime_ready=1/1",
        "[KERNEL] Entering Main Loop.",
    ))
    kernel_hash = sha256(root / "candidate/kernel.elf")
    image_hash = sha256(root / "candidate/hobbyos.img")
    for name, (machine, accel, smp) in QEMU_PROFILES.items():
        run = root / "qemu" / name
        run.mkdir(parents=True, exist_ok=True)
        (run / "serial.log").write_text("\n".join(guest_lines) + "\n")
        (run / "debugcon.log").write_text("fixture\n")
        (run / "trace.log").write_text("")
        (run / "launch.env").write_text(
            f"MACHINE={machine}\nACCEL={accel}\nSMP={smp}\n"
            f"IMAGE_SHA256={image_hash}\nKERNEL_SHA256={kernel_hash}\n")
        argv = ["qemu-system-x86_64", "-machine", machine, "-accel", accel,
                "-smp", f"{smp},sockets=1,cores={smp},threads=1"]
        (run / "argv.json").write_text(json.dumps(argv) + "\n")
        profile = {
            "name": name, "machine": machine, "accel": accel, "smp": smp,
            "qemu_pid": 1234 + smp, "completion_observed": True,
            "qemu_alive_after_cleanup": False,
            "kernel_sha256": kernel_hash, "image_sha256": image_hash,
            "run_image_path": f"qemu/{name}/hobbyos-run.img",
            "run_image_initial_sha256": image_hash,
            "run_image_final_sha256": image_hash,
        }
        (run / "hobbyos-run.img").write_bytes(b"fixture-image")
        (run / "profile.json").write_text(
            json.dumps(profile, indent=2, sort_keys=True) + "\n")
    write_consumer_fixture(root)
    collect_evidence(root)


def run_fixtures(output_dir):
    if output_dir.exists():
        raise EvidenceError("fixture output directory already exists")
    output_dir.mkdir(parents=True)
    valid = output_dir / "valid"
    write_fixture(valid)
    valid_errors = verify_evidence(valid)
    records = [{"name": "valid", "expected": "PASS",
                "errors": valid_errors,
                "status": "PASS" if not valid_errors else "FAIL"}]
    if valid_errors:
        raise EvidenceError("valid fixture was rejected: " + "; ".join(valid_errors))

    def serial_mutation(root, mutator):
        path = root / "qemu/q35-tcg-smp1/serial.log"
        lines = path.read_text().splitlines()
        path.write_text("\n".join(mutator(lines)) + "\n")

    fixtures = []

    def missing_case(root):
        serial_mutation(root, lambda lines: [line for line in lines if
                         line != "[FORMAT][CASE] id=padding status=PASS"])
    fixtures.append(("missing-case", missing_case, "guest case coverage differs"))

    def duplicate_case(root):
        serial_mutation(root, lambda lines: lines + [
            "[FORMAT][CASE] id=padding status=PASS"])
    fixtures.append(("duplicate-case", duplicate_case,
                     "guest case coverage differs"))

    def contract_token(field):
        def mutate(root):
            path = root / "host/normal/run.log"
            path.write_text(path.read_text().replace(
                f"{field}=PASS", f"{field}=FAIL", 1))
        return mutate
    fixtures.extend((
        ("canary-altered", contract_token("canary"),
         "memory/return contract differs"),
        ("nul-missing", contract_token("nul"),
         "memory/return contract differs"),
        ("truncation-return-wrong", contract_token("truncation_required"),
         "memory/return contract differs"),
    ))

    def other_binary(root):
        path = root / "qemu/q35-tcg-smp1/profile.json"
        value = json.loads(path.read_text())
        value["kernel_sha256"] = "0" * 64
        path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")
        result_path = root / "result.json"
        result = json.loads(result_path.read_text())
        for row in result["qemu_profiles"]:
            if row["name"] == "q35-tcg-smp1":
                row["kernel_sha256"] = "0" * 64
        result_path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    fixtures.append(("other-binary", other_binary,
                     "refers to another binary"))

    def incomplete(root):
        serial_mutation(root, lambda lines: [line for line in lines if
                         not line.startswith("[FORMAT][SUITE_END]")])
    fixtures.append(("not-completed", incomplete,
                     "test status is not completed"))

    def adversarial_confused(root):
        path = root / "result.json"
        value = json.loads(path.read_text())
        value["adversarial"] = True
        path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")
    fixtures.append(("adversarial-confused", adversarial_confused,
                     "expected and adversarial evidence are confused"))

    def modal_serial_mutation(root, mutator):
        path = (root / "consumers/modal-heap/q35-kvm-smp4-run1/serial.log")
        lines = path.read_text().splitlines()
        path.write_text("\n".join(mutator(lines)) + "\n")

    def remove_track_resource(root):
        modal_serial_mutation(root, lambda lines: [
            "[FIXTURE][OMITTED_TRACK_RESOURCE]" if line.startswith(
                "[TASKMANTEST][MEMORY_RESOURCE_TRACK] checkpoint=7 index=0 ")
            else line for line in lines
        ])
    fixtures.append(("checkpoint-track-resource-missing",
                     remove_track_resource,
                     "track resource count is 7, expected 8"))

    def progress_without_change(root):
        def mutate(lines):
            needle = ("[TASKMANTEST][MEMORY_RESOURCE_PROGRESS] "
                      "checkpoint=7 index=0 id=1000 generation=2000 "
                      "schedule=11 runtime_ns=10100")
            replacement = ("[TASKMANTEST][MEMORY_RESOURCE_PROGRESS] "
                           "checkpoint=7 index=0 id=1000 generation=2000 "
                           "schedule=10 runtime_ns=10000")
            return [line.replace(needle, replacement) for line in lines]
        modal_serial_mutation(root, mutate)
    fixtures.append(("checkpoint-worker-without-progress",
                     progress_without_change,
                     "resource 0 did not progress coherently"))

    def final_generation_changed(root):
        def mutate(lines):
            needle = ("[TASKMANTEST][MEMORY_RESOURCE_FINAL] "
                      "checkpoint=7 index=0 id=1000 generation=2000 ")
            replacement = ("[TASKMANTEST][MEMORY_RESOURCE_FINAL] "
                           "checkpoint=7 index=0 id=1000 generation=2999 ")
            return [line.replace(needle, replacement) for line in lines]
        modal_serial_mutation(root, mutate)
    fixtures.append(("checkpoint-final-generation-changed",
                     final_generation_changed,
                     "final resource 0 identity differs"))

    def retained_resource_promoted(root):
        def mutate(lines):
            output = []
            for line in lines:
                if line.startswith(
                        "[TASKMANTEST][MEMORY_CHECKPOINT_END] PASS "):
                    line = line.replace("released=8 retained=0 retained_bytes=0",
                                        "released=7 retained=1 retained_bytes=64")
                    line = line.replace("global_final_used=2112",
                                        "global_final_used=2048")
                    line = line.replace("global_direction=UP global_drift=64",
                                        "global_direction=ZERO global_drift=0")
                if line.startswith(
                        "[TASKMANTEST][MEMORY_RESOURCE_FINAL] "
                        "checkpoint=7 index=0 "):
                    line = line.replace("status=RELEASED", "status=RETAINED")
                output.append(line)
            return output
        modal_serial_mutation(root, mutate)
    fixtures.append(("checkpoint-net-zero-retention-promoted",
                     retained_resource_promoted,
                     "final target-lifetime checkpoint differs"))

    def inconsistent_global_delta(root):
        modal_serial_mutation(root, lambda lines: [
            line.replace("global_direction=UP global_drift=64",
                         "global_direction=UP global_drift=63")
            for line in lines
        ])
    fixtures.append(("checkpoint-global-delta-inconsistent",
                     inconsistent_global_delta,
                     "raw global heap observation is inconsistent"))

    def global_claim_promoted(root):
        modal_serial_mutation(root, lambda lines: [
            line.replace("global_comparable=0 endpoint=ESTABLISHED",
                         "global_comparable=1 endpoint=ESTABLISHED")
            for line in lines
        ])
    fixtures.append(("checkpoint-global-comparability-invented",
                     global_claim_promoted,
                     "final target-lifetime checkpoint differs"))

    def async_drain_promoted(root):
        modal_serial_mutation(root, lambda lines: [
            line.replace("release_attempts=1 drain_free_inflight=0",
                         "release_attempts=1 drain_free_inflight=1")
            for line in lines
        ])
    fixtures.append(("checkpoint-async-drain-promoted",
                     async_drain_promoted,
                     "final target-lifetime checkpoint differs"))

    def worker_activity_removed(root):
        modal_serial_mutation(root, lambda lines: [
            "[FIXTURE][NO_WORKER_ACTIVITY]" if line.startswith("[SMP] Task ")
            else line for line in lines
        ])
    fixtures.append(("checkpoint-worker-activity-missing",
                     worker_activity_removed,
                     "lacks worker activity between track and progress"))

    def duplicate_final_resource(root):
        def mutate(lines):
            needle = ("[TASKMANTEST][MEMORY_RESOURCE_FINAL] "
                      "checkpoint=7 index=1 id=1001 generation=2001 ")
            replacement = ("[TASKMANTEST][MEMORY_RESOURCE_FINAL] "
                           "checkpoint=7 index=0 id=1000 generation=2000 ")
            return [line.replace(needle, replacement) for line in lines]
        modal_serial_mutation(root, mutate)
    fixtures.append(("checkpoint-final-resource-duplicated",
                     duplicate_final_resource,
                     "final resource identity/status differs"))

    def fragmented_track_resource(root):
        def mutate(lines):
            needle = ("[TASKMANTEST][MEMORY_RESOURCE_TRACK] "
                      "checkpoint=7 index=0 id=1000 generation=2000 ")
            return [line.replace(needle,
                                 needle.replace("generation=2000 ",
                                                "generation=[BOOT] "))
                    for line in lines]
        modal_serial_mutation(root, mutate)
    fixtures.append(("checkpoint-track-resource-fragmented",
                     fragmented_track_resource,
                     "track numeric field generation is invalid"))

    for name, mutate, expected in fixtures:
        destination = output_dir / name
        shutil.copytree(valid, destination)
        mutate(destination)
        errors = verify_evidence(destination)
        detected = any(expected in error for error in errors)
        records.append({
            "name": name,
            "expected": expected,
            "errors": errors,
            "status": "NEGATIVE_DETECTED" if detected else "FAIL",
        })
        if not detected:
            raise EvidenceError(f"negative fixture was not rejected correctly: {name}")
    receipt = {
        "schema": 1,
        "valid_controls": 1,
        "negative_controls": len(fixtures),
        "records": records,
        "status": "PASS",
    }
    (output_dir / "fixture-results.json").write_text(
        json.dumps(receipt, indent=2, sort_keys=True) + "\n")
    print(f"FORMAT_FIXTURES: PASS valid=1 negatives={len(fixtures)}")


def main():
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    collect_parser = subparsers.add_parser("collect")
    collect_parser.add_argument("evidence", type=Path)
    core_parser = subparsers.add_parser("core")
    core_parser.add_argument("evidence", type=Path)
    all_parser = subparsers.add_parser("all")
    all_parser.add_argument("evidence", type=Path)
    fixture_parser = subparsers.add_parser("fixtures")
    fixture_parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()
    try:
        if args.command == "collect":
            collect_evidence(args.evidence)
            print("FORMAT_EVIDENCE_COLLECTION: PASS")
            return 0
        if args.command == "fixtures":
            run_fixtures(args.output_dir)
            return 0
        errors = verify_evidence(args.evidence,
                                 require_consumers=args.command == "all")
    except EvidenceError as error:
        print(f"FORMAT_EVIDENCE_ERROR: {error}", file=sys.stderr)
        return 1
    if errors:
        for error in errors:
            print(f"FORMAT_EVIDENCE_REJECTED: {error}", file=sys.stderr)
        return 1
    consumer_suffix = " consumers=modal5,visual2" if args.command == "all" else ""
    print("FORMAT_EVIDENCE: PASS host=3 qemu=2 guest_cases=22" +
          consumer_suffix)
    return 0


if __name__ == "__main__":
    sys.exit(main())
