#!/usr/bin/env bash
set -Eeuo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

artifact_dir=artifacts/build
ledger_tsv=$artifact_dir/cl13-ledger.tsv
ledger_json=$artifact_dir/cl13-ledger.json
ledger_log=$artifact_dir/cl13-ledger.log
source_before=$artifact_dir/cl13-v1-source-before.sha256
source_after_tests=$artifact_dir/cl13-v1-source-after-tests.sha256
source_after_build=$artifact_dir/cl13-v1-source-after-build.sha256
protected_fix4_before=$artifact_dir/cl13fix4-protected-runtime-before.sha256
protected_fix4_after_tests=$artifact_dir/cl13fix4-protected-runtime-after-tests.sha256
protected_fix4_after_build=$artifact_dir/cl13fix4-protected-runtime-after-build.sha256
protected_fix5_before=$artifact_dir/cl13fix5-protected-runtime-before.sha256
protected_fix5_after_tests=$artifact_dir/cl13fix5-protected-runtime-after-tests.sha256
protected_fix5_after_build=$artifact_dir/cl13fix5-protected-runtime-after-build.sha256
protected_fix7_before=$artifact_dir/cl13fix7-protected-runtime-before.sha256
protected_fix7_after_tests=$artifact_dir/cl13fix7-protected-runtime-after-tests.sha256
protected_fix7_after_build=$artifact_dir/cl13fix7-protected-runtime-after-build.sha256
protected_fix8_before=$artifact_dir/cl13fix8-protected-runtime-before.sha256
protected_fix8_after_tests=$artifact_dir/cl13fix8-protected-runtime-after-tests.sha256
protected_fix8_after_build=$artifact_dir/cl13fix8-protected-runtime-after-build.sha256
protected_fix10_before=$artifact_dir/cl13fix10-protected-runtime-before.sha256
protected_fix10_after_tests=$artifact_dir/cl13fix10-protected-runtime-after-tests.sha256
protected_fix10_after_build=$artifact_dir/cl13fix10-protected-runtime-after-build.sha256
time_fix10_before=$artifact_dir/cl13fix10-time-runtime-before.sha256
time_fix10_after_build=$artifact_dir/cl13fix10-time-runtime-after-build.sha256
expected_branch=feat/taskman
expected_base=4e700a9b38653fb57ecb0f107a18f87508db0736
serial=.qemu/qemu-serial.log
hmp=(python3 scripts/qemu_hmp.py --socket .qemu/hmp.sock)
harness_start_line=0
harness_command=cl13
current_stage=entry
mkdir -p "$artifact_dir"
source scripts/harness-common.sh
source scripts/harness-runtime-ready.sh
source scripts/harness-framed.sh
source scripts/harness-selftest.sh

usage(){
  echo "Usage: $0 preflight|static|selftest|matrix|matrix-resume-smp4|matrix-smp8-tcg|quantum|negatives|soak4|soak8|final-build|report|resume-after-boundary|resume-after-transport|resume-after-fixture257|resume-after-async-contract|resume-after-runtime-ready|resume-after-clock-sleep|resume-after-hpet-negative|resume-after-soak8-input-marker|resume-after-soak8-modal-baseline|resume-after-modal-global-heap-noise|resume-after-taskman-arm-marker|all" >&2
  exit 2
}

ensure_ledger(){
  if [[ ! -f $ledger_tsv ]]; then
    printf 'stage\tscenario\tsmp\taccel\tbuild_variant\tcommand\tstatus\tduration_ms\tartifact\thash\tearly_mask\tlate_mask\twait_error_mask\tlate_rounds\ttarget\tsamples\tp50\tp95\tp99\tmax\tlate_samples\thost_load_psi\n' >"$ledger_tsv"
  fi
  touch "$ledger_log"
}

record(){
  local stage=$1 scenario=$2 smp=$3 accel=$4 variant=$5 command=$6
  local status=$7 duration_ms=$8 artifact=${9:-} hash=-
  local early=${10:-0} late=${11:-0} wait_error=${12:-0}
  local late_rounds=${13:-0} target=${14:-0} samples=${15:-0}
  local p50=${16:-0} p95=${17:-0} p99=${18:-0} maximum=${19:-0}
  local late_samples=${20:-0} host_load_psi=${21:--}
  [[ $status == PASS || $status == FAIL || $status == SKIP || $status == BLOCKED_ENV ]]
  if [[ -n $artifact && -f $artifact ]]; then
    hash=$(sha256sum "$artifact" | awk '{print $1}')
  else
    artifact=-
  fi
  command=${command//$'\t'/ }
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$stage" "$scenario" "$smp" "$accel" "$variant" "$command" \
    "$status" "$duration_ms" "$artifact" "$hash" "$early" "$late" \
    "$wait_error" "$late_rounds" "$target" "$samples" "$p50" "$p95" \
    "$p99" "$maximum" "$late_samples" "$host_load_psi" >>"$ledger_tsv"
  printf '[CL13][LEDGER] stage=%s scenario=%s status=%s duration_ms=%s artifact=%s\n' \
    "$stage" "$scenario" "$status" "$duration_ms" "$artifact" | tee -a "$ledger_log"
}

now_ms(){ echo $(( $(date +%s%N) / 1000000 )); }

source_snapshot(){
  local output=$1
  local -a files=()
  mapfile -d '' -t files < <(
    {
      find kernel bootloader shared -type f \
        \( -name '*.c' -o -name '*.h' -o -name '*.S' -o -name '*.s' \
           -o -name '*.asm' -o -name '*.inc' -o -name '*.ld' \) -print0
      printf 'makefile\0'
    } | LC_ALL=C sort -z
  )
  sha256sum "${files[@]}" >"$output"
}

protected_fix4_snapshot(){
  local output=$1
  { rg --files kernel bootloader shared; printf '%s\n' makefile; } |
    LC_ALL=C sort |
    rg -v '^(kernel/src/shell/commands/cmd_taskmantest\.(c|h)|kernel/src/core/selftest\.c)$' |
    xargs sha256sum >"$output"
}

protected_fix4_check(){
  local output=$1
  [[ -f $protected_fix4_before ]]
  protected_fix4_snapshot "$output"
  cmp -s "$protected_fix4_before" "$output"
}

protected_fix5_snapshot(){
  local output=$1
  { rg --files kernel bootloader shared; printf '%s\n' makefile; } |
    LC_ALL=C sort |
    rg -v '^(kernel/src/shell/commands/cmd_schedtest\.(c|h)|kernel/src/core/selftest\.c)$' |
    xargs sha256sum >"$output"
}

protected_fix5_check(){
  local output=$1
  [[ -f $protected_fix5_before ]]
  protected_fix5_snapshot "$output"
  cmp -s "$protected_fix5_before" "$output"
}

protected_fix7_snapshot(){
  local output=$1
  { rg --files kernel bootloader shared; printf '%s\n' makefile; } |
    LC_ALL=C sort |
    rg -v '^kernel/src/core/selftest\.c$' |
    xargs sha256sum >"$output"
}

protected_fix7_check(){
  local output=$1
  [[ -f $protected_fix7_before ]]
  protected_fix7_snapshot "$output"
  cmp -s "$protected_fix7_before" "$output"
}

protected_fix8_snapshot(){
  local output=$1
  { rg --files kernel bootloader shared; printf '%s\n' makefile; } |
    LC_ALL=C sort |
    rg -v '^(makefile|kernel/kernel\.c|kernel/src/core/(kernel_init|runtime_ready|selftest|dpc)\.(c|h)|kernel/src/shell/shell\.(c|h)|kernel/src/shell/commands/cmd_(tasktest|synctest)\.(c|h)|kernel/src/drivers/usb/xhci/usb_hotplug\.(c|h))$' |
    xargs sha256sum >"$output"
}

protected_fix8_check(){
  local output=$1
  [[ -f $protected_fix8_before ]]
  protected_fix8_snapshot "$output"
  cmp -s "$protected_fix8_before" "$output"
}

protected_fix10_snapshot(){
  local output=$1
  { { rg --files kernel bootloader shared; printf '%s\n' makefile; } |
      LC_ALL=C sort |
      rg -v '^(kernel/src/shell/commands/cmd_accounttest\.(c|h)|kernel/src/core/selftest\.c)$' |
      xargs sha256sum; } >"$output"
}

time_fix10_snapshot(){
  sha256sum \
    kernel/src/core/clock.c kernel/src/core/clock.h \
    kernel/src/drivers/timer.c kernel/src/drivers/timer.h \
    kernel/src/core/timers.c kernel/src/core/timers.h \
    kernel/src/timer/hpet.c kernel/src/timer/hpet.h \
    kernel/src/apic/lapic.c kernel/src/apic/lapic.h \
    kernel/src/core/scheduler.c kernel/src/core/scheduler.h >"$1"
}

protected_fix10_check(){
  local output=$1
  [[ -f $protected_fix10_before ]]
  protected_fix10_snapshot "$output"
  cmp -s "$protected_fix10_before" "$output"
}

time_fix10_check(){
  local output=$1
  [[ -f $time_fix10_before ]]
  time_fix10_snapshot "$output"
  cmp -s "$time_fix10_before" "$output"
}

source_continuity(){
  local before=$1 after=$2 changed
  changed=$(diff -U0 "$before" "$after" 2>/dev/null |
    sed -n -E 's/^[+-][0-9a-f]{64}  (.*)$/\1/p' | sort -u || true)
  while IFS= read -r file; do
    [[ -z $file ]] && continue
    case "$file" in
      makefile|kernel/src/core/selftest.c|kernel/src/core/selftest.h|\
      kernel/src/core/semaphore.c|kernel/src/core/semaphore.h|\
      kernel/src/core/kernel_init.c|kernel/src/core/scheduler.c|\
      kernel/src/core/runtime_ready.c|kernel/src/core/runtime_ready.h|\
      kernel/src/core/dpc.c|kernel/src/core/dpc.h|\
      kernel/src/core/scheduler.h|kernel/src/core/task_format.c|\
      kernel/src/core/task_format.h|\
      kernel/src/shell/shell.c|kernel/src/shell/shell.h|\
      kernel/src/shell/commands/registry.c|\
      kernel/src/shell/commands/cmd_tasktest.c|\
      kernel/src/shell/commands/cmd_tasktest.h|\
      kernel/src/shell/commands/cmd_schedtest.c|\
      kernel/src/shell/commands/cmd_schedtest.h|\
      kernel/src/shell/commands/cmd_synctest.c|\
      kernel/src/shell/commands/cmd_synctest.h|\
      kernel/src/shell/commands/cmd_accounttest.c|\
      kernel/src/shell/commands/cmd_accounttest.h|\
      kernel/src/shell/commands/cmd_killtest.c|\
      kernel/src/shell/commands/cmd_killtest.h|\
      kernel/src/shell/commands/cmd_reaptest.c|\
      kernel/src/shell/commands/cmd_reaptest.h|\
      kernel/src/shell/commands/cmd_inputtest.c|\
      kernel/src/shell/commands/cmd_inputtest.h|\
      kernel/src/shell/commands/cmd_modaltest.c|\
      kernel/src/shell/commands/cmd_modaltest.h|\
      kernel/src/shell/commands/cmd_taskmantest.c|\
      kernel/src/shell/commands/cmd_taskmantest.h|\
      kernel/src/shell/commands/cmd_taskdiag.c|\
      kernel/src/shell/commands/cmd_taskdiag.h|\
      kernel/src/drivers/usb/xhci/usb_hotplug.c|\
      kernel/src/drivers/usb/xhci/usb_hotplug.h|kernel/kernel.c) ;;
      *) echo "unauthorized source continuity change: $file" >&2; return 1;;
    esac
  done <<<"$changed"
}

