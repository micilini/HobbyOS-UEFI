#!/usr/bin/env bash
set -Eeuo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

artifact_dir=artifacts/build
ledger=$artifact_dir/cl13-ledger.tsv
serial=.qemu/qemu-serial.log
hmp=(python3 scripts/qemu_hmp.py --socket .qemu/hmp.sock)
harness_start_line=0
harness_command=async-focused
current_scenario=
current_log=
protected_before=$artifact_dir/cl13fix5-protected-runtime-before.sha256
protected_after=$artifact_dir/cl13fix5-protected-runtime-after-focused.sha256
mkdir -p "$artifact_dir"
source scripts/harness-common.sh
source scripts/harness-runtime-ready.sh
source scripts/harness-framed.sh
source scripts/harness-selftest.sh

usage(){
  echo "Usage: $0 static|host-unit|yield|cpu-pin|negative|matrix|all" >&2
  exit 2
}

now_ms(){ echo $(( $(date +%s%N) / 1000000 )); }

ensure_ledger(){
  if [[ ! -f $ledger ]]; then
    printf 'stage\tscenario\tsmp\taccel\tvariant\tcommand\tstatus\tduration_ms\tartifact\tsha256\n' >"$ledger"
  fi
}

ledger_record(){
  local scenario=$1 smp=$2 accel=$3 command=$4 status=$5 duration=$6 artifact=$7
  local hash=-
  [[ ! -f $artifact ]] || hash=$(sha256sum "$artifact" | awk '{print $1}')
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    async-contract-focused "$scenario" "$smp" "$accel" selftest \
    "$command" "$status" "$duration" "$artifact" "$hash" >>"$ledger"
  echo "[CL13][LEDGER] stage=async-contract-focused scenario=$scenario status=$status duration_ms=$duration artifact=$artifact"
}

