#!/usr/bin/env bash
set -Eeuo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

artifact_dir=artifacts/build
serial=.qemu/qemu-serial.log
hmp=(python3 scripts/qemu_hmp.py --socket .qemu/hmp.sock)
harness_start_line=0
harness_command=modal-heap-focused
summary=$artifact_dir/cl13fix13-modal-focused.log
ledger=$artifact_dir/cl13fix13-ledger.tsv
main_ledger=$artifact_dir/cl13-ledger.tsv
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
    printf 'stage\tscenario\tsmp\taccel\tstatus\twarmup_id\twarmup_gone\thandles\thandles_gone\tbaseline_used\tfinal_used\tdirection\tsigned_delta\tbaseline_blocks\tfinal_blocks\tzombies\tfree_inflight\tartifact\thash\n' >"$ledger"
  fi
}

ledger_record(){
  local scenario=$1 smp=$2 status=$3 artifact=$4 hash=-
  [[ -f $artifact ]] && hash=$(sha256sum "$artifact" | awk '{print $1}')
  ensure_ledger
  printf 'modal-heap-focused\t%s\t%s\tkvm\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$scenario" "$smp" "$status" "${MODAL_WARMUP_ID:-0}" \
    "${MODAL_WARMUP_GONE:-0}" "${MODAL_HANDLES:-0}" \
    "${MODAL_HANDLES_GONE:-0}" "${MODAL_BASELINE_USED:-0}" \
    "${MODAL_FINAL_USED:-0}" "${MODAL_DIRECTION:--}" \
    "${MODAL_SIGNED_DELTA:-0}" "${MODAL_BASELINE_BLOCKS:-0}" \
    "${MODAL_FINAL_BLOCKS:-0}" "${MODAL_ZOMBIES:-0}" \
    "${MODAL_FREE_INFLIGHT:-0}" "$artifact" "$hash" >>"$ledger"
  printf 'modal-heap-focused\t%s\t%s\tkvm\tselftest\tmodaltest open-close 1000 warmup_id=%s warmup_gone=%s handles=%s handles_gone=%s baseline_used=%s final_used=%s direction=%s signed_delta=%s baseline_blocks=%s final_blocks=%s zombies=%s free_inflight=%s\t%s\t0\t%s\t%s\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t-\n' \
    "$scenario" "$smp" "${MODAL_WARMUP_ID:-0}" \
    "${MODAL_WARMUP_GONE:-0}" "${MODAL_HANDLES:-0}" \
    "${MODAL_HANDLES_GONE:-0}" "${MODAL_BASELINE_USED:-0}" \
    "${MODAL_FINAL_USED:-0}" "${MODAL_DIRECTION:--}" \
    "${MODAL_SIGNED_DELTA:-0}" "${MODAL_BASELINE_BLOCKS:-0}" \
    "${MODAL_FINAL_BLOCKS:-0}" "${MODAL_ZOMBIES:-0}" \
    "${MODAL_FREE_INFLIGHT:-0}" "$status" "$artifact" "$hash" \
    >>"$main_ledger"
}

