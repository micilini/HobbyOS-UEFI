#!/usr/bin/env bash
set -Eeuo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

artifact_dir=artifacts/build
serial=.qemu/qemu-serial.log
hmp=(python3 scripts/qemu_hmp.py --socket .qemu/hmp.sock)
harness_start_line=0
harness_command=modal-ownership-focused
summary=$artifact_dir/cl13fix14-modal-ownership-focused.log
ledger=$artifact_dir/cl13fix14-ledger.tsv
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
    printf 'stage\tscenario\tsmp\taccel\tstatus\townership\truns_delta\tworkers_delta\tnormal_delta\tcleanup_delta\tsignals_delta\tduplicates_delta\thandles_gone\ttimer_nodes_before\ttimer_nodes_after\tinternal_heap_direction\tinternal_heap_delta\touter_heap_drift\tartifact\thash\n' >"$ledger"
  fi
}

ledger_record(){
  local stage=$1 scenario=$2 smp=$3 status=$4 artifact=$5 hash=- accel=host
  ((smp == 0)) || accel=kvm
  [[ -f $artifact ]] && hash=$(sha256sum "$artifact" | awk '{print $1}')
  ensure_ledger
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$stage" "$scenario" "$smp" "$accel" "$status" \
    "${MODAL_OWNERSHIP:--}" "${MODAL_RUNS_DELTA:-0}" \
    "${MODAL_WORKERS_DELTA:-0}" "${MODAL_NORMAL_DELTA:-0}" \
    "${MODAL_CLEANUP_DELTA:-0}" "${MODAL_SIGNALS_DELTA:-0}" \
    "${MODAL_DUPLICATES_DELTA:-0}" "${MODAL_HANDLES_GONE:-0}" \
    "${MODAL_TIMER_NODES_BEFORE:-0}" "${MODAL_TIMER_NODES_AFTER:-0}" \
    "${MODAL_DIRECTION:--}" "${MODAL_SIGNED_DELTA:-0}" \
    "${OUTER_HEAP_DRIFT:--}" "$artifact" "$hash" >>"$ledger"
}

field(){
  local line=$1 name=$2
  [[ $line =~ (^|[[:space:]])${name}=([^[:space:]]+) ]] || return 1
  printf '%s' "${BASH_REMATCH[2]}"
}

parse_modal_record(){
  local file=$1 expected=${2:-1000} line expected_runs
  line=$(grep -E '^\[MODALTEST\]\[OPEN_CLOSE\] (PASS|FAIL) cycles=' \
    "$file" | tail -1)
  [[ $line =~ ^\[MODALTEST\]\[OPEN_CLOSE\][[:space:]]PASS[[:space:]]cycles=${expected}([[:space:]]|$) ]]
  [[ $(field "$line" cleanup) == "$expected" ]]
  MODAL_OWNERSHIP=$(field "$line" ownership)
  [[ $MODAL_OWNERSHIP == PASS ]]
  [[ $(field "$line" reason) == none ]]
  [[ $(field "$line" warmup_gone) == 1 ]]
  [[ $(field "$line" handles) == "$expected" ]]
  MODAL_HANDLES_GONE=$(field "$line" handles_gone)
  [[ $MODAL_HANDLES_GONE == "$expected" ]]
  [[ $(field "$line" first_live_id) == 0 ]]
  [[ $(field "$line" heap_scope) == GLOBAL_DIAGNOSTIC ]]
  [[ $(field "$line" heap_gate) == OUTER_SCENARIO ]]
  [[ $(field "$line" reaper_idle) == 1 ]]
  [[ $(field "$line" zombies) == 0 ]]
  [[ $(field "$line" free_inflight) == 0 ]]
  [[ $(field "$line" modal_violations) == 0 ]]
  [[ $(field "$line" session_violations) == 0 ]]
  expected_runs=$((expected + 1))
  MODAL_RUNS_DELTA=$(field "$line" runs_delta)
  MODAL_WORKERS_DELTA=$(field "$line" workers_delta)
  MODAL_NORMAL_DELTA=$(field "$line" normal_delta)
  MODAL_CLEANUP_DELTA=$(field "$line" cleanup_delta)
  MODAL_SIGNALS_DELTA=$(field "$line" signals_delta)
  MODAL_DUPLICATES_DELTA=$(field "$line" duplicates_delta)
  [[ $MODAL_RUNS_DELTA == "$expected_runs" ]]
  [[ $MODAL_WORKERS_DELTA == "$expected_runs" ]]
  [[ $MODAL_NORMAL_DELTA == "$expected_runs" ]]
  [[ $(field "$line" killed_delta) == 0 ]]
  [[ $MODAL_CLEANUP_DELTA == "$expected_runs" ]]
  [[ $MODAL_SIGNALS_DELTA == "$expected_runs" ]]
  [[ $MODAL_DUPLICATES_DELTA == 0 ]]
  [[ $(field "$line" alloc_fail_delta) == 0 ]]
  [[ $(field "$line" create_fail_delta) == 0 ]]
  [[ $(field "$line" begin_fail_delta) == 0 ]]
  [[ $(field "$line" recovery_delta) == 0 ]]
  [[ $(field "$line" second_session_delta) == 0 ]]
  [[ $(field "$line" contexts_live) == 0 ]]
  [[ $(field "$line" contexts_quarantined) == 0 ]]
  MODAL_DIRECTION=$(field "$line" direction)
  [[ $MODAL_DIRECTION == ZERO || $MODAL_DIRECTION == UP || $MODAL_DIRECTION == DOWN ]]
  MODAL_SIGNED_DELTA=$(field "$line" signed_delta)
  MODAL_TIMER_NODES_BEFORE=$(field "$line" timer_nodes_before)
  MODAL_TIMER_NODES_AFTER=$(field "$line" timer_nodes_after)
}

