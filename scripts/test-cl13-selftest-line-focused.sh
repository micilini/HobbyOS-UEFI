#!/usr/bin/env bash
set -Eeuo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

artifact_dir=artifacts/build
ledger=$artifact_dir/cl13-ledger.tsv
serial=.qemu/qemu-serial.log
hmp=(python3 scripts/qemu_hmp.py --socket .qemu/hmp.sock)
harness_start_line=0
harness_command=selftest-line-focused
current_scenario=entry
current_log=
image_ready=0
protected_before=$artifact_dir/cl13fix7-protected-runtime-before.sha256
protected_after=$artifact_dir/cl13fix7-protected-runtime-after-focused.sha256
mkdir -p "$artifact_dir"
source scripts/harness-common.sh
source scripts/harness-runtime-ready.sh
source scripts/harness-framed.sh
source scripts/harness-selftest.sh

usage(){
  echo "Usage: $0 static|host-unit|up-tcg|smp4-tcg|smp4-kvm|smp8-tcg|smp8-kvm|classification|all" >&2
  exit 2
}

now_ms(){ echo $(( $(date +%s%N) / 1000000 )); }

ensure_ledger(){
  [[ -f $ledger ]] ||
    printf 'stage\tscenario\tsmp\taccel\tvariant\tcommand\tstatus\tduration_ms\tartifact\tsha256\n' >"$ledger"
}

ledger_record(){
  local scenario=$1 smp=$2 accel=$3 command=$4 status=$5 duration=$6 artifact=$7
  local hash=-
  ensure_ledger
  [[ ! -f $artifact ]] || hash=$(sha256sum "$artifact" | awk '{print $1}')
  printf 'selftest-line-focused\t%s\t%s\t%s\tselftest\t%s\t%s\t%s\t%s\t%s\n' \
    "$scenario" "$smp" "$accel" "$command" "$status" "$duration" \
    "$artifact" "$hash" >>"$ledger"
}

protected_snapshot(){
  local output=$1
  { rg --files kernel bootloader shared; printf '%s\n' makefile; } |
    LC_ALL=C sort | rg -v '^kernel/src/core/selftest\.c$' |
    xargs sha256sum >"$output"
}

failure_trap(){
  local rc=$?
  set +e
  if [[ -n $current_log && -f $serial ]]; then
    cp "$serial" "${current_log%.log}-failed.log"
    ledger_record "$current_scenario" 0 unknown focused FAIL 0 \
      "${current_log%.log}-failed.log"
  fi
  stop_qemu
  harness_unlock 2>/dev/null || true
  exit "$rc"
}
trap failure_trap ERR
trap stop_qemu EXIT

prepare_image(){
  ((image_ready)) && return 0
  make kernel-check JOBS=2 SELFTEST=1 SELFTEST_AUTORUN=1 >/dev/null
  make image SELFTEST=1 SELFTEST_AUTORUN=1 >/dev/null
  image_ready=1
}

static_gate(){
  local begin end jobs
  local j2=/tmp/hobbyos-cl13fix7-selftest-j2.elf
  local jn=/tmp/hobbyos-cl13fix7-selftest-jN.elf
  current_scenario=static
  current_log=$artifact_dir/cl13fix7-static.log
  begin=$(now_ms)
  jobs=$(nproc)
  {
    make stack-check JOBS=2
    make kernel-check JOBS=2 SELFTEST=1 SELFTEST_AUTORUN=1
    cp kernel.elf "$j2"
    cp artifacts/build/kernel-check-j2.log "$artifact_dir/cl13fix7-build-selftest-j2.log"
    make kernel-check JOBS="$jobs" SELFTEST=1 SELFTEST_AUTORUN=1
    cp kernel.elf "$jn"
    cp "artifacts/build/kernel-check-j${jobs}.log" \
      "$artifact_dir/cl13fix7-build-selftest-jN.log"
    cmp -s "$j2" "$jn"
    make image SELFTEST=1 SELFTEST_AUTORUN=1
    test -z "$(nm -u kernel.elf)"
    python3 -m py_compile scripts/parse-selftest-log.py
    bash -n scripts/harness-selftest.sh
    bash -n scripts/test-cl13-selftest-line-focused.sh
    rg -n '"\\n\[SELFTEST\]\[' kernel/src/core/selftest.c
    rg -n 'AUTORUN\] BEGIN|AUTORUN\] PASS|AUTORUN\] FAIL|EMIT_ERROR' \
      kernel/src/core/selftest.c
    rg -n 'line_boundary_violations|prefixed SELFTEST record' \
      scripts/parse-selftest-log.py scripts/harness-selftest.sh
    ! rg -n 'serial_write_all\("\\n"\)' kernel/src/core/selftest.c
    ! grep -E 'kernel/src/core/selftest\.c:.*warning:' \
      "$artifact_dir/cl13fix7-build-selftest-j2.log" \
      "$artifact_dir/cl13fix7-build-selftest-jN.log"
  } >"$current_log" 2>&1
  image_ready=1
  end=$(now_ms)
  ledger_record static 0 host 'line-fence+j2+jN+stack+image+nm' PASS \
    "$((end-begin))" "$current_log"
  current_log=
}

