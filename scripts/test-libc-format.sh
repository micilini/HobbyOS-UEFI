#!/usr/bin/env bash
set -Eeuo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

timestamp=$(date '+%Y%m%d-%H%M%S-%z')
evidence=${FORMAT_EVIDENCE_DIR:-"artifacts/build/libc-format/$timestamp"}
control_runtime=
host_cc=${CC:-gcc}
base_commit=${FORMAT_BASE_COMMIT:-$(git rev-parse HEAD)}
qemu_owned=0
shared_qemu_owned=0
lock_held=0
serial=.qemu/qemu-serial.log
hmp=(python3 scripts/qemu_hmp.py --socket .qemu/hmp.sock)
HARNESS_AUTORUN_EXPECTED=1
source scripts/harness-common.sh
source scripts/harness-runtime-ready.sh
source scripts/harness-framed.sh
source scripts/harness-selftest.sh

usage()
{
    echo "usage: $0 preflight|host|static|build|qemu|consumers|fixtures|verify|all" >&2
    exit 2
}

record_command()
{
    local label=$1
    shift
    mkdir -p "$evidence/commands"
    {
        printf '%s' "$label"
        printf '\t%q' "$@"
        printf '\n'
    } >>"$evidence/commands/commands.tsv"
}

remove_control_runtime()
{
    local directory=$1
    [[ -n $directory && $directory == /tmp/hobbyos-format.* ]] || return 1
    python3 - "$directory" <<'PY'
from pathlib import Path
import shutil
import sys

path = Path(sys.argv[1])
if path.exists():
    shutil.rmtree(path)
PY
}

cleanup()
{
    local cleanup_status=0
    if ((qemu_owned)); then
        QEMU_RUNTIME="$control_runtime" scripts/qemu-agent.sh stop \
            >"$evidence/commands/emergency-qemu-cleanup.log" 2>&1 ||
            cleanup_status=$?
        qemu_owned=0
    fi
    if [[ -n $control_runtime && -d $control_runtime ]]; then
        mkdir -p "$evidence/commands/qemu-emergency"
        cp -a "$control_runtime/." "$evidence/commands/qemu-emergency/"
        remove_control_runtime "$control_runtime" || cleanup_status=1
        control_runtime=
    fi
    if ((shared_qemu_owned)); then
        scripts/qemu-agent.sh stop \
            >"$evidence/commands/emergency-consumer-cleanup.log" 2>&1 ||
            cleanup_status=$?
        shared_qemu_owned=0
    fi
    if ((lock_held)); then
        exec 9>&-
        lock_held=0
    fi
    return "$cleanup_status"
}
trap cleanup EXIT

ensure_evidence()
{
    mkdir -p "$evidence" "$evidence/commands"
    [[ ! -L $evidence ]] || {
        echo "format evidence root must not be a symlink" >&2
        return 1
    }
}

preflight()
{
    ensure_evidence
    mkdir -p "$evidence/preflight"
    record_command preflight git rev-parse HEAD
    python3 - "$evidence/preflight/environment.json" "$root" "$base_commit" <<'PY'
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys

output = Path(sys.argv[1])
root = Path(sys.argv[2])
base = sys.argv[3]

def command(*argv):
    try:
        return subprocess.check_output(argv, cwd=root, text=True,
                                       stderr=subprocess.STDOUT).strip()
    except (OSError, subprocess.CalledProcessError) as error:
        return f"UNAVAILABLE: {error}"

tools = {}
for name, argv in {
    "gcc": ("gcc", "--version"),
    "ld": ("ld", "--version"),
    "make": ("make", "--version"),
    "python": ("python3", "--version"),
    "qemu": ("qemu-system-x86_64", "--version"),
    "mtools": ("mcopy", "-V"),
}.items():
    tools[name] = {"path": shutil.which(argv[0]), "version": command(*argv).splitlines()[0]}

status = command("git", "status", "--short")
payload = {
    "schema": 1,
    "observed_at": datetime.datetime.now(datetime.timezone.utc).astimezone().isoformat(),
    "delivery_dir": str(root),
    "repo_root": command("git", "rev-parse", "--show-toplevel"),
    "branch": command("git", "branch", "--show-current"),
    "head": command("git", "rev-parse", "HEAD"),
    "tree": command("git", "rev-parse", "HEAD^{tree}"),
    "parent": command("git", "rev-parse", "HEAD^"),
    "base_for_scope_comparison": base,
    "worktree_status": status.splitlines() if status else [],
    "host": platform.platform(),
    "schedulable_cpus": len(os.sched_getaffinity(0)),
    "kvm_readable_writable": os.access("/dev/kvm", os.R_OK | os.W_OK),
    "tools": tools,
}
output.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
PY
    sha256sum AGENTS.md docs/foundation/validation-policy.md \
        >"$evidence/preflight/contracts.sha256"
    echo "FORMAT_PREFLIGHT: PASS evidence=$evidence"
}

