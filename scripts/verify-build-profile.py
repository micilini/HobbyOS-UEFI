#!/usr/bin/env python3
import hashlib
import json
import pathlib
import re
import sys


ABI_MARKER = "[BUILD_PROFILE][ABI]"
MEMORY_MARKER = "[BUILD_PROFILE][MEMORY]"
RUNTIME_MEMORY_SIZES = tuple(range(65)) + (
    127, 128, 129, 255, 256, 257, 511, 512, 513,
    1023, 1024, 1025, 2047, 2048, 2049, 4095, 4096, 4097,
)
REQUIRED_C_FLAGS = (
    "-O2",
    "-fno-strict-aliasing",
    "-fno-stack-protector",
    "-fno-delete-null-pointer-checks",
    "-fno-omit-frame-pointer",
    "-mgeneral-regs-only",
    "-Wextra",
    "-Wundef",
    "-Wmissing-prototypes",
    "-Wvla",
    "-Wframe-larger-than=2048",
    "-Werror",
)


class ValidationError(Exception):
    def __init__(self, code: str, detail: str = "") -> None:
        self.code = code
        self.detail = detail
        super().__init__(f"{code}: {detail}" if detail else code)


def fail(code: str, detail: str = "") -> None:
    raise ValidationError(code, detail)


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def require_file(path: pathlib.Path) -> pathlib.Path:
    if not path.is_file() or path.stat().st_size == 0:
        fail("FILE_MISSING", str(path))
    return path


def parse_fields(marker: str, line: str) -> tuple[str, dict[str, int]]:
    match = re.fullmatch(
        re.escape(marker) + r" (PASS|FAIL) ((?:[a-z0-9_]+=[0-9]+ ?)+)",
        line.strip(),
    )
    if not match:
        fail("MARKER_SCHEMA", line.strip())
    fields: dict[str, int] = {}
    for item in match.group(2).split():
        key, value = item.split("=", 1)
        if key in fields:
            fail("FIELD_DUPLICATE", key)
        fields[key] = int(value)
    return match.group(1), fields


def marker_lines(path: pathlib.Path, marker: str) -> list[str]:
    lines = [line.strip() for line in require_file(path).read_text(
        encoding="utf-8", errors="replace").splitlines() if marker in line]
    if len(lines) != 1:
        fail("MARKER_COUNT", f"{path}: {len(lines)}")
    return lines


def verify_abi_record(path: pathlib.Path) -> dict[str, int]:
    lines = marker_lines(path, ABI_MARKER)
    status, fields = parse_fields(ABI_MARKER, lines[0])
    expected = {
        "bsp_seen", "bsp_rsp_mod16", "bsp_df", "ap_expected", "ap_seen",
        "ap_bad_rsp", "ap_bad_df", "irq_entries", "irq_bad_df",
        "probe_vector", "probe_entries_delta", "probe_bad_df_delta",
        "restored_df", "final_df",
    }
    if set(fields) != expected:
        fail("FIELD_SET", f"missing={sorted(expected - set(fields))} "
             f"extra={sorted(set(fields) - expected)}")
    if status != "PASS":
        fail("HANDLER_FAIL", lines[0])
    if fields["bsp_seen"] != 1 or fields["bsp_rsp_mod16"] != 8:
        fail("BSP_STACK_ALIGNMENT", lines[0])
    if fields["ap_expected"] < 1 or fields["ap_seen"] != fields["ap_expected"]:
        fail("AP_COVERAGE", lines[0])
    if fields["ap_bad_rsp"] != 0:
        fail("AP_STACK_ALIGNMENT", lines[0])
    if (fields["bsp_df"] != 0 or fields["ap_bad_df"] != 0 or
            fields["irq_bad_df"] != 0 or
            fields["probe_bad_df_delta"] != 0):
        fail("DIRECTION_FLAG_NOT_CLEAR", lines[0])
    if fields["irq_entries"] < 1 or fields["probe_vector"] != 35 or \
            fields["probe_entries_delta"] < 1:
        fail("IRQ_PROBE_MISSING", lines[0])
    if fields["restored_df"] != 1 or fields["final_df"] != 0:
        fail("DIRECTION_FLAG_RESTORE", lines[0])
    return fields


def runtime_overlap_cases() -> int:
    count = 0
    for size in RUNTIME_MEMORY_SIZES:
        for src_alignment in range(16):
            for dest_alignment in range(16):
                delta = (src_alignment - dest_alignment) & 15
                if delta == 0:
                    delta = 16
                if size > delta:
                    count += 1
    return count