parser_case(){
  local id=$1 input=$2
  shift 2
  python3 scripts/parse-selftest-log.py --log "$input" \
    --json "$artifact_dir/cl13fix7-host-${id}.json" \
    --markdown "$artifact_dir/cl13fix7-host-${id}.md" "$@" \
    >"$artifact_dir/cl13fix7-host-${id}.out" 2>&1
}

write_prefix_fixture(){
  local output=$1
  printf '%s\n' \
    '[SELFTEST][AUTORUN] BEGIN' \
    '[XHCI] partial [SELFTEST][PASS] case.one severity=P0' \
    '[SELFTEST][SUMMARY] pass=1 fail=0 skip=0 p0_fail=0 p1_fail=0 p2_fail=0' \
    '[SELFTEST][AUTORUN] PASS' >"$output"
}

write_fence_fixture(){
  local output=$1
  printf '%s\n' \
    '[SELFTEST][AUTORUN] BEGIN' \
    '[XHCI] partial' \
    '[SELFTEST][PASS] case.one severity=P0' \
    'tail-from-xhci' \
    '[SELFTEST][SUMMARY] pass=1 fail=0 skip=0 p0_fail=0 p1_fail=0 p2_fail=0' \
    '[SELFTEST][AUTORUN] PASS' >"$output"
}

write_fix6_fixture(){
  local output=$1 fenced=$2
  local -a prefixes=(
    '[XHCI] Disabling slot 1... '
    '(Hardware disable OK) '
    '[XHCI-DBG] CMD Ring: enq_idx=5'
    '[XHCI-DBG] EVT Ring: deq_idx=13'
    '[XHCI-DBG] STS=0x'
  )
  local -a names=(case.one case.two case.three case.four case.five)
  printf '%s\n' '[SELFTEST][AUTORUN] BEGIN' >"$output"
  for index in 0 1 2 3 4; do
    if ((fenced)); then
      printf '%s\n%s\n' "${prefixes[index]}" \
        "[SELFTEST][PASS] ${names[index]} severity=P0" >>"$output"
    else
      printf '%s%s\n' "${prefixes[index]}" \
        "[SELFTEST][PASS] ${names[index]} severity=P0" >>"$output"
    fi
  done
  printf '%s\n' \
    '[SELFTEST][SUMMARY] pass=5 fail=0 skip=0 p0_fail=0 p1_fail=0 p2_fail=0' \
    '[SELFTEST][AUTORUN] PASS' >>"$output"
}

