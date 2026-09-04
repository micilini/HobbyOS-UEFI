#!/usr/bin/env bash
set -Eeuo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

artifact_dir=artifacts/build
ledger=$artifact_dir/cl13-ledger.tsv
serial=.qemu/qemu-serial.log
hmp=(python3 scripts/qemu_hmp.py --socket .qemu/hmp.sock)
harness_start_line=0
harness_command=selftest-record-focused
current_scenario=entry
current_log=
image_ready=0
protected_before=$artifact_dir/cl13fix6-protected-runtime-before.sha256
protected_after=$artifact_dir/cl13fix6-protected-runtime-after-focused.sha256
mkdir -p "$artifact_dir"
source scripts/harness-common.sh
source scripts/harness-runtime-ready.sh
source scripts/harness-framed.sh
source scripts/harness-selftest.sh

usage(){
  echo "Usage: $0 static|host-unit|up-tcg|smp4-tcg|smp4-kvm|smp8-tcg|smp8-kvm|all" >&2
  exit 2
}

now_ms(){ echo $(( $(date +%s%N) / 1000000 )); }

ensure_ledger(){
  if [[ ! -f $ledger ]]; then
    printf 'stage\tscenario\tsmp\taccel\tvariant\tcommand\tstatus\tduration_ms\tartifact\tsha256\n' >"$ledger"
  fi
}

ledger_record(){
  local scenario=$1 smp=$2 accel=$3 command=$4 status=$5 duration=$6 artifact=$7
  local hash=-
  ensure_ledger
  [[ ! -f $artifact ]] || hash=$(sha256sum "$artifact" | awk '{print $1}')
  printf 'selftest-record-focused\t%s\t%s\t%s\tselftest\t%s\t%s\t%s\t%s\t%s\n' \
    "$scenario" "$smp" "$accel" "$command" "$status" "$duration" \
    "$artifact" "$hash" >>"$ledger"
}

protected_snapshot(){
  local output=$1
  { rg --files kernel bootloader shared; printf '%s\n' makefile; } |
    LC_ALL=C sort |
    rg -v '^kernel/src/core/selftest\.c$' |
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
  if ((image_ready)); then return; fi
  make kernel-check JOBS=2 SELFTEST=1 SELFTEST_AUTORUN=1 >/dev/null
  make image SELFTEST=1 SELFTEST_AUTORUN=1 >/dev/null
  image_ready=1
}

static_gate(){
  local begin end jobs
  local j2=/tmp/hobbyos-cl13fix6-selftest-j2.elf
  local jn=/tmp/hobbyos-cl13fix6-selftest-jN.elf
  current_scenario=static
  current_log=$artifact_dir/cl13fix6-static.log
  begin=$(now_ms)
  jobs=$(nproc)
  {
    make stack-check JOBS=2
    make kernel-check JOBS=2 SELFTEST=1 SELFTEST_AUTORUN=1
    cp kernel.elf "$j2"
    cp artifacts/build/kernel-check-j2.log \
      "$artifact_dir/cl13fix6-build-selftest-j2.log"
    make kernel-check JOBS="$jobs" SELFTEST=1 SELFTEST_AUTORUN=1
    cp kernel.elf "$jn"
    cp "artifacts/build/kernel-check-j${jobs}.log" \
      "$artifact_dir/cl13fix6-build-selftest-jN.log"
    cmp -s "$j2" "$jn"
    make image SELFTEST=1 SELFTEST_AUTORUN=1
    test -z "$(nm -u kernel.elf)"
    python3 -m py_compile scripts/parse-selftest-log.py
    bash -n scripts/harness-selftest.sh
    bash -n scripts/test-cl13-selftest-record-focused.sh
    ! rg -n 'scheduler\.async\.completed_before_marker_contract severity=P0' scripts
    ! rg -n 'ui\.taskman\.fixture_capacity_257 severity=P0' scripts
    rg -n 'SELFTEST_RECORD_MAX|selftest_record_emit|registry\.selftest_records_fit' \
      kernel/src/core/selftest.c
    rg -n 'require-clean-framing|require-case' \
      scripts/parse-selftest-log.py scripts/harness-selftest.sh
    test "$(sed -n '/^void selftest_emit_result/,/^}/p' \
      kernel/src/core/selftest.c | grep -Fc selftest_record_emit)" -eq 1
    test "$(sed -n '/^void selftest_emit_summary/,/^}/p' \
      kernel/src/core/selftest.c | grep -Fc selftest_record_emit)" -eq 1
    ! sed -n '/^void selftest_emit_result/,/^}/p' \
      kernel/src/core/selftest.c | grep -Eq 'serial_(write|putc)'
    ! sed -n '/^void selftest_emit_summary/,/^}/p' \
      kernel/src/core/selftest.c | grep -Eq 'serial_(write|putc)'
    ! grep -E 'kernel/src/core/selftest\.c:.*warning:' \
      "$artifact_dir/cl13fix6-build-selftest-j2.log" \
      "$artifact_dir/cl13fix6-build-selftest-jN.log"
  } >"$current_log" 2>&1
  image_ready=1
  end=$(now_ms)
  ledger_record static 0 host 'builder+j2+jN+stack+image+nm' PASS \
    "$((end-begin))" "$current_log"
  current_log=
}