host_compile_and_run()
{
    local name=$1 sanitizer=$2
    shift 2
    local run="$evidence/host/$name"
    local binary="$run/libc-format-host"
    local compile_log="$run/compile.log"
    local run_log="$run/run.log"
    local source_hash rc
    local -a command=(
        "$host_cc" -std=gnu11 -Wall -Wextra -Werror -fno-builtin -g
        -Dstrlen=hobbyos_legacy_strlen
        -Dstrcmp=hobbyos_legacy_strcmp
        -Dstrncmp=hobbyos_legacy_strncmp
        -Dstrchr=hobbyos_legacy_strchr
        -Dstrcpy=hobbyos_legacy_strcpy
        -Dstrcat=hobbyos_legacy_strcat
        -Dstrrev=hobbyos_legacy_strrev
        "$@" kernel/src/libc/string.c scripts/libc-format-host.c
        -o "$binary"
    )
    mkdir -p "$run"
    source_hash=$(sha256sum kernel/src/libc/string.c | awk '{print $1}')
    record_command "host-$name-compile" "${command[@]}"
    set +e
    "${command[@]}" >"$compile_log" 2>&1
    rc=$?
    set -e
    printf '%s\n' "$rc" >"$run/compile-exit-code.txt"
    ((rc == 0)) || {
        cat "$compile_log" >&2
        echo "FORMAT_HOST_COMPILE: FAIL profile=$name" >&2
        return "$rc"
    }
    python3 - "$run/command.json" "$sanitizer" "$source_hash" \
        "${command[@]}" <<'PY'
import json
from pathlib import Path
import sys

Path(sys.argv[1]).write_text(json.dumps({
    "schema": 1,
    "sanitizer": sys.argv[2],
    "implementation_path": "kernel/src/libc/string.c",
    "implementation_sha256": sys.argv[3],
    "argv": sys.argv[4:],
}, indent=2, sort_keys=True) + "\n")
PY
    record_command "host-$name-run" timeout 20 "$binary"
    set +e
    if [[ $name == asan ]]; then
        ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
            timeout 20 "$binary" >"$run_log" 2>&1
        rc=$?
    else
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
            timeout 20 "$binary" >"$run_log" 2>&1
        rc=$?
    fi
    set -e
    printf '%s\n' "$rc" >"$run/exit-code.txt"
    ((rc == 0)) || {
        cat "$run_log" >&2
        echo "FORMAT_HOST_RUN: FAIL profile=$name rc=$rc" >&2
        return "$rc"
    }
    grep -Fxq '[FORMAT_HOST][CASE] id=deterministic-matrix status=PASS' \
        "$run_log"
    grep -Eq '^\[FORMAT_HOST\]\[SUITE\] status=PASS cases=10 assertions=[1-9][0-9]* failures=0 seed=0x5eedf04a7c9b312d matrix=4096$' \
        "$run_log"
    echo "FORMAT_HOST_RUN: PASS profile=$name"
}

host_tests()
{
    ensure_evidence
    host_compile_and_run normal none
    host_compile_and_run ubsan undefined \
        -fsanitize=undefined -fno-sanitize-recover=all
    host_compile_and_run asan address \
        -fsanitize=address -fno-omit-frame-pointer
    echo "FORMAT_HOST_TESTS: PASS profiles=3"
}

static_checks()
{
    ensure_evidence
    mkdir -p "$evidence/static"
    record_command static bash -n scripts/test-libc-format.sh
    bash -n scripts/test-libc-format.sh
    PYTHONDONTWRITEBYTECODE=1 python3 - <<'PY'
from pathlib import Path
compile(Path("scripts/verify-libc-format-evidence.py").read_text(),
        "scripts/verify-libc-format-evidence.py", "exec")
PY
    git diff --check
    [[ ! -e kernel/src/utils/utils.c && ! -e kernel/src/utils/utils.h ]]
    [[ -f bootloader/src/utils.c && -f bootloader/src/utils.h ]]
    ! rg -n '\b(k_memset|k_memcpy|k_delay)\s*\(' kernel \
        --glob '*.[chS]' >"$evidence/static/dead-utility-references.log"
    ! rg -n 'kinit_progress_append_|pmem_print_hex|heap_print_hex|heap_print_dec' \
        kernel/src/core/kernel_init.c kernel/src/memory/{pmem,heap}.c \
        >"$evidence/static/dead-formatters.log"
    ! rg -n '^static void serial_print_hex' kernel/src/memory/gdt.c \
        >"$evidence/static/dead-gdt-formatter.log"
    ! rg -n 'ksnprintf|kvsnprintf' kernel/src/memory/paging.c \
        >"$evidence/static/paging-format-scan.log"
    ! rg -n '\.\./utils/utils\.h' kernel/src/graphics/{console,terminal}.c \
        >"$evidence/static/dead-include-scan.log"
    ! rg -n '\$\(KERNEL_DIR\)/src/utils/utils\.c' makefile \
        >"$evidence/static/kernel-source-list-scan.log"
    rg -n '\$\(BOOT_DIR\)/src/utils\.o' makefile \
        >"$evidence/static/bootloader-utility-scan.log"
    if make -n kernel.elf FORMAT_TEST=1 SELFTEST=0 \
        >"$evidence/static/incompatible-config.log" 2>&1; then
        echo "FORMAT_STATIC: incompatible test configuration was accepted" >&2
        return 1
    fi
    grep -Fq 'FORMAT_TEST=1 requires SELFTEST=1' \
        "$evidence/static/incompatible-config.log"
    python3 - "$base_commit" "$evidence/static/include-exceptions.json" <<'PY'
import hashlib
import json
from pathlib import Path
import subprocess
import sys

base = sys.argv[1]
output = Path(sys.argv[2])
rows = []
for relative in ("kernel/src/graphics/console.c",
                 "kernel/src/graphics/terminal.c"):
    original = subprocess.check_output(["git", "show", f"{base}:{relative}"])
    current = Path(relative).read_bytes()
    expected = original.replace(b'#include "../utils/utils.h"\n', b'', 1)
    rows.append({
        "path": relative,
        "base_sha256": hashlib.sha256(original).hexdigest(),
        "current_sha256": hashlib.sha256(current).hexdigest(),
        "only_authorized_include_removed": current == expected,
    })
if not all(row["only_authorized_include_removed"] for row in rows):
    raise SystemExit("include-only exception differs from the authorized line")
output.write_text(json.dumps({"schema": 1, "files": rows, "status": "PASS"},
                             indent=2, sort_keys=True) + "\n")
PY
    python3 - "$evidence/static/result.json" <<'PY'
import json
from pathlib import Path
import sys
Path(sys.argv[1]).write_text(json.dumps({
    "schema": 1,
    "status": "PASS",
    "live_consumers_migrated": ["kernel/src/core/kernel_init.c",
                                  "kernel/src/memory/pmem.c"],
    "dead_helpers_removed": ["heap_print_hex", "heap_print_dec",
                               "gdt.serial_print_hex"],
    "constant_only_consumer": "kernel/src/memory/paging.c",
    "kernel_utilities_removed": ["k_memset", "k_memcpy", "k_delay"],
    "bootloader_utilities_preserved": True,
}, indent=2, sort_keys=True) + "\n")
PY
    echo "FORMAT_STATIC: PASS"
}