cleanup(){ stop_qemu; }
trap cleanup EXIT
error_cleanup(){
  local rc=$?
  stop_qemu
  return "$rc"
}
trap error_cleanup ERR

preflight(){
  current_stage=preflight
  local begin end
  begin=$(now_ms)
  [[ $(git branch --show-current) == "$expected_branch" ]]
  [[ $(git rev-parse HEAD) == "$expected_base" ]]
  git diff --check
  [[ ! -e .qemu/qemu.pid && ! -S .qemu/hmp.sock ]]
  [[ -f $source_before ]]
  [[ -r /dev/kvm && -w /dev/kvm ]] || {
    ensure_ledger
    record preflight kvm 0 kvm environment '/dev/kvm rw' BLOCKED_ENV 0 /dev/null
    return 3
  }
  ensure_ledger
  {
    echo "date=$(date --iso-8601=seconds)"
    echo "branch=$(git branch --show-current)"
    echo "base=$(git rev-parse HEAD)"
    echo "qemu=$(${QEMU:-qemu-system-x86_64} --version | head -1)"
    echo "kvm_readable=$([[ -r /dev/kvm ]] && echo 1 || echo 0)"
    echo "kvm_writable=$([[ -w /dev/kvm ]] && echo 1 || echo 0)"
    echo "cpus_allowed=$(awk '/Cpus_allowed_list/{print $2}' /proc/self/status)"
    if [[ -r /sys/fs/cgroup/cpu.max ]]; then
      echo "cpu_max=$(tr '\n' ' ' </sys/fs/cgroup/cpu.max)"
    else
      echo "cpu_max=unavailable"
    fi
    if [[ -r /sys/fs/cgroup/cpuset.cpus.effective ]]; then
      echo "cpuset=$(tr '\n' ' ' </sys/fs/cgroup/cpuset.cpus.effective)"
    else
      echo "cpuset=unavailable"
    fi
    echo "loadavg=$(tr '\n' ' ' </proc/loadavg)"
    echo "cpu_psi=$(tr '\n' ';' </proc/pressure/cpu 2>/dev/null || echo unavailable)"
  } >"$artifact_dir/cl13-environment.log"
  end=$(now_ms)
  record preflight baseline 0 host source "$0 preflight" PASS \
    "$((end-begin))" "$artifact_dir/cl13-environment.log"
}

owned_warnings(){
  local unexpected
  unexpected=$(grep -E \
    'kernel/src/(core/(selftest|scheduler|semaphore)|shell/(shell|commands/(cmd_tasktest|cmd_taskmantest|cmd_schedtest|cmd_synctest|cmd_accounttest|registry)))\.c:.*warning:' \
    "$1" | grep -Fv \
    "kernel/src/shell/shell.c:327:13: warning: ‘shell_delete’ defined but not used" \
    || true)
  [[ -z $unexpected ]]
}

