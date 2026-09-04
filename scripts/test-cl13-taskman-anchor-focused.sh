#!/usr/bin/env bash
set -Eeuo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

artifact_dir=artifacts/build
serial=.qemu/qemu-serial.log
hmp=(python3 scripts/qemu_hmp.py --socket .qemu/hmp.sock)
harness_start_line=0
harness_command=cl13fix2
ledger=$artifact_dir/cl13-ledger.tsv
runtime_before=$artifact_dir/cl13fix2-runtime-before.sha256
runtime_after_focused=$artifact_dir/cl13fix2-runtime-after-focused.sha256
mkdir -p "$artifact_dir"
source scripts/harness-common.sh
source scripts/harness-runtime-ready.sh
source scripts/harness-framed.sh
source scripts/harness-selftest.sh

usage(){
  echo "Usage: $0 static|negative-anchor|positive-anchor|variants|soak-smoke|all" >&2
  exit 2
}

cleanup(){ stop_qemu; }
trap cleanup EXIT

now_ms(){ echo $(( $(date +%s%N) / 1000000 )); }

ensure_ledger(){
  if [[ ! -f $ledger ]]; then
    printf 'stage\tscenario\tsmp\taccel\tbuild_variant\tcommand\tstatus\tduration_ms\tartifact\thash\n' >"$ledger"
  fi
}

record_pass(){
  local scenario=$1 smp=$2 accel=$3 command=$4 duration=$5 artifact=$6 hash
  ensure_ledger
  hash=$(sha256sum "$artifact" | awk '{print $1}')
  printf 'taskman-anchor-focused\t%s\t%s\t%s\tfix2\t%s\tPASS\t%s\t%s\t%s\n' \
    "$scenario" "$smp" "$accel" "$command" "$duration" "$artifact" "$hash" >>"$ledger"
}

runtime_snapshot(){
  local output=$1
  local -a files=()
  mapfile -d '' -t files < <(
    {
      find kernel bootloader shared -type f \
        \( -name '*.c' -o -name '*.h' -o -name '*.S' -o -name '*.s' \
           -o -name '*.asm' -o -name '*.inc' -o -name '*.ld' \) -print0
      printf 'makefile\0'
    } | LC_ALL=C sort -z
  )
  sha256sum "${files[@]}" >"$output"
}

require_runtime_unchanged(){
  local current=$artifact_dir/cl13fix2-runtime-current.sha256
  [[ -f $runtime_before ]]
  runtime_snapshot "$current"
  cmp -s "$runtime_before" "$current"
}

prepare_image(){
  make kernel-check JOBS=2 SELFTEST=1 SELFTEST_AUTORUN=0 >/dev/null
  make image SELFTEST=1 SELFTEST_AUTORUN=0 >/dev/null
}

boot(){
  local accel=$1
  harness_lock
  HARNESS_AUTORUN_EXPECTED=0
  start_qemu 4 "$accel"
  shell_sync
}

finish_boot(){
  stop_qemu
  harness_unlock
}

stats_value(){
  local line=$1 key=$2 regex
  regex="(^|[[:space:]])${key}=([0-9]+)($|[[:space:]])"
  [[ $line =~ $regex ]]
  printf '%s\n' "${BASH_REMATCH[2]}"
}

stats_mode(){
  local line=$1
  [[ $line =~ (^|[[:space:]])last_mode=([^[:space:]]+)($|[[:space:]]) ]]
  printf '%s\n' "${BASH_REMATCH[2]}"
}

latest_stats(){ grep -F '[TASKMANTEST][STATS]' "$serial" | tail -1; }

wait_clean_count(){
  local pattern=$1 before=$2 timeout=$3 deadline fresh
  deadline=$((SECONDS+timeout))
  while ((SECONDS<=deadline)); do
    (( $(count "$pattern") > before )) && return 0
    fresh=$(tail -n +$((harness_start_line+1)) "$serial" 2>/dev/null || true)
    if grep -Eq 'PANIC|#PF|#GP|FATAL|STRUCTURAL_FAULT|FINISH_FAULT' <<<"$fresh"; then
      return 3
    fi
    pgrep -f qemu-system-x86_64 >/dev/null || return 3
    sleep 1
  done
  return 4
}

