#!/usr/bin/env bash
set -Eeuo pipefail

export LC_ALL=C
export PYTHONDONTWRITEBYTECODE=1

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"

mode=${1:-}
jobs=${JOBS:-$(nproc)}
stamp=$(date '+%Y%m%d-%H%M%S-%z')
evidence=${BUILD_PROFILE_EVIDENCE_DIR:-"artifacts/build/build-profile/$stamp"}
evidence=$(realpath -m "$evidence")
runtime_parent=${BUILD_PROFILE_RUNTIME_PARENT:-${TMPDIR:-/tmp}}
current_runtime=
current_profile=
current_pid=

usage()
{
    echo "usage: $0 static|build|runtime|production|fixtures|verify|all" >&2
    exit 2
}

ensure_evidence()
{
    [[ ! -L $evidence ]] || {
        echo "build-profile evidence root must not be a symlink" >&2
        return 1
    }
    mkdir -p "$evidence" "$evidence/commands"
}

run_logged()
{
    local label=$1 log=$2
    shift 2
    local status
    mkdir -p "$(dirname "$log")"
    {
        printf '%s' "$label"
        printf '\t%q' "$@"
        printf '\n'
    } >>"$evidence/commands/commands.tsv"
    set +e
    "$@" >"$log" 2>&1
    status=$?
    set -e
    printf '%s\n' "$status" >"${log%.log}.exit-code.txt"
    if ((status != 0)); then
        cat "$log" >&2
        return "$status"
    fi
}

pid_active()
{
    local pid=$1
    [[ $pid =~ ^[1-9][0-9]*$ ]] && kill -0 "$pid" 2>/dev/null
}

qemu_agent()
{
    local action=$1
    shift
    QEMU_RUNTIME="$current_runtime" scripts/qemu-agent.sh "$action" "$@"
}

cleanup_active_vm()
{
    local reason=${1:-trap}
    [[ -n $current_runtime ]] || return 0
    if [[ -n $current_pid ]] && pid_active "$current_pid"; then
        qemu_agent stop >"${current_profile:-$evidence}/cleanup-stop.log" \
            2>&1 || true
    fi
    if [[ -n $current_profile && -d $current_runtime ]]; then
        mkdir -p "$current_profile/qemu"
        cp -a "$current_runtime/." "$current_profile/qemu/"
        printf 'cleanup_reason=%s\nrun_complete=NO\n' "$reason" >> \
            "$current_profile/profile.env"
    fi
    if [[ -d $current_runtime &&
          $current_runtime == "$runtime_parent"/hobbyos-build-profile-* ]]; then
        rm -r -- "$current_runtime"
    fi
    current_runtime=
    current_profile=
    current_pid=
}

trap 'cleanup_active_vm exit' EXIT
trap 'cleanup_active_vm interrupt; exit 130' INT
trap 'cleanup_active_vm terminate; exit 143' TERM

