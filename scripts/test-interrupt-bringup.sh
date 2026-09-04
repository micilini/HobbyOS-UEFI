#!/usr/bin/env bash
set -Eeuo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

artifact=${HOBBYOS_TEST_ARTIFACT:-artifacts/build/interrupt-bringup}
qemu_artifact=$artifact/qemu
screen_dir=$artifact/screens
runtime=${HOBBYOS_TEST_RUNTIME:-/tmp/hobbyos-irq-runtime}
screen_runtime=$runtime/screens
serial=$runtime/qemu-serial.log
hmp_socket=$runtime/hmp.sock
current_stage=
sync_sequence=0
early_capture_pid=
duration_ms=${DURATION_MS:-300000}

mkdir -p "$artifact" "$qemu_artifact" "$screen_dir" .qemu "$runtime" \
    "$screen_runtime"
exec 9>.qemu/interrupt-bringup.lock
flock -n 9 || { echo "another HobbyOS QEMU harness is active" >&2; exit 1; }

stop_vm()
{
    if [[ -n $current_stage ]]; then
        QEMU_RUNTIME="$runtime" scripts/qemu-agent.sh stop >/dev/null 2>&1 || true
        current_stage=
    fi
    if [[ -n ${early_capture_pid:-} ]]; then
        wait "$early_capture_pid" 2>/dev/null || true
        early_capture_pid=
    fi
}

cleanup()
{
    stop_vm
    [[ ! -S $hmp_socket && ! -e $runtime/qemu.pid ]] || {
        echo "residual QEMU PID/socket in $runtime" >&2
        return 1
    }
}
trap cleanup EXIT

sha()
{
    sha256sum "$1" | awk '{print $1}'
}

count_marker()
{
    grep -F -c -- "$1" "$serial" 2>/dev/null || true
}

vm_alive()
{
    [[ -f $runtime/qemu.pid ]] || return 1
    local pid
    pid=$(sed -n '1p' "$runtime/qemu.pid")
    [[ $pid =~ ^[0-9]+$ ]] && kill -0 "$pid" 2>/dev/null
}

wait_new_marker()
{
    local marker=$1 before=$2 timeout=$3 allow_fault=${4:-0}
    local deadline=$((SECONDS + timeout))
    while ((SECONDS <= deadline)); do
        if (( $(count_marker "$marker") > before )); then
            return 0
        fi
        if ((allow_fault == 0)) &&
           grep -Eq 'PANIC|#PF|#GP|STRUCTURAL_FAULT|FINISH_FAULT' "$serial" 2>/dev/null; then
            echo "guest fault while waiting for: $marker" >&2
            tail -120 "$serial" >&2 || true
            return 1
        fi
        vm_alive || { echo "QEMU exited while waiting for: $marker" >&2; return 1; }
        sleep .2
    done
    echo "timeout waiting for: $marker" >&2
    tail -120 "$serial" >&2 || true
    return 1
}

hmp()
{
    python3 scripts/qemu_hmp.py --socket "$hmp_socket" "$@"
}

sync_shell()
{
    local attempt marker before
    sync_sequence=$((sync_sequence + 1))
    marker="[INPUTTEST][MARKER] name=irqsync${sync_sequence}"
    before=$(count_marker "$marker")
    for attempt in 1 2 3 4 5; do
        hmp text "inputtest marker irqsync${sync_sequence}" --profile sync --enter >/dev/null
        if wait_new_marker "$marker" "$before" 20; then
            return 0
        fi
    done
    return 1
}

send_command()
{
    local command=$1 marker=$2 timeout=${3:-120} profile=${4:-normal}
    local before
    before=$(count_marker "$marker")
    printf '%s\t%s\n' "$(date --iso-8601=seconds)" "$command" >>"$runtime/commands.tsv"
    hmp text "$command" --profile "$profile" --enter >/dev/null
    wait_new_marker "$marker" "$before" "$timeout"
    sync_shell
}

send_without_marker()
{
    local command=$1
    printf '%s\t%s\n' "$(date --iso-8601=seconds)" "$command" >>"$runtime/commands.tsv"
    hmp text "$command" --profile normal --enter >/dev/null
    sync_shell
}

record_evidence()
{
    local stage=$1 machine=$2 smp=$3 accel=$4 kernel_hash=$5 scenario=$6
    local marker=$7 log=$8 ready=${9:-$3} first=${10:-34}
    python3 scripts/verify-interrupt-bringup.py record \
        --stage "$stage" --machine "$machine" --smp "$smp" --accel "$accel" \
        --kernel-hash "$kernel_hash" --scenario "$scenario" --result PASS \
        --marker "$marker" --log "$log" --cpus-expected "$smp" \
        --cpus-ready "$ready" --first-vector "$first" \
        --unexpected 0 --imbalance 0
}

capture_when_marker_appears()
{
    local marker=$1 output=$2 deadline=$((SECONDS + 120))
    while ((SECONDS <= deadline)); do
        if grep -Fq "$marker" "$serial" 2>/dev/null; then
            hmp command "screendump $output" >/dev/null
            [[ -s $output ]] || {
                echo "screendump did not create: $output" >&2
                return 1
            }
            return 0
        fi
        vm_alive || return 1
        sleep .02
    done
    return 1
}