parse_modal_record(){
  local file=$1 expected=${2:-1000} line
  line=$(grep -E '^\[MODALTEST\]\[OPEN_CLOSE\] (PASS|FAIL) cycles=' \
    "$file" | tail -1)
  [[ $line =~ ^\[MODALTEST\]\[OPEN_CLOSE\][[:space:]]PASS[[:space:]]cycles=([0-9]+) ]]
  [[ ${BASH_REMATCH[1]} == "$expected" ]]
  [[ $line =~ cleanup=([0-9]+) ]] && [[ ${BASH_REMATCH[1]} == "$expected" ]]
  [[ $line =~ warmup_id=([0-9]+) ]] && MODAL_WARMUP_ID=${BASH_REMATCH[1]}
  ((MODAL_WARMUP_ID > 0))
  [[ $line =~ warmup_gone=([01]) ]] && MODAL_WARMUP_GONE=${BASH_REMATCH[1]}
  [[ $MODAL_WARMUP_GONE == 1 ]]
  [[ $line =~ handles=([0-9]+) ]] && MODAL_HANDLES=${BASH_REMATCH[1]}
  [[ $MODAL_HANDLES == "$expected" ]]
  [[ $line =~ handles_gone=([0-9]+) ]] && MODAL_HANDLES_GONE=${BASH_REMATCH[1]}
  [[ $MODAL_HANDLES_GONE == "$expected" ]]
  [[ $line =~ first_live_id=([0-9]+) ]] && [[ ${BASH_REMATCH[1]} == 0 ]]
  [[ $line =~ baseline_used=([0-9]+) ]] && MODAL_BASELINE_USED=${BASH_REMATCH[1]}
  [[ $line =~ final_used=([0-9]+) ]] && MODAL_FINAL_USED=${BASH_REMATCH[1]}
  [[ $MODAL_BASELINE_USED == "$MODAL_FINAL_USED" ]]
  [[ $line =~ direction=(ZERO|UP|DOWN) ]] && MODAL_DIRECTION=${BASH_REMATCH[1]}
  [[ $MODAL_DIRECTION == ZERO ]]
  [[ $line =~ signed_delta=(-?[0-9]+) ]] && MODAL_SIGNED_DELTA=${BASH_REMATCH[1]}
  [[ $MODAL_SIGNED_DELTA == 0 ]]
  [[ $line =~ drift_abs=([0-9]+) ]] && [[ ${BASH_REMATCH[1]} == 0 ]]
  [[ $line =~ baseline_blocks=([0-9]+) ]] && MODAL_BASELINE_BLOCKS=${BASH_REMATCH[1]}
  [[ $line =~ final_blocks=([0-9]+) ]] && MODAL_FINAL_BLOCKS=${BASH_REMATCH[1]}
  [[ $MODAL_BASELINE_BLOCKS == "$MODAL_FINAL_BLOCKS" ]]
  [[ $line =~ heap_stable=([01]) ]] && [[ ${BASH_REMATCH[1]} == 1 ]]
  [[ $line =~ reaper_idle=([01]) ]] && [[ ${BASH_REMATCH[1]} == 1 ]]
  [[ $line =~ zombies=([0-9]+) ]] && MODAL_ZOMBIES=${BASH_REMATCH[1]}
  [[ $MODAL_ZOMBIES == 0 ]]
  [[ $line =~ free_inflight=([0-9]+) ]] && MODAL_FREE_INFLIGHT=${BASH_REMATCH[1]}
  [[ $MODAL_FREE_INFLIGHT == 0 ]]
  [[ $line =~ modal_violations=([0-9]+) ]] && [[ ${BASH_REMATCH[1]} == 0 ]]
  [[ $line =~ session_violations=([0-9]+) ]] && [[ ${BASH_REMATCH[1]} == 0 ]]
}

transport_balance(){
  local line
  framed_send_complete "tasktest transport-status" "[HARNESS][STATUS]" 90 stress
  line=$(grep -F '[HARNESS][STATUS]' "$serial" | tail -1)
  [[ $line =~ accepted=([0-9]+)[[:space:]]completed=([0-9]+) ]]
  [[ ${BASH_REMATCH[1]} == "${BASH_REMATCH[2]}" ]]
}

host_unit(){
  scripts/cl13-modal-checkpoint.py verify-fix12-modal-failure | tee -a "$summary"
  scripts/cl13-modal-checkpoint.py host-unit | tee -a "$summary"
  MODAL_DIRECTION=HOST_UNIT
  ledger_record host-unit 0 PASS \
    "$artifact_dir/cl13fix13-modal-heap-host-unit.log"
}

