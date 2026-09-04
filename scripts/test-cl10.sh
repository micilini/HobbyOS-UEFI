#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
serial=.qemu/qemu-serial.log
hmp=(python3 scripts/qemu_hmp.py --socket .qemu/hmp.sock)
mkdir -p artifacts/build
harness_start_line=0
harness_command=boot
source scripts/harness-common.sh
harness_lock
cleanup(){ stop_qemu; }
trap cleanup EXIT

common_checks(){
  send_complete "modaltest check" "[MODALTEST][CHECK] PASS" 60
  send_complete "inputtest check" "[INPUTTEST][CHECK] PASS" 60
  send_complete "schedtest check" "[SCHED][CHECK] PASS" 60
  send_complete "synctest check" "[SYNC][CHECK] PASS" 60
  send_complete "accounttest check" "[ACCOUNT][CHECK] PASS" 60
  send_complete "killtest check" "[KILLTEST][CHECK] PASS" 60
  send_complete "reaptest check" "[REAPTEST][CHECK] PASS" 60
}

clock_hpet(){
  exec 9>&-
  scripts/test-clock-rollover.sh all
  harness_lock
}

lapic_gate(){
  exec 9>&-
  scripts/test-lapic-rate.sh all
  harness_lock
}

positive(){
  local smp cycles
  make image >/dev/null
  for smp in 1 2 4 8; do
    case "$smp" in 1) cycles=250;; 2) cycles=500;; *) cycles=1000;; esac
    start_qemu "$smp" tcg
    send_complete "modaltest open-close $cycles" \
      "[MODALTEST][OPEN_CLOSE] PASS cycles=$cycles" 600
    send_complete "modaltest false-token" "[MODALTEST][FALSE_TOKEN] PASS" 60
    send_complete "modaltest stale-token" "[MODALTEST][STALE_TOKEN] PASS" 60
    send_complete "modaltest wrong-owner" "[MODALTEST][WRONG_OWNER] PASS" 60
    send_complete "modaltest input-token" "[MODALTEST][INPUT_TOKEN] PASS" 60
    send_complete "modaltest completion-once" "[MODALTEST][COMPLETION] PASS" 180
    send_complete "modaltest kill-before-begin" \
      "[MODALTEST][KILL_BEFORE_BEGIN] PASS" 60
    send_complete "modaltest idempotent-owner" \
      "[MODALTEST][IDEMPOTENT_OWNER] PASS" 60
    send_complete "modaltest kill-caller-blocked" \
      "[MODALTEST][KILL_CALLER_BLOCKED] PASS" 120
    send_complete "modaltest kill-caller-prewait" \
      "[MODALTEST][KILL_CALLER_PREWAIT] PASS" 120
    send_complete "modaltest kill-caller-completion-ready" \
      "[MODALTEST][KILL_CALLER_COMPLETION_READY] PASS" 120
    if ((smp>=2)); then
      send_complete "modaltest second-session" "[MODALTEST][SECOND] PASS" 60
      send_complete "modaltest shell-blocked" "[MODALTEST][SHELL_BLOCKED] PASS" 60
      send_complete "modaltest kill-owner" "[MODALTEST][KILL_OWNER] PASS" 60
      send_complete "modaltest owner-exit" "[MODALTEST][OWNER_EXIT] PASS" 60
      send_complete "modaltest reservation-gap" \
        "[MODALTEST][RESERVATION_GAP] PASS" 120
    fi
    send_complete "modaltest alloc-fail" "[MODALTEST][ALLOC_FAIL] PASS" 60
    send_complete "modaltest create-fail" "[MODALTEST][CREATE_FAIL] PASS" 60
    send_complete "modaltest router-begin-fail" "[MODALTEST][BEGIN_FAIL] PASS" 60
    send_complete "modaltest router-end-fail" "[MODALTEST][END_FAIL] PASS" 60
    if ((smp>=4)); then
      send_complete "modaltest concurrent-callers 10000" \
        "[MODALTEST][CONCURRENT_CALLERS] PASS rounds=10000" 1800
      send_complete "modaltest snapshot-race 100000" \
        "[MODALTEST][SNAPSHOT_RACE] PASS transitions=100000" 600
      send_complete "modaltest opening-boundary" \
        "[MODALTEST][OPENING_BOUNDARY] PASS" 120
      send_complete "modaltest closing-boundary" \
        "[MODALTEST][CLOSING_BOUNDARY] PASS" 120
      modal_cycle_complete
    fi
    common_checks
    cp "$serial" "artifacts/build/cl10fix-smp${smp}.log"
    assert_clean_log "artifacts/build/cl10fix-smp${smp}.log"
    stop_qemu
  done
}