start_vm()
{
    local stage=$1 machine=$2 smp=$3 accel=$4 capture=${5:-0}
    local dir=$qemu_artifact/$stage image=$qemu_artifact/$stage/hobbyos.img
    stop_vm
    mkdir -p "$dir" "$runtime"
    cp hobbyos.img "$image"
    : >"$runtime/commands.tsv"
    current_stage=$stage
    QEMU_RUNTIME="$runtime" HOBBYOS_IMAGE="$image" MACHINE="$machine" \
        SMP="$smp" ACCEL="$accel" scripts/qemu-agent.sh start \
        >"$dir/start.log" 2>&1
    cp "$runtime/launch.env" "$dir/launch.env"
    if ((capture)); then
        rm -f "$screen_runtime/uefi-before-kernel.ppm" \
            "$screen_runtime/early-core-cleared.ppm" \
            "$screen_runtime/final-shell.ppm"
        wait_new_marker "[UEFI] GOP Mode Selected" 0 60
        hmp command \
            "screendump $screen_runtime/uefi-before-kernel.ppm" >/dev/null
        [[ -s $screen_runtime/uefi-before-kernel.ppm ]]
        capture_when_marker_appears "[GRAPHICS][EARLY_CLEAR] PASS" \
            "$screen_runtime/early-core-cleared.ppm" &
        early_capture_pid=$!
    fi
}

collect_vm()
{
    local stage=$1
    local dir=$qemu_artifact/$stage image=$qemu_artifact/$stage/hobbyos.img
    stop_vm
    cp "$serial" "$dir/serial.log"
    cp "$runtime/qemu-debugcon.log" "$dir/debugcon.log"
    cp "$runtime/qemu-trace.log" "$dir/trace.log"
    cp "$runtime/commands.tsv" "$dir/commands.tsv"
    mcopy -o -i "$image" ::/kernel.elf "$dir/kernel.payload.elf"
    [[ $(sha "$dir/kernel.payload.elf") == $(sha kernel.elf) ]]
}

require_marker()
{
    grep -Fq "$1" "$2" || { echo "$2: missing $1" >&2; return 1; }
}

require_order()
{
    local log=$1
    shift
    local marker line previous=0
    for marker in "$@"; do
        line=$(grep -nF "$marker" "$log" | head -1 | cut -d: -f1)
        [[ $line =~ ^[0-9]+$ ]] && ((line > previous)) || {
            echo "$log: marker order failed at $marker" >&2
            return 1
        }
        previous=$line
    done
}

verify_boot_log()
{
    local log=$1 smp=$2
    require_order "$log" \
        "[IRQ][PIC] QUIESCENT" \
        "[IRQ][LAPIC] QUIESCENT slot=0" \
        "[IRQ][IOAPIC] QUIESCENT" \
        "[IRQ][BOOTSTRAP] CONTROLLERS_QUIESCENT" \
        "[IRQ][BOOTSTRAP] ROUTES_PREPARED" \
        "[IRQ][BOOTSTRAP] CPUS_PREPARED cpus=$smp" \
        "[SCHED][BOOT] START_OK" \
        "[IRQ][BSP_LAPIC_PROBE] PASS vector=34" \
        "[IRQ][HPET_PROBE] PASS vector=32" \
        "[SCHED][BOOTSTRAP_HANDOFF] PASS slot=0" \
        "[IRQ][BOOTSTRAP] TIMERS_ACTIVE cpus=$smp" \
        "[CORE] System Core Initialization Complete." \
        "[IRQ][BOOTSTRAP] SERVICES_ACTIVE" \
        "[BOOT][SHELL_READY] PASS" \
        "[BOOT][RUNTIME_READY] PASS cpus=$smp/$smp" \
        "[BOOT][TEST_READY] PASS autorun=0 selftests=0" \
        "[KERNEL] Entering Main Loop."
    require_marker "[GRAPHICS][EARLY_BIND] PASS clear=1" "$log"
    [[ $(grep -Fc "[IRQ][LAPIC] QUIESCENT slot=" "$log") == "$smp" ]]
    [[ $(grep -Fc "[IRQ][CPU_READY] PASS" "$log") == "$smp" ]]
    [[ $(grep -Fc "timer_us=1000 handoff=1 preempt=1" "$log") == "$smp" ]]
    [[ $(grep -Fc "[SCHED][BOOTSTRAP_HANDOFF] PASS slot=" "$log") == "$smp" ]]
    ! grep -Eq 'PANIC|#PF|#GP|STRUCTURAL_FAULT|FINISH_FAULT' "$log"
}

basic_commands()
{
    send_without_marker version
    send_command "irq check" "[IRQ][CHECK] PASS" 60
    send_command "irq boot" "[IRQ][BOOT] state=SERVICES_ACTIVE" 60
    send_command "taskdiag check" "[TASKDIAG][CHECK] PASS" 90
    send_command "inputtest check" "[INPUTTEST][CHECK] PASS" 90
    send_command "modaltest check" "[MODALTEST][CHECK] PASS" 90
    send_command "accounttest lapic-config" "[ACCOUNT][LAPIC_CONFIG] PASS" 90
    send_command "accounttest lapic-liveness 500" "[ACCOUNT][LAPIC_LIVENESS] PASS" 90
    send_command "accounttest check" "[ACCOUNT][CHECK] PASS" 90
}

