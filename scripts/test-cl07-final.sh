#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
duration_ms=${DURATION_MS:-120000}
[[ $duration_ms =~ ^[0-9]+$ ]] && ((duration_ms>=120000)) || { echo "DURATION_MS must be >= 120000" >&2; exit 2; }
serial=.qemu/qemu-serial.log
artifact=artifacts/build/cl08fix6-cl07-soak-smp8.log
hmp=(python3 scripts/qemu_hmp.py --socket .qemu/hmp.sock)
mkdir -p artifacts/build
harness_start_line=0;harness_command=boot
source scripts/harness-common.sh
harness_lock
cleanup(){ scripts/qemu-agent.sh stop >/dev/null 2>&1 || true; }
trap cleanup EXIT
ms(){ python3 -c 'import time; print(time.monotonic_ns()//1000000)'; }
count(){ grep -F -c -- "$1" "$serial" 2>/dev/null || true; }
wait_new(){
 local pattern=$1 before=$2 timeout=${3:-180}
 local end=$((SECONDS+timeout))
 while ((SECONDS<=end)); do
  (( $(count "$pattern") > before )) && return 0
  sleep 1
 done
 echo "timeout waiting for new: $pattern" >&2
 return 1
}
send_without_sync(){
 local command=$1 pattern=$2 timeout=${3:-180} profile=${4:-normal} before
 before=$(count "$pattern")
 hmp_text "$command" "$profile" --enter >/dev/null
 wait_new "$pattern" "$before" "$timeout"
}
send(){ send_complete "$@"; }
modal_cycle(){
 local begin end
 begin=$(count "[MODAL] session_begin OK")
 end=$(count "[MODAL] session_end OK")
 hmp_text taskman normal --enter >/dev/null
 wait_new "[MODAL] session_begin OK" "$begin" 30
 sleep .3
 hmp_key esc normal >/dev/null
 wait_new "[MODAL] session_end OK" "$end" 30
 shell_sync
}

source scripts/harness-common.sh
python3 scripts/qemu_hmp.py --selftest-keymap
scripts/qemu-agent.sh stop >/dev/null 2>&1 || true
SMP=${SMP:-8} ACCEL=${ACCEL:-auto} scripts/qemu-agent.sh start >/dev/null
scripts/wait-for-log.sh "$serial" "[KERNEL] Entering Main Loop." 90
start=$(ms)
printf '[KILLTEST][SOAK] START monotonic_ms=%s duration_ms=%s\n' "$start" "$duration_ms" >"$artifact"
send "killtest ui-telemetry on" "[KILLTEST][UI_TELEMETRY] ON" 30
send "synctest preblock-sem" "[SYNCTEST][PREBLOCK_SEM] PASS" 60
send "synctest preblock-timer" "[SYNCTEST][PREBLOCK_TIMER] PASS" 60
send "synctest preblock-loop 100 all" \
  "[SYNCTEST][PREBLOCK_LOOP] PASS count=100" 300

core_cycles=0 timeout_rounds=0 sweeps=0 modal_cycles=0 checks=0
while :; do
 now=$(ms)
 ((now-start>=duration_ms)) && break
 send "killtest all" "[KILLTEST][ALL] PASS" 180
 core_cycles=$((core_cycles+1))
 send "killtest timeout-race 1000" "[KILLTEST][TIMEOUT_RACE] PASS rounds=1000" 240
 timeout_rounds=$((timeout_rounds+1000))
  send "smpstress 16 0 250 9999 0 250" "[SMP] smpstress spawning workers=16" 60
  send "killtest smpstress-sweep" "[SMP][KILL_SWEEP] PASS workers=16" 120
  post_stress_barrier
  sweeps=$((sweeps+1))
  send "killtest sleeping" "[KILLTEST][SLEEPING] PASS" 60 stress
 send "killtest blocked" "[KILLTEST][BLOCKED] PASS" 60
 send "killtest duplicate" "[KILLTEST][DUPLICATE] PASS" 60
 send "killtest exiting-normal" "[KILLTEST][EXITING_NORMAL] PASS" 60
 send ps "[PS][SNAPSHOT]" 30
 modal_cycle
 modal_cycles=$((modal_cycles+1))
 send "killtest check" "[KILLTEST][CHECK] PASS" 60
 send "schedtest check" "[SCHED][CHECK] PASS" 60
 send "synctest check" "[SYNC][CHECK] PASS" 60
 send "accounttest check" "[ACCOUNT][CHECK] PASS" 60
 checks=$((checks+1))
done
while ((core_cycles<3||timeout_rounds<10000||sweeps<3||modal_cycles<5||checks<5)); do
 send "killtest all" "[KILLTEST][ALL] PASS" 180; core_cycles=$((core_cycles+1))
 send "killtest timeout-race 1000" "[KILLTEST][TIMEOUT_RACE] PASS rounds=1000" 240; timeout_rounds=$((timeout_rounds+1000))
 send "smpstress 16 0 250 9999 0 250" "[SMP] smpstress spawning workers=16" 60
 send "killtest smpstress-sweep" "[SMP][KILL_SWEEP] PASS workers=16" 120; sweeps=$((sweeps+1))
 post_stress_barrier
 modal_cycle; modal_cycles=$((modal_cycles+1))
 send "killtest check" "[KILLTEST][CHECK] PASS" 60
 send "schedtest check" "[SCHED][CHECK] PASS" 60
 send "synctest check" "[SYNC][CHECK] PASS" 60
 send "accounttest check" "[ACCOUNT][CHECK] PASS" 60; checks=$((checks+1))
done
send "killtest ui-telemetry off" "[KILLTEST][UI_TELEMETRY] OFF" 30
end=$(ms); elapsed=$((end-start))
printf '[KILLTEST][SOAK] END monotonic_ms=%s elapsed_ms=%s\n' "$end" "$elapsed" >>"$artifact"
printf '[KILLTEST][SOAK] COUNTS core=%s timeout_rounds=%s sweeps=%s taskman=%s checks=%s\n' "$core_cycles" "$timeout_rounds" "$sweeps" "$modal_cycles" "$checks" >>"$artifact"
cat "$serial" >>"$artifact"
((elapsed>=duration_ms))
! grep -E 'PANIC|panic|#PF|#GP|FATAL' "$serial"
cleanup
trap - EXIT
cp "$serial" artifacts/build/cl08fix6-cl07-regression-smp8.log
echo "[CL07][REGRESSION] PASS" | tee -a artifacts/build/cl08fix6-cl07-regression-smp8.log
exit 0
