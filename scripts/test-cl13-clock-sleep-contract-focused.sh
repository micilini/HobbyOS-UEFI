#!/usr/bin/env bash
set -Eeuo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

artifact_dir=artifacts/build
ledger=$artifact_dir/cl13-ledger.tsv
serial=.qemu/qemu-serial.log
hmp=(python3 scripts/qemu_hmp.py --socket .qemu/hmp.sock)
harness_start_line=0
harness_command=clock-sleep-contract-focused
current_scenario=
current_log=
protected_before=$artifact_dir/cl13fix10-protected-runtime-before.sha256
protected_after=$artifact_dir/cl13fix10-protected-runtime-after-focused.sha256
time_before=$artifact_dir/cl13fix10-time-runtime-before.sha256
time_after=$artifact_dir/cl13fix10-time-runtime-after-focused.sha256
mkdir -p "$artifact_dir"
source scripts/harness-common.sh
source scripts/harness-runtime-ready.sh
source scripts/harness-framed.sh
source scripts/harness-selftest.sh

usage(){
  echo "Usage: $0 host-unit|static|up-tcg|smp4-tcg|smp4-kvm|smp8-tcg|smp8-kvm|classification|all" >&2
  exit 2
}

now_ms(){ echo $(( $(date +%s%N) / 1000000 )); }

field_value(){
  local line=$1 field=$2
  sed -n "s/.* ${field}=\([0-9][0-9]*\).*/\1/p" <<<"$line"
}

protected_snapshot(){
  local output=$1
  { { rg --files kernel bootloader shared; printf '%s\n' makefile; } |
      LC_ALL=C sort |
      rg -v '^(kernel/src/shell/commands/cmd_accounttest\.(c|h)|kernel/src/core/selftest\.c)$' |
      xargs sha256sum; } >"$output"
}

time_snapshot(){
  sha256sum \
    kernel/src/core/clock.c kernel/src/core/clock.h \
    kernel/src/drivers/timer.c kernel/src/drivers/timer.h \
    kernel/src/core/timers.c kernel/src/core/timers.h \
    kernel/src/timer/hpet.c kernel/src/timer/hpet.h \
    kernel/src/apic/lapic.c kernel/src/apic/lapic.h \
    kernel/src/core/scheduler.c kernel/src/core/scheduler.h >"$1"
}

ledger_record(){
  local scenario=$1 smp=$2 accel=$3 command=$4 status=$5 duration=$6 artifact=$7
  local early=${8:-0} late=${9:-0} wait_error=${10:-0} late_rounds=${11:-0}
  local target=${12:-0} samples=${13:-0} p50=${14:-0} p95=${15:-0} p99=${16:-0}
  local maximum=${17:-0} late_samples=${18:-0} host_load=${19:--} host_psi=${20:--}
  local hash=-
  [[ ! -f $artifact ]] || hash=$(sha256sum "$artifact" | awk '{print $1}')
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    clock-sleep-contract-focused "$scenario" "$smp" "$accel" selftest \
    "$command" "$status" "$duration" "$artifact" "$hash" \
    "$early" "$late" "$wait_error" "$late_rounds" "$target" "$samples" \
    "$p50" "$p95" "$p99" "$maximum" "$late_samples" "$host_load/$host_psi" >>"$ledger"
  echo "[CL13][LEDGER] stage=clock-sleep-contract-focused scenario=$scenario status=$status early_mask=$early late_mask=$late wait_error_mask=$wait_error late_rounds=$late_rounds target=$target samples=$samples p50=$p50 p95=$p95 p99=$p99 max=$maximum late_samples=$late_samples host_load=$host_load host_psi=$host_psi artifact=$artifact"
}