def verify_memory_record(path: pathlib.Path) -> dict[str, int]:
    lines = marker_lines(path, MEMORY_MARKER)
    status, fields = parse_fields(MEMORY_MARKER, lines[0])
    expected = {
        "cases", "max_size", "alignments", "memcpy_failures",
        "memset_failures", "memmove_failures", "overlap_forward",
        "overlap_backward", "benchmark_bytes", "samples",
        "median_cycles", "checksum", "heap_ok",
    }
    if set(fields) != expected:
        fail("MEMORY_FIELD_SET",
             f"missing={sorted(expected - set(fields))} "
             f"extra={sorted(set(fields) - expected)}")
    if status != "PASS":
        fail("MEMORY_HANDLER_FAIL", lines[0])
    expected_cases = len(RUNTIME_MEMORY_SIZES) * (16 * 16 * 3 + 16 * 3)
    overlap_cases = runtime_overlap_cases()
    if fields["cases"] != expected_cases or fields["max_size"] != 4097 or \
            fields["alignments"] != 16:
        fail("MEMORY_COVERAGE", lines[0])
    if fields["overlap_forward"] != overlap_cases or \
            fields["overlap_backward"] != overlap_cases:
        fail("MEMORY_OVERLAP_COVERAGE", lines[0])
    if fields["memcpy_failures"] != 0 or \
            fields["memset_failures"] != 0 or \
            fields["memmove_failures"] != 0:
        fail("MEMORY_REFERENCE_MISMATCH", lines[0])
    if fields["benchmark_bytes"] != 8 * 1024 * 1024 or \
            fields["samples"] != 7 or fields["median_cycles"] <= 0 or \
            fields["checksum"] <= 0:
        fail("MEMORY_BENCHMARK", lines[0])
    if fields["heap_ok"] != 1:
        fail("MEMORY_HEAP_INTEGRITY", lines[0])
    return fields


def ordered(text: str, needles: tuple[str, ...], code: str) -> None:
    position = -1
    for needle in needles:
        found = text.find(needle, position + 1)
        if found < 0:
            fail(code, f"missing/order: {needle}")
        position = found


def function_slice(text: str, start: str, end: str) -> str:
    first = text.find(start)
    if first < 0:
        fail("SOURCE_SYMBOL_MISSING", start)
    last = text.find(end, first + len(start))
    if last < 0:
        fail("SOURCE_SYMBOL_MISSING", end)
    return text[first:last]


def verify_source(root: pathlib.Path) -> dict[str, int]:
    entry = require_file(root / "kernel/src/core/entry.S").read_text()
    trampoline = require_file(root / "kernel/src/smp/trampoline.S").read_text()
    ap_entry = require_file(root / "kernel/src/smp/ap_entry.S").read_text()
    stubs = require_file(root / "kernel/src/core/interrupt_stubs.S").read_text()
    command = require_file(
        root / "kernel/src/shell/commands/cmd_profiletest.c").read_text()
    memory_source = require_file(
        root / "kernel/src/libc/memory.c").read_text()
    memory_harness = require_file(
        root / "scripts/build-profile-memory-host.c").read_text()

    bsp = function_slice(entry, "_start:", "fill_pd:")
    ordered(bsp, ("cli", "cld", "sub rsp, 8", "and esi, 15", "jmp rax"),
            "BSP_SOURCE_ORDER")
    ap16 = function_slice(trampoline, "smp_trampoline_entry:",
                          "trampoline_32:")
    ordered(ap16, ("cli", "cld"), "AP_TRAMPOLINE_DF_SOURCE_ORDER")
    ap = function_slice(ap_entry, "ap_kernel_entry_asm:", ".size")
    ordered(ap, ("cld", "and rsp, -16", "sub rsp, 8", "and edi, 15",
                 "jmp ap_kernel_entry"), "AP_SOURCE_ORDER")
    irq = function_slice(stubs, "irq_external_common_entry:",
                         ".macro IRQ_VECTOR_STUB")
    ordered(irq, ("IRQ_SAVE_ALL", "cld", "call irq_external_dispatch"),
            "IRQ_SOURCE_ORDER")
    if '"std\\n\\t"' not in command or '"int $35\\n\\t"' not in command \
            or '"cld"' not in command:
        fail("DF_PROBE_SOURCE", "std/int35/cld block missing")
    for token in ("profiletest_memory_correctness",
                  "PROFILETEST_BENCHMARK_BYTES",
                  "profiletest_tsc_begin", "profiletest_tsc_end"):
        if token not in command:
            fail("MEMORY_RUNTIME_SOURCE", token)
    for token in ("MEMORY_MAX_SIZE 4097u", "hobby_memcpy",
                  "hobby_memset", "hobby_memmove", "overlap_forward",
                  "overlap_backward"):
        if token not in memory_harness:
            fail("MEMORY_HOST_SOURCE", token)
    for token in ("memory_word_t", "aligned(1)", "may_alias",
                  "word *= UINT64_C(0x0101010101010101)"):
        if token not in memory_source:
            fail("MEMORY_WORD_SOURCE", token)
    return {"bsp": 1, "ap": 1, "irq": 1, "df_probe": 1,
            "memory_runtime": 1, "memory_host": 1}