parse_outer_heap(){
  local file=$1 line
  line=$(grep -E '^\[TASKMANTEST\]\[HEAP\] PASS ' "$file" | tail -1)
  OUTER_HEAP_DRIFT=$(field "$line" drift)
  [[ $OUTER_HEAP_DRIFT == 0 ]]
  [[ $(field "$line" baseline_used) == "$(field "$line" final_used)" ]]
  [[ $(field "$line" baseline_blocks) == "$(field "$line" final_blocks)" ]]
}

transport_balance(){
  local line
  framed_send_complete "tasktest transport-status" "[HARNESS][STATUS]" 90 stress
  line=$(grep -F '[HARNESS][STATUS]' "$serial" | tail -1)
  [[ $line =~ accepted=([0-9]+)[[:space:]]completed=([0-9]+) ]]
  [[ ${BASH_REMATCH[1]} == "${BASH_REMATCH[2]}" ]]
}

host_unit(){
  scripts/cl13-modal-ownership-checkpoint.py verify-fix13-focused \
    | tee -a "$summary"
  scripts/cl13-modal-ownership-checkpoint.py host-unit | tee -a "$summary"
  MODAL_OWNERSHIP=PASS
  MODAL_DIRECTION=HOST_UNIT
  OUTER_HEAP_DRIFT=0
  ledger_record modal-ownership-focused host-unit 0 PASS \
    "$artifact_dir/cl13fix14-modal-ownership-host-unit.log"
}

static_gate(){
  local jobs
  jobs=$(nproc)
  : >"$artifact_dir/cl13fix14-modal-ownership-static.log"
  bash -n scripts/test-cl13-modal-ownership-focused.sh
  python3 -m py_compile scripts/cl13-modal-ownership-checkpoint.py
  git diff --check
  ! rg -n 'signed_drift == 0|baseline_used_blocks == result.final_used_blocks' \
    kernel/src/shell/commands/cmd_modaltest.c
  rg -n 'stats_before|stats_after|runs_delta|workers_delta|heap_scope|OUTER_SCENARIO' \
    kernel/src/shell/commands/cmd_modaltest.c >/dev/null
  rg -n 'timers_get_stats|timer_nodes_before|timer_nodes_after' \
    kernel/src/shell/commands/cmd_modaltest.c >/dev/null
  make stack-check | tee -a "$artifact_dir/cl13fix14-modal-ownership-static.log"
  make kernel-check JOBS=2 SELFTEST=1 SELFTEST_AUTORUN=1 \
    | tee -a "$artifact_dir/cl13fix14-modal-ownership-static.log"
  cp kernel.elf /tmp/hobbyos-cl13fix14-focused-j2.elf
  make kernel-check JOBS="$jobs" SELFTEST=1 SELFTEST_AUTORUN=1 \
    | tee -a "$artifact_dir/cl13fix14-modal-ownership-static.log"
  cp kernel.elf /tmp/hobbyos-cl13fix14-focused-jN.elf
  cmp -s /tmp/hobbyos-cl13fix14-focused-j2.elf \
    /tmp/hobbyos-cl13fix14-focused-jN.elf
  sha256sum /tmp/hobbyos-cl13fix14-focused-j2.elf \
    /tmp/hobbyos-cl13fix14-focused-jN.elf \
    >"$artifact_dir/cl13fix14-modal-ownership-build.sha256"
  make image SELFTEST=1 SELFTEST_AUTORUN=1 \
    | tee -a "$artifact_dir/cl13fix14-modal-ownership-static.log"
  nm -u kernel.elf >"$artifact_dir/cl13fix14-modal-ownership-nm.log"
  [[ ! -s $artifact_dir/cl13fix14-modal-ownership-nm.log ]]
  rg -q 'stack-check: PASS' "$artifact_dir/cl13fix14-modal-ownership-static.log"
  rg -q '^violations=0$' artifacts/build/stack-usage-report.txt
  ! rg -n 'kernel/src/(core/selftest|shell/commands/cmd_modaltest)\.c:.*warning:' \
    "$artifact_dir/cl13fix14-modal-ownership-static.log"
  MODAL_OWNERSHIP=PASS
  MODAL_DIRECTION=STATIC
  OUTER_HEAP_DRIFT=0
  ledger_record modal-ownership-focused static 0 PASS \
    "$artifact_dir/cl13fix14-modal-ownership-static.log"
}

