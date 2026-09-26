#!/usr/bin/env bash
set -Eeuo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

timestamp=$(date '+%Y%m%d-%H%M%S-%z')
evidence=${ASSERT_EVIDENCE_DIR:-"artifacts/build/runtime-assertions/$timestamp"}
base_commit=${ASSERT_BASE_COMMIT:-$(git rev-parse HEAD)}
jobs=${JOBS:-2}
host_cc=${CC:-gcc}
lock_held=0

usage()
{
    echo "usage: $0 preflight|host|static|build|runtime|production|fixtures|verify|all" >&2
    exit 2
}

cleanup()
{
    local status=0
    if ((lock_held)); then
        exec 9>&-
        lock_held=0
    fi
    if [[ -r .qemu/qemu.pid ]]; then
        echo "assertion gate did not acquire the shared QEMU runtime PID" >&2
        status=1
    fi
    return "$status"
}
trap cleanup EXIT

ensure_evidence()
{
    [[ ! -L $evidence ]] || {
        echo "assertion evidence root must not be a symlink" >&2
        return 1
    }
    mkdir -p "$evidence" "$evidence/commands"
}

record_command()
{
    local label=$1
    shift
    ensure_evidence
    {
        printf '%s' "$label"
        printf '\t%q' "$@"
        printf '\n'
    } >>"$evidence/commands/commands.tsv"
}

run_logged()
{
    local label=$1 log=$2
    shift 2
    local rc
    mkdir -p "$(dirname "$log")"
    record_command "$label" "$@"
    set +e
    "$@" >"$log" 2>&1
    rc=$?
    set -e
    printf '%s\n' "$rc" >"${log%.log}.exit-code.txt"
    if ((rc != 0)); then
        tail -120 "$log" >&2
        return "$rc"
    fi
}

preflight()
{
    ensure_evidence
    mkdir -p "$evidence/preflight"
    python3 - "$evidence/preflight/environment.json" "$root" "$base_commit" <<'PY'
import datetime
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys

def command(*argv):
    try:
        return subprocess.check_output(argv, text=True,
                                       stderr=subprocess.STDOUT).strip()
    except (OSError, subprocess.CalledProcessError) as error:
        return f"UNAVAILABLE: {error}"

tools = {}
for name, argv in {
    "gcc": ("gcc", "--version"), "ld": ("ld", "--version"),
    "make": ("make", "--version"), "python": ("python3", "--version"),
    "qemu": ("qemu-system-x86_64", "--version"),
    "gdb": ("gdb", "--version"), "mtools": ("mcopy", "-V"),
}.items():
    result = command(*argv)
    tools[name] = {"path": shutil.which(argv[0]),
                   "version": result.splitlines()[0] if result else ""}
status = command("git", "status", "--short")
payload = {
    "schema": 1,
    "observed_at": datetime.datetime.now(datetime.timezone.utc).astimezone().isoformat(),
    "delivery_dir": str(Path(sys.argv[2]).resolve()),
    "repo_root": command("git", "rev-parse", "--show-toplevel"),
    "branch": command("git", "branch", "--show-current"),
    "head": command("git", "rev-parse", "HEAD"),
    "tree": command("git", "rev-parse", "HEAD^{tree}"),
    "parent": command("git", "rev-parse", "HEAD^"),
    "base_for_scope_comparison": sys.argv[3],
    "worktree_status": status.splitlines() if status else [],
    "host": platform.platform(),
    "schedulable_cpus": len(os.sched_getaffinity(0)),
    "kvm_readable_writable": os.access("/dev/kvm", os.R_OK | os.W_OK),
    "tools": tools,
}
Path(sys.argv[1]).write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
PY
    sha256sum AGENTS.md docs/foundation/validation-policy.md \
        docs/foundation/panic.md docs/foundation/formatting.md \
        >"$evidence/preflight/contracts.sha256"
    for tool in gcc ld make python3 qemu-system-x86_64 gdb mcopy \
                nm objdump readelf size strings; do
        command -v "$tool" >/dev/null
    done
    if make -n kernel.elf ASSERT_TEST=1 SELFTEST=0 \
        >"$evidence/preflight/assert-without-selftest.log" 2>&1; then
        echo "ASSERT_TEST without SELFTEST was accepted" >&2
        return 1
    fi
    grep -Fq 'ASSERT_TEST=1 requires SELFTEST=1' \
        "$evidence/preflight/assert-without-selftest.log"
    if make -n kernel.elf ASSERT_TEST=1 SELFTEST=1 DEBUG_ASSERT=0 \
        >"$evidence/preflight/assert-without-debug.log" 2>&1; then
        echo "ASSERT_TEST without DEBUG_ASSERT was accepted" >&2
        return 1
    fi
    grep -Fq 'ASSERT_TEST=1 requires DEBUG_ASSERT=1' \
        "$evidence/preflight/assert-without-debug.log"
    if make -n kernel.elf DEBUG_ASSERT=2 \
        >"$evidence/preflight/invalid-debug-value.log" 2>&1; then
        echo "invalid DEBUG_ASSERT value was accepted" >&2
        return 1
    fi
    grep -Fq 'DEBUG_ASSERT must be 0 or 1' \
        "$evidence/preflight/invalid-debug-value.log"
    echo "ASSERT_PREFLIGHT: PASS evidence=$evidence"
}