static_gate(){
  current_stage=static
  ensure_ledger
  local begin end jobs
  begin=$(now_ms)
  jobs=$(nproc)
  make clean
  make stack-check JOBS=2
  cp artifacts/build/kernel-check-j2.log "$artifact_dir/cl13-build-stack.log"
  make stack-check JOBS=2 SELFTEST=1 SELFTEST_AUTORUN=1
  cp artifacts/build/kernel-check-j2.log \
    "$artifact_dir/cl13-build-stack-selftest.log"

  make kernel-check JOBS=2
  cp kernel.elf /tmp/hobbyos-cl13-static-j2.elf
  cp artifacts/build/kernel-check-j2.log "$artifact_dir/cl13-build-release-j2.log"
  make kernel-check JOBS="$jobs"
  cp kernel.elf /tmp/hobbyos-cl13-static-jN.elf
  cp "artifacts/build/kernel-check-j${jobs}.log" "$artifact_dir/cl13-build-release-jN.log"
  cmp -s /tmp/hobbyos-cl13-static-j2.elf /tmp/hobbyos-cl13-static-jN.elf
  sha256sum /tmp/hobbyos-cl13-static-j2.elf /tmp/hobbyos-cl13-static-jN.elf \
    >"$artifact_dir/cl13-static-build-hashes.log"
  make deps-check
  make image
  [[ -z $(nm -u kernel.elf) ]]
  ! strings kernel.elf | grep -Fq '[SELFTEST][AUTORUN] BEGIN'

  make kernel-check JOBS=2 SELFTEST=1
  cp artifacts/build/kernel-check-j2.log "$artifact_dir/cl13-build-selftest.log"
  nm kernel.elf | grep -E 'selftest_run_all|cmd_tasktest|selftest_autorun_if_enabled' \
    >"$artifact_dir/cl13-selftest-symbols.log"
  make kernel-check JOBS=2 SELFTEST=1 SELFTEST_AUTORUN=1
  cp artifacts/build/kernel-check-j2.log "$artifact_dir/cl13-build-autorun.log"
  nm kernel.elf | grep -E 'selftest_run_all|cmd_tasktest|selftest_autorun_if_enabled|shell_execute_command_line_for_selftest' \
    >>"$artifact_dir/cl13-selftest-symbols.log"

  if rg -n '(: warning:|: error:).*(implicit declaration|incompatible pointer|int-conversion)' \
      "$artifact_dir"/cl13-build-*.log; then return 1; fi
  grep -F \
    "kernel/src/shell/shell.c:327:13: warning: ‘shell_delete’ defined but not used" \
    "$artifact_dir/cl13-build-selftest.log" \
    "$artifact_dir/cl13-build-autorun.log" \
    >"$artifact_dir/cl13-preexisting-warning-baseline.log" || true
  owned_warnings "$artifact_dir/cl13-build-selftest.log"
  owned_warnings "$artifact_dir/cl13-build-autorun.log"
  ! rg -n 'strcmp\([^\n]*name[^\n]*(idle|reaper|shell)' \
    kernel/src/core kernel/src/shell
  rg -n 'task_t[[:space:]]*\*' kernel/src/core/timers.* \
    kernel/src/core/task_lifecycle.* kernel/src/core/dpc.* \
    >"$artifact_dir/cl13-lifetime-audit.log" || true
  rg -n 'scheduler_global_init|scheduler_cpu_init' \
    kernel/src/core/kernel_init.c kernel/src/smp \
    >"$artifact_dir/cl13-bootstrap-audit.log"
  bash -n scripts/run-qemu-selftest.sh
  bash -n scripts/test-taskman-v1-negatives.sh
  bash -n scripts/test-taskman-v1-soak.sh
  bash -n scripts/test-taskman-v1.sh
  bash -n scripts/harness-framed.sh
  bash -n scripts/harness-runtime-ready.sh
  bash -n scripts/harness-selftest.sh
  bash -n scripts/test-cl13-transport-focused.sh
  bash -n scripts/test-cl13-fixture257-focused.sh
  bash -n scripts/test-cl13-async-contract-focused.sh
  bash -n scripts/test-cl13-runtime-ready-focused.sh
  bash -n scripts/test-cl13-harness-record-focused.sh
  bash -n scripts/test-cl13-synctest-async-focused.sh
  bash -n scripts/test-cl13-clock-contract-focused.sh
  bash -n scripts/test-cl13-clock-sleep-contract-focused.sh
  rg -n 'shell_execute_command_line_for_selftest' kernel/src/shell/shell.* \
    >"$artifact_dir/cl13-transport-static.log"
  rg -n 'TASKTEST_HARNESS_RECORD_MAX|harness_record_(begin|emit)' \
    kernel/src/shell/commands/cmd_tasktest.c \
    >>"$artifact_dir/cl13-transport-static.log"
  for record_kind in FRAME BEGIN END REPLAY STATUS; do
    rg -n "harness_record_begin\\(&record, \"${record_kind}\"\\)" \
      kernel/src/shell/commands/cmd_tasktest.c \
      >>"$artifact_dir/cl13-transport-static.log"
  done
  rg -n 'source scripts/harness-framed.sh' \
    scripts/run-qemu-selftest.sh scripts/test-taskman-v1.sh \
    scripts/test-taskman-v1-soak.sh scripts/test-taskman-v1-negatives.sh \
    scripts/test-cl13-taskman-anchor-focused.sh \
    >>"$artifact_dir/cl13-transport-static.log"
  rg -n 'source scripts/harness-runtime-ready.sh' \
    scripts/run-qemu-selftest.sh scripts/test-taskman-v1.sh \
    scripts/test-taskman-v1-soak.sh scripts/test-taskman-v1-negatives.sh \
    scripts/test-cl13-runtime-ready-focused.sh \
    scripts/test-cl13-harness-record-focused.sh \
    scripts/test-cl13-synctest-async-focused.sh \
    >>"$artifact_dir/cl13-transport-static.log"
  python3 -m py_compile scripts/parse-selftest-log.py

  make kernel-check JOBS=2
  make image
  [[ -z $(nm -u kernel.elf) ]]
  end=$(now_ms)
  record static consolidated 0 host release+selftest "$0 static" PASS \
    "$((end-begin))" "$artifact_dir/cl13-build-autorun.log"
}

selftest_gate(){
  current_stage=selftest
  ensure_ledger
  local begin end
  begin=$(now_ms)
  scripts/run-qemu-selftest.sh --smp 1 --accel tcg --quantum default \
    --autorun --tasktest --timeout 300 --prefix cl13-selftest-up-tcg
  end=$(now_ms)
  record selftest autorun-up 1 tcg selftest-autorun \
    'tasktest all' PASS "$((end-begin))" \
    "$artifact_dir/cl13-selftest-up-tcg.log"
}

sleep_profile_set_gate(){
  local accel=$1 line
  send_complete "accounttest sleep-profile-set" \
    "[ACCOUNT][SLEEP_PROFILE_SET]" 1800 stress
  line=$(grep -F '[ACCOUNT][SLEEP_PROFILE_SET]' "$serial" | tail -1)
  if [[ $line == *' FAIL '* ]]; then
    echo REJECTED_RUNTIME_DEFECT_CONFIRMED >&2
    return 1
  fi
  if [[ $line == *' DEGRADED '* ]]; then
    if [[ $accel == kvm ]]; then
      echo BLOCKED_BY_EXTERNAL_ENVIRONMENT >&2
    else
      echo REJECTED_CL13_SLEEP_SERVICE_PERSISTENT >&2
    fi
    return 1
  fi
  [[ $line == *' PASS '* ]]
}

common_checks(){
  local accel=$1
  send_complete "tasktest all" "[SELFTEST][SUMMARY]" 300 stress
  send_complete "accounttest clock" "[ACCOUNT][CLOCK] PASS" 180 stress
  sleep_profile_set_gate "$accel"
  subsystem_checks
}

subsystem_checks(){
  send_complete "taskmantest fixture-capacity" \
    "[TASKMANTEST][FIXTURE_CAPACITY] PASS max=257" 90 stress
  send_complete "taskmantest fixture-status" \
    "[TASKMANTEST][FIXTURE_STATUS] PASS max=257 active=0 present=0 gate_release=0 cleanup=0" \
    90 stress
  send_complete "accounttest lapic-config" "[ACCOUNT][LAPIC_CONFIG] PASS" 120 stress
  send_complete "accounttest lapic-liveness 500" "[ACCOUNT][LAPIC_LIVENESS] PASS" 180 stress
  send_complete "schedtest check" "[SCHED][CHECK] PASS" 90 stress
  send_complete "synctest check" "[SYNC][CHECK] PASS" 90 stress
  send_complete "accounttest check" "[ACCOUNT][CHECK] PASS" 180 stress
  send_complete "killtest check" "[KILLTEST][CHECK] PASS" 90 stress
  send_complete "reaptest check" "[REAPTEST][CHECK] PASS" 90 stress
  send_complete "inputtest check" "[INPUTTEST][CHECK] PASS" 90 stress
  send_complete "modaltest check" "[MODALTEST][CHECK] PASS" 90 stress
  send_complete "taskmantest check" "[TASKMANTEST][CHECK] PASS" 90 stress
  send_complete "taskdiag check" "[TASKDIAG][CHECK] PASS" 90 stress
  transport_status_check
}

