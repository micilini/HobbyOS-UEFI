#!/usr/bin/env bash
set -Eeuo pipefail

export LC_ALL=C
export PYTHONDONTWRITEBYTECODE=1

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"

mode=${1:-}
jobs=${JOBS:-$(nproc)}
stamp=$(date '+%Y%m%d-%H%M%S-%z')
evidence=${ALLOC_HARDENING_EVIDENCE_DIR:-"artifacts/build/alloc-hardening/$stamp"}
evidence=$(realpath -m "$evidence")
runtime_parent=${ALLOC_HARDENING_RUNTIME_PARENT:-${TMPDIR:-/tmp}}
current_runtime=
current_profile=
current_pid=

usage()
{
    echo "usage: $0 host|build|runtime|production|fixtures|verify|all" >&2
    exit 2
}

ensure_evidence()
{
    [[ ! -L $evidence ]] || {
        echo "allocator evidence root must not be a symlink" >&2
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
        qemu_agent stop >"${current_profile:-$evidence}/cleanup-stop.log" 2>&1 || true
    fi
    if [[ -n $current_profile && -d $current_runtime ]]; then
        mkdir -p "$current_profile/qemu"
        cp -a "$current_runtime/." "$current_profile/qemu/"
        printf 'cleanup_reason=%s\nrun_complete=NO\n' "$reason" >> \
            "$current_profile/profile.env"
    fi
    if [[ -d $current_runtime &&
          $current_runtime == "$runtime_parent"/hobbyos-alloc-hardening-* ]]; then
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
        kernel/src/memory/heap.c
        kernel/src/memory/heap.h
        kernel/src/memory/pmem.c
        kernel/src/memory/pmem.h
        kernel/src/utils/bitmap.c
        kernel/src/utils/bitmap.h
        kernel/src/utils/bits.h
        kernel/src/shell/commands/cmd_alloctest.c
        kernel/src/shell/commands/cmd_alloctest.h
        kernel/src/shell/commands/registry.c
        scripts/alloc-hardening-host.c
        scripts/test-alloc-hardening.sh
        scripts/verify-alloc-hardening.py
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
        printf 'tree=%s\n' "$(git rev-parse HEAD^{tree})"
        printf 'schedulable_cpus=%s\n' "$(nproc)"
        printf 'evidence=%s\n' "$evidence"
    } >"$evidence/environment.env"
    git status --short >"$evidence/worktree-status.txt"
}

host_profile()
{
    local name=$1
    shift
    local dir="$evidence/host/$name"
    local binary="$dir/alloc-hardening-host"
    mkdir -p "$dir"
    run_logged "host-$name-compile" "$dir/compile.log" \
        gcc -std=gnu11 -O2 -g -Wall -Wextra -Werror "$@" \
        scripts/alloc-hardening-host.c kernel/src/utils/bitmap.c -o "$binary"
    local status=0
    set +e
    if [[ $name == ubsan ]]; then
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
            timeout 90 "$binary" >"$dir/run.log" 2>&1
        status=$?
    else
        timeout 90 "$binary" >"$dir/run.log" 2>&1
        status=$?
    fi
    set -e
    printf '%s\n' "$status" >"$dir/exit-code.txt"
    ((status == 0)) || {
        cat "$dir/run.log" >&2
        return "$status"
    }
    grep -Fxq '[ALLOCTEST][HOST] PASS sizes=7 patterns=3 reference_match=1 word_search=1 ranges=1 overflow=1' \
        "$dir/run.log"
    sha256sum "$binary" >"$dir/SHA256SUMS"
}

host_tests()
{
    capture_sources
    host_profile normal
    host_profile ubsan -fsanitize=undefined -fno-sanitize-recover=all
    echo "ALLOC_HARDENING_HOST: PASS profiles=2"
}

