#!/usr/bin/env bash
set -Eeuo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

artifact_dir=artifacts/build
serial=.qemu/qemu-serial.log
hmp=(python3 scripts/qemu_hmp.py --socket .qemu/hmp.sock)
harness_start_line=0
harness_command=fixture257-focused
current_scenario=entry
current_log=
protected_before=$artifact_dir/cl13fix4-protected-runtime-before.sha256
protected_after=$artifact_dir/cl13fix4-protected-runtime-after-focused.sha256
mkdir -p "$artifact_dir"
source scripts/harness-common.sh
source scripts/harness-runtime-ready.sh
source scripts/harness-framed.sh
source scripts/harness-selftest.sh

usage(){
  echo "Usage: $0 static|range|busy|exactly-once|growth257|smp4-tcg|smp4-kvm|all" >&2
  exit 2
}

now_ms(){ echo $(( $(date +%s%N) / 1000000 )); }

ledger_record(){
  local scenario=$1 smp=$2 accel=$3 command=$4 status=$5 duration=$6 artifact=$7
  local ledger=$artifact_dir/cl13-ledger.tsv hash=-
  [[ -f $ledger ]] ||
    printf 'stage\tscenario\tsmp\taccel\tbuild_variant\tcommand\tstatus\tduration_ms\tartifact\thash\n' >"$ledger"
  [[ -f $artifact ]] && hash=$(sha256sum "$artifact" | awk '{print $1}')
  printf 'fixture257-focused\t%s\t%s\t%s\tselftest\t%s\t%s\t%s\t%s\t%s\n' \
    "$scenario" "$smp" "$accel" "$command" "$status" "$duration" \
    "$artifact" "$hash" >>"$ledger"
}