transport_status_check(){
  local line
  send_complete "tasktest transport-status" "[HARNESS][STATUS]" 90 stress
  line=$(grep -F '[HARNESS][STATUS]' "$serial" | tail -1)
  [[ $line =~ accepted=([0-9]+)[[:space:]]completed=([0-9]+) ]]
  [[ ${BASH_REMATCH[1]} == "${BASH_REMATCH[2]}" ]]
}

taskman_wait_marker(){
  local pattern=$1 before=$2 timeout=$3 deadline fresh
  deadline=$((SECONDS+timeout))
  while ((SECONDS<=deadline)); do
    (( $(count "$pattern") > before )) && return 0
    fresh=$(tail -n +$((harness_start_line+1)) "$serial" 2>/dev/null || true)
    grep -Eq 'PANIC|#PF|#GP|FATAL|STRUCTURAL_FAULT|FINISH_FAULT' <<<"$fresh" && return 2
    pgrep -f qemu-system-x86_64 >/dev/null || return 2
    sleep 1
  done
  return 3
}

taskman_wait_auto_exit(){
  local pattern=$1 before=$2 frames=$3 timeout=$4 deadline fresh
  deadline=$((SECONDS+timeout))
  while ((SECONDS<=deadline)); do
    (( $(count "$pattern") > before )) && return 0
    fresh=$(tail -n +$((harness_start_line+1)) "$serial" 2>/dev/null || true)
    grep -Fq "[TASKMAN][AUTO_EXIT] FAIL target=$frames" <<<"$fresh" && return 1
    grep -Eq '(^|[[:space:]])FAIL([[:space:]]|$)|PANIC|#PF|#GP|FATAL|STRUCTURAL_FAULT|FINISH_FAULT' <<<"$fresh" && return 1
    pgrep -f qemu-system-x86_64 >/dev/null || return 2
    sleep 1
  done
  return 3
}

taskman_stat_value(){
  local line=$1 key=$2 regex
  regex="(^|[[:space:]])${key}=([0-9]+)($|[[:space:]])"
  [[ $line =~ $regex ]]
  printf '%s\n' "${BASH_REMATCH[2]}"
}

taskman_stat_mode(){
  local line=$1
  [[ $line =~ (^|[[:space:]])last_mode=([^[:space:]]+)($|[[:space:]]) ]]
  printf '%s\n' "${BASH_REMATCH[2]}"
}

taskman_failure_diagnose(){
  local refresh=$1 frames=$2 end_before=$3 outcome=$4
  local stats mode full fallback shortfalls model_live=0 classification
  local failure_log=$artifact_dir/cl13-taskman-failures.log rc

  if ((outcome == 2)) || ! pgrep -f qemu-system-x86_64 >/dev/null; then
    classification=TASKMAN_GUEST_FAULT
    printf '[CL13][TASKMAN_FAILURE] class=%s refresh=%s target=%s\n' \
      "$classification" "$refresh" "$frames" | tee -a "$failure_log" >&2
    return 1
  fi

  if (( $(count "[MODAL] session_end OK") <= end_before )); then
    hmp_key esc normal >/dev/null
    hmp_key esc normal >/dev/null
    hmp_key esc normal >/dev/null
    if ! taskman_wait_marker "[MODAL] session_end OK" "$end_before" 120; then
      classification=TASKMAN_RENDER_STALL
      printf '[CL13][TASKMAN_FAILURE] class=%s refresh=%s target=%s stats=unavailable shell_recovery=FAIL\n' \
        "$classification" "$refresh" "$frames" | tee -a "$failure_log" >&2
      return 1
    fi
  fi
  if ! framed_finish_started "[MODAL] session_end OK" 120 0; then
    classification=TASKMAN_RENDER_STALL
    printf '[CL13][TASKMAN_FAILURE] class=%s refresh=%s target=%s frame_end=FAIL\n' \
      "$classification" "$refresh" "$frames" | tee -a "$failure_log" >&2
    return 1
  fi
  if ! shell_sync; then
    classification=TASKMAN_RENDER_STALL
    printf '[CL13][TASKMAN_FAILURE] class=%s refresh=%s target=%s stats=unavailable shell_recovery=FAIL\n' \
      "$classification" "$refresh" "$frames" | tee -a "$failure_log" >&2
    return 1
  fi
  if ! send_complete "taskmantest stats" "[TASKMANTEST][STATS]" 90 stress; then
    classification=TASKMAN_GUEST_FAILURE
    printf '[CL13][TASKMAN_FAILURE] class=%s refresh=%s target=%s stats=unavailable\n' \
      "$classification" "$refresh" "$frames" | tee -a "$failure_log" >&2
    return 1
  fi
  stats=$(grep -F '[TASKMANTEST][STATS]' "$serial" | tail -1)
  mode=$(taskman_stat_mode "$stats")
  full=$(taskman_stat_value "$stats" last_session_full_frames)
  fallback=$(taskman_stat_value "$stats" last_session_fallback_frames)
  shortfalls=$(taskman_stat_value "$stats" auto_exit_shortfalls)
  if [[ ( $mode == TOO_SHORT || $mode == TOO_NARROW ) && $fallback -gt 0 ]]; then
    classification=TASKMAN_LAYOUT_FALLBACK
  elif [[ ( $mode == WIDE || $mode == COMPACT ) && $full -eq 0 ]]; then
    classification=TASKMAN_RENDER_STALL
  else
    classification=TASKMAN_GUEST_FAILURE
  fi
  printf '[CL13][TASKMAN_FAILURE] class=%s refresh=%s target=%s last_mode=%s full_frames=%s fallback_frames=%s shortfalls=%s model_live=%s shell_recovery=PASS\n' \
    "$classification" "$refresh" "$frames" "$mode" "$full" "$fallback" \
    "$shortfalls" "$model_live" | tee -a "$failure_log" >&2
  return 1
}

auto_taskman(){
  local refresh=$1 frames=$2 expected before end_before outcome rc
  send_complete "taskmantest anchor-reset" \
    "[TASKMANTEST][ANCHOR_RESET] PASS" 60 stress
  send_complete "taskmantest auto-exit-frames $frames" \
    "[TASKMANTEST][AUTO_EXIT_ARM] PASS frames=$frames" 90 stress
  expected="[TASKMAN][AUTO_EXIT] PASS target=$frames full_frames=$frames fallback_frames=0"
  before=$(count "$expected")
  end_before=$(count "[MODAL] session_end OK")
  harness_command="taskman $refresh"
  harness_start_line=$(wc -l <"$serial" 2>/dev/null || echo 0)
  if framed_send_complete "taskman $refresh" "$expected" 180 stress; then
    return 0
  else
    outcome=$?
  fi
  if [[ $HARNESS_FRAME_LAST_CLASSIFICATION == HMP_FRAME_TRANSPORT_FAILURE ||
        $HARNESS_FRAME_LAST_CLASSIFICATION == HMP_FRAME_CORRUPTION_PERSISTENT ||
        $HARNESS_FRAME_LAST_CLASSIFICATION == TASKMAN_GUEST_FAULT ]]; then
    return 1
  fi
  taskman_failure_diagnose "$refresh" "$frames" "$end_before" "$outcome"
  rc=$?
  return "$rc"
}

