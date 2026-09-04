#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd);cd "$root"
serial=.qemu/qemu-serial.log
hmp=(python3 scripts/qemu_hmp.py --socket .qemu/hmp.sock)
mkdir -p artifacts/build
harness_start_line=0;harness_command=boot
source scripts/harness-common.sh
harness_lock
cleanup(){ stop_qemu; }
trap cleanup EXIT

checks(){
 send_complete "killtest check" "[KILLTEST][CHECK] PASS" 60
 send_complete "schedtest check" "[SCHED][CHECK] PASS" 60
 send_complete "synctest check" "[SYNC][CHECK] PASS" 60
 send_complete "accounttest check" "[ACCOUNT][CHECK] PASS" 90
}
run_smp(){
 local smp=$1 rounds=$2 workers=$3
 start_qemu "$smp" tcg
 send_complete "killtest all" "[KILLTEST][ALL] PASS" 240
 send_complete "killtest timeout-race $rounds" "[KILLTEST][TIMEOUT_RACE] PASS rounds=$rounds" 600
 if ((smp>=4));then
  send_complete "smpstress $workers 0 250 9999 0 250" "[SMP] smpstress spawning workers=$workers" 60
  # TCG with serial-heavy nested certification can take longer than the
  # dedicated run even while every worker is still making progress.  Keep the
  # exact PASS sentinel and workload, but leave enough observation time for
  # the slowest supported environment.
  send_complete "killtest smpstress-sweep" "[SMP][KILL_SWEEP] PASS workers=$workers" 600
 fi
 if ((smp==4));then
  send_complete "killtest ui-setup" "[KILLTEST][UI_SETUP] PASS" 90
  send_complete "killtest ui-status" "[KILLTEST][UI_STATUS] PASS" 90
  modal_cycle_complete
  send_complete "killtest ui-cleanup" "[KILLTEST][UI_CLEANUP] PASS" 90
 fi
 checks
 cp "$serial" "artifacts/build/cl08fix6-cl07-smp${smp}.log"
 assert_clean_log "artifacts/build/cl08fix6-cl07-smp${smp}.log"
 stop_qemu
}
negative(){
 local macro=$1 command=$2 sentinel=$3 output=$4
 make kernel-check JOBS=2 KERNEL_EXTRA_CFLAGS="-D$macro" >/dev/null
 make image KERNEL_EXTRA_CFLAGS="-D$macro" >/dev/null
 start_qemu 4 tcg
 send_raw_complete "$command" "$sentinel" 120
 cp "$serial" "artifacts/build/$output"
 stop_qemu
 make kernel-check JOBS=2 >/dev/null
 make image >/dev/null
}

run_smp 1 500 1
run_smp 2 2000 2
run_smp 4 5000 8
HARNESS_LOCK_HELD=1 SMP=8 ACCEL=tcg DURATION_MS=120000 scripts/test-cl07-final.sh
negative HOBBYOS_KILL_NEGATIVE_IGNORE_KILLABLE "killtest nonkillable" \
 "[KILLTEST][NEGATIVE] NONKILLABLE_ACCEPTANCE_DETECTED" cl08fix6-negative-cl07-nonkillable.log
negative HOBBYOS_KILL_NEGATIVE_ACCEPT_EXITING "killtest exiting-normal" \
 "[KILLTEST][NEGATIVE] EXITING_ACCEPTED_NORMAL_DETECTED" cl08fix6-negative-cl07-exiting.log
echo "[CL07][MATRIX] PASS smp=1,2,4,8" | tee artifacts/build/cl08fix6-cl07-matrix.log