build_checks()
{
    capture_sources
    mkdir -p "$evidence/build" "$evidence/candidate"
    run_logged deps-check "$evidence/build/deps-check.log" make deps-check
    run_logged kernel-check "$evidence/build/kernel-check.log" \
        make kernel-check "JOBS=$jobs" SELFTEST=1 SELFTEST_AUTORUN=0
    run_logged stack-check "$evidence/build/stack-check.log" \
        make stack-check "JOBS=$jobs" SELFTEST=1 SELFTEST_AUTORUN=0
    run_logged image "$evidence/build/image.log" \
        make image "JOBS=$jobs" SELFTEST=1 SELFTEST_AUTORUN=0
    cp --reflink=auto kernel.elf BOOTX64.EFI hobbyos.img "$evidence/candidate/"
    (cd "$evidence/candidate" && \
        sha256sum kernel.elf BOOTX64.EFI hobbyos.img) > \
        "$evidence/candidate/SHA256SUMS"
    nm -an kernel.elf >"$evidence/candidate/kernel.symbols.txt"
    strings kernel.elf >"$evidence/candidate/kernel.strings.txt"
    rg -q 'cmd_alloctest' "$evidence/candidate/kernel.symbols.txt"
    echo "ALLOC_HARDENING_BUILD: PASS candidate=$(sha256sum kernel.elf | awk '{print $1}')"
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
    run_logged input-layout-run "$evidence/build/input-layout-run.log" "$binary"
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
printf '%q ' {shlex.quote(real_qemu)} "$@" -chardev {shlex.quote('socket,path='+gdb_socket+',server=on,wait=off,id=alloc_hardening_gdb')} -gdb chardev:alloc_hardening_gdb > {shlex.quote(argv_log)}
printf '\n' >> {shlex.quote(argv_log)}
exec {shlex.quote(real_qemu)} "$@" -chardev {shlex.quote('socket,path='+gdb_socket+',server=on,wait=off,id=alloc_hardening_gdb')} -gdb chardev:alloc_hardening_gdb
'''
open(wrapper,'w',encoding='utf-8').write(body)
PY
    chmod 700 "$wrapper"
    printf '%s\n' "$wrapper"
}

run_frame()
{
    local candidate_hash vm_id
    candidate_hash=$(sha256sum "$evidence/candidate/kernel.elf" | awk '{print $1}')
    vm_id=$(basename "$current_runtime")
    python3 scripts/foundation-qemu.py frame \
        --root "$root" --runtime "$current_runtime" \
        --gdb-socket "$current_runtime/gdb.sock" \
        --elf "$evidence/candidate/kernel.elf" \
        --input-layout "$evidence/candidate/command-input-layout.json" \
        --record "$current_profile/commands.jsonl" \
        --transport-log "$current_profile/transport-1.log" \
        --profile-id "$(basename "$current_profile")" --vm-id "$vm_id" \
        --candidate-sha256 "$candidate_hash" --sequence 1 \
        --payload "alloctest hardening" --timeout 120 --allowed-status 0 \
        --input-profile framed >"$current_profile/command-1.json"
}

runtime_tests()
{
    ensure_evidence
    [[ -r /dev/kvm && -w /dev/kvm ]] || {
        echo "ALLOC_HARDENING_RUNTIME: FAIL reason=kvm-unavailable" >&2
        return 1
    }
    ensure_candidate
    build_input_layout
    local name=q35-kvm-smp4 wrapper failed=0 cleanup=PASS
    local candidate_hash image_hash started elapsed
    current_profile="$evidence/runtime/$name"
    mkdir -p "$current_profile" "$runtime_parent"
    current_runtime=$(mktemp -d "$runtime_parent/hobbyos-alloc-hardening-$name.XXXXXX")
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
        run_frame || failed=1
    fi
    if ((failed == 0)); then
        set +e
        python3 scripts/verify-alloc-hardening.py record \
            "$current_runtime/qemu-serial.log" \
            >"$current_profile/runtime-record-verify.log" 2>&1
        local record_status=$?
        set -e
        printf '%s\n' "$record_status" > \
            "$current_profile/runtime-record-verify.exit-code.txt"
        ((record_status == 0)) || failed=1
    fi

    qemu_agent status >"$current_profile/status-before-stop.log" 2>&1 || cleanup=FAIL
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
    echo "ALLOC_HARDENING_RUNTIME: PASS profiles=1"
}

production_test()
{
    capture_sources
    mkdir -p "$evidence/production"
    run_logged production-image "$evidence/production/production-image.log" \
        make production-image "JOBS=$jobs"
    cp --reflink=auto kernel.elf BOOTX64.EFI hobbyos.img "$evidence/production/"
    strings kernel.elf >"$evidence/production/kernel.strings.txt"
    nm -an kernel.elf >"$evidence/production/kernel.symbols.txt"
    ! rg -qi 'alloctest' "$evidence/production/kernel.strings.txt"
    ! rg -qi 'alloctest' "$evidence/production/kernel.symbols.txt"
    run_logged policy "$evidence/production/policy.log" \
        scripts/verify-production-test-policy.sh
    (cd "$evidence/production" && \
        sha256sum kernel.elf BOOTX64.EFI hobbyos.img) > \
        "$evidence/production/SHA256SUMS"
    echo "ALLOC_HARDENING_PRODUCTION: PASS selftest_absent=1"
}

fixture_tests()
{
    capture_sources
    [[ ! -e $evidence/fixtures ]] || {
        echo "fixture directory already exists: $evidence/fixtures" >&2
        return 1
    }
    python3 scripts/verify-alloc-hardening.py fixtures "$evidence/fixtures"
}

verify_evidence()
{
    python3 scripts/verify-alloc-hardening.py all "$evidence"
}

case $mode in
    host) host_tests ;;
    build) build_checks ;;
    runtime) runtime_tests ;;
    production) production_test ;;
    fixtures) fixture_tests ;;
    verify) verify_evidence ;;
    all)
        host_tests
        build_checks
        runtime_tests
        production_test
        fixture_tests
        verify_evidence
        ;;
    *) usage ;;
esac

echo "ALLOC_HARDENING_GATE: PASS mode=$mode evidence=$evidence"
