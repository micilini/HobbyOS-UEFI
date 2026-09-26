#!/usr/bin/env bash
set -Eeuo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

timestamp=$(date '+%Y%m%d-%H%M%S-%z')
evidence=${SMP_OBSERVATION_EVIDENCE_DIR:-"artifacts/build/smp-observation/$timestamp/focal"}
base_commit=${SMP_OBSERVATION_BASE_COMMIT:-$(git rev-parse HEAD)}
host_cc=${CC:-gcc}

usage()
{
    echo "usage: $0 {preflight|static|host|fixtures|qemu|verify|all}" >&2
    exit 2
}

ensure_evidence()
{
    [[ ! -L $evidence ]] || {
        echo "SMP observation evidence root must not be a symlink" >&2
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
    local status
    mkdir -p "$(dirname "$log")"
    record_command "$label" "$@"
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

preflight()
{
    ensure_evidence
    mkdir -p "$evidence/preflight"
    for tool in awk gcc git nm python3 sha256sum timeout; do
        command -v "$tool" >/dev/null
    done
    {
        printf 'schema=1\n'
        printf 'observed_at=%s\n' "$(date --iso-8601=seconds)"
        printf 'delivery_dir=%s\n' "$root"
        printf 'repo_root=%s\n' "$(git rev-parse --show-toplevel)"
        printf 'branch=%s\n' "$(git branch --show-current)"
        printf 'head=%s\n' "$(git rev-parse HEAD)"
        printf 'tree=%s\n' "$(git rev-parse HEAD^{tree})"
        printf 'parent=%s\n' "$(git rev-parse HEAD^)"
        printf 'implementation_base_commit=%s\n' "$base_commit"
        printf 'host_schedulable_cpus=%s\n' "$(nproc)"
        printf 'kvm_available=%s\n' "$([[ -r /dev/kvm && -w /dev/kvm ]] && printf YES || printf NO)"
        printf 'gcc=%s\n' "$(gcc --version | head -1)"
        printf 'python=%s\n' "$(python3 --version)"
    } >"$evidence/preflight/environment.env"
    git status --short >"$evidence/preflight/worktree-status.txt"
    df -B1 . /dev/shm >"$evidence/preflight/disk-space.txt"
    sha256sum AGENTS.md docs/foundation/validation-policy.md \
        docs/foundation/architecture.md >"$evidence/preflight/contracts.sha256"
    echo "SMP_OBSERVATION_PREFLIGHT: PASS evidence=$evidence"
}

static_checks()
{
    ensure_evidence
    mkdir -p "$evidence/static"
    bash -n scripts/test-irq-snapshot-observation.sh
    bash -n scripts/test-panic-lock-contention.sh
    PYTHONDONTWRITEBYTECODE=1 python3 - <<'PY'
from pathlib import Path
for name in ("scripts/panic-qemu.py", "scripts/verify-smp-observation.py"):
    compile(Path(name).read_text(), name, "exec")
PY
    git diff --check
    python3 - "$base_commit" "$evidence/static/source-audit.json" <<'PY'
import json
from pathlib import Path
import subprocess
import sys

base, output = sys.argv[1:]
source = Path("kernel/src/shell/commands/cmd_irq.c").read_text()
panic = Path("scripts/panic-qemu.py").read_text()
runner = Path("scripts/test-panic-lock-contention.sh").read_text()
foundation_runner = Path("scripts/test-foundation-regression.sh").read_text()
changed = set(subprocess.check_output(
    ["git", "diff", "--name-only", base], text=True).splitlines())
allowed = {
    "kernel/src/shell/commands/cmd_irq.c",
    "scripts/irq-snapshot-host.c",
    "scripts/test-irq-snapshot-observation.sh",
    "scripts/verify-smp-observation.py",
    "scripts/panic-qemu.py",
    "scripts/test-panic-lock-contention.sh",
    "scripts/test-foundation-regression.sh",
    "docs/foundation/architecture.md",
    "docs/foundation/validation-policy.md",
}
boot_start = source.index("static int irq_boot(void)")
boot_end = source.index("\nint cmd_irq(", boot_start)
boot = source[boot_start:boot_end]
checks = {
    "scope": changed <= allowed,
    "shared_budget_5000":
        "#define IRQ_DIAGNOSTIC_SNAPSHOT_BUDGET_MS 5000u" in source,
    "bounded_record": "#define IRQ_BOOT_RECORD_CAPACITY 768u" in source,
    "bounded_topology": "boot.cpus_expected > HOBBYOS_MAX_CPUS" in boot,
    "private_storage": "kmalloc(samples_bytes)" in boot,
    "storage_cleanup": boot.count("kfree(samples);") == 2,
    "acquisition_precedes_publication":
        boot.index("for (cpu_slot_t slot = 0;") <
        boot.index("bool formatted = irq_boot_publish_record("),
    "single_record_serial": "serial_write_all(record);" in source,
    "boot_observation": "def observe_boot_readiness(" in panic,
    "boot_only_does_not_trigger": "if args.observe_boot_only:" in panic,
    "smp24_deadline":
        "common_kvm_smp24=(--machine q35 --accel kvm --memory 2G --boot-timeout 120" in runner,
    "smp24_checkpoint": "--boot-checkpoint 45 --post-timeout 25" in runner,
    "tcg_post_limit": "--post-timeout 60" in runner,
    "adversarial_post_limit": "--post-timeout 4" in runner,
    "foundation_test_ready_guard":
        '"[BOOT][TEST_READY] PASS autorun=0 selftests=0" --timeout 30' in foundation_runner,
}
Path(output).write_text(json.dumps({
    "schema": 1,
    "base_commit": base,
    "changed_paths": sorted(changed),
    "allowed_paths": sorted(allowed),
    "checks": checks,
    "record_capacity": 768,
    "cpu_limit": 32,
    "collection_budget_ms": 5000,
    "kvm_smp24_boot_timeout_seconds": 120,
    "boot_checkpoint_seconds": 45,
}, indent=2, sort_keys=True) + "\n")
failed = [name for name, passed in checks.items() if not passed]
if failed:
    raise SystemExit("SMP observation source audit failed: " + ", ".join(failed))
PY
    sha256sum kernel/src/shell/commands/cmd_irq.c \
        scripts/irq-snapshot-host.c scripts/panic-qemu.py \
        scripts/test-panic-lock-contention.sh \
        scripts/test-foundation-regression.sh \
        scripts/test-irq-snapshot-observation.sh \
        scripts/verify-smp-observation.py >"$evidence/static/source.sha256"
    echo "SMP_OBSERVATION_STATIC: PASS budget_ms=5000 record_capacity=768"
}

compile_host_profile()
{
    local name=$1 sanitizer=$2
    shift 2
    local run="$evidence/host/$name"
    local -a extra=("$@")
    local -a common=(-std=gnu11 -Wall -Wextra -Werror -fno-builtin -g
                     -ffunction-sections -fdata-sections)
    local -a legacy=(
        -Dstrlen=hobbyos_legacy_strlen
        -Dstrcmp=hobbyos_legacy_strcmp
        -Dstrncmp=hobbyos_legacy_strncmp
        -Dstrchr=hobbyos_legacy_strchr
        -Dstrcpy=hobbyos_legacy_strcpy
        -Dstrcat=hobbyos_legacy_strcat
        -Dstrrev=hobbyos_legacy_strrev)
    local -a command_cmd=("$host_cc" "${common[@]}" "${extra[@]}"
        -DHOBBYOS_IRQ_SNAPSHOT_HOST_TEST=1
        -c kernel/src/shell/commands/cmd_irq.c -o "$run/cmd.o")
    local -a command_string=("$host_cc" "${common[@]}" "${extra[@]}"
        "${legacy[@]}" -c kernel/src/libc/string.c -o "$run/string.o")
    local -a command_host=("$host_cc" "${common[@]}" "${extra[@]}"
        -c scripts/irq-snapshot-host.c -o "$run/host.o")
    local -a command_link=("$host_cc" "${extra[@]}" -Wl,--gc-sections
        "$run/cmd.o" "$run/string.o" "$run/host.o" -o "$run/host-test")
    [[ ! -e $run ]] || {
        echo "host profile already exists: $run" >&2
        return 1
    }
    mkdir -p "$run"
    : >"$run/compile.log"
    local status=0
    for command_name in command_cmd command_string command_host command_link; do
        local -n command_ref=$command_name
        record_command "host-$name-$command_name" "${command_ref[@]}"
        set +e
        "${command_ref[@]}" >>"$run/compile.log" 2>&1
        status=$?
        set -e
        ((status == 0)) || break
    done
    printf '%s\n' "$status" >"$run/compile-exit-code.txt"
    ((status == 0)) || {
        cat "$run/compile.log" >&2
        return "$status"
    }
    record_command "host-$name-run" timeout 30 "$run/host-test"
    set +e
    if [[ $name == asan ]]; then
        ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 \
            timeout 30 "$run/host-test" >"$run/run.log" 2>&1
    elif [[ $name == ubsan ]]; then
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
            timeout 30 "$run/host-test" >"$run/run.log" 2>&1
    else
        timeout 30 "$run/host-test" >"$run/run.log" 2>&1
    fi
    status=$?
    set -e
    printf '%s\n' "$status" >"$run/run-exit-code.txt"
    ((status == 0)) || {
        cat "$run/run.log" >&2
        return "$status"
    }
    sha256sum "$run"/*.o "$run/host-test" >"$run/SHA256SUMS"
}

host_tests()
{
    ensure_evidence
    compile_host_profile normal none
    compile_host_profile ubsan undefined \
        -fsanitize=undefined -fno-sanitize-recover=all
    compile_host_profile asan address \
        -fsanitize=address -fno-omit-frame-pointer
    echo "SMP_OBSERVATION_HOST: PASS profiles=3 cases=13 assertions=90"
}

fixtures()
{
    ensure_evidence
    run_logged observer-fixtures "$evidence/fixtures.log" \
        python3 scripts/verify-smp-observation.py fixtures \
        --output-dir "$evidence/fixtures"
    cat "$evidence/fixtures.log"
}

verify()
{
    ensure_evidence
    run_logged focal-replay "$evidence/replay.log" \
        python3 scripts/verify-smp-observation.py focal "$evidence"
    cat "$evidence/replay.log"
}

write_launch_json()
{
    local run=$1 runtime=$2 source_image_hash=$3 elf_hash=$4
    local qemu_pid=$5
    python3 - "$run/launch.json" "$runtime" "$source_image_hash" \
        "$elf_hash" "$qemu_pid" <<'PY'
import json
from pathlib import Path
import sys

output, runtime, image_hash, elf_hash, qemu_pid = sys.argv[1:]
argv_path = Path(runtime) / "qemu-argv.txt"
value = {
    "schema": 1,
    "kind": "irq-snapshot-launch",
    "machine": "q35",
    "accel": "kvm",
    "cpu": "host",
    "smp": 24,
    "memory": "2G",
    "elf_sha256": elf_hash,
    "source_image_sha256": image_hash,
    "qemu_pid": int(qemu_pid),
    "qemu_argv": argv_path.read_text().rstrip("\n"),
}
Path(output).write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")
PY
}

run_irq_qemu_once()
{
    local run_number=$1 image=$2 elf=$3 series=$4
    local run="$series/run-$run_number"
    local runtime="$run/runtime"
    local working="$run/working.img"
    local image_hash elf_hash pid start_status ready_status frame_status
    local query_status stop_status cleanup_status post_image_hash
    image_hash=$(sha256sum "$image" | awk '{print $1}')
    elf_hash=$(sha256sum "$elf" | awk '{print $1}')
    mkdir -p "$run" "$runtime"
    cp -- "$image" "$working"
    printf '%s\n' "$(date --iso-8601=ns)" >"$run/started-at.txt"
    record_command "irq-qemu-$run_number-start" env \
        QEMU_RUNTIME="$runtime" HOBBYOS_IMAGE="$working" SMP=24 \
        MACHINE=q35 ACCEL=kvm MEM=2G scripts/qemu-agent.sh start
    set +e
    QEMU_RUNTIME="$runtime" HOBBYOS_IMAGE="$working" SMP=24 \
        MACHINE=q35 ACCEL=kvm MEM=2G scripts/qemu-agent.sh start \
        >"$run/start.log" 2>&1
    start_status=$?
    set -e
    printf '%s\n' "$start_status" >"$run/start.exit-code.txt"
    if ((start_status != 0)); then
        return "$start_status"
    fi

    pid=$(cat "$runtime/qemu.pid")
    ps -p "$pid" -o args= >"$runtime/qemu-argv.txt"
    write_launch_json "$run" "$runtime" "$image_hash" "$elf_hash" "$pid"

    record_command "irq-qemu-$run_number-ready" python3 \
        scripts/foundation-qemu.py wait-ready --runtime "$runtime" \
        --marker "[BOOT][RUNTIME_READY] PASS cpus=24/24" --timeout 240 \
        --output "$run/runtime-ready.json"
    set +e
    python3 scripts/foundation-qemu.py wait-ready --runtime "$runtime" \
        --marker "[BOOT][RUNTIME_READY] PASS cpus=24/24" --timeout 240 \
        --output "$run/runtime-ready.json" >"$run/runtime-ready.log" 2>&1
    ready_status=$?
    set -e
    printf '%s\n' "$ready_status" >"$run/runtime-ready.exit-code.txt"

    frame_status=1
    if ((ready_status == 0)); then
        record_command "irq-qemu-$run_number-command" python3 \
            scripts/foundation-qemu.py frame --root "$root" \
            --runtime "$runtime" --record "$run/commands.jsonl" \
            --transport-log "$run/transport.log" \
            --profile-id "q35-kvm-smp24-irq-snapshot-$run_number" \
            --vm-id "irq-snapshot-$run_number" \
            --candidate-sha256 "$elf_hash" --sequence 1 \
            --payload "irq boot" --timeout 240 --allowed-status 0
        set +e
        python3 scripts/foundation-qemu.py frame --root "$root" \
            --runtime "$runtime" --record "$run/commands.jsonl" \
            --transport-log "$run/transport.log" \
            --profile-id "q35-kvm-smp24-irq-snapshot-$run_number" \
            --vm-id "irq-snapshot-$run_number" \
            --candidate-sha256 "$elf_hash" --sequence 1 \
            --payload "irq boot" --timeout 240 --allowed-status 0 \
            >"$run/command-controller.log" 2>&1
        frame_status=$?
        set -e
        if [[ -s $run/commands.jsonl ]]; then
            tail -n 1 "$run/commands.jsonl" >"$run/command.json"
        fi
    fi
    printf '%s\n' "$frame_status" >"$run/command.exit-code.txt"

    set +e
    QEMU_RUNTIME="$runtime" scripts/qemu-agent.sh status \
        >"$run/status-before-stop.log" 2>&1
    query_status=$?
    set -e
    printf '%s\n' "$query_status" >"$run/status-before-stop.exit-code.txt"
    cp -- "$runtime/qemu-serial.log" "$run/serial.log"
    cp -- "$runtime/qemu-debugcon.log" "$run/debugcon.log"
    cp -- "$runtime/qemu-trace.log" "$run/qemu-trace.log"
    cp -- "$runtime/launch.env" "$run/qemu-launch.env"
    cp -- "$runtime/qemu-argv.txt" "$run/qemu-argv.txt"
    post_image_hash=$(sha256sum "$working" | awk '{print $1}')
    printf 'before=%s\nafter=%s\n' "$image_hash" "$post_image_hash" \
        >"$run/image-identities.env"

    record_command "irq-qemu-$run_number-stop" env \
        QEMU_RUNTIME="$runtime" scripts/qemu-agent.sh stop
    set +e
    QEMU_RUNTIME="$runtime" scripts/qemu-agent.sh stop \
        >"$run/stop.log" 2>&1
    stop_status=$?
    set -e
    printf '%s\n' "$stop_status" >"$run/stop.exit-code.txt"
    cleanup_status=0
    if kill -0 "$pid" 2>/dev/null || [[ -e $runtime/qemu.pid || -S $runtime/hmp.sock ]]; then
        cleanup_status=1
    fi
    printf 'qemu_pid=%s\nstop_status=%s\nprocess_and_socket_absent=%s\n' \
        "$pid" "$stop_status" "$([[ $cleanup_status == 0 ]] && echo YES || echo NO)" \
        >"$run/cleanup.env"
    printf '%s\n' "$(date --iso-8601=ns)" >"$run/completed-at.txt"
    rm -f -- "$working"
    ((frame_status == 0 && query_status == 0 && stop_status == 0 && cleanup_status == 0))
}

qemu_series()
{
    ensure_evidence
    local image=${SMP_OBSERVATION_IMAGE:?SMP_OBSERVATION_IMAGE is required}
    local elf=${SMP_OBSERVATION_ELF:?SMP_OBSERVATION_ELF is required}
    local series="$evidence/irq-runtime"
    [[ -r /dev/kvm && -w /dev/kvm ]] || {
        echo "KVM is unavailable" >&2
        return 1
    }
    [[ -f $image && -f $elf ]] || {
        echo "candidate image or ELF is absent" >&2
        return 1
    }
    nm -n "$elf" | awk '$3 == "shell_execute_command_line_for_selftest" {
        found = 1
    } END { exit !found }' || {
        echo "candidate does not contain the framed selftest transport" >&2
        return 1
    }
    [[ ! -e $series ]] || {
        echo "IRQ runtime series already exists: $series" >&2
        return 1
    }
    mkdir -p "$series"
    local status=0
    for run_number in 1 2; do
        if ! run_irq_qemu_once "$run_number" "$image" "$elf" "$series"; then
            status=1
        fi
    done
    ((status == 0)) || return "$status"
    run_logged irq-series-replay "$evidence/irq-series-replay.log" \
        python3 scripts/verify-smp-observation.py irq-series "$evidence"
    cat "$evidence/irq-series-replay.log"
}

all()
{
    preflight
    static_checks
    host_tests
    fixtures
    verify
    echo "SMP_OBSERVATION_FOCAL: PASS evidence=$evidence"
}

case "${1:-}" in
    preflight) preflight ;;
    static) static_checks ;;
    host) host_tests ;;
    fixtures) fixtures ;;
    qemu) qemu_series ;;
    verify) verify ;;
    all) all ;;
    *) usage ;;
esac