reset_fixture(){
  send_complete "taskmantest reset-controls" "[TASKMANTEST][CONTROLS] RESET" 60 stress
  send_complete "taskmantest stats-reset" "[TASKMANTEST][STATS_RESET] PASS" 60 stress
  send_complete "taskmantest setup 20" "[TASKMANTEST][SETUP] PASS count=20" 240 stress
}

degrade_anchor(){
  # Four snapshots print at least 128 table/body lines with this 28-task
  # fixture, exceeding the largest certified console height.  This count is
  # fixed; it is never adapted by observing whether TASKMAN passes or fails.
  local pass
  for pass in 1 2 3 4; do
    send_complete "ps 1 128" "[PS][SNAPSHOT]" 120 stress
  done
}

cleanup_fixture(){
  send_complete "taskmantest cleanup" "[TASKMANTEST][CLEANUP] PASS removed=20" 240 stress
  send_complete "taskmantest reset-controls" "[TASKMANTEST][CONTROLS] RESET" 60 stress
  send_complete "taskmantest stats-reset" "[TASKMANTEST][STATS_RESET] PASS" 60 stress
  send_complete "taskmantest check" \
    "[TASKMANTEST][CHECK] PASS violations=0 model_live=0" 90 stress
  send_complete "taskdiag check" "[TASKDIAG][CHECK] PASS" 90 stress
}

negative_anchor(){
  local begin end owner auto stats mode full fallback shortfalls pending
  local start finish artifact=$artifact_dir/cl13fix2-negative-anchor.log
  require_runtime_unchanged
  prepare_image
  start=$(now_ms)
  boot tcg
  reset_fixture
  degrade_anchor
  send_complete "taskmantest auto-exit-frames 3" \
    "[TASKMANTEST][AUTO_EXIT_ARM] PASS frames=3" 90 stress

  begin=$(count "[MODAL] session_begin OK")
  end=$(count "[MODAL] session_end OK")
  owner=$(count "[TASKMAN][OWNER_STATE] shell=BLOCKED session=ACTIVE")
  auto=$(count "[TASKMAN][AUTO_EXIT] PASS target=3 full_frames=3 fallback_frames=0")
  framed_start_command "taskman 1000" normal 60
  wait_clean_count "[MODAL] session_begin OK" "$begin" 60
  wait_clean_count "[TASKMAN][OWNER_STATE] shell=BLOCKED session=ACTIVE" "$owner" 60

  if wait_clean_count "[TASKMAN][AUTO_EXIT] PASS target=3 full_frames=3 fallback_frames=0" "$auto" 20; then
    cp "$serial" "$artifact"
    echo "[CL13][TASKMAN_ANCHOR_NEGATIVE] FAIL reason=NEGATIVE_ANCHOR_NOT_REPRODUCED" >>"$artifact"
    echo "NEGATIVE_ANCHOR_NOT_REPRODUCED" >&2
    return 1
  else
    [[ $? == 4 ]]
  fi

  harness_start_line=$(wc -l <"$serial")
  hmp_key esc normal >/dev/null
  hmp_key esc normal >/dev/null
  hmp_key esc normal >/dev/null
  wait_clean_count "[MODAL] session_end OK" "$end" 120
  framed_finish_started "[MODAL] session_end OK" 120 0
  send_complete "taskmantest stats" "[TASKMANTEST][STATS]" 90 stress
  stats=$(latest_stats)
  mode=$(stats_mode "$stats")
  full=$(stats_value "$stats" last_session_full_frames)
  fallback=$(stats_value "$stats" last_session_fallback_frames)
  shortfalls=$(stats_value "$stats" auto_exit_shortfalls)
  pending=$(stats_value "$stats" auto_exit_pending)
  [[ $mode == TOO_SHORT ]]
  ((full == 0 && fallback > 0 && shortfalls >= 1 && pending == 0))
  send_complete "taskdiag check" "[TASKDIAG][CHECK] PASS" 90 stress
  cleanup_fixture
  assert_clean_log "$serial"
  cp "$serial" "$artifact"
  {
    echo "[CL13][TASKMAN_ANCHOR_NEGATIVE] PASS"
    echo "mode=$mode"
    echo "full_frames=$full"
    echo "fallback_frames=$fallback"
    echo "shortfalls=$shortfalls"
    echo "model_live=0"
  } >>"$artifact"
  finish_boot
  finish=$(now_ms)
  record_pass negative-anchor 4 tcg 'four fixed ps snapshots + bounded ESC diagnosis' \
    "$((finish-start))" "$artifact"
  echo "[CL13][TASKMAN_ANCHOR_NEGATIVE] PASS mode=$mode full_frames=$full fallback_frames=$fallback shortfalls=$shortfalls model_live=0"
}