host_evidence(){
  local output=$1
  {
    echo "timestamp=$(date --iso-8601=ns)"
    echo "nproc=$(nproc)"
    echo "loadavg=$(cat /proc/loadavg)"
    echo "cpus_allowed_list=$(awk '/Cpus_allowed_list/{print $2}' /proc/self/status)"
    echo "cgroup=$(awk -F: '{print $3}' /proc/self/cgroup | paste -sd, -)"
    echo "cpuset=$(cat /sys/fs/cgroup/cpuset.cpus.effective 2>/dev/null || true)"
    echo "cpu_max=$(cat /sys/fs/cgroup/cpu.max 2>/dev/null || true)"
    echo "kvm_readable=$([[ -r /dev/kvm ]] && echo 1 || echo 0)"
    echo "kvm_writable=$([[ -w /dev/kvm ]] && echo 1 || echo 0)"
    echo "qemu_version=$(qemu-system-x86_64 --version | head -1)"
    echo '[CPU_PSI]'
    cat /proc/pressure/cpu 2>/dev/null || true
    echo '[PROC_STAT_CPU]'
    awk '/^cpu/{print}' /proc/stat
    echo '[TOP_CPU_THREADS]'
    ps -eLo pid,tid,psr,pcpu,comm --sort=-pcpu | head -21 || true
  } >"$output"
}

host_unit_gate(){
  local old=$artifact_dir/cl13fix9-smp8-kvm-run2-failed.log
  local out=$artifact_dir/cl13fix10-sleep-host-audit.log begin end
  begin=$(now_ms)
  [[ -f $old ]]
  python3 - "$old" >"$out" <<'PY'
import pathlib
import re
import sys

text = pathlib.Path(sys.argv[1]).read_text(errors="replace")
record = re.search(r"\[ACCOUNT\]\[CLOCK\] FAIL reads=(\d+) failed_mask=(\d+) monotonic_failures=(\d+) d10=(\d+) d100=(\d+) d1000=(\d+).*?api_delta=(\d+) local_delta=(\d+) cross_delta=(\d+) retry_delta=(\d+) saturation_delta=(\d+) state_delta=(\d+) not_ready_delta=(\d+).*?wrapper_interval_valid=(\d+)", text)
if not record:
    raise SystemExit("historical CLOCK failure missing")
(reads, mask, monotonic, d10, d100, d1000, api, local, cross, retry,
 saturation, state, not_ready, wrapper) = map(int, record.groups())
if (reads, mask, monotonic) != (1_000_000, 4, 0):
    raise SystemExit(f"unexpected historical identity reads={reads} mask={mask} monotonic={monotonic}")
if d10 <= 70_000_000:
    raise SystemExit("10ms sample did not exceed old soft upper")
for ms, observed in ((100, d100), (1000, d1000)):
    upper = ms * 2_000_000 + 50_000_000
    if not ms * 500_000 <= observed <= upper:
        raise SystemExit(f"historical non-outlier delay invalid ms={ms} observed={observed}")
if any((api, local, retry, saturation, state, not_ready)):
    raise SystemExit("historical source anomaly present")
if wrapper != 1:
    raise SystemExit("historical wrapper interval invalid")
span = re.search(r"\[ACCOUNT\]\[CLOCK_WRAPPER_LOOP\] PASS .*?max_span_ms=(\d+)", text)
if not span or int(span.group(1)) < 70:
    raise SystemExit("historical wrapper span evidence missing")
if not re.search(r"\[ACCOUNT\]\[CLOCK_LOOP\] FAIL round=6 failed_mask=4", text):
    raise SystemExit("historical round identity missing")
if not re.search(r"\[HARNESS\]\[END\] seq=4 status=1", text):
    raise SystemExit("historical handler status missing")
if re.search(r"PANIC|#PF|#GP|FATAL|STRUCTURAL_FAULT|FINISH_FAULT", text):
    raise SystemExit("historical guest fault found")
overshoot = d10 - 10_000_000
if overshoot != 111_138_210:
    raise SystemExit(f"unexpected overshoot={overshoot}")
print("[CL13][SLEEP_HOST_AUDIT] PASS class=late-only source_anomalies=0")
print(f"reads={reads} failed_mask={mask} d10={d10} d100={d100} d1000={d1000} overshoot={overshoot} wrapper_span_ms={span.group(1)} wrapper_interval={wrapper} faults=0")
PY
  end=$(now_ms)
  ledger_record host-unit 0 host historical-log PASS "$((end-begin))" "$out" 0 1 0 1
}