compile_host_profile()
{
    local name=$1 debug=$2 sanitizer=$3
    shift 3
    local directory="$evidence/host/$name" binary="$evidence/host/$name/assert-list-host"
    local rc
    local -a command=(
        "$host_cc" -std=gnu11 -Wall -Wextra -Werror -g
        -DHOBBYOS_DEBUG_ASSERT="$debug" "$@"
        scripts/assert-list-host.c -o "$binary"
    )
    mkdir -p "$directory"
    run_logged "host-$name-compile" "$directory/compile.log" "${command[@]}"
    record_command "host-$name-run" timeout 30 "$binary"
    set +e
    if [[ $sanitizer == asan ]]; then
        ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
            timeout 30 "$binary" >"$directory/run.log" 2>&1
        rc=$?
    else
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
            timeout 30 "$binary" >"$directory/run.log" 2>&1
        rc=$?
    fi
    set -e
    printf '%s\n' "$rc" >"$directory/exit-code.txt"
    ((rc == 0)) || {
        cat "$directory/run.log" >&2
        return "$rc"
    }
    grep -Eq "^\[ASSERT_HOST\]\[SUITE\] status=PASS assertions=[1-9][0-9]* failures=0 seed=0x6173736572746c69 sequences=2048 debug=$debug$" \
        "$directory/run.log"
}

