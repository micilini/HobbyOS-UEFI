#!/usr/bin/env bash
set -uo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

serial=.qemu/qemu-serial.log
hmp=(python3 scripts/qemu_hmp.py --socket .qemu/hmp.sock)
artifact_dir=artifacts/build
summary_tsv=$artifact_dir/cl11fix12-kvm-summary.tsv
summary_log=$artifact_dir/cl11fix12-kvm-summary.log
controls_tsv=$artifact_dir/cl11fix12-kvm-controls-summary.tsv
harness_start_line=0
harness_command=boot
source scripts/harness-common.sh

mkdir -p "$artifact_dir"
harness_lock
trap stop_qemu EXIT

log_note() {
  printf '%s\n' "$*" | tee -a "$summary_log"
}

available_value() {
  local path=$1
  if [[ -r $path ]]; then
    sed -n '1p' "$path"
  else
    printf 'unavailable\n'
  fi
}

host_evidence() {
  local output=$1 accel=$2 smp=$3 mem=${MEM:-2G}
  {
    printf 'timestamp=%s\n' "$(date --iso-8601=seconds)"
    printf 'nproc=%s\n' "$(nproc)"
    printf 'loadavg='; cat /proc/loadavg
    printf '[cpu_pressure]\n'
    if [[ -r /proc/pressure/cpu ]]; then cat /proc/pressure/cpu; else echo unavailable; fi
    grep '^Cpus_allowed_list' /proc/self/status || true
    printf 'cpu.max=%s\n' "$(available_value /sys/fs/cgroup/cpu.max)"
    printf 'cpuset.cpus.effective=%s\n' "$(available_value /sys/fs/cgroup/cpuset.cpus.effective)"
    printf '[proc_stat_cpu]\n'
    grep -E '^cpu ' /proc/stat || true
    printf '[memory_mb]\n'
    free -m
    printf '[top_cpu_threads]\n'
    ps -eLo pid,tid,psr,pcpu,stat,comm --sort=-pcpu | head -40
    printf '[kvm]\n'
    [[ -e /dev/kvm ]] && echo 'exists=yes' || echo 'exists=no'
    [[ -r /dev/kvm ]] && echo 'readable=yes' || echo 'readable=no'
    [[ -w /dev/kvm ]] && echo 'writable=yes' || echo 'writable=no'
    printf 'qemu_version=%s\n' "$(qemu-system-x86_64 --version | head -n 1)"
    printf 'ACCEL=%s\nSMP=%s\nMEM=%s\n' "$accel" "$smp" "$mem"
  } >"$output"
}

clean_project_environment() {
  local pid cwd
  scripts/qemu-agent.sh stop >/dev/null 2>&1 || true
  [[ ! -S .qemu/hmp.sock ]] || { echo 'active .qemu/hmp.sock before boot' >&2; return 1; }
  if [[ -f .qemu/qemu.pid ]]; then
    pid=$(sed -n '1p' .qemu/qemu.pid 2>/dev/null || true)
    [[ ! $pid =~ ^[0-9]+$ ]] || ! kill -0 "$pid" 2>/dev/null || {
      echo 'active .qemu/qemu.pid before boot' >&2
      return 1
    }
    echo 'stale .qemu/qemu.pid before boot' >&2
    return 1
  fi
  while read -r pid; do
    [[ $pid =~ ^[0-9]+$ ]] || continue
    cwd=$(readlink "/proc/$pid/cwd" 2>/dev/null || true)
    [[ $cwd != "$root" ]] || {
      echo 'qemu-system-x86_64 from this workspace remains active' >&2
      return 1
    }
  done < <(pgrep -f 'qemu-system-x86_64' || true)
}

COMMAND_VERDICT=NOT_RUN
COMMAND_FAULT=0
COMMAND_TRANSPORT=0

