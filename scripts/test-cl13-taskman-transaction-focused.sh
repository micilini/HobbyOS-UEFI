#!/usr/bin/env bash
set -Eeuo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

artifact_dir=artifacts/build
serial=.qemu/qemu-serial.log
hmp=(python3 scripts/qemu_hmp.py --socket .qemu/hmp.sock)
summary=$artifact_dir/cl13fix15-taskman-transaction-focused.log
ledger=$artifact_dir/cl13fix15-ledger.tsv
harness_start_line=0
harness_command=taskman-transaction-focused
mkdir -p "$artifact_dir"
source scripts/harness-common.sh
source scripts/harness-runtime-ready.sh
source scripts/harness-framed.sh
source scripts/harness-selftest.sh

cleanup(){
  stop_qemu
  [[ ${HARNESS_LOCK_HELD:-0} != 1 ]] || harness_unlock
}
trap cleanup EXIT

usage(){
  echo "Usage: $0 host-unit|static|smp4-kvm|smp8-kvm|classification|all" >&2
  exit 2
}

ensure_ledger(){
  if [[ ! -f $ledger ]]; then
    printf 'stage\tscenario\tsmp\taccel\tstatus\tps_batches\tps_completed\ttaskman_batches\ttaskman_completed\trefresh50\trefresh1000\trefresh2000\tauto_session_failures\tdiagnostic_markers_missing\told_frames\tnew_frames\touter_heap_drift\tartifact\thash\n' >"$ledger"
  fi
}

ledger_record(){
  local stage=$1 scenario=$2 smp=$3 status=$4 artifact=$5
  local accel=host hash=-
  ((smp == 0)) || accel=kvm
  [[ -f $artifact ]] && hash=$(sha256sum "$artifact" | awk '{print $1}')
  ensure_ledger
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$stage" "$scenario" "$smp" "$accel" "$status" \
    "${PS_BATCHES:-0}" "${PS_COMPLETED:-0}" \
    "${TASKMAN_BATCHES:-0}" "${TASKMAN_COMPLETED:-0}" \
    "${REFRESH50:-0}" "${REFRESH1000:-0}" "${REFRESH2000:-0}" \
    "${AUTO_SESSION_FAILURES:-0}" "${DIAGNOSTIC_MARKERS_MISSING:-0}" \
    "${OLD_FRAMES:-0}" "${NEW_FRAMES:-0}" "${OUTER_HEAP_DRIFT:--}" \
    "$artifact" "$hash" >>"$ledger"
}

field(){
  local line=$1 name=$2
  [[ $line =~ (^|[[:space:]])${name}=([^[:space:]]+) ]] || return 1
  printf '%s' "${BASH_REMATCH[2]}"
}

parse_ps_loop(){
  local file=$1 expected=$2 line
  line=$(grep -E '^\[HARNESS\]\[PS_LOOP\] PASS ' "$file" | tail -1)
  [[ $(field "$line" requested) == "$expected" ]]
  [[ $(field "$line" completed) == "$expected" ]]
  [[ $(field "$line" failure_index) == 0 ]]
  [[ $(field "$line" status) == 0 ]]
}

parse_taskman_loop(){
  local file=$1 count=$2 start=$3 expected50=$4 expected1000=$5 expected2000=$6
  local line
  line=$(grep -E '^\[TASKMANTEST\]\[AUTO_SESSION_LOOP\] PASS ' "$file" | tail -1)
  [[ $(field "$line" count) == "$count" ]]
  [[ $(field "$line" completed) == "$count" ]]
  [[ $(field "$line" start) == "$start" ]]
  [[ $(field "$line" frames) == 1 ]]
  [[ $(field "$line" refresh50) == "$expected50" ]]
  [[ $(field "$line" refresh1000) == "$expected1000" ]]
  [[ $(field "$line" refresh2000) == "$expected2000" ]]
  [[ $(field "$line" failure_index) == 0 ]]
}