positive_anchor(){
  local stats mode full fallback captured render scroll start finish
  local artifact=$artifact_dir/cl13fix2-positive-anchor.log
  require_runtime_unchanged
  prepare_image
  start=$(now_ms)
  boot tcg
  reset_fixture
  degrade_anchor
  send_complete "taskmantest anchor-reset" "[TASKMANTEST][ANCHOR_RESET] PASS" 60 stress
  send_complete "taskmantest auto-exit-frames 3" \
    "[TASKMANTEST][AUTO_EXIT_ARM] PASS frames=3" 90 stress
  send_complete "taskman 1000" \
    "[TASKMAN][AUTO_EXIT] PASS target=3 full_frames=3 fallback_frames=0" 180 stress
  send_complete "taskmantest stats" "[TASKMANTEST][STATS]" 90 stress
  stats=$(latest_stats)
  mode=$(stats_mode "$stats")
  full=$(stats_value "$stats" last_session_full_frames)
  fallback=$(stats_value "$stats" last_session_fallback_frames)
  captured=$(stats_value "$stats" captured)
  render=$(stats_value "$stats" render_failures)
  scroll=$(stats_value "$stats" last_session_scroll_delta)
  [[ $mode == WIDE || $mode == COMPACT ]]
  ((full == 3 && fallback == 0 && captured >= 20 && render == 0 && scroll == 0))
  cleanup_fixture
  assert_clean_log "$serial"
  cp "$serial" "$artifact"
  echo "[CL13][TASKMAN_ANCHOR_POSITIVE] PASS mode=$mode full_frames=$full fallback_frames=$fallback captured=$captured" >>"$artifact"
  finish_boot
  finish=$(now_ms)
  record_pass positive-anchor 4 tcg 'four fixed ps snapshots + anchor reset + auto exit' \
    "$((finish-start))" "$artifact"
  [[ -f $artifact_dir/cl13fix2-negative-anchor.log ]]
  grep -Fq '[CL13][TASKMAN_ANCHOR_NEGATIVE] PASS' \
    "$artifact_dir/cl13fix2-negative-anchor.log"
  {
    echo '[CL13][TASKMAN_ANCHOR_CLASSIFICATION]'
    echo 'HARNESS_ANCHOR_DEGRADATION_CONFIRMED'
  } >"$artifact_dir/cl13fix2-taskman-anchor-classification.log"
  record_pass classification 0 host 'negative + positive anchor canaries' 0 \
    "$artifact_dir/cl13fix2-taskman-anchor-classification.log"
  echo "[CL13][TASKMAN_ANCHOR_POSITIVE] PASS"
  echo "[CL13][TASKMAN_ANCHOR_CLASSIFICATION] HARNESS_ANCHOR_DEGRADATION_CONFIRMED"
}