focused_command() {
  local command=$1 completion_re=$2 timeout=$3
  local deadline fresh line verdict sync_rc
  COMMAND_VERDICT=NOT_RUN
  COMMAND_FAULT=0
  COMMAND_TRANSPORT=0
  harness_command=$command
  harness_start_line=$(wc -l <"$serial" 2>/dev/null || echo 0)
  if ! hmp_text "$command" normal --enter >/dev/null; then
    COMMAND_TRANSPORT=1
    return 2
  fi
  deadline=$((SECONDS + timeout))
  while ((SECONDS <= deadline)); do
    fresh=$(tail -n +$((harness_start_line + 1)) "$serial" 2>/dev/null || true)
    if grep -q -E 'PANIC|#PF|#GP|FATAL|STRUCTURAL_FAULT|FINISH_FAULT|QEMU exited' <<<"$fresh"; then
      COMMAND_FAULT=1
      return 3
    fi
    line=$(grep -E "$completion_re" <<<"$fresh" | tail -n 1 || true)
    if [[ -n $line ]]; then
      if grep -q ' PASS' <<<"$line"; then verdict=PASS; else verdict=FAIL; fi
      COMMAND_VERDICT=$verdict
      shell_sync
      sync_rc=$?
      if ((sync_rc != 0)); then
        if grep -q -E 'PANIC|#PF|#GP|FATAL|STRUCTURAL_FAULT|FINISH_FAULT|QEMU exited' \
          "$serial" 2>/dev/null; then
          COMMAND_FAULT=1
          return 3
        fi
        COMMAND_TRANSPORT=1
        return 2
      fi
      [[ $verdict == PASS ]]
      return
    fi
    if ! pgrep -f 'qemu-system-x86_64' >/dev/null; then
      COMMAND_FAULT=1
      return 3
    fi
    sleep 1
  done
  COMMAND_TRANSPORT=1
  return 2
}

extract_metric() {
  local line=$1 key=$2 value
  value=$(sed -n "s/.*${key}=\([0-9][0-9]*\).*/\1/p" <<<"$line")
  printf '%s\n' "${value:-NA}"
}

host_summary_values() {
  local evidence=$1 load_line psi_some psi_full
  load_line=$(sed -n 's/^loadavg=//p' "$evidence" | head -n 1)
  HOST_LOAD1=$(awk '{print $1}' <<<"$load_line")
  HOST_LOAD5=$(awk '{print $2}' <<<"$load_line")
  HOST_LOAD15=$(awk '{print $3}' <<<"$load_line")
  psi_some=$(awk '/^some /{for(i=1;i<=NF;i++)if($i~/^avg10=/){sub("avg10=","",$i);print $i;exit}}' "$evidence")
  psi_full=$(awk '/^full /{for(i=1;i<=NF;i++)if($i~/^avg10=/){sub("avg10=","",$i);print $i;exit}}' "$evidence")
  HOST_PSI_SOME=${psi_some:-NA}
  HOST_PSI_FULL=${psi_full:-NA}
}

append_result_part() {
  local part=$1
  if [[ -z $RUN_RESULT ]]; then RUN_RESULT=$part; else RUN_RESULT+=",$part"; fi
}

LAST_CONFIG=NOT_RUN
LAST_LIVENESS=NOT_RUN
LAST_RATE_ORIGINAL=NOT_RUN
LAST_RATE_LONG=NOT_RUN
LAST_CHECK=NOT_RUN
LAST_GUEST_FAULT=0
LAST_TRANSPORT_FAILURE=0
LAST_WORST_ORIGINAL=NA
LAST_MIN_ORIGINAL=NA
LAST_MAX_ORIGINAL=NA
LAST_WORST_LONG=NA
LAST_MIN_LONG=NA
LAST_MAX_LONG=NA
LAST_RESULT=GUEST_FAULT