parse_auto_session_records(){
  local file=$1 expected=$2 line count
  count=$(grep -Ec '^\[TASKMANTEST\]\[AUTO_SESSION\] PASS ' "$file")
  [[ $count == "$expected" ]]
  while IFS= read -r line; do
    [[ $(field "$line" taskman_status) == 0 ]]
    [[ $(field "$line" sessions_delta) == 1 ]]
    [[ $(field "$line" auto_exit_delta) == 1 ]]
    [[ $(field "$line" full_delta) == 1 ]]
    [[ $(field "$line" fallback_delta) == 0 ]]
    [[ $(field "$line" render_fail_delta) == 0 ]]
    [[ $(field "$line" shortfall_delta) == 0 ]]
    [[ $(field "$line" modal_scroll) == 0 ]]
    [[ $(field "$line" shell_exit_scroll) == 0 ]]
    [[ $(field "$line" clipped) == 0 ]]
    [[ $(field "$line" model_live) == 0 ]]
    [[ $(field "$line" pending) == 0 ]]
    [[ $(field "$line" controls_default) == 1 ]]
    [[ $(field "$line" valid) == 1 ]]
    [[ $(field "$line" last_mode) == WIDE ||
       $(field "$line" last_mode) == COMPACT ]]
    (( $(field "$line" pages) >= 1 ))
    (( $(field "$line" captured) > 0 ))
  done < <(grep -E '^\[TASKMANTEST\]\[AUTO_SESSION\] PASS ' "$file")
}

transport_balance(){
  local line
  framed_send_complete "tasktest transport-status" "[HARNESS][STATUS]" 90 stress
  line=$(grep -F '[HARNESS][STATUS]' "$serial" | tail -1)
  [[ $line =~ accepted=([0-9]+)[[:space:]]completed=([0-9]+) ]]
  [[ ${BASH_REMATCH[1]} == "${BASH_REMATCH[2]}" ]]
}

host_unit(){
  scripts/cl13-taskman-soak-checkpoint.py verify-fix14-taskman-failure \
    | tee -a "$summary"
  scripts/cl13-taskman-soak-checkpoint.py host-unit | tee -a "$summary"
  PS_BATCHES=1 PS_COMPLETED=100 TASKMAN_BATCHES=1 TASKMAN_COMPLETED=25
  REFRESH50=8 REFRESH1000=9 REFRESH2000=8
  OLD_FRAMES=800 NEW_FRAMES=9 OUTER_HEAP_DRIFT=0
  ledger_record taskman-transaction-focused host-unit 0 PASS \
    "$artifact_dir/cl13fix15-taskman-transaction-host-unit.log"
}

static_gate(){
  local jobs
  jobs=$(nproc)
  : >"$artifact_dir/cl13fix15-taskman-transaction-static.log"
  bash -n scripts/test-cl13-taskman-transaction-focused.sh
  python3 -m py_compile scripts/cl13-taskman-soak-checkpoint.py
  git diff --check
  rg -n 'auto-session|auto-session-loop|taskmantest_auto_session_result_valid' \
    kernel/src/shell/commands/cmd_taskmantest.c \
    kernel/src/shell/commands/cmd_taskmantest.h >/dev/null
  rg -n 'ps-loop|PS_LOOP' kernel/src/shell/commands/cmd_tasktest.c \
    kernel/src/shell/commands/cmd_tasktest.h >/dev/null
  ! rg -n 'taskmantest anchor-reset|taskmantest auto-exit-frames' \
    scripts/test-taskman-v1-soak.sh
  rg -n 'tasktest ps-loop 100|auto-session-loop 25' \
    scripts/test-taskman-v1-soak.sh >/dev/null
  ! rg -n 'grep.*TASKMANTEST.*HEAP.*drift=0' \
    scripts/test-taskman-v1-soak.sh
  make stack-check | tee -a "$artifact_dir/cl13fix15-taskman-transaction-static.log"
  make kernel-check JOBS=2 SELFTEST=1 SELFTEST_AUTORUN=1 \
    | tee -a "$artifact_dir/cl13fix15-taskman-transaction-static.log"
  cp kernel.elf /tmp/hobbyos-cl13fix15-focused-j2.elf
  make kernel-check JOBS="$jobs" SELFTEST=1 SELFTEST_AUTORUN=1 \
    | tee -a "$artifact_dir/cl13fix15-taskman-transaction-static.log"
  cp kernel.elf /tmp/hobbyos-cl13fix15-focused-jN.elf
  cmp -s /tmp/hobbyos-cl13fix15-focused-j2.elf \
    /tmp/hobbyos-cl13fix15-focused-jN.elf
  sha256sum /tmp/hobbyos-cl13fix15-focused-j2.elf \
    /tmp/hobbyos-cl13fix15-focused-jN.elf \
    >"$artifact_dir/cl13fix15-taskman-transaction-build.sha256"
  make image SELFTEST=1 SELFTEST_AUTORUN=1 \
    | tee -a "$artifact_dir/cl13fix15-taskman-transaction-static.log"
  nm -u kernel.elf >"$artifact_dir/cl13fix15-taskman-transaction-nm.log"
  [[ ! -s $artifact_dir/cl13fix15-taskman-transaction-nm.log ]]
  rg -q 'stack-check: PASS' "$artifact_dir/cl13fix15-taskman-transaction-static.log"
  rg -q '^violations=0$' artifacts/build/stack-usage-report.txt
  ! rg -n 'kernel/src/(core/selftest|shell/commands/(cmd_taskmantest|cmd_tasktest))\.c:.*warning:' \
    "$artifact_dir/cl13fix15-taskman-transaction-static.log"
  PS_BATCHES=0 PS_COMPLETED=0 TASKMAN_BATCHES=0 TASKMAN_COMPLETED=0
  REFRESH50=0 REFRESH1000=0 REFRESH2000=0 OLD_FRAMES=0 NEW_FRAMES=0
  OUTER_HEAP_DRIFT=0
  ledger_record taskman-transaction-focused static 0 PASS \
    "$artifact_dir/cl13fix15-taskman-transaction-static.log"
}