static_gate(){
  local out=$artifact_dir/cl13fix10-static-build.log begin end
  begin=$(now_ms)
  : >"$out"
  bash -n scripts/test-cl13-clock-sleep-contract-focused.sh
  git diff --check
  ! rg -n 'target/2|target\*2\+50000000' kernel/src/shell/commands/cmd_accounttest.c
  rg -n 'ACCOUNT_SLEEP_EARLY_TOLERANCE_NS|sleep_early_mask|sleep_late_mask|sleep_wait_error_mask' \
    kernel/src/shell/commands/cmd_accounttest.c kernel/src/shell/commands/cmd_accounttest.h >>"$out"
  rg -n 'sleep-profile|SLEEP_PROFILE|p95' kernel/src/shell/commands/cmd_accounttest.c \
    scripts/test-cl13-clock-sleep-contract-focused.sh >>"$out"
  rg -n 'accounting\.sleep\.' kernel/src/core/selftest.c >>"$out"
  make stack-check >>"$out" 2>&1
  make kernel-check JOBS=2 SELFTEST=1 SELFTEST_AUTORUN=1 >>"$out" 2>&1
  cp kernel.elf /tmp/hobbyos-cl13fix10-selftest-j2.elf
  make kernel-check JOBS="$(nproc)" SELFTEST=1 SELFTEST_AUTORUN=1 >>"$out" 2>&1
  cp kernel.elf /tmp/hobbyos-cl13fix10-selftest-jN.elf
  cmp -s /tmp/hobbyos-cl13fix10-selftest-j2.elf /tmp/hobbyos-cl13fix10-selftest-jN.elf
  make image SELFTEST=1 SELFTEST_AUTORUN=1 >>"$out" 2>&1
  nm -u kernel.elf >"$artifact_dir/cl13fix10-focused-nm-undefined.log"
  [[ ! -s $artifact_dir/cl13fix10-focused-nm-undefined.log ]]
  ! rg -n 'cmd_accounttest\.c:.*warning:|selftest\.c:.*warning:' "$out"
  sha256sum /tmp/hobbyos-cl13fix10-selftest-j2.elf \
    /tmp/hobbyos-cl13fix10-selftest-jN.elf kernel.elf hobbyos.img \
    >"$artifact_dir/cl13fix10-focused-build-hashes.log"
  time_snapshot "$artifact_dir/cl13fix10-time-runtime-after-static.sha256"
  cmp -s "$time_before" "$artifact_dir/cl13fix10-time-runtime-after-static.sha256"
  end=$(now_ms)
  ledger_record static 0 host stack+j2+jN+image+nm PASS "$((end-begin))" "$out"
}

prepare_image(){
  make kernel-check JOBS=2 SELFTEST=1 SELFTEST_AUTORUN=1 >/dev/null
  make image SELFTEST=1 SELFTEST_AUTORUN=1 >/dev/null
}

diagnose_hard_failure(){
  framed_send_complete "accounttest clock-stats" "[ACCOUNT][STATS]" 180 stress "0 1" || true
  framed_send_complete "accounttest clock-events" "[ACCOUNT][CLOCK_EVENTS]" 180 stress "0 1" || true
  framed_send_complete "taskdiag accounting" "[TASKDIAG][ACCOUNTING]" 180 stress "0 1" || true
  framed_send_complete "accounttest check" "[ACCOUNT][CHECK]" 180 stress "0 1" || true
  cp "$serial" "${current_log%.log}-diagnostics.log"
}

run_hard_gate(){
  local command=$1 marker=$2 timeout=${3:-1800}
  framed_send_complete "$command" "$marker" "$timeout" stress "0 1"
  if [[ $HARNESS_FRAME_LAST_STATUS != 0 ]]; then
    diagnose_hard_failure
    echo REJECTED_RUNTIME_DEFECT_CONFIRMED >&2
    return 1
  fi
}

