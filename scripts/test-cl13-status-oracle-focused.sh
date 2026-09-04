#!/usr/bin/env bash
set -Eeuo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

artifact_dir=artifacts/build
ledger=$artifact_dir/cl13-ledger.tsv
serial=.qemu/qemu-serial.log
hmp=(python3 scripts/qemu_hmp.py --socket .qemu/hmp.sock)
harness_start_line=0
harness_command=status-oracle-focused
mkdir -p "$artifact_dir"
source scripts/harness-common.sh
source scripts/harness-runtime-ready.sh
source scripts/harness-framed.sh
source scripts/harness-selftest.sh

cleanup(){
  stop_qemu
  [[ ${HARNESS_LOCK_HELD:-0} != 1 ]] || harness_unlock
}
trap cleanup EXIT

usage(){
  echo "Usage: $0 host-unit|static|smp8-kvm|all" >&2
  exit 2
}

record(){
  local scenario=$1 smp=$2 accel=$3 command=$4 status=$5 artifact=$6 hash=-
  [[ ! -f $artifact ]] || hash=$(sha256sum "$artifact" | awk '{print $1}')
  printf 'status-oracle-focused\t%s\t%s\t%s\tselftest\t%s\t%s\t0\t%s\t%s\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t-\n' \
    "$scenario" "$smp" "$accel" "$command" "$status" "$artifact" "$hash" \
    >>"$ledger"
}

expect_status_pass(){
  local file=$1 marker=${2:-}
  framed_evaluate_status_log_contract "$file" 1 0 "$marker"
}

host_unit(){
  local temp out=$artifact_dir/cl13fix12-status-authority-host-unit.log
  temp=$(mktemp -d)

  printf '%s\n' \
    '[HARNESS][FRAME] ACCEPT seq=1 crc=c990a27a len=26' \
    '[HARNESS][BEGIN] seq=1' \
    '[INPUTTEST][PRODUCERS] PASS accepted=100000' \
    '[HARNESS][END] seq=1 status=0' >"$temp/present.log"
  expect_status_pass "$temp/present.log" \
    '[INPUTTEST][PRODUCERS] PASS accepted=100000'
  [[ $HARNESS_FRAME_LAST_DIAGNOSTIC_MARKER == 1 ]]

  printf '%s\n' \
    '[HARNESS][FRAME] ACCEPT seq=1 crc=c990a27a len=26' \
    '[HARNESS][BEGIN] seq=1' \
    '[SMP] prefix [INPUTTEST][PRODUCERS] PASS accepted=' \
    '100000 consumed=100000' \
    '[HARNESS][END] seq=1 status=0' >"$temp/interleaved.log"
  expect_status_pass "$temp/interleaved.log" \
    '[INPUTTEST][PRODUCERS] PASS accepted=100000'
  [[ $HARNESS_FRAME_LAST_DIAGNOSTIC_MARKER == 0 ]]

  printf '%s\n' \
    '[HARNESS][FRAME] ACCEPT seq=1 crc=c990a27a len=26' \
    '[HARNESS][BEGIN] seq=1' \
    '[HARNESS][END] seq=1 status=0' >"$temp/absent.log"
  expect_status_pass "$temp/absent.log" '[INPUTTEST][PRODUCERS] PASS'
  [[ $HARNESS_FRAME_LAST_DIAGNOSTIC_MARKER == 0 ]]

  printf '%s\n' \
    '[HARNESS][FRAME] ACCEPT seq=1 crc=c990a27a len=26' \
    '[HARNESS][BEGIN] seq=1' \
    '[HARNESS][END] seq=1 status=1' >"$temp/status1.log"
  ! expect_status_pass "$temp/status1.log"
  [[ $HARNESS_FRAME_LAST_CLASSIFICATION == GUEST_COMMAND_FAILURE ]]

  printf '%s\n' \
    '[HARNESS][FRAME] ACCEPT seq=1 crc=c990a27a len=26' \
    '[HARNESS][BEGIN] seq=1' >"$temp/stall.log"
  ! expect_status_pass "$temp/stall.log"
  [[ $HARNESS_FRAME_LAST_CLASSIFICATION == GUEST_COMMAND_STALL ]]

  printf '%s\n' \
    '[HARNESS][FRAME] ACCEPT seq=1 crc=c990a27a len=26' \
    '[HARNESS][END] seq=1 status=0' >"$temp/no-begin.log"
  ! expect_status_pass "$temp/no-begin.log"
  [[ $HARNESS_FRAME_LAST_CLASSIFICATION == GUEST_COMMAND_CONTRACT_FAILURE ]]

  printf '%s\n' '[HARNESS][REPLAY] seq=1 status=0' >"$temp/replay.log"
  expect_status_pass "$temp/replay.log"
  [[ $HARNESS_FRAME_LAST_STATUS == 0 ]]

  printf '%s\n' \
    '[CL13][STATUS_AUTHORITY_HOST_UNIT] PASS' \
    'marker_present=PASS diagnostic_marker=1' \
    'marker_interleaved=PASS diagnostic_marker=0' \
    'marker_absent=PASS diagnostic_marker=0' \
    'status1=REJECTED begin_without_end=STALL end_without_begin=CONTRACT replay0=PASS' \
    >"$out"
  cat "$out"
  record host-unit 0 host synthetic-log PASS "$out"
  rm -rf "$temp"
}