def verify_manifest(source: pathlib.Path) -> None:
    manifest = require_file(source / "SHA256SUMS")
    seen = 0
    for raw in manifest.read_text().splitlines():
        if not raw.strip():
            continue
        match = re.fullmatch(r"([0-9a-f]{64})  (.+)", raw)
        if not match:
            fail("MANIFEST_SCHEMA", raw)
        path = source / match.group(2)
        require_file(path)
        if sha256(path) != match.group(1):
            fail("HASH_MISMATCH", str(path))
        seen += 1
    if seen < 10:
        fail("MANIFEST_INCOMPLETE", str(seen))


def exit_zero(path: pathlib.Path) -> None:
    if require_file(path).read_text().strip() != "0":
        fail("COMMAND_STATUS", str(path))


def verify_compiler_log(path: pathlib.Path) -> None:
    log = require_file(path).read_text(
        encoding="utf-8", errors="replace")
    if re.search(r"(?:^|:) (?:warning|error):", log, re.MULTILINE):
        fail("COMPILER_DIAGNOSTIC", str(path))
    lines = [line for line in log.splitlines()
             if line.startswith("gcc ") and " -c kernel/" in line and
             line.rstrip().endswith(".o") and ".S -o " not in line]
    if not lines:
        fail("COMPILER_LINES_MISSING")
    for line in lines:
        for flag in REQUIRED_C_FLAGS:
            if flag not in line.split():
                fail("COMPILER_FLAG_MISSING", f"{flag}: {line}")
        suppressions = [token for token in line.split()
                        if token.startswith("-Wno-")]
        is_protected_graphics = \
            " -c kernel/src/graphics/graphics.c " in line
        if is_protected_graphics:
            if suppressions != ["-Wno-missing-prototypes"]:
                fail("PROTECTED_OBJECT_SUPPRESSION", line)
        elif suppressions:
            fail("WARNING_SUPPRESSION_OUTSIDE_PROTECTED_OBJECT", line)
        optimization = [token for token in line.split()
                        if re.fullmatch(r"-O(?:[0-3gs]|fast)", token)]
        if optimization != ["-O2"]:
            fail("OPTIMIZATION_PROFILE", line)
        pattern_guard = "-fno-tree-loop-distribute-patterns" in line.split()
        is_memory_runtime = " -c kernel/src/libc/memory.c " in line
        if pattern_guard != is_memory_runtime:
            fail("MEMORY_PATTERN_GUARD_SCOPE", line)


def verify_host_memory(root: pathlib.Path) -> None:
    for name in ("memory-object-build", "memory-harness-build",
                 "memory-host"):
        exit_zero(root / "host" / f"{name}.exit-code.txt")
    log = require_file(root / "host/memory-host.log").read_text()
    match = re.fullmatch(
        r"BUILD_PROFILE_MEMORY_HOST: PASS sizes=4098 alignments=16 "
        r"memcpy_cases=1049088 memset_cases=196704 "
        r"memmove_cases=2098176 overlap_forward=1046656 "
        r"overlap_backward=1046656\n?",
        log,
    )
    if match is None:
        fail("MEMORY_HOST_RESULT", log.strip())