ledger_latest_pass(){
  local scenario=$1 status artifact expected_hash actual_hash
  read -r status artifact expected_hash < <(awk -F '\t' -v wanted="$scenario" '
    $1 == "fixture257-focused" && $2 == wanted {
      status=$7; artifact=$9; hash=$10
    }
    END { print status, artifact, hash }
  ' "$artifact_dir/cl13-ledger.tsv")
  [[ $status == PASS && -f $artifact && $expected_hash != - ]] || return 1
  actual_hash=$(sha256sum "$artifact" | awk '{print $1}')
  [[ $actual_hash == "$expected_hash" ]]
}

protected_snapshot(){
  local output=$1
  { rg --files kernel bootloader shared; printf '%s\n' makefile; } |
    LC_ALL=C sort |
    rg -v '^(kernel/src/shell/commands/cmd_taskmantest\.(c|h)|kernel/src/core/selftest\.c)$' |
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
trap 'stop_qemu' EXIT

prepare_selftest_image(){
  make kernel-check JOBS=2 SELFTEST=1 SELFTEST_AUTORUN=1 >/dev/null
  make image SELFTEST=1 SELFTEST_AUTORUN=1 >/dev/null
}

boot_begin(){
  local smp=$1 accel=$2
  harness_lock
  start_qemu "$smp" "$accel"
  grep -Fq '[BOOT][SHELL_READY] PASS' "$serial"
  selftest_validate_autorun "$serial" \
    "$artifact_dir/cl13fix4-${current_scenario}"
}

boot_end(){
  stop_qemu
  harness_unlock
}

save_boot(){
  local output=$1
  assert_clean_log "$serial"
  cp "$serial" "$output"
  sha256sum "$output" >"${output%.log}.sha256"
}

transport_balance(){
  local minimum_replays=${1:-0} line
  framed_send_complete "tasktest transport-status" "[HARNESS][STATUS]" 90 stress
  line=$(grep -F '[HARNESS][STATUS]' "$serial" | tail -1)
  [[ $line =~ accepted=([0-9]+)[[:space:]]completed=([0-9]+) ]]
  [[ ${BASH_REMATCH[1]} == "${BASH_REMATCH[2]}" ]]
  [[ $line =~ replays=([0-9]+) ]]
  ((BASH_REMATCH[1] >= minimum_replays))
}

taskdiag_zero(){
  local line
  framed_send_complete "taskdiag check" "[TASKDIAG][CHECK] PASS" 120 stress
  line=$(grep -F '[TASKDIAG][CHECK] PASS' "$serial" | tail -1)
  [[ $line =~ free_inflight=0 ]]
  [[ $line =~ structural_faults=0 ]]
}

fixture_status(){
  local active=$1 present=$2 gate=${3:-0} cleanup=${4:-0}
  framed_send_complete "taskmantest fixture-status" \
    "[TASKMANTEST][FIXTURE_STATUS] PASS max=257 active=$active present=$present gate_release=$gate cleanup=$cleanup" \
    120 stress
}

capacity_body(){
  local line
  framed_send_complete "taskmantest fixture-capacity" \
    "[TASKMANTEST][FIXTURE_CAPACITY] PASS" 90 stress
  line=$(grep -F '[TASKMANTEST][FIXTURE_CAPACITY] PASS' "$serial" | tail -1)
  [[ $line =~ max=257 ]]
  [[ $line =~ handle_size=16 ]]
  [[ $line =~ handle_bytes=4112 ]]
  [[ $line =~ stack_bytes_est=4210688 ]]
  [[ $line =~ taskman_initial=128 ]]
  [[ $line =~ taskman_cap=4096 ]]
  fixture_status 0 0 0 0
  taskdiag_zero
}

invalid_setup(){
  local value=$1 reason=$2
  framed_send_complete "taskmantest setup $value" \
    "[TASKMANTEST][SETUP] FAIL reason=$reason" 120 stress 1
}

range_body(){
  capacity_body
  invalid_setup 0 range
  invalid_setup 258 range
  invalid_setup abc parse
  invalid_setup 4294967296 parse
  fixture_status 0 0 0 0
  taskdiag_zero
}

busy_body(){
  local setup_before busy_before
  setup_before=$(count '[TASKMANTEST][SETUP] PASS count=20')
  busy_before=$(count '[TASKMANTEST][SETUP] FAIL reason=busy')
  framed_send_complete "taskmantest setup 20" \
    "[TASKMANTEST][SETUP] PASS count=20" 240 stress
  framed_send_complete "taskmantest setup 1" \
    "[TASKMANTEST][SETUP] FAIL reason=busy requested=1 active=20 max=257" \
    120 stress 1
  (( $(count '[TASKMANTEST][SETUP] PASS count=20') == setup_before + 1 ))
  (( $(count '[TASKMANTEST][SETUP] FAIL reason=busy') == busy_before + 1 ))
  fixture_status 20 20 0 0
  framed_send_complete "taskmantest cleanup" \
    "[TASKMANTEST][CLEANUP] PASS removed=20 handles_gone=20 free_inflight=0 origin=manual" \
    300 stress
  framed_send_complete "taskmantest cleanup" \
    "[TASKMANTEST][CLEANUP] PASS removed=0 handles_gone=0 free_inflight=0 origin=manual" \
    120 stress
  fixture_status 0 0 0 0
  taskdiag_zero
}

wait_new_fixed(){
  local pattern=$1 before=$2 timeout=${3:-60} deadline
  deadline=$((SECONDS + timeout))
  while ((SECONDS <= deadline)); do
    (( $(count "$pattern") > before )) && return 0
    grep -Eq 'PANIC|#PF|#GP|FATAL|STRUCTURAL_FAULT|FINISH_FAULT' "$serial" && return 1
    framed_qemu_alive || return 1
    sleep 1
  done
  return 1
}

inject_bad_crc(){
  local payload=$1 sequence frame pattern before
  framed_boot_sync
  sequence=$((HARNESS_FRAME_SEQUENCE + 1))
  frame="tasktest exec $sequence 00000000 $payload"
  pattern="[HARNESS][FRAME] REJECT seq=$sequence reason=crc"
  before=$(count "$pattern")
  hmp_text "$frame" stress --enter >/dev/null
  wait_new_fixed "$pattern" "$before" 60
}

exactly_once_body(){
  local setup_before cleanup_before reject_before
  fixture_status 0 0 0 0
  setup_before=$(count '[TASKMANTEST][SETUP] PASS count=257')
  cleanup_before=$(count '[TASKMANTEST][CLEANUP] PASS removed=257')
  reject_before=$(count '[HARNESS][FRAME] REJECT')
  inject_bad_crc "taskmantest setup 257"
  (( $(count '[HARNESS][FRAME] REJECT') == reject_before + 1 ))
  (( $(count '[TASKMANTEST][SETUP] PASS count=257') == setup_before ))
  fixture_status 0 0 0 0
  framed_send_complete "taskmantest setup 257" \
    "[TASKMANTEST][SETUP] PASS count=257" 600 stress
  framed_replay_last "taskmantest setup 257" 0 120 stress
  (( $(count '[TASKMANTEST][SETUP] PASS count=257') == setup_before + 1 ))
  fixture_status 257 257 0 0
  framed_send_complete "taskmantest cleanup" \
    "[TASKMANTEST][CLEANUP] PASS removed=257 handles_gone=257 free_inflight=0 origin=manual" \
    600 stress
  (( $(count '[TASKMANTEST][CLEANUP] PASS removed=257') == cleanup_before + 1 ))
  fixture_status 0 0 0 0
  taskdiag_zero
  transport_balance 1
}

anchored_taskman(){
  local refresh=$1 frames=$2
  framed_send_complete "taskmantest anchor-reset" \
    "[TASKMANTEST][ANCHOR_RESET] PASS" 90 stress
  framed_send_complete "taskmantest auto-exit-frames $frames" \
    "[TASKMANTEST][AUTO_EXIT_ARM] PASS frames=$frames" 90 stress
  framed_send_complete "taskman $refresh" \
    "[TASKMAN][AUTO_EXIT] PASS target=$frames full_frames=$frames fallback_frames=0" \
    300 stress
}

growth257_body(){
  local setup_line p1 p2 p3 stats heap cleanup_line
  framed_send_complete "taskmantest reset-controls" \
    "[TASKMANTEST][CONTROLS] RESET" 90 stress
  framed_send_complete "taskmantest stats-reset" \
    "[TASKMANTEST][STATS_RESET] PASS" 90 stress
  framed_send_complete "taskmantest heap-begin" \
    "[TASKMANTEST][HEAP_BEGIN] PASS" 120 stress
  framed_send_complete "taskmantest setup 257" \
    "[TASKMANTEST][SETUP] PASS count=257" 600 stress
  setup_line=$(grep -F '[TASKMANTEST][SETUP] PASS count=257' "$serial" | tail -1)
  [[ $setup_line =~ requested=257 ]]
  [[ $setup_line =~ created=257 ]]
  [[ $setup_line =~ present=257 ]]
  [[ $setup_line =~ total=([0-9]+) ]]
  ((BASH_REMATCH[1] >= 257))
  [[ $setup_line =~ max=257 ]]
  [[ $setup_line =~ stack_bytes_est=4210688 ]]
  framed_send_complete "taskdiag summary" "[TASKDIAG][SUMMARY] PASS" 120 stress
  framed_send_complete "ps 1 128" "[PS][SNAPSHOT]" 180 stress
  p1=$(grep -F '[PS][SNAPSHOT]' "$serial" | tail -1)
  [[ $p1 =~ page=1[[:space:]]pages=3[[:space:]]page_size=128[[:space:]]offset=0[[:space:]]written=128 ]]
  framed_send_complete "ps 2 128" "[PS][SNAPSHOT]" 180 stress
  p2=$(grep -F '[PS][SNAPSHOT]' "$serial" | tail -1)
  [[ $p2 =~ page=2[[:space:]]pages=3[[:space:]]page_size=128[[:space:]]offset=128[[:space:]]written=128 ]]
  framed_send_complete "ps 3 128" "[PS][SNAPSHOT]" 180 stress
  p3=$(grep -F '[PS][SNAPSHOT]' "$serial" | tail -1)
  [[ $p3 =~ page=3[[:space:]]pages=3[[:space:]]page_size=128[[:space:]]offset=256[[:space:]]written=([1-9][0-9]*) ]]
  anchored_taskman 50 3
  framed_send_complete "taskmantest stats" "[TASKMANTEST][STATS]" 120 stress
  stats=$(grep -F '[TASKMANTEST][STATS]' "$serial" | tail -1)
  [[ $stats =~ last_session_full_frames=3 ]]
  [[ $stats =~ last_session_fallback_frames=0 ]]
  [[ $stats =~ last_mode=(WIDE|COMPACT) ]]
  [[ $stats =~ pages=([0-9]+) ]]
  ((BASH_REMATCH[1] >= 2))
  [[ $stats =~ captured=([0-9]+) ]]
  ((BASH_REMATCH[1] >= 257))
  [[ $stats =~ truncated=0 ]]
  [[ $stats =~ render_failures=0 ]]
  [[ $stats =~ last_session_scroll_delta=0 ]]
  fixture_status 257 257 0 0
  taskdiag_zero
  framed_send_complete "taskmantest cleanup" \
    "[TASKMANTEST][CLEANUP] PASS removed=257 handles_gone=257 free_inflight=0 origin=manual" \
    600 stress
  cleanup_line=$(grep -F '[TASKMANTEST][CLEANUP] PASS removed=257' "$serial" | tail -1)
  [[ $cleanup_line =~ handles_gone=257 ]]
  [[ $cleanup_line =~ free_inflight=0 ]]
  framed_send_complete "taskmantest heap-end" \
    "[TASKMANTEST][HEAP] PASS" 300 stress
  heap=$(grep -F '[TASKMANTEST][HEAP] PASS' "$serial" | tail -1)
  [[ $heap =~ baseline_used=([0-9]+) ]]
  local baseline_used=${BASH_REMATCH[1]}
  [[ $heap =~ final_used=([0-9]+) ]]
  [[ ${BASH_REMATCH[1]} == "$baseline_used" ]]
  [[ $heap =~ baseline_blocks=([0-9]+) ]]
  local baseline_blocks=${BASH_REMATCH[1]}
  [[ $heap =~ final_blocks=([0-9]+) ]]
  [[ ${BASH_REMATCH[1]} == "$baseline_blocks" ]]
  [[ $heap =~ drift=0 ]]
  fixture_status 0 0 0 0
  taskdiag_zero
}

count_sequence(){
  local count refresh
  for count in "$@"; do
    framed_send_complete "taskmantest setup $count" \
      "[TASKMANTEST][SETUP] PASS count=$count" 600 stress
    fixture_status "$count" "$count" 0 0
    case "$count" in
      1|129|257) refresh=50;;
      20) refresh=1000;;
      *) refresh=2000;;
    esac
    anchored_taskman "$refresh" 1
    framed_send_complete "taskmantest cleanup" \
      "[TASKMANTEST][CLEANUP] PASS removed=$count handles_gone=$count free_inflight=0 origin=manual" \
      600 stress
    fixture_status 0 0 0 0
  done
  taskdiag_zero
}