taskman_smoke()
{
    send_command "taskmantest anchor-reset" "[TASKMANTEST][ANCHOR_RESET] PASS" 60
    send_command "taskmantest auto-exit-frames 3" \
        "[TASKMANTEST][AUTO_EXIT_ARM] PASS frames=3" 60
    send_command "taskman 1000" "[TASKMAN][AUTO_EXIT] PASS target=3" 120
    send_command "taskmantest check" "[TASKMANTEST][CHECK] PASS" 90
    send_command "taskdiag check" "[TASKDIAG][CHECK] PASS" 90
    send_command "irq check" "[IRQ][CHECK] PASS" 90
}

stress_smoke()
{
    local before
    before=$(grep -c 'started on CPU' "$serial" 2>/dev/null || true)
    send_command "smpstress 8 0 5000" \
        "[SMP] smpstress spawning workers=8" 60 stress
    local deadline=$((SECONDS + 60))
    while ((SECONDS <= deadline)); do
        if (( $(grep -c 'started on CPU' "$serial" 2>/dev/null || true) >= before + 8 )); then
            break
        fi
        sleep .2
    done
    local fresh unique
    fresh=$(grep 'started on CPU' "$serial" | tail -8)
    unique=$(sed -n 's/.*started on CPU \([0-9][0-9]*\).*/\1/p' <<<"$fresh" | sort -u | wc -l)
    ((unique >= 2))
    send_command "killtest smpstress-sweep" \
        "[SMP][KILL_SWEEP] PASS workers=8" 180 stress
    send_command "taskdiag check" "[TASKDIAG][CHECK] PASS" 90
    send_command "irq check" "[IRQ][CHECK] PASS" 90
}

run_scenario()
{
    local stage=$1 machine=$2 smp=$3 accel=$4
    local capture=0
    [[ $stage == smp4-kvm ]] && capture=1
    start_vm "$stage" "$machine" "$smp" "$accel" "$capture"
    wait_new_marker "[KERNEL] Entering Main Loop." 0 180
    basic_commands
    if [[ $stage == smp4-kvm || $stage == smp24-kvm || $stage == pcat ]]; then
        send_command "irq controllers" "[IRQ][HPET] state=3" 90
        send_command "irq routes" "[IRQ][ROUTES] PASS count=2" 90
    fi
    if [[ $stage == smp4-kvm || $stage == smp24-kvm ]]; then
        taskman_smoke
    fi
    if [[ $stage == smp8-kvm || $stage == smp24-kvm ]]; then
        stress_smoke
    fi
    if ((capture)); then
        if [[ -n ${early_capture_pid:-} ]]; then
            wait "$early_capture_pid"
            early_capture_pid=
        fi
        hmp command "screendump $screen_runtime/final-shell.ppm" >/dev/null
        [[ -s $screen_runtime/final-shell.ppm ]]
        cp "$screen_runtime/uefi-before-kernel.ppm" \
            "$screen_runtime/early-core-cleared.ppm" \
            "$screen_runtime/final-shell.ppm" "$screen_dir/"
    fi
    collect_vm "$stage"
    local log=$qemu_artifact/$stage/serial.log
    verify_boot_log "$log" "$smp"
    require_marker "[IRQ][CHECK] PASS cpus=$smp/$smp" "$log"
    require_marker "unexpected=0 imbalance=0" "$log"
    if [[ $stage == pcat ]]; then
        require_marker "master=ff slave=ff" "$log"
    fi
    record_evidence "$stage" "$machine" "$smp" "$accel" \
        "$(sha kernel.elf)" "$stage" "[IRQ][CHECK] PASS" "$log"
    echo "[IRQ][SCENARIO] PASS stage=$stage machine=$machine smp=$smp accel=$accel"
}

write_manifest()
{
    local output=$1
    shift
    LC_ALL=C sha256sum "$@" >"$output"
}

preflight()
{
    local log=$artifact/preflight.log
    {
        [[ $(git branch --show-current) == feat/taskman ]]
        [[ $(git rev-parse HEAD) == 602532db881f5e328a78efdb52b0c012f35b4065 ]]
        [[ $(git rev-parse 'HEAD^{tree}') == 22a109bf7d2dd761bdcb8cca16b67e1258770872 ]]
        [[ $(git rev-parse HEAD^) == 7126ef635de448a9040689bd97feb6fd8ed59bab ]]
        git diff --cached --quiet
        [[ -f ROADMAP_TASKMAN_V1_CLOSURE_HARDENING.md ]]
        [[ -f $artifact/source-before.sha256 ]]
        [[ -f $artifact/taskman-protected-before.sha256 ]]
        [[ -f $artifact/bootloader-before.sha256 ]]
        git diff --check
        git diff --cached --check
        echo "[IRQ][PREFLIGHT] PASS"
    } 2>&1 | tee "$log"
    record_evidence preflight none 0 none "$(git rev-parse HEAD)" preflight \
        "[IRQ][PREFLIGHT] PASS" "$log" 0 0
}

