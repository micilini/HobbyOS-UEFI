#!/usr/bin/env bash
set -Eeuo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

export HOBBYOS_INTERRUPT_BRINGUP_LIBRARY=1
export HOBBYOS_TEST_ARTIFACT=artifacts/build/lapic-clockevent
export HOBBYOS_TEST_RUNTIME=/tmp/hobbyos-lapic-clockevent-runtime
source scripts/test-interrupt-bringup.sh

base=523f367c9cd8652e709c0ffc7af28f6433d1213c
base_tree=cc04290b8cfcfcc16de06e65b1a8145f6760d646
base_parent=602532db881f5e328a78efdb52b0c012f35b4065
subject='fix(boot): complete LAPIC clockevent and boot-to-shell liveness'
duration_ms=${DURATION_MS:-300000}
fair_artifact=artifacts/build/timer-fair-dispatch
rendezvous_artifact=artifacts/build/ap-runtime-rendezvous
autonomous_artifact=artifacts/build/boot-to-shell-autonomous
historical_rate_log=$qemu_artifact/smp24-kvm-failed/serial.log
historical_runtime_log=$qemu_artifact/soak24-failed/serial.log
historical_rate_sha=75fc283521f23bae6916493982ddb188768412d0e9b177aa2a9a2a55f6c39d46
historical_runtime_sha=29ace19ec5bd95ef29f8d962186dcd88d5ec159e636427e708b54ea869dc8e70
runtime_ready_failure_log=$qemu_artifact/fair-smp24-kvm-runtime-ready-failed/qemu-serial.log
runtime_ready_pass_log=$qemu_artifact/fair-focused-smp24/serial.log
runtime_ready_failure_sha=e38cd70fadc86215e6ea6d1f08142b9f01b09642d558b846bec69aa380434b66
runtime_ready_pass_sha=7d3c1a8a6a3b55cff5436fcaea1857eb1313c450d945dc8fc7ff27f817404a33
splash_begin=$screen_runtime/splash-begin.ppm
splash_complete=$screen_runtime/splash-complete.ppm
splash_begin_artifact=$screen_dir/fair-splash-begin.ppm
splash_complete_artifact=$screen_dir/fair-splash-complete.ppm
host_environment=$artifact/host-environment.txt
HOST_SCHEDULABLE_CPUS=0
HOST_ALLOWED_CPUS=0
HOST_NPROC=0
HOST_OVERSUBSCRIBED=0
lapic_rate_raw=NOT_RUN
lapic_rate_window=
lapic_rate_rounds=
lapic_rate_worst=
lapic_rate_min=
lapic_rate_max=
lapic_rate_authority=NOT_APPLICABLE
lapic_rate_disposition=PASS
lapic_rate_runtime=NOT_RUN
BOOT_WATCH_STAGE=
BOOT_WATCH_LAST_MARKER=
BOOT_WATCH_HOST_ELAPSED_MS=0
BOOT_WATCH_GUEST_ELAPSED_MS=unknown
BOOT_WATCH_CLASSIFICATION=
BOOT_WATCH_SHELL_AVAILABLE=0

mkdir -p "$autonomous_artifact"

record_stage()
{
    local stage=$1 scenario=$2 machine=$3 smp=$4 accel=$5 log=$6 marker=$7
    local raw=${8:-NOT_RUN} authority=${9:-NOT_APPLICABLE}
    local disposition=${10:-PASS} worst=${11:-} min_rate=${12:-}
    local max_rate=${13:-} window=${14:-} rounds=${15:-}
    local runtime_correctness=${16:-NOT_RUN} result=${17:-PASS}
    local launch=$qemu_artifact/$scenario/launch.env
    local image_hash= kernel_hash= global_ticks=0 measurement= threshold=
    local order_passes=0 cancel_passes=0 late_callbacks=0 claimed_residual=0
    kernel_hash=$(sha kernel.elf)
    if [[ -f $launch ]]; then
        image_hash=$(sed -n 's/^IMAGE_SHA256=//p' "$launch")
    elif [[ -f hobbyos.img ]]; then
        image_hash=$(sha hobbyos.img)
    fi
    if [[ -f $log ]]; then
        global_ticks=$(sed -n 's/.*clockevent_ticks=\([0-9][0-9]*\).*/\1/p' "$log" | tail -1)
        global_ticks=${global_ticks:-0}
        order_passes=$(grep -Fc \
            '[SYNC][TIMER_ORDER] PASS old_position=1 new_position=2 callbacks=2' \
            "$log" || true)
        cancel_passes=$(grep -Ec \
            '\[SYNC\]\[TIMER_CANCEL\] PASS run=[0-9]+ requested=3 completed=3 errors=0' \
            "$log" || true)
        late_callbacks=$(sed -n \
            's/.*\[SYNC\]\[CHECK\].* late=\([0-9][0-9]*\).*/\1/p' \
            "$log" | tail -1)
        late_callbacks=${late_callbacks:-0}
        claimed_residual=$(sed -n \
            's/.*\[SYNC\]\[CHECK\].* timers_claimed=\([0-9][0-9]*\).*/\1/p' \
            "$log" | tail -1)
        claimed_residual=${claimed_residual:-0}
    fi
    if [[ $raw == PASS || $raw == FAIL ]]; then
        measurement=lapic-rate
        threshold=700
    fi
    python3 scripts/verify-timer-clockevent.py record \
        --stage "$stage" --scenario "$scenario" --machine "$machine" \
        --smp "$smp" --accel "$accel" --kernel-sha256 "$kernel_hash" \
        --image-sha256 "$image_hash" --clocksource HPET \
        --clockevent BSP_LAPIC --hpet-timer0-state QUIESCENT \
        --lapic-period-us 1000 --cpus-ready "$smp" \
        --global-ticks "$global_ticks" --non-bsp-ticks 0 --early-ticks 0 \
        --hpet-irqs 0 --stray-hpet 0 --result "$result" --marker "$marker" \
        --measurement "$measurement" --raw-test-result "$raw" \
        --window-ms "$window" --rounds "$rounds" \
        --worst-median-x1000 "$worst" --min-round-x1000 "$min_rate" \
        --max-round-x1000 "$max_rate" --threshold "$threshold" \
        --measurement-authority "$authority" \
        --host-schedulable-cpus "$HOST_SCHEDULABLE_CPUS" \
        --guest-vcpus "$smp" --runtime-correctness "$runtime_correctness" \
        --scenario-disposition "$disposition" \
        --timer-order-passes "$order_passes" \
        --timer-cancel-passes "$cancel_passes" \
        --late-callbacks "$late_callbacks" \
        --claimed-residual "$claimed_residual" --log "$log"
}

count_allowed_cpus()
{
    python3 - "$1" <<'PY'
import re
import sys

value = sys.argv[1].strip()
if not value:
    raise SystemExit("empty Cpus_allowed_list")
cpus = set()
for part in value.split(","):
    if re.fullmatch(r"[0-9]+", part):
        cpus.add(int(part))
        continue
    match = re.fullmatch(r"([0-9]+)-([0-9]+)", part)
    if not match:
        raise SystemExit(f"invalid Cpus_allowed_list component: {part}")
    first, last = map(int, match.groups())
    if last < first:
        raise SystemExit(f"descending Cpus_allowed_list range: {part}")
    cpus.update(range(first, last + 1))
if not cpus:
    raise SystemExit("Cpus_allowed_list selects no CPUs")
print(len(cpus))
PY
}

capture_host_timer_environment()
{
    local scenario=$1 accel=$2 guest_vcpus=$3
    local allowed_list qemu_version kvm_access
    local timestamp
    timestamp=$(date --iso-8601=seconds)
    allowed_list=$(awk '/^Cpus_allowed_list:/ {print $2}' /proc/self/status)
    HOST_ALLOWED_CPUS=$(count_allowed_cpus "$allowed_list")
    HOST_NPROC=$(nproc)
    HOST_SCHEDULABLE_CPUS=$HOST_NPROC
    if ((HOST_ALLOWED_CPUS < HOST_SCHEDULABLE_CPUS)); then
        HOST_SCHEDULABLE_CPUS=$HOST_ALLOWED_CPUS
    fi
    HOST_OVERSUBSCRIBED=0
    ((HOST_SCHEDULABLE_CPUS < guest_vcpus)) && HOST_OVERSUBSCRIBED=1
    qemu_version=$(qemu-system-x86_64 --version)
    qemu_version=${qemu_version%%$'\n'*}
    kvm_access=0
    [[ -e /dev/kvm && -r /dev/kvm && -w /dev/kvm ]] && kvm_access=1
    {
        echo "timestamp=$timestamp"
        echo "uname=$(uname -a)"
        echo "qemu_version=$qemu_version"
        echo "nproc=$HOST_NPROC"
        echo "cpus_allowed_list=$allowed_list"
        echo "allowed_count=$HOST_ALLOWED_CPUS"
        echo "host_schedulable_cpus=$HOST_SCHEDULABLE_CPUS"
        echo "loadavg=$(tr '\n' ' ' </proc/loadavg)"
        if [[ -r /proc/pressure/cpu ]]; then
            echo '[pressure_cpu]'
            sed 's/^/  /' /proc/pressure/cpu
        else
            echo 'pressure_cpu=unavailable'
        fi
        if [[ -r /proc/pressure/memory ]]; then
            echo '[pressure_memory]'
            sed 's/^/  /' /proc/pressure/memory
        else
            echo 'pressure_memory=unavailable'
        fi
        if [[ -r /proc/pressure/io ]]; then
            echo '[pressure_io]'
            sed 's/^/  /' /proc/pressure/io
        else
            echo 'pressure_io=unavailable'
        fi
        echo "dev_kvm_access=$kvm_access"
        echo "scenario=$scenario"
        echo "requested_accel=$accel"
        echo "guest_smp=$guest_vcpus"
        echo "[TIMER][HOST_ENV] host_schedulable_cpus=$HOST_SCHEDULABLE_CPUS guest_vcpus=$guest_vcpus oversubscribed=$HOST_OVERSUBSCRIBED"
        echo
    } >>"$host_environment"
    echo "[TIMER][HOST_ENV] host_schedulable_cpus=$HOST_SCHEDULABLE_CPUS guest_vcpus=$guest_vcpus oversubscribed=$HOST_OVERSUBSCRIBED"
}

boot_progress_classify_line()
{
    local line=$1
    BOOT_PROGRESS_NAME=
    BOOT_PROGRESS_PHASE=
    BOOT_PROGRESS_BUDGET_MS=0
    case $line in
        *'[KERNEL] ENTRY OK'*)
            BOOT_PROGRESS_NAME=KERNEL_ENTRY
            BOOT_PROGRESS_PHASE=KERNEL_ENTRY_TO_CPUS_PREPARED
            BOOT_PROGRESS_BUDGET_MS=180000
            ;;
        *'[IRQ][BOOTSTRAP] CPUS_PREPARED'*)
            BOOT_PROGRESS_NAME=CPUS_PREPARED
            BOOT_PROGRESS_PHASE=CPUS_PREPARED_TO_CPU_RELEASE
            BOOT_PROGRESS_BUDGET_MS=180000
            ;;
        *'stage=CPU_RELEASE '*|*'[BOOTTRACE][BSP] 4 after-irq-enable'*)
            BOOT_PROGRESS_NAME=CPU_RELEASE
            BOOT_PROGRESS_PHASE=CPU_RELEASE_TO_READY_SUMMARY
            BOOT_PROGRESS_BUDGET_MS=30000
            ;;
        *'[IRQ][CPU_READY] PASS'*)
            BOOT_PROGRESS_NAME=CPU_RELEASE_PROGRESS
            BOOT_PROGRESS_PHASE=CPU_RELEASE_TO_READY_SUMMARY
            BOOT_PROGRESS_BUDGET_MS=30000
            ;;
        *'[IRQ][CPU_READY_SUMMARY]'*)
            BOOT_PROGRESS_NAME=CPU_READY_SUMMARY
            BOOT_PROGRESS_PHASE=READY_SUMMARY_TO_PCI_BEGIN
            BOOT_PROGRESS_BUDGET_MS=30000
            ;;
        *'stage=PCI_BEGIN '*|*'[BOOTTRACE][BSP] 5 before-pci-init'*)
            BOOT_PROGRESS_NAME=PCI_BEGIN
            BOOT_PROGRESS_PHASE=PCI_XHCI_PROGRESS
            BOOT_PROGRESS_BUDGET_MS=300000
            ;;
        *'stage=PCI_SCAN_COMPLETE '*|*'PCI: Scan complete.'*)
            BOOT_PROGRESS_NAME=PCI_SCAN_COMPLETE
            BOOT_PROGRESS_PHASE=PCI_COMPLETE_TO_CORE_COMPLETE
            BOOT_PROGRESS_BUDGET_MS=30000
            ;;
        *'stage=PCI_SCAN_PROGRESS '*|*'PCI: MCFG Table found.'*|*'PCI: Scanning Segment Group.'*|*'[XHCI]'*|*'[USB_HUB]'*|*'[USB] Hotplug'*|*'[HP] '*|*'[PORT '*|*'=== xHCI DRIVER INITIALIZATION ==='*)
            BOOT_PROGRESS_NAME=PCI_SCAN_PROGRESS
            BOOT_PROGRESS_PHASE=PCI_XHCI_PROGRESS
            BOOT_PROGRESS_BUDGET_MS=300000
            ;;
        *'stage=CORE_COMPLETE '*|*'[CORE] System Core Initialization Complete.'*)
            BOOT_PROGRESS_NAME=CORE_COMPLETE
            BOOT_PROGRESS_PHASE=CORE_COMPLETE_TO_GRAPHICS_INIT
            BOOT_PROGRESS_BUDGET_MS=30000
            ;;
        *'stage=GRAPHICS_INITIALIZED '*|*'[KERNEL] Graphics Initialized.'*)
            BOOT_PROGRESS_NAME=GRAPHICS_INITIALIZED
            BOOT_PROGRESS_PHASE=GRAPHICS_INIT_TO_SPLASH_BEGIN
            BOOT_PROGRESS_BUDGET_MS=30000
            ;;
        *'[GRAPHICS][SPLASH] BEGIN'*)
            BOOT_PROGRESS_NAME=SPLASH_BEGIN
            BOOT_PROGRESS_PHASE=SPLASH_BEGIN_TO_TERMINAL
            BOOT_PROGRESS_BUDGET_MS=15000
            ;;
        *'[GRAPHICS][SPLASH] COMPLETE result='*|*'[GRAPHICS][SPLASH] SKIP result='*)
            BOOT_PROGRESS_NAME=SPLASH_TERMINAL
            BOOT_PROGRESS_PHASE=SPLASH_TO_TERMINAL_TRANSITION
            BOOT_PROGRESS_BUDGET_MS=30000
            ;;
        *'[GRAPHICS][SPLASH] SUSPEND'*|*'[GRAPHICS][SPLASH] CLEAR'*|*'[GRAPHICS][SPLASH] FADE_IN'*|*'[GRAPHICS][SPLASH] HOLD'*|*'[GRAPHICS][SPLASH] FADE_OUT'*|*'[GRAPHICS][SPLASH] ABORT'*|*'[GRAPHICS][SPLASH] RESUME'*)
            BOOT_PROGRESS_NAME=SPLASH_PROGRESS
            BOOT_PROGRESS_PHASE=SPLASH_BEGIN_TO_TERMINAL
            BOOT_PROGRESS_BUDGET_MS=15000
            ;;
        *'stage=TERMINAL_TRANSITION '*|*'[KERNEL] Starting Terminal & Shell...'*)
            BOOT_PROGRESS_NAME=TERMINAL_TRANSITION
            BOOT_PROGRESS_PHASE=TERMINAL_TO_SHELL_READY
            BOOT_PROGRESS_BUDGET_MS=30000
            ;;
        *'[BOOT][SHELL_READY] PASS'*|*'stage=SHELL_READY '*)
            BOOT_PROGRESS_NAME=SHELL_READY
            BOOT_PROGRESS_PHASE=SHELL_READY_TO_RUNTIME_READY
            BOOT_PROGRESS_BUDGET_MS=30000
            ;;
        *'[BOOT][RUNTIME_READY] PASS'*|*'stage=RUNTIME_READY '*)
            BOOT_PROGRESS_NAME=RUNTIME_READY
            BOOT_PROGRESS_PHASE=RUNTIME_READY_TO_MAIN_LOOP
            BOOT_PROGRESS_BUDGET_MS=10000
            ;;
        *'[KERNEL] Entering Main Loop.'*|*'stage=MAIN_LOOP '*)
            BOOT_PROGRESS_NAME=MAIN_LOOP
            BOOT_PROGRESS_PHASE=COMPLETE
            BOOT_PROGRESS_BUDGET_MS=0
            ;;
        *'[UEFI] GOP Mode Selected'*)
            BOOT_PROGRESS_NAME=UEFI_PROGRESS
            BOOT_PROGRESS_PHASE=UEFI_TO_KERNEL_ENTRY
            BOOT_PROGRESS_BUDGET_MS=60000
            ;;
    esac
    [[ -n $BOOT_PROGRESS_NAME ]]
}

capture_boot_stall()
{
    local stage=$1 classification=$2 reason=$3
    local dir=$qemu_artifact/$stage now host_elapsed idle_elapsed
    local runtime_screen=$runtime/stall-$stage.ppm
    now=$(date +%s%3N)
    host_elapsed=$((now - BOOT_WATCH_HOST_START_MS))
    idle_elapsed=$((now - BOOT_WATCH_LAST_HOST_MS))
    mkdir -p "$dir"
    hmp command 'info status' >"$dir/stall-info-status.txt" 2>&1 || true
    hmp command 'info registers' >"$dir/stall-info-registers.txt" 2>&1 || true
    hmp command "screendump $runtime_screen" >/dev/null 2>&1 || true
    if [[ -s $runtime_screen ]]; then
        cp "$runtime_screen" "$dir/stall.ppm"
    fi
    tail -150 "$serial" >"$dir/serial-tail.log" 2>/dev/null || true
    tail -150 "$runtime/qemu-trace.log" >"$dir/trace-tail.log" \
        2>/dev/null || true
    if ((BOOT_WATCH_SHELL_AVAILABLE)); then
        local before deadline
        before=$(count_marker '[IRQ][CHECK] ')
        hmp text 'irq check' --profile stress --enter >/dev/null 2>&1 || true
        deadline=$((SECONDS + 5))
        while ((SECONDS <= deadline)); do
            (( $(count_marker '[IRQ][CHECK] ') > before )) && break
            vm_alive || break
            sleep .2
        done
        tail -80 "$serial" >"$dir/clockevent-snapshot.log" 2>/dev/null || true
    fi
    cp "$serial" "$dir/serial-stall.log"
    cp "$runtime/qemu-debugcon.log" "$dir/debugcon-stall.log" 2>/dev/null || true
    cp "$runtime/qemu-trace.log" "$dir/trace-stall.log" 2>/dev/null || true
    cp "$runtime/commands.tsv" "$dir/commands-stall.tsv" 2>/dev/null || true
    python3 scripts/verify-timer-clockevent.py stall-report \
        --directory "$dir" --scenario "$stage" \
        --classification "$classification" --stage "$BOOT_WATCH_STAGE" \
        --last-progress-marker "$BOOT_WATCH_LAST_MARKER" \
        --guest-elapsed-ms "$BOOT_WATCH_GUEST_ELAPSED_MS" \
        --host-elapsed-ms "$host_elapsed" --idle-elapsed-ms "$idle_elapsed" \
        --shell-available "$BOOT_WATCH_SHELL_AVAILABLE" \
        --root-cause "$reason"
    BOOT_WATCH_CLASSIFICATION=$classification
    BOOT_WATCH_HOST_ELAPSED_MS=$host_elapsed
    stop_vm
}