run_boot() {
  local accel=$1 smp=$2 label=$3 include_check=$4 affinity=${5:-0}
  local log=$artifact_dir/${label}.log
  local launch=$artifact_dir/${label}.launch.env
  local before=$artifact_dir/${label}-host-before.env
  local after=$artifact_dir/${label}-host-after.env
  local rate_original_line rate_long_line rc started=0

  LAST_CONFIG=NOT_RUN
  LAST_LIVENESS=NOT_RUN
  LAST_RATE_ORIGINAL=NOT_RUN
  LAST_RATE_LONG=NOT_RUN
  LAST_CHECK=NOT_RUN
  LAST_GUEST_FAULT=0
  LAST_TRANSPORT_FAILURE=0
  LAST_WORST_ORIGINAL=NA
  LAST_MIN_ORIGINAL=NA
  LAST_MAX_ORIGINAL=NA
  LAST_WORST_LONG=NA
  LAST_MIN_LONG=NA
  LAST_MAX_LONG=NA

  if ! clean_project_environment; then
    log_note "[$label] environment cleanup failed"
    LAST_TRANSPORT_FAILURE=1
  fi
  host_evidence "$before" "$accel" "$smp"

  log_note "[$label] boot accel=$accel smp=$smp"
  if start_qemu "$smp" "$accel"; then
    started=1
    cp .qemu/launch.env "$launch"
    if ((affinity)); then configure_affinity "$label"; fi
  else
    LAST_TRANSPORT_FAILURE=1
  fi

  if ((started)); then
    focused_command 'accounttest lapic-config' '\[ACCOUNT\]\[LAPIC_CONFIG\] (PASS|FAIL)' 60
    rc=$?; LAST_CONFIG=$COMMAND_VERDICT
    ((COMMAND_FAULT)) && LAST_GUEST_FAULT=1
    ((COMMAND_TRANSPORT)) && LAST_TRANSPORT_FAILURE=1
    log_note "[$label] lapic-config=$LAST_CONFIG rc=$rc"

    if ((LAST_GUEST_FAULT == 0)); then
      focused_command 'accounttest lapic-liveness 500' '\[ACCOUNT\]\[LAPIC_LIVENESS\] (PASS|FAIL).*window_ms=500' 60
      rc=$?; LAST_LIVENESS=$COMMAND_VERDICT
      ((COMMAND_FAULT)) && LAST_GUEST_FAULT=1
      ((COMMAND_TRANSPORT)) && LAST_TRANSPORT_FAILURE=1
      log_note "[$label] lapic-liveness-500=$LAST_LIVENESS rc=$rc"
    fi

    if ((LAST_GUEST_FAULT == 0)); then
      focused_command 'accounttest lapic-rate 2000 5' '\[ACCOUNT\]\[LAPIC_RATE\] (PASS|FAIL).*window_ms=2000 rounds=5' 180
      rc=$?; LAST_RATE_ORIGINAL=$COMMAND_VERDICT
      ((COMMAND_FAULT)) && LAST_GUEST_FAULT=1
      ((COMMAND_TRANSPORT)) && LAST_TRANSPORT_FAILURE=1
      log_note "[$label] lapic-rate-2000x5=$LAST_RATE_ORIGINAL rc=$rc"
    fi

    if ((LAST_GUEST_FAULT == 0)); then
      focused_command 'accounttest lapic-rate 5000 5' '\[ACCOUNT\]\[LAPIC_RATE\] (PASS|FAIL).*window_ms=5000 rounds=5' 300
      rc=$?; LAST_RATE_LONG=$COMMAND_VERDICT
      ((COMMAND_FAULT)) && LAST_GUEST_FAULT=1
      ((COMMAND_TRANSPORT)) && LAST_TRANSPORT_FAILURE=1
      log_note "[$label] lapic-rate-5000x5=$LAST_RATE_LONG rc=$rc"
    fi

    if ((include_check && LAST_GUEST_FAULT == 0)); then
      focused_command 'accounttest check' '\[ACCOUNT\]\[CHECK\] (PASS|FAIL)' 90
      rc=$?; LAST_CHECK=$COMMAND_VERDICT
      ((COMMAND_FAULT)) && LAST_GUEST_FAULT=1
      ((COMMAND_TRANSPORT)) && LAST_TRANSPORT_FAILURE=1
      log_note "[$label] accounttest-check=$LAST_CHECK rc=$rc"
    fi
  fi

  if [[ -f $serial ]]; then cp "$serial" "$log"; else : >"$log"; fi
  host_evidence "$after" "$accel" "$smp"
  stop_qemu

  if grep -q -E 'PANIC|#PF|#GP|FATAL|STRUCTURAL_FAULT|FINISH_FAULT' "$log" 2>/dev/null; then
    LAST_GUEST_FAULT=1
  fi
  rate_original_line=$(grep -E '\[ACCOUNT\]\[LAPIC_RATE\] (PASS|FAIL).*window_ms=2000 rounds=5' "$log" | tail -n 1 || true)
  rate_long_line=$(grep -E '\[ACCOUNT\]\[LAPIC_RATE\] (PASS|FAIL).*window_ms=5000 rounds=5' "$log" | tail -n 1 || true)
  LAST_WORST_ORIGINAL=$(extract_metric "$rate_original_line" worst_median_x1000)
  LAST_MIN_ORIGINAL=$(extract_metric "$rate_original_line" min_round_x1000)
  LAST_MAX_ORIGINAL=$(extract_metric "$rate_original_line" max_round_x1000)
  LAST_WORST_LONG=$(extract_metric "$rate_long_line" worst_median_x1000)
  LAST_MIN_LONG=$(extract_metric "$rate_long_line" min_round_x1000)
  LAST_MAX_LONG=$(extract_metric "$rate_long_line" max_round_x1000)

  RUN_RESULT=
  [[ $LAST_CONFIG == FAIL ]] && append_result_part CONFIG_FAIL
  [[ $LAST_LIVENESS == FAIL ]] && append_result_part LIVENESS_FAIL
  [[ $LAST_RATE_ORIGINAL == PASS ]] && append_result_part RATE_ORIGINAL_PASS
  [[ $LAST_RATE_ORIGINAL == FAIL ]] && append_result_part RATE_ORIGINAL_FAIL
  [[ $LAST_RATE_LONG == PASS ]] && append_result_part RATE_LONG_PASS
  [[ $LAST_RATE_LONG == FAIL ]] && append_result_part RATE_LONG_FAIL
  ((LAST_GUEST_FAULT)) && append_result_part GUEST_FAULT
  [[ -n $RUN_RESULT ]] || RUN_RESULT=GUEST_FAULT
  LAST_RESULT=$RUN_RESULT
  log_note "[$label] result=$LAST_RESULT transport_failure=$LAST_TRANSPORT_FAILURE"
}

