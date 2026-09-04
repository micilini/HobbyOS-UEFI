#!/usr/bin/env bash
set -Eeuo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

smp=
accel=
quantum=default
autorun=0
run_tasktest=0
timeout=180
prefix=

usage(){
  echo "Usage: $0 --smp N --accel tcg|kvm --quantum N|default --autorun [--tasktest] --timeout SEC --prefix NAME" >&2
  exit 2
}

while (($#)); do
  case "$1" in
    --smp) (($# >= 2)) || usage; smp=$2; shift 2;;
    --accel) (($# >= 2)) || usage; accel=$2; shift 2;;
    --quantum) (($# >= 2)) || usage; quantum=$2; shift 2;;
    --autorun) autorun=1; shift;;
    --tasktest) run_tasktest=1; shift;;
    --timeout) (($# >= 2)) || usage; timeout=$2; shift 2;;
    --prefix) (($# >= 2)) || usage; prefix=$2; shift 2;;
    *) usage;;
  esac
done

[[ $smp =~ ^[1-9][0-9]*$ ]] || usage
[[ $accel == tcg || $accel == kvm ]] || usage
[[ $timeout =~ ^[1-9][0-9]*$ ]] || usage
[[ -n $prefix && $prefix =~ ^[A-Za-z0-9._-]+$ ]] || usage
[[ $quantum == default || $quantum =~ ^[1-9][0-9]*$ ]] || usage

if [[ $accel == kvm && ! -r /dev/kvm ]]; then
  echo "[SELFTEST][SKIP] scheduler.kvm.only severity=P1 reason=environment_unavailable"
  exit 3
fi
if [[ $accel == kvm && ! -w /dev/kvm ]]; then
  echo "[SELFTEST][SKIP] scheduler.kvm.only severity=P1 reason=environment_unavailable"
  exit 3
fi

artifact_dir=artifacts/build
serial=.qemu/qemu-serial.log
hmp=(python3 scripts/qemu_hmp.py --socket .qemu/hmp.sock)
harness_start_line=0
harness_command=boot
mkdir -p "$artifact_dir"
source scripts/harness-common.sh
source scripts/harness-runtime-ready.sh
source scripts/harness-framed.sh
source scripts/harness-selftest.sh

cleanup(){ stop_qemu; }
trap cleanup EXIT

extra_flags=
if [[ $quantum != default ]]; then
  extra_flags="-DHOBBYOS_SCHED_TEST_QUANTUM=$quantum"
fi

build_log="$artifact_dir/${prefix}-build.log"
full_log="$artifact_dir/${prefix}.log"
selftest_log="$artifact_dir/${prefix}-selftest.log"
json_log="$artifact_dir/${prefix}-selftest.json"
markdown_log="$artifact_dir/${prefix}-selftest.md"

make kernel-check JOBS=2 SELFTEST=1 SELFTEST_AUTORUN="$autorun" \
  KERNEL_EXTRA_CFLAGS="$extra_flags"
cp artifacts/build/kernel-check-j2.log "$build_log"
make image SELFTEST=1 SELFTEST_AUTORUN="$autorun" \
  KERNEL_EXTRA_CFLAGS="$extra_flags" >/dev/null

harness_lock
HARNESS_AUTORUN_EXPECTED=$autorun
start_qemu "$smp" "$accel"
cp "$serial" "$full_log"

if ((autorun)); then
  selftest_validate_autorun "$serial" "$artifact_dir/$prefix" \
    accounting.sleep.minimum_deadline \
    accounting.sleep.one_ms_granularity \
    accounting.sleep.late_is_diagnostic \
    accounting.sleep.wait_result \
    accounting.sleep.percentiles
fi

grep -Fq '[BOOT][GLOBAL_INIT] PASS' "$serial"
grep -Fq '[BOOT][SHELL_READY] PASS' "$serial"
[[ $(grep -Fc '[BOOT][CPU_INIT] PASS' "$serial") == "$smp" ]]
[[ $(grep -Fc '[BOOT][IDLE] PASS' "$serial") == "$smp" ]]

if ((run_tasktest)); then
  send_complete "tasktest all" "[SELFTEST][SUMMARY]" "$timeout" stress
  grep -Eq '\[SELFTEST\]\[SUMMARY\] pass=[1-9][0-9]* fail=0 skip=0 p0_fail=0 p1_fail=0 p2_fail=0' "$serial"
  send_complete "taskdiag check" "[TASKDIAG][CHECK] PASS" 90
  send_complete "tasktest transport-status" "[HARNESS][STATUS]" 90 stress
  status_line=$(grep -F '[HARNESS][STATUS]' "$serial" | tail -1)
  [[ $status_line =~ accepted=([0-9]+)[[:space:]]completed=([0-9]+) ]]
  [[ ${BASH_REMATCH[1]} == "${BASH_REMATCH[2]}" ]]
fi

cp "$serial" "$full_log"
assert_clean_log "$full_log"
sha256sum kernel.elf >"$artifact_dir/${prefix}-kernel.sha256"
stop_qemu
harness_unlock
trap - EXIT

echo "[CL13][QEMU_SELFTEST] PASS prefix=$prefix smp=$smp accel=$accel quantum=$quantum"
