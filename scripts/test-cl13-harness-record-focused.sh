#!/usr/bin/env bash
set -Eeuo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
artifact_dir=artifacts/build
serial=.qemu/qemu-serial.log
hmp=(python3 scripts/qemu_hmp.py --socket .qemu/hmp.sock)
harness_start_line=0
harness_command=harness-record-focused
image_ready=0
mkdir -p "$artifact_dir"
source scripts/harness-common.sh
source scripts/harness-runtime-ready.sh
source scripts/harness-framed.sh
source scripts/harness-selftest.sh

usage(){ echo "Usage: $0 host-unit|smp1-tcg|smp4-tcg|smp4-kvm|smp8-tcg|smp8-kvm|all" >&2; exit 2; }
cleanup(){ stop_qemu; }
trap cleanup EXIT

host_unit(){
  local dir broken fenced prefixed linefenced
  dir=$(mktemp -d); broken=$dir/broken; fenced=$dir/fenced
  prefixed=$dir/prefixed; linefenced=$dir/linefenced
  printf '%s\n' '[HARNESS][END] seq=18 status=[TASK][CREATE] id=0' \
    '23 name=synctest-sem-wait' >"$broken"
  [[ -z $(framed_exact_record_line "$broken" '^\[HARNESS\]\[END\] seq=18 status=-?[0-9]+$') ]]
  printf '%s\n' '[TASK][CREATE] id=23 name=x' \
    '[HARNESS][END] seq=18 status=0' >"$fenced"
  [[ -n $(framed_exact_record_line "$fenced" '^\[HARNESS\]\[END\] seq=18 status=0$') ]]
  printf '%s\n' '[TASK][CREATE] partial [HARNESS][END] seq=18 status=0' >"$prefixed"
  [[ -z $(framed_exact_record_line "$prefixed" '^\[HARNESS\]\[END\] seq=18 status=0$') ]]
  printf '%s\n' '[TASK][CREATE] partial' \
    '[HARNESS][END] seq=18 status=0' >"$linefenced"
  [[ -n $(framed_exact_record_line "$linefenced" '^\[HARNESS\]\[END\] seq=18 status=0$') ]]
  rm -r "$dir"
  echo '[CL13][HARNESS_RECORD_HOST_UNIT] PASS'
}

prepare_image(){
  ((image_ready)) && return
  make kernel-check JOBS=2 SELFTEST=1 SELFTEST_AUTORUN=1 >/dev/null
  make image SELFTEST=1 SELFTEST_AUTORUN=1 >/dev/null
  image_ready=1
}

assert_harness_lines(){
  python3 - "$1" <<'PY'
from pathlib import Path
import re,sys
p=Path(sys.argv[1])
allowed=re.compile(r'^\[HARNESS\]\[(?:FRAME|BEGIN|END|REPLAY|STATUS|STATUS_DETAIL|EMIT_ERROR)\]')
seen=0
for number,line in enumerate(p.read_text(errors='replace').splitlines(),1):
    offset=line.find('[HARNESS][')
    if offset>0:
        raise SystemExit(f'prefixed HARNESS record line={number} offset={offset}')
    if offset==0:
        seen+=1
        if not allowed.match(line):
            raise SystemExit(f'malformed HARNESS record line={number}: {line}')
if not seen:
    raise SystemExit('no HARNESS records')
print(seen)
PY
}

stress_boot(){
  local name=$1 smp=$2 accel=$3 log i status_line records
  log="$artifact_dir/cl13fix8-harness-${name}.log"
  start_qemu "$smp" "$accel"
  selftest_validate_autorun "$serial" "$artifact_dir/cl13fix8-harness-${name}"
  for ((i=1; i<=100; i++)); do
    framed_send_complete "inputtest marker fix8h${i}" \
      "[INPUTTEST][MARKER] name=fix8h${i}" 90 sync
  done
  for ((i=1; i<=20; i++)); do
    framed_send_complete "taskmantest setup 1" \
      "[TASKMANTEST][SETUP] PASS count=1" 120 stress
    framed_send_complete "taskmantest cleanup" \
      "[TASKMANTEST][CLEANUP] PASS removed=1" 120 stress
  done
  for ((i=1; i<=20; i++)); do
    framed_synctest_async SEM "synctest sem 100" \
      "[SYNC][SEM] START" "[SYNC][SEM] PASS" 120 stress
  done
  framed_send_complete "tasktest transport-status" "[HARNESS][STATUS]" 120 stress
  status_line=$(framed_prefix_record_text "$serial" '[HARNESS][STATUS]' 1 | tail -1)
  [[ $status_line =~ accepted=([0-9]+)[[:space:]]completed=([0-9]+) ]]
  [[ ${BASH_REMATCH[1]} == "${BASH_REMATCH[2]}" ]]
  cp "$serial" "$log"
  records=$(assert_harness_lines "$log")
  assert_clean_log "$log"
  stop_qemu
  printf 'harness-record-focused\t%s\t%s\t%s\tPASS\thar_records=%s\tboundary=0\n' \
    "$name" "$smp" "$accel" "$records" >>"$artifact_dir/cl13-ledger.tsv"
}

group(){
  local name=$1 smp=$2 accel=$3
  [[ $accel != kvm || ( -r /dev/kvm && -w /dev/kvm ) ]] || return 3
  prepare_image; harness_lock; stress_boot "$name" "$smp" "$accel"; harness_unlock
}

case ${1:-} in
  host-unit) host_unit;;
  smp1-tcg) group smp1-tcg 1 tcg;;
  smp4-tcg) group smp4-tcg 4 tcg;;
  smp4-kvm) group smp4-kvm 4 kvm;;
  smp8-tcg) group smp8-tcg 8 tcg;;
  smp8-kvm) group smp8-kvm 8 kvm;;
  all)
    host_unit
    group smp1-tcg 1 tcg; group smp4-tcg 4 tcg; group smp4-kvm 4 kvm
    group smp8-tcg 8 tcg; group smp8-kvm 8 kvm
    echo '[CL13][HARNESS_RECORD_CLASSIFICATION] MULTI_CALL_HARNESS_INTERLEAVING_CONFIRMED'
    ;;
  *) usage;;
esac
