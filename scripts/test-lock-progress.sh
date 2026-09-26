#!/usr/bin/env bash
set -Eeuo pipefail

export LC_ALL=C
export PYTHONDONTWRITEBYTECODE=1

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"

mode=${1:-}
jobs=${JOBS:-$(nproc)}
stamp=$(date '+%Y%m%d-%H%M%S-%z')
evidence=${LOCK_PROGRESS_EVIDENCE_DIR:-"artifacts/build/lock-progress/$stamp"}
evidence=$(realpath -m "$evidence")
runtime_parent=${LOCK_PROGRESS_RUNTIME_PARENT:-${TMPDIR:-/tmp}}
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
        echo "lock progress evidence root must not be a symlink" >&2
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
    if [[ -d $current_runtime && $current_runtime == "$runtime_parent"/hobbyos-lock-progress-* ]]; then
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
        kernel/src/core/spinlock.c
        kernel/src/core/spinlock.h
        kernel/src/shell/commands/cmd_locktest.c
        kernel/src/shell/commands/cmd_locktest.h
        kernel/src/shell/commands/registry.c
        scripts/lock-progress-host.c
        scripts/test-lock-progress.sh
        scripts/verify-lock-progress.py
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
    local binary="$dir/lock-progress-host"
    mkdir -p "$dir"
    run_logged "host-$name-compile" "$dir/compile.log" \
        gcc -std=gnu11 -O2 -g -Wall -Wextra -Werror \
        -DHOBBYOS_SELFTEST=1 -pthread "$@" \
        scripts/lock-progress-host.c kernel/src/core/spinlock.c -o "$binary"
    local status=0
    set +e
    if [[ $name == ubsan ]]; then
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
            timeout 30 "$binary" >"$dir/run.log" 2>&1
        status=$?
    else
        timeout 30 "$binary" >"$dir/run.log" 2>&1
        status=$?
    fi
    set -e
    printf '%s\n' "$status" >"$dir/exit-code.txt"
    ((status == 0)) || {
        cat "$dir/run.log" >&2
        return "$status"
    }
    grep -Eq '^\[LOCKTEST\]\[HOST\] PASS .* progress=1 .*max_distance=[0-7]$' \
        "$dir/run.log"
    sha256sum "$binary" >"$dir/SHA256SUMS"
}

host_tests()
{
    capture_sources
    host_profile normal
    host_profile ubsan -fsanitize=undefined -fno-sanitize-recover=all
    echo "LOCK_PROGRESS_HOST: PASS profiles=2"
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
    rg -q 'cmd_locktest|spinlock_test_observer' \
        "$evidence/candidate/kernel.symbols.txt"
    echo "LOCK_PROGRESS_BUILD: PASS candidate=$(sha256sum kernel.elf | awk '{print $1}')"
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
    rm -f -- "$binary"
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
printf '%q ' {shlex.quote(real_qemu)} "$@" -chardev {shlex.quote('socket,path='+gdb_socket+',server=on,wait=off,id=lock_progress_gdb')} -gdb chardev:lock_progress_gdb > {shlex.quote(argv_log)}
printf '\n' >> {shlex.quote(argv_log)}
exec {shlex.quote(real_qemu)} "$@" -chardev {shlex.quote('socket,path='+gdb_socket+',server=on,wait=off,id=lock_progress_gdb')} -gdb chardev:lock_progress_gdb
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
        --transport-log "$current_profile/transport-$sequence.log" \
        --profile-id "$(basename "$current_profile")" --vm-id "$vm_id" \
        --candidate-sha256 "$candidate_hash" --sequence "$sequence" \
        --payload "$payload" --timeout "$timeout_seconds" --allowed-status 0 \
        --input-profile framed >"$current_profile/command-$sequence.json"
}

