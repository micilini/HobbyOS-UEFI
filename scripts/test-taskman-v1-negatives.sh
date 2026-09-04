#!/usr/bin/env bash
set -Eeuo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

serial=.qemu/qemu-serial.log
hmp=(python3 scripts/qemu_hmp.py --socket .qemu/hmp.sock)
harness_start_line=0
harness_command=negative
mkdir -p artifacts/build
source scripts/harness-common.sh
source scripts/harness-runtime-ready.sh
source scripts/harness-framed.sh
source scripts/harness-selftest.sh

cleanup(){ stop_qemu; }
trap cleanup EXIT

wait_detected(){
  local marker=$1 timeout=$2 deadline=$((SECONDS + timeout))
  while ((SECONDS <= deadline)); do
    grep -Fq "$marker" "$serial" && return 0
    grep -Eq 'PANIC|#PF|#GP|FATAL|STRUCTURAL_FAULT|QEMU exited' "$serial" && return 1
    pgrep -f qemu-system-x86_64 >/dev/null || return 1
    sleep 1
  done
  return 1
}

positive_canary(){
  local command=$1 marker=$2 timeout=$3
  send_complete "$command" "$marker" "$timeout" stress
}

transport_balanced(){
  local line
  send_complete "tasktest transport-status" "[HARNESS][STATUS]" 90 stress
  line=$(grep -F '[HARNESS][STATUS]' "$serial" | tail -1)
  [[ $line =~ accepted=([0-9]+)[[:space:]]completed=([0-9]+) ]]
  [[ ${BASH_REMATCH[1]} == "${BASH_REMATCH[2]}" ]]
}

runtime_snapshot(){
  local output=$1
  { { rg --files kernel bootloader shared; printf '%s\n' makefile; } |
      LC_ALL=C sort | xargs sha256sum; } >"$output"
}

checkpoint_negative(){
  local id=$1 macro=$2 contract=$3 marker=$4 negative_log=$5 reset_log=$6
  local runtime_manifest="artifacts/build/cl13-negative-checkpoints/${id}-runtime.sha256"
  mkdir -p artifacts/build/cl13-negative-checkpoints
  runtime_snapshot "$runtime_manifest"
  cmp -s artifacts/build/cl13fix11-runtime-before.sha256 "$runtime_manifest"
  scripts/cl13-evidence-checkpoint.py record-negative \
    --id "$id" --macro "$macro" --contract "$contract" --marker "$marker" \
    --negative-log "$negative_log" --reset-log "$reset_log" \
    --runtime-manifest "$runtime_manifest"
  scripts/cl13-evidence-checkpoint.py verify-negative --id "$id"
}