static_gate()
{
    local log=$artifact/static.log
    {
        rg -n 'outb\(LEGACY_PIC_(MASTER|SLAVE)_IMR, 0xFFu\)' kernel/src/apic/legacy_pic.c >/dev/null
        ! rg -n 'outb\(LEGACY_PIC_(MASTER|SLAVE)_IMR, 0x(?!FF)' kernel/src --pcre2 >/dev/null
        rg -n 'MADT_TYPE_ISO|madt_resolve_isa_irq' kernel/src/acpi/madt.c kernel/src/core/irq_bootstrap.c >/dev/null
        rg -n 'gsi_base|IOAPICVER|IOAPIC_REDIR_MASKED' kernel/src/apic/ioapic.c >/dev/null
        rg -n 'lower \| IOAPIC_REDIR_MASKED' kernel/src/apic/ioapic.c >/dev/null
        rg -n 'LAPIC_LVT_LINT0|LAPIC_LVT_LINT1' kernel/src/apic/lapic.c >/dev/null
        rg -n 'HPET_TIMER0_PREPARED_MASKED|HPET_TIMER0_ARMED_MASKED' kernel/src/timer/hpet.c >/dev/null
        rg -n 'LAPIC_TIMER_PREPARED_MASKED|LAPIC_TIMER_ACTIVE_PERIODIC' kernel/src/apic/lapic.c >/dev/null
        rg -n 'irq_bootstrap_cpu_wait_release' kernel/src/smp/smp_boot.c >/dev/null
        rg -n 'interrupt_context_consume_preempt_epilogue|handoff_complete|irq_preemption_enabled' kernel/src/core/scheduler.c >/dev/null
        rg -n 'scheduler_bootstrap_handoff_current_cpu' kernel/src/core/kernel_init.c kernel/src/smp/smp_boot.c >/dev/null
        rg -n 'madt_resolve_isa_irq\(1' kernel/src/core/irq_bootstrap.c >/dev/null
        ! rg -n 'ioapic_map_irq\s*\(\s*1\s*,\s*33\s*,\s*0' kernel >/dev/null
        ! rg -n 'g_cpu_count\s*(==|<=|>=|<|>)\s*24|cpu_count\s*(==|<=|>=|<|>)\s*24' kernel/src >/dev/null
        rg -n 'lapic_timer_calibrate\(1000|desired_period_us != 1000u' kernel/src >/dev/null
        git diff --quiet -- bootloader
        git diff --quiet -- \
            kernel/src/shell/commands/cmd_taskman.c \
            kernel/src/shell/commands/cmd_taskman.h \
            kernel/src/shell/commands/taskman_view.c \
            kernel/src/shell/commands/taskman_view.h \
            kernel/src/core/task_format.c kernel/src/core/task_format.h \
            kernel/src/core/modal_ui.c kernel/src/core/modal_ui.h \
            kernel/src/core/modal_session.c kernel/src/core/modal_session.h
        local active=(
            kernel/src/apic/legacy_pic.c kernel/src/apic/legacy_pic.h
            kernel/src/core/irq_bootstrap.c kernel/src/core/irq_bootstrap.h
            kernel/src/core/interrupt_context.c kernel/src/core/interrupt_context.h
            scripts/test-interrupt-bringup.sh
            scripts/verify-interrupt-bringup.py
            scripts/verify-early-framebuffer.py
        )
        local prohibited
        for prohibited in "TM""V1" "UX""01" "FI""X" "FA""SE" "PHA""SE" "C""L-[0-9]"; do
            ! rg -n "$prohibited" "${active[@]}" >/dev/null
        done
        bash -n scripts/test-interrupt-bringup.sh
        python3 -c 'import pathlib; [compile(path.read_text(), str(path), "exec") for path in map(pathlib.Path, ("scripts/verify-interrupt-bringup.py", "scripts/verify-early-framebuffer.py"))]'
        echo "[IRQ][ACTIVE_NAMING] PASS matches=0"
        echo "[IRQ][STATIC] PASS"
    } 2>&1 | tee "$log"
    record_evidence static none 0 none "$(git rev-parse HEAD)" static \
        "[IRQ][STATIC] PASS" "$log" 0 0
}

selftest_gate()
{
    local log=$artifact/selftest.log
    {
        python3 scripts/verify-interrupt-bringup.py selftest
        make kernel-check JOBS=2
        strings kernel.elf | grep -F '[IRQ][SELFTEST] PASS' >/dev/null
        echo "[IRQ][SELFTEST_GATE] PASS"
    } 2>&1 | tee "$log"
    record_evidence selftest host 0 none "$(sha kernel.elf)" selftest \
        "[IRQ][SELFTEST_GATE] PASS" "$log" 0 0
}

negative_one()
{
    local stage=$1 macro=$2 marker=$3
    local dir=$artifact/negatives/$stage log=$artifact/negatives/$stage/serial.log
    mkdir -p "$dir"
    make clean >>"$dir/build.log" 2>&1
    make kernel-check JOBS=2 KERNEL_EXTRA_CFLAGS="-D$macro" >>"$dir/build.log" 2>&1
    make image KERNEL_EXTRA_CFLAGS="-D$macro" >>"$dir/build.log" 2>&1
    grep -Fq -- "-D$macro" artifacts/build/kernel-check-j2.log
    cp hobbyos.img "$dir/hobbyos.img"
    stop_vm
    current_stage=$stage
    QEMU_RUNTIME="$runtime" HOBBYOS_IMAGE="$dir/hobbyos.img" MACHINE=q35 \
        SMP=1 ACCEL=tcg scripts/qemu-agent.sh start >"$dir/start.log" 2>&1
    wait_new_marker "$marker" 0 120 1
    stop_vm
    cp "$serial" "$log"
    require_marker "$marker" "$log"
    record_evidence "$stage" q35 1 tcg "$(sha kernel.elf)" "$stage" \
        "$marker" "$log"

    make clean >>"$dir/reset-build.log" 2>&1
    make kernel-check JOBS=2 >>"$dir/reset-build.log" 2>&1
    make image >>"$dir/reset-build.log" 2>&1
    ! strings kernel.elf | grep -Fq "$marker"
    cp hobbyos.img "$dir/reset-hobbyos.img"
    current_stage=${stage}-reset
    QEMU_RUNTIME="$runtime" HOBBYOS_IMAGE="$dir/reset-hobbyos.img" MACHINE=q35 \
        SMP=1 ACCEL=tcg scripts/qemu-agent.sh start >"$dir/reset-start.log" 2>&1
    wait_new_marker "[KERNEL] Entering Main Loop." 0 180
    require_marker "[IRQ][SELFTEST] PASS" "$serial"
    stop_vm
    cp "$serial" "$dir/reset-serial.log"
    ! grep -Fq "$marker" "$dir/reset-serial.log"
    echo "[IRQ][NEGATIVE_GATE] PASS stage=$stage"
}