sync_all_expanded(){
  framed_synctest_async SEM "synctest sem 20000" \
    "[SYNC][SEM] START" "[SYNC][SEM] PASS" 300 stress
  framed_synctest_async BOUNDARY "synctest boundary 5000" \
    "[SYNC][BOUNDARY] START" "[SYNC][BOUNDARY] PASS" 300 stress
  framed_synctest_async RACE "synctest race 2000" \
    "[SYNC][RACE] START" "[SYNC][RACE] PASS" 300 stress
  framed_synctest_async SLEEP "synctest sleep" \
    "[SYNC][SLEEP] START" "[SYNC][SLEEP] PASS" 120 stress
  framed_synctest_async CANCEL "synctest cancel" \
    "[SYNC][CANCEL] START" "[SYNC][CANCEL] PASS" 120 stress
  framed_synctest_async TIMER_CANCEL "synctest timer-cancel" \
    "[SYNC][TIMER_CANCEL] START" "[SYNC][TIMER_CANCEL] PASS" 120 stress
  send_complete "synctest oom" "[SYNC][OOM] PASS" 120 stress
  send_complete "synctest fallback" "[SYNC][FALLBACK] PASS" 120 stress
  send_complete "synctest stale" "[SYNC][STALE] PASS" 120 stress
}

account_core_without_rate(){
  send_complete "accounttest lapic-config" "[ACCOUNT][LAPIC_CONFIG] PASS" 120 stress
  send_complete "accounttest lapic-liveness 500" "[ACCOUNT][LAPIC_LIVENESS] PASS" 180 stress
  send_complete "accounttest sampler" "[ACCOUNT][SELFTEST] SAMPLER_GUARDS_OK" 90 stress
  send_complete "accounttest format" "[ACCOUNT][SELFTEST] FORMAT_GUARDS_OK" 90 stress
  send_complete "accounttest long" "[ACCOUNT][LONG] PASS" 90 stress
  send_complete "accounttest lifecycle" "[ACCOUNT][LIFECYCLE] PASS" 180 stress
}

scenario_up(){
  send_complete "tasktest identity" "[SELFTEST][SUMMARY]" 120
  send_complete "tasktest format" "[SELFTEST][SUMMARY]" 120
  send_complete "tasktest registry" "[SELFTEST][SUMMARY]" 120
  send_complete "ps" "[PS][SNAPSHOT]" 90
  sleep 1
  send_complete "ps" "[PS][SNAPSHOT]" 90
  send_complete "taskdiag summary" "[TASKDIAG][SUMMARY] PASS" 90
  send_complete "taskdiag all" "[TASKDIAG][ALL] PASS" 180
  framed_synctest_async SLEEP "synctest sleep" \
    "[SYNC][SLEEP] START" "[SYNC][SLEEP] PASS" 120 stress
  send_complete "killtest ready" "[KILLTEST][READY] PASS" 180
  send_complete "reaptest normal 32" "[REAPTEST][NORMAL] PASS created=32" 180
  send_complete "inputtest queue" "[INPUTTEST][QUEUE] PASS" 120
  send_complete "modaltest false-token" "[MODALTEST][FALSE_TOKEN] PASS" 120
  send_complete "taskmantest layout" "[TASKMANTEST][LAYOUT] PASS" 90
}

scenario_smp2(){
  framed_schedtest_async YIELD "schedtest yield 2 10000" \
    "[SCHED][STRESS] YIELD_START" \
    "[SCHED][STRESS] YIELD_COMPLETE" 600 stress
  framed_synctest_async SEM "synctest sem 20000" \
    "[SYNC][SEM] START" "[SYNC][SEM] PASS" 300 stress
  framed_synctest_async BOUNDARY "synctest boundary 5000" \
    "[SYNC][BOUNDARY] START" "[SYNC][BOUNDARY] PASS" 300 stress
  framed_synctest_async RACE "synctest race 2000" \
    "[SYNC][RACE] START" "[SYNC][RACE] PASS" 300 stress
  send_complete "accounttest share" "[ACCOUNT][SHARE] PASS" 180 stress
  send_complete "killtest running" "[KILLTEST][RUNNING] PASS" 180 stress
  send_complete "killtest sleeping" "[KILLTEST][SLEEPING] PASS" 180 stress
  send_complete "reaptest snapshot" "[REAPTEST][SNAPSHOT] PASS" 300 stress
  send_complete "inputtest begin-boundary" "[INPUTTEST][BEGIN_BOUNDARY]" 180 stress
  send_complete "modaltest second-session" "[MODALTEST][SECOND] PASS" 180 stress
  send_complete "taskmantest setup 50" "[TASKMANTEST][SETUP] PASS count=50" 180 stress
  auto_taskman 50 10
  send_complete "taskmantest cleanup" "[TASKMANTEST][CLEANUP] PASS removed=50" 180 stress
}

task_count_variants(){
  local count refresh
  for count in 1 20 50 129 257; do
    send_complete "taskmantest setup $count" \
      "[TASKMANTEST][SETUP] PASS count=$count" 240 stress
    send_complete "ps 1 128" "[PS][SNAPSHOT]" 120 stress
    refresh=50
    [[ $count == 20 ]] && refresh=1000
    [[ $count == 50 ]] && refresh=2000
    auto_taskman "$refresh" 3
    send_complete "taskmantest cleanup" \
      "[TASKMANTEST][CLEANUP] PASS removed=$count" 240 stress
    send_complete "taskmantest fixture-status" \
      "[TASKMANTEST][FIXTURE_STATUS] PASS max=257 active=0 present=0" \
      90 stress
  done
  send_complete "taskmantest pagination" \
    "[TASKMANTEST][PAGINATION] PASS cases=0,1,20,50,128,129,257" 120
}

scenario_smp4(){
  local accel=$1
  framed_schedtest_async YIELD "schedtest yield 4 20000" \
    "[SCHED][STRESS] YIELD_START" \
    "[SCHED][STRESS] YIELD_COMPLETE" 900 stress
  sync_all_expanded
  account_core_without_rate
  send_complete "accounttest allcpu" "[ACCOUNT][ALLCPU] PASS" 180 stress
  send_complete "accounttest migrate" "[ACCOUNT][MIGRATE] PASS" 180 stress
  send_complete "killtest all" "[KILLTEST][ALL] PASS" 600 stress
  send_complete "reaptest all" "[REAPTEST][CHECK] PASS" 900 stress
  send_complete "inputtest all" "[INPUTTEST][CHECK] PASS" 1200 stress
  send_complete "modaltest all" "[MODALTEST][ALL] PASS" 1200 stress
  task_count_variants
  send_complete "taskmantest churn 5000" \
    "[TASKMANTEST][CHURN] PASS rounds=5000" 180 stress
}

scenario_smp8(){
  framed_schedtest_async YIELD "schedtest yield 8 20000" \
    "[SCHED][STRESS] YIELD_START" \
    "[SCHED][STRESS] YIELD_COMPLETE" 1200 stress
  # LAUNCH_ONLY: spawn is the frame contract; kill sweep owns completion.
  framed_send_launch "smpstress 16 0 1000" \
    "[SMP] smpstress spawning workers=16" 90 stress
  send_complete "killtest smpstress-sweep" \
    "[SMP][KILL_SWEEP] PASS workers=16" 900 stress
  send_complete "synctest preblock-loop 500 all" \
    "[SYNCTEST][PREBLOCK_LOOP] PASS count=500" 1800 stress
  send_complete "synctest preblock-timer-noise 500" \
    "[SYNCTEST][PREBLOCK_TIMER_NOISE] PASS count=500" 1800 stress
  send_complete "accounttest clock-smp 32 1000000" \
    "[ACCOUNT][CLOCK_SMP] PASS" 1800 stress
  send_complete "killtest timeout-race 5000" \
    "[KILLTEST][TIMEOUT_RACE] PASS rounds=5000" 2400 stress
  send_complete "reaptest concurrent-churn 1000 1000 1000" \
    "[REAPTEST][CONCURRENT_CHURN] PASS" 2400 stress
  send_complete "reaptest create-handle-race 100000" \
    "[REAPTEST][CREATE_HANDLE_RACE] PASS created=100000" 3600 stress
  send_complete "reaptest timer-ref-loop 500" \
    "[REAPTEST][TIMER_REF_LOOP] PASS count=500" 2400 stress
  send_complete "inputtest producers 100000" \
    "[INPUTTEST][PRODUCERS] PASS accepted=100000" 900 stress
  send_complete "inputtest transitions 10000" \
    "[INPUTTEST][TRANSITIONS] PASS cycles=10000" 900 stress
  send_complete "modaltest kill-caller-loop 500 blocked" \
    "[MODALTEST][KILL_CALLER_LOOP] PASS mode=BLOCKED count=500" 3600 stress
  send_complete "modaltest kill-caller-loop 500 prewait" \
    "[MODALTEST][KILL_CALLER_LOOP] PASS mode=PREWAIT count=500" 3600 stress
  send_complete "modaltest kill-caller-loop 500 completion-ready" \
    "[MODALTEST][KILL_CALLER_LOOP] PASS mode=COMPLETION_READY count=500" 3600 stress
  send_complete "taskmantest stress 10000 10000 100 100 100" \
    "[TASKMANTEST][STRESS] PASS navigation=10000 churn=10000 zombies=100 kill_render=100 layouts=100" \
    3600 stress
}

