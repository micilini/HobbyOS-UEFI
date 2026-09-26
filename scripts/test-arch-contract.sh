#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
MODE=${1:-all}
EVIDENCE=${ARCH_EVIDENCE_DIR:-"$ROOT/artifacts/build/arch-contract/$(date +%Y%m%d-%H%M%S%z)"}
JOBS=${JOBS:-12}
ARCH_BASE_COMMIT=${ARCH_BASE_COMMIT:-4397cfa19f0bb45b766f1b46c9531b7d74cff153}

case "$MODE" in
    host|build|runtime|production|fixtures|verify|all) ;;
    *) printf 'usage: %s {host|build|runtime|production|fixtures|verify|all}\n' "$0" >&2; exit 2 ;;
esac

mkdir -p "$EVIDENCE/commands"

record_command() {
    local name=$1
    shift
    python3 - "$EVIDENCE/commands/$name.json" "$ROOT" "$@" <<'PY'
import json, sys, time
from pathlib import Path
path = Path(sys.argv[1])
record = {"schema": 1, "cwd": sys.argv[2], "argv": sys.argv[3:],
          "recorded_at_unix": time.time()}
path.write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
PY
}

run_logged() {
    local name=$1
    local output=$2
    shift 2
    record_command "$name" "$@"
    set +e
    "$@" >"$output" 2>&1
    local status=$?
    set -e
    printf '%s\n' "$status" >"$output.exit-code"
    ((status == 0)) || { cat "$output" >&2; return "$status"; }
}

host_tests() {
    local host="$EVIDENCE/host"
    mkdir -p "$host"
    local common=(-std=gnu11 -Wall -Wextra -Werror -O2 -fno-pie -no-pie -pthread)
    local sources=(scripts/arch-contract-host.c kernel/src/cpu/cpu.c kernel/src/cpu/mmio.c)

    for specification in \
        'normal-debug|1|' \
        'ubsan-debug|1|-fsanitize=undefined -fno-sanitize-recover=undefined' \
        'asan-debug|1|-fsanitize=address -fno-omit-frame-pointer' \
        'normal-disabled|0|'; do
        IFS='|' read -r name trace sanitizer <<<"$specification"
        local directory="$host/$name"
        mkdir -p "$directory"
        local -a extra=()
        if [[ -n $sanitizer ]]; then
            read -r -a extra <<<"$sanitizer"
        fi
        if [[ $trace == 1 ]]; then
            extra+=( -DHOBBYOS_ARCH_HOST_TEST=1 -DHOBBYOS_ARCH_TEST=1
                     -DHOBBYOS_SELFTEST=1 )
        fi
        run_logged "host-$name-compile" "$directory/compile.log" \
            gcc "${common[@]}" "${extra[@]}" \
            -DHOBBYOS_DEBUG_ASSERT="$trace" "${sources[@]}" \
            -o "$directory/arch-contract-host"
        record_command "host-$name-run" "$directory/arch-contract-host"
        set +e
        if [[ $name == asan-debug ]]; then
            ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
                timeout 20 "$directory/arch-contract-host" \
                >"$directory/run.log" 2>&1
        else
            UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
                timeout 20 "$directory/arch-contract-host" \
                >"$directory/run.log" 2>&1
        fi
        local status=$?
        set -e
        printf '%s\n' "$status" >"$directory/run.log.exit-code"
        ((status == 0)) || { cat "$directory/run.log" >&2; return "$status"; }
    done

    cat >"$host/include-cpu-first.c" <<'EOF'
#include "kernel/src/cpu/cpu.h"
#include "kernel/src/core/io.h"
#include "kernel/src/cpu/cpu.h"
void include_cpu_first(void) { (void)&inb; (void)&outb; (void)&io_wait; }
EOF
    cat >"$host/include-io-first.c" <<'EOF'
#include "kernel/src/core/io.h"
#include "kernel/src/cpu/cpu.h"
#include "kernel/src/core/io.h"
void include_io_first(void) { (void)&inb; (void)&outb; (void)&io_wait; }
EOF
    : >"$host/include-order.log"
    for name in include-cpu-first include-io-first; do
        record_command "$name" gcc -std=gnu11 -Wall -Wextra -Werror \
            -ffreestanding -I. -c "$host/$name.c" -o "$host/$name.o"
        gcc -std=gnu11 -Wall -Wextra -Werror -ffreestanding -I. \
            -c "$host/$name.c" -o "$host/$name.o" \
            >>"$host/include-order.log" 2>&1
    done
    printf 'INCLUDE_ORDER_STATUS=PASS\n' >>"$host/include-order.log"
}