negative_one(){
  local id=$1 macro=$2 smp=$3 command=$4 marker=$5
  local positive_command=$6 positive_marker=$7 timeout=${8:-180}
  local negative_log="artifacts/build/cl13-negative-${id}.log"
  local positive_log="artifacts/build/cl13-negative-${id}-reset.log"

  echo "[CL13][NEGATIVE] BEGIN id=$id macro=$macro"
  make kernel-check JOBS=2 SELFTEST=1 SELFTEST_AUTORUN=0 \
    KERNEL_EXTRA_CFLAGS="-D$macro" >/dev/null
  make image SELFTEST=1 SELFTEST_AUTORUN=0 \
    KERNEL_EXTRA_CFLAGS="-D$macro" >/dev/null
  HARNESS_AUTORUN_EXPECTED=0
  start_qemu "$smp" tcg
  if [[ $id == sync-old-sem-gap ]]; then
    framed_synctest_async_negative "$command" \
      "[SYNC][BOUNDARY] START" "$marker" "$timeout" stress "0 1"
  else
    framed_send_complete "$command" "$marker" "$timeout" stress "0 1"
  fi
  grep -Fq "$marker" "$serial"
  ! grep -Fq 'MISSED' "$serial"
  ! grep -Eq 'PANIC|#PF|#GP|FATAL|STRUCTURAL_FAULT|QEMU exited' "$serial"
  cp "$serial" "$negative_log"
  stop_qemu

  make kernel-check JOBS=2 SELFTEST=1 SELFTEST_AUTORUN=1 >/dev/null
  make image SELFTEST=1 SELFTEST_AUTORUN=1 >/dev/null
  ! grep -Fq "$macro" artifacts/build/kernel-check-j2.log
  HARNESS_AUTORUN_EXPECTED=1
  start_qemu "$smp" tcg
  selftest_validate_autorun "$serial" \
    "artifacts/build/cl13-negative-${id}-reset" \
    accounting.sleep.minimum_deadline \
    accounting.sleep.one_ms_granularity \
    accounting.sleep.late_is_diagnostic \
    accounting.sleep.wait_result \
    accounting.sleep.percentiles
  if [[ $id == sync-old-sem-gap ]]; then
    framed_synctest_async BOUNDARY "$positive_command" \
      "[SYNC][BOUNDARY] START" "$positive_marker" "$timeout" stress
  else
    positive_canary "$positive_command" "$positive_marker" "$timeout"
  fi
  if [[ $id == sync-old-sem-gap ]]; then
    framed_synctest_async BOUNDARY_NOISE \
      "synctest boundary-noise 1000 1000" \
      "[SYNC][BOUNDARY_NOISE] START" \
      "[SYNC][BOUNDARY_NOISE] PASS" 300 stress
  fi
  positive_canary "taskmantest fixture-status" \
    "[TASKMANTEST][FIXTURE_STATUS] PASS max=257 active=0 present=0" 90
  transport_balanced
  cp "$serial" "$positive_log"
  assert_clean_log "$positive_log"
  stop_qemu
  local contract=COMMAND
  [[ $id == sync-old-sem-gap ]] && contract=ASYNC
  checkpoint_negative "$id" "$macro" "$contract" "$marker" \
    "$negative_log" "$positive_log"
  echo "[CL13][NEGATIVE] PASS id=$id detected=1 missed=0 cleanup=1 rebuild=1"
}

negative_scheduler_stale_slot(){
  local negative_log=artifacts/build/cl13-negative-scheduler-stale-slot.log
  local positive_log=artifacts/build/cl13-negative-scheduler-stale-slot-reset.log
  echo "[CL13][NEGATIVE] BEGIN id=scheduler-stale-slot macro=HOBBYOS_SCHED_NEGATIVE_STALE_SLOT_CAPTURE"
  make kernel-check JOBS=2 SELFTEST=1 SELFTEST_AUTORUN=0 \
    KERNEL_EXTRA_CFLAGS="-DHOBBYOS_SCHED_NEGATIVE_STALE_SLOT_CAPTURE" >/dev/null
  make image SELFTEST=1 SELFTEST_AUTORUN=0 \
    KERNEL_EXTRA_CFLAGS="-DHOBBYOS_SCHED_NEGATIVE_STALE_SLOT_CAPTURE" >/dev/null
  HARNESS_AUTORUN_EXPECTED=0
  start_qemu 4 tcg
  framed_schedtest_async_negative "schedtest entry-window 200000" \
    "[SCHED][ENTRY_WINDOW] START" \
    "[SCHED][NEGATIVE] STALE_SLOT_DETECTED" 900 stress "0 1"
  ! grep -Fq 'MISSED' "$serial"
  ! grep -Eq 'PANIC|#PF|#GP|FATAL|STRUCTURAL_FAULT|QEMU exited' "$serial"
  cp "$serial" "$negative_log"
  stop_qemu

  make kernel-check JOBS=2 SELFTEST=1 SELFTEST_AUTORUN=1 >/dev/null
  make image SELFTEST=1 SELFTEST_AUTORUN=1 >/dev/null
  ! grep -Fq HOBBYOS_SCHED_NEGATIVE_STALE_SLOT_CAPTURE \
    artifacts/build/kernel-check-j2.log
  HARNESS_AUTORUN_EXPECTED=1
  start_qemu 4 tcg
  selftest_validate_autorun "$serial" \
    "artifacts/build/cl13-negative-scheduler-stale-slot-reset" \
    accounting.sleep.minimum_deadline \
    accounting.sleep.one_ms_granularity \
    accounting.sleep.late_is_diagnostic \
    accounting.sleep.wait_result \
    accounting.sleep.percentiles
  framed_schedtest_async CPU_PIN "schedtest cpu-pin 20000" \
    "[SCHED][CPU_PIN] START" "[SCHED][CPU_PIN] PASS" 900 stress
  positive_canary "schedtest async-status $HARNESS_ASYNC_LAST_RUN" \
    "[SCHED][ASYNC_STATUS] PASS run=$HARNESS_ASYNC_LAST_RUN kind=CPU_PIN state=PASS" 90
  positive_canary "taskmantest fixture-status" \
    "[TASKMANTEST][FIXTURE_STATUS] PASS max=257 active=0 present=0" 90
  transport_balanced
  cp "$serial" "$positive_log"
  assert_clean_log "$positive_log"
  stop_qemu
  checkpoint_negative scheduler-stale-slot \
    HOBBYOS_SCHED_NEGATIVE_STALE_SLOT_CAPTURE ASYNC \
    "[SCHED][NEGATIVE] STALE_SLOT_DETECTED" "$negative_log" "$positive_log"
  echo "[CL13][NEGATIVE] PASS id=scheduler-stale-slot detected=1 missed=0 cleanup=1 rebuild=1"
}

