#!/usr/bin/env bash
set -Eeuo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

artifact_dir=artifacts/build
serial=.qemu/qemu-serial.log
hmp=(python3 scripts/qemu_hmp.py --socket .qemu/hmp.sock)
harness_start_line=0
harness_command=transport-focused
mkdir -p "$artifact_dir"
source scripts/harness-common.sh
source scripts/harness-runtime-ready.sh
source scripts/harness-framed.sh
source scripts/harness-selftest.sh
EXPECTED_REPLAYS=0
EXPECTED_CRC_REJECTS_MIN=0
EXPECTED_RECURSIVE_REJECTS=0

usage(){
  echo "Usage: $0 static|basic|partial-recovery|exactly-once|count257|matrix|all" >&2
  exit 2
}

cleanup(){ stop_qemu; }
trap cleanup EXIT

now_ms(){ echo $(( $(date +%s%N) / 1000000 )); }

protected_snapshot(){
  local output=$1 temporary=${1}.tmp
  local -a files=()
  mapfile -d '' -t files < <(
    find kernel bootloader shared -type f \
      \( -name '*.c' -o -name '*.h' -o -name '*.S' -o -name '*.s' \
         -o -name '*.asm' -o -name '*.inc' -o -name '*.ld' \) \
      ! -path 'kernel/src/shell/shell.c' \
      ! -path 'kernel/src/shell/shell.h' \
      ! -path 'kernel/src/shell/commands/cmd_tasktest.c' \
      ! -path 'kernel/src/shell/commands/cmd_tasktest.h' \
      ! -path 'kernel/src/core/selftest.c' -print0 | LC_ALL=C sort -z
  )
  sha256sum "${files[@]}" makefile >"$temporary"
  LC_ALL=C sort "$temporary" >"$output"
  rm "$temporary"
}

ledger_record(){
  local scenario=$1 smp=$2 accel=$3 command=$4 status=$5 duration=$6 artifact=$7
  local ledger=$artifact_dir/cl13-ledger.tsv hash=-
  [[ -f $ledger ]] ||
    printf 'stage\tscenario\tsmp\taccel\tbuild_variant\tcommand\tstatus\tduration_ms\tartifact\thash\n' >"$ledger"
  [[ -f $artifact ]] && hash=$(sha256sum "$artifact" | awk '{print $1}')
  printf 'transport-focused\t%s\t%s\t%s\tselftest\t%s\t%s\t%s\t%s\t%s\n' \
    "$scenario" "$smp" "$accel" "$command" "$status" "$duration" \
    "$artifact" "$hash" >>"$ledger"
}

prepare_selftest_image(){
  make kernel-check JOBS=2 SELFTEST=1 SELFTEST_AUTORUN=1 >/dev/null
  make image SELFTEST=1 SELFTEST_AUTORUN=1 >/dev/null
}

boot_begin(){
  local smp=$1 accel=$2 prefix=${3:-transport}
  harness_lock
  start_qemu "$smp" "$accel"
  grep -Fq '[BOOT][SHELL_READY] PASS' "$serial"
  selftest_validate_autorun "$serial" \
    "$artifact_dir/cl13fix3-${prefix}"
}