host_unit_gate(){
  local begin end prefix=$artifact_dir/cl13fix7-host-prefix.log
  local fence=$artifact_dir/cl13fix7-host-fence.log
  local old=$artifact_dir/cl13fix7-host-fix6-unfenced.log
  local fixed=$artifact_dir/cl13fix7-host-fix6-fenced.log
  current_scenario=host-unit
  current_log=$artifact_dir/cl13fix7-host-unit.log
  begin=$(now_ms)
  write_prefix_fixture "$prefix"
  write_fence_fixture "$fence"
  write_fix6_fixture "$old" 0
  write_fix6_fixture "$fixed" 1

  if parser_case prefix "$prefix" --require-clean-framing; then return 1; fi
  grep -Fq 'prefixed SELFTEST record' "$artifact_dir/cl13fix7-host-prefix.out"
  ledger_record host-prefix-unit 0 host prefix-collision PASS 0 "$prefix"

  parser_case fence "$fence" --require-clean-framing --require-case case.one
  selftest_validate_json "$artifact_dir/cl13fix7-host-fence.json" case.one >/dev/null
  ledger_record host-fence-unit 0 host line-fence PASS 0 "$fence"

  if parser_case fix6-unfenced "$old" --require-clean-framing; then return 1; fi
  grep -Fq 'prefixed SELFTEST record' "$artifact_dir/cl13fix7-host-fix6-unfenced.out"
  parser_case fix6-fenced "$fixed" --require-clean-framing
  python3 - "$artifact_dir/cl13fix7-host-fix6-fenced.json" <<'PY'
import json, pathlib, sys
data = json.loads(pathlib.Path(sys.argv[1]).read_text())
assert data["case_count"] == 5
assert data["framing_repairs"] == 0
assert data["line_boundary_violations"] == 0
PY
  ledger_record fix6-reproduction 0 host five-prefix-patterns PASS 0 "$fixed"
  {
    echo '[CL13][SELFTEST_LINE_HOST_UNIT] PASS'
    echo 'prefix=REJECTED reason=prefixed_SELFTEST_record'
    echo 'fence=PASS repairs=0 line_boundary_violations=0'
    echo 'fix6-unfenced=REJECTED fix6-fenced=PASS cases=5'
  } >"$current_log"
  end=$(now_ms)
  ledger_record host-unit 0 host prefix+fence+fix6 PASS "$((end-begin))" "$current_log"
  current_log=
}

direct_boundary_check(){
  python3 - "$1" <<'PY'
from pathlib import Path
import sys

for number, line in enumerate(Path(sys.argv[1]).read_text(errors="replace").splitlines(), 1):
    offset = line.find("[SELFTEST][")
    if offset > 0:
        raise SystemExit(f"prefixed SELFTEST record line={number} offset={offset}")
PY
}

transport_balance(){
  local status
  framed_send_complete "tasktest transport-status" "[HARNESS][STATUS]" 120 stress
  status=$(grep -F '[HARNESS][STATUS]' "$serial" | tail -1)
  [[ $status =~ accepted=([0-9]+)[[:space:]]completed=([0-9]+) ]]
  [[ ${BASH_REMATCH[1]} == "${BASH_REMATCH[2]}" ]]
}

focused_boot(){
  local scenario=$1 smp=$2 accel=$3 run=$4 begin end prefix log noise
  current_scenario="${scenario}-run${run}"
  prefix=$artifact_dir/cl13fix7-${current_scenario}
  log=${prefix}.log
  current_log=$log
  begin=$(now_ms)
  harness_lock
  start_qemu "$smp" "$accel"
  grep -Fq '[BOOT][SHELL_READY] PASS' "$serial"
  selftest_validate_autorun "$serial" "$prefix"
  direct_boundary_check "$serial"
  ! grep -Fq '[SELFTEST][EMIT_ERROR]' "$SELFTEST_GATE_LOG"
  noise=0
  if grep -Eq '^\[(XHCI|HP)\]' "$SELFTEST_GATE_LOG"; then noise=1; fi
  framed_send_complete "taskdiag check" "[TASKDIAG][CHECK] PASS" 120 stress
  transport_balance
  cp "$serial" "$log"
  assert_clean_log "$log"
  stop_qemu
  harness_unlock
  end=$(now_ms)
  {
    echo "[CL13][SELFTEST_LINE_BOOT] PASS scenario=$scenario run=$run smp=$smp accel=$accel"
    echo "pass=$SELFTEST_GATE_PASS fail=0 skip=0 repairs=$SELFTEST_GATE_REPAIRS line_boundary_violations=$SELFTEST_GATE_BOUNDARY_VIOLATIONS case_count=$SELFTEST_GATE_CASE_COUNT required_cases=$SELFTEST_GATE_REQUIRED noise_present=$noise"
  } >>"$log"
  ledger_record "$current_scenario" "$smp" "$accel" \
    "pass=$SELFTEST_GATE_PASS fail=0 skip=0 repairs=0 boundary=0 case_count=$SELFTEST_GATE_CASE_COUNT required=$SELFTEST_GATE_REQUIRED noise=$noise" \
    PASS "$((end-begin))" "$log"
  SELFTEST_BOOT_NOISE=$noise
  current_log=
}