negative_boot_one(){
  local id=$1 macro=$2 smp=$3 boot_marker=$4 health_command=$5
  local health_marker=$6 timeout=${7:-180}
  local negative_log="artifacts/build/cl13-negative-${id}.log"
  local reset_log="artifacts/build/cl13fix11-hpet-positive-reset.log"
  local canonical_reset="artifacts/build/cl13-negative-${id}-reset.log"
  local marker_line runtime_line test_line begin_line health_line end_line

  echo "[CL13][BOOT_NEGATIVE] BEGIN id=$id macro=$macro"
  make kernel-check JOBS=2 SELFTEST=1 SELFTEST_AUTORUN=0 \
    KERNEL_EXTRA_CFLAGS="-D$macro" >/dev/null
  make image SELFTEST=1 SELFTEST_AUTORUN=0 \
    KERNEL_EXTRA_CFLAGS="-D$macro" >/dev/null
  grep -Fq -- "-D$macro" artifacts/build/kernel-check-j2.log
  HARNESS_AUTORUN_EXPECTED=0
  start_qemu "$smp" tcg
  runtime_validate_boot_log "$serial" 0
  [[ $(grep -Fc "$boot_marker" "$serial") == 1 ]]
  ! grep -Fq 'MISSED' "$serial"
  ! grep -Eq 'PANIC|#PF|#GP|FATAL|STRUCTURAL_FAULT|FINISH_FAULT|QEMU exited' "$serial"
  marker_line=$(grep -nF "$boot_marker" "$serial" | head -1 | cut -d: -f1)
  runtime_line=$(grep -nF '[BOOT][RUNTIME_READY] PASS' "$serial" | head -1 | cut -d: -f1)
  test_line=$(grep -nF '[BOOT][TEST_READY] PASS autorun=0' "$serial" | head -1 | cut -d: -f1)
  ((marker_line < runtime_line && runtime_line < test_line))

  framed_send_complete "$health_command" "$health_marker" "$timeout" stress "0"
  [[ $HARNESS_FRAME_LAST_STATUS == 0 ]]
  begin_line=$(grep -nF "[HARNESS][BEGIN] seq=$HARNESS_FRAME_LAST_SEQUENCE" "$serial" |
    tail -1 | cut -d: -f1)
  health_line=$(grep -nF "$health_marker" "$serial" | tail -1 | cut -d: -f1)
  end_line=$(grep -nF "[HARNESS][END] seq=$HARNESS_FRAME_LAST_SEQUENCE status=0" "$serial" |
    tail -1 | cut -d: -f1)
  ((test_line < begin_line && begin_line < health_line && health_line < end_line))
  cp "$serial" "$negative_log"
  assert_clean_log "$negative_log"
  stop_qemu

  make kernel-check JOBS=2 SELFTEST=1 SELFTEST_AUTORUN=1 >/dev/null
  make image SELFTEST=1 SELFTEST_AUTORUN=1 >/dev/null
  ! grep -Fq "$macro" artifacts/build/kernel-check-j2.log
  HARNESS_AUTORUN_EXPECTED=1
  start_qemu 1 tcg
  selftest_validate_autorun "$serial" \
    "artifacts/build/cl13fix11-hpet-positive-reset" \
    accounting.sleep.minimum_deadline \
    accounting.sleep.one_ms_granularity \
    accounting.sleep.late_is_diagnostic \
    accounting.sleep.wait_result \
    accounting.sleep.percentiles
  grep -Fq '[CLOCK][SELFTEST] HPET_COUNTER_ACCESS_OK' "$serial"
  framed_send_complete "$health_command" "$health_marker" "$timeout" stress
  [[ $HARNESS_FRAME_LAST_STATUS == 0 ]]
  positive_canary "accounttest check" "[ACCOUNT][CHECK] PASS" 180
  positive_canary "taskdiag check" "[TASKDIAG][CHECK] PASS" 180
  transport_balanced
  cp "$serial" "$reset_log"
  cp "$serial" "$canonical_reset"
  assert_clean_log "$reset_log"
  stop_qemu
  checkpoint_negative "$id" "$macro" BOOT "$boot_marker" \
    "$negative_log" "$reset_log"
  echo "[CL13][BOOT_NEGATIVE] PASS id=$id marker_line=$marker_line runtime_line=$runtime_line test_line=$test_line begin_line=$begin_line health_line=$health_line end_line=$end_line health=PASS faults=0 reset=PASS"
}