static_gate(){
  local out=$artifact_dir/cl13fix12-status-oracle-static.log
  : >"$out"
  bash -n scripts/harness-framed.sh
  bash -n scripts/test-cl13-status-oracle-focused.sh
  python3 -m py_compile scripts/cl13-soak-checkpoint.py
  rg -n 'framed_send_status_complete|STATUS_AUTHORITY_PASS' \
    scripts/harness-framed.sh >>"$out"
  scripts/cl13-soak-checkpoint.py verify-fix11-input-failure | tee -a "$out"
  git diff --check
  record static 0 host shell+python+offline PASS "$out"
}

host_evidence(){
  local output=$1
  {
    echo "timestamp=$(date --iso-8601=ns)"
    echo "nproc=$(nproc)"
    echo "loadavg=$(cat /proc/loadavg)"
    echo "cpus_allowed_list=$(awk '/Cpus_allowed_list/{print $2}' /proc/self/status)"
    echo "cpu_psi=$(tr '\n' ';' </proc/pressure/cpu 2>/dev/null || true)"
    echo "kvm_readable=$([[ -r /dev/kvm ]] && echo 1 || echo 0)"
    echo "kvm_writable=$([[ -w /dev/kvm ]] && echo 1 || echo 0)"
    echo "qemu_version=$(qemu-system-x86_64 --version | head -1)"
  } >"$output"
}

transport_balance(){
  local line
  framed_send_complete "tasktest transport-status" "[HARNESS][STATUS]" 90 stress
  line=$(grep -F '[HARNESS][STATUS]' "$serial" | tail -1)
  [[ $line =~ accepted=([0-9]+)[[:space:]]completed=([0-9]+) ]]
  [[ ${BASH_REMATCH[1]} == "${BASH_REMATCH[2]}" ]]
}

smp8_kvm(){
  local artifact=$artifact_dir/cl13fix12-status-oracle-smp8-kvm.log
  local hostlog=$artifact_dir/cl13fix12-status-oracle-smp8-kvm-host.log
  local before=$artifact_dir/cl13fix12-status-oracle-host-before.env
  local after=$artifact_dir/cl13fix12-status-oracle-host-after.env
  local -a producer_sequences=()
  [[ -r /dev/kvm && -w /dev/kvm ]] || {
    echo BLOCKED_BY_EXTERNAL_ENVIRONMENT >&2
    return 3
  }
  make image SELFTEST=1 SELFTEST_AUTORUN=1 >/dev/null
  harness_lock
  host_evidence "$before"
  start_qemu 8 kvm
  selftest_validate_autorun "$serial" \
    "$artifact_dir/cl13fix12-status-oracle-smp8-kvm" \
    accounting.sleep.minimum_deadline \
    accounting.sleep.one_ms_granularity \
    accounting.sleep.late_is_diagnostic \
    accounting.sleep.wait_result \
    accounting.sleep.percentiles
  HARNESS_STATUS_RECORD_LOG=$hostlog
  : >"$HARNESS_STATUS_RECORD_LOG"

  framed_send_launch "smpstress 16 0 1000" \
    "[SMP] smpstress spawning workers=16" 90 stress
  for run in 1 2; do
    framed_send_status_complete "inputtest producers 100000" 900 stress 0 \
      "[INPUTTEST][PRODUCERS] PASS accepted=100000"
    producer_sequences+=("$HARNESS_FRAME_LAST_SEQUENCE")
  done
  framed_send_status_complete "inputtest check" 90 stress 0 \
    "[INPUTTEST][CHECK] PASS"
  framed_send_status_complete "killtest smpstress-sweep" 900 stress 0 \
    "[SMP][KILL_SWEEP] PASS workers=16"
  framed_send_status_complete "taskmantest fixture-status" 90 stress 0 \
    "[TASKMANTEST][FIXTURE_STATUS] PASS"
  framed_send_status_complete "taskdiag check" 180 stress 0 \
    "[TASKDIAG][CHECK] PASS"
  transport_balance

  for sequence in "${producer_sequences[@]}"; do
    [[ $(framed_exact_record_count "$serial" \
      "^\\[HARNESS\\]\\[BEGIN\\] seq=$sequence$" 1) == 1 ]]
    [[ $(framed_exact_record_count "$serial" \
      "^\\[HARNESS\\]\\[END\\] seq=$sequence status=0$" 1) == 1 ]]
  done
  assert_clean_log "$serial"
  host_evidence "$after"
  cp "$serial" "$artifact"
  printf '%s\n' \
    '[CL13][STATUS_ORACLE_CLASSIFICATION] DIAGNOSTIC_MARKER_INTERLEAVING_CONFIRMED' \
    >>"$artifact"
  sha256sum "$artifact" >"${artifact%.log}.sha256"
  record smp8-kvm-collision 8 kvm \
    'smpstress 16 period1000 + producers 2 + checks + sweep' PASS "$artifact"
  unset HARNESS_STATUS_RECORD_LOG
  stop_qemu
  harness_unlock
  echo '[CL13][STATUS_ORACLE_CLASSIFICATION] DIAGNOSTIC_MARKER_INTERLEAVING_CONFIRMED'
}

case ${1:-} in
  host-unit) host_unit;;
  static) static_gate;;
  smp8-kvm) smp8_kvm;;
  all) host_unit; static_gate; smp8_kvm;;
  *) usage;;
esac