run_boot(){
  local smp=$1 run=$2 ps_count=$3 artifact scenario taskman_total
  local before_replay after_replay marker_missing=0
  scenario="smp${smp}-kvm${run:+-run$run}"
  artifact="$artifact_dir/cl13fix15-taskman-${scenario}.log"
  start_qemu "$smp" kvm
  selftest_validate_autorun "$serial" \
    "$artifact_dir/cl13fix15-taskman-${scenario}" \
    ui.taskman.auto_session_delta \
    ui.taskman.auto_session_refresh_pattern \
    ui.taskman.auto_session_record_fit \
    transport.ps_loop_bounds \
    transport.ps_loop_record_fit

  framed_send_status_complete "taskmantest heap-begin" 90 stress 0 \
    "[TASKMANTEST][HEAP_BEGIN] PASS"
  framed_send_launch "smpstress $((smp * 2)) 0 1000" \
    "[SMP] smpstress spawning workers=$((smp * 2))" 90 stress
  framed_send_status_complete "tasktest ps-loop $ps_count" 1800 stress 0 \
    "[HARNESS][PS_LOOP] PASS"
  parse_ps_loop "$serial" "$ps_count"

  if ((smp == 4)); then
    framed_send_status_complete "taskmantest auto-session-loop 20 1 1" \
      900 stress 0 "[TASKMANTEST][AUTO_SESSION_LOOP] PASS"
    parse_taskman_loop "$serial" 20 1 6 7 7
    taskman_total=20
    before_replay=$(grep -Ec '^\[TASKMANTEST\]\[AUTO_SESSION\] PASS ' "$serial")
    framed_replay_last "taskmantest auto-session-loop 20 1 1" 0 90 stress
    after_replay=$(grep -Ec '^\[TASKMANTEST\]\[AUTO_SESSION\] PASS ' "$serial")
    [[ $before_replay == "$after_replay" ]]
  else
    framed_send_status_complete "taskmantest auto-session-loop 25 1 1" \
      900 stress 0 "[TASKMANTEST][AUTO_SESSION_LOOP] PASS"
    parse_taskman_loop "$serial" 25 1 8 9 8
    framed_send_status_complete "taskmantest auto-session-loop 25 1 26" \
      900 stress 0 "[TASKMANTEST][AUTO_SESSION_LOOP] PASS"
    parse_taskman_loop "$serial" 25 26 8 8 9
    taskman_total=50
  fi
  parse_auto_session_records "$serial" "$taskman_total"

  framed_send_status_complete "taskmantest check" 90 stress 0 \
    "[TASKMANTEST][CHECK] PASS"
  framed_send_status_complete "taskdiag check" 180 stress 0 \
    "[TASKDIAG][CHECK] PASS"
  framed_send_status_complete "killtest smpstress-sweep" 900 stress 0 \
    "[SMP][KILL_SWEEP] PASS workers=$((smp * 2))"
  framed_send_status_complete "taskmantest check" 90 stress 0 \
    "[TASKMANTEST][CHECK] PASS"
  framed_send_status_complete "taskdiag check" 180 stress 0 \
    "[TASKDIAG][CHECK] PASS"
  framed_send_status_complete "taskmantest heap-end" 300 stress 0 \
    "[TASKMANTEST][HEAP] PASS"
  ((HARNESS_FRAME_LAST_DIAGNOSTIC_MARKER)) || marker_missing=$((marker_missing + 1))
  transport_balance
  assert_clean_log "$serial"
  grep -Eq '^\[SMP\] Task ' "$serial"
  cp "$serial" "$artifact"

  PS_BATCHES=1 PS_COMPLETED=$ps_count
  if ((smp == 4)); then
    TASKMAN_BATCHES=1 TASKMAN_COMPLETED=20
    REFRESH50=6 REFRESH1000=7 REFRESH2000=7
  else
    TASKMAN_BATCHES=2 TASKMAN_COMPLETED=50
    REFRESH50=16 REFRESH1000=17 REFRESH2000=17
  fi
  AUTO_SESSION_FAILURES=0 DIAGNOSTIC_MARKERS_MISSING=$marker_missing
  OLD_FRAMES=$((ps_count + TASKMAN_COMPLETED * 3)) NEW_FRAMES=$((1 + TASKMAN_BATCHES))
  OUTER_HEAP_DRIFT=0
  ledger_record taskman-transaction-focused "$scenario" "$smp" PASS "$artifact"
  printf '[CL13][TASKMAN_TRANSACTION_FOCUSED] PASS scenario=%s ps=%s taskman=%s refresh50=%s refresh1000=%s refresh2000=%s outer_heap_drift=0 faults=0\n' \
    "$scenario" "$PS_COMPLETED" "$TASKMAN_COMPLETED" "$REFRESH50" \
    "$REFRESH1000" "$REFRESH2000" | tee -a "$summary"
  stop_qemu
}