twenty_sessions(){
  local session refresh
  framed_send_complete "taskmantest setup 20" \
    "[TASKMANTEST][SETUP] PASS count=20" 240 stress
  for ((session=1; session<=20; session++)); do
    case $((session % 3)) in
      0) refresh=50;;
      1) refresh=1000;;
      *) refresh=2000;;
    esac
    anchored_taskman "$refresh" 1
  done
  framed_send_complete "taskmantest cleanup" \
    "[TASKMANTEST][CLEANUP] PASS removed=20 handles_gone=20 free_inflight=0 origin=manual" \
    300 stress
  fixture_status 0 0 0 0
  taskdiag_zero
}

smp4_tcg_run1_body(){ range_body; busy_body; count_sequence 1 20 50 129 257; }
smp4_tcg_run2_body(){ exactly_once_body; twenty_sessions; }
smp4_tcg_run3_body(){ growth257_body; count_sequence 1 20 50 129 257; }
smp4_kvm_run1_body(){ exactly_once_body; count_sequence 20 129 257; }
smp4_kvm_run2_body(){ growth257_body; }

smp8_smoke_body(){
  framed_send_complete "taskmantest setup 257" \
    "[TASKMANTEST][SETUP] PASS count=257" 600 stress
  fixture_status 257 257 0 0
  anchored_taskman 50 3
  framed_send_complete "taskmantest stats" "[TASKMANTEST][STATS]" 120 stress
  local stats
  stats=$(grep -F '[TASKMANTEST][STATS]' "$serial" | tail -1)
  [[ $stats =~ last_session_full_frames=3 ]]
  [[ $stats =~ last_session_fallback_frames=0 ]]
  [[ $stats =~ captured=([0-9]+) ]]
  ((BASH_REMATCH[1] >= 257))
  framed_send_complete "taskmantest cleanup" \
    "[TASKMANTEST][CLEANUP] PASS removed=257 handles_gone=257 free_inflight=0 origin=manual" \
    600 stress
  fixture_status 0 0 0 0
  taskdiag_zero
}