parse_boot_autorun(){
  selftest_validate_autorun "$serial" "$artifact_dir/$1" \
    accounting.sleep.minimum_deadline \
    accounting.sleep.one_ms_granularity \
    accounting.sleep.late_is_diagnostic \
    accounting.sleep.wait_result \
    accounting.sleep.percentiles
}

record_sleep_profiles(){
  local scenario=$1 smp=$2 accel=$3 log=$4 line target samples p50 p95 p99
  local maximum late_samples status
  while IFS= read -r line; do
    [[ $line =~ target_ms=([0-9]+) ]] && target=${BASH_REMATCH[1]}
    [[ $line =~ samples=([0-9]+) ]] && samples=${BASH_REMATCH[1]}
    [[ $line =~ p50=([0-9]+) ]] && p50=${BASH_REMATCH[1]}
    [[ $line =~ p95=([0-9]+) ]] && p95=${BASH_REMATCH[1]}
    [[ $line =~ p99=([0-9]+) ]] && p99=${BASH_REMATCH[1]}
    [[ $line =~ max=([0-9]+) ]] && maximum=${BASH_REMATCH[1]}
    [[ $line =~ late_soft=([0-9]+) ]] && late_samples=${BASH_REMATCH[1]}
    status=PASS
    [[ $line == *' DEGRADED '* ]] && status=FAIL
    [[ $line == *' FAIL '* ]] && status=FAIL
    record matrix "$scenario-profile-$target" "$smp" "$accel" selftest \
      sleep-profile "$status" 0 "$log" 0 0 0 0 "$target" "$samples" \
      "$p50" "$p95" "$p99" "$maximum" "$late_samples"
  done < <(grep -E '^\[ACCOUNT\]\[SLEEP_PROFILE\] (PASS|DEGRADED|FAIL) ' "$log")
}

matrix_one(){
  local id=$1 smp=$2 accel=$3 begin end log
  begin=$(now_ms)
  HARNESS_ASYNC_RECORD_LOG="$artifact_dir/cl13-matrix-${id}-async.log"
  : >"$HARNESS_ASYNC_RECORD_LOG"
  start_qemu "$smp" "$accel"
  parse_boot_autorun "cl13-matrix-${id}"
  [[ $(grep -Fc '[BOOT][IDLE] PASS' "$serial") == "$smp" ]]
  [[ $(grep -Fc '[BOOT][CPU_INIT] PASS' "$serial") == "$smp" ]]
  grep -Fq '[BOOT][SHELL_READY] PASS' "$serial"
  common_checks "$accel"
  case "$id" in
    up-tcg) scenario_up;;
    smp2-*) scenario_smp2;;
    smp4-*) scenario_smp4 "$accel";;
    smp8-*) scenario_smp8;;
  esac
  subsystem_checks
  log="$artifact_dir/cl13-matrix-${id}.log"
  cp "$serial" "$log"
  assert_clean_log "$log"
  stop_qemu
  end=$(now_ms)
  record_sleep_profiles "$id" "$smp" "$accel" "$log"
  record matrix "$id" "$smp" "$accel" selftest-autorun \
    'scenario + subsystem checks' PASS "$((end-begin))" "$log"
  unset HARNESS_ASYNC_RECORD_LOG
}