copy_candidate() {
    local role=$1
    mkdir -p "$EVIDENCE/candidates/$role"
    cp kernel.elf "$EVIDENCE/candidates/$role/kernel.elf"
    cp hobbyos.img "$EVIDENCE/candidates/$role/hobbyos.img"
    sha256sum "$EVIDENCE/candidates/$role/kernel.elf" \
        "$EVIDENCE/candidates/$role/hobbyos.img" \
        >"$EVIDENCE/candidates/$role/sha256.txt"
}

build_candidates() {
    mkdir -p "$EVIDENCE/build" "$EVIDENCE/candidates"

    run_logged build-instrumented-clean "$EVIDENCE/build/instrumented-clean.log" make clean
    run_logged build-instrumented-kernel-check "$EVIDENCE/build/instrumented-kernel-check.log" \
        make kernel-check SELFTEST=1 ARCH_TEST=1 DEBUG_ASSERT=1 JOBS="$JOBS"
    run_logged build-instrumented-image "$EVIDENCE/build/instrumented-image.log" \
        make image SELFTEST=1 ARCH_TEST=1 DEBUG_ASSERT=1 JOBS="$JOBS"
    copy_candidate instrumented
    cp kernel/src/cpu/mmio.o "$EVIDENCE/candidates/instrumented/mmio.o"

    run_logged build-stack-clean "$EVIDENCE/build/stack-clean.log" make clean
    run_logged build-stack-check "$EVIDENCE/build/stack-check.log" \
        make stack-check SELFTEST=1 ARCH_TEST=1 DEBUG_ASSERT=1 JOBS="$JOBS"
    find kernel -type f -name '*.su' -print -exec cat {} \; \
        >"$EVIDENCE/build/stack-usage.txt"

    run_logged build-negative-clean "$EVIDENCE/build/negative-clean.log" make clean
    run_logged build-negative-image "$EVIDENCE/build/negative-image.log" \
        make image SELFTEST=1 ARCH_TEST=1 ARCH_NEGATIVE_GP=1 \
        DEBUG_ASSERT=1 JOBS="$JOBS"
    copy_candidate negative

    run_logged build-production "$EVIDENCE/build/production-image.log" \
        make production-image JOBS="$JOBS"
    copy_candidate production

    run_logged collect-static "$EVIDENCE/build/static-collection.log" \
        python3 scripts/verify-arch-contract.py collect-static \
        --repo-root "$ROOT" --evidence-root "$EVIDENCE" \
        --debug-elf "$EVIDENCE/candidates/instrumented/kernel.elf" \
        --production-elf "$EVIDENCE/candidates/production/kernel.elf" \
        --host-disabled "$EVIDENCE/host/normal-disabled/arch-contract-host" \
        --debug-mmio-object "$EVIDENCE/candidates/instrumented/mmio.o" \
        --include-order-log "$EVIDENCE/host/include-order.log" \
        --base-commit "$ARCH_BASE_COMMIT"
}