capture_sources()
{
    ensure_evidence
    local files=(
        makefile
        bootloader/main.c
        bootloader/src/bmp.c
        kernel/kernel.c
        kernel/src/core/build_profile.c
        kernel/src/core/build_profile.h
        kernel/src/core/entry.S
        kernel/src/core/idt.c
        kernel/src/core/idt.h
        kernel/src/core/interrupt_stubs.S
        kernel/src/core/interrupts.c
        kernel/src/core/interrupts.h
        kernel/src/core/panic.h
        kernel/src/core/scheduler.c
        kernel/src/cpu/arch_selftest.h
        kernel/src/cpu/cpu.c
        kernel/src/drivers/keyboard.c
        kernel/src/drivers/keyboard.h
        kernel/src/drivers/pci.c
        kernel/src/drivers/pci.h
        kernel/src/drivers/pci_descriptors.c
        kernel/src/drivers/ps2.c
        kernel/src/drivers/ps2.h
        kernel/src/drivers/timer.h
        kernel/src/drivers/usb/xhci/usb_hotplug.c
        kernel/src/drivers/usb/xhci/usb_hotplug.h
        kernel/src/drivers/usb/xhci/usb_hub.h
        kernel/src/drivers/usb/xhci/xhci.c
        kernel/src/drivers/usb/xhci/xhci.h
        kernel/src/graphics/console.h
        kernel/src/graphics/terminal.c
        kernel/src/graphics/terminal.h
        kernel/src/libc/format_selftest.c
        kernel/src/libc/memory.c
        kernel/src/libc/memory.h
        kernel/src/memory/gdt.c
        kernel/src/memory/gdt.h
        kernel/src/memory/paging.c
        kernel/src/memory/paging.h
        kernel/src/shell/commands/cmd_echo.h
        kernel/src/smp/smp_boot.c
        kernel/src/smp/smp_boot.h
        kernel/src/smp/ap_entry.S
        kernel/src/smp/trampoline.S
        kernel/src/shell/commands/cmd_profiletest.c
        kernel/src/shell/commands/cmd_profiletest.h
        kernel/src/shell/commands/cmd_schedtest.c
        kernel/src/shell/commands/cmd_taskmantest.c
        kernel/src/shell/commands/taskman_view.c
        kernel/src/shell/commands/registry.c
        kernel/src/shell/shell.c
        kernel/src/shell/shell.h
        kernel/src/timer/hpet.c
        kernel/src/timer/hpet.h
        scripts/build-profile-memory-host.c
        scripts/test-build-profile.sh
        scripts/verify-arch-contract.py
        scripts/verify-build-profile.py
        scripts/verify-section-layout.py
        scripts/verify-production-test-policy.sh
    )
    for file in "${files[@]}"; do
        mkdir -p "$evidence/source/$(dirname "$file")"
        cp "$file" "$evidence/source/$file"
    done
    (cd "$evidence/source" && sha256sum "${files[@]}") > \
        "$evidence/source/SHA256SUMS"
    {
        printf 'schema=1\n'
        printf 'observed_at=%s\n' "$(date --iso-8601=seconds)"
        printf 'branch=%s\n' "$(git branch --show-current)"
        printf 'head=%s\n' "$(git rev-parse HEAD)"
        printf 'schedulable_cpus=%s\n' "$(nproc)"
        printf 'evidence=%s\n' "$evidence"
    } >"$evidence/environment.env"
    git status --short >"$evidence/worktree-status.txt"
}

static_checks()
{
    capture_sources
    mkdir -p "$evidence/static"
    run_logged source "$evidence/static/source.log" \
        python3 scripts/verify-build-profile.py source "$root"
    echo "BUILD_PROFILE_STATIC: PASS checks=6"
}

host_memory_checks()
{
    mkdir -p "$evidence/host"
    run_logged memory-object-host "$evidence/host/memory-object-build.log" \
        cc -std=c11 -O2 -Wall -Wextra -Werror -fno-builtin \
        -fno-strict-aliasing \
        -fno-tree-loop-distribute-patterns -mgeneral-regs-only \
        -Dmemcpy=hobby_memcpy -Dmemset=hobby_memset \
        -Dmemmove=hobby_memmove -Dmemcmp=hobby_memcmp \
        -c kernel/src/libc/memory.c \
        -o "$evidence/host/memory-under-test.o"
    run_logged memory-harness-host "$evidence/host/memory-harness-build.log" \
        cc -std=c11 -O2 -Wall -Wextra -Werror -fno-builtin \
        scripts/build-profile-memory-host.c \
        "$evidence/host/memory-under-test.o" \
        -o "$evidence/host/memory-host"
    run_logged memory-host "$evidence/host/memory-host.log" \
        timeout 300s "$evidence/host/memory-host"
    objdump -d -Mintel "$evidence/host/memory-under-test.o" > \
        "$evidence/host/memory-under-test.disassembly.txt"
    echo "BUILD_PROFILE_HOST_MEMORY: PASS"
}