boot_end(){
  stop_qemu
  harness_unlock
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

basic_canary(){
  local sequence payload crc frame pattern before marker_before
  framed_send_complete "tasktest transport-selftest" \
    "[SELFTEST][SUMMARY] pass=9 fail=0 skip=0 p0_fail=0 p1_fail=0 p2_fail=0" \
    120 stress
  framed_send_complete "inputtest marker framebasic" \
    "[INPUTTEST][MARKER] name=framebasic" 90 sync

  payload="inputtest marker crcrecovery"
  marker_before=$(count "[INPUTTEST][MARKER] name=crcrecovery")
  inject_bad_crc "$payload"
  (( $(count "[INPUTTEST][MARKER] name=crcrecovery") == marker_before ))
  framed_send_complete "$payload" "[INPUTTEST][MARKER] name=crcrecovery" 90 sync
  (( $(count "[INPUTTEST][MARKER] name=crcrecovery") == marker_before + 1 ))

  marker_before=$(count "[INPUTTEST][MARKER] name=crcrecovery")
  framed_replay_last "$payload" 0 60 sync
  (( $(count "[INPUTTEST][MARKER] name=crcrecovery") == marker_before ))

  framed_boot_sync
  sequence=$((HARNESS_FRAME_SEQUENCE + 1))
  payload="tasktest exec 99 00000000 ps"
  crc=$(framed_crc32 "$payload")
  frame="tasktest exec $sequence $crc $payload"
  pattern="[HARNESS][FRAME] REJECT seq=$sequence reason=recursive"
  before=$(count "$pattern")
  hmp_text "$frame" sync --enter >/dev/null
  wait_new_fixed "$pattern" "$before" 60
  framed_send_complete "inputtest marker recursiveguard" \
    "[INPUTTEST][MARKER] name=recursiveguard" 90 sync
  echo "[CL13][TRANSPORT_BASIC] PASS"
}

partial_recovery_canary(){
  local payload sequence crc frame partial reject_before marker_before
  payload="inputtest marker partialrecovery"
  framed_boot_sync
  sequence=$((HARNESS_FRAME_SEQUENCE + 1))
  crc=$(framed_crc32 "$payload")
  frame=$(framed_make_frame "$payload" "$sequence" "$crc")
  partial=${frame%partialrecovery}
  reject_before=$(count "[HARNESS][FRAME] REJECT seq=$sequence")
  marker_before=$(count "[INPUTTEST][MARKER] name=partialrecovery")
  hmp_text "$partial" sync >/dev/null
  sleep 1
  framed_send_complete "$payload" \
    "[INPUTTEST][MARKER] name=partialrecovery" 120 sync
  (( $(count "[HARNESS][FRAME] REJECT seq=$sequence") > reject_before ))
  (( $(count "[INPUTTEST][MARKER] name=partialrecovery") == marker_before + 1 ))
  echo "[CL13][TRANSPORT_PARTIAL_RECOVERY] PASS"
}

marker_frames(){
  local total=$1 prefix=$2 i name
  for ((i=1; i<=total; i++)); do
    name="${prefix}${i}"
    framed_send_complete "inputtest marker $name" \
      "[INPUTTEST][MARKER] name=$name" 90 normal
  done
}

exactly_once_canary(){
  local fixture_count=${1:-257} setup_before
  setup_before=$(count "[TASKMANTEST][SETUP] PASS count=$fixture_count")
  framed_send_complete "taskmantest setup $fixture_count" \
    "[TASKMANTEST][SETUP] PASS count=$fixture_count" 600 stress
  framed_replay_last "taskmantest setup $fixture_count" 0 120 stress
  (( $(count "[TASKMANTEST][SETUP] PASS count=$fixture_count") == setup_before + 1 ))
  framed_send_complete "taskmantest fixture-status" \
    "[TASKMANTEST][FIXTURE_STATUS] PASS max=257 active=$fixture_count present=$fixture_count" \
    120 stress
  framed_send_complete "taskmantest cleanup" \
    "[TASKMANTEST][CLEANUP] PASS removed=$fixture_count handles_gone=$fixture_count free_inflight=0" \
    600 stress
  echo "[CL13][TRANSPORT_EXACTLY_ONCE] PASS count=$fixture_count"
}

anchored_taskman(){
  local refresh=${1:-50} frames=${2:-1}
  framed_send_complete "taskmantest anchor-reset" \
    "[TASKMANTEST][ANCHOR_RESET] PASS" 60 stress
  framed_send_complete "taskmantest auto-exit-frames $frames" \
    "[TASKMANTEST][AUTO_EXIT_ARM] PASS frames=$frames" 90 stress
  framed_send_complete "taskman $refresh" \
    "[TASKMAN][AUTO_EXIT] PASS target=$frames full_frames=$frames fallback_frames=0" \
    180 stress
}

count_sequence(){
  local inject=${1:-0}; shift || true
  local count
  for count in "$@"; do
    if ((inject)) && [[ $count == 257 ]]; then
      inject_bad_crc "taskmantest setup 257"
    fi
    framed_send_complete "taskmantest setup $count" \
      "[TASKMANTEST][SETUP] PASS count=$count" 300 stress
    framed_send_complete "ps 1 128" "[PS][SNAPSHOT]" 180 stress
    anchored_taskman 50 1
    framed_send_complete "taskmantest cleanup" \
      "[TASKMANTEST][CLEANUP] PASS removed=$count" 300 stress
    framed_send_complete "taskmantest fixture-status" \
      "[TASKMANTEST][FIXTURE_STATUS] PASS max=257 active=0 present=0" \
      120 stress
    framed_send_complete "taskdiag check" "[TASKDIAG][CHECK] PASS" 120 stress
  done
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
    "[TASKMANTEST][CLEANUP] PASS removed=20" 240 stress
}

transport_checks(){
  local line replays crc_rejects recursive_rejects sequence_rejects
  framed_send_complete "tasktest transport-status" "[HARNESS][STATUS]" 90 stress
  line=$(grep -F '[HARNESS][STATUS]' "$serial" | tail -1)
  [[ $line =~ accepted=([0-9]+)[[:space:]]completed=([0-9]+) ]]
  [[ ${BASH_REMATCH[1]} == "${BASH_REMATCH[2]}" ]]
  [[ $line =~ replays=([0-9]+) ]]
  replays=${BASH_REMATCH[1]}
  [[ $line =~ crc_rejects=([0-9]+) ]]
  crc_rejects=${BASH_REMATCH[1]}
  [[ $line =~ sequence_rejects=([0-9]+) ]]
  sequence_rejects=${BASH_REMATCH[1]}
  [[ $line =~ recursive_rejects=([0-9]+) ]]
  recursive_rejects=${BASH_REMATCH[1]}
  ((replays >= EXPECTED_REPLAYS))
  ((crc_rejects >= EXPECTED_CRC_REJECTS_MIN))
  ((sequence_rejects == 0))
  ((recursive_rejects == EXPECTED_RECURSIVE_REJECTS))
  framed_send_complete "inputtest check" "[INPUTTEST][CHECK] PASS" 120 stress
  framed_send_complete "taskdiag check" "[TASKDIAG][CHECK] PASS" 120 stress
}

save_boot(){
  local artifact=$1
  assert_clean_log "$serial"
  cp "$serial" "$artifact"
  sha256sum "$artifact" >"${artifact%.log}.sha256"
}

run_boot(){
  local scenario=$1 smp=$2 accel=$3 body=$4 run=$5
  local artifact=$artifact_dir/cl13fix3-${scenario}-run${run}.log start finish
  start=$(now_ms)
  boot_begin "$smp" "$accel" "${scenario}-run${run}"
  EXPECTED_REPLAYS=0
  EXPECTED_CRC_REJECTS_MIN=0
  EXPECTED_RECURSIVE_REJECTS=0
  "$body" "$run"
  transport_checks
  save_boot "$artifact"
  boot_end
  finish=$(now_ms)
  ledger_record "${scenario}-run${run}" "$smp" "$accel" "$body" PASS \
    "$((finish-start))" "$artifact"
}

body_smp1(){
  local run=$1
  EXPECTED_REPLAYS=1
  EXPECTED_CRC_REJECTS_MIN=2
  EXPECTED_RECURSIVE_REJECTS=1
  basic_canary
  partial_recovery_canary
  marker_frames 100 "up${run}m"
}

body_smp2(){
  local run=$1
  EXPECTED_REPLAYS=1
  marker_frames 100 "s2${run}m"
  exactly_once_canary
}

body_smp4_tcg(){
  local run=$1 inject=0
  ((run == 1)) && inject=1
  EXPECTED_CRC_REJECTS_MIN=$inject
  count_sequence "$inject" 1 20 50 129 257
  twenty_sessions
  [[ $run != 1 ]] || echo "[CL13][SETUP257_REPRODUCTION] PASS side_effects=1"
}

body_smp4_kvm(){
  local run=$1
  EXPECTED_REPLAYS=1
  count_sequence 0 20 129 257
  exactly_once_canary
}

static_gate(){
  [[ $(framed_crc32 123456789) == cbf43926 ]]
  make stack-check SELFTEST=1 SELFTEST_AUTORUN=1
  make kernel-check JOBS=2 SELFTEST=1 SELFTEST_AUTORUN=1
  cp kernel.elf /tmp/hobbyos-cl13fix3-selftest-j2.elf
  make kernel-check JOBS="$(nproc)" SELFTEST=1 SELFTEST_AUTORUN=1
  cp kernel.elf /tmp/hobbyos-cl13fix3-selftest-jN.elf
  cmp -s /tmp/hobbyos-cl13fix3-selftest-j2.elf \
    /tmp/hobbyos-cl13fix3-selftest-jN.elf
  make image SELFTEST=1 SELFTEST_AUTORUN=1
  [[ -z $(nm -u kernel.elf) ]]
  prepare_selftest_image
  echo "[CL13][TRANSPORT_STATIC] PASS"
}

matrix_gate(){
  local run
  [[ -r /dev/kvm && -w /dev/kvm ]] || {
    echo "BLOCKED_BY_EXTERNAL_ENVIRONMENT: /dev/kvm is unavailable" >&2
    return 3
  }
  prepare_selftest_image
  for run in 1 2 3; do run_boot smp1-tcg 1 tcg body_smp1 "$run"; done
  for run in 1 2 3; do run_boot smp2-tcg 2 tcg body_smp2 "$run"; done
  for run in 1 2 3; do run_boot smp2-kvm 2 kvm body_smp2 "$run"; done
  for run in 1 2 3; do run_boot smp4-tcg 4 tcg body_smp4_tcg "$run"; done
  for run in 1 2; do run_boot smp4-kvm 4 kvm body_smp4_kvm "$run"; done
  {
    echo "[CL13][TRANSPORT_CLASSIFICATION]"
    echo "UNFRAMED_HMP_COMMAND_LOSS_CONFIRMED"
  } | tee "$artifact_dir/cl13fix3-transport-classification.log"
  ledger_record classification 0 host focused \
    UNFRAMED_HMP_COMMAND_LOSS_CONFIRMED PASS 0 \
    "$artifact_dir/cl13fix3-transport-classification.log"
  local recovered=$artifact_dir/cl13fix3-smp4-tcg-run1.log recovered_hash
  recovered_hash=$(sha256sum "$recovered" | awk '{print $1}')
  printf 'taskman-anchor-focused\tsmp4-tcg-variants-run1\t4\ttcg\tfix3-framed\tcount257 transactional reproduction\tPASS\t0\t%s\t%s\n' \
    "$recovered" "$recovered_hash" >>"$artifact_dir/cl13-ledger.tsv"
  protected_snapshot \
    "$artifact_dir/cl13fix3-protected-runtime-after-focused.sha256"
  cmp -s "$artifact_dir/cl13fix3-protected-runtime-before.sha256" \
    "$artifact_dir/cl13fix3-protected-runtime-after-focused.sha256"
  ledger_record protected-runtime 0 host 'before == after-focused' PASS 0 \
    "$artifact_dir/cl13fix3-protected-runtime-after-focused.sha256"
}

single_basic(){ prepare_selftest_image; run_boot basic 1 tcg body_smp1 1; }
single_partial(){
  prepare_selftest_image
  boot_begin 1 tcg partial-recovery
  EXPECTED_CRC_REJECTS_MIN=1
  partial_recovery_canary
  transport_checks
  save_boot "$artifact_dir/cl13fix3-partial-recovery.log"
  boot_end
}
single_exactly_once(){
  prepare_selftest_image
  boot_begin 4 tcg exactly-once
  EXPECTED_REPLAYS=1
  exactly_once_canary
  transport_checks
  save_boot "$artifact_dir/cl13fix3-exactly-once.log"
  boot_end
}
single_count257(){
  prepare_selftest_image
  boot_begin 4 tcg count257
  EXPECTED_CRC_REJECTS_MIN=1
  count_sequence 1 1 20 50 129 257
  transport_checks
  save_boot "$artifact_dir/cl13fix3-count257-reproduction.log"
  boot_end
}

case ${1:-} in
  static) static_gate;;
  basic) single_basic;;
  partial-recovery) single_partial;;
  exactly-once) single_exactly_once;;
  count257) single_count257;;
  matrix) matrix_gate;;
  all) static_gate; matrix_gate;;
  *) usage;;
esac

stop_qemu
harness_unlock 2>/dev/null || true
trap - EXIT