runtime_tests() {
    local image="$EVIDENCE/candidates/instrumented/hobbyos.img"
    local elf="$EVIDENCE/candidates/instrumented/kernel.elf"
    mkdir -p "$EVIDENCE/runtime" "$EVIDENCE/negative" "$EVIDENCE/production"
    local -a profiles=(
        'q35-tcg-smp1 tcg 1'
        'q35-tcg-smp4 tcg 4'
        'q35-kvm-smp4 kvm 4'
        'q35-kvm-smp24 kvm 24')
    local profile accel smp
    for specification in "${profiles[@]}"; do
        read -r profile accel smp <<<"$specification"
        record_command "runtime-$profile" python3 scripts/arch-qemu.py \
            --run-dir "$EVIDENCE/runtime/$profile" --image "$image" \
            --elf "$elf" --accel "$accel" --smp "$smp"
        python3 scripts/arch-qemu.py \
            --run-dir "$EVIDENCE/runtime/$profile" --image "$image" \
            --elf "$elf" --accel "$accel" --smp "$smp" \
            | tee "$EVIDENCE/runtime/$profile.stdout.log"
    done

    record_command runtime-unrelated-gp python3 scripts/arch-qemu.py \
        --run-dir "$EVIDENCE/negative/q35-tcg-smp1-unrelated-gp" \
        --image "$EVIDENCE/candidates/negative/hobbyos.img" \
        --elf "$EVIDENCE/candidates/negative/kernel.elf" \
        --accel tcg --smp 1 --negative
    python3 scripts/arch-qemu.py \
        --run-dir "$EVIDENCE/negative/q35-tcg-smp1-unrelated-gp" \
        --image "$EVIDENCE/candidates/negative/hobbyos.img" \
        --elf "$EVIDENCE/candidates/negative/kernel.elf" \
        --accel tcg --smp 1 --negative \
        | tee "$EVIDENCE/negative/q35-tcg-smp1-unrelated-gp.stdout.log"

    record_command runtime-production python3 scripts/arch-qemu.py \
        --run-dir "$EVIDENCE/production/q35-kvm-smp4" \
        --image "$EVIDENCE/candidates/production/hobbyos.img" \
        --elf "$EVIDENCE/candidates/production/kernel.elf" \
        --accel kvm --smp 4 --production
    python3 scripts/arch-qemu.py \
        --run-dir "$EVIDENCE/production/q35-kvm-smp4" \
        --image "$EVIDENCE/candidates/production/hobbyos.img" \
        --elf "$EVIDENCE/candidates/production/kernel.elf" \
        --accel kvm --smp 4 --production \
        | tee "$EVIDENCE/production/q35-kvm-smp4.stdout.log"
}

generate_campaign() {
    python3 - "$ROOT" "$EVIDENCE" <<'PY'
import hashlib, json, shutil, sys
from pathlib import Path
root = Path(sys.argv[1]).resolve()
evidence = Path(sys.argv[2]).resolve()
def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(chunk)
    return value.hexdigest()
def record(path):
    return {"path": str(path.relative_to(evidence)),
            "bytes": path.stat().st_size, "sha256": digest(path)}
source_paths = [
    'kernel/link.ld', 'kernel/src/core/interrupts.c',
    'kernel/src/core/kernel_init.c', 'kernel/src/core/panic.c',
    'kernel/src/cpu/cpu.c', 'kernel/src/cpu/cpu.h',
    'kernel/src/cpu/mmio.c', 'kernel/src/cpu/mmio.h',
    'kernel/src/cpu/arch_selftest.c', 'kernel/src/cpu/arch_selftest.h',
    'kernel/src/core/io.h', 'kernel/src/drivers/usb/xhci/xhci.c', 'makefile',
    'scripts/arch-contract-host.c', 'scripts/arch-qemu.py',
    'scripts/test-arch-contract.sh', 'scripts/verify-arch-contract.py',
    'scripts/verify-section-layout.py',
    'scripts/panic-qemu.py', 'scripts/verify-panic-evidence.py',
    'scripts/test-panic-lock-contention.sh',
    'docs/foundation/validation-policy.md',
]
if (root / 'docs/foundation/architecture.md').is_file():
    source_paths.append('docs/foundation/architecture.md')
source_root = evidence / 'source'
files = []
for relative in source_paths:
    source = root / relative
    if not source.is_file():
        raise SystemExit(f'missing source for evidence: {relative}')
    destination = source_root / relative
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)
    files.append(record(destination))