validate_taskdiag_accounting(){
  local line
  framed_send_complete "taskdiag accounting" "[TASKDIAG][ACCOUNTING] PASS" 180 stress
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
  framed_send_complete "tasktest transport-status" "[HARNESS][STATUS]" 180 stress
  line=$(grep -F '[HARNESS][STATUS]' "$serial" | tail -1)
  [[ $line =~ accepted=([0-9]+)[[:space:]]completed=([0-9]+) ]]
  [[ ${BASH_REMATCH[1]} == "${BASH_REMATCH[2]}" ]]
}

validate_boot_records(){
  local log=$1 loop_line profile_line target p95 upper
  local early wait_error source_bad=0 profile_count=0 degraded=0 late_samples=0
  loop_line=$(grep -F '[ACCOUNT][CLOCK_LOOP] PASS' "$log" | tail -1)
  [[ $(field_value "$loop_line" failed_mask) == 0 ]]
  [[ $(field_value "$loop_line" sleep_early_mask) == 0 ]]
  [[ $(field_value "$loop_line" sleep_wait_error_mask) == 0 ]]
  while IFS= read -r profile_line; do
    ((profile_count+=1))
    early=$(field_value "$profile_line" early)
    wait_error=$(field_value "$profile_line" wait_errors)
    [[ $early == 0 && $wait_error == 0 ]]
    for field in api_delta local_delta retry_delta saturation_delta state_delta not_ready_delta; do
      [[ $(field_value "$profile_line" "$field") == 0 ]] || source_bad=1
    done
    target=$(field_value "$profile_line" target_ms)
    p95=$(field_value "$profile_line" p95)
    upper=$(field_value "$profile_line" soft_upper)
    ((p95 <= upper)) || ((degraded+=1))
    late_samples=$((late_samples + $(field_value "$profile_line" late_soft)))
    ledger_record "$current_scenario-profile-$target" 0 profile sleep-profile \
      "$([[ $p95 -le $upper ]] && echo PASS || echo DEGRADED)" 0 "$log" \
      0 0 0 0 "$target" "$(field_value "$profile_line" samples)" \
      "$(field_value "$profile_line" p50)" "$p95" \
      "$(field_value "$profile_line" p99)" "$(field_value "$profile_line" max)" \
      "$(field_value "$profile_line" late_soft)"
  done < <(grep -E '^\[ACCOUNT\]\[SLEEP_PROFILE\] (PASS|DEGRADED|FAIL) ' "$log")
  [[ $profile_count == 3 && $source_bad == 0 ]]
  VALIDATE_LATE_ROUNDS=$(field_value "$loop_line" late_rounds)
  VALIDATE_LATE_MASK=$(( ($(field_value "$loop_line" late10)>0 ? 1 : 0) |
                          ($(field_value "$loop_line" late100)>0 ? 2 : 0) |
                          ($(field_value "$loop_line" late1000)>0 ? 4 : 0) ))
  VALIDATE_LATE_SAMPLES=$late_samples
  VALIDATE_DEGRADED=$degraded
}

