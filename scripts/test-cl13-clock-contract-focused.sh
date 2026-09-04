#!/usr/bin/env bash
set -Eeuo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

artifact_dir=artifacts/build
ledger=$artifact_dir/cl13-ledger.tsv
serial=.qemu/qemu-serial.log
hmp=(python3 scripts/qemu_hmp.py --socket .qemu/hmp.sock)
harness_start_line=0
harness_command=clock-contract-focused
current_scenario=
current_log=
protected_before=$artifact_dir/cl13fix9-protected-runtime-before.sha256
protected_after=$artifact_dir/cl13fix9-protected-runtime-after-focused.sha256
clock_before=$artifact_dir/cl13fix9-clock-runtime-before.sha256
clock_after=$artifact_dir/cl13fix9-clock-runtime-after-focused.sha256
mkdir -p "$artifact_dir"
source scripts/harness-common.sh
source scripts/harness-runtime-ready.sh
source scripts/harness-framed.sh
source scripts/harness-selftest.sh

usage(){
  echo "Usage: $0 host-unit|static|up-tcg|smp4-tcg|smp4-kvm|smp8-tcg|smp8-kvm|all" >&2
  exit 2
}

now_ms(){ echo $(( $(date +%s%N) / 1000000 )); }

protected_snapshot(){
  local output=$1
  { { rg --files kernel bootloader shared; printf '%s\n' makefile; } |
      LC_ALL=C sort |
      rg -v '^(kernel/src/shell/commands/cmd_accounttest\.(c|h)|kernel/src/core/selftest\.c)$' |
      xargs sha256sum; } >"$output"
}

clock_snapshot(){
  sha256sum \
    kernel/src/core/clock.c kernel/src/core/clock.h \
    kernel/src/drivers/timer.c kernel/src/drivers/timer.h \
    kernel/src/timer/hpet.c kernel/src/timer/hpet.h \
    kernel/src/apic/lapic.c kernel/src/apic/lapic.h >"$1"
}

ledger_record(){
  local scenario=$1 smp=$2 accel=$3 command=$4 status=$5 duration=$6 artifact=$7
  local failed_mask=${8:-0} legacy=${9:-0} interval=${10:-0}
  local api=${11:-0} local_delta=${12:-0} retry=${13:-0} cross=${14:-0} span=${15:-0}
  local hash=-
  [[ ! -f $artifact ]] || hash=$(sha256sum "$artifact" | awk '{print $1}')
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    clock-contract-focused "$scenario" "$smp" "$accel" selftest \
    "$command" "$status" "$duration" "$artifact" "$hash" \
    "$failed_mask" "$legacy" "$interval" "$api" "$local_delta" "$retry" "$cross" "$span" >>"$ledger"
  echo "[CL13][LEDGER] stage=clock-contract-focused scenario=$scenario status=$status failed_mask=$failed_mask legacy_mismatches=$legacy interval_failures=$interval api_delta=$api local_delta=$local_delta retry_delta=$retry cross_delta=$cross wrapper_span_ms=$span artifact=$artifact"
}

field_value(){
  local line=$1 field=$2
  sed -n "s/.* ${field}=\([0-9][0-9]*\).*/\1/p" <<<"$line"
}

diagnose_clock_failure(){
  framed_send_complete "accounttest clock-stats" "[ACCOUNT][STATS]" 120 stress "0 1"
  framed_send_complete "accounttest clock-events" "[ACCOUNT][CLOCK_EVENTS]" 120 stress "0 1"
  framed_send_complete "taskdiag accounting" "[TASKDIAG][ACCOUNTING]" 120 stress "0 1"
  framed_send_complete "accounttest check" "[ACCOUNT][CHECK]" 120 stress "0 1"
}