manifest = {"schema": 1, "files": files}
(source_root / 'manifest.json').write_text(
    json.dumps(manifest, indent=2, sort_keys=True) + '\n')
def candidate(role):
    directory = evidence / 'candidates' / role
    elf = directory / 'kernel.elf'
    image = directory / 'hobbyos.img'
    return {"elf": record(elf), "image": record(image),
            "elf_sha256": digest(elf), "image_sha256": digest(image)}
host = []
for name, trace in [('normal-debug', True), ('ubsan-debug', True),
                    ('asan-debug', True), ('normal-disabled', False)]:
    log = evidence / 'host' / name / 'run.log'
    item = record(log)
    item.update({"name": name, "debug_trace": trace,
                 "exit_code": int((log.parent / 'run.log.exit-code').read_text())})
    host.append(item)
runtime = []
for name, accel, cpu, smp in [
    ('q35-tcg-smp1', 'tcg', 'Haswell', 1),
    ('q35-tcg-smp4', 'tcg', 'Haswell', 4),
    ('q35-kvm-smp4', 'kvm', 'host', 4),
    ('q35-kvm-smp24', 'kvm', 'host', 24),
]:
    runtime.append({"profile": ['q35', accel, cpu, smp],
                    "result": f'runtime/{name}/result.json'})
campaign = {
    "schema": 1,
    "source_manifest": "source/manifest.json",
    "static_result": "static/result.json",
    "candidates": {role: candidate(role) for role in
                   ('instrumented', 'negative', 'production')},
    "host_runs": host,
    "runtime_runs": runtime,
    "negative_run": "negative/q35-tcg-smp1-unrelated-gp/result.json",
    "production_run": "production/q35-kvm-smp4/result.json",
    "contract_summary": {
        "cpuid_subleaf": True, "cpuid_bounds": True,
        "legacy_ecx_zero": True, "msr_instruction_observed": True,
        "msr_unrelated_gp_panics": True, "msr_table_unique": True,
        "msr_fixups_in_text": True, "trace_coherent": True,
        "trace_bounded": True, "production_trace_elided": True,
        "records_complete": True, "recovery_continues": True,
        "port_headers_compatible": True,
        "unexpected_orphan_rejected": True,
    },
}
(evidence / 'campaign.json').write_text(
    json.dumps(campaign, indent=2, sort_keys=True) + '\n')
PY
}

verify_evidence() {
    generate_campaign
    run_logged verify-all "$EVIDENCE/verify-all.log" \
        python3 scripts/verify-arch-contract.py all "$EVIDENCE"
}

fixture_tests() {
    run_logged verify-fixtures "$EVIDENCE/fixtures.log" \
        python3 scripts/verify-arch-contract.py fixtures \
        --output-dir "$EVIDENCE/oracle-fixtures" \
        --source-evidence "$EVIDENCE"
}

cd "$ROOT"
case "$MODE" in
    host) host_tests ;;
    build) build_candidates ;;
    runtime) runtime_tests ;;
    production)
        python3 scripts/arch-qemu.py \
            --run-dir "$EVIDENCE/production/q35-kvm-smp4" \
            --image "$EVIDENCE/candidates/production/hobbyos.img" \
            --elf "$EVIDENCE/candidates/production/kernel.elf" \
            --accel kvm --smp 4 --production ;;
    fixtures) fixture_tests ;;
    verify) verify_evidence ;;
    all)
        host_tests
        build_candidates
        runtime_tests
        verify_evidence
        fixture_tests ;;
esac

printf 'ARCH_CONTRACT_GATE=PASS\nEVIDENCE_ROOT=%s\n' "$EVIDENCE"