host_tests()
{
    ensure_evidence
    compile_host_profile normal 1 none
    compile_host_profile ubsan 1 ubsan \
        -fsanitize=undefined -fno-sanitize-recover=all
    compile_host_profile asan 1 asan \
        -fsanitize=address -fno-omit-frame-pointer
    compile_host_profile disabled 0 none

    local disabled="$evidence/host/disabled-object"
    mkdir -p "$disabled"
    cat >"$disabled/probe.c" <<'C'
#include "kernel/src/core/list.h"
void disabled_assert_probe(struct list_head *node, struct list_head *head,
                           int *effect __attribute__((unused)))
{
    list_add(node, head);
    KWARN_ON(++*effect);
    KBUG_ON(++*effect);
}
C
    run_logged host-disabled-object "$disabled/compile.log" \
        "$host_cc" -std=gnu11 -Wall -Wextra -Werror -O0 -I. \
        -DHOBBYOS_DEBUG_ASSERT=0 -c "$disabled/probe.c" \
        -o "$disabled/probe.o"
    set +e
    nm -u "$disabled/probe.o" | \
        grep -E 'assertion_(warn|bug)_report' \
        >"$disabled/helper-references.txt"
    local helper_scan_rc=${PIPESTATUS[1]}
    strings "$disabled/probe.o" | \
        grep -E 'new_node == NULL|prev->next != next|\[ASSERT\]' \
        >"$disabled/callsite-strings.txt"
    local callsite_scan_rc=${PIPESTATUS[1]}
    set -e
    ((helper_scan_rc == 1 && callsite_scan_rc == 1))
    [[ ! -s $disabled/helper-references.txt &&
       ! -s $disabled/callsite-strings.txt ]]

    local adversarial="$evidence/host/adversarial"
    mkdir -p "$adversarial/include"
    cp kernel/src/core/panic.h "$adversarial/include/panic.h"
    python3 - kernel/src/core/list.h "$adversarial/include/list.h" <<'PY'
from pathlib import Path
import sys
source = Path(sys.argv[1]).read_text()
needle = "    KBUG_ON(prev->next != next);\n"
if source.count(needle) != 1:
    raise SystemExit("insertion reciprocity check was not unique")
Path(sys.argv[2]).write_text(source.replace(needle, "    ((void)0);\n", 1))
PY
    run_logged host-adversarial-compile "$adversarial/compile.log" \
        "$host_cc" -std=gnu11 -Wall -Wextra -Werror -g \
        -DHOBBYOS_DEBUG_ASSERT=1 -DASSERT_LIST_NO_QUEUE=1 \
        -DASSERT_LIST_HEADER=\"list.h\" -I"$adversarial/include" \
        scripts/assert-list-host.c -o "$adversarial/assert-list-host"
    record_command host-adversarial-run timeout 30 \
        "$adversarial/assert-list-host"
    set +e
    timeout 30 "$adversarial/assert-list-host" \
        >"$adversarial/run.log" 2>&1
    local adversarial_rc=$?
    set -e
    printf '%s\n' "$adversarial_rc" >"$adversarial/exit-code.txt"
    ((adversarial_rc != 0 && adversarial_rc != 124))
    grep -Fq 'broken previous reciprocity accepted' "$adversarial/run.log"

    python3 - "$evidence/host/summary.json" "$evidence/host" \
        "$adversarial_rc" <<'PY'
import hashlib
import json
from pathlib import Path
import sys

output, root, adversarial_rc = Path(sys.argv[1]), Path(sys.argv[2]), int(sys.argv[3])
def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()
profiles = {name: {"log_sha256": digest(root / name / "run.log")}
            for name in ("normal", "ubsan", "asan", "disabled")}
payload = {
    "schema": 1, "status": "PASS", "profiles": profiles,
    "adversarial": {
        "status": "NEGATIVE_DETECTED", "exit_code": adversarial_rc,
        "mutation": "removed insertion previous-neighbor reciprocity check",
        "log_sha256": digest(root / "adversarial" / "run.log"),
    },
}
output.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
PY
    echo "ASSERT_HOST: PASS profiles=4 adversarial=NEGATIVE_DETECTED"
}