def verify_build(root: pathlib.Path) -> None:
    for name in ("deps-check", "kernel-check", "kernel-check-all-tests",
                 "stack-check", "image"):
        exit_zero(root / "build" / f"{name}.exit-code.txt")
    exit_zero(root / "build/section-layout-candidate.exit-code.txt")
    verify_compiler_log(root / "build/kernel-check.log")
    verify_compiler_log(root / "build/kernel-check-all-tests.log")
    verify_host_memory(root)
    stack = require_file(root / "build/stack-check.log").read_text()
    if "violations=0" not in stack or "stack-check: PASS" not in stack:
        fail("STACK_GATE", "terminal markers missing")
    memory_disassembly = require_file(
        root / "candidate/memory.disassembly.txt").read_text().lower()
    if re.search(r"\b(?:xmm|ymm|zmm)[0-9]+\b", memory_disassembly):
        fail("MEMORY_VECTOR_REGISTER", "SSE/AVX instruction emitted")
    for symbol in ("memcpy", "memset", "memmove"):
        start = memory_disassembly.find(f"<{symbol}>:")
        if start < 0:
            fail("MEMORY_SYMBOL", symbol)
        following = [memory_disassembly.find(f"<{next_symbol}>:", start + 1)
                     for next_symbol in ("memset", "memmove", "memcmp")]
        following = [position for position in following if position > start]
        end = min(following) if following else len(memory_disassembly)
        body = memory_disassembly[start:end]
        if "qword ptr" not in body:
            fail("MEMORY_WORD_DISASSEMBLY", symbol)


def verify_runtime(root: pathlib.Path) -> tuple[dict[str, int], dict[str, int]]:
    profile = root / "runtime/q35-kvm-smp4"
    command_lines = require_file(profile / "commands.jsonl").read_text().splitlines()
    if len(command_lines) != 2:
        fail("COMMAND_COUNT", str(len(command_lines)))
    commands = [json.loads(line) for line in command_lines]
    for sequence, (command, payload) in enumerate(zip(
            commands, ("profiletest abi", "profiletest memory")), start=1):
        if command.get("classification") != "PASS" or \
                command.get("handler_status") != 0 or \
                command.get("disposition") != "EXECUTED" or \
                command.get("sequence") != sequence or \
                not command.get("guest_input_text", "").endswith(payload):
            fail("COMMAND_RECORD", json.dumps(command, sort_keys=True))
    env = require_file(profile / "profile.env").read_text()
    if "cleanup=PASS" not in env or "run_complete=YES" not in env:
        fail("QEMU_CLEANUP")
    serial = profile / "qemu/qemu-serial.log"
    return verify_abi_record(serial), verify_memory_record(serial)


def verify_production(root: pathlib.Path) -> None:
    exit_zero(root / "production/production-image.exit-code.txt")
    exit_zero(root / "production/policy.exit-code.txt")
    exit_zero(root / "production/section-layout-production.exit-code.txt")
    verify_compiler_log(root / "production/production-image.log")
    for name in ("kernel.strings.txt", "kernel.symbols.txt"):
        text = require_file(root / "production" / name).read_text(
            encoding="utf-8", errors="replace").lower()
        if "profiletest" in text:
            fail("PRODUCTION_SELFTEST_PRESENT", name)
    if "[BOOT][PRODUCTION_TEST_POLICY] PASS" not in require_file(
            root / "production/policy.log").read_text():
        fail("PRODUCTION_POLICY")


def fixture_line(**changes: int) -> str:
    values = {
        "bsp_seen": 1, "bsp_rsp_mod16": 8, "bsp_df": 0,
        "ap_expected": 3, "ap_seen": 3, "ap_bad_rsp": 0,
        "ap_bad_df": 0, "irq_entries": 100, "irq_bad_df": 0,
        "probe_vector": 35, "probe_entries_delta": 1,
        "probe_bad_df_delta": 0, "restored_df": 1, "final_df": 0,
    }
    values.update(changes)
    return ABI_MARKER + " PASS " + " ".join(
        f"{key}={value}" for key, value in values.items()) + "\n"


def memory_fixture_line(**changes: int) -> str:
    overlap_cases = runtime_overlap_cases()
    values = {
        "cases": len(RUNTIME_MEMORY_SIZES) * (16 * 16 * 3 + 16 * 3),
        "max_size": 4097,
        "alignments": 16,
        "memcpy_failures": 0,
        "memset_failures": 0,
        "memmove_failures": 0,
        "overlap_forward": overlap_cases,
        "overlap_backward": overlap_cases,
        "benchmark_bytes": 8 * 1024 * 1024,
        "samples": 7,
        "median_cycles": 1000000,
        "checksum": 123456789,
        "heap_ok": 1,
    }
    values.update(changes)
    return MEMORY_MARKER + " PASS " + " ".join(
        f"{key}={value}" for key, value in values.items()) + "\n"