exact_loops(){
  local smp mode sentinel
  make image >/dev/null
  for smp in 1 8; do
    for mode in blocked prewait completion-ready; do
      case "$mode" in
        blocked) sentinel=BLOCKED;;
        prewait) sentinel=PREWAIT;;
        completion-ready) sentinel=COMPLETION_READY;;
      esac
      start_qemu "$smp" tcg
      send_complete "modaltest kill-caller-loop 1000 $mode" \
        "[MODALTEST][KILL_CALLER_LOOP] PASS mode=$sentinel count=1000" 3600 stress
      cp "$serial" \
        "artifacts/build/cl10fix11-modal-${mode}-smp${smp}.log"
      assert_clean_log \
        "artifacts/build/cl10fix11-modal-${mode}-smp${smp}.log"
      stop_qemu
    done
  done
}

taskman(){
  make image >/dev/null
  start_qemu 4 tcg
  local begin end
  begin=$(count "[MODAL] session_begin OK")
  end=$(count "[MODAL] session_end OK")
  hmp_text taskman normal --enter >/dev/null
  wait_new "[MODAL] session_begin OK" "$begin" 30
  wait_new "[TASKMAN][OWNER_STATE] shell=BLOCKED session=ACTIVE" 0 30
  hmp_key esc normal >/dev/null
  wait_new "[MODAL] session_end OK" "$end" 30
  shell_sync
  cp "$serial" artifacts/build/cl10fix-taskman-normal.log
  assert_clean_log artifacts/build/cl10fix-taskman-normal.log
  stop_qemu

  start_qemu 4 tcg
  send_complete "modaltest arm-kill-next-ui" \
    "[MODALTEST][ARM_KILL_NEXT_UI] OK" 30
  hmp_text taskman normal --enter >/dev/null
  wait_new "[TASKMAN][OWNER]" 0 30
  wait_new "[MODAL] session_end OK" 0 30
  shell_sync
  cp "$serial" artifacts/build/cl10fix-taskman-killed.log
  assert_clean_log artifacts/build/cl10fix-taskman-killed.log
  stop_qemu
}

negative_one(){
  local macro=$1 command=$2 sentinel=$3 artifact=$4
  make kernel-check JOBS=2 KERNEL_EXTRA_CFLAGS="-D$macro" >/dev/null
  make image KERNEL_EXTRA_CFLAGS="-D$macro" >/dev/null
  start_qemu 4 tcg
  send_raw_complete "$command" "$sentinel" 120
  cp "$serial" "artifacts/build/$artifact"
  stop_qemu
  make kernel-check JOBS=2 >/dev/null
  make image >/dev/null
}

negatives(){
  negative_one HOBBYOS_MODAL_NEGATIVE_ACCEPT_STALE_TOKEN \
    "modaltest stale-token" \
    "[MODALTEST][NEGATIVE] STALE_TOKEN_ACCEPTED_DETECTED" \
    cl10-negative-stale-token.log
  negative_one HOBBYOS_MODAL_NEGATIVE_DISABLE_OWNER_RECOVERY \
    "modaltest owner-exit" \
    "[MODALTEST][NEGATIVE] OWNER_RECOVERY_MISSING_DETECTED" \
    cl10-negative-owner-recovery.log
  start_qemu 4 tcg
  send_complete "modaltest stale-token" "[MODALTEST][STALE_TOKEN] PASS" 60
  send_complete "modaltest owner-exit" "[MODALTEST][OWNER_EXIT] PASS" 60
  stop_qemu
}