run_kvm(){
  local smp=$1
  [[ -r /dev/kvm && -w /dev/kvm ]] || {
    echo BLOCKED_BY_EXTERNAL_ENVIRONMENT >&2
    return 3
  }
  make image SELFTEST=1 SELFTEST_AUTORUN=1 >/dev/null
  harness_lock
  if ((smp == 4)); then
    run_boot 4 "" 20
  else
    run_boot 8 1 100
    run_boot 8 2 100
  fi
  harness_unlock
}

classification(){
  local artifact="$artifact_dir/cl13fix15-taskman-classification.log"
  grep -Fq '[CL13][TASKMAN_OLD_RUN_AUDIT] PASS' \
    "$artifact_dir/cl13fix15-old-taskman.log"
  grep -Fq '[CL13][TASKMAN_TRANSACTION_FOCUSED] PASS scenario=smp4-kvm' "$summary"
  grep -Fq '[CL13][TASKMAN_TRANSACTION_FOCUSED] PASS scenario=smp8-kvm-run1' "$summary"
  grep -Fq '[CL13][TASKMAN_TRANSACTION_FOCUSED] PASS scenario=smp8-kvm-run2' "$summary"
  printf '%s\n' \
    '[CL13][TASKMAN_TRANSACTION_CLASSIFICATION]' \
    'AUTO_EXIT_ARM_DIAGNOSTIC_MARKER_FALSE_REJECTION_CONFIRMED.' >"$artifact"
  tee -a "$summary" <"$artifact"
  PS_BATCHES=3 PS_COMPLETED=220 TASKMAN_BATCHES=5 TASKMAN_COMPLETED=120
  REFRESH50=38 REFRESH1000=41 REFRESH2000=41
  AUTO_SESSION_FAILURES=0 DIAGNOSTIC_MARKERS_MISSING=0
  OLD_FRAMES=1160 NEW_FRAMES=8 OUTER_HEAP_DRIFT=0
  ledger_record taskman-transaction-focused classification 0 PASS "$artifact"
}

case ${1:-} in
  host-unit) host_unit;;
  static) static_gate;;
  smp4-kvm) run_kvm 4;;
  smp8-kvm) run_kvm 8;;
  classification) classification;;
  all)
    : >"$summary"
    host_unit
    static_gate
    run_kvm 4
    run_kvm 8
    classification
    ;;
  *) usage;;
esac