negatives()
{
    negative_one negative-pic HOBBYOS_IRQ_NEGATIVE_PIC_UNMASKED \
        "[IRQ][NEGATIVE] PIC_NOT_QUIESCENT_DETECTED"
    negative_one negative-ioapic HOBBYOS_IRQ_NEGATIVE_IOAPIC_ROUTE_ACTIVE \
        "[IRQ][NEGATIVE] IOAPIC_NOT_QUIESCENT_DETECTED"
    negative_one negative-iso HOBBYOS_IRQ_NEGATIVE_IGNORE_ISO \
        "[IRQ][NEGATIVE] INTERRUPT_OVERRIDE_LOST_DETECTED"
    negative_one negative-hpet HOBBYOS_IRQ_NEGATIVE_HPET_EARLY_ARM \
        "[IRQ][NEGATIVE] HPET_EARLY_DELIVERY_DETECTED"
    negative_one negative-preemption HOBBYOS_IRQ_NEGATIVE_EARLY_PREEMPTION \
        "[IRQ][NEGATIVE] PREEMPTION_BEFORE_HANDOFF_DETECTED"
    make production-image >"$artifact/normal-after-negatives.log" 2>&1
    ! strings kernel.elf | grep -Fq '[IRQ][NEGATIVE]'
}

matrix()
{
    run_scenario up-tcg q35 1 tcg
    run_scenario smp2-tcg q35 2 tcg
    [[ -e /dev/kvm && -r /dev/kvm && -w /dev/kvm ]] || {
        echo "BLOCKED_BY_EXTERNAL_ENVIRONMENT: /dev/kvm unavailable" >&2
        return 1
    }
    run_scenario smp4-kvm q35 4 kvm
    run_scenario smp8-kvm q35 8 kvm
    run_scenario smp24-kvm q35 24 kvm
    run_scenario pcat pc 4 tcg
    python3 scripts/verify-early-framebuffer.py \
        "$screen_dir/uefi-before-kernel.ppm" \
        "$screen_dir/early-core-cleared.ppm" \
        "$screen_dir/final-shell.ppm" | tee "$artifact/framebuffer.log"
}

soak24()
{
    [[ $duration_ms =~ ^[0-9]+$ ]] && ((duration_ms >= 300000)) || {
        echo "DURATION_MS must be at least 300000" >&2
        return 2
    }
    local matrix_hash
    matrix_hash=$(sed -n 's/^KERNEL_SHA256=//p' "$qemu_artifact/smp24-kvm/launch.env")
    [[ $matrix_hash == "$(sha kernel.elf)" ]]
    start_vm soak24 q35 24 kvm 0
    local start now elapsed iteration=0 taskman_runs=0 stress_runs=0
    wait_new_marker "[KERNEL] Entering Main Loop." 0 180
    send_command "inputtest check" "[INPUTTEST][CHECK] PASS" 90
    send_command "modaltest check" "[MODALTEST][CHECK] PASS" 90
    start=$(date +%s%3N)
    while :; do
        iteration=$((iteration + 1))
        send_command "irq check" "[IRQ][CHECK] PASS" 90 stress
        send_command "taskdiag check" "[TASKDIAG][CHECK] PASS" 90 stress
        if ((stress_runs < 4)); then
            stress_smoke
            stress_runs=$((stress_runs + 1))
        fi
        if ((taskman_runs < 4)); then
            taskman_smoke
            taskman_runs=$((taskman_runs + 1))
        fi
        now=$(date +%s%3N)
        elapsed=$((now - start))
        if ((iteration >= 10 && stress_runs >= 4 && taskman_runs >= 4 && elapsed >= duration_ms)); then
            break
        fi
        if ((iteration >= 10 && stress_runs >= 4 && taskman_runs >= 4)); then
            sleep 5
        fi
    done
    send_command "inputtest check" "[INPUTTEST][CHECK] PASS" 90
    send_command "modaltest check" "[MODALTEST][CHECK] PASS" 90
    send_command "accounttest check" "[ACCOUNT][CHECK] PASS" 90
    send_command "killtest check" "[KILLTEST][CHECK] PASS" 90
    send_command "taskmantest check" "[TASKMANTEST][CHECK] PASS" 90
    send_command "irq boot" "[IRQ][BOOT] state=SERVICES_ACTIVE" 90
    send_command "irq check" "[IRQ][CHECK] PASS cpus=24/24" 90
    now=$(date +%s%3N)
    elapsed=$((now - start))
    collect_vm soak24
    local log=$qemu_artifact/soak24/serial.log
    printf '[IRQ][SOAK] PASS smp=24 duration_ms=%s\n' "$elapsed" >>"$log"
    verify_boot_log "$log" 24
    (( $(grep -Fc '[IRQ][CHECK] PASS' "$log") >= 10 ))
    (( $(grep -Fc '[TASKDIAG][CHECK] PASS' "$log") >= 10 ))
    (( $(grep -Fc '[SMP][KILL_SWEEP] PASS workers=8' "$log") >= 4 ))
    (( $(grep -Fc '[TASKMAN][AUTO_EXIT] PASS target=3' "$log") >= 4 ))
    record_evidence soak24 q35 24 kvm "$(sha kernel.elf)" soak24 \
        "[IRQ][SOAK] PASS" "$log"
}