classify_clock_failure(){
  local log=$1 out=$artifact_dir/cl13fix9-clock-failure-classification.log
  local line mask=0 interval=0 classification=REJECTED_CL13_FIX9_CLOCK_CONTRACT
  line=$(grep -F '[ACCOUNT][CLOCK] FAIL' "$log" | tail -1 || true)
  [[ -z $line ]] || mask=$(field_value "$line" failed_mask)
  line=$(grep -F '[ACCOUNT][CLOCK_WRAPPER_LOOP] FAIL' "$log" | tail -1 || true)
  [[ -z $line ]] || interval=$(field_value "$line" interval_failures)
  if ((mask & 2018)); then
    classification=REJECTED_RUNTIME_DEFECT_CONFIRMED
    printf '%s\n' '[CL13][CLOCK_CLASSIFICATION]' 'CLOCK_SOURCE_ANOMALY_CONFIRMED.' >"$out"
  elif ((mask & 28)); then
    classification=REJECTED_CL13_FIX9_DELAY
    printf '%s\n' '[CL13][CLOCK_CLASSIFICATION]' 'CLOCK_DELAY_CONTRACT_FAILURE.' >"$out"
  elif ((mask & 2048)) || ((interval > 0)); then
    classification=REJECTED_CL13_FIX9_WRAPPER
    printf '%s\n' '[CL13][CLOCK_CLASSIFICATION]' 'CLOCK_WRAPPER_INTERVAL_FAILURE.' >"$out"
  else
    printf '%s\n' '[CL13][CLOCK_CLASSIFICATION]' 'CLOCK_TEST_CONTRACT_FAILURE.' >"$out"
  fi
  printf 'status=%s failed_mask=%s interval_failures=%s scenario=%s\n' \
    "$classification" "$mask" "$interval" "$current_scenario" >>"$out"
  ledger_record classification 0 host failure "$classification" 0 "$out" "$mask" 0 "$interval"
  echo "$classification" >&2
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

host_unit_gate(){
  local old=$artifact_dir/cl13fix8-matrix-smp4-tcg-account-clock-failed.log
  local out=$artifact_dir/cl13fix9-clock-host-audit.log begin end
  begin=$(now_ms)
  [[ -f $old ]]
  git show HEAD:kernel/src/shell/commands/cmd_accounttest.c |
    rg -q 'timer_get_uptime_ms\(\)==clock_monotonic_ms\(\)'
  ! rg -q 'timer_get_uptime_ms\(\)[[:space:]]*==[[:space:]]*clock_monotonic_ms\(\)' \
    kernel/src/shell/commands/cmd_accounttest.c
  python3 - "$old" >"$out" <<'PY'
import pathlib
import re
import sys

text = pathlib.Path(sys.argv[1]).read_text(errors="replace")
clock = re.search(r"\[ACCOUNT\]\[CLOCK\] FAIL reads=(\d+) d10=(\d+) d100=(\d+) d1000=(\d+)", text)
if not clock:
    raise SystemExit("old ACCOUNT CLOCK failure record missing")
reads, d10, d100, d1000 = map(int, clock.groups())
if reads != 1_000_000:
    raise SystemExit(f"unexpected reads={reads}")
for ms, observed in ((10, d10), (100, d100), (1000, d1000)):
    target = ms * 1_000_000
    if not target // 2 <= observed <= target * 2 + 50_000_000:
        raise SystemExit(f"delay failed ms={ms} observed={observed}")
clock_pos = clock.start()
if text.find("[ACCOUNT][CHECK] PASS", 0, clock_pos) < 0:
    raise SystemExit("ACCOUNT CHECK before failure missing")
if text.find("[TASKDIAG][CHECK] PASS", 0, clock_pos) < 0:
    raise SystemExit("TASKDIAG CHECK before failure missing")
if not re.search(r"\[HARNESS\]\[END\] seq=33 status=1", text[clock_pos:]):
    raise SystemExit("handler status=1 missing")
if re.search(r"PANIC|#PF|#GP|FATAL|STRUCTURAL_FAULT|FINISH_FAULT", text):
    raise SystemExit("guest fault found")
print("[CL13][CLOCK_HOST_AUDIT] PASS delays=3 legacy_gate=1")
print(f"reads={reads} d10={d10} d100={d100} d1000={d1000} status=1 faults=0")
PY
  end=$(now_ms)
  ledger_record host-unit 0 host old-log+source PASS "$((end-begin))" "$out"
}

static_gate(){
  local out=$artifact_dir/cl13fix9-static-build.log begin end
  begin=$(now_ms)
  : >"$out"
  bash -n scripts/test-cl13-clock-contract-focused.sh
  git diff --check
  ! rg -n 'timer_get_uptime_ms\(\)[[:space:]]*==[[:space:]]*clock_monotonic_ms\(\)' \
    kernel/src/shell/commands/cmd_accounttest.c
  rg -n 'accounttest_uptime_wrapper_sample_valid|wrapper_before_ms|failed_mask' \
    kernel/src/shell/commands/cmd_accounttest.c kernel/src/shell/commands/cmd_accounttest.h >>"$out"
  rg -n 'accounting\.uptime\.wrapper_interval' kernel/src/core/selftest.c >>"$out"
  rg -n 'clock-wrapper-loop|clock-loop|clock-contract' \
    kernel/src/shell/commands/cmd_accounttest.c \
    scripts/test-cl13-clock-contract-focused.sh >>"$out"
  make stack-check >>"$out" 2>&1
  make kernel-check JOBS=2 SELFTEST=1 SELFTEST_AUTORUN=1 >>"$out" 2>&1
  cp kernel.elf /tmp/hobbyos-cl13fix9-selftest-j2.elf
  make kernel-check JOBS="$(nproc)" SELFTEST=1 SELFTEST_AUTORUN=1 >>"$out" 2>&1
  cp kernel.elf /tmp/hobbyos-cl13fix9-selftest-jN.elf
  cmp -s /tmp/hobbyos-cl13fix9-selftest-j2.elf /tmp/hobbyos-cl13fix9-selftest-jN.elf
  make image SELFTEST=1 SELFTEST_AUTORUN=1 >>"$out" 2>&1
  nm -u kernel.elf >"$artifact_dir/cl13fix9-focused-nm-undefined.log"
  [[ ! -s $artifact_dir/cl13fix9-focused-nm-undefined.log ]]
  ! rg -n 'cmd_accounttest\.c:.*warning:|selftest\.c:.*warning:' "$out"
  sha256sum /tmp/hobbyos-cl13fix9-selftest-j2.elf \
    /tmp/hobbyos-cl13fix9-selftest-jN.elf kernel.elf hobbyos.img \
    >"$artifact_dir/cl13fix9-focused-build-hashes.log"
  end=$(now_ms)
  ledger_record static 0 host stack+j2+jN+image+nm PASS "$((end-begin))" "$out"
}

prepare_image(){
  make kernel-check JOBS=2 SELFTEST=1 SELFTEST_AUTORUN=1 >/dev/null
  make image SELFTEST=1 SELFTEST_AUTORUN=1 >/dev/null
}

run_clock_gate(){
  local command=$1 marker=$2 timeout=${3:-300}
  framed_send_complete "$command" "$marker" "$timeout" stress "0 1"
  if [[ $HARNESS_FRAME_LAST_STATUS != 0 ]]; then
    diagnose_clock_failure
    cp "$serial" "${current_log%.log}-diagnostics.log"
    classify_clock_failure "${current_log%.log}-diagnostics.log"
    return 1
  fi
}

validate_taskdiag_accounting(){
  local line
  framed_send_complete "taskdiag accounting" "[TASKDIAG][ACCOUNTING] PASS" 120 stress
  line=$(grep -F '[TASKDIAG][ACCOUNTING] PASS' "$serial" | tail -1)
  [[ $line =~ runtime_regressions=0 ]]
  [[ $line =~ runtime_overflows=0 ]]
  [[ $line =~ accounting_clock_regressions=0 ]]
  [[ $line =~ clock_api_regressions=0 ]]
  [[ $line =~ clock_source_regressions=0 ]]
  [[ $line =~ retry_exhaustion=0 ]]
  [[ $line =~ state_corruption=0 ]]
}

transport_balance(){
  local line
  framed_send_complete "tasktest transport-status" "[HARNESS][STATUS]" 120 stress
  line=$(grep -F '[HARNESS][STATUS]' "$serial" | tail -1)
  [[ $line =~ accepted=([0-9]+)[[:space:]]completed=([0-9]+) ]]
  [[ ${BASH_REMATCH[1]} == "${BASH_REMATCH[2]}" ]]
}

run_boot(){
  local scenario=$1 smp=$2 accel=$3 run=$4 wrapper_samples=$5 workers=$6
  local begin end prefix log wrapper_line loop_line legacy_wrapper legacy_loop
  local interval span failed_mask cross loop_timeout=600 smp_timeout=900
  current_scenario="${scenario}-run${run}"
  prefix=$artifact_dir/cl13fix9-${current_scenario}
  log=${prefix}.log
  current_log=$log
  begin=$(now_ms)
  start_qemu "$smp" "$accel"
  runtime_validate_boot_log "$serial" 1
  selftest_validate_autorun "$serial" "$prefix" accounting.uptime.wrapper_interval
  framed_send_complete "tasktest readiness-status" \
    "[BOOT][READINESS_STATUS] PASS state=TEST_READY cpus=$smp/$smp" 120 stress
  run_clock_gate "accounttest clock-contract" "[ACCOUNT][CLOCK_CONTRACT]" 120
  run_clock_gate "accounttest clock-wrapper-loop $wrapper_samples" \
    "[ACCOUNT][CLOCK_WRAPPER_LOOP]" 300
  if [[ $accel == kvm && $smp == 8 ]]; then
    loop_timeout=1800
    smp_timeout=1800
  fi
  run_clock_gate "accounttest clock-loop 10" "[ACCOUNT][CLOCK_LOOP]" "$loop_timeout"
  if ((workers)); then
    run_clock_gate "accounttest clock-smp $workers 1000000" "[ACCOUNT][CLOCK_SMP]" "$smp_timeout"
  fi
  run_clock_gate "accounttest clock-classifier" "[ACCOUNT][CLOCK_CLASSIFIER]" 120
  if [[ $accel == kvm && $smp == 4 ]]; then
    run_clock_gate "accounttest lapic-config" "[ACCOUNT][LAPIC_CONFIG]" 120
    run_clock_gate "accounttest lapic-liveness 500" "[ACCOUNT][LAPIC_LIVENESS]" 120
  fi
  run_clock_gate "accounttest check" "[ACCOUNT][CHECK]" 120
  validate_taskdiag_accounting
  transport_balance
  cp "$serial" "$log"
  assert_clean_log "$log"
  wrapper_line=$(grep -F '[ACCOUNT][CLOCK_WRAPPER_LOOP] PASS' "$log" | tail -1)
  loop_line=$(grep -F '[ACCOUNT][CLOCK_LOOP] PASS' "$log" | tail -1)
  legacy_wrapper=$(field_value "$wrapper_line" legacy_exact_mismatches)
  legacy_loop=$(field_value "$loop_line" legacy_exact_mismatches)
  interval=$(field_value "$wrapper_line" interval_failures)
  span=$(field_value "$wrapper_line" max_span_ms)
  failed_mask=$(field_value "$loop_line" failed_mask)
  cross=$(field_value "$loop_line" cross_lag_total)
  [[ $interval == 0 && $failed_mask == 0 ]]
  end=$(now_ms)
  stop_qemu
  ledger_record "$current_scenario" "$smp" "$accel" focused PASS \
    "$((end-begin))" "$log" "$failed_mask" "$((legacy_wrapper+legacy_loop))" \
    "$interval" 0 0 0 "$cross" "$span"
  current_log=
}

group_gate(){
  local scenario=$1 smp=$2 accel=$3 runs=$4 samples=$5 workers=$6 run
  if [[ $accel == kvm && (! -r /dev/kvm || ! -w /dev/kvm) ]]; then
    echo 'BLOCKED_BY_EXTERNAL_ENVIRONMENT: KVM unavailable' >&2
    return 3
  fi
  prepare_image
  harness_lock
  for ((run=1; run<=runs; run++)); do
    run_boot "$scenario" "$smp" "$accel" "$run" "$samples" "$workers"
  done
  harness_unlock
}

classification_gate(){
  local out=$artifact_dir/cl13fix9-clock-classification.log
  local logs wrapper_count loop_count contract_count source_fail interval_fail
  local legacy wrapper_samples rounds
  logs=(
    "$artifact_dir"/cl13fix9-up-tcg-run{1..2}.log
    "$artifact_dir"/cl13fix9-smp4-tcg-run{1..5}.log
    "$artifact_dir"/cl13fix9-smp4-kvm-run{1..3}.log
    "$artifact_dir"/cl13fix9-smp8-tcg-run{1..3}.log
    "$artifact_dir"/cl13fix9-smp8-kvm-run{1..3}.log
  )
  local item
  for item in "${logs[@]}"; do [[ -f $item ]]; done
  wrapper_count=$(grep -hFc '[ACCOUNT][CLOCK_WRAPPER_LOOP] PASS' "${logs[@]}" | awk '{s+=$1} END{print s+0}')
  loop_count=$(grep -hFc '[ACCOUNT][CLOCK_LOOP] PASS' "${logs[@]}" | awk '{s+=$1} END{print s+0}')
  contract_count=$(grep -hFc '[ACCOUNT][CLOCK_CONTRACT] PASS' "${logs[@]}" | awk '{s+=$1} END{print s+0}')
  [[ $wrapper_count == 16 && $loop_count == 16 && $contract_count == 16 ]]
  ! grep -hEq '\[ACCOUNT\]\[(CLOCK|CLOCK_LOOP|CLOCK_WRAPPER_LOOP)\] FAIL' "${logs[@]}"
  interval_fail=$(grep -hF '[ACCOUNT][CLOCK_WRAPPER_LOOP] PASS' "${logs[@]}" |
    sed -n 's/.* interval_failures=\([0-9][0-9]*\).*/\1/p' |
    awk '{s+=$1} END{print s+0}')
  legacy=$(grep -hE '\[ACCOUNT\]\[(CLOCK_WRAPPER_LOOP|CLOCK_LOOP)\] PASS' "${logs[@]}" |
    sed -n 's/.* legacy_exact_mismatches=\([0-9][0-9]*\).*/\1/p' |
    awk '{s+=$1} END{print s+0}')
  wrapper_samples=$(grep -hF '[ACCOUNT][CLOCK_WRAPPER_LOOP] PASS' "${logs[@]}" |
    sed -n 's/.* samples=\([0-9][0-9]*\).*/\1/p' |
    awk '{s+=$1} END{print s+0}')
  rounds=$(grep -hF '[ACCOUNT][CLOCK_LOOP] PASS' "${logs[@]}" |
    sed -n 's/.* rounds=\([0-9][0-9]*\).*/\1/p' |
    awk '{s+=$1} END{print s+0}')
  [[ $interval_fail == 0 && $wrapper_samples == 3700000 && $rounds == 160 ]]
  source_fail=$({ grep -hE 'clock_api_regressions=[1-9]|clock_source_regressions=[1-9]|retry_exhaustion=[1-9]|state_corruption=[1-9]' "${logs[@]}" || true; } | wc -l)
  [[ $source_fail == 0 ]]
  protected_snapshot "$protected_after"
  cmp -s "$protected_before" "$protected_after"
  clock_snapshot "$clock_after"
  cmp -s "$clock_before" "$clock_after"
  {
    echo '[CL13][CLOCK_CLASSIFICATION]'
    echo 'LEGACY_EXACT_MS_EQUALITY_FALSE_REJECTION_CONFIRMED.'
    echo "boots=16 wrapper_samples=$wrapper_samples clock_rounds=$rounds legacy_mismatches=$legacy interval_failures=$interval_fail source_anomalies=0 protected_runtime=unchanged"
  } >"$out"
  ledger_record classification 0 host focused+continuity PASS 0 "$out" 0 "$legacy" 0 0 0 0 0 0
}

all_gate(){
  host_unit_gate
  static_gate
  group_gate up-tcg 1 tcg 2 100000 0
  group_gate smp4-tcg 4 tcg 5 250000 16
  group_gate smp4-kvm 4 kvm 3 250000 16
  group_gate smp8-tcg 8 tcg 3 250000 32
  group_gate smp8-kvm 8 kvm 3 250000 32
  classification_gate
}

case ${1:-} in
  host-unit) host_unit_gate;;
  static) static_gate;;
  up-tcg) group_gate up-tcg 1 tcg 2 100000 0;;
  smp4-tcg) group_gate smp4-tcg 4 tcg 5 250000 16;;
  smp4-kvm) group_gate smp4-kvm 4 kvm 3 250000 16;;
  smp8-tcg) group_gate smp8-tcg 8 tcg 3 250000 32;;
  smp8-kvm) group_gate smp8-kvm 8 kvm 3 250000 32;;
  all) all_gate;;
  *) usage;;
esac

trap - ERR EXIT