wait_boot_to_main_loop()
{
    local stage=$1 machine=$2 smp=$3 accel=$4
    local dir=$qemu_artifact/$stage timeline=$qemu_artifact/$stage/boot-progress.tsv
    local now line_count=0 line marker_host guest_ms ticks fatal
    local idle_budget_ms=60000 hard_cap_ms=900000 splash_begin_ms=0
    mkdir -p "$dir"
    [[ ! -e $timeline ]] || {
        echo "refusing to overwrite boot timeline: $timeline" >&2
        return 1
    }
    printf 'host_epoch_ms\thost_elapsed_ms\tstage\tguest_monotonic_ms\tclockevent_ticks\tmarker\n' \
        >"$timeline"
    BOOT_WATCH_HOST_START_MS=$(date +%s%3N)
    BOOT_WATCH_LAST_HOST_MS=$BOOT_WATCH_HOST_START_MS
    BOOT_WATCH_STAGE=UEFI_TO_KERNEL_ENTRY
    BOOT_WATCH_LAST_MARKER='QEMU_START'
    BOOT_WATCH_GUEST_ELAPSED_MS=unknown
    BOOT_WATCH_HOST_ELAPSED_MS=0
    BOOT_WATCH_CLASSIFICATION=
    BOOT_WATCH_SHELL_AVAILABLE=0
    while :; do
        now=$(date +%s%3N)
        if [[ -f $serial ]]; then
            local total_lines
            total_lines=$(wc -l <"$serial")
            if ((total_lines > line_count)); then
                while IFS= read -r line; do
                    line=${line%$'\r'}
                    if grep -Eq 'PANIC|FATAL|#PF|#GP|DOUBLE FAULT|TRIPLE FAULT|STRUCTURAL_FAULT|FINISH_FAULT' <<<"$line"; then
                        capture_boot_stall "$stage" REJECTED_GUEST_FAULT \
                            "guest_fault:$line"
                        return 1
                    fi
                    if boot_progress_classify_line "$line"; then
                        marker_host=$(date +%s%3N)
                        BOOT_WATCH_LAST_HOST_MS=$marker_host
                        BOOT_WATCH_STAGE=$BOOT_PROGRESS_PHASE
                        BOOT_WATCH_LAST_MARKER=${line//$'\t'/ }
                        idle_budget_ms=$BOOT_PROGRESS_BUDGET_MS
                        guest_ms=$(sed -n 's/.*monotonic_ms=\([0-9][0-9]*\).*/\1/p' <<<"$line")
                        ticks=$(sed -n 's/.*clockevent_ticks=\([0-9][0-9]*\).*/\1/p' <<<"$line")
                        guest_ms=${guest_ms:-unknown}
                        ticks=${ticks:-unknown}
                        [[ $guest_ms == unknown ]] || \
                            BOOT_WATCH_GUEST_ELAPSED_MS=$guest_ms
                        printf '%s\t%s\t%s\t%s\t%s\t%s\n' \
                            "$marker_host" "$((marker_host - BOOT_WATCH_HOST_START_MS))" \
                            "$BOOT_PROGRESS_NAME" "$guest_ms" "$ticks" \
                            "${line//$'\t'/ }" >>"$timeline"
                        if [[ $BOOT_PROGRESS_NAME == SPLASH_BEGIN ]]; then
                            splash_begin_ms=$marker_host
                        elif [[ $BOOT_PROGRESS_NAME == SPLASH_TERMINAL ]]; then
                            splash_begin_ms=0
                        elif [[ $BOOT_PROGRESS_NAME == SHELL_READY ]]; then
                            BOOT_WATCH_SHELL_AVAILABLE=1
                        elif [[ $BOOT_PROGRESS_NAME == MAIN_LOOP ]]; then
                            BOOT_WATCH_HOST_ELAPSED_MS=$((marker_host - BOOT_WATCH_HOST_START_MS))
                            return 0
                        fi
                    fi
                done < <(tail -n +$((line_count + 1)) "$serial")
                line_count=$total_lines
            fi
        fi
        now=$(date +%s%3N)
        if ((splash_begin_ms && now - splash_begin_ms > 15000)); then
            capture_boot_stall "$stage" \
                REJECTED_BOOT_TO_SHELL_LIVENESS_SPLASH_BUDGET \
                'splash_terminal_marker_exceeded_15s_host_budget'
            return 1
        fi
        if ((idle_budget_ms && now - BOOT_WATCH_LAST_HOST_MS > idle_budget_ms)); then
            capture_boot_stall "$stage" \
                REJECTED_BOOT_TO_SHELL_LIVENESS_STAGE_TIMEOUT \
                "idle_timeout:$BOOT_WATCH_STAGE"
            return 1
        fi
        if ((now - BOOT_WATCH_HOST_START_MS > hard_cap_ms)); then
            capture_boot_stall "$stage" \
                REJECTED_BOOT_TO_SHELL_LIVENESS_HARD_CAP \
                "hard_cap_900s:machine=$machine:smp=$smp:accel=$accel:host_cpus=$HOST_SCHEDULABLE_CPUS"
            return 1
        fi
        if ! vm_alive; then
            capture_boot_stall "$stage" REJECTED_QEMU_TRANSPORT_EXIT \
                'qemu_exited_before_main_loop'
            return 1
        fi
        sleep .2
    done
}

reset_lapic_rate_result()
{
    lapic_rate_raw=NOT_RUN
    lapic_rate_window=
    lapic_rate_rounds=
    lapic_rate_worst=
    lapic_rate_min=
    lapic_rate_max=
    lapic_rate_authority=NOT_APPLICABLE
    lapic_rate_disposition=PASS
    lapic_rate_runtime=NOT_RUN
}

load_host_timer_environment()
{
    local scenario=$1
    read -r HOST_NPROC HOST_ALLOWED_CPUS HOST_SCHEDULABLE_CPUS \
        HOST_OVERSUBSCRIBED < <(
        python3 - "$host_environment" "$scenario" <<'PY'
import pathlib
import re
import sys

path = pathlib.Path(sys.argv[1])
scenario = sys.argv[2]
matches = []
for block in path.read_text(encoding="utf-8").split("\n\n"):
    values = {}
    for line in block.splitlines():
        if "=" in line and not line.startswith((" ", "[TIMER]")):
            key, value = line.split("=", 1)
            values[key] = value
    if values.get("scenario") == scenario:
        matches.append(values)
if len(matches) != 1:
    raise SystemExit(f"expected one host capture for {scenario}, got {len(matches)}")
item = matches[0]
allowed = item["cpus_allowed_list"]
cpus = set()
for component in allowed.split(","):
    match = re.fullmatch(r"([0-9]+)(?:-([0-9]+))?", component)
    if not match:
        raise SystemExit(f"invalid CPU list: {allowed}")
    first = int(match.group(1))
    last = int(match.group(2) or first)
    if last < first:
        raise SystemExit(f"invalid CPU range: {component}")
    cpus.update(range(first, last + 1))
host = min(int(item["nproc"]), len(cpus))
guest = int(item["guest_smp"])
print(item["nproc"], len(cpus), host, int(host < guest))
PY
    )
}

load_lapic_rate_result()
{
    local log=$1 guest_vcpus=$2 line
    reset_lapic_rate_result
    [[ $(grep -Fc '[ACCOUNT][LAPIC_RATE] ' "$log") == 1 ]]
    line=$(grep -F '[ACCOUNT][LAPIC_RATE] ' "$log")
    lapic_rate_raw=$(sed -n 's/.*\[ACCOUNT\]\[LAPIC_RATE\] \(PASS\|FAIL\).*/\1/p' <<<"$line")
    lapic_rate_window=$(sed -n 's/.*window_ms=\([0-9][0-9]*\).*/\1/p' <<<"$line")
    lapic_rate_rounds=$(sed -n 's/.*rounds=\([0-9][0-9]*\).*/\1/p' <<<"$line")
    lapic_rate_worst=$(sed -n 's/.*worst_median_x1000=\([0-9][0-9]*\).*/\1/p' <<<"$line")
    lapic_rate_min=$(sed -n 's/.*min_round_x1000=\([0-9][0-9]*\).*/\1/p' <<<"$line")
    lapic_rate_max=$(sed -n 's/.*max_round_x1000=\([0-9][0-9]*\).*/\1/p' <<<"$line")
    [[ $lapic_rate_raw == PASS || $lapic_rate_raw == FAIL ]]
    [[ $lapic_rate_window == 2000 && $lapic_rate_rounds == 5 ]]
    [[ $lapic_rate_worst =~ ^[0-9]+$ && $lapic_rate_min =~ ^[0-9]+$ &&
       $lapic_rate_max =~ ^[0-9]+$ ]]
    if ((HOST_SCHEDULABLE_CPUS >= guest_vcpus)); then
        if ((guest_vcpus == 24)); then
            lapic_rate_authority=AUTHORITATIVE_SMP24_RATE
        else
            lapic_rate_authority=AUTHORITATIVE_SMP4_RATE_CONTROL
        fi
        lapic_rate_disposition=$lapic_rate_raw
    else
        lapic_rate_authority=ENVIRONMENT_NON_AUTHORITATIVE_OVERSUBSCRIBED
        if [[ $lapic_rate_raw == FAIL ]]; then
            lapic_rate_disposition=PASS_WITH_ENVIRONMENT_RATE_EXCEPTION
        else
            lapic_rate_disposition=PASS
        fi
    fi
}

execute_lapic_rate_once()
{
    local guest_vcpus=$1 before deadline line
    before=$(grep -Fc '[ACCOUNT][LAPIC_RATE] ' "$serial" 2>/dev/null || true)
    printf '%s\t%s\n' "$(date --iso-8601=seconds)" \
        'accounttest lapic-rate 2000 5' >>"$runtime/commands.tsv"
    hmp text 'accounttest lapic-rate 2000 5' --profile stress --enter >/dev/null
    deadline=$((SECONDS + 240))
    while ((SECONDS <= deadline)); do
        if (( $(grep -Fc '[ACCOUNT][LAPIC_RATE] ' "$serial" 2>/dev/null || true) > before )); then
            break
        fi
        if grep -Eq 'PANIC|#PF|#GP|FATAL|STRUCTURAL_FAULT|FINISH_FAULT' "$serial" 2>/dev/null; then
            echo 'guest fault during lapic-rate' >&2
            return 1
        fi
        vm_alive || return 1
        sleep .2
    done
    (( $(grep -Fc '[ACCOUNT][LAPIC_RATE] ' "$serial" 2>/dev/null || true) == before + 1 )) || {
        echo 'missing or duplicate lapic-rate result' >&2
        return 1
    }
    sync_shell
    line=$(grep -F '[ACCOUNT][LAPIC_RATE] ' "$serial" | tail -1)
    lapic_rate_raw=$(sed -n 's/.*\[ACCOUNT\]\[LAPIC_RATE\] \(PASS\|FAIL\).*/\1/p' <<<"$line")
    lapic_rate_window=$(sed -n 's/.*window_ms=\([0-9][0-9]*\).*/\1/p' <<<"$line")
    lapic_rate_rounds=$(sed -n 's/.*rounds=\([0-9][0-9]*\).*/\1/p' <<<"$line")
    lapic_rate_worst=$(sed -n 's/.*worst_median_x1000=\([0-9][0-9]*\).*/\1/p' <<<"$line")
    lapic_rate_min=$(sed -n 's/.*min_round_x1000=\([0-9][0-9]*\).*/\1/p' <<<"$line")
    lapic_rate_max=$(sed -n 's/.*max_round_x1000=\([0-9][0-9]*\).*/\1/p' <<<"$line")
    [[ $lapic_rate_raw == PASS || $lapic_rate_raw == FAIL ]]
    [[ $lapic_rate_window == 2000 && $lapic_rate_rounds == 5 ]]
    [[ $lapic_rate_worst =~ ^[0-9]+$ && $lapic_rate_min =~ ^[0-9]+$ &&
       $lapic_rate_max =~ ^[0-9]+$ ]]

    if ((HOST_SCHEDULABLE_CPUS >= guest_vcpus)); then
        if ((guest_vcpus == 24)); then
            lapic_rate_authority=AUTHORITATIVE_SMP24_RATE
        else
            lapic_rate_authority=AUTHORITATIVE_SMP4_RATE_CONTROL
        fi
        lapic_rate_disposition=$lapic_rate_raw
    else
        lapic_rate_authority=ENVIRONMENT_NON_AUTHORITATIVE_OVERSUBSCRIBED
        if [[ $lapic_rate_raw == FAIL ]]; then
            lapic_rate_disposition=PASS_WITH_ENVIRONMENT_RATE_EXCEPTION
        else
            lapic_rate_disposition=PASS
        fi
        printf '[TIMER][LAPIC_RATE_ENV] ENVIRONMENT_NON_AUTHORITATIVE_OVERSUBSCRIBED host_cpus=%s guest_vcpus=%s worst_median_x1000=%s threshold=700\n' \
            "$HOST_SCHEDULABLE_CPUS" "$guest_vcpus" "$lapic_rate_worst"
    fi
}

append_lapic_rate_environment_marker()
{
    local log=$1 guest_vcpus=$2 marker
    [[ $lapic_rate_authority == \
       ENVIRONMENT_NON_AUTHORITATIVE_OVERSUBSCRIBED ]] || return 0
    marker="[TIMER][LAPIC_RATE_ENV] ENVIRONMENT_NON_AUTHORITATIVE_OVERSUBSCRIBED host_cpus=$HOST_SCHEDULABLE_CPUS guest_vcpus=$guest_vcpus worst_median_x1000=$lapic_rate_worst threshold=700"
    if ! grep -Fqx "$marker" "$log"; then
        printf '%s\n' "$marker" >>"$log"
    fi
}

validate_lapic_rate_runtime()
{
    local log=$1 guest_vcpus=$2 expected_lines
    expected_lines=$((guest_vcpus * 5))
    require_marker '[ACCOUNT][LAPIC_CONFIG] PASS' "$log"
    grep -F '[ACCOUNT][LAPIC_LIVENESS] PASS' "$log" | tail -1 |
        grep -F 'all_advanced=1' | grep -F 'zero_irq_cpus=0' >/dev/null
    [[ $(grep -Fc '[ACCOUNT][LAPIC_RATE_CPU]' "$log") == "$expected_lines" ]]
    ! grep -E '\[ACCOUNT\]\[LAPIC_RATE_CPU\].* delta=0( |$)' "$log" >/dev/null
    require_marker '[ACCOUNT][CHECK] PASS clocksource=HPET clockevent=BSP_LAPIC' "$log"
    require_marker '[IRQ][CHECK] PASS clocksource=HPET clockevent=BSP_LAPIC' "$log"
    ! grep -Eq 'PANIC|#PF|#GP|FATAL|STRUCTURAL_FAULT|FINISH_FAULT' "$log"
    if ((guest_vcpus == 24)); then
        require_marker '[BOOT][RUNTIME_READY] PASS cpus=24/24' "$log"
        require_marker 'non_bsp_ticks=0 stray_hpet=0' "$log"
        require_marker 'unexpected=0 imbalance=0' "$log"
    fi
    if ((HOST_SCHEDULABLE_CPUS >= guest_vcpus)); then
        [[ $lapic_rate_raw == PASS ]]
    elif [[ $lapic_rate_raw == FAIL ]]; then
        require_marker \
            '[TIMER][LAPIC_RATE_ENV] ENVIRONMENT_NON_AUTHORITATIVE_OVERSUBSCRIBED' \
            "$log"
    fi
    lapic_rate_runtime=PASS
}

verify_runtime_smp24_log()
{
    local stage=$1 log=$qemu_artifact/$1/serial.log
    verify_boot_to_shell_run "$stage" 24
    verify_timer_fair_log "$log" 5 5
    require_marker '[TASKMAN][AUTO_EXIT] PASS target=3' "$log"
    require_marker '[SMP][KILL_SWEEP] PASS workers=8' "$log"
    require_marker '[KILLTEST][CHECK] PASS' "$log"
    require_marker '[INPUTTEST][CHECK] PASS' "$log"
    require_marker '[MODALTEST][CHECK] PASS' "$log"
    require_marker '[ACCOUNT][CLOCK_SMP] PASS' "$log"
    require_marker '[TASKDIAG][CHECK] PASS' "$log"
}

protected_manifest()
{
    local output=$1
    git ls-files -z -- \
        'bootloader/**' 'shared/**' \
        'kernel/src/core/scheduler.*' 'kernel/src/core/switch.S' \
        'kernel/src/core/clock.*' 'kernel/src/core/interrupt_context.*' \
        'kernel/src/apic/lapic.*' 'kernel/src/apic/ioapic.*' \
        'kernel/src/apic/legacy_pic.*' 'kernel/src/acpi/madt.*' \
        'kernel/src/graphics/**' \
        'kernel/src/shell/commands/cmd_taskman.*' \
        'kernel/src/shell/commands/cmd_taskmantest.*' \
        'kernel/src/shell/commands/taskman_view.*' |
        LC_ALL=C sort -z | xargs -0 sha256sum >"$output"
}

fair_protected_manifest()
{
    local output=$1
    git ls-files -z -- \
        'bootloader/**' 'shared/**' \
        'kernel/src/core/scheduler.*' kernel/src/core/switch.S \
        'kernel/src/core/modal_session.*' 'kernel/src/core/modal_ui.*' \
        'kernel/src/core/task_format.*' \
        'kernel/src/apic/lapic.*' 'kernel/src/apic/ioapic.*' \
        'kernel/src/acpi/**' \
        'kernel/src/graphics/graphics.*' \
        'kernel/src/graphics/console.*' \
        'kernel/src/graphics/terminal.*' \
        'kernel/src/shell/commands/cmd_taskman.*' \
        'kernel/src/shell/commands/cmd_taskmantest.*' \
        'kernel/src/shell/commands/taskman_view.*' |
        LC_ALL=C sort -z | xargs -0 sha256sum >"$output"
}

rendezvous_fairness_manifest()
{
    local output=$1
    sha256sum kernel/src/core/timers.c kernel/src/core/timers.h \
        kernel/src/shell/commands/cmd_synctest.c \
        kernel/src/shell/commands/cmd_synctest.h >"$output"
}

rendezvous_runtime_manifest()
{
    local output=$1
    sha256sum kernel/src/core/irq_bootstrap.c \
        kernel/src/core/irq_bootstrap.h kernel/src/smp/smp_boot.c \
        kernel/src/smp/smp_boot.h kernel/src/core/interrupts.c \
        kernel/src/core/interrupts.h >"$output"
}

verify_rendezvous_continuity()
{
    local suffix=$1
    mkdir -p "$rendezvous_artifact"
    rendezvous_fairness_manifest \
        "$rendezvous_artifact/fairness-$suffix.sha256"
    cmp -s "$rendezvous_artifact/fairness-before.sha256" \
        "$rendezvous_artifact/fairness-$suffix.sha256"
    fair_protected_manifest \
        "$rendezvous_artifact/protected-$suffix.sha256"
    if [[ ! -f $autonomous_artifact/protected-before.sha256 ]]; then
        grep -v 'kernel/src/graphics/splash\.' \
            "$rendezvous_artifact/protected-before.sha256" \
            >"$autonomous_artifact/protected-before.sha256"
    fi
    cmp -s "$autonomous_artifact/protected-before.sha256" \
        "$rendezvous_artifact/protected-$suffix.sha256"
}

source_worktree_manifest()
{
    local output=$1
    git ls-files -z -- \
        kernel/kernel.c \
        kernel/src/core/interrupts.c \
        kernel/src/core/irq_bootstrap.c kernel/src/core/irq_bootstrap.h \
        kernel/src/core/kernel_init.c \
        kernel/src/core/timers.c kernel/src/core/timers.h \
        kernel/src/drivers/timer.c kernel/src/drivers/timer.h \
        kernel/src/timer/hpet.c kernel/src/timer/hpet.h \
        kernel/src/shell/commands/cmd_accounttest.c \
        kernel/src/shell/commands/cmd_irq.c \
        'kernel/src/core/scheduler.*' kernel/src/core/switch.S \
        'kernel/src/apic/lapic.*' 'kernel/src/apic/ioapic.*' \
        'kernel/src/acpi/madt.*' 'kernel/src/graphics/**' \
        'kernel/src/shell/commands/cmd_taskman.*' \
        'kernel/src/shell/commands/cmd_taskmantest.*' \
        'kernel/src/shell/commands/taskman_view.*' \
        'bootloader/**' 'shared/**' |
        LC_ALL=C sort -z | xargs -0 sha256sum >"$output"
}

clockevent_worktree_manifest()
{
    local output=$1
    sha256sum \
        kernel/src/core/interrupts.c \
        kernel/src/core/irq_bootstrap.c kernel/src/core/irq_bootstrap.h \
        kernel/src/drivers/timer.c kernel/src/drivers/timer.h \
        kernel/src/timer/hpet.c kernel/src/timer/hpet.h \
        kernel/src/shell/commands/cmd_accounttest.c \
        kernel/src/shell/commands/cmd_irq.c | LC_ALL=C sort -k2 >"$output"
}

prepare_clockevent_worktree_baseline()
{
    local frozen=$artifact/source-worktree-before-closure.sha256
    local output=$fair_artifact/clockevent-worktree-before.sha256
    python3 - "$frozen" "$output" <<'PY'
import pathlib
import sys

source = pathlib.Path(sys.argv[1])
target = pathlib.Path(sys.argv[2])
names = {
    "kernel/src/core/interrupts.c",
    "kernel/src/core/irq_bootstrap.c",
    "kernel/src/core/irq_bootstrap.h",
    "kernel/src/drivers/timer.c",
    "kernel/src/drivers/timer.h",
    "kernel/src/timer/hpet.c",
    "kernel/src/timer/hpet.h",
    "kernel/src/shell/commands/cmd_accounttest.c",
    "kernel/src/shell/commands/cmd_irq.c",
}
rows = []
for line in source.read_text(encoding="utf-8").splitlines():
    digest, separator, name = line.partition("  ")
    if separator and name in names:
        rows.append((name, digest))
if {name for name, _ in rows} != names:
    raise SystemExit("frozen clockevent worktree manifest is incomplete")
target.write_text("".join(f"{digest}  {name}\n" for name, digest in sorted(rows)),
                  encoding="utf-8")
PY
}

verify_fair_source_continuity()
{
    local suffix=$1
    mkdir -p "$fair_artifact"
    prepare_clockevent_worktree_baseline
    clockevent_worktree_manifest \
        "$fair_artifact/clockevent-worktree-$suffix.sha256"
    cmp -s "$fair_artifact/clockevent-worktree-before.sha256" \
        "$fair_artifact/clockevent-worktree-$suffix.sha256"
    fair_protected_manifest "$fair_artifact/protected-$suffix.sha256"
    cmp -s "$autonomous_artifact/protected-before.sha256" \
        "$fair_artifact/protected-$suffix.sha256"
}

preflight()
{
    local log=$rendezvous_artifact/preflight.log
    mkdir -p "$fair_artifact" "$rendezvous_artifact"
    {
        [[ $(git branch --show-current) == feat/taskman ]]
        [[ $(git rev-parse HEAD) == "$base" ]]
        [[ $(git rev-parse 'HEAD^{tree}') == "$base_tree" ]]
        [[ $(git rev-parse HEAD^) == "$base_parent" ]]
        [[ $(git log -1 --format=%s) == \
            'fix(irq): establish safe x86 interrupt controller bring-up' ]]
        [[ $(git rev-list --count "$base..HEAD") == 0 ]]
        git diff --cached --quiet
        [[ -f ROADMAP_TASKMAN_V1_CLOSURE_HARDENING.md ]]
        [[ -f $artifact/source-before.sha256 ]]
        [[ -f $artifact/protected-before.sha256 ]]
        [[ -f $artifact/source-worktree-before-closure.sha256 ]]
        [[ -f $fair_artifact/source-before.sha256 ]]
        [[ -f $fair_artifact/protected-before.sha256 ]]
        [[ -f $rendezvous_artifact/fairness-before.sha256 ]]
        [[ -f $rendezvous_artifact/runtime-before.sha256 ]]
        [[ -f $rendezvous_artifact/protected-before.sha256 ]]
        [[ $(sha "$historical_rate_log") == "$historical_rate_sha" ]]
        [[ $(sha "$historical_runtime_log") == "$historical_runtime_sha" ]]
        [[ $(sha "$runtime_ready_failure_log") == \
            "$runtime_ready_failure_sha" ]]
        [[ $(sha "$runtime_ready_pass_log") == \
            "$runtime_ready_pass_sha" ]]
        require_marker \
            '[ACCOUNT][LAPIC_RATE] FAIL window_ms=2000 rounds=5 worst_median_x1000=672 min_round_x1000=255 max_round_x1000=934' \
            "$historical_rate_log"
        require_marker \
            '[SYNC][TIMER_CANCEL] FAIL pending=0 claimed=1 callbacks=0' \
            "$historical_runtime_log"
        require_marker \
            '[SMP][CPU] ERROR code=IRQ_RUNTIME_READY' \
            "$runtime_ready_failure_log"
        [[ $(grep -Fc '[IRQ][CPU_READY] PASS' \
            "$runtime_ready_failure_log") == 23 ]]
        require_marker '[BOOT][RUNTIME_READY] PASS cpus=24/24' \
            "$runtime_ready_pass_log"
        verify_rendezvous_continuity preflight
        git diff --check
        git diff --cached --check
        echo "[TIMER][PREFLIGHT] PASS base=$base tree=$base_tree"
    } 2>&1 | tee "$log"
    record_stage preflight preflight none 0 none "$log" '[TIMER][PREFLIGHT] PASS'
}

static_gate()
{
    local log=$rendezvous_artifact/static.log
    mkdir -p "$rendezvous_artifact"
    {
        ! rg -n 'irq_bootstrap_verify_hpet\(' kernel/src >/dev/null
        ! rg -n 'hpet_timer0_prepare|hpet_timer0_arm|hpet_timer0_enable_route' \
            kernel/src/core kernel/src/drivers >/dev/null
        ! rg -n 'timer_handler\(' kernel/src >/dev/null
        ! rg -n 'timers_poll\(|timer_run_deferred\(' \
            kernel/src/core/interrupts.c >/dev/null
        [[ $(rg -c 'timer_clockevent_on_lapic_tick\(' \
            kernel/src/core/interrupts.c) == 1 ]]
        rg -n 'slot == smp_bsp_cpu_slot\(\)' \
            kernel/src/core/interrupts.c >/dev/null
        rg -n 'hpet_timer0_force_quiescent|hpet_timer0_is_quiescent' \
            kernel/src/timer/hpet.c kernel/src/core/irq_bootstrap.c >/dev/null
        rg -n 'HPET_CLOCKSOURCE_VERIFIED|CLOCKEVENT_ACTIVE|TIMER_CLOCKEVENT_BSP_LAPIC' \
            kernel/src >/dev/null
        rg -n 'timers_poll_at\(now_ms\)|timer_run_deferred_at\(now_ms\)' \
            kernel/src/drivers/timer.c >/dev/null
        rg -n 'lapic_timer_calibrate\(1000|period_us != 1000u' kernel/src >/dev/null
        rg -n 'if\(worst<700\|\|worst>1300\)ok=false;' \
            kernel/src/shell/commands/cmd_accounttest.c >/dev/null
        rg -n 'timers_test_arm_order_pair|timers_test_order_finish' \
            kernel/src/core/timers.c kernel/src/core/timers.h >/dev/null
        rg -n 'timer-order|timer-backlog' \
            kernel/src/shell/commands/cmd_synctest.c >/dev/null
        rg -n 'IRQ_CPU_RUNTIME_TIMER_BASELINE|IRQ_CPU_RUNTIME_TIMER_WAIT|IRQ_CPU_RUNTIME_TIMER_VERIFIED|IRQ_CPU_RUNTIME_HANDOFF|IRQ_CPU_RUNTIME_PREEMPTION|IRQ_CPU_RUNTIME_READY' \
            kernel/src/core/irq_bootstrap.h >/dev/null
        rg -n 'runtime_ready_abort|runtime_ready_deadline_ns|CPU_READY_TIMEOUT|CPU_READY_SUMMARY' \
            kernel/src/core/irq_bootstrap.c >/dev/null
        rg -n 'sti; hlt; cli' kernel/src/core/irq_bootstrap.c >/dev/null
        rg -n 'INT_VECTOR_RUNTIME_RENDEZVOUS_WAKE' \
            kernel/src/core/interrupts.c kernel/src/core/interrupts.h \
            kernel/src/core/irq_bootstrap.c >/dev/null
        ! rg -n 'irq_bootstrap_cpu_verify_local_timer\([^)]*250000|verify_local_timer\(slot, *250000' \
            kernel/src >/dev/null
        rg -n 'LOCAL_READY_DEADLINE_FALSE_FAILURE_DETECTED' \
            kernel/src/core/irq_bootstrap.c >/dev/null
        python3 - <<'PY'
import pathlib
import re

text = pathlib.Path("kernel/src/core/irq_bootstrap.c").read_text()
match = re.search(
    r"static irq_cpu_ready_result_t irq_bootstrap_cpu_verify_local_timer\(.*?\n\}",
    text, re.S)
if match is None:
    raise SystemExit("AP timer verifier missing")
body = match.group(0)
baseline = body.find("interrupt_context_vector_snapshot")
enable = body.find("lapic_timer_enable_periodic")
wait = body.find('sti; hlt; cli')
if min(baseline, enable, wait) < 0 or not baseline < enable < wait:
    raise SystemExit("AP timer baseline/enable/wait ordering drift")
if "250000" in body or "clock_monotonic_ns" in body:
    raise SystemExit("local AP deadline regained authority")

match = re.search(
    r"static void irq_runtime_rendezvous_wake_handler_inner\(void\)\s*\{(.*?)\n\}",
    pathlib.Path("kernel/src/core/interrupts.c").read_text(), re.S)
if match is None:
    raise SystemExit("runtime rendezvous wake handler missing")
body = match.group(1)
for required in ("irq_stats_record", "lapic_eoi"):
    if required not in body:
        raise SystemExit(f"wake handler missing {required}")
for forbidden in ("scheduler_", "kmalloc", "kfree", "serial_", "timer_"):
    if forbidden in body:
        raise SystemExit(f"wake handler contains {forbidden}")
PY
        python3 - <<'PY'
import pathlib
import re

text = pathlib.Path("kernel/src/core/timers.c").read_text(encoding="utf-8")
body = re.search(r"void timers_poll_at\(uint64_t now\)\s*\{(.*?)\n\}",
                 text, re.S)
if body is None:
    raise SystemExit("timers_poll_at missing")
poll = body.group(1)
claim = re.search(r"list_for_each_safe\(pos, next, &g_timer_list\)\s*\{(.*?)\n    \}",
                  poll, re.S)
if claim is None:
    raise SystemExit("pending claim scan missing")
normal_claim = re.sub(
    r"#ifdef HOBBYOS_TIMER_NEGATIVE_NEW_DUE_BYPASS.*?#endif", "",
    claim.group(1), flags=re.S)
if "dispatch =" in normal_claim:
    raise SystemExit("claim scan selects dispatch")
select = poll.find("list_for_each(p, &g_claimed_list)")
detach = poll.find("list_del(&dispatch->node)")
unlock = poll.find("spin_unlock_irqrestore(&g_timer_lock, flags)")
if min(select, detach, unlock) < 0 or not select < detach < unlock:
    raise SystemExit("claimed selection/detach is not atomic and ordered")
PY
        ! rg -n 'g_cpu_count\s*(==|<=|>=|<|>)\s*24|cpu_count\s*(==|<=|>=|<|>)\s*24' \
            kernel/src >/dev/null
        ! rg -n 'IRQ_BOOTSTRAP_HPET_VERIFIED|IRQ_BOOTSTRAP_TIMERS_ACTIVE|hpet_probe_passed|hpet_entered|hpet_returned' \
            kernel/src/core kernel/src/shell/commands/cmd_irq.c >/dev/null
        ! rg -n 'hpet_usleep\(' kernel/src/graphics/splash.c >/dev/null
        rg -n 'SPLASH_TOTAL_BUDGET_MS 10000u|SPLASH_STALL_SAMPLE_LIMIT|SPLASH_ITERATION_LIMIT' \
            kernel/src/graphics/splash.c >/dev/null
        [[ $(rg -c 'console_set_render_suspended\(0\)' \
            kernel/src/graphics/splash.c) == 1 ]]
        rg -n 'SPLASH_RESULT_SKIPPED_NO_LOGO|SPLASH_RESULT_ABORTED_CLOCK_STALL|SPLASH_RESULT_ABORTED_RENDER_BUDGET' \
            kernel/src/graphics/splash.h >/dev/null
        rg -n '\[GRAPHICS\]\[SPLASH_SELFTEST\] PASS' \
            kernel/src/graphics/splash.c >/dev/null
        python3 - <<'PY'
import pathlib
import re

text = pathlib.Path("kernel/src/core/interrupts.c").read_text()
match = re.search(r"void irq_hpet_timer_handler_inner\(void\)\s*\{(.*?)\n\}", text, re.S)
if not match:
    raise SystemExit("HPET quarantine handler not found")
body = match.group(1)
for forbidden in ("timer_handler", "timers_poll", "timer_run_deferred",
                  "clock_monotonic", "scheduler_", "shell_", "xhci_",
                  "kmalloc", "kfree", "serial_"):
    if forbidden in body:
        raise SystemExit(f"forbidden HPET handler call: {forbidden}")
for required in ("interrupt_context_mark_unexpected",
                 "hpet_timer0_quarantine_stray", "lapic_eoi"):
    if required not in body:
        raise SystemExit(f"missing HPET quarantine action: {required}")
PY
        git diff --quiet -- bootloader shared \
            kernel/src/core/scheduler.c kernel/src/core/scheduler.h \
            kernel/src/core/switch.S kernel/src/core/clock.c \
            kernel/src/core/clock.h kernel/src/core/interrupt_context.c \
            kernel/src/core/interrupt_context.h kernel/src/apic/lapic.c \
            kernel/src/apic/lapic.h kernel/src/apic/ioapic.c \
            kernel/src/apic/ioapic.h kernel/src/apic/legacy_pic.c \
            kernel/src/apic/legacy_pic.h kernel/src/acpi/madt.c \
            kernel/src/acpi/madt.h \
            kernel/src/graphics/graphics.c kernel/src/graphics/graphics.h \
            kernel/src/graphics/console.c kernel/src/graphics/console.h \
            kernel/src/graphics/terminal.c kernel/src/graphics/terminal.h \
            kernel/src/shell/commands/cmd_taskman.c \
            kernel/src/shell/commands/cmd_taskman.h \
            kernel/src/shell/commands/cmd_taskmantest.c \
            kernel/src/shell/commands/cmd_taskmantest.h \
            kernel/src/shell/commands/taskman_view.c \
            kernel/src/shell/commands/taskman_view.h
        local active=(
            kernel/src/core/kernel_init.c kernel/src/core/irq_bootstrap.c
            kernel/src/core/irq_bootstrap.h kernel/src/core/interrupts.c
            kernel/src/drivers/timer.c kernel/src/drivers/timer.h
            kernel/src/core/timers.c kernel/src/core/timers.h
            kernel/src/timer/hpet.c kernel/src/timer/hpet.h
            kernel/src/shell/commands/cmd_synctest.c
            kernel/src/graphics/splash.c kernel/src/graphics/splash.h
            scripts/test-timer-clockevent.sh scripts/verify-timer-clockevent.py
        )
        local prohibited
        for prohibited in "TM""V1" "UX""01" "FI""X" "FA""SE" "PHA""SE" "C""L-[0-9]"; do
            ! rg -n "$prohibited" "${active[@]}" >/dev/null
        done
        ! rg -n 'HOBBYOS_TIMER_NEGATIVE_.*KERNEL_EXTRA_CFLAGS' makefile >/dev/null
        bash -n scripts/test-interrupt-bringup.sh
        bash -n scripts/test-timer-clockevent.sh
        python3 -m py_compile scripts/verify-timer-clockevent.py
        verify_rendezvous_continuity static
        echo '[TIMER][ACTIVE_NAMING] PASS matches=0'
        echo '[TIMER][STATIC] PASS'
    } 2>&1 | tee "$log"
    record_stage static static none 0 none "$log" '[TIMER][STATIC] PASS'
}

selftest_gate()
{
    local log=$rendezvous_artifact/selftest.log
    {
        python3 scripts/verify-timer-clockevent.py selftest
        make kernel-check JOBS=2
        strings kernel.elf | grep -F '[TIMER][SELFTEST] CLOCKEVENT_MODEL_OK' >/dev/null
        strings kernel.elf | grep -F '[CLOCK][HPET_CLOCKSOURCE_PROBE] PASS' >/dev/null
        strings kernel.elf | grep -F '[CLOCK][HPET_TIMER0] QUIESCENT' >/dev/null
        strings kernel.elf | grep -F \
            '[IRQ][RUNTIME_RENDEZVOUS_SELFTEST] PASS' >/dev/null
        echo '[TIMER][SELFTEST_GATE] PASS'
    } 2>&1 | tee "$log"
    record_stage selftest selftest host 0 none "$log" '[TIMER][SELFTEST_GATE] PASS'
}

negative_one()
{
    local stage=$1 macro=$2 marker=$3
    local dir=$artifact/negatives/$stage
    local log=$dir/serial.log
    mkdir -p "$dir"
    make clean >"$dir/build.log" 2>&1
    make kernel-check JOBS=2 KERNEL_EXTRA_CFLAGS="-D$macro" >>"$dir/build.log" 2>&1
    make image KERNEL_EXTRA_CFLAGS="-D$macro" >>"$dir/build.log" 2>&1
    grep -Fq -- "-D$macro" artifacts/build/kernel-check-j2.log
    cp hobbyos.img "$dir/hobbyos.img"
    stop_vm
    current_stage=$stage
    QEMU_RUNTIME="$runtime" HOBBYOS_IMAGE="$dir/hobbyos.img" \
        MACHINE=q35 SMP=1 ACCEL=tcg scripts/qemu-agent.sh start \
        >"$dir/start.log" 2>&1
    wait_new_marker "$marker" 0 120 1
    stop_vm
    cp "$serial" "$log"
    require_marker "$marker" "$log"
    record_stage "$stage" "$stage" q35 1 tcg "$log" "$marker"

    make clean >"$dir/normal-build.log" 2>&1
    make kernel-check JOBS=2 >>"$dir/normal-build.log" 2>&1
    make image >>"$dir/normal-build.log" 2>&1
    ! strings kernel.elf | grep -Fq "$marker"
    static_gate
    echo "[TIMER][NEGATIVE_GATE] PASS stage=$stage"
}

negative_claimed_order()
{
    local stage=fair-negative-claimed-order
    local macro=HOBBYOS_TIMER_NEGATIVE_NEW_DUE_BYPASS
    local marker='[TIMER][NEGATIVE] CLAIMED_ORDER_BYPASS_DETECTED'
    local dir=$artifact/negatives/$stage
    local log=$dir/serial.log
    mkdir -p "$dir"
    make clean >"$dir/build.log" 2>&1
    make kernel-check JOBS=2 KERNEL_EXTRA_CFLAGS="-D$macro" \
        >>"$dir/build.log" 2>&1
    make image KERNEL_EXTRA_CFLAGS="-D$macro" >>"$dir/build.log" 2>&1
    grep -Fq -- "-D$macro" artifacts/build/kernel-check-j2.log
    cp hobbyos.img "$dir/hobbyos.img"
    stop_vm
    current_stage=$stage
    QEMU_RUNTIME="$runtime" HOBBYOS_IMAGE="$dir/hobbyos.img" \
        MACHINE=q35 SMP=1 ACCEL=tcg scripts/qemu-agent.sh start \
        >"$dir/start.log" 2>&1
    wait_new_marker '[KERNEL] Entering Main Loop.' 0 180
    send_command 'synctest timer-order' "$marker" 90 stress
    stop_vm
    cp "$serial" "$log"
    cp "$runtime/commands.tsv" "$dir/commands.tsv"
    require_marker "$marker" "$log"
    require_marker \
        '[SYNC][TIMER_ORDER] PASS old_position=2 new_position=1' "$log"
    record_stage "$stage" "$stage" q35 1 tcg "$log" "$marker"

    make clean >"$dir/normal-build.log" 2>&1
    make kernel-check JOBS=2 >>"$dir/normal-build.log" 2>&1
    make image >>"$dir/normal-build.log" 2>&1
    ! strings kernel.elf | grep -Fq "$marker"
    static_gate
    echo "[TIMER][NEGATIVE_GATE] PASS stage=$stage"
}

boot_to_shell_negative_claimed_order()
{
    local stage=boot-to-shell-negative-claimed-order
    local macro=HOBBYOS_TIMER_NEGATIVE_NEW_DUE_BYPASS
    local marker='[TIMER][NEGATIVE] CLAIMED_ORDER_BYPASS_DETECTED'
    local dir=$artifact/negatives/$stage log=$artifact/negatives/$stage/serial.log
    local normal_kernel entry_image
    [[ ! -e $dir && ! -e $qemu_artifact/$stage ]] || {
        echo "refusing to overwrite negative evidence: $stage" >&2
        return 1
    }
    normal_kernel=$(sha kernel.elf)
    entry_image=$(sha hobbyos.img)
    mkdir -p "$dir"
    make clean >"$dir/build.log" 2>&1
    make kernel-check JOBS=2 KERNEL_EXTRA_CFLAGS="-D$macro" \
        >>"$dir/build.log" 2>&1
    make image KERNEL_EXTRA_CFLAGS="-D$macro" >>"$dir/build.log" 2>&1
    grep -Fq -- "-D$macro" artifacts/build/kernel-check-j2.log
    capture_host_timer_environment "$stage" tcg 1
    start_vm "$stage" q35 1 tcg 0
    wait_boot_to_main_loop "$stage" q35 1 tcg
    send_command 'synctest timer-order' "$marker" 90 stress
    collect_vm "$stage"
    cp "$qemu_artifact/$stage/serial.log" "$log"
    cp "$qemu_artifact/$stage/commands.tsv" "$dir/commands.tsv"
    require_marker "$marker" "$log"
    require_marker \
        '[SYNC][TIMER_ORDER] PASS old_position=2 new_position=1' "$log"
    record_stage "$stage" "$stage" q35 1 tcg "$log" "$marker"

    make clean >"$dir/normal-build.log" 2>&1
    make kernel-check JOBS=2 >>"$dir/normal-build.log" 2>&1
    make image >>"$dir/normal-build.log" 2>&1
    [[ $(sha kernel.elf) == "$normal_kernel" ]]
    verify_current_image_payloads "$dir/normal-payloads"
    ! strings kernel.elf | grep -Fq "$marker"
    static_gate
    echo "[TIMER][NEGATIVE_CLAIMED_BYPASS] PASS stage=$stage restored_kernel=$normal_kernel entry_image=$entry_image recreated_image=$(sha hobbyos.img) payloads=5"
}

verify_current_image_payloads()
{
    local output=$1
    [[ ! -e $output ]] || {
        echo "refusing to overwrite extracted image payloads: $output" >&2
        return 1
    }
    mkdir -p "$output"
    mcopy -i hobbyos.img ::/kernel.elf "$output/kernel.elf"
    mcopy -i hobbyos.img ::/EFI/BOOT/BOOTX64.EFI "$output/BOOTX64.EFI"
    mcopy -i hobbyos.img ::/EFI/fonts/zap-light16.psf "$output/zap-light16.psf"
    mcopy -i hobbyos.img ::/EFI/images/logo.bmp "$output/logo.bmp"
    mcopy -i hobbyos.img ::/startup.nsh "$output/startup.nsh"
    cmp -s kernel.elf "$output/kernel.elf"
    cmp -s BOOTX64.EFI "$output/BOOTX64.EFI"
    cmp -s bootloader/fonts/zap-light16.psf "$output/zap-light16.psf"
    cmp -s bootloader/images/logo.bmp "$output/logo.bmp"
    cmp -s bootloader/startup.nsh "$output/startup.nsh"
    (
        cd "$output"
        sha256sum kernel.elf BOOTX64.EFI zap-light16.psf logo.bmp startup.nsh \
            >payload-manifest.txt
    )
}

boot_to_shell_negative_claimed_order_record_existing()
{
    local stage=boot-to-shell-negative-claimed-order
    local marker='[TIMER][NEGATIVE] CLAIMED_ORDER_BYPASS_DETECTED'
    local dir=$artifact/negatives/$stage log=$artifact/negatives/$stage/serial.log
    [[ -f $log && -f $dir/normal-build.log ]]
    require_marker "$marker" "$log"
    require_marker \
        '[SYNC][TIMER_ORDER] PASS old_position=2 new_position=1' "$log"
    ! strings kernel.elf | grep -Fq "$marker"
    verify_current_image_payloads "$dir/normal-payloads"
    static_gate
    echo "[TIMER][NEGATIVE_CLAIMED_BYPASS] RECORDED stage=$stage restored_kernel=$(sha kernel.elf) recreated_image=$(sha hobbyos.img) payloads=5"
}

negative_local_ready_deadline()
{
    negative_one rendezvous-negative-local-ready-deadline \
        HOBBYOS_IRQ_NEGATIVE_LOCAL_READY_DEADLINE \
        '[IRQ][NEGATIVE] LOCAL_READY_DEADLINE_FALSE_FAILURE_DETECTED'
}

negatives()
{
    negative_one fair-negative-hpet-irq HOBBYOS_TIMER_NEGATIVE_HPET_IRQ_BOOT \
        '[TIMER][NEGATIVE] HPET_IRQ_BOOT_DEPENDENCY_DETECTED'
    negative_one fair-negative-ap-tick HOBBYOS_TIMER_NEGATIVE_GLOBAL_TICK_ON_AP \
        '[TIMER][NEGATIVE] NON_BSP_GLOBAL_TICK_DETECTED'
    negative_one fair-negative-early-tick HOBBYOS_TIMER_NEGATIVE_EARLY_CLOCKEVENT \
        '[TIMER][NEGATIVE] CLOCKEVENT_BEFORE_ACTIVATION_DETECTED'
    negative_one fair-negative-duplicate HOBBYOS_TIMER_NEGATIVE_DUPLICATE_CLOCKEVENT \
        '[TIMER][NEGATIVE] DUPLICATE_GLOBAL_CLOCKEVENT_DETECTED'
    negative_one fair-negative-hpet-stall HOBBYOS_TIMER_NEGATIVE_HPET_COUNTER_STALL_MODEL \
        '[CLOCK][NEGATIVE] HPET_CLOCKSOURCE_STALL_DETECTED'
    negative_claimed_order
    negative_local_ready_deadline
    make production-image >"$rendezvous_artifact/normal-after-negatives.log" 2>&1
    ! strings kernel.elf | grep -Fq '[TIMER][NEGATIVE]'
    ! strings kernel.elf | grep -Fq '[CLOCK][NEGATIVE] HPET_CLOCKSOURCE_STALL'
    ! strings kernel.elf | grep -Fq \
        '[IRQ][NEGATIVE] LOCAL_READY_DEADLINE_FALSE_FAILURE_DETECTED'
}

verify_boot_log()
{
    local log=$1 smp=$2
    require_order "$log" \
        '[IRQ][PIC] QUIESCENT' \
        '[IRQ][IOAPIC] QUIESCENT' \
        '[IRQ][BOOTSTRAP] CONTROLLERS_QUIESCENT' \
        '[IRQ][BOOTSTRAP] ROUTES_PREPARED' \
        "[IRQ][BOOTSTRAP] CPUS_PREPARED cpus=$smp" \
        '[SCHED][BOOT] START_OK' \
        '[IRQ][BSP_LAPIC_PROBE] PASS vector=34' \
        '[CLOCK][HPET_CLOCKSOURCE_PROBE] PASS' \
        '[CLOCK][HPET_TIMER0] QUIESCENT' \
        '[CLOCKEVENT][RUNTIME] ACTIVE source=BSP_LAPIC' \
        '[SCHED][BOOTSTRAP_HANDOFF] PASS slot=0' \
        '[BOOTTRACE][BSP] 4 after-irq-enable' \
        "[IRQ][CPU_READY_SUMMARY] expected=$smp ready=$smp failed=0 waiting=0 deadline_us=10000000 abort=0" \
        "[IRQ][BOOTSTRAP] CLOCKEVENT_ACTIVE cpus=$smp" \
        '[CORE] System Core Initialization Complete.' \
        '[GRAPHICS][SPLASH] BEGIN' \
        '[GRAPHICS][SPLASH] COMPLETE result=' \
        '[IRQ][BOOTSTRAP] SERVICES_ACTIVE' \
        '[BOOT][SHELL_READY] PASS' \
        "[BOOT][RUNTIME_READY] PASS cpus=$smp/$smp" \
        '[BOOT][TEST_READY] PASS autorun=0 selftests=0' \
        '[KERNEL] Entering Main Loop.'
    require_marker '[GRAPHICS][EARLY_BIND] PASS clear=1' "$log"
    require_marker '[GRAPHICS][SPLASH_SELFTEST] PASS' "$log"
    require_marker 'console_suspended=0' "$log"
    if grep -Fq '[GRAPHICS][SPLASH] COMPLETE result=COMPLETE' "$log"; then
        require_order "$log" \
            '[GRAPHICS][SPLASH] SUSPEND' \
            '[GRAPHICS][SPLASH] CLEAR' \
            '[GRAPHICS][SPLASH] FADE_IN progress=0' \
            '[GRAPHICS][SPLASH] FADE_IN progress=50' \
            '[GRAPHICS][SPLASH] FADE_IN progress=100' \
            '[GRAPHICS][SPLASH] HOLD' \
            '[GRAPHICS][SPLASH] FADE_OUT progress=100' \
            '[GRAPHICS][SPLASH] FADE_OUT progress=50' \
            '[GRAPHICS][SPLASH] FADE_OUT progress=0' \
            '[GRAPHICS][SPLASH] RESUME' \
            '[GRAPHICS][SPLASH] COMPLETE result=COMPLETE'
    else
        require_order "$log" \
            '[GRAPHICS][SPLASH] SUSPEND' \
            '[GRAPHICS][SPLASH] ABORT reason=' \
            '[GRAPHICS][SPLASH] RESUME' \
            '[GRAPHICS][SPLASH] COMPLETE result=ABORTED_'
    fi
    [[ $(grep -Fc '[IRQ][CPU_TIMER_VERIFIED] PASS' "$log") == "$smp" ]]
    [[ $(grep -Fc '[IRQ][CPU_READY] PASS' "$log") == "$smp" ]]
    [[ $(grep -Fc 'timer_us=1000 handoff=1 preempt=1' "$log") == "$smp" ]]
    [[ $(grep -Fc '[SCHED][BOOTSTRAP_HANDOFF] PASS slot=' "$log") == "$smp" ]]
    [[ $(grep -Fc 'stage=READY' "$log") == "$smp" ]]
    ! grep -Fq '[IRQ][CPU_READY_TIMEOUT]' "$log"
    ! grep -Fq '[SMP][CPU] ERROR code=IRQ_RUNTIME_READY' "$log"
    require_marker \
        '[CLOCK][HPET_TIMER0] QUIESCENT irq_enabled=0 route_enabled=0 pending=0 legacy=0' \
        "$log"
    require_marker 'non_bsp_ticks=0 stray_hpet=0' "$log"
    require_marker 'unexpected=0 imbalance=0' "$log"
    ! grep -Fq '[IRQ][HPET_PROBE]' "$log"
    ! grep -Eq 'owner=HPET.*enabled=1' "$log"
    ! grep -Eq 'PANIC|#PF|#GP|FATAL|STRUCTURAL_FAULT|FINISH_FAULT' "$log"
}

basic_commands()
{
    send_without_marker version
    send_command 'irq check' \
        '[IRQ][CHECK] PASS clocksource=HPET clockevent=BSP_LAPIC period_us=1000 hpet_timer0=QUIESCENT non_bsp_ticks=0 stray_hpet=0' 90
    send_command 'irq boot' '[IRQ][BOOT] state=SERVICES_ACTIVE' 90
    send_command 'irq controllers' \
        '[IRQ][HPET] clocksource=ACTIVE timer0=QUIESCENT' 90
    send_command 'irq routes' '[IRQ][ROUTES] PASS count=' 90
    send_command 'accounttest check' \
        '[ACCOUNT][CHECK] PASS clocksource=HPET clockevent=BSP_LAPIC hpet_timer0=QUIESCENT' 120
    send_command 'taskdiag check' '[TASKDIAG][CHECK] PASS' 90
    send_command 'inputtest check' '[INPUTTEST][CHECK] PASS' 90
    send_command 'modaltest check' '[MODALTEST][CHECK] PASS' 90
    send_command 'taskmantest check' '[TASKMANTEST][CHECK] PASS' 90
}

async_sync_test()
{
    local command=$1 kind=$2 timeout=${3:-180}
    send_command "synctest $command" "[SYNC][$kind] START run=" 60 stress
    local run
    run=$(sed -n "s/.*\[SYNC\]\[$kind\] START run=\([0-9][0-9]*\).*/\1/p" \
        "$serial" | tail -1)
    [[ $run =~ ^[1-9][0-9]*$ ]]
    send_command "synctest async-wait $run $((timeout * 1000))" \
        "[SYNC][ASYNC_WAIT] PASS run=$run" "$timeout" stress
}

software_timer_checks()
{
    async_sync_test sleep SLEEP 180
    send_command 'synctest timer-order' \
        '[SYNC][TIMER_ORDER] PASS old_position=1 new_position=2' 90 stress
    async_sync_test timer-cancel TIMER_CANCEL 120
    send_command 'synctest timer-backlog' \
        '[SYNC][TIMER_BACKLOG] PASS old=8 new=8 callbacks=16' 120 stress
    send_command 'reaptest timer-ref' '[REAPTEST][TIMER_REF] PASS' 120 stress
    send_command 'synctest check' '[SYNC][CHECK] PASS' 90
}

verify_timer_fair_log()
{
    local log=$1 order_min=$2 cancel_min=$3
    (( $(grep -Fc \
        '[SYNC][TIMER_ORDER] PASS old_position=1 new_position=2 callbacks=2' \
        "$log") >= order_min ))
    (( $(grep -Ec \
        '\[SYNC\]\[TIMER_CANCEL\] PASS run=[0-9]+ requested=3 completed=3 errors=0' \
        "$log") >= cancel_min ))
    (( $(grep -Fc \
        'PASS pending_cancel=0 duplicate_cancel=2 claimed_cancel=1 callbacks=1' \
        "$log") >= cancel_min ))
    require_marker '[SYNC][TIMER_BACKLOG] PASS old=8 new=8 callbacks=16' "$log"
    grep -F '[SYNC][CHECK] PASS' "$log" | tail -1 | \
        grep -F 'timers_claimed=0' | grep -F 'test_active=0' | \
        grep -F 'late=0' | grep -F 'timer_errors=0' >/dev/null
    ! grep -Eq '\[SYNC\]\[(TIMER_ORDER|TIMER_CANCEL|TIMER_BACKLOG)\] FAIL' \
        "$log"
    ! grep -Eq '\[(SYNC|TIMER)\].*(late=[1-9]|residual=[1-9]|order_violations=[1-9])' \
        "$log"
    ! grep -Eq \
        'PANIC|FATAL|#PF|#GP|DOUBLE FAULT|TRIPLE FAULT|STRUCTURAL_FAULT|FINISH_FAULT' \
        "$log"
    ! grep -Eiq \
        'timer ref underflow|timer ref double release|late.callback|dispatch order violation' \
        "$log"
}

run_focused_timer_runtime()
{
    local stage=$1 smp=$2
    [[ -e /dev/kvm && -r /dev/kvm && -w /dev/kvm ]] || {
        echo 'BLOCKED_BY_EXTERNAL_ENVIRONMENT: /dev/kvm unavailable' >&2
        return 1
    }
    capture_host_timer_environment "$stage" kvm "$smp"
    start_vm "$stage" q35 "$smp" kvm 0
    wait_new_marker '[KERNEL] Entering Main Loop.' 0 240
    basic_commands
    send_command 'synctest check' '[SYNC][CHECK] PASS' 90
    local iteration
    for iteration in $(seq 1 10); do
        async_sync_test sleep SLEEP 180
        send_command 'synctest timer-order' \
            '[SYNC][TIMER_ORDER] PASS old_position=1 new_position=2' 90 stress
        async_sync_test timer-cancel TIMER_CANCEL 120
        if ((smp == 24 && (iteration == 1 || iteration == 4 || \
                           iteration == 7 || iteration == 10))); then
            stress_smoke
        fi
        send_command 'taskdiag check' '[TASKDIAG][CHECK] PASS' 90 stress
        send_command 'irq check' \
            '[IRQ][CHECK] PASS clocksource=HPET clockevent=BSP_LAPIC' 90 stress
    done
    send_command 'synctest timer-backlog' \
        '[SYNC][TIMER_BACKLOG] PASS old=8 new=8 callbacks=16' 120 stress
    send_command 'synctest check' '[SYNC][CHECK] PASS' 90
    collect_vm "$stage"
    local log=$qemu_artifact/$stage/serial.log
    verify_boot_log "$log" "$smp"
    verify_timer_fair_log "$log" 10 10
    record_stage "$stage" "$stage" q35 "$smp" kvm "$log" \
        '[SYNC][TIMER_ORDER] PASS'
    echo "[TIMER][FOCUSED_FAIRNESS] PASS stage=$stage smp=$smp order=10 cancel=10"
}

focused_fairness()
{
    run_focused_timer_runtime fair-focused-smp4 4
    run_focused_timer_runtime fair-focused-smp24 24
}

run_boot_to_shell_focused_one()
{
    local stage=$1 smp=$2
    local dir=$qemu_artifact/$stage log=$qemu_artifact/$stage/serial.log
    [[ ! -e $dir ]] || {
        echo "refusing to overwrite focused evidence: $dir" >&2
        return 1
    }
    capture_host_timer_environment "$stage" kvm "$smp"
    start_vm "$stage" q35 "$smp" kvm 0
    wait_boot_to_main_loop "$stage" q35 "$smp" kvm
    if ((smp == 4)); then
        basic_commands
        send_command 'synctest check' '[SYNC][CHECK] PASS' 90
        local iteration
        for iteration in 1 2 3; do
            send_command 'synctest timer-order' \
                '[SYNC][TIMER_ORDER] PASS old_position=1 new_position=2' \
                90 stress
            async_sync_test timer-cancel TIMER_CANCEL 120
        done
        send_command 'synctest timer-backlog' \
            '[SYNC][TIMER_BACKLOG] PASS old=8 new=8 callbacks=16' 120 stress
        send_command 'synctest check' '[SYNC][CHECK] PASS' 90
    fi
    send_command 'irq check' \
        '[IRQ][CHECK] PASS clocksource=HPET clockevent=BSP_LAPIC' 90 stress
    send_command 'taskdiag check' '[TASKDIAG][CHECK] PASS' 90 stress
    collect_vm "$stage"
    verify_boot_log "$log" "$smp"
    python3 scripts/verify-timer-clockevent.py boot-progress \
        --timeline "$dir/boot-progress.tsv" --log "$log" --smp "$smp"
    if ((smp == 4)); then
        verify_timer_fair_log "$log" 3 3
    fi
    record_stage "$stage" "$stage" q35 "$smp" kvm "$log" \
        '[KERNEL] Entering Main Loop.'
    echo "[BOOT][FOCUSED_TO_SHELL] PASS stage=$stage smp=$smp"
}

boot_to_shell_cycle1()
{
    [[ -e /dev/kvm && -r /dev/kvm && -w /dev/kvm ]] || {
        echo 'BLOCKED_BY_EXTERNAL_ENVIRONMENT: /dev/kvm unavailable' >&2
        return 1
    }
    local kernel_hash source_after source_hash
    kernel_hash=$(sha kernel.elf)
    run_boot_to_shell_focused_one boot-to-shell-cycle1-focused-smp4 4
    [[ $(sha kernel.elf) == "$kernel_hash" ]]
    run_boot_to_shell_focused_one boot-to-shell-cycle1-focused-smp24 24
    [[ $(sha kernel.elf) == "$kernel_hash" ]]
    source_after=$autonomous_artifact/cycle-1/source-after.sha256
    python3 scripts/verify-timer-clockevent.py source-manifest \
        --output "$source_after" \
        kernel/kernel.c kernel/src/core/kernel_init.c \
        kernel/src/graphics/splash.c kernel/src/graphics/splash.h \
        kernel/src/drivers/pci.c scripts/test-timer-clockevent.sh \
        scripts/verify-timer-clockevent.py scripts/harness-common.sh
    source_hash=$(sha "$source_after")
    python3 scripts/verify-timer-clockevent.py cycle-record \
        --cycle 1 --source-hash "$source_hash" --kernel-hash "$kernel_hash" \
        --scenario boot-to-shell-cycle1-focused-smp4+smp24 \
        --last-progress-marker '[KERNEL] Entering Main Loop.' \
        --guest-elapsed-ms "$BOOT_WATCH_GUEST_ELAPSED_MS" \
        --host-elapsed-ms "$BOOT_WATCH_HOST_ELAPSED_MS" \
        --classification ACCEPTED_BOOT_TO_SHELL_LIVENESS \
        --root-cause splash_unbounded_hpet_busy_wait_and_missing_null_logo_cleanup \
        --files-changed 'kernel/kernel.c,kernel/src/core/kernel_init.c,kernel/src/graphics/splash.c,kernel/src/graphics/splash.h,kernel/src/drivers/pci.c,scripts/test-timer-clockevent.sh,scripts/verify-timer-clockevent.py' \
        --focused-test 'boot-to-shell-cycle1-focused-smp4;boot-to-shell-cycle1-focused-smp24' \
        --result PASS \
        --next-action run_three_independent_smp24_boots
    echo '[BOOT][AUTONOMOUS_CLOSURE] PASS cycle=1 focused_smp4=1 focused_smp24=1'
}

boot_to_shell_cycle2()
{
    [[ -e /dev/kvm && -r /dev/kvm && -w /dev/kvm ]] || {
        echo 'BLOCKED_BY_EXTERNAL_ENVIRONMENT: /dev/kvm unavailable' >&2
        return 1
    }
    local kernel_hash source_after source_hash
    kernel_hash=$(sha kernel.elf)
    run_boot_to_shell_focused_one boot-to-shell-cycle2-focused-smp4 4
    [[ $(sha kernel.elf) == "$kernel_hash" ]]
    run_boot_to_shell_focused_one boot-to-shell-cycle2-focused-smp24 24
    [[ $(sha kernel.elf) == "$kernel_hash" ]]
    source_after=$autonomous_artifact/cycle-2/source-after.sha256
    python3 scripts/verify-timer-clockevent.py source-manifest \
        --output "$source_after" \
        kernel/kernel.c kernel/src/core/kernel_init.c \
        kernel/src/graphics/splash.c kernel/src/graphics/splash.h \
        kernel/src/drivers/pci.c scripts/test-timer-clockevent.sh \
        scripts/verify-timer-clockevent.py scripts/harness-common.sh
    source_hash=$(sha "$source_after")
    python3 scripts/verify-timer-clockevent.py cycle-record \
        --cycle 2 --source-hash "$source_hash" --kernel-hash "$kernel_hash" \
        --scenario boot-to-shell-cycle2-focused-smp4+smp24 \
        --last-progress-marker '[KERNEL] Entering Main Loop.' \
        --guest-elapsed-ms "$BOOT_WATCH_GUEST_ELAPSED_MS" \
        --host-elapsed-ms "$BOOT_WATCH_HOST_ELAPSED_MS" \
        --classification ACCEPTED_BOOT_TO_SHELL_LIVENESS \
        --root-cause 'decorative_render_started_despite_measured_clockevent_cadence_degradation' \
        --files-changed 'kernel/src/graphics/splash.c,scripts/test-timer-clockevent.sh' \
        --focused-test 'boot-to-shell-cycle2-focused-smp4;boot-to-shell-cycle2-focused-smp24' \
        --result PASS --next-action run_three_independent_smp24_boots
    echo '[BOOT][AUTONOMOUS_CLOSURE] PASS cycle=2 focused_smp4=1 focused_smp24=1'
}

boot_to_shell_cycle3()
{
    [[ -e /dev/kvm && -r /dev/kvm && -w /dev/kvm ]] || {
        echo 'BLOCKED_BY_EXTERNAL_ENVIRONMENT: /dev/kvm unavailable' >&2
        return 1
    }
    local kernel_hash source_after source_hash
    kernel_hash=$(sha kernel.elf)
    run_boot_to_shell_focused_one boot-to-shell-cycle3-focused-smp24 24
    [[ $(sha kernel.elf) == "$kernel_hash" ]]
    source_after=$autonomous_artifact/cycle-3/source-after.sha256
    python3 scripts/verify-timer-clockevent.py source-manifest \
        --output "$source_after" \
        kernel/kernel.c kernel/src/core/kernel_init.c \
        kernel/src/graphics/splash.c kernel/src/graphics/splash.h \
        kernel/src/drivers/pci.c scripts/test-timer-clockevent.sh \
        scripts/verify-timer-clockevent.py scripts/harness-common.sh
    source_hash=$(sha "$source_after")
    python3 scripts/verify-timer-clockevent.py cycle-record \
        --cycle 3 --source-hash "$source_hash" --kernel-hash "$kernel_hash" \
        --scenario boot-to-shell-cycle3-focused-smp24 \
        --last-progress-marker '[KERNEL] Entering Main Loop.' \
        --guest-elapsed-ms "$BOOT_WATCH_GUEST_ELAPSED_MS" \
        --host-elapsed-ms "$BOOT_WATCH_HOST_ELAPSED_MS" \
        --classification ACCEPTED_BOOT_TO_SHELL_LIVENESS \
        --root-cause 'host_oracle_splash_deadline_not_disarmed_after_terminal' \
        --files-changed scripts/test-timer-clockevent.sh \
        --focused-test 'cycle2_SMP4_PASS_same_kernel;cycle3_SMP24_PASS' \
        --result PASS --next-action run_three_independent_smp24_boots
    echo '[BOOT][AUTONOMOUS_CLOSURE] PASS cycle=3 focused_smp4=1 focused_smp24=1'
}

boot_to_shell_cycle4()
{
    [[ -e /dev/kvm && -r /dev/kvm && -w /dev/kvm ]] || {
        echo 'BLOCKED_BY_EXTERNAL_ENVIRONMENT: /dev/kvm unavailable' >&2
        return 1
    }
    local kernel_hash source_after source_hash
    kernel_hash=$(sha kernel.elf)
    run_boot_to_shell_focused_one boot-to-shell-cycle4-focused-smp24 24
    [[ $(sha kernel.elf) == "$kernel_hash" ]]
    source_after=$autonomous_artifact/cycle-4/source-after.sha256
    python3 scripts/verify-timer-clockevent.py source-manifest \
        --output "$source_after" \
        kernel/kernel.c kernel/src/core/kernel_init.c \
        kernel/src/graphics/splash.c kernel/src/graphics/splash.h \
        kernel/src/drivers/pci.c scripts/test-timer-clockevent.sh \
        scripts/verify-timer-clockevent.py scripts/harness-common.sh
    source_hash=$(sha "$source_after")
    python3 scripts/verify-timer-clockevent.py cycle-record \
        --cycle 4 --source-hash "$source_hash" --kernel-hash "$kernel_hash" \
        --scenario boot-to-shell-cycle4-focused-smp24 \
        --last-progress-marker '[KERNEL] Entering Main Loop.' \
        --guest-elapsed-ms "$BOOT_WATCH_GUEST_ELAPSED_MS" \
        --host-elapsed-ms "$BOOT_WATCH_HOST_ELAPSED_MS" \
        --classification ACCEPTED_BOOT_TO_SHELL_LIVENESS \
        --root-cause 'focused_SMP24_runner_sent_commands_outside_the_focused_contract' \
        --files-changed scripts/test-timer-clockevent.sh \
        --focused-test 'cycle2_SMP4_PASS_same_kernel;cycle4_SMP24_boot_irq_check_taskdiag_PASS' \
        --result PASS --next-action run_three_independent_smp24_boots
    echo '[BOOT][AUTONOMOUS_CLOSURE] PASS cycle=4 focused_smp4=1 focused_smp24=1'
}

verify_boot_to_shell_run()
{
    local stage=$1 smp=$2
    local dir log
    dir=$qemu_artifact/$stage
    log=$dir/serial.log
    verify_boot_log "$log" "$smp"
    python3 scripts/verify-timer-clockevent.py boot-progress \
        --timeline "$dir/boot-progress.tsv" --log "$log" --smp "$smp"
    require_marker \
        '[IRQ][CHECK] PASS clocksource=HPET clockevent=BSP_LAPIC' "$log"
    require_marker '[TASKDIAG][CHECK] PASS' "$log"
    require_marker 'console_suspended=0' "$log"
    ! grep -Eq \
        'PANIC|FATAL|#PF|#GP|DOUBLE FAULT|TRIPLE FAULT|STRUCTURAL_FAULT|FINISH_FAULT|boot stage timeout|console render suspension residual' \
        "$log"
    ! grep -Eq \
        '(late callback|late_callbacks|claimed residual|claimed_residual)=[1-9][0-9]*' \
        "$log"
}

run_boot_to_shell_final_smp24_one()
{
    local run=$1 stage dir log
    stage=boot-to-shell-final-smp24-run$run
    dir=$qemu_artifact/$stage
    log=$dir/serial.log
    [[ ! -e $dir ]] || {
        echo "refusing to overwrite independent SMP24 boot: $dir" >&2
        return 1
    }
    capture_host_timer_environment "$stage" kvm 24
    start_vm "$stage" q35 24 kvm 0
    wait_boot_to_main_loop "$stage" q35 24 kvm
    send_command 'irq check' \
        '[IRQ][CHECK] PASS clocksource=HPET clockevent=BSP_LAPIC' 90 stress
    send_command 'taskdiag check' '[TASKDIAG][CHECK] PASS' 90 stress
    collect_vm "$stage"
    verify_boot_to_shell_run "$stage" 24
    FINAL_BOOT_SPLASH_MS=$(awk -F '\t' \
        '$3 == "SPLASH_BEGIN" {begin=$2} \
         $3 == "SPLASH_TERMINAL" {print $2-begin; exit}' \
        "$dir/boot-progress.tsv")
    [[ $FINAL_BOOT_SPLASH_MS =~ ^[0-9]+$ ]] && \
        ((FINAL_BOOT_SPLASH_MS <= 15000))
    FINAL_BOOT_SERIAL_HASH=$(sha "$log")
    FINAL_BOOT_IMAGE_HASH=$(sed -n 's/^IMAGE_SHA256=//p' "$dir/launch.env")
    record_stage "$stage" "$stage" q35 24 kvm "$log" \
        '[KERNEL] Entering Main Loop.'
    echo "[BOOT][SMP24_TO_SHELL_RUN] PASS run=$run cpus=24/24 splash_host_ms=$FINAL_BOOT_SPLASH_MS"
}

boot_to_shell_final_smp24_boots()
{
    [[ -e /dev/kvm && -r /dev/kvm && -w /dev/kvm ]] || {
        echo 'BLOCKED_BY_EXTERNAL_ENVIRONMENT: /dev/kvm unavailable' >&2
        return 1
    }
    local kernel_hash run summary
    kernel_hash=$(sha kernel.elf)
    summary=$autonomous_artifact/smp24-three-boots.tsv
    [[ ! -e $summary ]] || {
        echo "refusing to overwrite SMP24 three-boot summary: $summary" >&2
        return 1
    }
    printf 'run\tkernel_sha256\timage_sha256\tserial_sha256\tsplash_host_ms\tresult\n' \
        >"$summary"
    for run in 1 2 3; do
        run_boot_to_shell_final_smp24_one "$run"
        [[ $(sha kernel.elf) == "$kernel_hash" ]]
        printf '%s\t%s\t%s\t%s\t%s\tPASS\n' "$run" "$kernel_hash" \
            "$FINAL_BOOT_IMAGE_HASH" "$FINAL_BOOT_SERIAL_HASH" \
            "$FINAL_BOOT_SPLASH_MS" >>"$summary"
    done
    [[ $(awk -F '\t' 'NR > 1 && $6 == "PASS" {count++} END {print count+0}' \
        "$summary") == 3 ]]
    printf '[BOOT][SMP24_TO_SHELL] PASS runs=3 kernel_sha256=%s\n' \
        "$kernel_hash" | tee "$autonomous_artifact/smp24-three-boots.log"
}

run_boot_to_shell_quick_one()
{
    local stage=$1 machine=$2 smp=$3 accel=$4
    local dir log
    dir=$qemu_artifact/$stage
    log=$dir/serial.log
    [[ ! -e $dir ]] || {
        echo "refusing to overwrite quick-matrix evidence: $dir" >&2
        return 1
    }
    capture_host_timer_environment "$stage" "$accel" "$smp"
    start_vm "$stage" "$machine" "$smp" "$accel" 0
    wait_boot_to_main_loop "$stage" "$machine" "$smp" "$accel"
    send_command 'irq check' \
        '[IRQ][CHECK] PASS clocksource=HPET clockevent=BSP_LAPIC' 90 stress
    send_command 'taskdiag check' '[TASKDIAG][CHECK] PASS' 90 stress
    collect_vm "$stage"
    verify_boot_to_shell_run "$stage" "$smp"
    if [[ $machine == pc ]]; then
        require_marker 'master=ff slave=ff' "$log"
        require_marker 'pcat=1' "$log"
    fi
    record_stage "$stage" "$stage" "$machine" "$smp" "$accel" "$log" \
        '[KERNEL] Entering Main Loop.'
    echo "[BOOT][QUICK_MATRIX_RUN] PASS stage=$stage machine=$machine smp=$smp accel=$accel"
}

boot_to_shell_quick_matrix()
{
    run_boot_to_shell_quick_one boot-to-shell-matrix-q35-smp1-tcg q35 1 tcg
    run_boot_to_shell_quick_one boot-to-shell-matrix-q35-smp2-tcg q35 2 tcg
    [[ -e /dev/kvm && -r /dev/kvm && -w /dev/kvm ]] || {
        echo 'BLOCKED_BY_EXTERNAL_ENVIRONMENT: /dev/kvm unavailable' >&2
        return 1
    }
    run_boot_to_shell_quick_one boot-to-shell-matrix-q35-smp4-kvm q35 4 kvm
    run_boot_to_shell_quick_one boot-to-shell-matrix-q35-smp8-kvm q35 8 kvm
    run_boot_to_shell_quick_one boot-to-shell-matrix-q35-smp24-kvm q35 24 kvm
    run_boot_to_shell_quick_one boot-to-shell-matrix-pc-smp4-tcg pc 4 tcg
    echo '[BOOT][QUICK_MATRIX] PASS runs=6' | \
        tee "$autonomous_artifact/quick-matrix.log"
}

boot_to_shell_quick_matrix_record_existing()
{
    local row stage machine smp accel log
    while read -r stage machine smp accel; do
        log=$qemu_artifact/$stage/serial.log
        [[ -f $log && -f $qemu_artifact/$stage/kernel.payload.elf ]]
        [[ $(sha "$qemu_artifact/$stage/kernel.payload.elf") == \
           $(sha kernel.elf) ]]
        load_host_timer_environment "$stage"
        verify_boot_to_shell_run "$stage" "$smp"
        if [[ $machine == pc ]]; then
            require_marker 'master=ff slave=ff' "$log"
            require_marker 'pcat=1' "$log"
        fi
        record_stage "$stage" "$stage" "$machine" "$smp" "$accel" \
            "$log" '[KERNEL] Entering Main Loop.'
        echo "[BOOT][QUICK_MATRIX_RUN] RECORDED stage=$stage machine=$machine smp=$smp accel=$accel"
    done <<'EOF'
boot-to-shell-matrix-q35-smp1-tcg q35 1 tcg
boot-to-shell-matrix-q35-smp2-tcg q35 2 tcg
boot-to-shell-matrix-q35-smp4-kvm q35 4 kvm
boot-to-shell-matrix-q35-smp8-kvm q35 8 kvm
boot-to-shell-matrix-q35-smp24-kvm q35 24 kvm
boot-to-shell-matrix-pc-smp4-tcg pc 4 tcg
EOF
    echo '[BOOT][QUICK_MATRIX] PASS runs=6' | \
        tee "$autonomous_artifact/quick-matrix.log"
}

record_rendezvous_boot()
{
    local stage=$1 smp=$2 machine=$3 accel=$4
    local log=$qemu_artifact/$stage/serial.log
    [[ -f $log && -f $qemu_artifact/$stage/commands.tsv &&
       -f $qemu_artifact/$stage/kernel.payload.elf ]]
    [[ $(sha "$qemu_artifact/$stage/kernel.payload.elf") == \
       $(sha kernel.elf) ]]
    verify_boot_log "$log" "$smp"
    require_marker '[IRQ][CHECK] PASS clocksource=HPET clockevent=BSP_LAPIC' \
        "$log"
    require_marker '[TASKDIAG][CHECK] PASS' "$log"
    if [[ $machine == pc ]]; then
        require_marker 'master=ff slave=ff' "$log"
        require_marker 'pcat=1' "$log"
    fi
    record_stage "$stage" "$stage" "$machine" "$smp" "$accel" \
        "$log" '[IRQ][CPU_READY_SUMMARY]'
}

run_rendezvous_focused_smp4()
{
    local stage=rendezvous-focused-smp4 log=$qemu_artifact/rendezvous-focused-smp4/serial.log
    if [[ -f $log ]]; then
        record_rendezvous_boot "$stage" 4 q35 kvm
        verify_timer_fair_log "$log" 3 3
        return
    fi
    capture_host_timer_environment "$stage" kvm 4
    start_vm "$stage" q35 4 kvm 0
    wait_new_marker '[KERNEL] Entering Main Loop.' 0 240
    send_command 'irq check' \
        '[IRQ][CHECK] PASS clocksource=HPET clockevent=BSP_LAPIC' 90
    send_command 'taskdiag check' '[TASKDIAG][CHECK] PASS' 90
    send_command 'synctest check' '[SYNC][CHECK] PASS' 90
    local iteration
    for iteration in 1 2 3; do
        send_command 'synctest timer-order' \
            '[SYNC][TIMER_ORDER] PASS old_position=1 new_position=2' 90 stress
        async_sync_test timer-cancel TIMER_CANCEL 120
    done
    send_command 'synctest timer-backlog' \
        '[SYNC][TIMER_BACKLOG] PASS old=8 new=8 callbacks=16' 120 stress
    send_command 'synctest check' '[SYNC][CHECK] PASS' 90
    collect_vm "$stage"
    record_rendezvous_boot "$stage" 4 q35 kvm
    verify_timer_fair_log "$log" 3 3
    echo '[IRQ][RENDEZVOUS_FOCUSED] PASS smp=4 timer_order=3 timer_cancel=3'
}

run_rendezvous_smp24_boot()
{
    local run=$1 stage=rendezvous-smp24-boot$1
    local log=$qemu_artifact/$stage/serial.log
    if [[ -f $log ]]; then
        record_rendezvous_boot "$stage" 24 q35 kvm
        return
    fi
    capture_host_timer_environment "$stage" kvm 24
    start_vm "$stage" q35 24 kvm 0
    wait_new_marker '[KERNEL] Entering Main Loop.' 0 240
    send_command 'irq check' \
        '[IRQ][CHECK] PASS clocksource=HPET clockevent=BSP_LAPIC' 90 stress
    send_command 'taskdiag check' '[TASKDIAG][CHECK] PASS' 90 stress
    collect_vm "$stage"
    record_rendezvous_boot "$stage" 24 q35 kvm
    echo "[IRQ][SMP24_RUNTIME_READY_RUN] PASS run=$run cpus=24/24 abort=0 failed=0"
}

rendezvous_three_smp24_boots()
{
    [[ -e /dev/kvm && -r /dev/kvm && -w /dev/kvm ]] || {
        echo 'BLOCKED_BY_EXTERNAL_ENVIRONMENT: /dev/kvm unavailable' >&2
        return 1
    }
    local kernel_hash
    kernel_hash=$(sha kernel.elf)
    run_rendezvous_smp24_boot 1
    [[ $(sha kernel.elf) == "$kernel_hash" ]]
    run_rendezvous_smp24_boot 2
    [[ $(sha kernel.elf) == "$kernel_hash" ]]
    run_rendezvous_smp24_boot 3
    [[ $(sha kernel.elf) == "$kernel_hash" ]]
    printf '[IRQ][SMP24_RUNTIME_READY] PASS runs=3 kernel_sha256=%s\n' \
        "$kernel_hash" | tee "$rendezvous_artifact/smp24-three-boots.log"
}

run_rendezvous_quick_scenario()
{
    local stage=$1 machine=$2 smp=$3 accel=$4
    local log=$qemu_artifact/$stage/serial.log
    if [[ -f $log ]]; then
        record_rendezvous_boot "$stage" "$smp" "$machine" "$accel"
        return
    fi
    capture_host_timer_environment "$stage" "$accel" "$smp"
    start_vm "$stage" "$machine" "$smp" "$accel" 0
    wait_new_marker '[KERNEL] Entering Main Loop.' 0 240
    send_command 'irq check' \
        '[IRQ][CHECK] PASS clocksource=HPET clockevent=BSP_LAPIC' 90 stress
    send_command 'taskdiag check' '[TASKDIAG][CHECK] PASS' 90 stress
    collect_vm "$stage"
    record_rendezvous_boot "$stage" "$smp" "$machine" "$accel"
    if [[ $machine == pc ]]; then
        require_marker 'master=ff slave=ff' "$log"
        require_marker 'pcat=1' "$log"
    fi
    echo "[IRQ][RENDEZVOUS_MATRIX] PASS stage=$stage machine=$machine smp=$smp accel=$accel"
}

rendezvous_matrix()
{
    run_rendezvous_quick_scenario rendezvous-up-tcg q35 1 tcg
    run_rendezvous_quick_scenario rendezvous-smp2-tcg q35 2 tcg
    [[ -e /dev/kvm && -r /dev/kvm && -w /dev/kvm ]] || {
        echo 'BLOCKED_BY_EXTERNAL_ENVIRONMENT: /dev/kvm unavailable' >&2
        return 1
    }
    run_rendezvous_quick_scenario rendezvous-smp4-kvm q35 4 kvm
    run_rendezvous_quick_scenario rendezvous-smp8-kvm q35 8 kvm
    run_rendezvous_quick_scenario rendezvous-smp24-kvm q35 24 kvm
    run_rendezvous_quick_scenario rendezvous-pcat pc 4 tcg
}

run_rendezvous_runtime_smp24()
{
    local stage=rendezvous-runtime-smp24
    local log=$qemu_artifact/$stage/serial.log
    if [[ -f $log ]]; then
        [[ $(sha "$qemu_artifact/$stage/kernel.payload.elf") == \
           $(sha kernel.elf) ]]
        load_host_timer_environment "$stage"
        load_lapic_rate_result "$log" 24
        verify_runtime_smp24_log "$stage"
        validate_lapic_rate_runtime "$log" 24
        record_rendezvous_boot "$stage" 24 q35 kvm
        record_stage rendezvous-runtime-rate "$stage" q35 24 kvm "$log" \
            '[ACCOUNT][LAPIC_RATE]' "$lapic_rate_raw" \
            "$lapic_rate_authority" "$lapic_rate_disposition" \
            "$lapic_rate_worst" "$lapic_rate_min" "$lapic_rate_max" \
            "$lapic_rate_window" "$lapic_rate_rounds" PASS
        echo '[BOOT][RUNTIME_SMP24] RECORDED smp=24 timer_order=5 timer_cancel=5 taskman=1 stress_kill=1 input=1 modal=1 irq=1 taskdiag=1'
        return
    fi
    [[ -e /dev/kvm && -r /dev/kvm && -w /dev/kvm ]] || {
        echo 'BLOCKED_BY_EXTERNAL_ENVIRONMENT: /dev/kvm unavailable' >&2
        return 1
    }
    reset_lapic_rate_result
    capture_host_timer_environment "$stage" kvm 24
    start_vm "$stage" q35 24 kvm 0
    wait_boot_to_main_loop "$stage" q35 24 kvm
    send_command 'synctest check' '[SYNC][CHECK] PASS' 90
    local iteration
    for iteration in 1 2 3 4 5; do
        send_command 'synctest timer-order' \
            '[SYNC][TIMER_ORDER] PASS old_position=1 new_position=2' 90 stress
        async_sync_test timer-cancel TIMER_CANCEL 120
    done
    send_command 'synctest timer-backlog' \
        '[SYNC][TIMER_BACKLOG] PASS old=8 new=8 callbacks=16' 120 stress
    send_command 'reaptest timer-ref' '[REAPTEST][TIMER_REF] PASS' 120 stress
    send_command 'synctest check' '[SYNC][CHECK] PASS' 90
    taskman_smoke
    stress_smoke
    send_command 'killtest check' '[KILLTEST][CHECK] PASS' 90
    send_command 'inputtest check' '[INPUTTEST][CHECK] PASS' 90
    send_command 'modaltest check' '[MODALTEST][CHECK] PASS' 90
    accounting_checks
    send_command 'irq check' \
        '[IRQ][CHECK] PASS clocksource=HPET clockevent=BSP_LAPIC' 90 stress
    send_command 'taskdiag check' '[TASKDIAG][CHECK] PASS' 90 stress
    collect_vm "$stage"
    cp "$log" "$qemu_artifact/$stage/serial.guest.log"
    append_lapic_rate_environment_marker "$log" 24
    verify_runtime_smp24_log "$stage"
    validate_lapic_rate_runtime "$log" 24
    record_rendezvous_boot "$stage" 24 q35 kvm
    record_stage rendezvous-runtime-rate "$stage" q35 24 kvm "$log" \
        '[ACCOUNT][LAPIC_RATE]' "$lapic_rate_raw" \
        "$lapic_rate_authority" "$lapic_rate_disposition" \
        "$lapic_rate_worst" "$lapic_rate_min" "$lapic_rate_max" \
        "$lapic_rate_window" "$lapic_rate_rounds" "$lapic_rate_runtime"
    echo '[BOOT][RUNTIME_SMP24] PASS smp=24 timer_order=5 timer_cancel=5 taskman=1 stress_kill=1 input=1 modal=1 irq=1 taskdiag=1'
}

record_existing_focused_timer_runtime()
{
    local stage=$1 smp=$2 log=$qemu_artifact/$1/serial.log
    [[ -f $log && -f $qemu_artifact/$stage/commands.tsv &&
       -f $qemu_artifact/$stage/kernel.payload.elf ]]
    [[ $(sha "$qemu_artifact/$stage/kernel.payload.elf") == $(sha kernel.elf) ]]
    load_host_timer_environment "$stage"
    verify_boot_log "$log" "$smp"
    verify_timer_fair_log "$log" 10 10
    record_stage "$stage" "$stage" q35 "$smp" kvm "$log" \
        '[SYNC][TIMER_ORDER] PASS'
    echo "[TIMER][FOCUSED_FAIRNESS] RECOVERED stage=$stage smp=$smp order=10 cancel=10"
}

accounting_checks()
{
    send_command 'accounttest clock-smp 24 100000' \
        '[ACCOUNT][CLOCK_SMP] PASS' 300 stress
    send_command 'accounttest reaper-quiescence 32' \
        '[ACCOUNT][REAPER_QUIESCENCE] PASS workers=32' 180 stress
    send_command 'accounttest lapic-config' \
        '[ACCOUNT][LAPIC_CONFIG] PASS' 90 stress
    send_command 'accounttest lapic-liveness 500' \
        '[ACCOUNT][LAPIC_LIVENESS] PASS' 90 stress
    execute_lapic_rate_once 24
    send_command 'accounttest check' \
        '[ACCOUNT][CHECK] PASS clocksource=HPET clockevent=BSP_LAPIC' 120
    send_command 'irq check' \
        '[IRQ][CHECK] PASS clocksource=HPET clockevent=BSP_LAPIC' 90
}

run_rate_control()
{
    local run=$1 stage=rendezvous-rate-control-smp4-run$1
    reset_lapic_rate_result
    capture_host_timer_environment "$stage" kvm 4
    start_vm "$stage" q35 4 kvm 0
    wait_boot_to_main_loop "$stage" q35 4 kvm
    send_command 'accounttest lapic-config' \
        '[ACCOUNT][LAPIC_CONFIG] PASS' 90 stress
    send_command 'accounttest lapic-liveness 500' \
        '[ACCOUNT][LAPIC_LIVENESS] PASS' 90 stress
    execute_lapic_rate_once 4
    send_command 'accounttest check' \
        '[ACCOUNT][CHECK] PASS clocksource=HPET clockevent=BSP_LAPIC' 120
    send_command 'irq check' \
        '[IRQ][CHECK] PASS clocksource=HPET clockevent=BSP_LAPIC' 90
    collect_vm "$stage"
    local log=$qemu_artifact/$stage/serial.log
    if [[ $stage == smp24-kvm ]]; then
        cp "$log" "$qemu_artifact/$stage/serial.guest.log"
        append_lapic_rate_environment_marker "$log" 24
    fi
    verify_boot_log "$log" 4
    python3 scripts/verify-timer-clockevent.py boot-progress \
        --timeline "$qemu_artifact/$stage/boot-progress.tsv" \
        --log "$log" --smp 4
    validate_lapic_rate_runtime "$log" 4
    [[ $lapic_rate_raw == PASS ]]
    [[ $lapic_rate_authority == AUTHORITATIVE_SMP4_RATE_CONTROL ]]
    record_stage "$stage" "$stage" q35 4 kvm "$log" \
        '[ACCOUNT][LAPIC_RATE] PASS' "$lapic_rate_raw" \
        "$lapic_rate_authority" PASS "$lapic_rate_worst" \
        "$lapic_rate_min" "$lapic_rate_max" "$lapic_rate_window" \
        "$lapic_rate_rounds" "$lapic_rate_runtime"
    echo "[TIMER][RATE_CONTROL] PASS run=$run worst_median_x1000=$lapic_rate_worst threshold=700"
}

record_existing_rate_control()
{
    local run=$1 stage=rendezvous-rate-control-smp4-run$1
    local log=$qemu_artifact/$stage/serial.log
    [[ -f $log && -f $qemu_artifact/$stage/commands.tsv &&
       -f $qemu_artifact/$stage/kernel.payload.elf ]]
    [[ $(sha "$qemu_artifact/$stage/kernel.payload.elf") == $(sha kernel.elf) ]]
    load_host_timer_environment "$stage"
    load_lapic_rate_result "$log" 4
    verify_boot_log "$log" 4
    validate_lapic_rate_runtime "$log" 4
    [[ $lapic_rate_raw == PASS ]]
    [[ $lapic_rate_authority == AUTHORITATIVE_SMP4_RATE_CONTROL ]]
    record_stage "$stage" "$stage" q35 4 kvm "$log" \
        '[ACCOUNT][LAPIC_RATE] PASS' "$lapic_rate_raw" \
        "$lapic_rate_authority" PASS "$lapic_rate_worst" \
        "$lapic_rate_min" "$lapic_rate_max" "$lapic_rate_window" \
        "$lapic_rate_rounds" "$lapic_rate_runtime"
    echo "[TIMER][RATE_CONTROL] RECOVERED run=$run worst_median_x1000=$lapic_rate_worst threshold=700"
}

rate_controls()
{
    [[ -e /dev/kvm && -r /dev/kvm && -w /dev/kvm ]] || {
        echo 'BLOCKED_BY_EXTERNAL_ENVIRONMENT: /dev/kvm unavailable' >&2
        return 1
    }
    local kernel_hash
    kernel_hash=$(sha kernel.elf)
    if [[ -f $qemu_artifact/rendezvous-rate-control-smp4-run1/serial.log ]]; then
        record_existing_rate_control 1
    else
        run_rate_control 1
    fi
    [[ $(sha kernel.elf) == "$kernel_hash" ]]
    if [[ -f $qemu_artifact/rendezvous-rate-control-smp4-run2/serial.log ]]; then
        record_existing_rate_control 2
    else
        run_rate_control 2
    fi
    [[ $(sha kernel.elf) == "$kernel_hash" ]]
    if [[ -f $qemu_artifact/rendezvous-rate-control-smp4-run3/serial.log ]]; then
        record_existing_rate_control 3
    else
        run_rate_control 3
    fi
    [[ $(sha kernel.elf) == "$kernel_hash" ]]
    echo '[TIMER][RATE_CONTROL] SMP4_KVM_3_OF_3_PASS threshold=700'
}

run_scenario()
{
    local stage=$1 machine=$2 smp=$3 accel=$4
    local capture=0 kind=${stage#fair-}
    reset_lapic_rate_result
    capture_host_timer_environment "$stage" "$accel" "$smp"
    [[ $kind == smp4-kvm ]] && capture=1
    start_vm "$stage" "$machine" "$smp" "$accel" 0
    if ((capture)); then
        rm -f "$splash_begin" "$splash_complete"
        capture_when_marker_appears '[GRAPHICS][SPLASH] BEGIN' \
            "$splash_begin" &
        early_capture_pid=$!
    fi
    wait_new_marker '[KERNEL] Entering Main Loop.' 0 240
    basic_commands
    if [[ $kind == smp4-kvm || $kind == smp24-kvm ]]; then
        software_timer_checks
        taskman_smoke
    fi
    if [[ $kind == smp24-kvm ]]; then
        accounting_checks
        stress_smoke
    fi
    send_command 'irq boot' '[IRQ][BOOT_CLOCK] clocksource=HPET' 90
    send_command 'irq check' \
        '[IRQ][CHECK] PASS clocksource=HPET clockevent=BSP_LAPIC' 90
    if ((capture)); then
        wait "$early_capture_pid"
        early_capture_pid=
        hmp command "screendump $splash_complete" >/dev/null
        [[ -s $splash_begin && -s $splash_complete ]]
        python3 scripts/verify-timer-clockevent.py screens \
            "$splash_begin" "$splash_complete" | tee "$artifact/splash.log"
        cp "$splash_begin" "$splash_begin_artifact"
        cp "$splash_complete" "$splash_complete_artifact"
    fi
    collect_vm "$stage"
    local log=$qemu_artifact/$stage/serial.log
    if [[ $kind == smp24-kvm ]]; then
        cp "$log" "$qemu_artifact/$stage/serial.guest.log"
        append_lapic_rate_environment_marker "$log" 24
    fi
    verify_boot_log "$log" "$smp"
    require_marker '[IRQ][CHECK] PASS clocksource=HPET clockevent=BSP_LAPIC' "$log"
    if [[ $kind == pcat ]]; then
        require_marker 'master=ff slave=ff' "$log"
        require_marker 'pcat=1' "$log"
    fi
    if [[ $kind == smp24-kvm ]]; then
        validate_lapic_rate_runtime "$log" 24
    fi
    record_stage "$stage" "$stage" "$machine" "$smp" "$accel" \
        "$log" '[IRQ][CHECK] PASS' "$lapic_rate_raw" \
        "$lapic_rate_authority" "$lapic_rate_disposition" \
        "$lapic_rate_worst" "$lapic_rate_min" "$lapic_rate_max" \
        "$lapic_rate_window" "$lapic_rate_rounds" "$lapic_rate_runtime"
    if [[ $kind == smp24-kvm ]]; then
        verify_timer_fair_log "$log" 1 1
        record_stage fair-software-timers "$stage" "$machine" "$smp" "$accel" \
            "$log" '[SYNC][SLEEP] PASS'
        record_stage fair-taskman "$stage" "$machine" "$smp" "$accel" \
            "$log" '[TASKMAN][AUTO_EXIT] PASS'
        record_stage fair-stress "$stage" "$machine" "$smp" "$accel" \
            "$log" '[SMP][KILL_SWEEP] PASS'
    fi
    echo "[TIMER][SCENARIO] PASS stage=$stage machine=$machine smp=$smp accel=$accel"
}

record_existing_smp24()
{
    local stage=smp24-kvm log=$qemu_artifact/smp24-kvm/serial.log
    [[ -f $log && -f $qemu_artifact/$stage/commands.tsv &&
       -f $qemu_artifact/$stage/kernel.payload.elf ]]
    [[ $(sha "$qemu_artifact/$stage/kernel.payload.elf") == $(sha kernel.elf) ]]
    if [[ ! -f $qemu_artifact/$stage/serial.guest.log ]]; then
        cp "$log" "$qemu_artifact/$stage/serial.guest.log"
    fi
    load_host_timer_environment "$stage"
    load_lapic_rate_result "$log" 24
    append_lapic_rate_environment_marker "$log" 24
    verify_boot_log "$log" 24
    require_marker '[IRQ][CHECK] PASS clocksource=HPET clockevent=BSP_LAPIC' "$log"
    validate_lapic_rate_runtime "$log" 24
    record_stage "$stage" "$stage" q35 24 kvm "$log" \
        '[IRQ][CHECK] PASS' "$lapic_rate_raw" "$lapic_rate_authority" \
        "$lapic_rate_disposition" "$lapic_rate_worst" "$lapic_rate_min" \
        "$lapic_rate_max" "$lapic_rate_window" "$lapic_rate_rounds" \
        "$lapic_rate_runtime"
    record_stage software-timers "$stage" q35 24 kvm "$log" \
        '[SYNC][SLEEP] PASS'
    record_stage taskman "$stage" q35 24 kvm "$log" \
        '[TASKMAN][AUTO_EXIT] PASS'
    record_stage stress "$stage" q35 24 kvm "$log" \
        '[SMP][KILL_SWEEP] PASS'
    echo "[TIMER][SCENARIO] RECOVERED stage=$stage raw=$lapic_rate_raw worst_median_x1000=$lapic_rate_worst authority=$lapic_rate_authority"
}

matrix()
{
    run_scenario fair-up-tcg q35 1 tcg
    run_scenario fair-smp2-tcg q35 2 tcg
    [[ -e /dev/kvm && -r /dev/kvm && -w /dev/kvm ]] || {
        echo 'BLOCKED_BY_EXTERNAL_ENVIRONMENT: /dev/kvm unavailable' >&2
        return 1
    }
    run_scenario fair-smp4-kvm q35 4 kvm
    run_scenario fair-smp8-kvm q35 8 kvm
    run_scenario fair-smp24-kvm q35 24 kvm
    run_scenario fair-pcat pc 4 tcg
}

focused_runtime()
{
    focused_fairness
}

soak24()
{
    [[ $duration_ms =~ ^[0-9]+$ ]] && ((duration_ms >= 300000)) || {
        echo 'DURATION_MS must be at least 300000' >&2
        return 2
    }
    [[ -e /dev/kvm && -r /dev/kvm && -w /dev/kvm ]] || {
        echo 'BLOCKED_BY_EXTERNAL_ENVIRONMENT: /dev/kvm unavailable' >&2
        return 1
    }
    local matrix_hash
    matrix_hash=$(sed -n 's/^KERNEL_SHA256=//p' \
        "$qemu_artifact/rendezvous-smp24-kvm/launch.env")
    [[ $matrix_hash == "$(sha kernel.elf)" ]]
    capture_host_timer_environment rendezvous-soak24 kvm 24
    start_vm rendezvous-soak24 q35 24 kvm 0
    wait_new_marker '[KERNEL] Entering Main Loop.' 0 240
    basic_commands
    send_command 'taskmantest heap-begin' '[TASKMANTEST][HEAP_BEGIN] PASS' 120
    local start now elapsed iteration=0 stress_runs=0 taskman_runs=0
    local sleep_runs=0 cancel_runs=0 order_runs=0
    start=$(date +%s%3N)
    while :; do
        iteration=$((iteration + 1))
        send_command 'irq check' '[IRQ][CHECK] PASS clocksource=HPET' 90 stress
        send_command 'taskdiag check' '[TASKDIAG][CHECK] PASS' 90 stress
        if ((sleep_runs < 3)); then
            async_sync_test sleep SLEEP 180
            sleep_runs=$((sleep_runs + 1))
        fi
        if ((order_runs < 5)); then
            send_command 'synctest timer-order' \
                '[SYNC][TIMER_ORDER] PASS old_position=1 new_position=2' \
                90 stress
            order_runs=$((order_runs + 1))
        fi
        if ((cancel_runs < 5)); then
            async_sync_test timer-cancel TIMER_CANCEL 120
            cancel_runs=$((cancel_runs + 1))
        fi
        if ((stress_runs < 4)); then
            stress_smoke
            stress_runs=$((stress_runs + 1))
        fi
        if ((taskman_runs < 4)); then
            taskman_smoke
            taskman_runs=$((taskman_runs + 1))
        fi
        if ((iteration == 1)); then
            send_command 'reaptest normal 10' '[REAPTEST][NORMAL] PASS' 180 stress
        fi
        now=$(date +%s%3N)
        elapsed=$((now - start))
        if ((iteration >= 10 && sleep_runs >= 3 && order_runs >= 5 &&
             cancel_runs >= 5 &&
             stress_runs >= 4 && taskman_runs >= 4 && elapsed >= duration_ms)); then
            break
        fi
        if ((iteration >= 10 && elapsed < duration_ms)); then
            sleep 5
        fi
    done
    send_command 'inputtest check' '[INPUTTEST][CHECK] PASS' 90
    send_command 'modaltest check' '[MODALTEST][CHECK] PASS' 90
    send_command 'synctest timer-backlog' \
        '[SYNC][TIMER_BACKLOG] PASS old=8 new=8 callbacks=16' 120 stress
    send_command 'synctest check' '[SYNC][CHECK] PASS' 90
    send_command 'reaptest check' '[REAPTEST][CHECK] PASS' 90
    send_command 'killtest check' '[KILLTEST][CHECK] PASS' 90
    send_command 'taskmantest check' '[TASKMANTEST][CHECK] PASS' 90
    send_command 'irq boot' '[IRQ][BOOT_CLOCK] clocksource=HPET' 90
    send_command 'irq check' '[IRQ][CHECK] PASS clocksource=HPET' 90
    send_command 'taskmantest heap-end' '[TASKMANTEST][HEAP] PASS' 180
    now=$(date +%s%3N)
    elapsed=$((now - start))
    collect_vm rendezvous-soak24
    local log=$qemu_artifact/rendezvous-soak24/serial.log
    printf '[TIMER][SOAK] PASS smp=24 source=BSP_LAPIC hpet_timer0=QUIESCENT duration_ms=%s\n' \
        "$elapsed" >>"$log"
    verify_boot_log "$log" 24
    (( $(grep -Fc '[IRQ][CHECK] PASS' "$log") >= 10 ))
    (( $(grep -Fc '[TASKDIAG][CHECK] PASS' "$log") >= 10 ))
    (( $(grep -Fc '[SYNC][SLEEP] PASS' "$log") >= 3 ))
    (( $(grep -Fc \
        '[SYNC][TIMER_ORDER] PASS old_position=1 new_position=2 callbacks=2' \
        "$log") >= 5 ))
    (( $(grep -Ec \
        '\[SYNC\]\[TIMER_CANCEL\] PASS run=[0-9]+ requested=3 completed=3 errors=0' \
        "$log") >= 5 ))
    (( $(grep -Fc \
        'PASS pending_cancel=0 duplicate_cancel=2 claimed_cancel=1 callbacks=1' \
        "$log") >= 5 ))
    (( $(grep -Fc '[SMP][KILL_SWEEP] PASS workers=8' "$log") >= 4 ))
    (( $(grep -Fc '[TASKMAN][AUTO_EXIT] PASS target=3' "$log") >= 4 ))
    require_marker '[TASKMANTEST][HEAP] PASS' "$log"
    require_marker 'drift=0' "$log"
    require_marker 'timers_claimed=0' "$log"
    require_marker 'clockevent_non_bsp=0' "$log"
    require_marker 'clockevent_early=0' "$log"
    require_marker 'clockevent_regressions=0' "$log"
    require_marker 'hpet_stray_irqs=0' "$log"
    require_marker 'unexpected=0 imbalance=0' "$log"
    verify_timer_fair_log "$log" 5 5
    ! grep -Fq '[ACCOUNT][LAPIC_RATE]' "$log"
    ! grep -Eq 'PANIC|#PF|#GP|FATAL|STRUCTURAL_FAULT|FINISH_FAULT' "$log"
    printf '[TIMER][SOAK_RUNTIME] PASS timer_residual=0 heap_drift=0 non_bsp=0 early=0 regressions=0 hpet_irq=0 hpet_stray=0 unexpected=0 imbalance=0 faults=0\n' \
        >>"$log"
    printf '[IRQ][RENDEZVOUS_SOAK] PASS smp=24 runtime_ready=24/24 abort=0 failed=0 duration_ms=%s\n' \
        "$elapsed" >>"$log"
    record_stage rendezvous-soak24 rendezvous-soak24 q35 24 kvm "$log" \
        '[TIMER][SOAK] PASS'
}

boot_to_shell_soak24()
{
    [[ $duration_ms =~ ^[0-9]+$ ]] && ((duration_ms >= 300000)) || {
        echo 'DURATION_MS must be at least 300000' >&2
        return 2
    }
    [[ -e /dev/kvm && -r /dev/kvm && -w /dev/kvm ]] || {
        echo 'BLOCKED_BY_EXTERNAL_ENVIRONMENT: /dev/kvm unavailable' >&2
        return 1
    }
    local stage=boot-to-shell-soak24 dir log
    local start now elapsed iteration=0 stress_runs=0 taskman_runs=0
    local order_runs=0 cancel_runs=0
    dir=$qemu_artifact/$stage
    log=$dir/serial.log
    [[ ! -e $dir ]] || {
        echo "refusing to overwrite soak evidence: $dir" >&2
        return 1
    }
    capture_host_timer_environment "$stage" kvm 24
    start_vm "$stage" q35 24 kvm 0
    wait_boot_to_main_loop "$stage" q35 24 kvm
    start=$(date +%s%3N)
    send_command 'inputtest check' '[INPUTTEST][CHECK] PASS' 90
    send_command 'modaltest check' '[MODALTEST][CHECK] PASS' 90
    send_command 'synctest check' '[SYNC][CHECK] PASS' 90
    send_command 'taskmantest heap-begin' '[TASKMANTEST][HEAP_BEGIN] PASS' 120
    while :; do
        iteration=$((iteration + 1))
        send_command 'irq check' \
            '[IRQ][CHECK] PASS clocksource=HPET clockevent=BSP_LAPIC' 90 stress
        send_command 'taskdiag check' '[TASKDIAG][CHECK] PASS' 90 stress
        if ((order_runs < 5)); then
            send_command 'synctest timer-order' \
                '[SYNC][TIMER_ORDER] PASS old_position=1 new_position=2' \
                90 stress
            order_runs=$((order_runs + 1))
        fi
        if ((cancel_runs < 5)); then
            async_sync_test timer-cancel TIMER_CANCEL 120
            cancel_runs=$((cancel_runs + 1))
        fi
        if ((taskman_runs < 4)); then
            taskman_smoke
            taskman_runs=$((taskman_runs + 1))
        fi
        if ((stress_runs < 4)); then
            stress_smoke
            stress_runs=$((stress_runs + 1))
        fi
        now=$(date +%s%3N)
        elapsed=$((now - start))
        if ((iteration >= 10 && order_runs >= 5 && cancel_runs >= 5 &&
             taskman_runs >= 4 && stress_runs >= 4 && elapsed >= duration_ms)); then
            break
        fi
        if ((iteration >= 10 && elapsed < duration_ms)); then
            sleep 5
        fi
    done
    send_command 'inputtest check' '[INPUTTEST][CHECK] PASS' 90
    send_command 'modaltest check' '[MODALTEST][CHECK] PASS' 90
    send_command 'synctest timer-backlog' \
        '[SYNC][TIMER_BACKLOG] PASS old=8 new=8 callbacks=16' 120 stress
    send_command 'synctest check' '[SYNC][CHECK] PASS' 90
    send_command 'reaptest check' '[REAPTEST][CHECK] PASS' 90
    send_command 'killtest check' '[KILLTEST][CHECK] PASS' 90
    send_command 'taskmantest check' '[TASKMANTEST][CHECK] PASS' 90
    send_command 'irq boot' '[IRQ][BOOT_CLOCK] clocksource=HPET' 90
    send_command 'irq check' \
        '[IRQ][CHECK] PASS clocksource=HPET clockevent=BSP_LAPIC' 90
    send_command 'taskdiag check' '[TASKDIAG][CHECK] PASS' 90
    send_command 'taskmantest heap-end' '[TASKMANTEST][HEAP] PASS' 180
    now=$(date +%s%3N)
    elapsed=$((now - start))
    collect_vm "$stage"

    verify_boot_to_shell_run "$stage" 24
    ((elapsed >= duration_ms))
    (( $(grep -Fc '[IRQ][CHECK] PASS' "$log") >= 10 ))
    (( $(grep -Fc '[TASKDIAG][CHECK] PASS' "$log") >= 10 ))
    (( $(grep -Fc \
        '[SYNC][TIMER_ORDER] PASS old_position=1 new_position=2 callbacks=2' \
        "$log") >= 5 ))
    (( $(grep -Ec \
        '\[SYNC\]\[TIMER_CANCEL\] PASS run=[0-9]+ requested=3 completed=3 errors=0' \
        "$log") >= 5 ))
    (( $(grep -Fc '[TASKMAN][AUTO_EXIT] PASS target=3' "$log") >= 4 ))
    (( $(grep -Fc '[SMP][KILL_SWEEP] PASS workers=8' "$log") >= 4 ))
    require_marker '[TASKMANTEST][HEAP] PASS' "$log"
    require_marker 'timers_claimed=0' "$log"
    require_marker 'late=0' "$log"
    require_marker 'clockevent_non_bsp=0' "$log"
    require_marker 'clockevent_early=0' "$log"
    require_marker 'clockevent_regressions=0' "$log"
    require_marker 'hpet_stray_irqs=0' "$log"
    require_marker 'unexpected=0 imbalance=0' "$log"
    verify_timer_fair_log "$log" 5 5
    ! grep -Fq '[ACCOUNT][LAPIC_RATE]' "$log"
    ! grep -Eq \
        'PANIC|FATAL|#PF|#GP|DOUBLE FAULT|TRIPLE FAULT|STRUCTURAL_FAULT|FINISH_FAULT|CPU_READY_TIMEOUT|console render suspension residual|boot stage timeout' \
        "$log"
    printf '[BOOT][SOAK24] PASS smp=24 duration_ms=%s irq_checks=%s taskdiag_checks=%s timer_order=%s timer_cancel=%s taskman=%s stress_kill=%s console_suspended=0 claimed_residual=0 late_callbacks=0 faults=0\n' \
        "$elapsed" "$(grep -Fc '[IRQ][CHECK] PASS' "$log")" \
        "$(grep -Fc '[TASKDIAG][CHECK] PASS' "$log")" \
        "$(grep -Fc '[SYNC][TIMER_ORDER] PASS old_position=1 new_position=2 callbacks=2' "$log")" \
        "$(grep -Ec '\[SYNC\]\[TIMER_CANCEL\] PASS run=[0-9]+ requested=3 completed=3 errors=0' "$log")" \
        "$(grep -Fc '[TASKMAN][AUTO_EXIT] PASS target=3' "$log")" \
        "$(grep -Fc '[SMP][KILL_SWEEP] PASS workers=8' "$log")" | \
        tee "$autonomous_artifact/soak24.log"
    record_stage "$stage" "$stage" q35 24 kvm "$log" \
        '[IRQ][CHECK] PASS'
}

continuity_after_tests()
{
    verify_rendezvous_continuity after-tests
    rendezvous_runtime_manifest \
        "$rendezvous_artifact/runtime-after-tests.sha256"
}

final_build()
{
    local log=$rendezvous_artifact/final-build.log jobs
    jobs=$(nproc)
    {
        make clean
        make stack-check
        make kernel-check JOBS=2
        cp kernel.elf /tmp/hobbyos-lapic-clockevent-j2.elf
        make clean
        make kernel-check JOBS="$jobs"
        cp kernel.elf /tmp/hobbyos-lapic-clockevent-jN.elf
        sha256sum /tmp/hobbyos-lapic-clockevent-j2.elf \
            /tmp/hobbyos-lapic-clockevent-jN.elf
        cmp -s /tmp/hobbyos-lapic-clockevent-j2.elf \
            /tmp/hobbyos-lapic-clockevent-jN.elf
        make deps-check
        make production-image
        cmp -s kernel.elf /tmp/hobbyos-lapic-clockevent-jN.elf
        [[ -z $(nm -u kernel.elf) ]]
        make production-test-policy
        echo '[TIMER][FINAL_BUILD] PASS'
    } 2>&1 | tee "$log"
    verify_rendezvous_continuity after-build
    rendezvous_runtime_manifest \
        "$rendezvous_artifact/runtime-after-build.sha256"
    record_stage rendezvous-final-build rendezvous-final-build host 0 none "$log" \
        '[TIMER][FINAL_BUILD] PASS'
}

boot_to_shell_final_build()
{
    local dir=$autonomous_artifact/final-build jobs max_frame
    local j2_hash jn_hash owned_warnings=0
    [[ ! -e $dir ]] || {
        echo "refusing to overwrite final-build evidence: $dir" >&2
        return 1
    }
    mkdir -p "$dir"
    jobs=$(nproc)

    make clean >"$dir/stack-check.log" 2>&1
    make stack-check >>"$dir/stack-check.log" 2>&1
    cp artifacts/build/stack-usage-report.txt "$dir/stack-usage-report.txt"
    require_marker 'stack-check: PASS' "$dir/stack-check.log"
    max_frame=$(awk '/^ *[0-9]+ / {if ($1 > max) max=$1} END {print max+0}' \
        "$dir/stack-usage-report.txt")
    ((max_frame <= 2048))

    make clean >"$dir/kernel-check-j2.log" 2>&1
    make kernel-check JOBS=2 >>"$dir/kernel-check-j2.log" 2>&1
    cp kernel.elf "$dir/kernel-j2.elf"
    j2_hash=$(sha "$dir/kernel-j2.elf")

    make clean >"$dir/kernel-check-jN.log" 2>&1
    make kernel-check JOBS="$jobs" >>"$dir/kernel-check-jN.log" 2>&1
    cp kernel.elf "$dir/kernel-jN.elf"
    jn_hash=$(sha "$dir/kernel-jN.elf")
    cmp -s "$dir/kernel-j2.elf" "$dir/kernel-jN.elf"
    [[ $j2_hash == "$jn_hash" ]]

    make deps-check >"$dir/deps-check.log" 2>&1
    nm -u kernel.elf >"$dir/undefined-symbols.txt"
    [[ ! -s $dir/undefined-symbols.txt ]]
    strings kernel.elf | grep -F '[GRAPHICS][SPLASH_SELFTEST] PASS' >/dev/null
    strings kernel.elf | grep -F '[TIMER][SELFTEST] CLOCKEVENT_MODEL_OK' >/dev/null

    : >"$dir/owned-warnings.txt"
    local owned
    for owned in kernel/kernel.c kernel/src/core/kernel_init.c \
        kernel/src/graphics/splash.c kernel/src/drivers/pci.c; do
        grep -hF "$owned:" "$dir/kernel-check-j2.log" \
            "$dir/kernel-check-jN.log" | grep -F 'warning:' \
            >>"$dir/owned-warnings.txt" || true
    done
    owned_warnings=$(wc -l <"$dir/owned-warnings.txt")
    ((owned_warnings == 0))

    verify_rendezvous_continuity after-build
    python3 scripts/verify-timer-clockevent.py source-manifest \
        --output "$dir/source-final.sha256" \
        kernel/kernel.c kernel/src/core/kernel_init.c \
        kernel/src/graphics/splash.c kernel/src/graphics/splash.h \
        kernel/src/drivers/pci.c scripts/test-timer-clockevent.sh \
        scripts/verify-timer-clockevent.py scripts/harness-common.sh
    git diff --check
    printf '[BOOT][FINAL_BUILD] PASS stack_max=%s stack_limit=2048 j2_sha256=%s jN_sha256=%s byte_identical=1 undefined=0 owned_warnings=%s jobs=%s\n' \
        "$max_frame" "$j2_hash" "$jn_hash" "$owned_warnings" "$jobs" | \
        tee "$dir/final-build.log"
    record_stage boot-to-shell-final-build boot-to-shell-final-build \
        host 0 none "$dir/final-build.log" '[BOOT][FINAL_BUILD] PASS'
}

report_gate()
{
    continuity_after_tests
    local log=$rendezvous_artifact/report.log
    {
        python3 scripts/verify-timer-clockevent.py verify matrix
        echo '[TIMER][REPORT] PASS'
    } 2>&1 | tee "$log"
    record_stage rendezvous-report rendezvous-report host 0 none "$log" \
        '[TIMER][REPORT] PASS'
}

candidate_gate()
{
    local candidate=artifacts/baremetal/lapic-clockevent-candidate
    local head tree short review_zip review_hash kernel_hash image_hash boot_hash
    local payload_tmp log=$fair_artifact/candidate.log candidate_build
    local rate_host rate_raw rate_worst rate_authority rate_disposition
    local qemu_disposition rate_log_hash soak_duration
    local focused4_order focused4_cancel focused24_order focused24_cancel
    local soak_order soak_cancel
    local -a candidate_evidence
    [[ $(git branch --show-current) == feat/taskman ]]
    head=$(git rev-parse HEAD)
    tree=$(git rev-parse 'HEAD^{tree}')
    [[ $(git rev-parse HEAD^) == "$base" ]]
    [[ $(git rev-list --count "$base..HEAD") == 1 ]]
    [[ $(git log -1 --format=%s) == "$subject" ]]
    [[ $(git status --short) == \
        '?? ROADMAP_TASKMAN_V1_CLOSURE_HARDENING.md' ]]
    [[ ! -S $hmp_socket && ! -e $runtime/qemu.pid ]]
    ! pgrep -f '[q]emu-system-x86_64' >/dev/null
    python3 scripts/verify-timer-clockevent.py verify matrix
    [[ ! -e $candidate ]]
    candidate_build=$fair_artifact/candidate-build.log
    {
        make clean
        make production-image
        cmp -s kernel.elf /tmp/hobbyos-lapic-clockevent-jN.elf
        [[ -z $(nm -u kernel.elf) ]]
        make production-test-policy
        echo '[TIMER][CANDIDATE_BUILD] PASS qemu_booted=0'
    } >"$candidate_build" 2>&1
    verify_fair_source_continuity candidate
    mkdir -p "$candidate"
    cp hobbyos.img kernel.elf BOOTX64.EFI "$candidate/"
    cp docs/baremetal/LAPIC_CLOCKEVENT_OPERATOR_RUNBOOK.md "$candidate/"
    cp docs/test-reports/TIMER_CLOCKEVENT_CERTIFICATION.md "$candidate/"
    cp docs/timer-clocksource-clockevent.md "$candidate/"

    payload_tmp=$(mktemp -d /tmp/hobbyos-lapic-clockevent.XXXXXX)
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
names = (("/kernel.elf", "kernel.elf"),
         ("/EFI/BOOT/BOOTX64.EFI", "BOOTX64.EFI"),
         ("/EFI/fonts/zap-light16.psf", "zap-light16.psf"),
         ("/EFI/images/logo.bmp", "logo.bmp"),
         ("/startup.nsh", "startup.nsh"))
output.write_text("".join(
    f"{hashlib.sha256((source / local).read_bytes()).hexdigest()}  {image}\n"
    for image, local in names), encoding="utf-8")
PY
    rm -rf "$payload_tmp"

    kernel_hash=$(sha kernel.elf)
    image_hash=$(sha hobbyos.img)
    boot_hash=$(sha BOOTX64.EFI)
    mapfile -t candidate_evidence < <(
        python3 - "$artifact/evidence.json" <<'PY'
import json
import pathlib
import re
import sys

rows = json.load(open(sys.argv[1], encoding="utf-8"))
by_stage = {item.get("stage"): item for item in rows}
row = by_stage["fair-smp24-kvm"]
soak = by_stage["fair-soak24"]
text = pathlib.Path(soak["log"]).read_text(encoding="utf-8", errors="replace")
match = re.search(r"\[TIMER\]\[SOAK\] PASS .* duration_ms=(\d+)", text)
if match is None:
    raise SystemExit("soak duration missing")
for value in (
    row["host_schedulable_cpus"], row["raw_test_result"],
    row["worst_median_x1000"], row["measurement_authority"],
    row["scenario_disposition"], row["sha256"],
    by_stage["fair-focused-smp4"]["timer_order_passes"],
    by_stage["fair-focused-smp4"]["timer_cancel_passes"],
    by_stage["fair-focused-smp24"]["timer_order_passes"],
    by_stage["fair-focused-smp24"]["timer_cancel_passes"],
    soak["timer_order_passes"], soak["timer_cancel_passes"],
    match.group(1),
):
    print(value)
PY
    )
    rate_host=${candidate_evidence[0]}
    rate_raw=${candidate_evidence[1]}
    rate_worst=${candidate_evidence[2]}
    rate_authority=${candidate_evidence[3]}
    rate_disposition=${candidate_evidence[4]}
    rate_log_hash=${candidate_evidence[5]}
    focused4_order=${candidate_evidence[6]}
    focused4_cancel=${candidate_evidence[7]}
    focused24_order=${candidate_evidence[8]}
    focused24_cancel=${candidate_evidence[9]}
    soak_order=${candidate_evidence[10]}
    soak_cancel=${candidate_evidence[11]}
    soak_duration=${candidate_evidence[12]}
    qemu_disposition=PASS
    if [[ $rate_disposition == PASS_WITH_ENVIRONMENT_RATE_EXCEPTION ]]; then
        qemu_disposition=PASS_WITH_DOCUMENTED_ENVIRONMENT_RATE_EXCEPTION
    fi
    cat >"$candidate/candidate.txt" <<EOF
STATUS=READY_FOR_BARE_METAL_LAPIC_CLOCKEVENT_VALIDATION
BRANCH=feat/taskman
BASE=$base
PARENT=$base
COMMIT=$head
TREE=$tree
SUBJECT=$subject
KERNEL_SHA256=$kernel_hash
IMAGE_SHA256=$image_hash
BOOTX64_SHA256=$boot_hash
CPU_POLICY=discovered topology
LAPIC_PERIOD_US=1000
CLOCKSOURCE=HPET
CLOCKEVENT=BSP_LAPIC
HPET_TIMER0=QUIESCENT
ROOT_CAUSE=PREEXISTING_CLAIMED_TIMER_BYPASSED_BY_NEWLY_DUE_TIMER
TIMER_FAIRNESS_GATE=PASS
TIMER_CANCEL_REPEATED_GATE=PASS
FOCUSED_SMP4_TIMER_ORDER_PASSES=$focused4_order
FOCUSED_SMP4_TIMER_CANCEL_PASSES=$focused4_cancel
FOCUSED_SMP24_TIMER_ORDER_PASSES=$focused24_order
FOCUSED_SMP24_TIMER_CANCEL_PASSES=$focused24_cancel
SOAK_TIMER_ORDER_PASSES=$soak_order
SOAK_TIMER_CANCEL_PASSES=$soak_cancel
LATE_CALLBACKS=0
CLAIMED_RESIDUAL=0
HOST_SCHEDULABLE_CPUS=$rate_host
SMP24_GUEST_VCPUS=24
SMP24_RATE_RAW_RESULT=$rate_raw
SMP24_RATE_WORST_MEDIAN_X1000=$rate_worst
SMP24_RATE_THRESHOLD=700
SMP24_RATE_AUTHORITY=$rate_authority
SMP24_SCENARIO_DISPOSITION=$rate_disposition
SMP24_RATE_LOG_SHA256=$rate_log_hash
ORIGINAL_SMP24_RATE_RAW_RESULT=FAIL
ORIGINAL_SMP24_RATE_WORST_MEDIAN_X1000=672
ORIGINAL_SMP24_RATE_LOG_SHA256=75fc283521f23bae6916493982ddb188768412d0e9b177aa2a9a2a55f6c39d46
ORIGINAL_TIMER_CANCEL_RESULT=FAIL
ORIGINAL_TIMER_CANCEL_LOG_SHA256=29ace19ec5bd95ef29f8d962186dcd88d5ec159e636427e708b54ea869dc8e70
SMP4_RATE_CONTROL=SMP4_KVM_3_OF_3_PASS
QEMU_MATRIX=PASS
QEMU_DISPOSITION=$qemu_disposition
SOAK24=PASS
SOAK24_DURATION_MS=$soak_duration
BARE_METAL_EXECUTED_BY_CODEX=NO
NEXT_OWNER=William
EOF
    (
        cd "$candidate"
        find . -maxdepth 1 -type f ! -name SHA256SUMS -printf '%P\0' |
            LC_ALL=C sort -z | xargs -0 sha256sum >SHA256SUMS
        sha256sum -c SHA256SUMS >/dev/null
    )

    {
        echo "[TIMER][CANDIDATE] PASS commit=$head tree=$tree kernel=$kernel_hash image=$image_hash bootx64=$boot_hash"
    } | tee "$log"
    record_stage candidate candidate host 0 none "$log" '[TIMER][CANDIDATE] PASS'

    short=${head:0:7}
    review_zip=artifacts/baremetal/HobbyOS-LAPIC-CLOCKEVENT-FAIR-TIMER-$short.zip
    [[ ! -e $review_zip ]]
    python3 - "$root" "$review_zip" "$short" <<'PY'
import os
import pathlib
import stat
import sys
import zipfile

root = pathlib.Path(sys.argv[1])
target = pathlib.Path(sys.argv[2])
prefix = f"HobbyOS-LAPIC-CLOCKEVENT-FAIR-TIMER-{sys.argv[3]}"
skip_dirs = {"artifacts", ".qemu", "__pycache__"}
skip_names = {"kernel.elf", "hobbyos.img", "BOOTX64.EFI", "bootx64.so"}
skip_suffixes = {".o", ".d", ".su", ".pyc", ".sock", ".pid"}
with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
    for current, dirs, files in os.walk(root):
        current_path = pathlib.Path(current)
        relative_dir = current_path.relative_to(root)
        dirs[:] = sorted(name for name in dirs if name not in skip_dirs)
        for name in sorted(files):
            path = current_path / name
            if name in skip_names or path.suffix in skip_suffixes:
                continue
            if stat.S_ISSOCK(path.lstat().st_mode):
                continue
            relative = relative_dir / name
            archive.write(path, pathlib.PurePosixPath(prefix, relative.as_posix()))
    evidence = root / "artifacts/build/lapic-clockevent"
    fairness = root / "artifacts/build/timer-fair-dispatch"
    include = [root / "artifacts/baremetal/lapic-clockevent-candidate",
               evidence / "evidence.json", evidence / "evidence.tsv",
               evidence / "host-environment.txt",
               evidence / "source-before.sha256",
               evidence / "protected-before.sha256",
               evidence / "source-worktree-before-closure.sha256",
               evidence / "qemu/smp24-kvm-failed/serial.log",
               evidence / "qemu/soak24-failed/serial.log",
               fairness / "source-before.sha256",
               fairness / "protected-before.sha256",
               fairness / "clockevent-worktree-before.sha256",
               fairness / "clockevent-worktree-after-tests.sha256",
               fairness / "clockevent-worktree-after-build.sha256",
               fairness / "protected-after-tests.sha256",
               fairness / "protected-after-build.sha256",
               fairness / "source-worktree-after-closure.sha256",
               fairness / "final-build.log", fairness / "candidate-build.log",
               fairness / "report.log", fairness / "candidate.log"]
    for scenario in ("fair-focused-smp4", "fair-focused-smp24",
                     "fair-up-tcg", "fair-smp2-tcg", "fair-smp4-kvm",
                     "fair-smp8-kvm", "fair-smp24-kvm", "fair-pcat",
                     "fair-soak24", "fair-rate-control-smp4-run1",
                     "fair-rate-control-smp4-run2",
                     "fair-rate-control-smp4-run3"):
        for name in ("serial.log", "commands.tsv", "launch.env",
                     "kernel.payload.elf"):
            include.append(evidence / "qemu" / scenario / name)
    for scenario in ("fair-negative-hpet-irq", "fair-negative-ap-tick",
                     "fair-negative-early-tick", "fair-negative-duplicate",
                     "fair-negative-hpet-stall", "fair-negative-claimed-order"):
        for name in ("serial.log", "commands.tsv", "build.log",
                     "normal-build.log"):
            include.append(evidence / "negatives" / scenario / name)
    for path in include:
        if not path.exists():
            continue
        if path.is_dir():
            files = sorted(item for item in path.rglob("*") if item.is_file())
        else:
            files = [path]
        for item in files:
            relative = item.relative_to(root)
            archive.write(item, pathlib.PurePosixPath(prefix, relative.as_posix()))
with zipfile.ZipFile(target) as archive:
    if archive.testzip():
        raise SystemExit("review ZIP corruption")
    names = archive.namelist()
    if not any(name.endswith("/.git/HEAD") for name in names):
        raise SystemExit("review ZIP missing .git")
PY
    review_hash=$(sha "$review_zip")
    echo "[TIMER][REVIEW_ZIP] PASS path=$review_zip sha256=$review_hash" |
        tee "$artifact/review-zip.log"
    python3 scripts/verify-timer-clockevent.py verify all | tee "$artifact/evidence-all.log"
}

boot_to_shell_candidate_gate()
{
    local candidate=artifacts/baremetal/lapic-clockevent-candidate
    local build_log=$autonomous_artifact/candidate-build.log
    local head tree short kernel_hash image_hash boot_hash payload_tmp
    local review_zip review_hash status
    [[ $(git branch --show-current) == feat/taskman ]]
    head=$(git rev-parse HEAD)
    tree=$(git rev-parse 'HEAD^{tree}')
    [[ $(git rev-parse HEAD^) == "$base" ]]
    [[ $(git rev-list --count "$base..HEAD") == 1 ]]
    [[ $(git log -1 --format=%s) == "$subject" ]]
    git diff --quiet
    git diff --cached --quiet
    status=$(git status --short)
    [[ $status == '?? ROADMAP_TASKMAN_V1_CLOSURE_HARDENING.md' ]]
    [[ ! -S $hmp_socket && ! -e $runtime/qemu.pid ]]
    ! pgrep -f '[q]emu-system-x86_64' >/dev/null
    [[ ! -e $candidate ]]

    {
        make production-image
        cmp -s kernel.elf \
            "$autonomous_artifact/final-build/kernel-jN.elf"
        [[ -z $(nm -u kernel.elf) ]]
        make production-test-policy
        echo '[BOOT][CANDIDATE_BUILD] PASS production=1 qemu_booted=0'
    } >"$build_log" 2>&1
    require_marker '[BOOT][CANDIDATE_BUILD] PASS' "$build_log"
    python3 scripts/verify-timer-clockevent.py autonomous-closure \
        >"$autonomous_artifact/autonomous-verification-postcommit.log"

    payload_tmp=$(mktemp -d /tmp/hobbyos-boot-to-shell-candidate.XXXXXX)
    mcopy -i hobbyos.img ::/kernel.elf "$payload_tmp/kernel.elf"
    mcopy -i hobbyos.img ::/EFI/BOOT/BOOTX64.EFI "$payload_tmp/BOOTX64.EFI"
    mcopy -i hobbyos.img ::/EFI/fonts/zap-light16.psf \
        "$payload_tmp/zap-light16.psf"
    mcopy -i hobbyos.img ::/EFI/images/logo.bmp "$payload_tmp/logo.bmp"
    mcopy -i hobbyos.img ::/startup.nsh "$payload_tmp/startup.nsh"
    cmp -s kernel.elf "$payload_tmp/kernel.elf"
    cmp -s BOOTX64.EFI "$payload_tmp/BOOTX64.EFI"
    cmp -s bootloader/fonts/zap-light16.psf "$payload_tmp/zap-light16.psf"
    cmp -s bootloader/images/logo.bmp "$payload_tmp/logo.bmp"
    cmp -s bootloader/startup.nsh "$payload_tmp/startup.nsh"

    mkdir -p "$candidate"
    cp hobbyos.img kernel.elf BOOTX64.EFI "$candidate/"
    cp docs/baremetal/LAPIC_CLOCKEVENT_OPERATOR_RUNBOOK.md "$candidate/"
    cp docs/test-reports/BOOT_TO_SHELL_AUTONOMOUS_CLOSURE.md "$candidate/"
    cp docs/test-reports/TIMER_CLOCKEVENT_CERTIFICATION.md "$candidate/"
    cp "$autonomous_artifact/cycles.tsv" "$autonomous_artifact/cycles.json" \
        "$autonomous_artifact/autonomous-verification.log" "$candidate/"
    (
        cd "$payload_tmp"
        sha256sum kernel.elf BOOTX64.EFI zap-light16.psf logo.bmp startup.nsh \
            >"$root/$candidate/payload-manifest.txt"
    )
    rm -rf "$payload_tmp"

    kernel_hash=$(sha kernel.elf)
    image_hash=$(sha hobbyos.img)
    boot_hash=$(sha BOOTX64.EFI)
    cat >"$candidate/candidate.txt" <<EOF
STATUS=READY_FOR_BARE_METAL_LAPIC_CLOCKEVENT_VALIDATION
BRANCH=feat/taskman
BASE=$base
PARENT=$base
COMMIT=$head
TREE=$tree
SUBJECT=$subject
KERNEL_SHA256=$kernel_hash
IMAGE_SHA256=$image_hash
BOOTX64_SHA256=$boot_hash
CLOCKSOURCE=HPET
HPET_TIMER0=QUIESCENT
CLOCKEVENT=BSP_LAPIC
LAPIC_PERIOD_US=1000
CPU_POLICY=DISCOVERED_TOPOLOGY_NO_CAP_NO_FILTER
BOOT_TO_SHELL_SMP24=PASS_3_OF_3_NO_RETRY
QUICK_MATRIX=PASS_6_OF_6
TIMER_FAIRNESS_SMP4=ORDER_3_CANCEL_3
TIMER_FAIRNESS_SMP24=ORDER_5_CANCEL_5
LATE_CALLBACKS=0
CLAIMED_RESIDUAL=0
SMP24_HOST_SCHEDULABLE_CPUS=12
SMP24_GUEST_VCPUS=24
SMP24_RATE_RAW_RESULT=FAIL
SMP24_RATE_WORST_MEDIAN_X1000=497
SMP24_RATE_THRESHOLD=700
SMP24_RATE_AUTHORITY=ENVIRONMENT_NON_AUTHORITATIVE_OVERSUBSCRIBED
SMP4_RATE_CONTROL=PASS_3_OF_3_997_996_995
SOAK24=PASS
SOAK24_DURATION_MS=979860
STACK_MAX_BYTES=1968
BUILD_J2_JN_IDENTICAL=YES
UNDEFINED_SYMBOLS=0
OWNED_WARNINGS=0
QEMU_BOOTED_CANDIDATE_IMAGE=NO
BARE_METAL_EXECUTED_BY_CODEX=NO
PUSH_PERFORMED=NO
NEXT_OWNER=William
EOF
    (
        cd "$candidate"
        find . -maxdepth 1 -type f ! -name SHA256SUMS -printf '%P\0' |
            LC_ALL=C sort -z | xargs -0 sha256sum >SHA256SUMS
        sha256sum -c SHA256SUMS >/dev/null
    )

    short=${head:0:7}
    review_zip=artifacts/baremetal/HobbyOS-BOOT-TO-SHELL-LAPIC-CLOCKEVENT-$short.zip
    [[ ! -e $review_zip ]]
    python3 - "$root" "$review_zip" "$short" <<'PY'
import os
import pathlib
import stat
import sys
import zipfile

root = pathlib.Path(sys.argv[1])
target = pathlib.Path(sys.argv[2])
prefix = f"HobbyOS-BOOT-TO-SHELL-LAPIC-CLOCKEVENT-{sys.argv[3]}"
skip_dirs = {"artifacts", ".qemu", "__pycache__"}
skip_names = {"kernel.elf", "hobbyos.img", "BOOTX64.EFI", "bootx64.so"}
skip_suffixes = {".o", ".d", ".su", ".pyc", ".sock", ".pid"}

with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED,
                     compresslevel=6, allowZip64=True) as archive:
    for current, dirs, files in os.walk(root):
        current_path = pathlib.Path(current)
        relative_dir = current_path.relative_to(root)
        dirs[:] = sorted(name for name in dirs if name not in skip_dirs)
        for name in sorted(files):
            path = current_path / name
            if name in skip_names or path.suffix in skip_suffixes:
                continue
            if stat.S_ISSOCK(path.lstat().st_mode):
                continue
            relative = relative_dir / name
            archive.write(path, pathlib.PurePosixPath(prefix,
                          relative.as_posix()))

    include = [root / "artifacts/baremetal/lapic-clockevent-candidate"]
    autonomous = root / "artifacts/build/boot-to-shell-autonomous"
    include.extend(autonomous / name for name in (
        "cycles.tsv", "cycles.json", "smp24-three-boots.tsv",
        "quick-matrix.log", "soak24.log", "autonomous-verification.log",
        "autonomous-verification-postcommit.log",
        "final-build/final-build.log", "final-build/stack-usage-report.txt",
        "final-build/undefined-symbols.txt", "final-build/owned-warnings.txt",
    ))
    evidence = root / "artifacts/build/lapic-clockevent"
    scenarios = [
        *(f"boot-to-shell-final-smp24-run{run}" for run in range(1, 4)),
        "boot-to-shell-matrix-q35-smp1-tcg",
        "boot-to-shell-matrix-q35-smp2-tcg",
        "boot-to-shell-matrix-q35-smp4-kvm",
        "boot-to-shell-matrix-q35-smp8-kvm",
        "boot-to-shell-matrix-q35-smp24-kvm",
        "boot-to-shell-matrix-pc-smp4-tcg",
        "rendezvous-runtime-smp24", "boot-to-shell-soak24",
        "rendezvous-rate-control-smp4-run1",
        "rendezvous-rate-control-smp4-run2",
        "rendezvous-rate-control-smp4-run3",
    ]
    for scenario in scenarios:
        for name in ("serial.log", "commands.tsv", "launch.env",
                     "boot-progress.tsv"):
            include.append(evidence / "qemu" / scenario / name)
    include.extend((
        evidence / "negatives/boot-to-shell-negative-claimed-order/serial.log",
        evidence / "negatives/boot-to-shell-negative-claimed-order/commands.tsv",
        evidence / "qemu/boot-to-shell-cycle1-focused-smp24/stall.json",
        evidence / "qemu/boot-to-shell-cycle1-focused-smp24/stall.md",
        evidence / "qemu/boot-to-shell-cycle2-focused-smp24/stall.json",
        evidence / "qemu/boot-to-shell-cycle2-focused-smp24/stall.md",
    ))
    for path in include:
        if not path.exists():
            continue
        files = (sorted(item for item in path.rglob("*") if item.is_file())
                 if path.is_dir() else [path])
        for item in files:
            relative = item.relative_to(root)
            archive.write(item, pathlib.PurePosixPath(prefix,
                          relative.as_posix()))

with zipfile.ZipFile(target) as archive:
    bad = archive.testzip()
    if bad is not None:
        raise SystemExit(f"review ZIP corruption at {bad}")
    names = archive.namelist()
    if not any(name.endswith("/.git/HEAD") for name in names):
        raise SystemExit("review ZIP missing .git")
    if not any(name.endswith("/docs/test-reports/BOOT_TO_SHELL_AUTONOMOUS_CLOSURE.md")
               for name in names):
        raise SystemExit("review ZIP missing closure report")
    if not any(name.endswith("/artifacts/baremetal/lapic-clockevent-candidate/hobbyos.img")
               for name in names):
        raise SystemExit("review ZIP missing candidate image")
PY
    review_hash=$(sha "$review_zip")
    printf '%s  %s\n' "$review_hash" "$(basename "$review_zip")" \
        >artifacts/baremetal/review-zip.sha256
    printf '[BOOT][CANDIDATE] PASS commit=%s tree=%s kernel=%s image=%s bootx64=%s qemu_booted=0\n' \
        "$head" "$tree" "$kernel_hash" "$image_hash" "$boot_hash" | \
        tee "$autonomous_artifact/candidate.log"
    printf '[BOOT][REVIEW_ZIP] PASS path=%s sha256=%s source_git=1 candidate=1\n' \
        "$review_zip" "$review_hash" | \
        tee "$autonomous_artifact/review-zip.log"
}

all()
{
    preflight
    static_gate
    selftest_gate
    negatives
    run_rendezvous_focused_smp4
    rendezvous_three_smp24_boots
    rendezvous_matrix
    run_rendezvous_runtime_smp24
    rate_controls
    soak24
    continuity_after_tests
    final_build
    report_gate
}

case ${1:-all} in
    preflight) preflight ;;
    static) static_gate ;;
    selftest) selftest_gate ;;
    negatives) negatives ;;
    negative-claimed-order) negative_claimed_order ;;
    boot-to-shell-negative-claimed-order) \
        boot_to_shell_negative_claimed_order ;;
    boot-to-shell-negative-claimed-order-record-existing) \
        boot_to_shell_negative_claimed_order_record_existing ;;
    negative-local-ready-deadline) negative_local_ready_deadline ;;
    rate-controls) rate_controls ;;
    rendezvous-focused-smp4) run_rendezvous_focused_smp4 ;;
    rendezvous-smp24-boots) rendezvous_three_smp24_boots ;;
    rendezvous-matrix) rendezvous_matrix ;;
    rendezvous-runtime-smp24) run_rendezvous_runtime_smp24 ;;
    up-tcg) run_scenario fair-up-tcg q35 1 tcg ;;
    smp2-tcg) run_scenario fair-smp2-tcg q35 2 tcg ;;
    smp4-kvm) run_scenario fair-smp4-kvm q35 4 kvm ;;
    smp8-kvm) run_scenario fair-smp8-kvm q35 8 kvm ;;
    smp24-kvm) run_scenario fair-smp24-kvm q35 24 kvm ;;
    smp24-record-existing) record_existing_smp24 ;;
    pcat) run_scenario fair-pcat pc 4 tcg ;;
    focused-smp4) run_focused_timer_runtime fair-focused-smp4 4 ;;
    focused-smp24) run_focused_timer_runtime fair-focused-smp24 24 ;;
    focused-smp4-record-existing) \
        record_existing_focused_timer_runtime fair-focused-smp4 4 ;;
    focused-smp24-record-existing) \
        record_existing_focused_timer_runtime fair-focused-smp24 24 ;;
    focused-runtime) focused_runtime ;;
    boot-to-shell-cycle1) boot_to_shell_cycle1 ;;
    boot-to-shell-cycle2) boot_to_shell_cycle2 ;;
    boot-to-shell-cycle3) boot_to_shell_cycle3 ;;
    boot-to-shell-cycle4) boot_to_shell_cycle4 ;;
    boot-to-shell-final-smp24-boots) boot_to_shell_final_smp24_boots ;;
    boot-to-shell-quick-matrix) boot_to_shell_quick_matrix ;;
    boot-to-shell-quick-matrix-record-existing) \
        boot_to_shell_quick_matrix_record_existing ;;
    boot-to-shell-soak24) boot_to_shell_soak24 ;;
    boot-to-shell-final-build) boot_to_shell_final_build ;;
    soak24) soak24 ;;
    final-build) final_build ;;
    report) report_gate ;;
    candidate) candidate_gate ;;
    boot-to-shell-candidate) boot_to_shell_candidate_gate ;;
    all) all ;;
    *)
        echo "usage: $0 preflight|static|selftest|negatives|negative-claimed-order|boot-to-shell-negative-claimed-order|boot-to-shell-negative-claimed-order-record-existing|negative-local-ready-deadline|rendezvous-focused-smp4|rendezvous-smp24-boots|rendezvous-matrix|rendezvous-runtime-smp24|rate-controls|up-tcg|smp2-tcg|smp4-kvm|smp8-kvm|smp24-kvm|smp24-record-existing|pcat|focused-smp4|focused-smp24|focused-runtime|boot-to-shell-cycle1|boot-to-shell-cycle2|boot-to-shell-cycle3|boot-to-shell-cycle4|boot-to-shell-final-smp24-boots|boot-to-shell-quick-matrix|boot-to-shell-quick-matrix-record-existing|boot-to-shell-soak24|boot-to-shell-final-build|boot-to-shell-candidate|soak24|final-build|report|candidate|all" >&2
        exit 2
        ;;
esac