run_boot(){
  local smp=$1 scenario="smp${smp}-kvm"
  local artifact="$artifact_dir/cl13fix14-modal-${scenario}.log"
  start_qemu "$smp" kvm
  selftest_validate_autorun "$serial" \
    "$artifact_dir/cl13fix14-modal-${scenario}" \
    modal.open_close.heap_direction \
    modal.open_close.used_blocks \
    modal.open_close.zero_is_not_idle \
    modal.open_close.record_fit \
    modal.open_close.stats_delta \
    modal.open_close.global_heap_is_diagnostic \
    modal.open_close.outer_heap_required \
    modal.open_close.timer_node_noise
  framed_send_status_complete "taskmantest heap-begin" 90 stress 0 \
    "[TASKMANTEST][HEAP_BEGIN] PASS"
  framed_send_launch "smpstress $((smp * 2)) 0 1000" \
    "[SMP] smpstress spawning workers=$((smp * 2))" 90 stress
  framed_send_status_complete "modaltest open-close 1000" 1800 stress 0 \
    "[MODALTEST][OPEN_CLOSE] PASS cycles=1000"
  framed_send_status_complete "modaltest check" 90 stress 0 \
    "[MODALTEST][CHECK] PASS"
  framed_send_status_complete "taskdiag check" 180 stress 0 \
    "[TASKDIAG][CHECK] PASS"
  framed_send_status_complete "killtest smpstress-sweep" 900 stress 0 \
    "[SMP][KILL_SWEEP] PASS workers=$((smp * 2))"
  framed_send_status_complete "modaltest check" 90 stress 0 \
    "[MODALTEST][CHECK] PASS"
  framed_send_status_complete "taskdiag check" 180 stress 0 \
    "[TASKDIAG][CHECK] PASS"
  framed_send_status_complete "taskmantest heap-end" 300 stress 0 \
    "[TASKMANTEST][HEAP] PASS"
  transport_balance
  assert_clean_log "$serial"
  cp "$serial" "$artifact"
  parse_modal_record "$artifact" 1000
  parse_outer_heap "$artifact"
  grep -Eq '^\[SMP\] Task ' "$artifact"
  ledger_record modal-ownership-focused "$scenario" "$smp" PASS "$artifact"
  printf '[CL13][MODAL_OWNERSHIP_FOCUSED] PASS scenario=%s ownership=%s handles_gone=%s direction=%s signed_delta=%s outer_heap_drift=%s\n' \
    "$scenario" "$MODAL_OWNERSHIP" "$MODAL_HANDLES_GONE" \
    "$MODAL_DIRECTION" "$MODAL_SIGNED_DELTA" "$OUTER_HEAP_DRIFT" \
    | tee -a "$summary"
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
  run_boot "$smp"
  harness_unlock
}

classification(){
  local artifact="$artifact_dir/cl13fix14-modal-classification.log"
  grep -Fq '[CL13][MODAL_OWNERSHIP_OLD_RUN] PASS' \
    "$artifact_dir/cl13fix14-old-focused.log"
  parse_modal_record "$artifact_dir/cl13fix14-modal-smp4-kvm.log" 1000
  parse_outer_heap "$artifact_dir/cl13fix14-modal-smp4-kvm.log"
  parse_modal_record "$artifact_dir/cl13fix14-modal-smp8-kvm.log" 1000
  parse_outer_heap "$artifact_dir/cl13fix14-modal-smp8-kvm.log"
  printf '%s\n' \
    '[CL13][MODAL_HEAP_CLASSIFICATION]' \
    'GLOBAL_TIMER_NODE_NOISE_FALSE_REJECTION_CONFIRMED.' >"$artifact"
  tee -a "$summary" <"$artifact"
  MODAL_OWNERSHIP=PASS
  OUTER_HEAP_DRIFT=0
  ledger_record modal-ownership-focused classification 0 PASS "$artifact"
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