build_checks()
{
    capture_sources
    mkdir -p "$evidence/build" "$evidence/candidate"
    host_memory_checks
    run_logged deps-check "$evidence/build/deps-check.log" make deps-check
    run_logged kernel-check "$evidence/build/kernel-check.log" \
        make kernel-check "JOBS=$jobs" SELFTEST=1 SELFTEST_AUTORUN=0 \
        DEBUG_ASSERT=1
    run_logged stack-check "$evidence/build/stack-check.log" \
        make stack-check "JOBS=$jobs" SELFTEST=1 SELFTEST_AUTORUN=0 \
        DEBUG_ASSERT=1
    run_logged image "$evidence/build/image.log" \
        make image "JOBS=$jobs" SELFTEST=1 SELFTEST_AUTORUN=0 \
        DEBUG_ASSERT=1
    cp --reflink=auto kernel.elf BOOTX64.EFI hobbyos.img \
        "$evidence/candidate/"
    (cd "$evidence/candidate" && \
        sha256sum kernel.elf BOOTX64.EFI hobbyos.img) > \
        "$evidence/candidate/SHA256SUMS"
    nm -an kernel.elf >"$evidence/candidate/kernel.symbols.txt"
    strings kernel.elf >"$evidence/candidate/kernel.strings.txt"
    objdump -d -Mintel kernel/src/core/entry.o > \
        "$evidence/candidate/entry.disassembly.txt"
    objdump -d -Mintel kernel/src/core/interrupt_stubs.o > \
        "$evidence/candidate/interrupt-stubs.disassembly.txt"
    objdump -d -Mintel kernel/src/smp/trampoline.o > \
        "$evidence/candidate/trampoline.disassembly.txt"
    objdump -d -Mintel kernel/src/smp/ap_entry.o > \
        "$evidence/candidate/ap-entry.disassembly.txt"
    objdump -d -Mintel kernel/src/libc/memory.o > \
        "$evidence/candidate/memory.disassembly.txt"
    run_logged section-layout-candidate \
        "$evidence/build/section-layout-candidate.log" \
        python3 scripts/verify-section-layout.py elf \
        "$evidence/candidate/kernel.elf"
    rg -q 'cmd_profiletest' "$evidence/candidate/kernel.symbols.txt"
    run_logged kernel-check-all-tests \
        "$evidence/build/kernel-check-all-tests.log" \
        make kernel-check "JOBS=$jobs" SELFTEST=1 SELFTEST_AUTORUN=0 \
        DEBUG_ASSERT=1 FORMAT_TEST=1 ASSERT_TEST=1 ARCH_TEST=1
    echo "BUILD_PROFILE_BUILD: PASS candidate=$(sha256sum "$evidence/candidate/kernel.elf" | awk '{print $1}')"
}

ensure_candidate()
{
    if [[ ! -s $evidence/candidate/kernel.elf ||
          ! -s $evidence/candidate/hobbyos.img ]]; then
        build_checks
    fi
}

build_input_layout()
{
    local binary="$evidence/candidate/command-transport-layout-host"
    run_logged input-layout-build "$evidence/build/input-layout-build.log" \
        cc -std=c11 -Wall -Wextra -Werror -I"$root" \
        scripts/command-transport-layout.c -o "$binary"
    run_logged input-layout-run "$evidence/build/input-layout-run.log" \
        "$binary"
    tail -n 1 "$evidence/build/input-layout-run.log" > \
        "$evidence/candidate/command-input-layout.json"
    unlink "$binary"
}