continuity_manifests()
{
    local protected=(
        kernel/src/core/modal_session.c kernel/src/core/modal_session.h
        kernel/src/core/modal_ui.c kernel/src/core/modal_ui.h
        kernel/src/core/task_format.c kernel/src/core/task_format.h
        kernel/src/shell/commands/cmd_taskman.c
        kernel/src/shell/commands/cmd_taskman.h
        kernel/src/shell/commands/taskman_view.c
        kernel/src/shell/commands/taskman_view.h
    )
    mapfile -t boot_files < <(rg --files bootloader | LC_ALL=C sort)
    write_manifest "$artifact/taskman-protected-after.sha256" "${protected[@]}"
    write_manifest "$artifact/bootloader-after.sha256" "${boot_files[@]}"
    mapfile -t sources < <({ rg --files kernel shared scripts docs; echo makefile; echo README.md; echo AGENTS.md; } | LC_ALL=C sort -u)
    write_manifest "$artifact/source-after-tests.sha256" "${sources[@]}"
}

final_build()
{
    local log=$artifact/final-build.log jobs
    jobs=$(nproc)
    {
        make clean
        make stack-check
        make kernel-check JOBS=2
        cp kernel.elf /tmp/hobbyos-irq-final-j2.elf
        make clean
        make kernel-check JOBS="$jobs"
        cp kernel.elf /tmp/hobbyos-irq-final-jN.elf
        sha256sum /tmp/hobbyos-irq-final-j2.elf /tmp/hobbyos-irq-final-jN.elf
        cmp -s /tmp/hobbyos-irq-final-j2.elf /tmp/hobbyos-irq-final-jN.elf
        make deps-check
        make production-image
        cmp -s kernel.elf /tmp/hobbyos-irq-final-jN.elf
        [[ -z $(nm -u kernel.elf) ]]
        echo "[IRQ][FINAL_BUILD] PASS"
    } 2>&1 | tee "$log"
    mapfile -t sources < <({ rg --files kernel shared scripts docs; echo makefile; echo README.md; echo AGENTS.md; } | LC_ALL=C sort -u)
    write_manifest "$artifact/source-after-build.sha256" "${sources[@]}"
    record_evidence final-build host 0 none "$(sha kernel.elf)" final-build \
        "[IRQ][FINAL_BUILD] PASS" "$log" 0 0
}

report_gate()
{
    local log=$artifact/report.log
    continuity_manifests
    {
        python3 scripts/verify-interrupt-bringup.py verify matrix
        echo "[IRQ][REPORT] PASS"
    } 2>&1 | tee "$log"
    record_evidence report host 0 none "$(sha kernel.elf)" report \
        "[IRQ][REPORT] PASS" "$log" 0 0
    if [[ -d artifacts/baremetal/interrupt-bringup-candidate ]]; then
        python3 scripts/verify-interrupt-bringup.py verify all 2>&1 |
            tee "$artifact/evidence-all.log"
    fi
}