static_gate(){
  local jobs
  jobs=$(nproc)
  : >"$artifact_dir/cl13fix13-modal-static.log"
  bash -n scripts/test-cl13-modal-heap-focused.sh
  python3 -m py_compile scripts/cl13-modal-checkpoint.py
  git diff --check
  make stack-check | tee -a "$artifact_dir/cl13fix13-modal-static.log"
  make kernel-check JOBS=2 SELFTEST=1 SELFTEST_AUTORUN=1 \
    | tee -a "$artifact_dir/cl13fix13-modal-static.log"
  cp kernel.elf /tmp/hobbyos-cl13fix13-focused-j2.elf
  make kernel-check JOBS="$jobs" SELFTEST=1 SELFTEST_AUTORUN=1 \
    | tee -a "$artifact_dir/cl13fix13-modal-static.log"
  cp kernel.elf /tmp/hobbyos-cl13fix13-focused-jN.elf
  cmp -s /tmp/hobbyos-cl13fix13-focused-j2.elf \
    /tmp/hobbyos-cl13fix13-focused-jN.elf
  sha256sum /tmp/hobbyos-cl13fix13-focused-j2.elf \
    /tmp/hobbyos-cl13fix13-focused-jN.elf \
    >"$artifact_dir/cl13fix13-modal-focused-build.sha256"
  make image SELFTEST=1 SELFTEST_AUTORUN=1 \
    | tee -a "$artifact_dir/cl13fix13-modal-static.log"
  [[ -z $(nm -u kernel.elf) ]]
  rg -q 'stack-check: PASS' "$artifact_dir/cl13fix13-modal-static.log"
  rg -q '^violations=0$' artifacts/build/stack-usage-report.txt
  ! rg -n 'kernel/src/(core/selftest|shell/commands/cmd_modaltest)\.c:.*warning:' \
    "$artifact_dir/cl13fix13-modal-static.log"
  ledger_record static 0 PASS "$artifact_dir/cl13fix13-modal-static.log"
}

run_boot(){
  local smp=$1 run=$2 scenario="smp${smp}-kvm-run${run}"
  local artifact="$artifact_dir/cl13fix13-modal-${scenario}.log"
  start_qemu "$smp" kvm
  selftest_validate_autorun "$serial" \
    "$artifact_dir/cl13fix13-modal-${scenario}" \
    modal.open_close.heap_direction \
    modal.open_close.used_blocks \
    modal.open_close.zero_is_not_idle \
    modal.open_close.record_fit
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
  transport_balance
  assert_clean_log "$serial"
  cp "$serial" "$artifact"
  parse_modal_record "$artifact" 1000
  grep -Eq '^\[SMP\] Task ' "$artifact"
  ledger_record "$scenario" "$smp" PASS "$artifact"
  printf '[CL13][MODAL_HEAP_FOCUSED] PASS scenario=%s warmup_id=%s handles_gone=%s direction=%s signed_delta=%s\n' \
    "$scenario" "$MODAL_WARMUP_ID" "$MODAL_HANDLES_GONE" \
    "$MODAL_DIRECTION" "$MODAL_SIGNED_DELTA" | tee -a "$summary"
  stop_qemu
}

run_kvm_set(){
  local smp=$1 runs=$2
  [[ -r /dev/kvm && -w /dev/kvm ]] || {
    echo BLOCKED_BY_EXTERNAL_ENVIRONMENT >&2
    return 3
  }
  make image SELFTEST=1 SELFTEST_AUTORUN=1 >/dev/null
  harness_lock
  for ((run=1; run<=runs; run++)); do
    run_boot "$smp" "$run"
  done
  harness_unlock
}

classification(){
  local artifact="$artifact_dir/cl13fix13-modal-classification.log"
  grep -Fq '[CL13][MODAL_OLD_RUN_AUDIT] PASS' \
    "$artifact_dir/cl13fix13-modal-old-run.log"
  for run in 1 2; do
    parse_modal_record "$artifact_dir/cl13fix13-modal-smp4-kvm-run${run}.log"
  done
  for run in 1 2 3; do
    parse_modal_record "$artifact_dir/cl13fix13-modal-smp8-kvm-run${run}.log"
  done
  printf '%s\n' \
    '[CL13][MODAL_HEAP_CLASSIFICATION]' \
    'WARMUP_BASELINE_CONTAMINATION_CONFIRMED.' >"$artifact"
  tee -a "$summary" <"$artifact"
  MODAL_DIRECTION=ZERO
  ledger_record classification 0 PASS "$artifact"
}

case ${1:-} in
  host-unit) host_unit;;
  static) static_gate;;
  smp4-kvm) run_kvm_set 4 2;;
  smp8-kvm) run_kvm_set 8 3;;
  classification) classification;;
  all)
    : >"$summary"
    host_unit
    static_gate
    run_kvm_set 4 2
    run_kvm_set 8 3
    classification
    ;;
  *) usage;;
esac