run_boot(){
  local scenario=$1 smp=$2 accel=$3 body=$4 run=${5:-1}
  local start finish
  current_scenario=$scenario
  current_log=$artifact_dir/cl13fix4-${scenario}.log
  start=$(now_ms)
  boot_begin "$smp" "$accel"
  "$body" "$run"
  transport_balance 0
  save_boot "$current_log"
  boot_end
  finish=$(now_ms)
  ledger_record "$scenario" "$smp" "$accel" "$body" PASS \
    "$((finish-start))" "$current_log"
  current_log=
}

run_boot_unless_pass(){
  local scenario=$1
  shift
  if ledger_latest_pass "$scenario"; then
    echo "[CL13][FIXTURE257] REUSE scenario=$scenario status=PASS"
    return 0
  fi
  run_boot "$scenario" "$@"
}

static_gate(){
  local jobs start finish log=$artifact_dir/cl13fix4-static.log
  current_scenario=static
  start=$(now_ms)
  jobs=$(nproc)
  {
    make stack-check JOBS=2 SELFTEST=1 SELFTEST_AUTORUN=1
    cp artifacts/build/kernel-check-j2.log "$artifact_dir/cl13fix4-stack.log"
    make kernel-check JOBS=2 SELFTEST=1 SELFTEST_AUTORUN=1
    cp kernel.elf /tmp/hobbyos-cl13fix4-static-j2.elf
    make kernel-check JOBS="$jobs" SELFTEST=1 SELFTEST_AUTORUN=1
    cp kernel.elf /tmp/hobbyos-cl13fix4-static-jN.elf
    cmp -s /tmp/hobbyos-cl13fix4-static-j2.elf \
      /tmp/hobbyos-cl13fix4-static-jN.elf
    make image SELFTEST=1 SELFTEST_AUTORUN=1
    test -z "$(nm -u kernel.elf)"
    bash -n scripts/test-cl13-fixture257-focused.sh
    bash -n scripts/test-cl13-transport-focused.sh
    bash -n scripts/test-taskman-v1.sh
    bash -n scripts/test-taskman-v1-soak.sh
    ! grep -E 'kernel/src/(core/selftest|shell/commands/cmd_taskmantest)\.c:.*warning:' \
      "$artifact_dir/kernel-check-j2.log" \
      "$artifact_dir/kernel-check-j${jobs}.log"
  } 2>&1 | tee "$log"
  finish=$(now_ms)
  ledger_record static 0 host 'stack+j2+jN+image+nm' PASS \
    "$((finish-start))" "$log"
  run_boot capacity 1 tcg capacity_body
}

