#!/usr/bin/env bash
set -uo pipefail

if ((EUID == 0)); then
  printf '%s\n' '[LAPIC_CLEAN_HOST][ENVIRONMENT] FAIL reason=do-not-run-as-root'
  printf '%s\n' 'Run as the normal project user.'
  exit 1
fi

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

artifact_dir=artifacts/build
prefix=cl11fix13-clean-kvm
summary_tsv=$artifact_dir/${prefix}-summary.tsv
summary_log=$artifact_dir/${prefix}-summary.log
frozen_before=$artifact_dir/${prefix}-frozen-before.sha256
frozen_after=$artifact_dir/${prefix}-frozen-after.sha256
serial=.qemu/qemu-serial.log
hmp=(python3 scripts/qemu_hmp.py --socket .qemu/hmp.sock)
harness_start_line=0
harness_command=boot
busy_override=0

readonly -a frozen_sources=(
  kernel/src/apic/lapic.c
  kernel/src/apic/lapic.h
  kernel/src/shell/commands/cmd_accounttest.c
  scripts/test-lapic-rate.sh
  kernel/src/core/clock.c
  kernel/src/timer/hpet.c
)

source scripts/harness-common.sh

environment_fail() {
  local reason=$1
  shift
  printf '[LAPIC_CLEAN_HOST][ENVIRONMENT] FAIL reason=%s\n' "$reason"
  if (($# > 0)); then
    printf '%s\n' "$@"
  fi
}

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

qemu_agent_pid() {
  local pid stat
  [[ -f .qemu/qemu.pid ]] || return 1
  pid=$(sed -n '1p' .qemu/qemu.pid 2>/dev/null || true)
  [[ $pid =~ ^[0-9]+$ ]] || return 1
  kill -0 "$pid" 2>/dev/null || return 1
  stat=$(ps -p "$pid" -o stat= 2>/dev/null || true)
  [[ $stat != Z* ]] || return 1
  printf '%s\n' "$pid"
}

qemu_agent_active() {
  local pid args
  pid=$(qemu_agent_pid) || return 1
  args=$(ps -ww -p "$pid" -o args= 2>/dev/null || true)
  [[ $args == *qemu-system-x86_64* && $args == *'-monitor unix:.qemu/hmp.sock'* ]]
}

workspace_qemu_pids() {
  local pid cwd
  while read -r pid; do
    [[ $pid =~ ^[0-9]+$ ]] || continue
    cwd=$(readlink "/proc/$pid/cwd" 2>/dev/null || true)
    if [[ $cwd == "$root" ]]; then
      printf '%s\n' "$pid"
    fi
  done < <(pgrep -f '[q]emu-system-x86_64' || true)
}

workspace_qemu_active() {
  local pid
  pid=$(workspace_qemu_pids | sed -n '1p')
  [[ -n $pid ]]
}

cleanup() {
  local cleanup_rc=$?
  trap - EXIT INT TERM
  scripts/qemu-agent.sh stop >/dev/null 2>&1 || true
  if workspace_qemu_active || qemu_agent_active || [[ -S .qemu/hmp.sock ]]; then
    printf '%s\n' \
      '[LAPIC_CLEAN_HOST][CLEANUP] FAIL reason=qemu-still-active' >&2
  fi
  if ((cleanup_rc == 130 || cleanup_rc == 143)); then
    exit "$cleanup_rc"
  fi
  return "$cleanup_rc"
}

clean_project_environment() {
  scripts/qemu-agent.sh stop >/dev/null 2>&1 || true
  if workspace_qemu_active; then
    printf '%s\n' 'qemu-system-x86_64 from this workspace remains active' >&2
    return 1
  fi
  if [[ -S .qemu/hmp.sock ]]; then
    printf '%s\n' 'active .qemu/hmp.sock remains' >&2
    return 1
  fi
  if qemu_agent_active; then
    printf '%s\n' 'active .qemu/qemu.pid remains' >&2
    return 1
  fi
  return 0
}

same_user_processes() {
  local pattern=$1
  LC_ALL=C ps -u "$EUID" -o pid=,comm=,pcpu= | awk -v pattern="$pattern" '
    tolower($2) ~ pattern { printf "%s\t%s\t%s\n", $1, $2, $3 }
  '
}

check_user_processes() {
  local codex_processes heavy_processes
  codex_processes=$(same_user_processes '^(codex|codex-code-mode)$')
  if [[ -n $codex_processes ]]; then
    environment_fail codex-still-running 'Exit Codex before running this script.'
    return 1
  fi

  heavy_processes=$(same_user_processes \
    '^(chrome|chromium|firefox|slack|code|code-insiders|steam|discord)$')
  if [[ -n $heavy_processes ]]; then
    if ((busy_override == 0)); then
      environment_fail heavy-user-apps-running
    else
      printf '%s\n' \
        '[LAPIC_CLEAN_HOST][ENVIRONMENT] WARN reason=heavy-user-apps-running override=ALLOW_BUSY_HOST'
    fi
    printf 'PID\tcomm\tCPU%%\n'
    printf '%s\n' "$heavy_processes"
    ((busy_override == 1)) || return 1
  fi
  return 0
}

expand_cpu_list() {
  local list=$1 item first last cpu
  AVAILABLE_CPUS=()
  IFS=',' read -ra cpu_items <<<"$list"
  for item in "${cpu_items[@]}"; do
    if [[ $item =~ ^([0-9]+)-([0-9]+)$ ]]; then
      first=${BASH_REMATCH[1]}
      last=${BASH_REMATCH[2]}
      for ((cpu=first; cpu<=last; cpu++)); do
        AVAILABLE_CPUS+=("$cpu")
      done
    elif [[ $item =~ ^[0-9]+$ ]]; then
      AVAILABLE_CPUS+=("$item")
    fi
  done
}

thermal_evidence() {
  local path cpu_dir cpu zone
  printf '[cpu_frequency]\n'
  for path in /sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_governor; do
    [[ -e $path ]] || continue
    cpu_dir=${path%/cpufreq/scaling_governor}
    cpu=${cpu_dir##*/}
    printf '%s governor=%s current_frequency_khz=%s\n' \
      "$cpu" "$(available_value "$path")" \
      "$(available_value "$cpu_dir/cpufreq/scaling_cur_freq")"
  done
  printf 'intel_pstate_status=%s\n' \
    "$(available_value /sys/devices/system/cpu/intel_pstate/status)"
  printf 'intel_pstate_no_turbo=%s\n' \
    "$(available_value /sys/devices/system/cpu/intel_pstate/no_turbo)"
  printf 'cpufreq_boost=%s\n' \
    "$(available_value /sys/devices/system/cpu/cpufreq/boost)"

  printf '[thermal_zones]\n'
  for zone in /sys/class/thermal/thermal_zone*; do
    [[ -d $zone ]] || continue
    printf '%s type=%s temp_millic=%s\n' "${zone##*/}" \
      "$(available_value "$zone/type")" "$(available_value "$zone/temp")"
  done

  printf '[sensors]\n'
  if command -v sensors >/dev/null 2>&1; then
    sensors 2>&1 || true
  else
    printf 'unavailable\n'
  fi
}

host_evidence() {
  local output=$1 phase=$2 qemu_pid=${3:-} qemu_cpu=unavailable
  if [[ $qemu_pid =~ ^[0-9]+$ ]] && kill -0 "$qemu_pid" 2>/dev/null; then
    qemu_cpu=$(LC_ALL=C ps -p "$qemu_pid" -o pcpu= 2>/dev/null | awk 'NR==1 {print $1}')
    qemu_cpu=${qemu_cpu:-unavailable}
  fi
  {
    printf 'timestamp=%s\n' "$(date --iso-8601=seconds)"
    printf 'phase=%s\n' "$phase"
    printf 'nproc=%s\n' "$(nproc)"
    printf 'loadavg='; cat /proc/loadavg
    printf '[cpu_pressure]\n'
    if [[ -r /proc/pressure/cpu ]]; then cat /proc/pressure/cpu; else printf 'unavailable\n'; fi
    grep '^Cpus_allowed_list' /proc/self/status || true
    printf 'cpuset.cpus.effective=%s\n' \
      "$(available_value /sys/fs/cgroup/cpuset.cpus.effective)"
    printf 'cpu.max=%s\n' "$(available_value /sys/fs/cgroup/cpu.max)"
    printf '[proc_stat_cpu]\n'
    grep -E '^cpu ' /proc/stat || true
    printf '[memory_mb]\n'
    free -m
    printf '[top_cpu_threads]\n'
    LC_ALL=C ps -eLo pid,tid,psr,pcpu,stat,comm --sort=-pcpu | head -40
    printf '[kvm]\n'
    [[ -e /dev/kvm ]] && printf 'exists=yes\n' || printf 'exists=no\n'
    [[ -r /dev/kvm ]] && printf 'readable=yes\n' || printf 'readable=no\n'
    [[ -w /dev/kvm ]] && printf 'writable=yes\n' || printf 'writable=no\n'
    printf 'qemu_version=%s\n' "$(qemu-system-x86_64 --version | sed -n '1p')"
    printf 'ACCEL=kvm\nSMP=8\nMEM=2G\n'
    printf '[qemu_consumption]\n'
    printf 'qemu_pid=%s\n' "${qemu_pid:-unavailable}"
    printf 'qemu_cpu_percent=%s\n' "$qemu_cpu"
    if [[ $qemu_pid =~ ^[0-9]+$ ]] && kill -0 "$qemu_pid" 2>/dev/null; then
      LC_ALL=C ps -p "$qemu_pid" -o pid,ppid,psr,pcpu,pmem,etime,stat,comm,args
      printf '[qemu_threads]\n'
      LC_ALL=C ps -L -p "$qemu_pid" -o pid,tid,psr,pcpu,stat,comm --sort=-pcpu
    fi
    thermal_evidence
  } >"$output" 2>&1
}

host_summary_values() {
  local before=$1 after=$2 load_line psi_some
  load_line=$(sed -n 's/^loadavg=//p' "$before" | sed -n '1p')
  HOST_LOAD1=$(awk '{print $1}' <<<"$load_line")
  HOST_LOAD5=$(awk '{print $2}' <<<"$load_line")
  psi_some=$(awk '/^some /{for(i=1;i<=NF;i++)if($i~/^avg10=/){sub("avg10=","",$i);print $i;exit}}' "$before")
  HOST_PSI_SOME=${psi_some:-NA}
  HOST_QEMU_CPU=$(sed -n 's/^qemu_cpu_percent=//p' "$after" | sed -n '1p')
  HOST_LOAD1=${HOST_LOAD1:-NA}
  HOST_LOAD5=${HOST_LOAD5:-NA}
  HOST_QEMU_CPU=${HOST_QEMU_CPU:-NA}
}

COMMAND_VERDICT=NOT_RUN
COMMAND_FAULT=0
COMMAND_TRANSPORT=0
COMMAND_LINE=

focused_command() {
  local command=$1 completion_re=$2 timeout=$3
  local deadline fresh line verdict sync_rc
  COMMAND_VERDICT=NOT_RUN
  COMMAND_FAULT=0
  COMMAND_TRANSPORT=0
  COMMAND_LINE=
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
      COMMAND_LINE=$line
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
    if ! qemu_agent_active; then
      COMMAND_TRANSPORT=1
      return 2
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

LAST_CONFIG=NOT_RUN
LAST_LIVENESS=NOT_RUN
LAST_RATE=NOT_RUN
LAST_CHECK=NOT_RUN
LAST_GUEST_FAULT=0
LAST_TRANSPORT_FAILURE=0
LAST_ZERO_IRQ=NA
LAST_WORST=NA
LAST_MIN=NA
LAST_MAX=NA
LAST_RESULT=TRANSPORT_FAILURE

capture_command_result() {
  local destination=$1
  printf -v "$destination" '%s' "$COMMAND_VERDICT"
  ((COMMAND_FAULT)) && LAST_GUEST_FAULT=1
  ((COMMAND_TRANSPORT)) && LAST_TRANSPORT_FAILURE=1
}

run_boot() {
  local run=$1 label=${prefix}-run${run}
  local log=$artifact_dir/${label}.log
  local launch=$artifact_dir/${label}.launch.env
  local before=$artifact_dir/${label}-host-before.env
  local after=$artifact_dir/${label}-host-after.env
  local liveness_line= rate_line= rc started=0 pid=

  LAST_CONFIG=NOT_RUN
  LAST_LIVENESS=NOT_RUN
  LAST_RATE=NOT_RUN
  LAST_CHECK=NOT_RUN
  LAST_GUEST_FAULT=0
  LAST_TRANSPORT_FAILURE=0
  LAST_ZERO_IRQ=NA
  LAST_WORST=NA
  LAST_MIN=NA
  LAST_MAX=NA
  LAST_RESULT=TRANSPORT_FAILURE

  printf 'ACCEL=kvm\nSMP=8\nMEM=2G\nAFFINITY=none\nPRIORITY=default\nlaunch_status=NOT_STARTED\n' \
    >"$launch"

  if ! clean_project_environment; then
    LAST_TRANSPORT_FAILURE=1
    log_note "[$label] environment cleanup failed"
  fi
  host_evidence "$before" before

  log_note "[$label] boot accel=kvm smp=8 mem=2G affinity=none priority=default"
  if ((LAST_TRANSPORT_FAILURE == 0)) && MEM=2G QEMU=qemu-system-x86_64 start_qemu 8 kvm; then
    started=1
    cp .qemu/launch.env "$launch"
    printf 'AFFINITY=none\nPRIORITY=default\n' >>"$launch"
    pid=$(qemu_agent_pid || true)
  else
    LAST_TRANSPORT_FAILURE=1
  fi

  if ((started)); then
    focused_command 'accounttest lapic-config' \
      '\[ACCOUNT\]\[LAPIC_CONFIG\] (PASS|FAIL)' 60
    rc=$?
    capture_command_result LAST_CONFIG
    log_note "[$label] lapic-config=$LAST_CONFIG rc=$rc"

    if ((LAST_GUEST_FAULT == 0)); then
      focused_command 'accounttest lapic-liveness 500' \
        '\[ACCOUNT\]\[LAPIC_LIVENESS\] (PASS|FAIL).*window_ms=500' 60
      rc=$?
      liveness_line=$COMMAND_LINE
      capture_command_result LAST_LIVENESS
      log_note "[$label] lapic-liveness-500=$LAST_LIVENESS rc=$rc"
    fi

    if ((LAST_GUEST_FAULT == 0)); then
      focused_command 'accounttest lapic-rate 2000 5' \
        '\[ACCOUNT\]\[LAPIC_RATE\] (PASS|FAIL).*window_ms=2000 rounds=5' 180
      rc=$?
      rate_line=$COMMAND_LINE
      capture_command_result LAST_RATE
      log_note "[$label] lapic-rate-2000x5=$LAST_RATE rc=$rc"
    fi

    if ((LAST_GUEST_FAULT == 0)); then
      focused_command 'accounttest check' '\[ACCOUNT\]\[CHECK\] (PASS|FAIL)' 90
      rc=$?
      capture_command_result LAST_CHECK
      log_note "[$label] accounttest-check=$LAST_CHECK rc=$rc"
    fi
  fi

  if [[ -f $serial ]]; then cp "$serial" "$log"; else : >"$log"; fi
  if [[ -z $liveness_line ]]; then
    liveness_line=$(grep -E '\[ACCOUNT\]\[LAPIC_LIVENESS\] (PASS|FAIL).*window_ms=500' \
      "$log" | tail -n 1 || true)
  fi
  if [[ -z $rate_line ]]; then
    rate_line=$(grep -E '\[ACCOUNT\]\[LAPIC_RATE\] (PASS|FAIL).*window_ms=2000 rounds=5' \
      "$log" | tail -n 1 || true)
  fi

  LAST_ZERO_IRQ=$(extract_metric "$liveness_line" zero_irq_cpus)
  LAST_WORST=$(extract_metric "$rate_line" worst_median_x1000)
  LAST_MIN=$(extract_metric "$rate_line" min_round_x1000)
  LAST_MAX=$(extract_metric "$rate_line" max_round_x1000)

  if grep -q -E 'PANIC|#PF|#GP|FATAL|STRUCTURAL_FAULT|FINISH_FAULT' "$log" 2>/dev/null; then
    LAST_GUEST_FAULT=1
  fi

  host_evidence "$after" after "$pid"
  stop_qemu
  if workspace_qemu_active || qemu_agent_active || [[ -S .qemu/hmp.sock ]]; then
    LAST_TRANSPORT_FAILURE=1
  fi

  if ((LAST_GUEST_FAULT)) || [[ $LAST_CONFIG == FAIL || $LAST_LIVENESS == FAIL || $LAST_CHECK == FAIL ]] ||
    [[ $LAST_ZERO_IRQ =~ ^[0-9]+$ && $LAST_ZERO_IRQ -gt 0 ]]; then
    LAST_RESULT=KERNEL_BEHAVIOR_CHANGED
  elif ((LAST_TRANSPORT_FAILURE)); then
    LAST_RESULT=TRANSPORT_FAILURE
  elif [[ $LAST_CONFIG != PASS || $LAST_LIVENESS != PASS || $LAST_CHECK != PASS ]]; then
    LAST_RESULT=KERNEL_BEHAVIOR_CHANGED
  elif [[ $LAST_RATE == PASS ]]; then
    LAST_RESULT=PASS
  else
    LAST_RESULT=RATE_FAIL
  fi
  log_note "[$label] result=$LAST_RESULT guest_fault=$LAST_GUEST_FAULT transport_failure=$LAST_TRANSPORT_FAILURE zero_irq_cpus=$LAST_ZERO_IRQ"
}

case ${ALLOW_BUSY_HOST:-0} in
  0|'') busy_override=0 ;;
  1) busy_override=1 ;;
  *)
    environment_fail invalid-allow-busy-host 'Use ALLOW_BUSY_HOST=1 only for a non-certifiable diagnostic run.'
    exit 1
    ;;
esac

if [[ ! -e /dev/kvm ]]; then
  environment_fail kvm-missing '/dev/kvm does not exist.'
  exit 1
fi
if [[ ! -r /dev/kvm ]]; then
  environment_fail kvm-not-readable '/dev/kvm is not readable by the normal project user.'
  exit 1
fi
if [[ ! -w /dev/kvm ]]; then
  environment_fail kvm-not-writable \
    '/dev/kvm is not writable by the normal project user.' \
    'Do not use sudo for the benchmark; correct this user account membership in the kvm group.'
  exit 1
fi
if ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
  environment_fail qemu-missing 'qemu-system-x86_64 was not found.'
  exit 1
fi
qemu_version=$(qemu-system-x86_64 --version | sed -n '1p')
if [[ $qemu_version != 'QEMU emulator version 8.2.2 '* ]]; then
  environment_fail qemu-version-mismatch "Expected QEMU 8.2.2; found: $qemu_version"
  exit 1
fi
host_nproc=$(nproc)
if [[ ! $host_nproc =~ ^[0-9]+$ || $host_nproc -lt 8 ]]; then
  environment_fail insufficient-nproc "nproc=$host_nproc required=8"
  exit 1
fi
allowed_line=$(grep '^Cpus_allowed_list' /proc/self/status || true)
allowed_list=$(awk '{print $2}' <<<"$allowed_line")
expand_cpu_list "$allowed_list"
if ((${#AVAILABLE_CPUS[@]} < 8)); then
  environment_fail insufficient-cpus-allowed \
    "Cpus_allowed_list=$allowed_list count=${#AVAILABLE_CPUS[@]} required=8"
  exit 1
fi
if ! check_user_processes; then
  exit 1
fi
if [[ ! -f kernel.elf || ! -f hobbyos.img ]]; then
  environment_fail image-missing \
    'kernel.elf and hobbyos.img must be prepared before cleaning and stabilizing the host.' \
    'The runner will not build them automatically.'
  exit 1
fi
for source_path in "${frozen_sources[@]}"; do
  if [[ ! -f $source_path ]]; then
    environment_fail frozen-source-missing "$source_path"
    exit 1
  fi
done

shopt -s nullglob
existing_artifacts=("$artifact_dir"/"$prefix"*)
shopt -u nullglob
if ((${#existing_artifacts[@]} > 0)); then
  environment_fail fix13-artifacts-already-exist \
    'Preserve the existing FIX13 batch; do not replace or retry individual runs.'
  printf '%s\n' "${existing_artifacts[@]}"
  exit 1
fi

harness_lock
trap cleanup EXIT INT TERM

if ! clean_project_environment; then
  printf '%s\n' '[LAPIC_CLEAN_HOST][ENVIRONMENT] FAIL reason=workspace-qemu-active'
  exit 1
fi
printf '%s\n' '[LAPIC_CLEAN_HOST][STABILIZE] START seconds=60'
sleep 60
if ! check_user_processes; then
  exit 1
fi
if ! clean_project_environment; then
  printf '%s\n' \
    '[LAPIC_CLEAN_HOST][ENVIRONMENT] FAIL reason=workspace-qemu-active-after-stabilization'
  exit 1
fi
printf '%s\n' '[LAPIC_CLEAN_HOST][STABILIZE] PASS seconds=60'

mkdir -p "$artifact_dir"
: >"$summary_log"
printf 'run\tconfig\tliveness\tworst_median\tmin_round\tmax_round\taccount_check\tload1_before\tload5_before\tpsi_some_before\tqemu_cpu_after\tresult\n' \
  >"$summary_tsv"

log_note "[LAPIC_CLEAN_HOST][ENVIRONMENT] PASS qemu_version=$qemu_version nproc=$host_nproc cpus_available=${#AVAILABLE_CPUS[@]} allowed=$allowed_list busy_override=$busy_override"
log_note '[LAPIC_CLEAN_HOST][STABILIZE] PASS seconds=60'
log_note '[LAPIC_CLEAN_HOST][INPUT] SHA256'
sha256sum kernel.elf hobbyos.img | tee -a "$summary_log"
sha256sum "${frozen_sources[@]}" >"$frozen_before"

config_passes=0
liveness_passes=0
rate_passes=0
account_passes=0
guest_faults=0
transport_failures=0
zero_irq_runs=0
kernel_behavior_runs=0

for run in 1 2 3 4 5; do
  run_boot "$run"
  before=$artifact_dir/${prefix}-run${run}-host-before.env
  after=$artifact_dir/${prefix}-run${run}-host-after.env
  host_summary_values "$before" "$after"
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$run" "$LAST_CONFIG" "$LAST_LIVENESS" "$LAST_WORST" "$LAST_MIN" "$LAST_MAX" \
    "$LAST_CHECK" "$HOST_LOAD1" "$HOST_LOAD5" "$HOST_PSI_SOME" "$HOST_QEMU_CPU" "$LAST_RESULT" \
    >>"$summary_tsv"
  [[ $LAST_CONFIG == PASS ]] && config_passes=$((config_passes + 1))
  [[ $LAST_LIVENESS == PASS ]] && liveness_passes=$((liveness_passes + 1))
  [[ $LAST_RATE == PASS ]] && rate_passes=$((rate_passes + 1))
  [[ $LAST_CHECK == PASS ]] && account_passes=$((account_passes + 1))
  ((LAST_GUEST_FAULT)) && guest_faults=$((guest_faults + 1))
  ((LAST_TRANSPORT_FAILURE)) && transport_failures=$((transport_failures + 1))
  if [[ $LAST_ZERO_IRQ =~ ^[0-9]+$ && $LAST_ZERO_IRQ -gt 0 ]]; then
    zero_irq_runs=$((zero_irq_runs + 1))
  fi
  [[ $LAST_RESULT == KERNEL_BEHAVIOR_CHANGED ]] && \
    kernel_behavior_runs=$((kernel_behavior_runs + 1))
done

log_note '[LAPIC_CLEAN_HOST][COLLECTION] PASS runs=5'
sha256sum "${frozen_sources[@]}" >"$frozen_after"
if cmp -s "$frozen_before" "$frozen_after"; then
  frozen_equal=1
  log_note '[LAPIC_CLEAN_HOST][FREEZE] PASS hashes_equal=yes'
else
  frozen_equal=0
  log_note '[LAPIC_CLEAN_HOST][FREEZE] FAIL hashes_equal=no'
fi

if workspace_qemu_active || qemu_agent_active || [[ -S .qemu/hmp.sock ]]; then
  final_qemu_inactive=0
  transport_failures=$((transport_failures + 1))
  log_note '[LAPIC_CLEAN_HOST][CLEANUP] FAIL qemu_active=yes'
else
  final_qemu_inactive=1
  log_note '[LAPIC_CLEAN_HOST][CLEANUP] PASS qemu_active=no'
fi

log_note "[LAPIC_CLEAN_HOST][SUMMARY] config=$config_passes/5 liveness=$liveness_passes/5 rate_2000x5=$rate_passes/5 account_check=$account_passes/5 guest_faults=$guest_faults transport_failures=$transport_failures zero_irq_runs=$zero_irq_runs frozen_equal=$frozen_equal qemu_inactive=$final_qemu_inactive busy_override=$busy_override"

if ((kernel_behavior_runs > 0 || guest_faults > 0 || zero_irq_runs > 0 || frozen_equal == 0 ||
      config_passes != 5 || liveness_passes != 5 || account_passes != 5 ||
      transport_failures > 0 || final_qemu_inactive == 0)); then
  final_result=KERNEL_BEHAVIOR_CHANGED
elif ((busy_override)); then
  final_result=NON_CERTIFIABLE_BUSY_RUN
elif ((rate_passes == 5)); then
  final_result=READY_FOR_FINAL_CERTIFICATION
else
  final_result=HOST_STILL_UNSUITABLE
fi

log_note "[LAPIC_CLEAN_HOST][RESULT] $final_result"
log_note '[LAPIC_CLEAN_HOST][DONE] Five fixed boots completed; no retry, affinity, priority change, build, or regression was used.'