parser_case(){
  local id=$1 log=$2
  shift 2
  python3 scripts/parse-selftest-log.py --log "$log" \
    --json "$artifact_dir/cl13fix6-host-${id}.json" \
    --markdown "$artifact_dir/cl13fix6-host-${id}.md" "$@" \
    >"$artifact_dir/cl13fix6-host-${id}.out" 2>&1
}

host_unit_gate(){
  local dir=$artifact_dir/cl13fix6-host begin end
  local clean=$dir-clean.log foreign=$dir-foreign.log split=$dir-split.log
  local mismatch=$dir-mismatch.log duplicate=$dir-duplicate.log
  local missing=$dir-missing.log repair=$dir-repair.log
  current_scenario=host-unit
  current_log=$artifact_dir/cl13fix6-parser-host-unit.log
  begin=$(now_ms)

  printf '%s\n' \
    '[SELFTEST][AUTORUN] BEGIN' \
    '[SELFTEST][PASS] case.one severity=P0' \
    '[SELFTEST][PASS] case.two severity=P1' \
    '[SELFTEST][SUMMARY] pass=2 fail=0 skip=0 p0_fail=0 p1_fail=0 p2_fail=0' \
    '[SELFTEST][AUTORUN] PASS' >"$clean"
  printf '%s\n' \
    '[SELFTEST][AUTORUN] BEGIN' \
    '[SELFTEST][PASS] case.one severity=P0' \
    '[XHCI] independent foreign line' \
    '[SELFTEST][PASS] case.two severity=P1' \
    '[SELFTEST][SUMMARY] pass=2 fail=0 skip=0 p0_fail=0 p1_fail=0 p2_fail=0' \
    '[SELFTEST][AUTORUN] PASS' >"$foreign"
  printf '%s\n' \
    '[SELFTEST][AUTORUN] BEGIN' \
    '[SELFTEST][PASS] case.one severity=[XHCI] interleaved' \
    'P0' \
    '[SELFTEST][SUMMARY] pass=1 fail=0 skip=0 p0_fail=0 p1_fail=0 p2_fail=0' \
    '[SELFTEST][AUTORUN] PASS' >"$split"
  printf '%s\n' \
    '[SELFTEST][AUTORUN] BEGIN' \
    '[SELFTEST][PASS] case.one severity=P0' \
    '[SELFTEST][SUMMARY] pass=2 fail=0 skip=0 p0_fail=0 p1_fail=0 p2_fail=0' \
    '[SELFTEST][AUTORUN] PASS' >"$mismatch"
  printf '%s\n' \
    '[SELFTEST][AUTORUN] BEGIN' \
    '[SELFTEST][PASS] case.one severity=P0' \
    '[SELFTEST][PASS] case.one severity=P0' \
    '[SELFTEST][SUMMARY] pass=2 fail=0 skip=0 p0_fail=0 p1_fail=0 p2_fail=0' \
    '[SELFTEST][AUTORUN] PASS' >"$duplicate"
  cp "$clean" "$missing"
  printf '%s\n' \
    '[SELFTEST][AUTORUN] BEGIN' \
    '[SELFTEST][PASS] case.one[XHCI] complete foreign record' \
    ' severity=P0' \
    '[SELFTEST][SUMMARY] pass=1 fail=0 skip=0 p0_fail=0 p1_fail=0 p2_fail=0' \
    '[SELFTEST][AUTORUN] PASS' >"$repair"

  parser_case clean "$clean" --require-clean-framing --require-case case.one
  parser_case foreign "$foreign" --require-clean-framing --require-case case.two
  if parser_case split "$split" --require-clean-framing; then return 1; fi
  if parser_case mismatch "$mismatch" --require-clean-framing; then return 1; fi
  if parser_case duplicate "$duplicate" --require-clean-framing; then return 1; fi
  if parser_case missing "$missing" --require-clean-framing \
      --require-case case.absent; then return 1; fi
  parser_case repair-compatible "$repair" --require-case case.one
  python3 - "$artifact_dir/cl13fix6-host-repair-compatible.json" <<'PY'
import json, pathlib, sys
data = json.loads(pathlib.Path(sys.argv[1]).read_text())
assert data["framing_repairs"] == 1
PY
  if parser_case repair-clean "$repair" --require-clean-framing; then return 1; fi
  {
    echo '[CL13][SELFTEST_PARSER_HOST_UNIT] PASS'
    echo 'clean=PASS foreign-between-records=PASS split=REJECTED'
    echo 'summary-mismatch=REJECTED duplicate=REJECTED missing-required=REJECTED'
    echo 'historical-repair=PASS repairs=1 clean-repair=REJECTED'
  } >"$current_log"
  end=$(now_ms)
  ledger_record host-unit 0 host strict-parser PASS "$((end-begin))" "$current_log"
  current_log=
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
  prefix=$artifact_dir/cl13fix6-${current_scenario}
  log=${prefix}.log
  current_log=$log
  begin=$(now_ms)
  harness_lock
  start_qemu "$smp" "$accel"
  grep -Fq '[BOOT][SHELL_READY] PASS' "$serial"
  selftest_validate_autorun "$serial" "$prefix"
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
    echo "[CL13][SELFTEST_BOOT] PASS scenario=$scenario run=$run smp=$smp accel=$accel"
    echo "pass=$SELFTEST_GATE_PASS fail=0 skip=0 repairs=$SELFTEST_GATE_REPAIRS case_count=$SELFTEST_GATE_CASE_COUNT required_cases=$SELFTEST_GATE_REQUIRED noise_present=$noise"
  } >>"$log"
  ledger_record "$current_scenario" "$smp" "$accel" \
    "pass=$SELFTEST_GATE_PASS fail=0 skip=0 repairs=0 case_count=$SELFTEST_GATE_CASE_COUNT required=$SELFTEST_GATE_REQUIRED noise=$noise" \
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
smp8_tcg_gate(){ run_group smp8-tcg 8 tcg 3; }

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
  printf '[CL13][SELFTEST_HOTPLUG] PASS noisy_boots=%s required=3\n' \
    "$noise_boots" | tee "$artifact_dir/cl13fix6-hotplug-noise.log"
}