run_group(){
  local scenario=$1 smp=$2 accel=$3 runs=$4 run
  prepare_image
  for ((run=1; run<=runs; run++)); do
    focused_boot "$scenario" "$smp" "$accel" "$run"
  done
}

up_tcg_gate(){ run_group up-tcg 1 tcg 2; }
smp4_tcg_gate(){ run_group smp4-tcg 4 tcg 3; }
smp4_kvm_gate(){ run_group smp4-kvm 4 kvm 3; }

smp8_tcg_gate(){
  local run noise_boots=0
  prepare_image
  for ((run=1; run<=5; run++)); do
    focused_boot smp8-tcg 8 tcg "$run"
    ((noise_boots += SELFTEST_BOOT_NOISE)) || true
  done
  ((noise_boots >= 2))
  printf '[CL13][SELFTEST_LINE_HOTPLUG] PASS smp=8 accel=tcg noisy_boots=%s required=2\n' \
    "$noise_boots" | tee "$artifact_dir/cl13fix7-smp8-tcg-noise.log"
}

smp8_kvm_gate(){
  local run noise_boots=0
  prepare_image
  for ((run=1; run<=5; run++)); do
    focused_boot smp8-kvm 8 kvm "$run"
    ((noise_boots += SELFTEST_BOOT_NOISE)) || true
  done
  for ((run=6; run<=7 && noise_boots<3; run++)); do
    focused_boot smp8-kvm-extra 8 kvm "$run"
    ((noise_boots += SELFTEST_BOOT_NOISE)) || true
  done
  ((noise_boots >= 3))
  printf '[CL13][SELFTEST_LINE_HOTPLUG] PASS smp=8 accel=kvm noisy_boots=%s required=3\n' \
    "$noise_boots" | tee "$artifact_dir/cl13fix7-smp8-kvm-noise.log"
}

classification_gate(){
  local old=$artifact_dir/cl13fix6-smp8-tcg-run2-failed.log
  local prefix=$artifact_dir/cl13fix7-original-fix6
  local out=$artifact_dir/cl13fix7-selftest-line-classification.log
  [[ -f $old ]]
  selftest_extract_autorun "$old" "${prefix}-selftest.log"
  if python3 scripts/parse-selftest-log.py \
      --log "${prefix}-selftest.log" --json "${prefix}-selftest.json" \
      --markdown "${prefix}-selftest.md" --require-clean-framing \
      >"${prefix}-parser.out" 2>&1; then
    echo 'FIX6 prefix-collision log unexpectedly passed' >&2
    return 1
  fi
  grep -Fq 'prefixed SELFTEST record' "${prefix}-parser.out"
  protected_snapshot "$protected_after"
  cmp -s "$protected_before" "$protected_after"
  {
    echo '[CL13][SELFTEST_LINE_CLASSIFICATION]'
    echo 'OPEN_FOREIGN_LINE_PREFIX_CONFIRMED'
  } >"$out"
  ledger_record classification 0 host old-prefix-new-zero PASS 0 "$out"
  ledger_record protected-runtime 0 host before=after-focused PASS 0 "$protected_after"
}

all_gate(){
  static_gate
  host_unit_gate
  up_tcg_gate
  smp4_tcg_gate
  smp4_kvm_gate
  smp8_tcg_gate
  smp8_kvm_gate
  classification_gate
}

case ${1:-} in
  static) static_gate;;
  host-unit) host_unit_gate;;
  up-tcg) up_tcg_gate;;
  smp4-tcg) smp4_tcg_gate;;
  smp4-kvm) smp4_kvm_gate;;
  smp8-tcg) smp8_tcg_gate;;
  smp8-kvm) smp8_kvm_gate;;
  classification) classification_gate;;
  all) all_gate;;
  *) usage;;
esac

stop_qemu
trap - ERR EXIT
