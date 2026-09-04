#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd); cd "$root"
serial=.qemu/qemu-serial.log
artifact=artifacts/build/cl11fix3-hmp-transport-smp8.log
hmp=(python3 scripts/qemu_hmp.py --socket .qemu/hmp.sock)
mkdir -p artifacts/build
harness_start_line=0; harness_command=boot
source scripts/harness-common.sh
harness_lock
cleanup(){ stop_qemu; }
trap cleanup EXIT

markers(){
  local phase=$1 i name sentinel
  for i in $(seq 1 25); do
    name="hmp-${phase}-${i}"
    sentinel="[INPUTTEST][MARKER] name=$name"
    send_raw_complete "inputtest marker $name" "$sentinel" 60 stress
  done
}

python3 scripts/qemu_hmp.py --selftest-keymap
python3 scripts/qemu_hmp.py --selftest-timing
make image >/dev/null
start_qemu 8 tcg
send_complete "smpstress 16 0 250 9999 0 250" "[SMP] smpstress spawning workers=16" 90 stress
markers during
send_complete "killtest smpstress-sweep" "[SMP][KILL_SWEEP] PASS workers=16" 180 stress
post_stress_barrier
markers after
send_complete "inputtest check" "[INPUTTEST][CHECK] PASS" 90 stress
for phase in during after; do
  [[ $(grep -F "[INPUTTEST][MARKER] name=hmp-$phase-" "$serial" | wc -l) -eq 25 ]]
  [[ $(grep -F "[INPUTTEST][MARKER] name=hmp-$phase-" "$serial" | sort -u | wc -l) -eq 25 ]]
done
assert_clean_log "$serial"
echo "[HMP][TRANSPORT] PASS during_stress=25 after_stress=25 lost=0 duplicates=0" | tee -a "$serial"
cp "$serial" "$artifact"
stop_qemu
trap - EXIT