create_gdb_qemu_wrapper()
{
    local wrapper="$current_runtime/qemu-with-gdb"
    local real_qemu gdb_socket="$current_runtime/gdb.sock"
    real_qemu=$(command -v qemu-system-x86_64)
    python3 - "$wrapper" "$real_qemu" "$gdb_socket" \
        "$current_runtime/qemu-argv.txt" <<'PY'
import shlex,sys
wrapper,real_qemu,gdb_socket,argv_log=sys.argv[1:]
body=f'''#!/usr/bin/env bash
set -euo pipefail
if [[ ${{1:-}} == --version ]]; then
  exec {shlex.quote(real_qemu)} "$@"
fi
printf '%q ' {shlex.quote(real_qemu)} "$@" -chardev {shlex.quote('socket,path='+gdb_socket+',server=on,wait=off,id=build_profile_gdb')} -gdb chardev:build_profile_gdb > {shlex.quote(argv_log)}
printf '\n' >> {shlex.quote(argv_log)}
exec {shlex.quote(real_qemu)} "$@" -chardev {shlex.quote('socket,path='+gdb_socket+',server=on,wait=off,id=build_profile_gdb')} -gdb chardev:build_profile_gdb
'''
open(wrapper,'w',encoding='utf-8').write(body)
PY
    chmod 700 "$wrapper"
    printf '%s\n' "$wrapper"
}

run_frame()
{
    local sequence=$1 payload=$2 timeout_seconds=$3
    local candidate_hash vm_id
    candidate_hash=$(sha256sum "$evidence/candidate/kernel.elf" | awk '{print $1}')
    vm_id=$(basename "$current_runtime")
    python3 scripts/foundation-qemu.py frame \
        --root "$root" --runtime "$current_runtime" \
        --gdb-socket "$current_runtime/gdb.sock" \
        --elf "$evidence/candidate/kernel.elf" \
        --input-layout "$evidence/candidate/command-input-layout.json" \
        --record "$current_profile/commands.jsonl" \
        --transport-log "$current_profile/transport-${sequence}.log" \
        --profile-id "$(basename "$current_profile")" --vm-id "$vm_id" \
        --candidate-sha256 "$candidate_hash" --sequence "$sequence" \
        --payload "$payload" --timeout "$timeout_seconds" --allowed-status 0 \
        --input-profile framed >"$current_profile/command-${sequence}.json"
}

runtime_tests()
{
    ensure_evidence
    [[ -r /dev/kvm && -w /dev/kvm ]] || {
        echo "BUILD_PROFILE_RUNTIME: FAIL reason=kvm-unavailable" >&2
        return 1
    }
    ensure_candidate
    build_input_layout
    local name=q35-kvm-smp4 wrapper failed=0 cleanup=PASS
    local candidate_hash image_hash started elapsed record_status
    current_profile="$evidence/runtime/$name"
    mkdir -p "$current_profile" "$runtime_parent"
    current_runtime=$(mktemp -d \
        "$runtime_parent/hobbyos-build-profile-$name.XXXXXX")
    chmod 700 "$current_runtime"
    cp --reflink=auto "$evidence/candidate/hobbyos.img" \
        "$current_profile/working.img"
    : >"$current_profile/commands.jsonl"
    candidate_hash=$(sha256sum "$evidence/candidate/kernel.elf" | awk '{print $1}')
    image_hash=$(sha256sum "$current_profile/working.img" | awk '{print $1}')
    {
        printf 'schema=1\nname=%s\nmachine=q35\naccel=kvm\nsmp=4\n' "$name"
        printf 'candidate_sha256=%s\nimage_sha256_before=%s\n' \
            "$candidate_hash" "$image_hash"
        printf 'cleanup=PENDING\n'
    } >"$current_profile/profile.env"
    wrapper=$(create_gdb_qemu_wrapper)
    started=$(date +%s)
    if ! QEMU="$wrapper" QEMU_RUNTIME="$current_runtime" \
        HOBBYOS_IMAGE="$current_profile/working.img" \
        HOBBYOS_KERNEL="$evidence/candidate/kernel.elf" \
        SMP=4 MACHINE=q35 ACCEL=kvm MEM=2G \
        scripts/qemu-agent.sh start >"$current_profile/start.log" 2>&1; then
        cat "$current_profile/start.log" >&2
        return 1
    fi
    current_pid=$(<"$current_runtime/qemu.pid")
    python3 scripts/foundation-qemu.py wait-ready \
        --runtime "$current_runtime" \
        --marker '[BOOT][RUNTIME_READY] PASS cpus=4/4' --timeout 240 \
        --output "$current_profile/runtime-ready.json" || failed=1
    if ((failed == 0)); then
        python3 scripts/foundation-qemu.py wait-ready \
            --runtime "$current_runtime" \
            --marker '[BOOT][TEST_READY] PASS autorun=0 selftests=0' \
            --timeout 30 --output "$current_profile/test-ready.json" || failed=1
    fi
    if ((failed == 0)); then
        run_frame 1 "profiletest abi" 120 || failed=1
    fi
    if ((failed == 0)); then
        run_frame 2 "profiletest memory" 300 || failed=1
    fi
    if ((failed == 0)); then
        set +e
        python3 scripts/verify-build-profile.py record \
            "$current_runtime/qemu-serial.log" > \
            "$current_profile/runtime-record-verify.log" 2>&1
        record_status=$?
        set -e
        printf '%s\n' "$record_status" > \
            "$current_profile/runtime-record-verify.exit-code.txt"
        ((record_status == 0)) || failed=1
    fi

    qemu_agent status >"$current_profile/status-before-stop.log" 2>&1 || \
        cleanup=FAIL
    qemu_agent stop >"$current_profile/stop.log" 2>&1 || cleanup=FAIL
    if pid_active "$current_pid" || [[ -e $current_runtime/qemu.pid ||
                                      -S $current_runtime/hmp.sock ]]; then
        cleanup=FAIL
    fi
    elapsed=$(($(date +%s) - started))
    printf 'host_elapsed_seconds=%s\ncleanup=%s\nrun_complete=YES\n' \
        "$elapsed" "$cleanup" >>"$current_profile/profile.env"
    mkdir -p "$current_profile/qemu"
    cp -a "$current_runtime/." "$current_profile/qemu/"
    rm -r -- "$current_runtime"
    current_runtime=
    current_profile=
    current_pid=
    [[ $cleanup == PASS && $failed == 0 ]]
    echo "BUILD_PROFILE_RUNTIME: PASS profiles=1"
}

