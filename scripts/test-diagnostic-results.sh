#!/usr/bin/env bash
set -Eeuo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

base=380a8c9fe796a7e3714162a14740701a13800652
artifact=artifacts/build/diagnostic-results
mkdir -p "$artifact"

required_selftest_markers=(
  '[SHELLRESULTTEST][SYNC_PASS] PASS'
  '[SHELLRESULTTEST][SYNC_FAIL] PASS'
  '[SHELLRESULTTEST][ASYNC_RUNNING] PASS'
  '[SHELLRESULTTEST][ASYNC_PASS] PASS'
  '[SHELLRESULTTEST][ASYNC_FAIL] PASS'
  '[SHELLRESULTTEST][MODAL_DEFER] PASS'
  '[SHELLRESULTTEST][PROMPT_PRESERVE] PASS'
  '[SHELLRESULTTEST][BOUNDED] PASS'
  '[SHELLRESULTTEST][ALL] PASS'
)

serial_files=(
  kernel/src/shell/commands/cmd_accounttest.c
  kernel/src/shell/commands/cmd_taskmantest.c
  kernel/src/shell/commands/cmd_inputtest.c
  kernel/src/shell/commands/cmd_modaltest.c
  kernel/src/shell/commands/cmd_synctest.c
  kernel/src/shell/commands/cmd_reaptest.c
  kernel/src/shell/commands/cmd_killtest.c
  kernel/src/shell/commands/cmd_schedtest.c
)

require_marker()
{
  local log=$1 marker=$2
  grep -Fq -- "$marker" "$log" || {
    printf 'missing marker: %s\n' "$marker" >&2
    return 1
  }
}

verify_faults()
{
  local log=$1
  ! grep -E 'PANIC|FATAL|#PF|#GP|DOUBLE FAULT|TRIPLE FAULT|STRUCTURAL_FAULT|FINISH_FAULT' "$log"
}

verify_serial_literals()
{
  local temporary file before after
  temporary=$(mktemp -d --tmpdir hobbyos-diagnostic-serial.XXXXXX)
  for file in "${serial_files[@]}"; do
    before=$temporary/before
    after=$temporary/after
    git show "$base:$file" | rg -o 'serial_write_all\([^;]*' | sort >"$before"
    rg -o 'serial_write_all\([^;]*' "$file" | sort >"$after"
    cmp -s "$before" "$after" || {
      printf 'serial literal drift: %s\n' "$file" >&2
      diff -u "$before" "$after" >&2 || true
      rm -rf "$temporary"
      return 1
    }
  done
  rm -rf "$temporary"
}

static_gate()
{
  local naming_pattern
  bash -n scripts/test-diagnostic-results.sh
  git diff --check
  naming_pattern='T''MV1|U''X01|F''IX[0-9]*|F''ASE|P''HASE'
  ! rg -n "$naming_pattern" \
    kernel/src/shell/diagnostic_result.c \
    kernel/src/shell/diagnostic_result.h \
    scripts/test-diagnostic-results.sh \
    docs/diagnostic-result-output.md
  ! rg -n '(^|[^[:alnum:]_])(malloc|kmalloc)[[:space:]]*\(' \
    kernel/src/shell/diagnostic_result.c \
    kernel/src/shell/diagnostic_result.h \
    kernel/src/shell/shell.c kernel/src/shell/shell.h
  rg -q 'return return_code;' kernel/src/shell/diagnostic_result.c
  rg -q 'DIAGNOSTIC_RESULT_RUNNING' kernel/src/shell/diagnostic_result.c
  rg -q 'shell_status_complete\(generation, line\)' \
    kernel/src/shell/diagnostic_result.c
  rg -q 'diagnostic_result_async_started' \
    kernel/src/shell/commands/cmd_synctest.c
  rg -q 'return print_stats\("check"\) \? 0 : 1;' \
    kernel/src/shell/commands/cmd_schedtest.c
  verify_serial_literals
  printf '[SHELLRESULT][STATIC] PASS\n' | tee "$artifact/static.log"
}

verify_selftest_log()
{
  local log=$1 marker
  [[ -s $log ]]
  for marker in "${required_selftest_markers[@]}"; do
    require_marker "$log" "$marker"
  done
  verify_faults "$log"
  printf '[SHELLRESULT][SELFTEST_LOG] PASS log=%s\n' "$log"
}

verify_smp4_log()
{
  local log=$1 marker
  [[ -s $log ]]
  for marker in \
    '[BOOT][RUNTIME_READY] PASS cpus=4/4' \
    '[ACCOUNT][CHECK] PASS' \
    '[TASKMANTEST][CHECK] PASS' \
    '[INPUTTEST][CHECK] PASS' \
    '[MODALTEST][CHECK] PASS' \
    '[SYNC][CHECK] PASS' \
    '[SYNC][TIMER_ORDER] PASS' \
    '[SYNC][TIMER_CANCEL] PASS' \
    '[REAPTEST][TIMER_REF] PASS' \
    '[REAPTEST][CHECK] PASS' \
    '[KILLTEST][CHECK] PASS' \
    '[SCHED][CHECK] PASS'; do
    require_marker "$log" "$marker"
  done
  verify_faults "$log"
  printf '[SHELLRESULT][SMP4_LOG] PASS log=%s\n' "$log"
}

verify_smp24_log()
{
  local log=$1 marker
  [[ -s $log ]]
  for marker in \
    '[IRQ][CPU_READY_SUMMARY] expected=24 ready=24 failed=0 waiting=0' \
    '[BOOT][RUNTIME_READY] PASS cpus=24/24' \
    '[ACCOUNT][CHECK] PASS' \
    '[TASKMANTEST][CHECK] PASS' \
    '[INPUTTEST][CHECK] PASS' \
    '[MODALTEST][CHECK] PASS' \
    '[SYNC][CHECK] PASS' \
    '[SYNC][TIMER_ORDER] PASS' \
    '[SYNC][TIMER_CANCEL] PASS' \
    '[REAPTEST][TIMER_REF] PASS' \
    '[IRQ][CHECK] PASS' \
    '[TASKDIAG][CHECK] PASS'; do
    require_marker "$log" "$marker"
  done
  verify_faults "$log"
  printf '[SHELLRESULT][SMP24_LOG] PASS log=%s\n' "$log"
}

usage()
{
  echo 'usage: scripts/test-diagnostic-results.sh static|selftest-log LOG|smp4-log LOG|smp24-log LOG|all' >&2
  exit 2
}

case ${1:-} in
  static) static_gate ;;
  selftest-log) [[ $# == 2 ]] || usage; verify_selftest_log "$2" ;;
  smp4-log) [[ $# == 2 ]] || usage; verify_smp4_log "$2" ;;
  smp24-log) [[ $# == 2 ]] || usage; verify_smp24_log "$2" ;;
  all)
    [[ -n ${SELFTEST_LOG:-} && -n ${SMP4_LOG:-} && -n ${SMP24_LOG:-} ]] || usage
    static_gate
    verify_selftest_log "$SELFTEST_LOG"
    verify_smp4_log "$SMP4_LOG"
    verify_smp24_log "$SMP24_LOG"
    printf '[SHELLRESULT][ALL] PASS\n'
    ;;
  *) usage ;;
esac