auto_session(){
  local refresh=$1 frames=$2 minimum=$3 stats mode full fallback captured render scroll shortfalls
  send_complete "taskmantest anchor-reset" "[TASKMANTEST][ANCHOR_RESET] PASS" 60 stress
  send_complete "taskmantest auto-exit-frames $frames" \
    "[TASKMANTEST][AUTO_EXIT_ARM] PASS frames=$frames" 90 stress
  send_complete "taskman $refresh" \
    "[TASKMAN][AUTO_EXIT] PASS target=$frames full_frames=$frames fallback_frames=0" 180 stress
  send_complete "taskmantest stats" "[TASKMANTEST][STATS]" 90 stress
  stats=$(latest_stats)
  mode=$(stats_mode "$stats")
  full=$(stats_value "$stats" last_session_full_frames)
  fallback=$(stats_value "$stats" last_session_fallback_frames)
  captured=$(stats_value "$stats" captured)
  render=$(stats_value "$stats" render_failures)
  scroll=$(stats_value "$stats" last_session_scroll_delta)
  shortfalls=$(stats_value "$stats" auto_exit_shortfalls)
  [[ $mode == WIDE || $mode == COMPACT ]]
  ((full == frames && fallback == 0 && captured >= minimum && render == 0 && scroll == 0 && shortfalls == 0))
}

variant_count(){
  local count=$1 refresh=$2 frames=$3
  send_complete "taskmantest setup $count" \
    "[TASKMANTEST][SETUP] PASS count=$count" 240 stress
  send_complete "ps 1 128" "[PS][SNAPSHOT]" 120 stress
  auto_session "$refresh" "$frames" "$count"
  send_complete "taskmantest cleanup" \
    "[TASKMANTEST][CLEANUP] PASS removed=$count" 240 stress
  send_complete "taskdiag check" "[TASKDIAG][CHECK] PASS" 90 stress
}

variants(){
  local run accel start finish artifact
  require_runtime_unchanged
  grep -Fq HARNESS_ANCHOR_DEGRADATION_CONFIRMED \
    "$artifact_dir/cl13fix2-taskman-anchor-classification.log"
  prepare_image
  for run in 1 2 3; do
    start=$(now_ms)
    boot tcg
    send_complete "taskmantest reset-controls" "[TASKMANTEST][CONTROLS] RESET" 60 stress
    send_complete "taskmantest stats-reset" "[TASKMANTEST][STATS_RESET] PASS" 60 stress
    variant_count 1 50 3
    variant_count 20 1000 3
    variant_count 50 2000 3
    variant_count 129 50 3
    variant_count 257 50 3
    send_complete "taskmantest check" "[TASKMANTEST][CHECK] PASS" 90 stress
    assert_clean_log "$serial"
    artifact=$artifact_dir/cl13fix2-smp4-tcg-variants-run${run}.log
    cp "$serial" "$artifact"
    finish_boot
    finish=$(now_ms)
    record_pass "smp4-tcg-variants-run${run}" 4 tcg 'counts 1,20,50,129,257' \
      "$((finish-start))" "$artifact"
  done
  [[ -r /dev/kvm && -w /dev/kvm ]]
  for run in 1 2; do
    start=$(now_ms)
    boot kvm
    send_complete "taskmantest reset-controls" "[TASKMANTEST][CONTROLS] RESET" 60 stress
    send_complete "taskmantest stats-reset" "[TASKMANTEST][STATS_RESET] PASS" 60 stress
    variant_count 20 1000 3
    variant_count 129 50 15
    send_complete "taskmantest check" "[TASKMANTEST][CHECK] PASS" 90 stress
    assert_clean_log "$serial"
    artifact=$artifact_dir/cl13fix2-smp4-kvm-variants-run${run}.log
    cp "$serial" "$artifact"
    finish_boot
    finish=$(now_ms)
    record_pass "smp4-kvm-variants-run${run}" 4 kvm 'counts 20,129' \
      "$((finish-start))" "$artifact"
  done
  echo '[CL13][TASKMAN_VARIANTS] PASS tcg=3/3 kvm=2/2'
}

