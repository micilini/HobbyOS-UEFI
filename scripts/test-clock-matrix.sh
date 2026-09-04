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

validate_launch(){
 local expected_accel=$1 expected_thread=$2 output=$3
 set -a;source .qemu/launch.env;set +a
 [[ $ACCEL == "$expected_accel" && $SMP == 8 ]]
 if [[ $expected_accel == tcg ]];then [[ $TCG_THREAD == "$expected_thread" ]];fi
 [[ -n ${QEMU_VERSION:-} ]]
 cp .qemu/launch.env "$output"
}
run_environment(){
 local label=$1 accel=$2 thread=$3
 TCG_THREAD=$thread start_qemu 8 "$accel"
 validate_launch "$accel" "$thread" "artifacts/build/cl08fix6-clock-${label}.launch.env"
 send_complete "accounttest clock-reset-test-stats" "[ACCOUNT][CLOCK_RESET] PASS" 30
 send_complete "accounttest clock-classifier" "[ACCOUNT][CLOCK_CLASSIFIER] PASS cases=10" 30
 send_complete "accounttest clock-smp 32 1000000" "[ACCOUNT][CLOCK_SMP] PASS" 600
 send_complete "reaptest concurrent-churn 1000 1000 1000" "[REAPTEST][CONCURRENT_CHURN] PASS" 600
 send_complete "accounttest clock-stats" "[ACCOUNT][STATS]" 30
 send_complete "accounttest clock-events" "[ACCOUNT][CLOCK_EVENTS] PASS" 30
 send_complete "accounttest check" "[ACCOUNT][CHECK] PASS" 90
 cp "$serial" "artifacts/build/cl08fix6-clock-${label}.log"
 assert_clean_log "artifacts/build/cl08fix6-clock-${label}.log"
 stop_qemu
}

run_environment multi tcg multi
run_environment single tcg single
if [[ -e /dev/kvm && -r /dev/kvm && -w /dev/kvm ]];then
 run_environment kvm kvm multi
else
 printf 'SKIP_BY_ENVIRONMENT\n' >artifacts/build/cl08fix6-clock-kvm.launch.env
 printf '[CLOCK][KVM] SKIP_BY_ENVIRONMENT\n' >artifacts/build/cl08fix6-clock-kvm.log
fi
echo "[CLOCK][MATRIX] PASS multi=1 single=1 kvm=$([[ -e /dev/kvm && -r /dev/kvm && -w /dev/kvm ]]&&echo 1||echo SKIP)"