run_boot(){
  local scenario=$1 smp=$2 accel=$3 run=$4 workers=$5 include_wrapper=$6
  local begin end prefix log host_before= host_after= loop_line
  current_scenario="${scenario}-run${run}"
  prefix=$artifact_dir/cl13fix10-${current_scenario}
  log=${prefix}.log
  current_log=$log
  begin=$(now_ms)
  if [[ $accel == kvm ]]; then
    host_before=${prefix}-host-before.env
    host_after=${prefix}-host-after.env
    host_evidence "$host_before"
  fi
  start_qemu "$smp" "$accel"
  runtime_validate_boot_log "$serial" 1
  selftest_validate_autorun "$serial" "$prefix" \
    accounting.sleep.minimum_deadline \
    accounting.sleep.one_ms_granularity \
    accounting.sleep.late_is_diagnostic \
    accounting.sleep.wait_result \
    accounting.sleep.percentiles
  framed_send_complete "tasktest readiness-status" \
    "[BOOT][READINESS_STATUS] PASS state=TEST_READY cpus=$smp/$smp" 180 stress
  if ((include_wrapper)); then
    run_hard_gate "accounttest clock-contract" "[ACCOUNT][CLOCK_CONTRACT]" 180
    run_hard_gate "accounttest clock-wrapper-loop 100000" \
      "[ACCOUNT][CLOCK_WRAPPER_LOOP]" 900
  fi
  run_hard_gate "accounttest clock-loop 20" "[ACCOUNT][CLOCK_LOOP]" 3600
  run_hard_gate "accounttest sleep-profile-set" "[ACCOUNT][SLEEP_PROFILE_SET]" 1800
  if ((workers)); then
    run_hard_gate "accounttest clock-smp $workers 1000000" "[ACCOUNT][CLOCK_SMP]" 2400
  fi
  run_hard_gate "accounttest clock-classifier" "[ACCOUNT][CLOCK_CLASSIFIER]" 180
  if [[ $accel == kvm && $smp == 4 ]]; then
    run_hard_gate "accounttest lapic-config" "[ACCOUNT][LAPIC_CONFIG]" 180
    run_hard_gate "accounttest lapic-liveness 500" "[ACCOUNT][LAPIC_LIVENESS]" 180
  fi
  run_hard_gate "accounttest check" "[ACCOUNT][CHECK]" 180
  validate_taskdiag_accounting
  transport_balance
  cp "$serial" "$log"
  assert_clean_log "$log"
  validate_boot_records "$log"
  if [[ $accel == kvm ]]; then
    host_evidence "$host_after"
  fi
  end=$(now_ms)
  stop_qemu
  loop_line=$(grep -F '[ACCOUNT][CLOCK_LOOP] PASS' "$log" | tail -1)
  ledger_record "$current_scenario" "$smp" "$accel" focused PASS \
    "$((end-begin))" "$log" 0 "$VALIDATE_LATE_MASK" 0 "$VALIDATE_LATE_ROUNDS" \
    0 0 0 0 0 0 "$VALIDATE_LATE_SAMPLES" \
    "$([[ -n $host_before ]] && awk -F= '/^loadavg=/{print $2; exit}' "$host_before" | tr ' ' '_' || echo -)" \
    "$([[ -n $host_before ]] && awk '/^some avg10=/{print $2; exit}' "$host_before" || echo -)"
  current_log=
}

group_gate(){
  local scenario=$1 smp=$2 accel=$3 runs=$4 workers=$5 include_wrapper=$6 run
  if [[ $accel == kvm && (! -r /dev/kvm || ! -w /dev/kvm) ]]; then
    echo 'BLOCKED_BY_EXTERNAL_ENVIRONMENT: KVM unavailable' >&2
    return 3
  fi
  prepare_image
  harness_lock
  for ((run=1; run<=runs; run++)); do
    run_boot "$scenario" "$smp" "$accel" "$run" "$workers" "$include_wrapper"
  done
  harness_unlock
}