ledger_latest_pass(){
  local scenario=$1 status artifact expected actual
  read -r status artifact expected < <(awk -F '\t' -v wanted="$scenario" '
    $1 == "async-contract-focused" && $2 == wanted {
      status=$7; artifact=$9; hash=$10
    }
    END { print status, artifact, hash }
  ' "$ledger")
  [[ $status == PASS && -f $artifact && $expected != - ]] || return 1
  actual=$(sha256sum "$artifact" | awk '{print $1}')
  [[ $actual == "$expected" ]]
}

protected_snapshot(){
  local output=$1
  { rg --files kernel bootloader shared; printf '%s\n' makefile; } |
    LC_ALL=C sort |
    rg -v '^(kernel/src/shell/commands/cmd_schedtest\.(c|h)|kernel/src/core/selftest\.c)$' |
    xargs sha256sum >"$output"
}

failure_trap(){
  local rc=$?
  set +e
  if [[ -n $current_log && -f $serial ]]; then
    cp "$serial" "${current_log%.log}-failed.log"
    ledger_record "${current_scenario:-unknown}" 0 unknown focused FAIL 0 \
      "${current_log%.log}-failed.log"
  fi
  stop_qemu
  harness_unlock 2>/dev/null || true
  exit "$rc"
}
trap failure_trap ERR
trap stop_qemu EXIT

static_gate(){
  local begin end jobs j2=/tmp/hobbyos-cl13fix5-selftest-j2.elf
  local jn=/tmp/hobbyos-cl13fix5-selftest-jN.elf
  current_scenario=static
  current_log=$artifact_dir/cl13fix5-static.log
  begin=$(now_ms)
  jobs=$(nproc)
  make stack-check JOBS=2
  make kernel-check JOBS=2 SELFTEST=1 SELFTEST_AUTORUN=1
  cp kernel.elf "$j2"
  cp artifacts/build/kernel-check-j2.log "$artifact_dir/cl13fix5-build-j2.log"
  make kernel-check JOBS="$jobs" SELFTEST=1 SELFTEST_AUTORUN=1
  cp kernel.elf "$jn"
  cp "artifacts/build/kernel-check-j${jobs}.log" "$artifact_dir/cl13fix5-build-jN.log"
  cmp -s "$j2" "$jn"
  make image SELFTEST=1 SELFTEST_AUTORUN=1
  [[ -z $(nm -u kernel.elf) ]]
  ! grep -E 'kernel/src/(core/selftest|shell/commands/cmd_schedtest)\.c:.*warning:' \
    "$artifact_dir/cl13fix5-build-j2.log" "$artifact_dir/cl13fix5-build-jN.log"
  bash -n scripts/harness-framed.sh
  bash -n scripts/test-cl13-async-contract-focused.sh
  bash -n scripts/test-taskman-v1.sh
  bash -n scripts/test-taskman-v1-soak.sh
  bash -n scripts/test-taskman-v1-negatives.sh
  rg -n 'scheduler\.async\.(kind_strings|initial_idle|run_nonzero|snapshot_coherent|completed_before_marker_contract|unknown_run_reject)' \
    kernel/src/core/selftest.c >"$current_log"
  rg -n 'YIELD_START run=|YIELD_COMPLETE run=|ASYNC_WAIT|ASYNC_STATUS' \
    kernel/src/shell/commands/cmd_schedtest.c >>"$current_log"
  end=$(now_ms)
  ledger_record static 0 host 'stack+j2+jN+image+nm' PASS "$((end-begin))" "$current_log"
  current_log=
}

host_unit_gate(){
  local begin end dir=$artifact_dir/cl13fix5-host-unit
  local sync_ok=$dir-sync-ok.log sync_bad=$dir-sync-bad.log
  local async_before=$dir-async-before.log async_after=$dir-async-after.log
  local async_missing=$dir-async-missing.log out=$artifact_dir/cl13fix5-host-unit.log
  current_scenario=host-unit
  current_log=$out
  begin=$(now_ms)
  printf '%s\n' '[HARNESS][BEGIN] seq=1' 'RESULT' '[HARNESS][END] seq=1 status=0' >"$sync_ok"
  printf '%s\n' '[HARNESS][BEGIN] seq=1' '[HARNESS][END] seq=1 status=0' 'RESULT' >"$sync_bad"
  printf '%s\n' '[HARNESS][BEGIN] seq=1' 'START run=1' 'COMPLETE run=1' '[HARNESS][END] seq=1 status=0' >"$async_before"
  printf '%s\n' '[HARNESS][BEGIN] seq=1' 'START run=1' '[HARNESS][END] seq=1 status=0' 'COMPLETE run=1' >"$async_after"
  printf '%s\n' '[HARNESS][BEGIN] seq=1' 'START run=1' '[HARNESS][END] seq=1 status=0' >"$async_missing"
  framed_evaluate_log_contract SYNC "$sync_ok" '' RESULT
  if framed_evaluate_log_contract SYNC "$sync_bad" '' RESULT; then return 1; fi
  [[ $HARNESS_FRAME_LAST_CLASSIFICATION == GUEST_COMMAND_CONTRACT_FAILURE ]]
  framed_evaluate_log_contract ASYNC "$async_before" START COMPLETE
  [[ $HARNESS_ASYNC_LAST_ORDER == COMPLETE_BEFORE_END ]]
  framed_evaluate_log_contract ASYNC "$async_after" START COMPLETE
  [[ $HARNESS_ASYNC_LAST_ORDER == END_BEFORE_COMPLETE ]]
  if framed_evaluate_log_contract ASYNC "$async_missing" START COMPLETE; then return 1; fi
  [[ $HARNESS_FRAME_LAST_CLASSIFICATION == GUEST_ASYNC_COMPLETION_TIMEOUT ]]
  printf '%s\n' \
    '[CL13][ASYNC_HOST_UNIT] PASS sync-correct=1 sync-late-rejected=1 async-before=1 async-after=1 async-timeout=1' \
    >"$out"
  end=$(now_ms)
  ledger_record host-unit 0 host synthetic-ordering PASS "$((end-begin))" "$out"
  current_log=
}

prepare_image(){
  make kernel-check JOBS=2 SELFTEST=1 SELFTEST_AUTORUN=1 >/dev/null
  make image SELFTEST=1 SELFTEST_AUTORUN=1 >/dev/null
}

common_checks(){
  framed_send_complete "schedtest check" "[SCHED][CHECK] PASS" 120 stress
  framed_send_complete "taskdiag check" "[TASKDIAG][CHECK] PASS" 120 stress
  framed_send_complete "tasktest transport-status" "[HARNESS][STATUS]" 120 stress
  local status
  status=$(grep -F '[HARNESS][STATUS]' "$serial" | tail -1)
  [[ $status =~ accepted=([0-9]+)[[:space:]]completed=([0-9]+) ]]
  [[ ${BASH_REMATCH[1]} == "${BASH_REMATCH[2]}" ]]
}

yield_body(){
  local smp=$1 first=0
  if [[ $smp == 1 ]]; then
    framed_send_complete "schedtest async-status" \
      "[SCHED][ASYNC_STATUS] PASS run=0 kind=NONE state=IDLE" 90 stress
    framed_send_complete "schedtest async-status 999" \
      "[SCHED][ASYNC_STATUS] FAIL reason=unknown-run" 90 stress 1
    framed_schedtest_async YIELD "schedtest yield 1 100000" \
      "[SCHED][STRESS] YIELD_START" "[SCHED][STRESS] YIELD_COMPLETE" 900 stress
    [[ $HARNESS_ASYNC_LAST_ORDER == END_BEFORE_COMPLETE ]]
    first=$HARNESS_ASYNC_LAST_RUN
    framed_schedtest_async YIELD "schedtest yield 1 20000" \
      "[SCHED][STRESS] YIELD_START" "[SCHED][STRESS] YIELD_COMPLETE" 600 stress
    ((HARNESS_ASYNC_LAST_RUN > first))
  else
    framed_schedtest_async YIELD "schedtest yield $smp 20000" \
      "[SCHED][STRESS] YIELD_START" "[SCHED][STRESS] YIELD_COMPLETE" 900 stress
  fi
  framed_send_complete "schedtest async-status $HARNESS_ASYNC_LAST_RUN" \
    "[SCHED][ASYNC_STATUS] PASS run=$HARNESS_ASYNC_LAST_RUN kind=YIELD state=PASS" 90 stress
  common_checks
}

pin_entry_body(){
  local first
  framed_schedtest_async CPU_PIN "schedtest cpu-pin 20000" \
    "[SCHED][CPU_PIN] START" "[SCHED][CPU_PIN] PASS" 900 stress
  first=$HARNESS_ASYNC_LAST_RUN
  framed_send_complete "schedtest async-status $first" \
    "[SCHED][ASYNC_STATUS] PASS run=$first kind=CPU_PIN state=PASS" 90 stress
  framed_schedtest_async ENTRY_WINDOW "schedtest entry-window 20000" \
    "[SCHED][ENTRY_WINDOW] START" "[SCHED][ENTRY_WINDOW] PASS" 900 stress
  ((HARNESS_ASYNC_LAST_RUN > first))
  framed_send_complete "schedtest async-status $HARNESS_ASYNC_LAST_RUN" \
    "[SCHED][ASYNC_STATUS] PASS run=$HARNESS_ASYNC_LAST_RUN kind=ENTRY_WINDOW state=PASS" 90 stress
  common_checks
}

run_boot(){
  local scenario=$1 smp=$2 accel=$3 body=$4 begin end log records
  if ledger_latest_pass "$scenario"; then
    echo "[CL13][ASYNC_FOCUSED] REUSE scenario=$scenario status=PASS"
    return 0
  fi
  current_scenario=$scenario
  log=$artifact_dir/cl13fix5-${scenario}.log
  records=$artifact_dir/cl13fix5-${scenario}-orders.log
  current_log=$log
  HARNESS_ASYNC_RECORD_LOG=$records
  : >"$records"
  begin=$(now_ms)
  harness_lock
  start_qemu "$smp" "$accel"
  selftest_validate_autorun "$serial" "$artifact_dir/cl13fix5-${scenario}"
  "$body" "$smp"
  cp "$serial" "$log"
  printf '\n[HOST ASYNC ORDER]\n' >>"$log"
  cat "$records" >>"$log"
  assert_clean_log "$log"
  ! grep -Fq 'GUEST_ASYNC_COMPLETION_TIMEOUT' "$log"
  ! grep -Fq 'reason=stale-worker' "$log"
  ! grep -Eq '\[SCHED\]\[ASYNC(_WAIT)?\] FAIL' "$log"
  stop_qemu
  harness_unlock
  end=$(now_ms)
  ledger_record "$scenario" "$smp" "$accel" "$body" PASS "$((end-begin))" "$log"
  if [[ $body == pin_entry_body ]]; then
    ledger_record "${scenario/cpu-pin/entry-window}" "$smp" "$accel" \
      entry_window PASS "$((end-begin))" "$log"
  fi
  unset HARNESS_ASYNC_RECORD_LOG
  current_log=
}

yield_gate(){
  local run
  prepare_image
  for run in 1 2 3; do run_boot "yield-smp1-tcg-run$run" 1 tcg yield_body; done
  for run in 1 2 3; do run_boot "yield-smp2-tcg-run$run" 2 tcg yield_body; done
  for run in 1 2 3; do run_boot "yield-smp2-kvm-run$run" 2 kvm yield_body; done
  for run in 1 2 3 4 5; do run_boot "yield-smp4-tcg-run$run" 4 tcg yield_body; done
  for run in 1 2 3; do run_boot "yield-smp4-kvm-run$run" 4 kvm yield_body; done
  for run in 1 2; do run_boot "yield-smp8-tcg-run$run" 8 tcg yield_body; done
  for run in 1 2; do run_boot "yield-smp8-kvm-run$run" 8 kvm yield_body; done
}

pin_gate(){
  local run
  prepare_image
  for run in 1 2 3; do run_boot "cpu-pin-smp4-tcg-run$run" 4 tcg pin_entry_body; done
  for run in 1 2 3; do run_boot "cpu-pin-smp4-kvm-run$run" 4 kvm pin_entry_body; done
  run_boot cpu-pin-smp8-tcg-run1 8 tcg pin_entry_body
  run_boot cpu-pin-smp8-kvm-run1 8 kvm pin_entry_body
}

negative_gate(){
  local begin end log=$artifact_dir/cl13fix5-stale-slot-negative.log
  local records=$artifact_dir/cl13fix5-stale-slot-negative-orders.log
  if ledger_latest_pass stale-slot-negative; then
    echo '[CL13][ASYNC_FOCUSED] REUSE scenario=stale-slot-negative status=PASS'
    return 0
  fi
  current_scenario=stale-slot-negative
  current_log=$log
  begin=$(now_ms)
  harness_lock
  make kernel-check JOBS=2 SELFTEST=1 SELFTEST_AUTORUN=0 \
    KERNEL_EXTRA_CFLAGS='-DHOBBYOS_SCHED_NEGATIVE_STALE_SLOT_CAPTURE' >/dev/null
  make image SELFTEST=1 SELFTEST_AUTORUN=0 \
    KERNEL_EXTRA_CFLAGS='-DHOBBYOS_SCHED_NEGATIVE_STALE_SLOT_CAPTURE' >/dev/null
  HARNESS_AUTORUN_EXPECTED=0
  start_qemu 4 tcg
  framed_schedtest_async_negative "schedtest entry-window 200000" \
    "[SCHED][ENTRY_WINDOW] START" \
    "[SCHED][NEGATIVE] STALE_SLOT_DETECTED" 900 stress "0 1"
  cp "$serial" "$log"
  stop_qemu
  make kernel-check JOBS=2 SELFTEST=1 SELFTEST_AUTORUN=1 >/dev/null
  make image SELFTEST=1 SELFTEST_AUTORUN=1 >/dev/null
  ! grep -Fq HOBBYOS_SCHED_NEGATIVE_STALE_SLOT_CAPTURE artifacts/build/kernel-check-j2.log
  HARNESS_AUTORUN_EXPECTED=1
  HARNESS_ASYNC_RECORD_LOG=$records
  : >"$records"
  start_qemu 4 tcg
  selftest_validate_autorun "$serial" \
    "$artifact_dir/cl13fix5-stale-slot-positive"
  framed_schedtest_async CPU_PIN "schedtest cpu-pin 20000" \
    "[SCHED][CPU_PIN] START" "[SCHED][CPU_PIN] PASS" 900 stress
  common_checks
  printf '\n[POSITIVE RESET]\n' >>"$log"
  cat "$serial" >>"$log"
  printf '\n[HOST ASYNC ORDER]\n' >>"$log"
  cat "$records" >>"$log"
  assert_clean_log "$log"
  stop_qemu
  harness_unlock
  unset HARNESS_ASYNC_RECORD_LOG
  end=$(now_ms)
  ledger_record stale-slot-negative 4 tcg negative+reset PASS "$((end-begin))" "$log"
  current_log=
}

classification_gate(){
  local out=$artifact_dir/cl13fix5-async-contract-classification.log
  protected_snapshot "$protected_after"
  cmp -s "$protected_before" "$protected_after"
  awk '/marker_order=END_BEFORE_COMPLETE/ { print; exit }' \
    "$artifact_dir"/cl13fix5-*-orders.log >"$out"
  {
    echo '[CL13][ASYNC_CONTRACT_CLASSIFICATION]'
    echo 'SYNC_WRAPPER_FALSE_REJECTION_CONFIRMED'
  } >>"$out"
  ledger_record classification 0 host focused+continuity PASS 0 "$out"
  ledger_record protected-runtime 0 host before=after-focused PASS 0 "$protected_after"
}

matrix_gate(){ yield_gate; pin_gate; }

all_gate(){
  static_gate
  host_unit_gate
  yield_gate
  pin_gate
  negative_gate
  classification_gate
}

ensure_ledger
case ${1:-} in
  static) static_gate;;
  host-unit) host_unit_gate;;
  yield) yield_gate;;
  cpu-pin) pin_gate;;
  negative) negative_gate;;
  matrix) matrix_gate;;
  all) all_gate;;
  *) usage;;
esac