expand_cpu_list() {
  local list=$1 item first last cpu
  AVAILABLE_CPUS=()
  IFS=',' read -ra items <<<"$list"
  for item in "${items[@]}"; do
    if [[ $item == *-* ]]; then
      first=${item%-*}; last=${item#*-}
      for ((cpu=first; cpu<=last; cpu++)); do AVAILABLE_CPUS+=("$cpu"); done
    elif [[ $item =~ ^[0-9]+$ ]]; then
      AVAILABLE_CPUS+=("$item")
    fi
  done
}

configure_affinity() {
  local label=$1 pid tid comm index=0 service_set cpu
  local output=$artifact_dir/${label}-affinity.env
  pid=$(sed -n '1p' .qemu/qemu.pid)
  service_set=${AVAILABLE_CPUS[8]},${AVAILABLE_CPUS[9]}
  {
    printf 'available_cpus=%s\n' "$(IFS=,; echo "${AVAILABLE_CPUS[*]}")"
    printf 'service_cpus=%s\n' "$service_set"
    taskset -apc "$service_set" "$pid"
    for task_dir in /proc/"$pid"/task/*; do
      tid=${task_dir##*/}
      comm=$(sed -n '1p' "$task_dir/comm" 2>/dev/null || true)
      if [[ $comm =~ ^CPU\ [0-9]+/KVM$ && $index -lt 8 ]]; then
        cpu=${AVAILABLE_CPUS[$index]}
        taskset -pc "$cpu" "$tid"
        printf 'vcpu_thread=%s comm=%s cpu=%s\n' "$tid" "$comm" "$cpu"
        index=$((index + 1))
      fi
    done
    printf 'vcpu_threads_pinned=%s\n' "$index"
    printf '[qemu_process_affinity]\n'
    taskset -pc "$pid"
    printf '[thread_affinity]\n'
    for task_dir in /proc/"$pid"/task/*; do
      tid=${task_dir##*/}
      comm=$(sed -n '1p' "$task_dir/comm" 2>/dev/null || true)
      printf 'tid=%s comm=%s ' "$tid" "$comm"
      taskset -pc "$tid" | sed 's/.*current affinity list: /affinity=/'
    done
  } >"$output" 2>&1
}

printf 'run\tconfig\tliveness\tworst_median_2000x5\tmin_round_2000x5\tmax_round_2000x5\tworst_median_5000x5\tmin_round_5000x5\tmax_round_5000x5\tload1_before\tload5_before\tload15_before\tcpu_psi_some_avg10_before\tcpu_psi_full_avg10_before\tresult\n' >"$summary_tsv"
printf 'control\tsmp\tconfig\tliveness\tworst_median_2000x5\tworst_median_5000x5\tresult\n' >"$controls_tsv"
: >"$summary_log"