run_runtime_profile()
{
    local smp=$1 iterations=$2 wrapper failed=0
    local name="q35-kvm-smp$smp"
    local candidate_hash image_hash elapsed started
    current_profile="$evidence/runtime/$name"
    mkdir -p "$current_profile" "$runtime_parent"
    current_runtime=$(mktemp -d "$runtime_parent/hobbyos-lock-progress-$name.XXXXXX")
    chmod 700 "$current_runtime"
    current_pid=
    cp --reflink=auto "$evidence/candidate/hobbyos.img" \
        "$current_profile/working.img"
    : >"$current_profile/commands.jsonl"
    candidate_hash=$(sha256sum "$evidence/candidate/kernel.elf" | awk '{print $1}')
    image_hash=$(sha256sum "$current_profile/working.img" | awk '{print $1}')
    {
        printf 'schema=1\nname=%s\nmachine=q35\naccel=kvm\nsmp=%s\n' \
            "$name" "$smp"
        printf 'candidate_sha256=%s\nimage_sha256_before=%s\n' \
            "$candidate_hash" "$image_hash"
        printf 'host_schedulable_cpus=%s\nfairness_authority=%s\n' \
            "$(nproc)" "$([[ $smp == 8 ]] && echo AUTHORITATIVE || echo FUNCTIONAL_ONLY)"
        printf 'cleanup=PENDING\n'
    } >"$current_profile/profile.env"
    wrapper=$(create_gdb_qemu_wrapper)
    started=$(date +%s)
    if ! QEMU="$wrapper" QEMU_RUNTIME="$current_runtime" \
        HOBBYOS_IMAGE="$current_profile/working.img" \
        HOBBYOS_KERNEL="$evidence/candidate/kernel.elf" \
        SMP="$smp" MACHINE=q35 ACCEL=kvm MEM=2G \
        scripts/qemu-agent.sh start >"$current_profile/start.log" 2>&1; then
        cat "$current_profile/start.log" >&2
        return 1
    fi
    current_pid=$(cat "$current_runtime/qemu.pid")
    python3 scripts/foundation-qemu.py wait-ready \
        --runtime "$current_runtime" \
        --marker "[BOOT][RUNTIME_READY] PASS cpus=$smp/$smp" --timeout 240 \
        --output "$current_profile/runtime-ready.json" || failed=1
    if ((failed == 0)); then
        python3 scripts/foundation-qemu.py wait-ready \
            --runtime "$current_runtime" \
            --marker "[BOOT][TEST_READY] PASS autorun=0 selftests=0" \
            --timeout 30 --output "$current_profile/test-ready.json" || failed=1
    fi
    if ((failed == 0)); then
        run_frame 1 "locktest progress $smp $iterations" 360 || failed=1
    fi
    if ((failed == 0 && smp == 8)); then
        run_frame 2 "accounttest clock-smp 32 1000000" 360 || failed=1
    fi

    local cleanup=PASS
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
}

runtime_tests()
{
    ensure_evidence
    [[ -r /dev/kvm && -w /dev/kvm ]] || {
        echo "LOCK_PROGRESS_RUNTIME: FAIL reason=kvm-unavailable" >&2
        return 1
    }
    ensure_candidate
    build_input_layout
    run_runtime_profile 8 9000
    run_runtime_profile 24 1000
    echo "LOCK_PROGRESS_RUNTIME: PASS profiles=2"
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
    ! rg -qi 'locktest' "$evidence/production/kernel.strings.txt"
    ! rg -qi 'locktest' "$evidence/production/kernel.symbols.txt"
    run_logged policy "$evidence/production/policy.log" \
        scripts/verify-production-test-policy.sh
    (cd "$evidence/production" && \
        sha256sum kernel.elf BOOTX64.EFI hobbyos.img) > \
        "$evidence/production/SHA256SUMS"
    echo "LOCK_PROGRESS_PRODUCTION: PASS selftest_absent=1"
}

fixture_tests()
{
    capture_sources
    rm -rf -- "$evidence/fixtures"
    python3 scripts/verify-lock-progress.py fixtures "$evidence/fixtures"
}

verify_evidence()
{
    python3 scripts/verify-lock-progress.py all "$evidence"
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

echo "LOCK_PROGRESS_GATE: PASS mode=$mode evidence=$evidence"
