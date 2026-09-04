#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd); cd "$root"
serial=.qemu/qemu-serial.log
hmp=(python3 scripts/qemu_hmp.py --socket .qemu/hmp.sock)
mkdir -p artifacts/build
harness_start_line=0; harness_command=boot
source scripts/harness-common.sh
harness_lock
cleanup(){ stop_qemu; }
trap cleanup EXIT

run_common(){
  send_complete "inputtest preblock-cancel" \
    "[INPUTTEST][PREBLOCK_CANCEL] PASS" 60
  send_complete "inputtest preblock-loop 100" \
    "[INPUTTEST][PREBLOCK_LOOP] PASS count=100" 300
  send_complete "inputtest queue" "[INPUTTEST][QUEUE] PASS" 60
  send_complete "inputtest try-wait" "[INPUTTEST][TRY_WAIT] PASS" 60
  send_complete "inputtest drain" "[INPUTTEST][DRAIN] PASS" 60
  send_complete "inputtest full" "[INPUTTEST][FULL] PASS" 60
  send_complete "inputtest mixed" "[INPUTTEST][MIXED] PASS" 60
  send_complete "inputtest check" "[INPUTTEST][CHECK] PASS" 60
  send_complete "schedtest check" "[SCHED][CHECK] PASS" 60
  send_complete "synctest check" "[SYNC][CHECK] PASS" 60
  send_complete "accounttest check" "[ACCOUNT][CHECK] PASS" 60
  send_complete "killtest check" "[KILLTEST][CHECK] PASS" 60
  send_complete "reaptest check" "[REAPTEST][CHECK] PASS" 60
}

positive(){
  local smp
  make image >/dev/null
  for smp in 1 2 4 8; do
    start_qemu "$smp" tcg
    run_common
    if ((smp==1 || smp==8)); then
      send_complete "inputtest preblock-loop 1000" \
        "[INPUTTEST][PREBLOCK_LOOP] PASS count=1000" 1800
    fi
    if ((smp>=2)); then
      send_complete "inputtest producers" "[INPUTTEST][PRODUCERS] PASS" 180
    fi
    if ((smp>=4)); then
      send_complete "inputtest begin-boundary" "[INPUTTEST][BEGIN_BOUNDARY] PASS" 60
      send_complete "inputtest end-boundary" "[INPUTTEST][END_BOUNDARY] PASS" 60
      send_complete "inputtest end-rollback" "[INPUTTEST][END_ROLLBACK] PASS" 60
      send_complete "inputtest shell-pause-boundary" \
        "[INPUTTEST][SHELL_PAUSE_BOUNDARY] PASS" 60
    fi
    if ((smp==4)); then
      send_complete "inputtest transitions 10000" "[INPUTTEST][TRANSITIONS] PASS cycles=10000" 180
      send_complete "inputtest keyboard" "[INPUTTEST][KEYBOARD] PASS" 180
      modal_cycle_complete
    fi
    cp "$serial" "artifacts/build/cl09fix-smp${smp}.log"
    assert_clean_log "artifacts/build/cl09fix-smp${smp}.log"
    stop_qemu
  done
}

leakage(){
  start_qemu 4 tcg
  local begin end leaked after
  begin=$(count "[MODAL] session_begin OK"); end=$(count "[MODAL] session_end OK")
  hmp_text taskman normal --enter >/dev/null
  wait_new "[MODAL] session_begin OK" "$begin" 30
  hmp_key esc normal >/dev/null
  hmp_text "inputtest marker leaked" normal --enter >/dev/null
  wait_new "[MODAL] session_end OK" "$end" 30
  shell_sync
  leaked=$(count "[INPUTTEST][MARKER] name=leaked")
  send_complete "inputtest marker after" "[INPUTTEST][MARKER] name=after" 30
  after=$(count "[INPUTTEST][MARKER] name=after")
  [[ $leaked == 0 && $after -ge 1 ]]
  echo "[INPUTTEST][LEAKAGE] PASS esc_leaked=0 rapid_text_leaked=0 after_delivered=1" >>"$serial"
  cp "$serial" artifacts/build/cl09fix-leakage.log
  stop_qemu
}

negative_one(){
  local macro=$1 command=$2 sentinel=$3 artifact=$4
  make kernel-check JOBS=2 KERNEL_EXTRA_CFLAGS="-D$macro" >/dev/null
  make image KERNEL_EXTRA_CFLAGS="-D$macro" >/dev/null
  start_qemu 2 tcg
  send_raw_complete "$command" "$sentinel" 60
  cp "$serial" "artifacts/build/$artifact"
  stop_qemu
  make kernel-check JOBS=2 >/dev/null
  make image >/dev/null
}

