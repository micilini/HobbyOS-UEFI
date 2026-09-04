#!/usr/bin/env bash
harness_lock(){ [[ ${HARNESS_LOCK_HELD:-0} == 1 ]]&&return;mkdir -p .qemu;exec 9>.qemu/harness.lock;flock -n 9||{ echo "another HobbyOS QEMU harness is active" >&2;exit 1;};HARNESS_LOCK_HELD=1; }
harness_unlock(){ stop_qemu;[[ ! -S .qemu/hmp.sock&&! -e .qemu/qemu.pid ]]||{ echo "residual QEMU namespace before nested harness" >&2;return 1;};exec 9>&-;HARNESS_LOCK_HELD=0; }
count(){ grep -F -o -- "$1" "$serial" 2>/dev/null | wc -l || true; }
hmp_text(){ local text=$1 profile=${2:-normal}; shift 2 2>/dev/null||true; "${hmp[@]}" text "$text" --profile "$profile" "$@"; }
hmp_key(){ local key=$1 profile=${2:-normal}; "${hmp[@]}" key "$key" --profile "$profile"; }
harness_diagnose(){ echo "command: $1" >&2;echo "expected: $2" >&2;echo "failure: ${3:-timeout}" >&2;tail -150 "$serial" >&2||true;scripts/qemu-agent.sh status >&2||true; }
wait_new(){ local pattern=$1 before=$2 timeout=${3:-180} deadline fresh fatal;deadline=$((SECONDS+timeout));while ((SECONDS<=deadline));do fresh=$(tail -n +$((harness_start_line+1)) "$serial" 2>/dev/null||true);fatal=$(grep -m1 -E '(^|[[:space:]])FAIL([[:space:]]|$)|PANIC|#PF|#GP|FATAL|STRUCTURAL_FAULT|FINISH_FAULT|QEMU exited' <<<"$fresh"||true);if [[ -n $fatal ]];then harness_diagnose "${harness_command:-unknown}" "$pattern" "$fatal";if grep -q -E 'PANIC|#PF|#GP|FATAL|STRUCTURAL_FAULT|FINISH_FAULT|QEMU exited' <<<"$fatal";then return 3;else return 2;fi;fi;(( $(count "$pattern")>before ))&&return 0;pgrep -f qemu-system-x86_64 >/dev/null||{ harness_diagnose "${harness_command:-unknown}" "$pattern" "QEMU exited";return 3;};sleep 1;done;harness_diagnose "${harness_command:-unknown}" "$pattern" timeout;return 4; }
transport_probe(){
  local attempt nonce pattern before
  for attempt in 1 2 3; do
    HARNESS_TRANSPORT_NONCE=$(( ${HARNESS_TRANSPORT_NONCE:-0}+1 ))
    nonce="transport${HARNESS_TRANSPORT_NONCE}"
    pattern="[INPUTTEST][MARKER] name=$nonce"
    before=$(count "$pattern")
    hmp_text "inputtest marker $nonce" sync --enter >/dev/null
    harness_command="transport probe $nonce"
    harness_start_line=$(wc -l <"$serial" 2>/dev/null||echo 0)
    local deadline=$((SECONDS+30))
    while ((SECONDS<=deadline)); do
      (( $(count "$pattern")>before ))&&return 0
      pgrep -f qemu-system-x86_64 >/dev/null||return 2
      sleep 1
    done
  done
  return 1
}
send_raw_complete(){
  local command=$1 pattern=$2 timeout=${3:-180} profile=${4:-normal} before rc
  harness_command=$command;harness_start_line=$(wc -l <"$serial" 2>/dev/null||echo 0);before=$(count "$pattern")
  hmp_text "$command" "$profile" --enter >/dev/null
  if wait_new "$pattern" "$before" "$timeout"; then return 0; else rc=$?; fi
  if ((rc==2)); then echo "[HMP][FAILURE] GUEST_TEST_FAILURE command=$command" >&2; return 1; fi
  if ((rc==3)); then echo "[HMP][TIMEOUT] GUEST_FAILURE command=$command" >&2; return 1; fi
  if ! pgrep -f qemu-system-x86_64 >/dev/null; then echo "[HMP][TIMEOUT] GUEST_FAILURE command=$command" >&2; return 1; fi
  if transport_probe; then
    echo "[HMP][TIMEOUT] TRANSPORT_LOSS command=$command (not retried)" >&2
  else
    rc=$?; if ((rc==2)); then echo "[HMP][TIMEOUT] GUEST_FAILURE command=$command" >&2; else echo "[HMP][TIMEOUT] SHELL_INPUT_STALL command=$command" >&2; fi
  fi
  return 1
}
shell_sync(){
  local before deadline attempt_deadline fresh fatal
  harness_command="killtest stats"
  harness_start_line=$(wc -l <"$serial" 2>/dev/null||echo 0)
  before=$(count "[KILLTEST][STATS]")
  # Under the SMP stress trace the emulated USB keyboard needs a wider gap
  # between synthetic key transitions.  The marker is instantaneous and
  # idempotent, so this proves shell return without relying on a fixed sleep.
  deadline=$((SECONDS+${SHELL_SYNC_TIMEOUT:-600}))
  while ((SECONDS<=deadline)); do
    hmp_text "killtest stats" sync --enter >/dev/null
    attempt_deadline=$((SECONDS+30))
    while ((SECONDS<=attempt_deadline && SECONDS<=deadline)); do
      fresh=$(tail -n +$((harness_start_line+1)) "$serial" 2>/dev/null||true)
      fatal=$(grep -m1 -E '(^|[[:space:]])FAIL([[:space:]]|$)|PANIC|#PF|#GP|FATAL|STRUCTURAL_FAULT|FINISH_FAULT|QEMU exited' <<<"$fresh"||true)
      [[ -z $fatal ]]||{ harness_diagnose "$harness_command" "[KILLTEST][STATS]" "$fatal";return 1;}
      (( $(count "[KILLTEST][STATS]")>before ))&&return 0
      pgrep -f qemu-system-x86_64 >/dev/null||{ harness_diagnose "$harness_command" "[KILLTEST][STATS]" "QEMU exited";return 1;}
      sleep 1
    done
  done
  harness_diagnose "$harness_command" "[KILLTEST][STATS]" timeout
  return 1
}
send_complete(){ send_raw_complete "$@";shell_sync; }
post_stress_barrier(){
  shell_sync
  transport_probe
  send_complete "inputtest check" "[INPUTTEST][CHECK] PASS" 90 stress
}
modal_cycle_complete(){
  local begin end owner
  begin=$(count "[MODAL] session_begin OK")
  end=$(count "[MODAL] session_end OK")
  owner=$(count "[TASKMAN][OWNER_STATE] shell=BLOCKED session=ACTIVE")
  harness_command="taskman 50"
  harness_start_line=$(wc -l <"$serial" 2>/dev/null||echo 0)
  hmp_text "taskman 50" normal --enter >/dev/null
  wait_new "[MODAL] session_begin OK" "$begin" 120
  wait_new "[TASKMAN][OWNER_STATE] shell=BLOCKED session=ACTIVE" "$owner" 120
  # TCG/xHCI may occasionally drop a single synthetic key transition.  A
  # short ESC burst is safe: the first closes the modal queue and any later
  # typed-ahead is discarded by the next atomic begin transition.
  hmp_key esc normal >/dev/null
  hmp_key esc normal >/dev/null
  hmp_key esc normal >/dev/null
  wait_new "[MODAL] session_end OK" "$end" 120
  shell_sync
}
taskman_killed_cycle_complete(){
  local begin end owner
  send_complete "modaltest arm-kill-next-ui" \
    "[MODALTEST][ARM_KILL_NEXT_UI] OK" 60
  begin=$(count "[MODAL] session_begin OK")
  end=$(count "[MODAL] session_end OK")
  owner=$(count "[TASKMAN][OWNER_STATE] shell=BLOCKED session=ACTIVE")
  harness_command="taskman 50 (armed kill)"
  harness_start_line=$(wc -l <"$serial" 2>/dev/null||echo 0)
  hmp_text "taskman 50" normal --enter >/dev/null
  wait_new "[MODAL] session_begin OK" "$begin" 120
  wait_new "[TASKMAN][OWNER_STATE] shell=BLOCKED session=ACTIVE" "$owner" 120
  wait_new "[MODAL] session_end OK" "$end" 120
  shell_sync
}
stop_qemu(){ scripts/qemu-agent.sh stop >/dev/null 2>&1||true; }
start_qemu(){ local smp=$1 accel=${2:-tcg};stop_qemu;HARNESS_RUNTIME_READY=0;HARNESS_RUNTIME_READY_PID=;[[ ! -S .qemu/hmp.sock&&! -e .qemu/qemu.pid ]]||{ echo "residual QEMU control files" >&2;return 1;};SMP=$smp ACCEL=$accel scripts/qemu-agent.sh start >/dev/null;scripts/wait-for-log.sh "$serial" "[KERNEL] Entering Main Loop." 120 >/dev/null;if declare -F runtime_wait_boot_ready >/dev/null;then runtime_wait_boot_ready "$serial" "${RUNTIME_READY_TIMEOUT:-180}" "${HARNESS_AUTORUN_EXPECTED:-1}";fi; }
assert_clean_log(){ ! grep -E 'PANIC|#PF|#GP|FATAL|STRUCTURAL_FAULT|FINISH_FAULT' "$1"; }