def create_fixtures(root: pathlib.Path) -> dict[str, object]:
    root.mkdir(parents=True, exist_ok=False)
    valid = root / "valid-runtime.log"
    bad_stack = root / "misaligned-bsp.log"
    bad_df = root / "direction-flag-set.log"
    valid_memory = root / "valid-memory.log"
    bad_memory = root / "memory-corruption.log"
    bad_benchmark = root / "memory-zero-cycles.log"
    valid.write_text(fixture_line())
    bad_stack.write_text(fixture_line(bsp_rsp_mod16=0))
    bad_df.write_text(fixture_line(irq_bad_df=1,
                                   probe_bad_df_delta=1))
    valid_memory.write_text(memory_fixture_line())
    bad_memory.write_text(memory_fixture_line(memmove_failures=1))
    bad_benchmark.write_text(memory_fixture_line(median_cycles=0))
    verify_abi_record(valid)
    verify_memory_record(valid_memory)
    rejected: dict[str, str] = {}
    for name, verifier, path, expected in (
        ("misaligned-bsp", verify_abi_record, bad_stack,
         "BSP_STACK_ALIGNMENT"),
        ("direction-flag-set", verify_abi_record, bad_df,
         "DIRECTION_FLAG_NOT_CLEAR"),
        ("memory-corruption", verify_memory_record, bad_memory,
         "MEMORY_REFERENCE_MISMATCH"),
        ("memory-zero-cycles", verify_memory_record, bad_benchmark,
         "MEMORY_BENCHMARK"),
    ):
        try:
            verifier(path)
        except ValidationError as error:
            if error.code != expected:
                raise
            rejected[name] = error.code
        else:
            fail("NEGATIVE_ACCEPTED", name)
    result: dict[str, object] = {
        "schema": 1, "status": "PASS", "negative_fixtures": 4,
        "rejected": rejected,
    }
    (root / "result.json").write_text(json.dumps(result, indent=2,
                                                   sort_keys=True) + "\n")
    return result


def verify_all(root: pathlib.Path) -> dict[str, object]:
    source = root / "source"
    verify_manifest(source)
    verify_source(source)
    verify_build(root)
    abi_fields, memory_fields = verify_runtime(root)
    verify_production(root)
    fixture = json.loads(require_file(root / "fixtures/result.json").read_text())
    if fixture.get("status") != "PASS" or fixture.get("negative_fixtures") != 4:
        fail("FIXTURE_RESULT")
    result: dict[str, object] = {
        "schema": 1,
        "status": "PASS",
        "runtime_profile": "q35-kvm-smp4",
        "ap_count": abi_fields["ap_seen"],
        "irq_entries": abi_fields["irq_entries"],
        "memory_cases": memory_fields["cases"],
        "memory_median_cycles": memory_fields["median_cycles"],
        "memory_checksum": memory_fields["checksum"],
        "negative_fixtures": 4,
    }
    (root / "verify-result.json").write_text(json.dumps(
        result, indent=2, sort_keys=True) + "\n")
    return result


def main() -> int:
    if len(sys.argv) < 3:
        print("usage: verify-build-profile.py source|record|fixtures|all PATH",
              file=sys.stderr)
        return 2
    mode = sys.argv[1]
    path = pathlib.Path(sys.argv[2]).resolve()
    try:
        if mode == "source":
            result = verify_source(path)
            print(f"BUILD_PROFILE_SOURCE: PASS checks={sum(result.values())}")
        elif mode == "record":
            abi_fields = verify_abi_record(path)
            memory_fields = verify_memory_record(path)
            print("BUILD_PROFILE_RECORD: PASS "
                  f"aps={abi_fields['ap_seen']} "
                  f"irq_entries={abi_fields['irq_entries']} "
                  f"memory_cases={memory_fields['cases']} "
                  f"median_cycles={memory_fields['median_cycles']}")
        elif mode == "fixtures":
            result = create_fixtures(path)
            print("BUILD_PROFILE_FIXTURES: PASS "
                  f"negatives={result['negative_fixtures']}")
        elif mode == "all":
            result = verify_all(path)
            print("BUILD_PROFILE_VERIFY: PASS "
                  f"aps={result['ap_count']} negatives={result['negative_fixtures']}")
        else:
            return 2
    except (ValidationError, OSError, ValueError, json.JSONDecodeError) as error:
        if isinstance(error, ValidationError):
            print(f"BUILD_PROFILE_VERIFY: FAIL code={error.code} "
                  f"detail={error.detail}", file=sys.stderr)
        else:
            print(f"BUILD_PROFILE_VERIFY: FAIL code=EXCEPTION detail={error}",
                  file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