matrix_latest_pass(){
  local id=$1 status artifact expected_hash actual_hash
  read -r status artifact expected_hash < <(awk -F '\t' -v wanted="$id" '
    $1 == "matrix" && $2 == wanted {
      status=$7; artifact=$9; hash=$10
    }
    END { print status, artifact, hash }
  ' "$ledger_tsv")
  [[ $status == PASS && -f $artifact && $expected_hash != - ]] || return 1
  actual_hash=$(sha256sum "$artifact" | awk '{print $1}')
  [[ $actual_hash == "$expected_hash" ]] || return 1
  grep -Fq '[ACCOUNT][CLOCK] PASS' "$artifact" || return 1
  grep -Fq '[ACCOUNT][SLEEP_PROFILE_SET] PASS profiles=3 hard_failures=0 degraded_profiles=0' \
    "$artifact" || return 1
}

matrix_one_unless_pass(){
  local id=$1 smp=$2 accel=$3
  if [[ ${CL13_FORCE_MATRIX:-0} != 1 ]] && matrix_latest_pass "$id"; then
    printf '[CL13][MATRIX] REUSE scenario=%s status=PASS\n' "$id"
    return 0
  fi
  matrix_one "$id" "$smp" "$accel"
}

matrix_recover_smp2_kvm(){
  local id=smp2-kvm log=$artifact_dir/cl13-matrix-smp2-kvm.log
  local legacy_prefix=$artifact_dir/cl13-matrix-smp2-kvm-legacy
  [[ ${CL13_FORCE_MATRIX:-0} != 1 ]] || return 0
  matrix_latest_pass "$id" && return 0
  [[ -f $log ]] || return 0
  if ! (
    [[ $(grep -Fc '[BOOT][IDLE] PASS' "$log") == 2 ]]
    [[ $(grep -Fc '[BOOT][CPU_INIT] PASS' "$log") == 2 ]]
    grep -Fq '[BOOT][SHELL_READY] PASS' "$log"
    selftest_extract_autorun "$log" "${legacy_prefix}-selftest.log"
    python3 scripts/parse-selftest-log.py \
      --log "${legacy_prefix}-selftest.log" \
      --json "${legacy_prefix}-selftest.json" \
      --markdown "${legacy_prefix}-selftest.md" >/dev/null
    grep -Fq '[SCHED][STRESS] YIELD_COMPLETE workers=2 iterations=10000' "$log"
    grep -Fq '[SYNC][SEM] PASS iterations=20000' "$log"
    grep -Eq '\[SYNC\]\[BOUNDARY\] PASS rounds=5000 .*signals_woke=5000 signals_permit=0 .*target_hits=5000 .*sem_count=0 sem_waiters=0 .*cleanup=PASS observer_active=0 observer_inflight=0' "$log"
    grep -Fq '[SYNC][RACE] PASS rounds=2000' "$log"
    grep -Fq '[ACCOUNT][SHARE] PASS' "$log"
    grep -Fq '[KILLTEST][RUNNING] PASS' "$log"
    grep -Fq '[KILLTEST][SLEEPING] PASS' "$log"
    grep -Fq '[REAPTEST][SNAPSHOT] PASS' "$log"
    grep -Fq '[MODALTEST][SECOND] PASS' "$log"
    grep -Fq '[TASKMANTEST][CLEANUP] PASS removed=50' "$log"
    grep -Fq '[SCHED][CHECK] PASS' "$log"
    grep -Fq '[SYNC][CHECK] PASS' "$log"
    grep -Fq '[ACCOUNT][CHECK] PASS' "$log"
    grep -Fq '[KILLTEST][CHECK] PASS' "$log"
    grep -Fq '[REAPTEST][CHECK] PASS' "$log"
    grep -Fq '[INPUTTEST][CHECK] PASS' "$log"
    grep -Fq '[MODALTEST][CHECK] PASS' "$log"
    grep -Fq '[TASKMANTEST][CHECK] PASS' "$log"
    grep -Fq '[TASKDIAG][CHECK] PASS' "$log"
    assert_clean_log "$log"
  ); then
    return 0
  fi
  parse_autorun_log "$log" "cl13-matrix-${id}"
  record matrix "$id" 2 kvm selftest-autorun \
    'completed guest log recovered after strict serial framing repair' PASS 0 \
    "$log"
}

matrix_gate(){
  current_stage=matrix
  ensure_ledger
  [[ -r /dev/kvm && -w /dev/kvm ]] || return 3
  make kernel-check JOBS=2 SELFTEST=1 SELFTEST_AUTORUN=1 >/dev/null
  make image SELFTEST=1 SELFTEST_AUTORUN=1 >/dev/null
  harness_lock
  matrix_recover_smp2_kvm
  matrix_one_unless_pass up-tcg 1 tcg
  matrix_one_unless_pass smp2-tcg 2 tcg
  matrix_one_unless_pass smp2-kvm 2 kvm
  matrix_one_unless_pass smp4-tcg 4 tcg
  matrix_one_unless_pass smp4-kvm 4 kvm
  matrix_one_unless_pass smp8-tcg 8 tcg
  matrix_one_unless_pass smp8-kvm 8 kvm
  harness_unlock
}

matrix_resume_smp4_gate(){
  current_stage=matrix
  ensure_ledger
  [[ -r /dev/kvm && -w /dev/kvm ]] || return 3
  make kernel-check JOBS=2 SELFTEST=1 SELFTEST_AUTORUN=1 >/dev/null
  make image SELFTEST=1 SELFTEST_AUTORUN=1 >/dev/null
  harness_lock
  matrix_one_unless_pass smp4-tcg 4 tcg
  matrix_one_unless_pass smp4-kvm 4 kvm
  matrix_one_unless_pass smp8-tcg 8 tcg
  matrix_one_unless_pass smp8-kvm 8 kvm
  harness_unlock
}

matrix_smp8_tcg_gate(){
  current_stage=matrix
  ensure_ledger
  make kernel-check JOBS=2 SELFTEST=1 SELFTEST_AUTORUN=1 >/dev/null
  make image SELFTEST=1 SELFTEST_AUTORUN=1 >/dev/null
  harness_lock
  matrix_one smp8-tcg 8 tcg
  harness_unlock
}

quantum_one(){
  local quantum=$1 smp=$2 begin end log
  begin=$(now_ms)
  make kernel-check JOBS=2 SELFTEST=1 SELFTEST_AUTORUN=1 \
    KERNEL_EXTRA_CFLAGS="-DHOBBYOS_SCHED_TEST_QUANTUM=$quantum" >/dev/null
  make image SELFTEST=1 SELFTEST_AUTORUN=1 \
    KERNEL_EXTRA_CFLAGS="-DHOBBYOS_SCHED_TEST_QUANTUM=$quantum" >/dev/null
  start_qemu "$smp" tcg
  selftest_validate_autorun "$serial" \
    "$artifact_dir/cl13-quantum-${quantum}-smp${smp}-tcg" \
    accounting.sleep.minimum_deadline \
    accounting.sleep.one_ms_granularity \
    accounting.sleep.late_is_diagnostic \
    accounting.sleep.wait_result \
    accounting.sleep.percentiles
  sleep_profile_set_gate tcg
  send_complete "tasktest scheduler" "[SELFTEST][SUMMARY]" 180 stress
  HARNESS_ASYNC_RECORD_LOG="$artifact_dir/cl13-quantum-${quantum}-async.log"
  : >"$HARNESS_ASYNC_RECORD_LOG"
  framed_schedtest_async YIELD "schedtest yield $smp 20000" \
    "[SCHED][STRESS] YIELD_START" \
    "[SCHED][STRESS] YIELD_COMPLETE" 1200 stress
  if [[ $quantum == 1 ]]; then
    send_complete "synctest check" "[SYNC][CHECK] PASS" 90
    send_complete "killtest check" "[KILLTEST][CHECK] PASS" 90
  else
    send_complete "accounttest check" "[ACCOUNT][CHECK] PASS" 180
    send_complete "taskmantest check" "[TASKMANTEST][CHECK] PASS" 90
  fi
  log="$artifact_dir/cl13-quantum-${quantum}-smp${smp}-tcg.log"
  cp "$serial" "$log"
  assert_clean_log "$log"
  stop_qemu
  end=$(now_ms)
  record quantum "q$quantum" "$smp" tcg "quantum=$quantum" \
    'scheduler selftest and yield' PASS "$((end-begin))" "$log"
  unset HARNESS_ASYNC_RECORD_LOG
}

quantum_gate(){
  current_stage=quantum
  ensure_ledger
  harness_lock
  quantum_one 1 2
  quantum_one 7 8
  make kernel-check JOBS=2 >/dev/null
  make image >/dev/null
  harness_unlock
  record quantum default 0 host release 'normal rebuild' PASS 0 \
    artifacts/build/kernel-check-j2.log
}

negatives_gate(){
  current_stage=negatives
  ensure_ledger
  local begin end
  begin=$(now_ms)
  bash scripts/test-taskman-v1-negatives.sh
  end=$(now_ms)
  record negatives consolidated 0 tcg isolated-builds \
    '21 causal negatives + pure sleep contract canary + positive rebuilds' PASS "$((end-begin))" \
    "$artifact_dir/cl13-negative-taskman-no-page-clamp-reset.log"
}

soak_gate(){
  local smp=$1 accel=tcg begin end
  [[ $smp == 8 ]] && accel=kvm
  current_stage="soak$smp"
  ensure_ledger
  begin=$(now_ms)
  bash scripts/test-taskman-v1-soak.sh "$smp"
  end=$(now_ms)
  record "soak$smp" "smp$smp-$accel" "$smp" "$accel" selftest-autorun \
    '180000ms consolidated soak' PASS "$((end-begin))" \
    "$artifact_dir/cl13-soak-smp${smp}-${accel}.log"
}

final_build(){
  current_stage=final-build
  ensure_ledger
  local begin end jobs
  begin=$(now_ms)
  jobs=$(nproc)
  make clean
  make stack-check JOBS=2
  cp artifacts/build/kernel-check-j2.log "$artifact_dir/cl13-final-stack.log"
  make kernel-check JOBS=2
  cp kernel.elf /tmp/hobbyos-cl13fix12-j2.elf
  cp artifacts/build/kernel-check-j2.log "$artifact_dir/cl13-final-j2.log"
  make kernel-check JOBS="$jobs"
  cp kernel.elf /tmp/hobbyos-cl13fix12-jN.elf
  cp "artifacts/build/kernel-check-j${jobs}.log" "$artifact_dir/cl13-final-jN.log"
  cmp -s /tmp/hobbyos-cl13fix12-j2.elf /tmp/hobbyos-cl13fix12-jN.elf
  sha256sum /tmp/hobbyos-cl13fix12-j2.elf /tmp/hobbyos-cl13fix12-jN.elf \
    >"$artifact_dir/cl13-final-release-hashes.log"
  make kernel-check JOBS=2 SELFTEST=1 SELFTEST_AUTORUN=1
  cp kernel.elf /tmp/hobbyos-cl13-selftest.elf
  sha256sum /tmp/hobbyos-cl13-selftest.elf \
    >"$artifact_dir/cl13-final-selftest-hash.log"
  make kernel-check JOBS=2
  make deps-check
  make image
  [[ -z $(nm -u kernel.elf) ]]
  ! strings kernel.elf | grep -Fq '[SELFTEST][AUTORUN] BEGIN'
  sha256sum kernel.elf hobbyos.img >"$artifact_dir/cl13-final-image-hashes.log"
  source_snapshot "$source_after_build"
  source_continuity "$source_before" "$source_after_build"
  protected_fix8_check "$protected_fix8_after_build"
  protected_fix10_check "$protected_fix10_after_build"
  time_fix10_check "$time_fix10_after_build"
  end=$(now_ms)
  record final-build release 0 host release \
    'stack j2 jN selftest deps image nm hashes' PASS "$((end-begin))" \
    "$artifact_dir/cl13-final-image-hashes.log"
}

ledger_json_write(){
  python3 - "$ledger_tsv" "$ledger_json" <<'PY'
import csv, json, pathlib, sys
source, target = map(pathlib.Path, sys.argv[1:])
fields = [
    "stage", "scenario", "smp", "accel", "build_variant", "command",
    "status", "duration_ms", "artifact", "hash", "early_mask",
    "late_mask", "wait_error_mask", "late_rounds", "target", "samples",
    "p50", "p95", "p99", "max", "late_samples", "host_load_psi",
]
with source.open(newline="") as handle:
    reader = csv.reader(handle, delimiter="\t")
    next(reader, None)
    rows = []
    for values in reader:
        values = (values + ["0"] * len(fields))[:len(fields)]
        rows.append(dict(zip(fields, values)))
for row in rows:
    row["smp"] = int(row["smp"])
    row["duration_ms"] = int(row["duration_ms"])
target.write_text(json.dumps(rows, indent=2, sort_keys=True) + "\n")
PY
}

report_gate(){
  current_stage=report
  ensure_ledger
  ledger_json_write
  [[ -f docs/test-reports/TMV1-CL-13-taskman-v1-certification.md ]]
  [[ -f docs/test-reports/TMV1-CL-13-visual-checklist.md ]]
  [[ -f docs/test-reports/TMV1-CL-13-FIX12-soak-status-oracle.md ]]
  local item stage scenario
  local -a required
  required=(
    'matrix up-tcg' 'matrix smp2-tcg' 'matrix smp2-kvm'
    'matrix smp4-tcg' 'matrix smp4-kvm' 'matrix smp8-tcg' 'matrix smp8-kvm'
    'quantum q1' 'quantum q7' 'quantum default'
    'negatives fix11-resume' 'soak4 smp4-tcg' 'soak8 smp8-kvm'
    'final-build release'
  )
  for item in "${required[@]}"; do
    read -r stage scenario <<<"$item"
    [[ $(awk -F '\t' -v wanted_stage="$stage" -v wanted_scenario="$scenario" '
      $1 == wanted_stage && $2 == wanted_scenario { status=$7 }
      END { print status }
    ' "$ledger_tsv") == PASS ]]
  done
  python3 - "$artifact_dir/cl13fix11-checkpoint.json" <<'PY'
import json, pathlib, sys
data = json.loads(pathlib.Path(sys.argv[1]).read_text())
if data.get("status") != "PASS":
    raise SystemExit("FIX11 checkpoint is not PASS")
PY
  record report certification 0 host documentation \
    'ledger + certification report' PASS 0 \
    docs/test-reports/TMV1-CL-13-taskman-v1-certification.md
  ledger_json_write
}

run_all(){
  preflight
  static_gate
  selftest_gate
  matrix_gate
  quantum_gate
  negatives_gate
  soak_gate 4
  soak_gate 8
  source_snapshot "$source_after_tests"
  source_continuity "$source_before" "$source_after_tests"
  protected_fix10_check "$protected_fix10_after_tests"
  final_build
  report_gate
}

resume_after_boundary(){
  static_gate
  selftest_gate
  matrix_gate
  quantum_gate
  negatives_gate
  source_snapshot "$source_after_tests"
  source_continuity "$source_before" "$source_after_tests"
  soak_gate 4
  soak_gate 8
  final_build
  report_gate
}

resume_after_transport(){
  static_gate
  selftest_gate
  matrix_gate
  quantum_gate
  negatives_gate
  source_snapshot "$source_after_tests"
  source_continuity "$source_before" "$source_after_tests"
  soak_gate 4
  soak_gate 8
  final_build
  report_gate
}

resume_after_fixture257(){
  static_gate
  selftest_gate
  matrix_gate
  quantum_gate
  negatives_gate
  source_snapshot "$source_after_tests"
  source_continuity "$source_before" "$source_after_tests"
  protected_fix4_check "$protected_fix4_after_tests"
  soak_gate 4
  soak_gate 8
  final_build
  report_gate
}

resume_after_async_contract(){
  static_gate
  selftest_gate
  matrix_gate
  quantum_gate
  negatives_gate
  source_snapshot "$source_after_tests"
  source_continuity "$source_before" "$source_after_tests"
  protected_fix4_check "$protected_fix4_after_tests"
  protected_fix5_check "$protected_fix5_after_tests"
  protected_fix7_check "$protected_fix7_after_tests"
  soak_gate 4
  soak_gate 8
  final_build
  report_gate
}

resume_after_runtime_ready(){
  static_gate
  selftest_gate
  matrix_gate
  quantum_gate
  negatives_gate
  source_snapshot "$source_after_tests"
  source_continuity "$source_before" "$source_after_tests"
  protected_fix8_check "$protected_fix8_after_tests"
  soak_gate 4
  soak_gate 8
  final_build
  report_gate
}

resume_after_clock_sleep(){
  grep -Fq 'SINGLE_SAMPLE_SLEEP_QOS_FALSE_REJECTION_CONFIRMED.' \
    "$artifact_dir/cl13fix10-clock-sleep-classification.log"
  cmp -s "$protected_fix10_before" \
    "$artifact_dir/cl13fix10-protected-runtime-after-focused.sha256"
  cmp -s "$time_fix10_before" \
    "$artifact_dir/cl13fix10-time-runtime-after-focused.sha256"
  matrix_resume_smp4_gate
  quantum_gate
  negatives_gate
  soak_gate 4
  soak_gate 8
  source_snapshot "$source_after_tests"
  source_continuity "$source_before" "$source_after_tests"
  protected_fix10_check "$protected_fix10_after_tests"
  final_build
  report_gate
}

case ${1:-} in
  preflight) preflight;;
  static) static_gate;;
  selftest) selftest_gate;;
  matrix) matrix_gate;;
  matrix-resume-smp4) matrix_resume_smp4_gate;;
  matrix-smp8-tcg) matrix_smp8_tcg_gate;;
  quantum) quantum_gate;;
  negatives) negatives_gate;;
  soak4) soak_gate 4;;
  soak8) soak_gate 8;;
  final-build) final_build;;
  report) report_gate;;
  resume-after-boundary) resume_after_boundary;;
  resume-after-transport) resume_after_transport;;
  resume-after-fixture257) resume_after_fixture257;;
  resume-after-async-contract) resume_after_async_contract;;
  resume-after-runtime-ready) resume_after_runtime_ready;;
  resume-after-clock-sleep) resume_after_clock_sleep;;
  resume-after-hpet-negative) bash scripts/test-cl13-fix11-resume.sh;;
  resume-after-soak8-input-marker) bash scripts/test-cl13-fix12-resume.sh;;
  resume-after-soak8-modal-baseline) bash scripts/test-cl13-fix13-resume.sh;;
  resume-after-modal-global-heap-noise) bash scripts/test-cl13-fix14-resume.sh;;
  resume-after-taskman-arm-marker) bash scripts/test-cl13-fix15-resume.sh;;
  all) run_all;;
  *) usage;;
esac

stop_qemu
trap - ERR EXIT