candidate_gate()
{
    local base=602532db881f5e328a78efdb52b0c012f35b4065
    local candidate=artifacts/baremetal/interrupt-bringup-candidate
    local baremetal_root=artifacts/baremetal
    local payload_tmp head tree short review_zip review_hash image_hash
    local kernel_hash boot_hash

    [[ $(git branch --show-current) == feat/taskman ]]
    head=$(git rev-parse HEAD)
    tree=$(git rev-parse 'HEAD^{tree}')
    [[ $(git rev-parse HEAD^) == "$base" ]]
    [[ $(git rev-list --count "$base..HEAD") == 1 ]]
    [[ $(git log -1 --format=%s) == \
        'fix(irq): establish safe x86 interrupt controller bring-up' ]]
    [[ $(git status --short) == '?? ROADMAP_TASKMAN_V1_CLOSURE_HARDENING.md' ]]
    [[ ! -S $hmp_socket && ! -e $runtime/qemu.pid ]]
    ! pgrep -f '[q]emu-system-x86_64' >/dev/null
    python3 scripts/verify-interrupt-bringup.py verify matrix

    kernel_hash=$(sha kernel.elf)
    image_hash=$(sha hobbyos.img)
    boot_hash=$(sha BOOTX64.EFI)
    python3 - "$artifact/evidence.json" "$kernel_hash" <<'PY'
import json
import pathlib
import sys

rows = {row["stage"]: row for row in json.loads(pathlib.Path(sys.argv[1]).read_text())}
expected = sys.argv[2]
for stage in ("up-tcg", "smp2-tcg", "smp4-kvm", "smp8-kvm",
              "smp24-kvm", "pcat", "soak24", "final-build"):
    if rows[stage]["kernel_hash"] != expected:
        raise SystemExit(f"kernel drift at {stage}")
PY

    mkdir -p "$baremetal_root"
    [[ $candidate == artifacts/baremetal/interrupt-bringup-candidate ]]
    rm -rf "$candidate"
    mkdir -p "$candidate"
    cp hobbyos.img kernel.elf BOOTX64.EFI "$candidate/"
    cp docs/baremetal/INTERRUPT_BRINGUP_OPERATOR_RUNBOOK.md "$candidate/"
    cp docs/interrupt-controller-bringup.md "$candidate/"
    cp docs/test-reports/INTERRUPT_BRINGUP_CERTIFICATION.md "$candidate/"

    payload_tmp=$(mktemp -d /tmp/hobbyos-irq-candidate.XXXXXX)
    mcopy -i hobbyos.img ::/kernel.elf "$payload_tmp/kernel.elf"
    mcopy -i hobbyos.img ::/EFI/BOOT/BOOTX64.EFI "$payload_tmp/BOOTX64.EFI"
    mcopy -i hobbyos.img ::/EFI/fonts/zap-light16.psf "$payload_tmp/zap-light16.psf"
    mcopy -i hobbyos.img ::/EFI/images/logo.bmp "$payload_tmp/logo.bmp"
    mcopy -i hobbyos.img ::/startup.nsh "$payload_tmp/startup.nsh"
    cmp -s kernel.elf "$payload_tmp/kernel.elf"
    cmp -s BOOTX64.EFI "$payload_tmp/BOOTX64.EFI"
    cmp -s bootloader/fonts/zap-light16.psf "$payload_tmp/zap-light16.psf"
    cmp -s bootloader/images/logo.bmp "$payload_tmp/logo.bmp"
    cmp -s bootloader/startup.nsh "$payload_tmp/startup.nsh"
    python3 - "$payload_tmp" "$candidate/payload-manifest.txt" <<'PY'
import hashlib
import pathlib
import sys

source = pathlib.Path(sys.argv[1])
output = pathlib.Path(sys.argv[2])
names = (
    ("/kernel.elf", "kernel.elf"),
    ("/EFI/BOOT/BOOTX64.EFI", "BOOTX64.EFI"),
    ("/EFI/fonts/zap-light16.psf", "zap-light16.psf"),
    ("/EFI/images/logo.bmp", "logo.bmp"),
    ("/startup.nsh", "startup.nsh"),
)
lines = []
for image_name, local_name in names:
    digest = hashlib.sha256((source / local_name).read_bytes()).hexdigest()
    lines.append(f"{digest}  {image_name}")
output.write_text("\n".join(lines) + "\n", encoding="utf-8")
PY
    rm -rf "$payload_tmp"

    fsck.fat -n hobbyos.img >"$candidate/fat-fsck.txt" 2>&1
    {
        mdir -i hobbyos.img ::
        mdir -i hobbyos.img ::/EFI/BOOT
        mdir -i hobbyos.img ::/EFI/fonts
        mdir -i hobbyos.img ::/EFI/images
    } >"$candidate/fat-metadata.txt"
    python3 - hobbyos.img "$candidate/boot-signature.txt" <<'PY'
import pathlib
import sys

image = pathlib.Path(sys.argv[1]).read_bytes()
if len(image) < 512 or image[510:512] != b"\x55\xaa":
    raise SystemExit("FAT boot signature is not 55aa")
pathlib.Path(sys.argv[2]).write_text("FAT_BOOT_SIGNATURE=55aa\n", encoding="ascii")
PY

    python3 - "$artifact/evidence.json" \
        "$candidate/QEMU-certification-summary.txt" <<'PY'
import json
import pathlib
import re
import sys

rows = {row["stage"]: row for row in json.loads(pathlib.Path(sys.argv[1]).read_text())}
stages = ("up-tcg", "smp2-tcg", "smp4-kvm", "smp8-kvm",
          "smp24-kvm", "pcat", "soak24")
lines = ["HobbyOS interrupt bring-up QEMU certification", ""]
for stage in stages:
    row = rows[stage]
    lines.append(
        f"{stage}: {row['result']} machine={row['machine']} smp={row['smp']} "
        f"accel={row['accel']} kernel={row['kernel_hash']}"
    )
soak_log = pathlib.Path(rows["soak24"]["log"]).read_text(errors="replace")
duration = re.search(r"\[IRQ\]\[SOAK\] PASS smp=24 duration_ms=(\d+)", soak_log)
if not duration:
    raise SystemExit("soak duration marker missing")
lines.extend(("", f"soak_duration_ms={duration.group(1)}",
              "unexpected=0", "imbalance=0", "faults=0"))
pathlib.Path(sys.argv[2]).write_text("\n".join(lines) + "\n", encoding="utf-8")
PY

    cat >"$candidate/expected-markers.txt" <<'EOF'
[IRQ][PIC] QUIESCENT
[IRQ][IOAPIC] QUIESCENT
[IRQ][LAPIC] QUIESCENT slot=0
[IRQ][BOOTSTRAP] CONTROLLERS_QUIESCENT
[IRQ][BOOTSTRAP] ROUTES_PREPARED
[IRQ][BOOTSTRAP] CPUS_PREPARED
[SCHED][BOOT] START_OK
[IRQ][BSP_LAPIC_PROBE] PASS
[IRQ][HPET_PROBE] PASS
[SCHED][BOOTSTRAP_HANDOFF] PASS slot=0
[IRQ][CPU_READY] PASS
[IRQ][BOOTSTRAP] TIMERS_ACTIVE
[GRAPHICS][EARLY_BIND] PASS
[CORE] System Core Initialization Complete.
[IRQ][BOOTSTRAP] SERVICES_ACTIVE
[BOOT][SHELL_READY] PASS
[BOOT][RUNTIME_READY] PASS
[BOOT][TEST_READY] PASS autorun=0 selftests=0
[KERNEL] Entering Main Loop.
EOF

    short=${head:0:7}
    review_zip=$baremetal_root/HobbyOS-INTERRUPT-BRINGUP-$short.zip
    rm -f "$review_zip"
    python3 - "$root" "$review_zip" "$short" <<'PY'
import os
import pathlib
import stat
import sys
import zipfile

root = pathlib.Path(sys.argv[1])
target = pathlib.Path(sys.argv[2])
prefix = f"HobbyOS-INTERRUPT-BRINGUP-{sys.argv[3]}"
skip_dirs = {"artifacts", ".qemu", "__pycache__"}
skip_names = {
    "kernel.elf", "hobbyos.img", "BOOTX64.EFI", "bootx64.so",
    "qemu-debugcon.log",
}
skip_suffixes = {".o", ".d", ".su", ".pyc", ".sock", ".pid"}
with zipfile.ZipFile(target, "w", compression=zipfile.ZIP_DEFLATED,
                     compresslevel=6) as archive:
    for current, dirs, files in os.walk(root):
        current_path = pathlib.Path(current)
        relative_dir = current_path.relative_to(root)
        dirs[:] = sorted(name for name in dirs if name not in skip_dirs)
        for name in sorted(files):
            path = current_path / name
            relative = relative_dir / name
            if name in skip_names or path.suffix in skip_suffixes:
                continue
            mode = path.lstat().st_mode
            if stat.S_ISSOCK(mode):
                continue
            archive.write(path, pathlib.PurePosixPath(prefix, relative.as_posix()))
with zipfile.ZipFile(target) as archive:
    bad = archive.testzip()
    if bad:
        raise SystemExit(f"corrupt ZIP member: {bad}")
    names = set(archive.namelist())
    required = {
        f"{prefix}/.git/HEAD",
        f"{prefix}/kernel/kernel.c",
        f"{prefix}/scripts/test-interrupt-bringup.sh",
        f"{prefix}/docs/interrupt-controller-bringup.md",
    }
    if not required <= names:
        raise SystemExit("review ZIP is missing source or .git metadata")
PY
    review_hash=$(sha "$review_zip")

    cat >"$candidate/candidate.txt" <<EOF
STATUS=READY_FOR_BARE_METAL_INTERRUPT_VALIDATION
BRANCH=feat/taskman
BASE=$base
PARENT=$base
COMMIT=$head
TREE=$tree
SUBJECT=fix(irq): establish safe x86 interrupt controller bring-up
KERNEL_SHA256=$kernel_hash
IMAGE_SHA256=$image_hash
BOOTX64_SHA256=$boot_hash
REVIEW_ZIP=$review_zip
REVIEW_ZIP_SHA256=$review_hash
BARE_METAL_EXECUTED_BY_CODEX=NO
NEXT_OWNER=William
EOF

    (
        cd "$candidate"
        find . -maxdepth 1 -type f ! -name SHA256SUMS -printf '%P\0' |
            LC_ALL=C sort -z | xargs -0 sha256sum >SHA256SUMS
        sha256sum -c SHA256SUMS
    ) >/dev/null

    local log=$artifact/candidate.log
    {
        echo "[IRQ][CANDIDATE] PASS commit=$head tree=$tree kernel=$kernel_hash image=$image_hash bootx64=$boot_hash"
        echo "[IRQ][REVIEW_ZIP] PASS path=$review_zip sha256=$review_hash"
    } | tee "$log"
    record_evidence candidate host 0 none "$kernel_hash" candidate \
        "[IRQ][CANDIDATE] PASS" "$log" 0 0
    report_gate
}