range_gate(){ prepare_selftest_image; run_boot range 1 tcg range_body; }
busy_gate(){ prepare_selftest_image; run_boot busy 1 tcg busy_body; }
exactly_once_gate(){ prepare_selftest_image; run_boot exactly-once-257 2 tcg exactly_once_body; }
growth257_gate(){ prepare_selftest_image; run_boot growth257 4 tcg growth257_body; }

smp4_tcg_gate(){
  prepare_selftest_image
  run_boot smp4-tcg-run1 4 tcg smp4_tcg_run1_body
  run_boot smp4-tcg-run2 4 tcg smp4_tcg_run2_body
  run_boot smp4-tcg-run3 4 tcg smp4_tcg_run3_body
}

smp4_kvm_gate(){
  [[ -r /dev/kvm && -w /dev/kvm ]] || {
    echo 'BLOCKED_BY_EXTERNAL_ENVIRONMENT: /dev/kvm unavailable' >&2
    return 3
  }
  prepare_selftest_image
  run_boot smp4-kvm-run1 4 kvm smp4_kvm_run1_body
  run_boot smp4-kvm-run2 4 kvm smp4_kvm_run2_body
}

smp8_smokes(){
  prepare_selftest_image
  run_boot smp8-tcg-smoke 8 tcg smp8_smoke_body
  [[ -r /dev/kvm && -w /dev/kvm ]] || {
    echo 'BLOCKED_BY_EXTERNAL_ENVIRONMENT: /dev/kvm unavailable' >&2
    return 3
  }
  run_boot smp8-kvm-smoke 8 kvm smp8_smoke_body
}