if [[ ! -e /dev/kvm || ! -r /dev/kvm || ! -w /dev/kvm ]]; then
  log_note '[LAPIC_KVM_FOCUSED][ENVIRONMENT] FAIL /dev/kvm unavailable'
  exit 1
fi
qemu_version=$(qemu-system-x86_64 --version | head -n 1)
if [[ $qemu_version != 'QEMU emulator version 8.2.2 '* ]]; then
  log_note "[LAPIC_KVM_FOCUSED][ENVIRONMENT] FAIL qemu_version=$qemu_version"
  exit 1
fi

allowed_list=$(awk '/^Cpus_allowed_list:/{print $2}' /proc/self/status)
expand_cpu_list "$allowed_list"
log_note "[LAPIC_KVM_FOCUSED][ENVIRONMENT] PASS qemu_version=$qemu_version cpus_available=${#AVAILABLE_CPUS[@]} allowed=$allowed_list"

if ! clean_project_environment; then
  log_note '[LAPIC_KVM_FOCUSED][CLEAN] FAIL before initial wait'
  exit 1
fi
log_note '[LAPIC_KVM_FOCUSED][CLEAN] PASS initial_wait_seconds=30'
sleep 30

main_original_failures=0
main_config_or_liveness_failures=0
for run in 1 2 3 4 5; do
  label=cl11fix12-kvm-run${run}
  run_boot kvm 8 "$label" 1
  host_summary_values "$artifact_dir/${label}-host-before.env"
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$run" "$LAST_CONFIG" "$LAST_LIVENESS" \
    "$LAST_WORST_ORIGINAL" "$LAST_MIN_ORIGINAL" "$LAST_MAX_ORIGINAL" \
    "$LAST_WORST_LONG" "$LAST_MIN_LONG" "$LAST_MAX_LONG" \
    "$HOST_LOAD1" "$HOST_LOAD5" "$HOST_LOAD15" "$HOST_PSI_SOME" "$HOST_PSI_FULL" "$LAST_RESULT" \
    >>"$summary_tsv"
  [[ $LAST_RATE_ORIGINAL == PASS ]] || main_original_failures=$((main_original_failures + 1))
  if [[ $LAST_CONFIG != PASS || $LAST_LIVENESS != PASS ]]; then
    main_config_or_liveness_failures=$((main_config_or_liveness_failures + 1))
  fi
done

log_note '[LAPIC_KVM_FOCUSED][COLLECTION] PASS runs=5'

if ((main_original_failures > 0)); then
  log_note "[LAPIC_KVM_FOCUSED][CONTROLS] START original_failures=$main_original_failures"
  for run in 1 2 3; do
    label=cl11fix12-kvm-smp4-run${run}
    run_boot kvm 4 "$label" 0
    printf 'smp4-run%s\t4\t%s\t%s\t%s\t%s\t%s\n' \
      "$run" "$LAST_CONFIG" "$LAST_LIVENESS" "$LAST_WORST_ORIGINAL" "$LAST_WORST_LONG" "$LAST_RESULT" \
      >>"$controls_tsv"
  done

  if ((${#AVAILABLE_CPUS[@]} >= 10)); then
    label=cl11fix12-kvm-affinity-diagnostic
    run_boot kvm 8 "$label" 0 1
    printf 'affinity-diagnostic\t8\t%s\t%s\t%s\t%s\t%s\n' \
      "$LAST_CONFIG" "$LAST_LIVENESS" "$LAST_WORST_ORIGINAL" "$LAST_WORST_LONG" "$LAST_RESULT" \
      >>"$controls_tsv"
  else
    log_note "[LAPIC_KVM_FOCUSED][AFFINITY] SKIP cpus_available=${#AVAILABLE_CPUS[@]} required=10"
  fi
fi

if ((main_config_or_liveness_failures > 0)); then
  label=cl11fix12-tcg-diagnostic
  run_boot tcg 8 "$label" 0
  printf 'tcg-diagnostic\t8\t%s\t%s\t%s\t%s\t%s\n' \
    "$LAST_CONFIG" "$LAST_LIVENESS" "$LAST_WORST_ORIGINAL" "$LAST_WORST_LONG" "$LAST_RESULT" \
    >>"$controls_tsv"
else
  log_note '[LAPIC_KVM_FOCUSED][TCG] SKIP config_and_liveness_failures=0'
fi

log_note '[LAPIC_KVM_FOCUSED][DONE] data collection complete'