all()
{
    preflight
    static_gate
    selftest_gate
    negatives
    matrix
    soak24
    continuity_manifests
    final_build
    report_gate
}

if [[ ${HOBBYOS_INTERRUPT_BRINGUP_LIBRARY:-0} != 1 ]]; then
case ${1:-all} in
    preflight) preflight ;;
    static) static_gate ;;
    selftest) selftest_gate ;;
    negatives) negatives ;;
    up-tcg) run_scenario up-tcg q35 1 tcg ;;
    smp2-tcg) run_scenario smp2-tcg q35 2 tcg ;;
    smp4-kvm) run_scenario smp4-kvm q35 4 kvm ;;
    smp8-kvm) run_scenario smp8-kvm q35 8 kvm ;;
    smp24-kvm) run_scenario smp24-kvm q35 24 kvm ;;
    pcat) run_scenario pcat pc 4 tcg ;;
    soak24) soak24 ;;
    final-build) final_build ;;
    report) report_gate ;;
    candidate) candidate_gate ;;
    all) all ;;
    *)
        echo "usage: $0 preflight|static|selftest|negatives|up-tcg|smp2-tcg|smp4-kvm|smp8-kvm|smp24-kvm|pcat|soak24|final-build|report|candidate|all" >&2
        exit 2
        ;;
esac
fi