negative_clock_sleep_diagnostic(){
  local id=clock-sleep-late-diagnostic
  local log="artifacts/build/cl13-negative-${id}.log"
  local marker='[SELFTEST][PASS] accounting.sleep.late_is_diagnostic severity=P0'
  echo "[CL13][COMMAND_NEGATIVE] BEGIN id=$id macro=none"
  HARNESS_AUTORUN_EXPECTED=1
  start_qemu 1 tcg
  selftest_validate_autorun "$serial" \
    "artifacts/build/cl13-negative-${id}" \
    accounting.sleep.minimum_deadline \
    accounting.sleep.one_ms_granularity \
    accounting.sleep.late_is_diagnostic \
    accounting.sleep.wait_result \
    accounting.sleep.percentiles
  send_complete "tasktest accounting" "[SELFTEST][SUMMARY]" 180 stress
  grep -Fq "$marker" "$serial"
  transport_balanced
  cp "$serial" "$log"
  assert_clean_log "$log"
  stop_qemu
  checkpoint_negative "$id" none COMMAND "$marker" "$log" "$log"
  echo "[CL13][COMMAND_NEGATIVE] PASS id=$id detected=1 missed=0 cleanup=1 rebuild=1"
}

run_case(){
  case "$1" in
    scheduler-stale-slot) negative_scheduler_stale_slot;;
    sync-old-sem-gap)
      negative_one "$1" HOBBYOS_SYNC_NEGATIVE_OLD_SEM_GAP 2 \
        "synctest boundary 5000" "[SYNC][NEGATIVE] OLD_SEM_GAP_DETECTED" \
        "synctest boundary 5000" "[SYNC][BOUNDARY] PASS" 300;;
    accounting-irq-ticks)
      negative_one "$1" HOBBYOS_ACCOUNT_NEGATIVE_IRQ_TICKS 1 \
        "accounttest busy 3000" "[ACCOUNT][NEGATIVE] IRQ_TICK_MODEL_DETECTED" \
        "accounttest busy 3000" "[ACCOUNT][BUSY] PASS" 300;;
    accounting-unsafe-muldiv)
      negative_one "$1" HOBBYOS_ACCOUNT_NEGATIVE_UNSAFE_MULDIV 1 \
        "accounttest long" "[ACCOUNT][NEGATIVE] UNSAFE_MULDIV_DETECTED" \
        "accounttest long" "[ACCOUNT][LONG] PASS" 120;;
    clock-global-only)
      negative_one "$1" HOBBYOS_CLOCK_NEGATIVE_GLOBAL_ONLY_REGRESSION 2 \
        "accounttest clock-classifier" "[CLOCK][NEGATIVE] GLOBAL_ONLY_FALSE_REGRESSION_DETECTED" \
        "accounttest clock-classifier" "[ACCOUNT][CLOCK_CLASSIFIER] PASS" 120;;
    clock-local-backward)
      negative_one "$1" HOBBYOS_CLOCK_NEGATIVE_LOCAL_BACKWARD 2 \
        "accounttest clock-classifier" "[CLOCK][NEGATIVE] LOCAL_SOURCE_REGRESSION_DETECTED" \
        "accounttest clock-classifier" "[ACCOUNT][CLOCK_CLASSIFIER] PASS" 120;;
    hpet-torn-read)
      negative_boot_one "$1" HOBBYOS_HPET_NEGATIVE_TORN_COUNTER_READ 1 \
        "[CLOCK][NEGATIVE] TORN_64BIT_COUNTER_READ_DETECTED" \
        "accounttest hpet-stats" "[ACCOUNT][HPET_STATS]" 120;;
    kill-ignore-killable)
      negative_one "$1" HOBBYOS_KILL_NEGATIVE_IGNORE_KILLABLE 4 \
        "killtest nonkillable" "[KILLTEST][NEGATIVE] NONKILLABLE_ACCEPTANCE_DETECTED" \
        "killtest nonkillable" "[KILLTEST][NONKILLABLE] PASS" 180;;
    kill-accept-exiting)
      negative_one "$1" HOBBYOS_KILL_NEGATIVE_ACCEPT_EXITING 4 \
        "killtest exiting-normal" "[KILLTEST][NEGATIVE] EXITING_ACCEPTED_NORMAL_DETECTED" \
        "killtest exiting-normal" "[KILLTEST][EXITING_NORMAL] PASS" 180;;
    reaper-ignore-timer-ref)
      negative_one "$1" HOBBYOS_REAPER_NEGATIVE_IGNORE_TIMER_REF 2 \
        "reaptest timer-ref" "[REAPTEST][NEGATIVE] TIMER_REF_IGNORED_DETECTED" \
        "reaptest timer-ref" "[REAPTEST][TIMER_REF] PASS" 240;;
    reaper-on-cpu)
      negative_one "$1" HOBBYOS_REAPER_NEGATIVE_ON_CPU 4 \
        "reaptest oncpu" "[REAPTEST][NEGATIVE] ON_CPU_REAP_DETECTED" \
        "reaptest oncpu" "[REAPTEST][ONCPU] PASS" 240;;
    reaper-duplicate-notification)
      negative_one "$1" HOBBYOS_REAPER_NEGATIVE_DUPLICATE_NOTIFICATION 2 \
        "reaptest negative" "[REAPTEST][NEGATIVE] DUPLICATE_NOTIFICATION_DETECTED" \
        "reaptest notification" "[REAPTEST][NOTIFY] PASS" 240;;
    input-phantom-permit)
      negative_one "$1" HOBBYOS_INPUT_NEGATIVE_PHANTOM_PERMIT 2 \
        "inputtest negative-phantom" "[INPUTTEST][NEGATIVE] PHANTOM_PERMIT_DETECTED" \
        "inputtest try-wait" "[INPUTTEST][TRY_WAIT] PASS" 180;;
    input-route-after-unlock)
      negative_one "$1" HOBBYOS_INPUT_NEGATIVE_ROUTE_AFTER_UNLOCK 4 \
        "inputtest negative-route" "[INPUTTEST][NEGATIVE] NONATOMIC_ROUTE_DETECTED" \
        "inputtest begin-boundary" "[INPUTTEST][BEGIN_BOUNDARY] PASS" 180;;
    modal-stale-token)
      negative_one "$1" HOBBYOS_MODAL_NEGATIVE_ACCEPT_STALE_TOKEN 4 \
        "modaltest stale-token" "[MODALTEST][NEGATIVE] STALE_TOKEN_ACCEPTED_DETECTED" \
        "modaltest stale-token" "[MODALTEST][STALE_TOKEN] PASS" 180;;
    modal-owner-recovery)
      negative_one "$1" HOBBYOS_MODAL_NEGATIVE_DISABLE_OWNER_RECOVERY 4 \
        "modaltest owner-exit" "[MODALTEST][NEGATIVE] OWNER_RECOVERY_MISSING_DETECTED" \
        "modaltest owner-exit" "[MODALTEST][OWNER_EXIT] PASS" 180;;
    wait-preblock)
      negative_one "$1" HOBBYOS_WAIT_NEGATIVE_IGNORE_PREBLOCK_CANCEL 2 \
        "synctest preblock-negative" "[SYNCTEST][NEGATIVE] PREBLOCK_CANCELLATION_LOST_DETECTED" \
        "synctest preblock-sem" "[SYNCTEST][PREBLOCK_SEM] PASS" 240;;
    taskman-render-all)
      negative_one "$1" HOBBYOS_TASKMAN_NEGATIVE_RENDER_ALL_ROWS 2 \
        "taskmantest model" "[TASKMANTEST][NEGATIVE] PANEL_OVERFLOW_DETECTED" \
        "taskmantest model" "[TASKMANTEST][MODEL] PASS" 120;;
    taskman-exit-any-key)
      negative_one "$1" HOBBYOS_TASKMAN_NEGATIVE_EXIT_ANY_KEY 2 \
        "taskmantest input" "[TASKMANTEST][NEGATIVE] NON_ESC_EXIT_DETECTED" \
        "taskmantest input" "[TASKMANTEST][INPUT] PASS" 120;;
    taskman-fixed-128)
      negative_one "$1" HOBBYOS_TASKMAN_NEGATIVE_FIXED_128 2 \
        "taskmantest model" "[TASKMANTEST][NEGATIVE] HIDDEN_TRUNCATION_DETECTED" \
        "taskmantest model" "[TASKMANTEST][MODEL] PASS" 120;;
    taskman-no-page-clamp)
      negative_one "$1" HOBBYOS_TASKMAN_NEGATIVE_NO_PAGE_CLAMP 2 \
        "taskmantest pagination" "[TASKMANTEST][NEGATIVE] PAGE_CLAMP_MISSING_DETECTED" \
        "taskmantest pagination" "[TASKMANTEST][PAGINATION] PASS" 120;;
    clock-sleep-late-diagnostic) negative_clock_sleep_diagnostic;;
    *) echo "unknown negative id: $1" >&2; return 2;;
  esac
}