production_test()
{
    capture_sources
    mkdir -p "$evidence/production"
    run_logged production-image \
        "$evidence/production/production-image.log" \
        make production-image "JOBS=$jobs"
    cp --reflink=auto kernel.elf BOOTX64.EFI hobbyos.img \
        "$evidence/production/"
    strings kernel.elf >"$evidence/production/kernel.strings.txt"
    nm -an kernel.elf >"$evidence/production/kernel.symbols.txt"
    ! rg -qi 'profiletest' "$evidence/production/kernel.strings.txt"
    ! rg -qi 'profiletest' "$evidence/production/kernel.symbols.txt"
    run_logged policy "$evidence/production/policy.log" \
        scripts/verify-production-test-policy.sh
    run_logged section-layout-production \
        "$evidence/production/section-layout-production.log" \
        python3 scripts/verify-section-layout.py elf \
        "$evidence/production/kernel.elf"
    (cd "$evidence/production" && \
        sha256sum kernel.elf BOOTX64.EFI hobbyos.img) > \
        "$evidence/production/SHA256SUMS"
    echo "BUILD_PROFILE_PRODUCTION: PASS selftest_absent=1"
}

fixture_tests()
{
    capture_sources
    [[ ! -e $evidence/fixtures ]] || {
        echo "fixture directory already exists: $evidence/fixtures" >&2
        return 1
    }
    python3 scripts/verify-build-profile.py fixtures "$evidence/fixtures"
}

verify_evidence()
{
    python3 scripts/verify-build-profile.py all "$evidence"
}

case $mode in
    static) static_checks ;;
    build) build_checks ;;
    runtime) runtime_tests ;;
    production) production_test ;;
    fixtures) fixture_tests ;;
    verify) verify_evidence ;;
    all)
        static_checks
        build_checks
        runtime_tests
        production_test
        fixture_tests
        verify_evidence
        ;;
    *) usage ;;
esac

echo "BUILD_PROFILE_GATE: PASS mode=$mode evidence=$evidence"