classification_gate(){
  local log=$artifact_dir/cl13fix4-fixture257-classification.log
  protected_snapshot "$protected_after"
  cmp -s "$protected_before" "$protected_after"
  {
    echo '[CL13][FIXTURE257_CLASSIFICATION]'
    echo 'FIXTURE_CAPACITY_MISMATCH_CONFIRMED'
  } | tee "$log"
  ledger_record classification 0 host 'focused matrix + protected runtime' \
    PASS 0 "$log"
  ledger_record protected-runtime 0 host 'before == after-focused' \
    PASS 0 "$protected_after"
}

run_all(){
  [[ -f $protected_before ]]
  if ledger_latest_pass static && ledger_latest_pass capacity; then
    echo '[CL13][FIXTURE257] REUSE scenario=static+capacity status=PASS'
  else
    static_gate
  fi
  if ledger_latest_pass range; then
    echo '[CL13][FIXTURE257] REUSE scenario=range status=PASS'
  else
    range_gate
  fi
  if ledger_latest_pass busy; then
    echo '[CL13][FIXTURE257] REUSE scenario=busy status=PASS'
  else
    busy_gate
  fi
  if ledger_latest_pass exactly-once-257; then
    echo '[CL13][FIXTURE257] REUSE scenario=exactly-once-257 status=PASS'
  else
    exactly_once_gate
  fi
  if ledger_latest_pass growth257; then
    echo '[CL13][FIXTURE257] REUSE scenario=growth257 status=PASS'
  else
    growth257_gate
  fi
  prepare_selftest_image
  run_boot_unless_pass smp4-tcg-run1 4 tcg smp4_tcg_run1_body
  run_boot_unless_pass smp4-tcg-run2 4 tcg smp4_tcg_run2_body
  run_boot_unless_pass smp4-tcg-run3 4 tcg smp4_tcg_run3_body
  [[ -r /dev/kvm && -w /dev/kvm ]] || {
    echo 'BLOCKED_BY_EXTERNAL_ENVIRONMENT: /dev/kvm unavailable' >&2
    return 3
  }
  run_boot_unless_pass smp4-kvm-run1 4 kvm smp4_kvm_run1_body
  run_boot_unless_pass smp4-kvm-run2 4 kvm smp4_kvm_run2_body
  run_boot_unless_pass smp8-tcg-smoke 8 tcg smp8_smoke_body
  run_boot_unless_pass smp8-kvm-smoke 8 kvm smp8_smoke_body
  classification_gate
}

case ${1:-} in
  static) static_gate;;
  range) range_gate;;
  busy) busy_gate;;
  exactly-once) exactly_once_gate;;
  growth257) growth257_gate;;
  smp4-tcg) smp4_tcg_gate;;
  smp4-kvm) smp4_kvm_gate;;
  all) run_all;;
  *) usage;;
esac

stop_qemu
harness_unlock 2>/dev/null || true
trap - ERR EXIT