negative_ids=(
  scheduler-stale-slot
  sync-old-sem-gap
  accounting-irq-ticks
  accounting-unsafe-muldiv
  clock-global-only
  clock-local-backward
  hpet-torn-read
  kill-ignore-killable
  kill-accept-exiting
  reaper-ignore-timer-ref
  reaper-on-cpu
  reaper-duplicate-notification
  input-phantom-permit
  input-route-after-unlock
  modal-stale-token
  modal-owner-recovery
  wait-preblock
  taskman-render-all
  taskman-exit-any-key
  taskman-fixed-128
  taskman-no-page-clamp
  clock-sleep-late-diagnostic
)

index_of(){
  local wanted=$1 i
  for ((i=0; i<${#negative_ids[@]}; i++)); do
    [[ ${negative_ids[i]} == "$wanted" ]] && { echo "$i"; return 0; }
  done
  return 1
}

mode=${1:-all}
selected=()
case "$mode" in
  list)
    printf '%s\n' "${negative_ids[@]}"
    trap - EXIT
    exit 0;;
  all)
    (($# == 0 || $# == 1)) || { echo "Usage: $0 all|hpet|from <id>|after <id>|list" >&2; exit 2; }
    selected=("${negative_ids[@]}");;
  hpet)
    (($# == 1)) || { echo "Usage: $0 hpet" >&2; exit 2; }
    selected=(hpet-torn-read);;
  from|after)
    (($# == 2)) || { echo "Usage: $0 $mode <id>" >&2; exit 2; }
    start=$(index_of "$2") || { echo "unknown negative id: $2" >&2; exit 2; }
    [[ $mode == from ]] || ((start+=1))
    ((start < ${#negative_ids[@]})) || { echo "no negatives selected" >&2; exit 2; }
    selected=("${negative_ids[@]:start}");;
  *)
    echo "Usage: $0 all|hpet|from <id>|after <id>|list" >&2
    exit 2;;
esac

harness_lock
for id in "${selected[@]}"; do
  run_case "$id"
done
harness_unlock
trap - EXIT

echo "[CL13][NEGATIVES] PASS selected=${#selected[@]} first=${selected[0]} last=${selected[-1]}"