negatives(){
  negative_one HOBBYOS_INPUT_NEGATIVE_PHANTOM_PERMIT "inputtest negative-phantom" \
    "[INPUTTEST][NEGATIVE] PHANTOM_PERMIT_DETECTED" cl09fix-negative-phantom.log
  negative_one HOBBYOS_INPUT_NEGATIVE_ROUTE_AFTER_UNLOCK "inputtest negative-route" \
    "[INPUTTEST][NEGATIVE] NONATOMIC_ROUTE_DETECTED" cl09fix-negative-route.log
  start_qemu 4 tcg
  send_complete "inputtest try-wait" "[INPUTTEST][TRY_WAIT] PASS" 60
  send_complete "inputtest drain" "[INPUTTEST][DRAIN] PASS" 60
  send_complete "inputtest begin-boundary" "[INPUTTEST][BEGIN_BOUNDARY] PASS" 60
  send_complete "inputtest end-boundary" "[INPUTTEST][END_BOUNDARY] PASS" 60
  stop_qemu
}

cl08_stabilization(){
  local run
  make image >/dev/null
  for run in 1 2 3; do
    start_qemu 1 tcg
    send_complete "reaptest concurrent-churn 100 100 100" \
      "[REAPTEST][CONCURRENT_CHURN] PASS" 240
    rg -q 'forced_generation_changes=[1-9][0-9]*' "$serial"
    cp "$serial" "artifacts/build/cl09fix-cl08-smp1-run${run}.log"
    assert_clean_log "artifacts/build/cl09fix-cl08-smp1-run${run}.log"
    stop_qemu
  done
  for run in 1 2 3; do
    start_qemu 8 tcg
    send_complete "reaptest grace" "[REAPTEST][GRACE] PASS" 90
    rg -q 'g3000_after=1 g3000_actor=(background|manual)' "$serial"
    cp "$serial" "artifacts/build/cl09fix-cl08-smp8-run${run}.log"
    assert_clean_log "artifacts/build/cl09fix-cl08-smp8-run${run}.log"
    stop_qemu
  done
}

soak(){
  local duration=${DURATION_MS:-180000} start_ms cycles=0
  ((duration>=180000))
  start_qemu 8 tcg
  start_ms=$(( $(date +%s%N) / 1000000 ))
  while (( $(date +%s%N) / 1000000 - start_ms < duration )); do
    send_complete "inputtest producers" "[INPUTTEST][PRODUCERS] PASS" 180
    send_complete "inputtest drain" "[INPUTTEST][DRAIN] PASS" 60
    send_complete "inputtest begin-boundary" "[INPUTTEST][BEGIN_BOUNDARY] PASS" 60
    send_complete "inputtest end-boundary" "[INPUTTEST][END_BOUNDARY] PASS" 60
    send_complete "inputtest shell-pause-boundary" \
      "[INPUTTEST][SHELL_PAUSE_BOUNDARY] PASS" 60
    send_complete "inputtest transitions 1000" "[INPUTTEST][TRANSITIONS] PASS cycles=1000" 120
    send_complete "inputtest keyboard" "[INPUTTEST][KEYBOARD] PASS" 180
    send_complete "inputtest full" "[INPUTTEST][FULL] PASS" 60
    cycles=$((cycles+1))
  done
  send_complete "inputtest boundary-loop 1000" \
    "[INPUTTEST][BOUNDARY_LOOP] PASS cycles=1000" 600
  local reaper_idle=0 reaper_line attempt
  for ((attempt=0; attempt<120; attempt++)); do
    send_complete "reaptest check" "[REAPTEST][CHECK] PASS" 180
    reaper_line=$(grep -F '[REAPTEST][CHECK] PASS' "$serial" | tail -1)
    if [[ $reaper_line == *'backlog=0'* ]]; then reaper_idle=1; break; fi
  done
  ((reaper_idle==1))
  local taskman_cycles=0
  while ((taskman_cycles<100)); do modal_cycle_complete; taskman_cycles=$((taskman_cycles+1)); done
  send_complete "inputtest check" "[INPUTTEST][CHECK] PASS" 60
  send_complete "reaptest concurrent-churn 250 250 250" "[REAPTEST][CONCURRENT_CHURN] PASS" 360
  send_complete "schedtest check" "[SCHED][CHECK] PASS" 60
  send_complete "synctest check" "[SYNC][CHECK] PASS" 60
  send_complete "accounttest check" "[ACCOUNT][CHECK] PASS" 60
  send_complete "killtest check" "[KILLTEST][CHECK] PASS" 60
  send_complete "reaptest check" "[REAPTEST][CHECK] PASS" 60
  echo "[CL09][SOAK] PASS duration_ms=$duration cycles=$cycles" >>"$serial"
  cp "$serial" artifacts/build/cl09fix-soak-smp8.log
  assert_clean_log artifacts/build/cl09fix-soak-smp8.log
  stop_qemu
}

case ${1:-all} in
  positive) positive;;
  leakage) leakage;;
  negatives) negatives;;
  stabilize-cl08) cl08_stabilization;;
  soak) soak;;
  all)
    positive
    leakage
    negatives
    cl08_stabilization
    # Nested certification scripts own the same .qemu namespace.  Hand the
    # advisory lock to them explicitly, then reacquire it for this soak.
    exec 9>&-
    scripts/test-cl07-matrix.sh
    scripts/test-cl08.sh all
    harness_lock
    soak
    echo "[CL09][CERTIFICATION] PASS"
    ;;
  *) echo "usage: $0 positive|leakage|negatives|stabilize-cl08|soak|all" >&2; exit 2;;
esac