soak(){
  local duration=${DURATION_MS:-180000} start_ms cycles=0 taskman_cycles=0
  ((duration>=180000))
  make image >/dev/null
  start_qemu 8 tcg
  send_complete "accounttest hpet-rollover 1" \
    "[ACCOUNT][HPET_ROLLOVER] PASS" 120
  send_complete "accounttest lapic-rate 2000 5" \
    "[ACCOUNT][LAPIC_RATE] PASS" 120
  # Exercise real HMP input before the high-volume synthetic task churn so a
  # serial backlog cannot delay ESC delivery or its completion sentinel.
  for _ in $(seq 1 10); do
    modal_cycle_complete
    taskman_cycles=$((taskman_cycles+1))
  done
  for _ in $(seq 1 90); do
    taskman_killed_cycle_complete
    taskman_cycles=$((taskman_cycles+1))
  done
  send_complete "modaltest stress 5000 1000 100 1000 1000 100" \
    "[MODALTEST][STRESS] PASS normal=5000 killed=1000 recovered=100 busy=1000 token=1000 failures=100" 7200
  send_complete "modaltest snapshot-race 100000" \
    "[MODALTEST][SNAPSHOT_RACE] PASS transitions=100000" 600
  send_complete "modaltest kill-caller-loop 100 blocked" \
    "[MODALTEST][KILL_CALLER_LOOP] PASS mode=BLOCKED count=100" 600 stress
  send_complete "modaltest kill-caller-loop 100 prewait" \
    "[MODALTEST][KILL_CALLER_LOOP] PASS mode=PREWAIT count=100" 600 stress
  send_complete "modaltest kill-caller-loop 100 completion-ready" \
    "[MODALTEST][KILL_CALLER_LOOP] PASS mode=COMPLETION_READY count=100" 600 stress
  start_ms=$(( $(date +%s%N) / 1000000 ))
  while (( $(date +%s%N) / 1000000 - start_ms < duration )); do
    send_complete "modaltest open-close 250" \
      "[MODALTEST][OPEN_CLOSE] PASS cycles=250" 300
    send_complete "modaltest false-token" "[MODALTEST][FALSE_TOKEN] PASS" 60
    send_complete "modaltest stale-token" "[MODALTEST][STALE_TOKEN] PASS" 60
    send_complete "modaltest wrong-owner" "[MODALTEST][WRONG_OWNER] PASS" 60
    send_complete "modaltest second-session" "[MODALTEST][SECOND] PASS" 60
    send_complete "modaltest kill-owner" "[MODALTEST][KILL_OWNER] PASS" 60
    send_complete "modaltest owner-exit" "[MODALTEST][OWNER_EXIT] PASS" 60
    send_complete "modaltest router-end-fail" "[MODALTEST][END_FAIL] PASS" 60
    cycles=$((cycles+1))
  done
  send_complete "modaltest check" "[MODALTEST][CHECK] PASS" 60
  send_complete "inputtest check" "[INPUTTEST][CHECK] PASS" 60
  send_complete "reaptest concurrent-churn 250 250 250" \
    "[REAPTEST][CONCURRENT_CHURN] PASS" 360
  send_complete "accounttest hpet-wrap-soak 180000" \
    "[ACCOUNT][HPET_WRAP_SOAK] PASS" 300
  send_complete "accounttest lapic-after-load" \
    "[ACCOUNT][LAPIC_AFTER_LOAD] PASS" 300
  send_complete "accounttest lapic-rate 2000 5" \
    "[ACCOUNT][LAPIC_RATE] PASS" 120
  common_checks
  echo "[CL10][SOAK] PASS duration_ms=$duration cycles=$cycles taskman_cycles=$taskman_cycles" >>"$serial"
  cp "$serial" artifacts/build/cl10fix2-soak-smp8.log
  assert_clean_log artifacts/build/cl10fix2-soak-smp8.log
  stop_qemu
}

case ${1:-all} in
  positive) positive;;
  exact) exact_loops;;
  taskman) taskman;;
  negatives) negatives;;
  soak) soak;;
  all)
    clock_hpet
    lapic_gate
    positive
    exact_loops
    taskman
    negatives
    exec 9>&-
    scripts/test-cl07-matrix.sh
    scripts/test-cl08.sh all
    scripts/test-cl09.sh all
    harness_lock
    soak
    echo "[CL10][CERTIFICATION] PASS"
    ;;
  *) echo "usage: $0 positive|exact|taskman|negatives|soak|all" >&2; exit 2;;
esac