run_make_logged()
{
    local label=$1 log=$2
    shift 2
    local rc
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

extract_payloads()
{
    local image="$evidence/candidate/hobbyos.img"
    local destination="$evidence/candidate/payloads"
    mkdir -p "$destination"
    mcopy -o -i "$image" ::/EFI/BOOT/BOOTX64.EFI \
        "$destination/BOOTX64.EFI"
    mcopy -o -i "$image" ::/kernel.elf "$destination/kernel.elf"
    mcopy -o -i "$image" ::/EFI/fonts/zap-light16.psf \
        "$destination/zap-light16.psf"
    mcopy -o -i "$image" ::/EFI/images/logo.bmp "$destination/logo.bmp"
    mcopy -o -i "$image" ::/startup.nsh "$destination/startup.nsh"
}

serial_exact_once()
{
    local file=$1 expected=$2
    awk -v expected="$expected" '
        {
            line = $0
            sub(/\r$/, "", line)
            if (line == expected)
                count++
        }
        END { exit count == 1 ? 0 : 1 }
    ' "$file"
}

build_candidate()
{
    ensure_evidence
    mkdir -p "$evidence/build" "$evidence/candidate"
    run_make_logged kernel-check "$evidence/build/kernel-check-command.log" \
        make kernel-check JOBS=2 SELFTEST=1 SELFTEST_AUTORUN=0 FORMAT_TEST=1
    cp artifacts/build/kernel-check-j2.log \
        "$evidence/build/kernel-check-canonical.log"
    cp kernel.elf "$evidence/build/kernel-check.elf"
    run_make_logged stack-check "$evidence/build/stack-check-command.log" \
        make stack-check JOBS=2 SELFTEST=1 SELFTEST_AUTORUN=0 FORMAT_TEST=1
    cp artifacts/build/kernel-check-j2.log \
        "$evidence/build/stack-check-kernel-canonical.log"
    cp artifacts/build/stack-usage-report.txt \
        "$evidence/build/stack-usage-report.txt"
    grep -Fxq 'stack-check: PASS' "$evidence/build/stack-check-command.log"
    grep -Fxq 'violations=0' "$evidence/build/stack-usage-report.txt"
    run_make_logged image-clean "$evidence/build/image-clean.log" make clean
    run_make_logged image "$evidence/build/image-command.log" \
        make image SELFTEST=1 SELFTEST_AUTORUN=0 FORMAT_TEST=1
    cp kernel.elf hobbyos.img BOOTX64.EFI "$evidence/candidate/"
    extract_payloads
    cmp -s "$evidence/candidate/kernel.elf" \
        "$evidence/candidate/payloads/kernel.elf"
    cmp -s "$evidence/candidate/BOOTX64.EFI" \
        "$evidence/candidate/payloads/BOOTX64.EFI"
    nm -u kernel.elf >"$evidence/build/kernel-undefined-symbols.txt"
    [[ ! -s $evidence/build/kernel-undefined-symbols.txt ]]
    nm -n kernel.elf >"$evidence/build/kernel-symbols.txt"
    rg -n ' (ksnprintf|kvsnprintf|format_selftest_run)$' \
        "$evidence/build/kernel-symbols.txt" \
        >"$evidence/build/format-symbols.txt"
    objdump -d kernel/src/libc/string.o \
        >"$evidence/build/string-disassembly.txt"
    ! rg -n '\b(xmm|ymm|zmm|mm[0-7])' \
        "$evidence/build/string-disassembly.txt" \
        >"$evidence/build/string-simd-scan.txt"
    nm -u kernel/src/libc/string.o \
        >"$evidence/build/string-undefined-symbols.txt"
    [[ ! -s $evidence/build/string-undefined-symbols.txt ]]
    [[ ! -e kernel/src/utils/utils.o && ! -e kernel/src/utils/utils.d &&
       ! -e kernel/src/utils/utils.su ]]
    ! rg -n 'kernel/src/(libc/(string|format_selftest)|core/kernel_init|memory/(pmem|heap|gdt)|graphics/(console|terminal))\.c:.*warning:' \
        "$evidence/build/kernel-check-canonical.log" \
        "$evidence/build/stack-check-kernel-canonical.log" \
        "$evidence/build/image-command.log" \
        >"$evidence/build/related-warning-scan.txt"
    sha256sum "$evidence/candidate/kernel.elf" \
        "$evidence/candidate/hobbyos.img" \
        "$evidence/candidate/BOOTX64.EFI" \
        >"$evidence/build/candidate.sha256"
    echo "FORMAT_BUILD: PASS"
}

write_qemu_profile()
{
    local name=$1 machine=$2 accel=$3 smp=$4 pid=$5 started=$6 ended=$7
    local run_image=$8 initial_image_hash=$9
    local directory="$evidence/qemu/$name"
    local kernel_hash image_hash final_image_hash
    kernel_hash=$(sha256sum "$evidence/candidate/kernel.elf" | awk '{print $1}')
    image_hash=$(sha256sum "$evidence/candidate/hobbyos.img" | awk '{print $1}')
    final_image_hash=$(sha256sum "$run_image" | awk '{print $1}')
    python3 - "$directory/profile.json" "$name" "$machine" "$accel" \
        "$smp" "$pid" "$started" "$ended" "$kernel_hash" "$image_hash" \
        "qemu/$name/hobbyos-run.img" "$initial_image_hash" \
        "$final_image_hash" <<'PY'
import json
from pathlib import Path
import sys

Path(sys.argv[1]).write_text(json.dumps({
    "schema": 1,
    "name": sys.argv[2],
    "machine": sys.argv[3],
    "accel": sys.argv[4],
    "smp": int(sys.argv[5]),
    "qemu_pid": int(sys.argv[6]),
    "started_at": sys.argv[7],
    "ended_at": sys.argv[8],
    "kernel_sha256": sys.argv[9],
    "image_sha256": sys.argv[10],
    "run_image_path": sys.argv[11],
    "run_image_initial_sha256": sys.argv[12],
    "run_image_final_sha256": sys.argv[13],
    "completion_observed": True,
    "qemu_alive_after_cleanup": False,
}, indent=2, sort_keys=True) + "\n")
PY
}

run_qemu_profile()
{
    local name=$1 machine=$2 accel=$3 smp=$4
    local directory="$evidence/qemu/$name"
    local pid started ended rc run_image initial_image_hash
    mkdir -p "$directory" "$control_runtime"
    run_image="$directory/hobbyos-run.img"
    cp "$evidence/candidate/hobbyos.img" "$run_image"
    initial_image_hash=$(sha256sum "$run_image" | awk '{print $1}')
    [[ $initial_image_hash == "$(sha256sum "$evidence/candidate/hobbyos.img" | awk '{print $1}')" ]]
    printf '%s  hobbyos-run.img\n' "$initial_image_hash" \
        >"$directory/run-image-before.sha256"
    started=$(date --iso-8601=seconds)
    record_command "qemu-$name-start" env QEMU_RUNTIME="$control_runtime" \
        HOBBYOS_IMAGE="$run_image" MACHINE="$machine" \
        ACCEL="$accel" SMP="$smp" scripts/qemu-agent.sh start
    set +e
    QEMU_RUNTIME="$control_runtime" \
    HOBBYOS_IMAGE="$run_image" \
    MACHINE="$machine" ACCEL="$accel" SMP="$smp" \
        scripts/qemu-agent.sh start >"$directory/start.log" 2>&1
    rc=$?
    set -e
    printf '%s\n' "$rc" >"$directory/start-exit-code.txt"
    ((rc == 0)) || {
        cat "$directory/start.log" >&2
        return "$rc"
    }
    qemu_owned=1
    pid=$(cat "$control_runtime/qemu.pid")
    [[ $pid =~ ^[1-9][0-9]*$ ]]
    python3 - "$pid" "$directory/argv.json" <<'PY'
import json
from pathlib import Path
import sys
raw = Path(f"/proc/{sys.argv[1]}/cmdline").read_bytes()
argv = [item.decode(errors="replace") for item in raw.split(b"\0") if item]
Path(sys.argv[2]).write_text(json.dumps(argv, indent=2) + "\n")
PY
    scripts/wait-for-log.sh "$control_runtime/qemu-serial.log" \
        '[FORMAT][SUITE_END] status=PASS completed=1 cases=22' 120 \
        >"$directory/selftest-wait.log" 2>&1
    scripts/wait-for-log.sh "$control_runtime/qemu-serial.log" \
        '[KERNEL] Entering Main Loop.' 180 \
        >"$directory/runtime-wait.log" 2>&1
    QEMU_RUNTIME="$control_runtime" scripts/qemu-agent.sh status \
        >"$directory/status-before-cleanup.log" 2>&1
    record_command "qemu-$name-stop" env QEMU_RUNTIME="$control_runtime" \
        scripts/qemu-agent.sh stop
    set +e
    QEMU_RUNTIME="$control_runtime" scripts/qemu-agent.sh stop \
        >"$directory/stop.log" 2>&1
    rc=$?
    set -e
    printf '%s\n' "$rc" >"$directory/stop-exit-code.txt"
    ((rc == 0)) || return "$rc"
    qemu_owned=0
    local process_stat=""
    for _ in {1..30}; do
        if ! process_stat=$(ps -p "$pid" -o stat= 2>/dev/null); then
            process_stat=""
        fi
        if [[ -z $process_stat || $process_stat == Z* ]]; then
            break
        fi
        sleep 0.1
    done
    if [[ -n $process_stat && $process_stat != Z* ]]; then
        echo "QEMU process remains after cleanup: $pid" >&2
        return 1
    fi
    [[ ! -e $control_runtime/qemu.pid && ! -S $control_runtime/hmp.sock ]]
    cp "$control_runtime/qemu-serial.log" "$directory/serial.log"
    cp "$control_runtime/qemu-debugcon.log" "$directory/debugcon.log"
    cp "$control_runtime/qemu-trace.log" "$directory/trace.log"
    cp "$control_runtime/launch.env" "$directory/launch.env"
    serial_exact_once "$directory/serial.log" \
        '[FORMAT][SUITE_BEGIN] cases=22'
    serial_exact_once "$directory/serial.log" \
        '[FORMAT][SUITE_END] status=PASS completed=1 cases=22'
    ! rg -n 'FORMAT_ERROR|PANIC|#PF|#GP|FATAL' "$directory/serial.log" \
        >"$directory/fatal-scan.log"
    ended=$(date --iso-8601=seconds)
    write_qemu_profile "$name" "$machine" "$accel" "$smp" "$pid" \
        "$started" "$ended" "$run_image" "$initial_image_hash"
    sha256sum "$run_image" >"$directory/run-image-after.sha256"
    echo "FORMAT_QEMU: PASS profile=$name"
}

qemu_tests()
{
    ensure_evidence
    [[ -f $evidence/candidate/hobbyos.img &&
       -f $evidence/candidate/kernel.elf ]]
    cmp -s kernel.elf "$evidence/candidate/kernel.elf"
    [[ -r /dev/kvm && -w /dev/kvm ]] || {
        echo "FORMAT_QEMU: BLOCKED KVM unavailable" >&2
        return 3
    }
    mkdir -p .qemu
    exec 9>.qemu/harness.lock
    flock -n 9 || {
        echo "another HobbyOS QEMU harness is active" >&2
        return 1
    }
    lock_held=1
    [[ ! -r .qemu/qemu.pid ]] || {
        echo "shared QEMU runtime is occupied" >&2
        return 1
    }
    control_runtime=$(mktemp -d /tmp/hobbyos-format.XXXXXX)
    chmod 700 "$control_runtime"
    mkdir -p "$evidence/qemu"
    printf '%s\n' "$control_runtime" >"$evidence/qemu/runtime-path.txt"
    run_qemu_profile q35-tcg-smp1 q35 tcg 1
    run_qemu_profile q35-kvm-smp4 q35 kvm 4
    remove_control_runtime "$control_runtime"
    control_runtime=
    exec 9>&-
    lock_held=0
    sha256sum -c "$evidence/build/candidate.sha256" \
        >"$evidence/qemu/candidate-after.sha256.log"
    echo "FORMAT_QEMU_TESTS: PASS profiles=2"
}

extract_consumer_payloads()
{
    local image="$evidence/consumers/candidate/hobbyos.img"
    local destination="$evidence/consumers/candidate/payloads"
    mkdir -p "$destination"
    mcopy -o -i "$image" ::/EFI/BOOT/BOOTX64.EFI \
        "$destination/BOOTX64.EFI"
    mcopy -o -i "$image" ::/kernel.elf "$destination/kernel.elf"
    mcopy -o -i "$image" ::/EFI/fonts/zap-light16.psf \
        "$destination/zap-light16.psf"
    mcopy -o -i "$image" ::/EFI/images/logo.bmp "$destination/logo.bmp"
    mcopy -o -i "$image" ::/startup.nsh "$destination/startup.nsh"
}

build_consumer_candidate()
{
    mkdir -p "$evidence/consumers/build" "$evidence/consumers/candidate"
    run_make_logged consumer-clean "$evidence/consumers/build/clean.log" \
        make clean
    run_make_logged consumer-image "$evidence/consumers/build/image.log" \
        make image SELFTEST=1 SELFTEST_AUTORUN=1 FORMAT_TEST=0
    cp kernel.elf hobbyos.img BOOTX64.EFI "$evidence/consumers/candidate/"
    extract_consumer_payloads
    cmp -s "$evidence/consumers/candidate/kernel.elf" \
        "$evidence/consumers/candidate/payloads/kernel.elf"
    cmp -s "$evidence/consumers/candidate/BOOTX64.EFI" \
        "$evidence/consumers/candidate/payloads/BOOTX64.EFI"
    nm -n kernel.elf >"$evidence/consumers/build/kernel-symbols.txt"
    ! rg -n 'format_selftest_run|kinit_progress_format_probe' \
        "$evidence/consumers/build/kernel-symbols.txt" \
        >"$evidence/consumers/build/format-test-symbol-scan.txt"
    sha256sum "$evidence/consumers/candidate/kernel.elf" \
        "$evidence/consumers/candidate/hobbyos.img" \
        "$evidence/consumers/candidate/BOOTX64.EFI" \
        >"$evidence/consumers/build/candidate.sha256"
}

wait_process_inactive()
{
    local pid=$1 state=""
    for _ in {1..30}; do
        if ! state=$(ps -p "$pid" -o stat= 2>/dev/null); then
            state=""
        fi
        [[ -z $state || $state == Z* ]] && return 0
        sleep 0.1
    done
    return 1
}

write_process_argv()
{
    local pid=$1 output=$2
    python3 - "$pid" "$output" <<'PY'
import json
from pathlib import Path
import sys
raw = Path(f"/proc/{sys.argv[1]}/cmdline").read_bytes()
argv = [item.decode(errors="replace") for item in raw.split(b"\0") if item]
Path(sys.argv[2]).write_text(json.dumps(argv, indent=2) + "\n")
PY
}

start_consumer_qemu()
{
    local run_dir=$1 run_image=$2 smp=$3
    local rc
    [[ ! -e .qemu/qemu.pid && ! -S .qemu/hmp.sock ]]
    HARNESS_RUNTIME_READY=0
    HARNESS_RUNTIME_READY_PID=
    HARNESS_FRAME_QEMU_PID=
    HARNESS_FRAME_SEQUENCE=0
    HARNESS_FRAME_LAST_SEQUENCE=0
    HARNESS_FRAME_LAST_STATUS=
    HARNESS_STATUS_RECORD_LOG="$run_dir/status-authority.log"
    export HARNESS_RUNTIME_READY HARNESS_RUNTIME_READY_PID
    export HOBBYOS_IMAGE="$run_image"
    record_command "consumer-${smp}-start" env HOBBYOS_IMAGE="$run_image" \
        MACHINE=q35 ACCEL=kvm SMP="$smp" scripts/qemu-agent.sh start
    set +e
    MACHINE=q35 ACCEL=kvm SMP="$smp" scripts/qemu-agent.sh start \
        >"$run_dir/start.log" 2>&1
    rc=$?
    set -e
    printf '%s\n' "$rc" >"$run_dir/start-exit-code.txt"
    ((rc == 0)) || return "$rc"
    shared_qemu_owned=1
    CONSUMER_QEMU_PID=$(cat .qemu/qemu.pid)
    [[ $CONSUMER_QEMU_PID =~ ^[1-9][0-9]*$ ]]
    write_process_argv "$CONSUMER_QEMU_PID" "$run_dir/argv.json"
    scripts/wait-for-log.sh "$serial" '[KERNEL] Entering Main Loop.' 180 \
        >"$run_dir/runtime-wait.log" 2>&1
    runtime_wait_boot_ready "$serial" 180 1
    selftest_validate_autorun "$serial" "$run_dir/autorun" \
        modal.open_close.heap_direction \
        modal.open_close.used_blocks \
        modal.open_close.zero_is_not_idle \
        modal.open_close.record_fit \
        >"$run_dir/autorun-validation.log" 2>&1
}

stop_consumer_qemu()
{
    local run_dir=$1 name=$2 smp=$3 run_image=$4 started=$5 kind=$6
    local rc ended kernel_hash image_hash initial_image_hash final_image_hash
    scripts/qemu-agent.sh status >"$run_dir/status-before-cleanup.log" 2>&1
    record_command "consumer-$name-stop" scripts/qemu-agent.sh stop
    set +e
    scripts/qemu-agent.sh stop >"$run_dir/stop.log" 2>&1
    rc=$?
    set -e
    printf '%s\n' "$rc" >"$run_dir/stop-exit-code.txt"
    ((rc == 0)) || return "$rc"
    shared_qemu_owned=0
    wait_process_inactive "$CONSUMER_QEMU_PID" || {
        echo "consumer QEMU process remains active: $CONSUMER_QEMU_PID" >&2
        return 1
    }
    [[ ! -e .qemu/qemu.pid && ! -S .qemu/hmp.sock ]]
    cp .qemu/qemu-serial.log "$run_dir/serial.log"
    cp .qemu/qemu-debugcon.log "$run_dir/debugcon.log"
    cp .qemu/qemu-trace.log "$run_dir/trace.log"
    cp .qemu/launch.env "$run_dir/launch.env"
    ended=$(date --iso-8601=seconds)
    kernel_hash=$(sha256sum "$evidence/consumers/candidate/kernel.elf" | awk '{print $1}')
    image_hash=$(sha256sum "$evidence/consumers/candidate/hobbyos.img" | awk '{print $1}')
    initial_image_hash=$(awk '{print $1}' "$run_dir/run-image-before.sha256")
    final_image_hash=$(sha256sum "$run_image" | awk '{print $1}')
    sha256sum "$run_image" >"$run_dir/run-image-after.sha256"
    python3 - "$run_dir/profile.json" "$name" "$smp" "$kind" \
        "$CONSUMER_QEMU_PID" "$started" "$ended" "$kernel_hash" \
        "$image_hash" "$initial_image_hash" "$final_image_hash" <<'PY'
import json
from pathlib import Path
import sys
name = sys.argv[2]
kind = sys.argv[4]
relative = (f"consumers/modal-heap/{name}/hobbyos-run.img" if kind == "modal-heap"
            else "consumers/visual/q35-kvm-smp4/hobbyos-run.img")
Path(sys.argv[1]).write_text(json.dumps({
    "schema": 1,
    "name": name,
    "machine": "q35",
    "accel": "kvm",
    "smp": int(sys.argv[3]),
    "kind": kind,
    "qemu_pid": int(sys.argv[5]),
    "started_at": sys.argv[6],
    "ended_at": sys.argv[7],
    "kernel_sha256": sys.argv[8],
    "image_sha256": sys.argv[9],
    "run_image_path": relative,
    "run_image_initial_sha256": sys.argv[10],
    "run_image_final_sha256": sys.argv[11],
    "completion_observed": True,
    "qemu_alive_after_cleanup": False,
}, indent=2, sort_keys=True) + "\n")
PY
    unset HOBBYOS_IMAGE
}

record_consumer_command()
{
    local run_dir=$1 payload=$2 marker=$3 timeout=$4 profile=$5
    local start_line end_line sequence status crc
    start_line=$(wc -l <"$serial")
    crc=$(framed_crc32 "$payload")
    framed_send_complete "$payload" "$marker" "$timeout" "$profile" 0
    end_line=$(wc -l <"$serial")
    sequence=$HARNESS_FRAME_LAST_SEQUENCE
    status=$HARNESS_FRAME_LAST_STATUS
    python3 - "$run_dir/commands.jsonl" "$payload" "$marker" "$start_line" \
        "$end_line" "$sequence" "$status" "$crc" <<'PY'
import json
from pathlib import Path
import sys
path = Path(sys.argv[1])
row = {
    "schema": 1,
    "payload": sys.argv[2],
    "marker": sys.argv[3],
    "start_line_count": int(sys.argv[4]),
    "end_line_count": int(sys.argv[5]),
    "sequence": int(sys.argv[6]),
    "handler_status": int(sys.argv[7]),
    "crc32": sys.argv[8],
}
with path.open("a") as stream:
    stream.write(json.dumps(row, sort_keys=True) + "\n")
PY
}

run_modal_heap_profile()
{
    local smp=$1 run=$2 name
    name="q35-kvm-smp${smp}-run${run}"
    local run_dir="$evidence/consumers/modal-heap/$name"
    local run_image="$run_dir/hobbyos-run.img" started workers
    mkdir -p "$run_dir"
    cp "$evidence/consumers/candidate/hobbyos.img" "$run_image"
    sha256sum "$run_image" >"$run_dir/run-image-before.sha256"
    started=$(date --iso-8601=seconds)
    start_consumer_qemu "$run_dir" "$run_image" "$smp"
    : >"$run_dir/commands.jsonl"
    workers=$((smp * 2))
    record_consumer_command "$run_dir" \
        "taskmantest memory-checkpoint-begin 1000 $workers" \
        "[TASKMANTEST][MEMORY_CHECKPOINT_BEGIN] PASS" 90 stress
    record_consumer_command "$run_dir" "smpstress $workers 0 1000" \
        "[SMP] smpstress spawning workers=$workers" 90 stress
    record_consumer_command "$run_dir" "taskmantest memory-checkpoint-track" \
        "[TASKMANTEST][MEMORY_CHECKPOINT_TRACK] PASS" 90 stress
    record_consumer_command "$run_dir" "modaltest open-close 1000" \
        "[MODALTEST][OPEN_CLOSE] PASS cycles=1000" 1800 stress
    record_consumer_command "$run_dir" "modaltest check" \
        "[MODALTEST][CHECK] PASS" 90 stress
    record_consumer_command "$run_dir" "taskdiag check" \
        "[TASKDIAG][CHECK] PASS" 180 stress
    record_consumer_command "$run_dir" \
        "taskmantest memory-checkpoint-progress" \
        "[TASKMANTEST][MEMORY_CHECKPOINT_PROGRESS] PASS" 90 stress
    record_consumer_command "$run_dir" "killtest smpstress-sweep" \
        "[SMP][KILL_SWEEP] PASS workers=$workers" 900 stress
    record_consumer_command "$run_dir" "modaltest check" \
        "[MODALTEST][CHECK] PASS" 90 stress
    record_consumer_command "$run_dir" "taskdiag check" \
        "[TASKDIAG][CHECK] PASS" 180 stress
    record_consumer_command "$run_dir" "taskmantest memory-checkpoint-end" \
        "[TASKMANTEST][MEMORY_CHECKPOINT_END] PASS" 300 stress
    record_consumer_command "$run_dir" "tasktest transport-status" \
        "[HARNESS][STATUS]" 90 stress
    assert_clean_log "$serial"
    stop_consumer_qemu "$run_dir" "$name" "$smp" "$run_image" \
        "$started" modal-heap
    echo "FORMAT_CONSUMER: PASS profile=$name"
}

capture_taskman_mode()
{
    local run_dir=$1 mode=$2 columns=$3
    local begin end owner screenshot absolute stats frames start_line end_line
    record_consumer_command "$run_dir" \
        "taskmantest geometry-runtime $columns 40" \
        "[TASKMANTEST][GEOMETRY_RUNTIME] PASS" 120 normal
    record_consumer_command "$run_dir" "taskmantest anchor-reset" \
        "[TASKMANTEST][ANCHOR_RESET] PASS" 120 normal
    begin=$(count '[MODAL] session_begin OK')
    end=$(count '[MODAL] session_end OK')
    owner=$(count '[TASKMAN][OWNER_STATE] shell=BLOCKED session=ACTIVE')
    start_line=$(wc -l <"$serial")
    harness_command="taskman $mode smoke"
    harness_start_line=$start_line
    hmp_text 'taskman 1000' normal --enter >"$run_dir/${mode,,}-launch-hmp.log"
    wait_new '[MODAL] session_begin OK' "$begin" 120
    wait_new '[TASKMAN][OWNER_STATE] shell=BLOCKED session=ACTIVE' "$owner" 120
    sleep 2
    screenshot="$run_dir/${mode,,}-screendump.ppm"
    absolute=$(realpath -m "$screenshot")
    "${hmp[@]}" command "screendump \"$absolute\"" \
        >"$run_dir/${mode,,}-screendump-hmp.log"
    [[ -f $screenshot && $(head -c 2 "$screenshot") == P6 ]]
    hmp_key esc normal >"$run_dir/${mode,,}-escape-hmp.log"
    wait_new '[MODAL] session_end OK' "$end" 120
    shell_sync
    record_consumer_command "$run_dir" "taskmantest stats" \
        "[TASKMANTEST][STATS]" 120 normal
    end_line=$(wc -l <"$serial")
    stats=$(grep -F '[TASKMANTEST][STATS]' "$serial" | tail -1)
    grep -Eq "last_mode=$mode([[:space:]]|$)" <<<"$stats"
    frames=$(sed -nE 's/.*last_session_full_frames=([0-9]+).*/\1/p' <<<"$stats")
    [[ -n $frames && $frames -ge 1 ]]
    for field in last_session_fallback_frames stable_frame_full_clears \
                 stale_cells clipped last_session_scroll_delta workspace_live; do
        grep -Eq "${field}=0([[:space:]]|$)" <<<"$stats"
    done
    sha256sum "$screenshot" >"$run_dir/${mode,,}-screendump.sha256"
    python3 - "$run_dir/visual-observations.jsonl" "$mode" "$columns" \
        "$start_line" "$end_line" "$begin" "$end" "$owner" \
        "$screenshot" "$frames" "$stats" <<'PY'
import hashlib
import json
from pathlib import Path
import sys
path = Path(sys.argv[1])
screenshot = Path(sys.argv[9])
row = {
    "schema": 1,
    "mode": sys.argv[2],
    "columns": int(sys.argv[3]),
    "start_line_count": int(sys.argv[4]),
    "end_line_count": int(sys.argv[5]),
    "begin_count_before": int(sys.argv[6]),
    "end_count_before": int(sys.argv[7]),
    "owner_count_before": int(sys.argv[8]),
    "screenshot": screenshot.name,
    "screenshot_bytes": screenshot.stat().st_size,
    "screenshot_sha256": hashlib.sha256(screenshot.read_bytes()).hexdigest(),
    "full_frames": int(sys.argv[10]),
    "stats_line": sys.argv[11],
}
with path.open("a") as stream:
    stream.write(json.dumps(row, sort_keys=True) + "\n")
PY
}

run_visual_profile()
{
    local name=q35-kvm-smp4 run_dir="$evidence/consumers/visual/q35-kvm-smp4"
    local run_image="$evidence/consumers/visual/q35-kvm-smp4/hobbyos-run.img"
    local started
    mkdir -p "$run_dir"
    cp "$evidence/consumers/candidate/hobbyos.img" "$run_image"
    sha256sum "$run_image" >"$run_dir/run-image-before.sha256"
    started=$(date --iso-8601=seconds)
    start_consumer_qemu "$run_dir" "$run_image" 4
    : >"$run_dir/commands.jsonl"
    : >"$run_dir/visual-observations.jsonl"
    record_consumer_command "$run_dir" "taskmantest stats-reset" \
        "[TASKMANTEST][STATS_RESET] PASS" 120 normal
    capture_taskman_mode "$run_dir" WIDE 118
    capture_taskman_mode "$run_dir" COMPACT 76
    record_consumer_command "$run_dir" "taskmantest geometry-clear" \
        "[TASKMANTEST][GEOMETRY_RUNTIME] CLEARED" 120 normal
    record_consumer_command "$run_dir" "inputtest check" \
        "[INPUTTEST][CHECK] PASS" 120 normal
    record_consumer_command "$run_dir" "modaltest check" \
        "[MODALTEST][CHECK] PASS" 120 normal
    record_consumer_command "$run_dir" "taskdiag check" \
        "[TASKDIAG][CHECK] PASS" 180 normal
    record_consumer_command "$run_dir" "taskmantest check" \
        "[TASKMANTEST][CHECK] PASS" 180 normal
    assert_clean_log "$serial"
    stop_consumer_qemu "$run_dir" "$name" 4 "$run_image" "$started" visual
    echo "FORMAT_VISUAL: PASS modes=WIDE,COMPACT"
}

write_consumer_result()
{
    python3 - "$evidence/consumers/result.json" "$evidence" <<'PY'
import hashlib
import json
from pathlib import Path
import sys
output = Path(sys.argv[1])
evidence = Path(sys.argv[2])
candidate = evidence / "consumers/candidate"
def receipt(path):
    return {"path": path.relative_to(evidence).as_posix(),
            "bytes": path.stat().st_size,
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
payloads = {name: receipt(candidate / "payloads" / name) for name in
            ("BOOTX64.EFI", "kernel.elf", "zap-light16.psf", "logo.bmp",
             "startup.nsh")}
result = {
    "schema": 1,
    "status": "PASS",
    "configuration": {
        "SELFTEST": 1,
        "SELFTEST_AUTORUN": 1,
        "FORMAT_TEST": 0,
    },
    "candidate": {name: receipt(candidate / name) for name in
                  ("kernel.elf", "hobbyos.img", "BOOTX64.EFI")},
    "payloads": payloads,
    "modal_heap_profiles": [
        f"q35-kvm-smp{smp}-run{run}"
        for smp, count in ((4, 2), (8, 3))
        for run in range(1, count + 1)
    ],
    "visual_profile": "q35-kvm-smp4",
}
output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
PY
}

current_consumer_tests()
{
    ensure_evidence
    [[ -r /dev/kvm && -w /dev/kvm ]] || {
        echo "FORMAT_CONSUMERS: BLOCKED KVM unavailable" >&2
        return 3
    }
    build_consumer_candidate
    mkdir -p .qemu
    exec 9>.qemu/harness.lock
    flock -n 9 || {
        echo "another HobbyOS QEMU harness is active" >&2
        return 1
    }
    lock_held=1
    [[ ! -e .qemu/qemu.pid && ! -S .qemu/hmp.sock ]]
    for run in 1 2; do
        run_modal_heap_profile 4 "$run"
    done
    for run in 1 2 3; do
        run_modal_heap_profile 8 "$run"
    done
    run_visual_profile
    exec 9>&-
    lock_held=0
    sha256sum -c "$evidence/consumers/build/candidate.sha256" \
        >"$evidence/consumers/candidate-after.sha256.log"
    write_consumer_result
    echo "FORMAT_CONSUMERS: PASS modal_runs=5 visual_modes=2"
}

collect_and_verify()
{
    local mode=${1:-all}
    ensure_evidence
    record_command collect python3 scripts/verify-libc-format-evidence.py \
        collect "$evidence"
    PYTHONDONTWRITEBYTECODE=1 python3 scripts/verify-libc-format-evidence.py \
        collect "$evidence" >"$evidence/commands/collect.log" 2>&1
    record_command verify python3 scripts/verify-libc-format-evidence.py \
        "$mode" "$evidence"
    PYTHONDONTWRITEBYTECODE=1 python3 scripts/verify-libc-format-evidence.py \
        "$mode" "$evidence" | tee "$evidence/commands/verify.log"
}

fixture_tests()
{
    ensure_evidence
    local output="$evidence/oracle-fixtures"
    [[ ! -e $output ]] || {
        echo "fixture output already exists: $output" >&2
        return 1
    }
    record_command fixtures python3 scripts/verify-libc-format-evidence.py \
        fixtures --output-dir "$output"
    PYTHONDONTWRITEBYTECODE=1 python3 scripts/verify-libc-format-evidence.py \
        fixtures --output-dir "$output" | tee "$evidence/commands/fixtures.log"
}

verify_only()
{
    PYTHONDONTWRITEBYTECODE=1 python3 scripts/verify-libc-format-evidence.py \
        all "$evidence"
}

case ${1:-} in
    preflight) preflight ;;
    host) preflight; host_tests ;;
    static) preflight; static_checks ;;
    build) preflight; build_candidate ;;
    qemu) qemu_tests; collect_and_verify core ;;
    consumers) current_consumer_tests ;;
    fixtures) fixture_tests ;;
    verify) verify_only ;;
    all)
        preflight
        host_tests
        static_checks
        build_candidate
        qemu_tests
        current_consumer_tests
        collect_and_verify all
        fixture_tests
        verify_only
        echo "FORMAT_GATE: PASS evidence=$evidence"
        ;;
    *) usage ;;
esac
