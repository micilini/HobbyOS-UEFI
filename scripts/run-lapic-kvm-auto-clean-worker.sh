#!/usr/bin/env bash
set -euo pipefail

readonly EXPECTED_BRANCH=feat/taskman
readonly EXPECTED_HEAD=88645901d9393f4796af84cecd9e13d3d6671c70

if ((EUID == 0)); then
  printf '%s\n' '[LAPIC_AUTOCLEAN][ENVIRONMENT] FAIL reason=do-not-run-as-root'
  exit 1
fi

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
cd "$REPO_ROOT"

readonly ARTIFACT_DIR=artifacts/build
readonly WORKER_LOG=$ARTIFACT_DIR/cl11fix14-autoclean-worker.log
readonly BEFORE_FILE=$ARTIFACT_DIR/cl11fix14-autoclean-processes-before.tsv
readonly AFTER_FILE=$ARTIFACT_DIR/cl11fix14-autoclean-processes-after.tsv
readonly CLOSED_FILE=$ARTIFACT_DIR/cl11fix14-autoclean-closed.tsv
readonly RESULT_FILE=$ARTIFACT_DIR/cl11fix14-autoclean-result.txt
readonly HOST_FILE=$ARTIFACT_DIR/cl11fix14-autoclean-host.env
readonly PACKAGE=$ARTIFACT_DIR/cl11fix14-autoclean-results.tar.gz
readonly OPERATOR_CONSOLE=$ARTIFACT_DIR/cl11fix13-operator-console.log
readonly RUNNER=$REPO_ROOT/scripts/run-lapic-kvm-clean-host.sh

readonly -a ALLOWLIST=(
  chrome chromium chromium-browse brave brave-browser firefox slack code
  code-insiders codium VSCodium Discord discord steam steamwebhelper spotify
  teams-for-linux msedge opera vivaldi-bin
)

readonly -a PROTECTED=(
  bash zsh fish sh sshd ssh gnome-terminal gnome-terminal-server
  gnome-terminal- konsole xterm systemd dbus-daemon dbus-broker Xorg Xwayland
  gnome-shell plasmashell NetworkManager pipewire wireplumber pulseaudio login
  agetty
)

mkdir -p "$ARTIFACT_DIR"
touch "$WORKER_LOG"
exec > >(tee -a "$WORKER_LOG") 2>&1