classification_gate(){
  local out=$artifact_dir/cl13fix10-clock-sleep-classification.log
  local logs=() item tcg_degraded=0 smp4_kvm_degraded=0 smp8_kvm_degraded=0
  local all_degraded=0 late_total=0 classification status=PASS
  logs+=("$artifact_dir"/cl13fix10-up-tcg-run{1..2}.log)
  logs+=("$artifact_dir"/cl13fix10-smp4-tcg-run{1..3}.log)
  logs+=("$artifact_dir"/cl13fix10-smp4-kvm-run{1..3}.log)
  logs+=("$artifact_dir"/cl13fix10-smp8-tcg-run{1..3}.log)
  logs+=("$artifact_dir"/cl13fix10-smp8-kvm-run{1..5}.log)
  for item in "${logs[@]}"; do [[ -f $item ]]; done
  ! grep -hEq '\[ACCOUNT\]\[(CLOCK|CLOCK_LOOP|CLOCK_SMP|SLEEP_PROFILE|SLEEP_PROFILE_SET)\] FAIL' "${logs[@]}"
  ! grep -hEq ' sleep_early_mask=[1-9]| sleep_wait_error_mask=[1-9]| early=[1-9]| wait_errors=[1-9]| api_delta=[1-9]| local_delta=[1-9]| retry_delta=[1-9]| saturation_delta=[1-9]| state_delta=[1-9]| not_ready_delta=[1-9]' "${logs[@]}"
  for item in "${logs[@]}"; do
    local scenario_degraded=0
    while IFS= read -r line; do
      (( $(field_value "$line" p95) <= $(field_value "$line" soft_upper) )) || ((scenario_degraded+=1))
      late_total=$((late_total + $(field_value "$line" late_soft)))
    done < <(grep -E '^\[ACCOUNT\]\[SLEEP_PROFILE\] (PASS|DEGRADED) ' "$item")
    all_degraded=$((all_degraded + scenario_degraded))
    case $item in
      *-tcg-*) tcg_degraded=$((tcg_degraded + scenario_degraded));;
      *smp4-kvm*) smp4_kvm_degraded=$((smp4_kvm_degraded + scenario_degraded));;
      *smp8-kvm*) smp8_kvm_degraded=$((smp8_kvm_degraded + scenario_degraded));;
    esac
  done
  protected_snapshot "$protected_after"
  time_snapshot "$time_after"
  cmp -s "$protected_before" "$protected_after"
  cmp -s "$time_before" "$time_after"
  if ((tcg_degraded)); then
    classification=REJECTED_CL13_SLEEP_SERVICE_PERSISTENT
    status=$classification
  elif ((smp8_kvm_degraded && !smp4_kvm_degraded)); then
    classification=KVM_SLEEP_SERVICE_ENVIRONMENT_DEGRADED
    status=BLOCKED_BY_EXTERNAL_ENVIRONMENT
  elif ((smp4_kvm_degraded || smp8_kvm_degraded)); then
    classification=KVM_SLEEP_SERVICE_ENVIRONMENT_DEGRADED
    status=BLOCKED_BY_EXTERNAL_ENVIRONMENT
  elif ((late_total)); then
    classification=SINGLE_SAMPLE_SLEEP_QOS_FALSE_REJECTION_CONFIRMED
  else
    classification=CLOCK_AND_SLEEP_SERVICE_CLEAN
  fi
  {
    echo '[CL13][CLOCK_SLEEP_CLASSIFICATION]'
    echo "$classification."
    echo "boots=16 late_samples=$late_total degraded_profiles=$all_degraded tcg_degraded=$tcg_degraded smp4_kvm_degraded=$smp4_kvm_degraded smp8_kvm_degraded=$smp8_kvm_degraded hard_failures=0 protected_runtime=unchanged time_runtime=unchanged"
  } >"$out"
  ledger_record classification 0 host focused+continuity "$status" 0 "$out" \
    0 0 0 0 0 0 0 0 0 0 "$late_total"
  [[ $status == PASS ]] || { echo "$status" >&2; return 3; }
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

all_gate(){
  host_unit_gate
  static_gate
  group_gate up-tcg 1 tcg 2 0 1
  group_gate smp4-tcg 4 tcg 3 16 0
  group_gate smp4-kvm 4 kvm 3 16 0
  group_gate smp8-tcg 8 tcg 3 32 0
  group_gate smp8-kvm 8 kvm 5 32 0
  classification_gate
}

case ${1:-} in
  host-unit) host_unit_gate;;
  static) static_gate;;
  up-tcg) group_gate up-tcg 1 tcg 2 0 1;;
  smp4-tcg) group_gate smp4-tcg 4 tcg 3 16 0;;
  smp4-kvm) group_gate smp4-kvm 4 kvm 3 16 0;;
  smp8-tcg) group_gate smp8-tcg 8 tcg 3 32 0;;
  smp8-kvm) group_gate smp8-kvm 8 kvm 5 32 0;;
  classification) classification_gate;;
  all) all_gate;;
  *) usage;;
esac

trap - ERR EXIT