static_checks()
{
    ensure_evidence
    mkdir -p "$evidence/static"
    bash -n scripts/test-assert-list.sh
    PYTHONDONTWRITEBYTECODE=1 python3 - <<'PY'
from pathlib import Path
for path in (Path("scripts/assert-qemu.py"),
             Path("scripts/verify-assert-list-evidence.py")):
    compile(path.read_text(), str(path), "exec")
PY
    git diff --check
    rg -n '#if HOBBYOS_DEBUG_ASSERT|KBUG_ON\(|KWARN_ON\(' \
        kernel/src/core/{panic.h,list.h} >"$evidence/static/assertion-sites.txt"
    rg -n 'list_(add|add_tail|del|init)\(' kernel/src \
        --glob '*.[ch]' >"$evidence/static/list-consumers.txt"
    rg -n 'list_del\(&node->node\)|list_add_tail\(&node->node, &g_claimed_list\)' \
        kernel/src/core/timers.c >"$evidence/static/timer-transfer.txt"
    git show "$base_commit:kernel/src/core/queue.h" \
        >"$evidence/static/queue-base.h"
    cmp -s "$evidence/static/queue-base.h" kernel/src/core/queue.h
    python3 - "$base_commit" "$evidence/static/scope.json" <<'PY'
import hashlib
import json
from pathlib import Path
import subprocess
import sys
base = sys.argv[1]
unchanged = [
    "kernel/src/core/queue.h",
    "kernel/src/drivers/serial.c", "kernel/src/core/scheduler.c",
    "kernel/src/core/timers.c", "kernel/src/core/dpc.c",
    "kernel/src/memory/heap.c", "kernel/src/libc/string.c",
]
rows = []
for name in unchanged:
    old = subprocess.check_output(["git", "show", f"{base}:{name}"])
    new = Path(name).read_bytes()
    rows.append({"path": name, "unchanged": old == new,
                 "sha256": hashlib.sha256(new).hexdigest()})
if not all(row["unchanged"] for row in rows):
    raise SystemExit("protected source changed")
interrupts = Path("kernel/src/core/interrupts.c").read_bytes()
interrupts_sha256 = hashlib.sha256(interrupts).hexdigest()
expected_architecture_sha256 = (
    "5e1928c11acdbfef27f2fbcdd02359036c2b2817d38ece2d691bd45b24a5e429"
)
integration_tokens = (
    b"static __attribute__((noinline)) bool general_protection_msr_fixup",
    b"cpu_msr_fixup_lookup",
    b"#ifdef HOBBYOS_ARCH_TEST",
    b"arch_test_timer_hook",
    b"uint64_t irq_lapic_timer_handler_inner(void)",
    b"return now_ns;",
    b"void irq_external_dispatch(uint64_t vector_value, uint64_t entry_df)",
    b"build_profile_record_irq_entry(vector, entry_df)",
    b"scheduler_preempt_from_irq(now_ns)",
    b"arch_selftest_record_unhandled_gp(frame->rip, error_code, frame->cs);",
)
if interrupts_sha256 != expected_architecture_sha256 or not all(
        token in interrupts for token in integration_tokens) or interrupts.count(
            b"arch_selftest_record_unhandled_gp(frame->rip, error_code, "
            b"frame->cs);") != 2:
    raise SystemExit("architecture interrupt integration is incomplete")
rows.append({
    "path": "kernel/src/core/interrupts.c",
    "unchanged": False,
    "authorized_architecture_integration": True,
    "required_tokens": [token.decode() for token in integration_tokens],
    "unhandled_gp_recorder_call_count": 2,
    "sha256": interrupts_sha256,
})
Path(sys.argv[2]).write_text(json.dumps({"schema": 1, "status": "PASS",
                                        "files": rows},
                                       indent=2, sort_keys=True) + "\n")
PY
    echo "ASSERT_STATIC: PASS"
}

extract_payloads()
{
    local image=$1 destination=$2
    mkdir -p "$destination"
    mcopy -o -i "$image" ::/EFI/BOOT/BOOTX64.EFI "$destination/BOOTX64.EFI"
    mcopy -o -i "$image" ::/kernel.elf "$destination/kernel.elf"
    mcopy -o -i "$image" ::/EFI/fonts/zap-light16.psf "$destination/zap-light16.psf"
    mcopy -o -i "$image" ::/EFI/images/logo.bmp "$destination/logo.bmp"
    mcopy -o -i "$image" ::/startup.nsh "$destination/startup.nsh"
}