classification_gate(){
  local old=$artifact_dir/cl13fix5-yield-smp8-kvm-run2-failed.log
  local old_prefix=$artifact_dir/cl13fix6-original-interleaved
  local out=$artifact_dir/cl13fix6-selftest-record-classification.log
  [[ -f $old ]]
  selftest_extract_autorun "$old" "${old_prefix}-selftest.log"
  if python3 scripts/parse-selftest-log.py \
      --log "${old_prefix}-selftest.log" \
      --json "${old_prefix}-selftest.json" \
      --markdown "${old_prefix}-selftest.md" \
      --require-clean-framing >"${old_prefix}-parser.out" 2>&1; then
    echo 'old interleaved log unexpectedly parsed as clean' >&2
    return 1
  fi
  protected_snapshot "$protected_after"
  cmp -s "$protected_before" "$protected_after"
  {
    echo '[CL13][SELFTEST_RECORD_CLASSIFICATION]'
    echo 'MULTI_CALL_SERIAL_INTERLEAVING_CONFIRMED'
  } >"$out"
  ledger_record classification 0 host old-fails-new-clean PASS 0 "$out"
  ledger_record protected-runtime 0 host before=after-focused PASS 0 \
    "$protected_after"
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
  all) all_gate;;
  *) usage;;
esac

stop_qemu
trap - ERR EXIT