soak_smoke(){
  local session refresh stats sessions fallback shortfalls scroll start finish
  local artifact=$artifact_dir/cl13fix2-taskman-soak-smoke.log
  require_runtime_unchanged
  prepare_image
  start=$(now_ms)
  boot tcg
  send_complete "taskmantest reset-controls" "[TASKMANTEST][CONTROLS] RESET" 60 stress
  send_complete "taskmantest stats-reset" "[TASKMANTEST][STATS_RESET] PASS" 60 stress
  send_complete "taskmantest setup 20" "[TASKMANTEST][SETUP] PASS count=20" 240 stress
  for ((session=1; session<=20; session++)); do
    case $((session % 3)) in
      0) refresh=50;;
      1) refresh=1000;;
      *) refresh=2000;;
    esac
    auto_session "$refresh" 1 20
  done
  send_complete "taskmantest stats" "[TASKMANTEST][STATS]" 90 stress
  stats=$(latest_stats)
  sessions=$(stats_value "$stats" auto_exit_sessions)
  fallback=$(stats_value "$stats" fallback_frames)
  shortfalls=$(stats_value "$stats" auto_exit_shortfalls)
  scroll=$(stats_value "$stats" scroll_delta)
  ((sessions == 20 && fallback == 0 && shortfalls == 0 && scroll == 0))
  send_complete "taskmantest cleanup" "[TASKMANTEST][CLEANUP] PASS removed=20" 240 stress
  send_complete "taskmantest check" "[TASKMANTEST][CHECK] PASS" 90 stress
  send_complete "taskdiag check" "[TASKDIAG][CHECK] PASS" 90 stress
  assert_clean_log "$serial"
  cp "$serial" "$artifact"
  echo "[CL13][TASKMAN_SOAK_SMOKE] PASS sessions=20 fallback=0 shortfalls=0 modal_scroll=0" >>"$artifact"
  finish_boot
  finish=$(now_ms)
  record_pass soak-smoke 4 tcg '20 anchored sessions target=1' \
    "$((finish-start))" "$artifact"
  runtime_snapshot "$runtime_after_focused"
  cmp -s "$runtime_before" "$runtime_after_focused"
  {
    echo '[CL13][TASKMAN_TIMEOUT_CLASSIFICATION]'
    echo 'HARNESS_ANCHOR_DEGRADATION_CONFIRMED'
  } >"$artifact_dir/cl13fix2-taskman-timeout-classification.log"
  record_pass classification-final 0 host 'focused variants + soak smoke + continuity' 0 \
    "$artifact_dir/cl13fix2-taskman-timeout-classification.log"
  echo '[CL13][TASKMAN_SOAK_SMOKE] PASS'
  echo '[CL13][TASKMAN_TIMEOUT_CLASSIFICATION] HARNESS_ANCHOR_DEGRADATION_CONFIRMED'
}

static_gate(){
  local start finish artifact=$artifact_dir/cl13fix2-static.log
  require_runtime_unchanged
  start=$(now_ms)
  {
    sha256sum kernel/src/shell/commands/cmd_taskman.c \
      kernel/src/shell/commands/cmd_taskman.h \
      kernel/src/shell/commands/cmd_taskmantest.c \
      kernel/src/shell/commands/cmd_taskmantest.h \
      kernel/src/core/modal_ui.c kernel/src/core/task_metrics.c \
      kernel/src/graphics/console.c scripts/test-cl11.sh
    bash -n scripts/test-cl13-taskman-anchor-focused.sh
    bash -n scripts/test-taskman-v1.sh
    bash -n scripts/test-taskman-v1-soak.sh
    make stack-check
    make kernel-check JOBS=2
    test -z "$(nm -u kernel.elf)"
  } 2>&1 | tee "$artifact"
  finish=$(now_ms)
  record_pass static 0 host 'stack-check + kernel-check j2 + nm' \
    "$((finish-start))" "$artifact"
}

case ${1:-} in
  static) static_gate;;
  negative-anchor) negative_anchor;;
  positive-anchor) positive_anchor;;
  variants) variants;;
  soak-smoke) soak_smoke;;
  all)
    static_gate
    negative_anchor
    positive_anchor
    variants
    soak_smoke
    ;;
  *) usage;;
esac

stop_qemu
trap - EXIT