build_candidate()
{
    ensure_evidence
    local build="$evidence/build" candidate="$evidence/builds/instrumented"
    mkdir -p "$build" "$candidate"
    run_logged kernel-check "$build/kernel-check-command.log" \
        make kernel-check JOBS="$jobs" DEBUG_ASSERT=1
    cp "artifacts/build/kernel-check-j${jobs}.log" \
        "$build/kernel-check-canonical.log"
    run_logged stack-check "$build/stack-check-command.log" \
        make stack-check JOBS="$jobs" DEBUG_ASSERT=1
    cp "artifacts/build/kernel-check-j${jobs}.log" \
        "$build/stack-check-kernel-canonical.log"
    cp artifacts/build/stack-usage-report.txt "$build/stack-usage-report.txt"
    grep -Fxq 'stack-check: PASS' "$build/stack-check-command.log"
    grep -Fxq 'violations=0' "$build/stack-usage-report.txt"

    run_logged assert-stack-kernel "$build/assert-stack-kernel.log" \
        make kernel-check JOBS="$jobs" SELFTEST=1 SELFTEST_AUTORUN=0 \
        ASSERT_TEST=1 DEBUG_ASSERT=1 \
        KERNEL_EXTRA_CFLAGS='-fstack-usage -DHOBBYOS_PANIC_TEST'
    run_logged assert-stack-verify "$build/assert-stack-verify.log" \
        python3 scripts/check-stack-usage.py --limit 2048 \
        --report "$build/assert-stack-usage-report.txt"
    grep -Fxq 'stack-check: PASS' "$build/assert-stack-verify.log"
    grep -Fxq 'violations=0' "$build/assert-stack-usage-report.txt"

    run_logged instrumented-clean "$build/instrumented-clean.log" make clean
    run_logged instrumented-image "$build/instrumented-image.log" \
        make -j"$jobs" image SELFTEST=1 SELFTEST_AUTORUN=0 ASSERT_TEST=1 \
        DEBUG_ASSERT=1 KERNEL_EXTRA_CFLAGS=-DHOBBYOS_PANIC_TEST
    cp kernel.elf hobbyos.img BOOTX64.EFI "$candidate/"
    extract_payloads "$candidate/hobbyos.img" "$candidate/payloads"
    cmp -s "$candidate/kernel.elf" "$candidate/payloads/kernel.elf"
    cmp -s "$candidate/BOOTX64.EFI" "$candidate/payloads/BOOTX64.EFI"
    nm -u "$candidate/kernel.elf" >"$build/instrumented-undefined.txt"
    [[ ! -s $build/instrumented-undefined.txt ]]
    nm -an "$candidate/kernel.elf" >"$build/instrumented-symbols.txt"
    for symbol in assertion_warn_report assertion_bug_report \
                  assertion_test_timer_hook g_assertion_test_state \
                  g_assertion_test_fixture g_panic_test_state \
                  g_heap_lock g_scheduler_lock; do
        grep -Eq "[[:space:]]${symbol}$" "$build/instrumented-symbols.txt"
    done
    objdump -dr kernel/src/core/panic.o >"$build/panic-disassembly.txt"
    objdump -dr kernel/src/core/assert_selftest.o \
        >"$build/assert-selftest-disassembly.txt"
    ! rg -n 'kernel/src/core/(panic|list|assert_selftest|kernel_init)\.c:.*warning:' \
        "$build/kernel-check-canonical.log" \
        "$build/stack-check-kernel-canonical.log" \
        "$build/assert-stack-kernel.log" "$build/instrumented-image.log" \
        >"$build/related-warning-scan.txt"
    sha256sum "$candidate/kernel.elf" "$candidate/hobbyos.img" \
        "$candidate/BOOTX64.EFI" "$candidate"/payloads/* \
        >"$build/instrumented.sha256"
    python3 - "$build/result.json" "$candidate" "$build" <<'PY'
import hashlib
import json
from pathlib import Path
import re
import sys
candidate, build = Path(sys.argv[2]), Path(sys.argv[3])
report = (build / "assert-stack-usage-report.txt").read_text()
sizes = [int(match.group(1)) for match in
         map(lambda line: re.match(r"^\s*([0-9]+)\s", line), report.splitlines())
         if match]
def digest(path): return hashlib.sha256(path.read_bytes()).hexdigest()
Path(sys.argv[1]).write_text(json.dumps({
    "schema": 1, "status": "PASS", "kernel_check_exit": 0,
    "stack_check_exit": 0, "assert_stack_check_exit": 0,
    "max_automatic_frame": max(sizes, default=0),
    "candidate_elf_sha256": digest(candidate / "kernel.elf"),
    "candidate_image_sha256": digest(candidate / "hobbyos.img"),
}, indent=2, sort_keys=True) + "\n")
PY
    echo "ASSERT_BUILD: PASS"
}

write_runtime_plan()
{
    local plan="$evidence/runtime-plan.tsv"
    : >"$plan"
    local scenario lock slot
    local -a scenarios=(warn false valid-list double-insert insert-reciprocity \
                        remove-reciprocity bug locked-bug reentry second-reentry)
    for accel in tcg kvm; do
        for scenario in "${scenarios[@]}"; do
            lock=none
            [[ $scenario == locked-bug ]] && lock=heap
            case $scenario in
                warn) slot=1 ;;
                false) slot=2 ;;
                valid-list) slot=3 ;;
                double-insert) slot=0 ;;
                insert-reciprocity) slot=1 ;;
                remove-reciprocity) slot=2 ;;
                bug) slot=3 ;;
                locked-bug) slot=0 ;;
                reentry) slot=1 ;;
                second-reentry) slot=2 ;;
            esac
            printf 'q35-%s-smp4-%s-%s\t%s\t%s\tq35\t%s\t4\t%s\n' \
                "$accel" "$scenario" "$lock" "$scenario" "$lock" \
                "$accel" "$slot" >>"$plan"
        done
    done
    printf 'q35-tcg-smp1-bug-none\tbug\tnone\tq35\ttcg\t1\t0\n' >>"$plan"
    printf 'q35-kvm-smp24-warn-none\twarn\tnone\tq35\tkvm\t24\t23\n' >>"$plan"
    printf 'q35-kvm-smp24-valid-list-none\tvalid-list\tnone\tq35\tkvm\t24\t22\n' >>"$plan"
    printf 'q35-kvm-smp24-locked-bug-scheduler\tlocked-bug\tscheduler\tq35\tkvm\t24\t21\n' >>"$plan"
    printf 'q35-kvm-smp24-reentry-none\treentry\tnone\tq35\tkvm\t24\t20\n' >>"$plan"
}

write_campaign()
{
    python3 - "$evidence/campaign.json" "$evidence/runtime-plan.tsv" \
        "$evidence/builds/instrumented/kernel.elf" \
        "$evidence/builds/instrumented/hobbyos.img" <<'PY'
import hashlib
import json
from pathlib import Path
import sys
output, plan, elf, image = map(Path, sys.argv[1:])
def digest(path): return hashlib.sha256(path.read_bytes()).hexdigest()
rows = []
for line in plan.read_text().splitlines():
    name, scenario, lock, machine, accel, smp, slot = line.split("\t")
    rows.append({"name": name, "scenario": scenario, "lock": lock,
                 "trigger_slot": int(slot),
                 "profile": {"machine": machine, "accel": accel,
                             "smp": int(smp), "memory": "2G",
                             "cpu": "host" if accel == "kvm" else "max"}})
payload = {
    "schema": 1, "status": "PASS", "planned_runs": rows,
    "candidate": {"elf_path": "builds/instrumented/kernel.elf",
                  "image_path": "builds/instrumented/hobbyos.img",
                  "elf_sha256": digest(elf), "image_sha256": digest(image)},
}
output.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
PY
}

runtime_tests()
{
    ensure_evidence
    [[ -f $evidence/builds/instrumented/kernel.elf &&
       -f $evidence/builds/instrumented/hobbyos.img ]]
    [[ -r /dev/kvm && -w /dev/kvm ]] || {
        echo "ASSERT_RUNTIME: BLOCKED KVM unavailable" >&2
        return 3
    }
    mkdir -p .qemu "$evidence/runtime"
    exec 9>.qemu/harness.lock
    flock -n 9 || {
        echo "another HobbyOS QEMU harness is active" >&2
        return 1
    }
    lock_held=1
    [[ ! -e .qemu/qemu.pid && ! -S .qemu/hmp.sock ]]
    write_runtime_plan
    while IFS=$'\t' read -r name scenario lock machine accel smp slot; do
        local -a command=(
            python3 scripts/assert-qemu.py
            --run-dir "$evidence/runtime/$name"
            --image "$evidence/builds/instrumented/hobbyos.img"
            --elf "$evidence/builds/instrumented/kernel.elf"
            --scenario "$scenario" --lock "$lock" --machine "$machine"
            --accel "$accel" --smp "$smp" --trigger-slot "$slot"
            --boot-timeout 90 --post-timeout 30 --cleanup-timeout 4
        )
        record_command "runtime-$name" "${command[@]}"
        PYTHONDONTWRITEBYTECODE=1 "${command[@]}" \
            >"$evidence/commands/runtime-$name.log" 2>&1 || {
            cat "$evidence/commands/runtime-$name.log" >&2
            return 1
        }
        grep -Fq '"status": "PASS"' "$evidence/commands/runtime-$name.log"
    done <"$evidence/runtime-plan.tsv"
    exec 9>&-
    lock_held=0
    write_campaign
    echo "ASSERT_RUNTIME: PASS runs=$(wc -l <"$evidence/runtime-plan.tsv")"
}

collect_build_inspection()
{
    local directory=$1
    size -A "$directory/kernel.elf" >"$directory/size-A.txt"
    readelf -SW "$directory/kernel.elf" >"$directory/sections.txt"
    nm -an "$directory/kernel.elf" >"$directory/symbols.txt"
    strings "$directory/kernel.elf" >"$directory/strings.txt"
    objdump -dr kernel/src/core/panic.o >"$directory/panic-disassembly.txt"
    objdump -dr kernel/src/core/kernel_init.o \
        >"$directory/kernel-init-disassembly.txt"
    objdump -dr kernel/src/core/timers.o >"$directory/timers-disassembly.txt"
    objdump -dr kernel/src/core/scheduler.o \
        >"$directory/scheduler-disassembly.txt"
    objdump -dr kernel/src/memory/heap.o >"$directory/heap-disassembly.txt"
}

production_tests()
{
    ensure_evidence
    local production="$evidence/production"
    mkdir -p "$production/debug-on" "$production/debug-off" \
        "$production/final/payloads"
    run_logged debug-on-clean "$production/debug-on/clean.log" make clean
    run_logged debug-on-build "$production/debug-on/build.log" \
        make -j"$jobs" kernel.elf SELFTEST=0 SELFTEST_AUTORUN=0 \
        FORMAT_TEST=0 ASSERT_TEST=0 DEBUG_ASSERT=1 KERNEL_EXTRA_CFLAGS=
    cp kernel.elf "$production/debug-on/kernel.elf"
    collect_build_inspection "$production/debug-on"

    run_logged debug-off-clean "$production/debug-off/clean.log" make clean
    run_logged debug-off-build "$production/debug-off/build.log" \
        make -j"$jobs" kernel.elf SELFTEST=0 SELFTEST_AUTORUN=0 \
        FORMAT_TEST=0 ASSERT_TEST=0 DEBUG_ASSERT=0 KERNEL_EXTRA_CFLAGS=
    cp kernel.elf "$production/debug-off/kernel.elf"
    collect_build_inspection "$production/debug-off"
    ! grep -E '[[:space:]](assertion_warn_report|assertion_bug_report|assertion_test_timer_hook|g_assertion_test_state|g_assertion_test_fixture)$' \
        "$production/debug-off/symbols.txt" \
        >"$production/debug-off/assert-symbol-scan.txt"
    ! grep -E 'new_node == NULL|prev->next != next|\[ASSERT\]\[(WARN|BUG)\]' \
        "$production/debug-off/strings.txt" \
        >"$production/debug-off/assert-string-scan.txt"
    grep -E '[[:space:]](assertion_warn_report|assertion_bug_report)$' \
        "$production/debug-on/symbols.txt" \
        >"$production/debug-on/assert-symbols.txt"

    run_logged production-image "$production/final/build.log" \
        make production-image JOBS="$jobs"
    cp kernel.elf hobbyos.img BOOTX64.EFI "$production/final/"
    cmp -s "$production/debug-off/kernel.elf" "$production/final/kernel.elf"
    extract_payloads "$production/final/hobbyos.img" \
        "$production/final/payloads"
    cmp -s "$production/final/kernel.elf" \
        "$production/final/payloads/kernel.elf"
    cmp -s "$production/final/BOOTX64.EFI" \
        "$production/final/payloads/BOOTX64.EFI"
    nm -u "$production/final/kernel.elf" \
        >"$production/final/undefined-symbols.txt"
    [[ ! -s $production/final/undefined-symbols.txt ]]
    python3 - "$production/result.json" "$production" <<'PY'
import hashlib
import json
from pathlib import Path
import re
import sys
root = Path(sys.argv[2])
def digest(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def allocated(path):
    total = 0
    for line in path.read_text().splitlines():
        match = re.match(r"^\S+\s+([0-9]+)\s+", line)
        if match:
            total += int(match.group(1))
    return total
on = root / "debug-on/kernel.elf"
off = root / "debug-off/kernel.elf"
final = root / "final/kernel.elf"
on_alloc = allocated(root / "debug-on/size-A.txt")
off_alloc = allocated(root / "debug-off/size-A.txt")
def receipt(path):
    return {"bytes": path.stat().st_size, "sha256": digest(path)}
payloads = root / "final/payloads"
result = {
    "schema": 1, "status": "PASS",
    "paired_configuration": {
        "common": {"SELFTEST": 0, "SELFTEST_AUTORUN": 0,
                   "FORMAT_TEST": 0, "ASSERT_TEST": 0,
                   "KERNEL_EXTRA_CFLAGS": ""},
        "only_difference": "DEBUG_ASSERT",
    },
    "debug_on_sha256": digest(on), "debug_off_sha256": digest(off),
    "final_sha256": digest(final),
    "section_comparison": {"status": "PASS",
                           "debug_on_allocated_bytes": on_alloc,
                           "debug_off_allocated_bytes": off_alloc,
                           "difference_bytes": on_alloc - off_alloc},
    "final_artifacts": {name: receipt(root / "final" / name) for name in
                        ("hobbyos.img", "BOOTX64.EFI")},
    "payloads": {name: receipt(payloads / name) for name in
                 ("kernel.elf", "BOOTX64.EFI", "zap-light16.psf",
                  "logo.bmp", "startup.nsh")},
}
if not on_alloc > off_alloc:
    raise SystemExit("debug-on allocated sections did not exceed debug-off")
Path(sys.argv[1]).write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
PY
    echo "ASSERT_PRODUCTION: PASS"
}

fixture_tests()
{
    ensure_evidence
    local output="$evidence/oracle-fixtures"
    [[ ! -e $output ]] || {
        echo "fixture output already exists: $output" >&2
        return 1
    }
    record_command fixtures python3 scripts/verify-assert-list-evidence.py \
        fixtures --output-dir "$output"
    PYTHONDONTWRITEBYTECODE=1 python3 scripts/verify-assert-list-evidence.py \
        fixtures --output-dir "$output" | tee "$evidence/commands/fixtures.log"
}

verify_only()
{
    record_command verify python3 scripts/verify-assert-list-evidence.py \
        all "$evidence"
    PYTHONDONTWRITEBYTECODE=1 python3 scripts/verify-assert-list-evidence.py \
        all "$evidence" | tee "$evidence/commands/verify.log"
}

case ${1:-} in
    preflight) preflight ;;
    host) preflight; host_tests ;;
    static) preflight; static_checks ;;
    build) preflight; build_candidate ;;
    runtime) runtime_tests ;;
    production) production_tests ;;
    fixtures) fixture_tests ;;
    verify) verify_only ;;
    all)
        preflight
        host_tests
        static_checks
        build_candidate
        runtime_tests
        production_tests
        fixture_tests
        verify_only
        echo "ASSERT_GATE: PASS evidence=$evidence"
        ;;
    *) usage ;;
esac