RUN_ID=
if (($# == 2)) && [[ $1 == --run-id ]]; then
  RUN_ID=$2
else
  printf '%s\n' '[LAPIC_AUTOCLEAN][ENVIRONMENT] FAIL reason=invalid-worker-arguments'
  exit 2
fi
[[ $RUN_ID =~ ^[A-Za-z0-9._-]+$ ]] || {
  printf '%s\n' '[LAPIC_AUTOCLEAN][ENVIRONMENT] FAIL reason=invalid-run-id'
  exit 2
}

in_array() {
  local needle=$1 item
  shift
  for item in "$@"; do
    [[ $needle == "$item" ]] && return 0
  done
  return 1
}

is_allowlisted() {
  in_array "$1" "${ALLOWLIST[@]}"
}

action_for_comm() {
  local comm=$1
  if is_allowlisted "$comm"; then
    printf 'TERM_THEN_KILL_IF_NEEDED\n'
  elif in_array "$comm" "${PROTECTED[@]}"; then
    printf 'PROTECTED\n'
  else
    printf 'OBSERVE_ONLY\n'
  fi
}

write_process_snapshot() {
  local destination=$1 temporary
  local pid comm pcpu pmem action

  temporary=$destination.tmp.$$
  printf 'pid\tcomm\tpcpu\tpmem\taction\n' >"$temporary"
  while read -r pid comm pcpu pmem; do
    [[ $pid =~ ^[0-9]+$ && -n $comm ]] || continue
    action=$(action_for_comm "$comm")
    printf '%s\t%s\t%s\t%s\t%s\n' "$pid" "$comm" "$pcpu" "$pmem" "$action" \
      >>"$temporary"
  done < <(LC_ALL=C ps -u "$UID" -o pid=,comm=,pcpu=,pmem=)
  mv "$temporary" "$destination"
}

list_codex_pids() {
  {
    pgrep -u "$UID" -x codex 2>/dev/null || true
    pgrep -u "$UID" -f 'codex-code-mode' 2>/dev/null || true
  } | awk -v self="$$" '$1 ~ /^[0-9]+$/ && $1 != self { seen[$1]=1 }
    END { for (pid in seen) print pid }' | LC_ALL=C sort -n
}

list_allowlisted_processes() {
  local pid comm pcpu pmem
  while read -r pid comm pcpu pmem; do
    [[ $pid =~ ^[0-9]+$ && -n $comm ]] || continue
    if is_allowlisted "$comm"; then
      printf '%s\t%s\t%s\t%s\n' "$pid" "$comm" "$pcpu" "$pmem"
    fi
  done < <(LC_ALL=C ps -u "$UID" -o pid=,comm=,pcpu=,pmem=)
}

pid_identity_matches() {
  local pid=$1 expected_comm=$2 real_uid current_comm
  [[ $pid =~ ^[0-9]+$ && -r /proc/$pid/status && -r /proc/$pid/comm ]] || return 1
  real_uid=$(awk '$1 == "Uid:" { print $2; exit }' "/proc/$pid/status" 2>/dev/null || true)
  current_comm=$(sed -n '1p' "/proc/$pid/comm" 2>/dev/null || true)
  [[ $real_uid == "$UID" && $current_comm == "$expected_comm" ]] || return 1
  is_allowlisted "$current_comm"
}

workspace_qemu_pids() {
  local pid cwd
  while read -r pid; do
    [[ $pid =~ ^[0-9]+$ ]] || continue
    cwd=$(readlink "/proc/$pid/cwd" 2>/dev/null || true)
    [[ $cwd == "$REPO_ROOT" ]] && printf '%s\n' "$pid"
  done < <(pgrep -f '[q]emu-system-x86_64' 2>/dev/null || true)
}

stop_workspace_qemu() {
  scripts/qemu-agent.sh stop >/dev/null 2>&1 || true
  if [[ -z $(workspace_qemu_pids) ]]; then
    rm -f .qemu/hmp.sock .qemu/qemu.pid
    printf '%s\n' '[LAPIC_AUTOCLEAN][CLEANUP] PASS qemu_active=no'
    return 0
  fi
  printf '%s\n' '[LAPIC_AUTOCLEAN][CLEANUP] FAIL qemu_active=yes'
  return 1
}

package_results() {
  local temporary=$ARTIFACT_DIR/.cl11fix14-autoclean-results.$RUN_ID.tar.gz
  local path
  local -a files=()

  shopt -s nullglob
  for path in "$ARTIFACT_DIR"/cl11fix13-clean-kvm-* \
    "$OPERATOR_CONSOLE" "$ARTIFACT_DIR"/cl11fix14-autoclean-*; do
    [[ -e $path && $path != "$PACKAGE" ]] && files+=("$path")
  done
  shopt -u nullglob
  for path in \
    scripts/run-lapic-kvm-clean-host.sh \
    scripts/launch-lapic-kvm-auto-clean.sh \
    scripts/run-lapic-kvm-auto-clean-worker.sh \
    docs/taskman-v1/reports/TMV1-CL-11-FIX13-manual-run-instructions.md \
    docs/taskman-v1/reports/TMV1-CL-11-FIX14-auto-clean-instructions.md \
    AGENTS.md; do
    [[ -f $path ]] && files+=("$path")
  done

  if ((${#files[@]} == 0)); then
    printf '%s\n' '[LAPIC_AUTOCLEAN][PACKAGE] FAIL reason=no-files'
    return 1
  fi
  tar -czf "$temporary" -- "${files[@]}"
  mv "$temporary" "$PACKAGE"
  printf '[LAPIC_AUTOCLEAN][PACKAGE] PASS file=%s files=%s\n' "$PACKAGE" "${#files[@]}"
}

write_result() {
  local result=$1 source_line=${2:-}
  {
    [[ -n $source_line ]] && printf '%s\n' "$source_line"
    printf '[LAPIC_AUTOCLEAN][RESULT] %s\n' "$result"
  } >"$RESULT_FILE"
  printf '[LAPIC_AUTOCLEAN][RESULT] %s\n' "$result"
}

notify_result() {
  local result=$1
  if command -v notify-send >/dev/null 2>&1; then
    notify-send 'HobbyOS LAPIC benchmark finished' "$result" >/dev/null 2>&1 || true
  fi
}

finish() {
  local result=$1 status=$2 source_line=${3:-}
  if ! stop_workspace_qemu; then
    result=AUTOCLEAN_FAILED
    status=1
    source_line=
  fi
  write_result "$result" "$source_line"
  if ! package_results; then
    result=AUTOCLEAN_FAILED
    status=1
    write_result "$result"
  fi
  notify_result "$result"
  exit "$status"
}

cleanup() {
  local status=$?
  trap - EXIT INT TERM
  stop_workspace_qemu || status=1
  if [[ ! -s $RESULT_FILE ]]; then
    write_result AUTOCLEAN_FAILED
    package_results || true
    notify_result AUTOCLEAN_FAILED
  fi
  exit "$status"
}
trap cleanup EXIT INT TERM

worker_preflight() {
  local branch head qemu_version
  branch=$(git branch --show-current 2>/dev/null || true)
  head=$(git rev-parse HEAD 2>/dev/null || true)
  [[ $branch == "$EXPECTED_BRANCH" && $head == "$EXPECTED_HEAD" ]] || return 1
  [[ -f $RUNNER && -f kernel.elf && -f hobbyos.img ]] || return 1
  bash -n "$RUNNER" || return 1
  [[ -e /dev/kvm && -r /dev/kvm && -w /dev/kvm ]] || return 1
  command -v qemu-system-x86_64 >/dev/null 2>&1 || return 1
  qemu_version=$(qemu-system-x86_64 --version | sed -n '1p')
  [[ $qemu_version == 'QEMU emulator version 8.2.2 '* ]] || return 1
  shopt -s nullglob
  local existing=("$ARTIFACT_DIR"/cl11fix13-clean-kvm-*)
  shopt -u nullglob
  ((${#existing[@]} == 0))
}

wait_for_codex_exit() {
  local deadline=$((SECONDS + 900)) pids last_report=-30
  printf '%s\n' '[LAPIC_AUTOCLEAN][WAIT_CODEX] START timeout_seconds=900'
  while ((SECONDS <= deadline)); do
    pids=$(list_codex_pids)
    if [[ -z $pids ]]; then
      printf '%s\n' '[LAPIC_AUTOCLEAN][WAIT_CODEX] PASS processes=0'
      return 0
    fi
    if ((SECONDS - last_report >= 30)); then
      printf '[LAPIC_AUTOCLEAN][WAIT_CODEX] active_pids=%s\n' "$(tr '\n' ',' <<<"$pids" | sed 's/,$//')"
      last_report=$SECONDS
    fi
    sleep 2
  done
  printf '%s\n' '[LAPIC_AUTOCLEAN][RESULT] CODEX_DID_NOT_EXIT'
  return 1
}

close_allowlisted_processes() {
  local pid comm pcpu pmem timestamp deadline remaining
  local -a pids=()
  declare -A comm_by_pid=() term_by_pid=() outcome_by_pid=() final_by_pid=()

  printf 'pid\tcomm\tTERM timestamp\toutcome\tfinal timestamp\n' >"$CLOSED_FILE"
  while IFS=$'\t' read -r pid comm pcpu pmem; do
    [[ $pid =~ ^[0-9]+$ ]] || continue
    pids+=("$pid")
    comm_by_pid[$pid]=$comm
  done < <(list_allowlisted_processes)

  for pid in "${pids[@]}"; do
    comm=${comm_by_pid[$pid]}
    if ! pid_identity_matches "$pid" "$comm"; then
      outcome_by_pid[$pid]='exited before TERM'
      final_by_pid[$pid]=$(date --iso-8601=seconds)
      continue
    fi
    timestamp=$(date --iso-8601=seconds)
    if kill -TERM -- "$pid" 2>/dev/null; then
      term_by_pid[$pid]=$timestamp
      printf '[LAPIC_AUTOCLEAN][TERM] pid=%s comm=%s timestamp=%s\n' "$pid" "$comm" "$timestamp"
    else
      outcome_by_pid[$pid]='TERM failed'
      final_by_pid[$pid]=$(date --iso-8601=seconds)
    fi
  done

  deadline=$((SECONDS + 30))
  while ((SECONDS < deadline)); do
    remaining=0
    for pid in "${pids[@]}"; do
      [[ -n ${term_by_pid[$pid]:-} && -z ${outcome_by_pid[$pid]:-} ]] || continue
      comm=${comm_by_pid[$pid]}
      if pid_identity_matches "$pid" "$comm"; then
        remaining=$((remaining + 1))
      else
        outcome_by_pid[$pid]='exited gracefully'
        final_by_pid[$pid]=$(date --iso-8601=seconds)
      fi
    done
    ((remaining == 0)) && break
    sleep 1
  done

  for pid in "${pids[@]}"; do
    [[ -n ${term_by_pid[$pid]:-} && -z ${outcome_by_pid[$pid]:-} ]] || continue
    comm=${comm_by_pid[$pid]}
    if pid_identity_matches "$pid" "$comm"; then
      timestamp=$(date --iso-8601=seconds)
      if kill -KILL -- "$pid" 2>/dev/null; then
        outcome_by_pid[$pid]='KILL required'
        final_by_pid[$pid]=$timestamp
        printf '[LAPIC_AUTOCLEAN][KILL] pid=%s comm=%s timestamp=%s\n' "$pid" "$comm" "$timestamp"
      else
        outcome_by_pid[$pid]='KILL failed'
        final_by_pid[$pid]=$timestamp
      fi
    else
      outcome_by_pid[$pid]='exited gracefully'
      final_by_pid[$pid]=$(date --iso-8601=seconds)
    fi
  done

  for pid in "${pids[@]}"; do
    printf '%s\t%s\t%s\t%s\t%s\n' \
      "$pid" "${comm_by_pid[$pid]}" "${term_by_pid[$pid]:-NA}" \
      "${outcome_by_pid[$pid]:-unknown}" "${final_by_pid[$pid]:-NA}" >>"$CLOSED_FILE"
  done
}

available_value() {
  local path=$1
  if [[ -r $path ]]; then
    sed -n '1p' "$path"
  else
    printf 'unavailable\n'
  fi
}

record_host_state() {
  local path cpu_dir cpu zone
  {
    printf 'timestamp=%s\n' "$(date --iso-8601=seconds)"
    printf 'loadavg='; sed -n '1p' /proc/loadavg
    printf '[cpu_pressure]\n'
    [[ -r /proc/pressure/cpu ]] && sed -n '1,20p' /proc/pressure/cpu || printf 'unavailable\n'
    printf '[memory]\n'
    free -m
    printf '[top_cpu_threads]\n'
    LC_ALL=C ps -eLo pid,tid,psr,pcpu,pmem,stat,comm --sort=-pcpu | sed -n '1,41p'
    printf '[cgroup]\n'
    grep '^Cpus_allowed_list' /proc/self/status || true
    printf 'cpuset.cpus.effective=%s\n' "$(available_value /sys/fs/cgroup/cpuset.cpus.effective)"
    printf 'cpu.max=%s\n' "$(available_value /sys/fs/cgroup/cpu.max)"
    printf '[cpu_frequency]\n'
    shopt -s nullglob
    for path in /sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_governor; do
      cpu_dir=${path%/cpufreq/scaling_governor}
      cpu=${cpu_dir##*/}
      printf '%s governor=%s current_frequency_khz=%s\n' "$cpu" \
        "$(available_value "$path")" \
        "$(available_value "$cpu_dir/cpufreq/scaling_cur_freq")"
    done
    for zone in /sys/class/thermal/thermal_zone*; do
      [[ -d $zone ]] || continue
      printf '%s type=%s temp_millic=%s\n' "${zone##*/}" \
        "$(available_value "$zone/type")" "$(available_value "$zone/temp")"
    done
    shopt -u nullglob
    if command -v sensors >/dev/null 2>&1; then
      printf '[sensors]\n'
      sensors 2>&1 || true
    fi
  } >"$HOST_FILE"
  sed -n '1,240p' "$HOST_FILE"
}

printf '[LAPIC_AUTOCLEAN][WORKER] START run_id=%s pid=%s root=%s\n' "$RUN_ID" "$$" "$REPO_ROOT"

if ! wait_for_codex_exit; then
  finish CODEX_DID_NOT_EXIT 1
fi

if ! worker_preflight; then
  printf '%s\n' '[LAPIC_AUTOCLEAN][ENVIRONMENT] FAIL reason=worker-preflight'
  finish AUTOCLEAN_FAILED 1
fi

write_process_snapshot "$BEFORE_FILE"
close_allowlisted_processes
sleep 2
write_process_snapshot "$AFTER_FILE"

if ! stop_workspace_qemu; then
  finish AUTOCLEAN_FAILED 1
fi
if [[ -n $(workspace_qemu_pids) ]]; then
  printf '%s\n' '[LAPIC_AUTOCLEAN][STABILIZE] FAIL reason=workspace-qemu-active'
  finish AUTOCLEAN_FAILED 1
fi
if [[ -n $(list_codex_pids) ]]; then
  printf '%s\n' '[LAPIC_AUTOCLEAN][STABILIZE] FAIL reason=codex-reappeared'
  finish AUTOCLEAN_FAILED 1
fi
if [[ -n $(list_allowlisted_processes) ]]; then
  printf '%s\n' '[LAPIC_AUTOCLEAN][STABILIZE] FAIL reason=allowlisted-apps-remain'
  finish AUTOCLEAN_FAILED 1
fi

printf '%s\n' '[LAPIC_AUTOCLEAN][STABILIZE] START seconds=90'
sleep 90

if [[ -n $(workspace_qemu_pids) || -n $(list_codex_pids) || -n $(list_allowlisted_processes) ]]; then
  printf '%s\n' '[LAPIC_AUTOCLEAN][STABILIZE] FAIL reason=host-processes-reappeared'
  finish AUTOCLEAN_FAILED 1
fi
record_host_state
printf '%s\n' '[LAPIC_AUTOCLEAN][STABILIZE] PASS seconds=90'

unset ALLOW_BUSY_HOST
printf '%s\n' '[LAPIC_AUTOCLEAN][BENCHMARK] START command=scripts/run-lapic-kvm-clean-host.sh'
set +e
scripts/run-lapic-kvm-clean-host.sh |& tee "$OPERATOR_CONSOLE"
runner_status=${PIPESTATUS[0]}
set -e
printf '[LAPIC_AUTOCLEAN][BENCHMARK] END status=%s\n' "$runner_status"

result_line=$(awk '/^\[LAPIC_CLEAN_HOST\]\[RESULT\] / { line=$0 } END { print line }' \
  "$OPERATOR_CONSOLE")
result=${result_line##* }
case $result in
  READY_FOR_FINAL_CERTIFICATION|HOST_STILL_UNSUITABLE|NON_CERTIFIABLE_BUSY_RUN|KERNEL_BEHAVIOR_CHANGED)
    finish "$result" 0 "$result_line"
    ;;
  *)
    printf '[LAPIC_AUTOCLEAN][BENCHMARK] FAIL runner_status=%s result_line=%s\n' \
      "$runner_status" "${result_line:-missing}"
    finish AUTOCLEAN_FAILED 1
    ;;
esac
