#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

serial=.qemu/qemu-serial.log
hmp=(python3 scripts/qemu_hmp.py --socket .qemu/hmp.sock)
mkdir -p artifacts/build
harness_start_line=0
harness_command=boot
source scripts/harness-common.sh

cleanup() { stop_qemu; }
trap cleanup EXIT

value_from_line() {
  local line=$1 key=$2 regex
  regex="(^|[[:space:]])${key}=([^[:space:]]+)($|[[:space:]])"
  [[ $line =~ $regex ]] || return 1
  printf '%s\n' "${BASH_REMATCH[2]}"
}

focused_checks() {
  send_complete "schedtest check" "[SCHED][CHECK] PASS" 60
  send_complete "synctest check" "[SYNC][CHECK] PASS" 60
  send_complete "accounttest check" "[ACCOUNT][CHECK] PASS" 60
  send_complete "killtest check" "[KILLTEST][CHECK] PASS" 60
  send_complete "reaptest check" "[REAPTEST][CHECK] PASS" 60
  send_complete "inputtest check" "[INPUTTEST][CHECK] PASS" 60
  send_complete "modaltest check" "[MODALTEST][CHECK] PASS" 60
  send_complete "taskmantest check" "[TASKMANTEST][CHECK] PASS" 60
  send_complete "taskdiag check" "[TASKDIAG][CHECK] PASS" 60
}

static_gate() {
  local owned_warning
  if rg -n 'Fase 5B|Fase 9|TASKMAN Fase 1|TASKMAN Fase 5A|TMV1-CL' \
      kernel/src/shell; then
    echo "CL12 static: internal phase text remains in shell UX" >&2
    return 1
  fi
  if rg -n 'print_mem_est_field|task_class_to_string|print_cpu_percent_field' \
      kernel/src/shell/commands/cmd_ps.c; then
    echo "CL12 static: duplicated ps formatter remains" >&2
    return 1
  fi
  rg -n 'task_format_snapshot_fields|task_format_memory|task_format_cpu_percent' \
    kernel/src/shell/commands/cmd_ps.c \
    kernel/src/shell/commands/cmd_taskman.c
  rg -n 'task_id_parse_decimal_ex|scheduler_task_kill_result_to_string' \
    kernel/src/core kernel/src/shell/commands/cmd_kill.c
  if rg -n 'Commmand|Descriptions' \
      kernel/src/shell/commands/cmd_help.c; then
    echo "CL12 static: help typo remains" >&2
    return 1
  fi
  rg -n 'taskdiag' kernel/src/shell/commands/registry.c \
    kernel/src/shell/commands/cmd_help.c \
    kernel/src/shell/commands/cmd_taskdiag.c
  if rg -n 'TASK SNAPSHOT \(Fase|Task snapshot \(TASKMAN|termination.*Fase' \
      kernel/src; then
    echo "CL12 static: release UX contains internal phase text" >&2
    return 1
  fi
  bash -n scripts/test-cl12.sh
  git diff --check
  make stack-check JOBS=2
  make kernel-check JOBS=2
  make image
  [[ -z $(nm -u kernel.elf) ]]
  owned_warning=$(grep -E \
    'kernel/src/(core/task_format|shell/commands/(cmd_ps|cmd_kill|cmd_help|cmd_taskman|cmd_taskdiag|registry))\.c:.*warning:' \
    artifacts/build/kernel-check-j2.log || true)
  [[ -z $owned_warning ]]
  echo "[CL12][STATIC] PASS"
}

help_matrix() {
  send_complete "help" "Available Commands:" 60
  local command canonical
  for command in ps tasks tasklist; do
    send_complete "help $command" "Command: ps" 60
  done
  for command in kill terminate taskkill; do
    send_complete "help $command" "Command: kill" 60
  done
  for command in taskman tm top; do
    send_complete "help $command" "Command: taskman" 60
  done
  for command in taskdiag td tdiag; do
    send_complete "help $command" "Command: taskdiag" 60
  done
  send_complete "help unknown" "Command 'unknown' not found." 60
  send_complete "help kill extra" "Usage: help [command]" 60
  grep -Fq "Lists HobbyOS kernel tasks, not POSIX processes." "$serial"
  grep -Fq "Requests cooperative cancellation." "$serial"
  grep -Fq "ESC is the only exit key." "$serial"
  ! grep -Eq 'Commmand|Descriptions|TASKMAN Fase|Fase 5B|TMV1-CL' "$serial"
}

ps_invalid_matrix() {
  local command pattern
  for command in 'ps 0' 'ps -1' 'ps +1' 'ps abc' \
                 'ps 18446744073709551616'; do
    send_complete "$command" "ps: page must be a positive decimal number" 60
  done
  for command in 'ps 1 0' 'ps 1 129'; do
    send_complete "$command" "ps: page_size must be 1..128" 60
  done
  send_complete 'ps 1 32 extra' 'Usage: ps [page] [page_size]' 60
  send_complete 'ps 9999' 'ps: page is outside the task snapshot range' 60
}

smp1_gate() {
  make image >/dev/null
  harness_lock
  start_qemu 1 tcg
  send_complete "taskdiag selftest" "[TASKDIAG][SELFTEST] PASS" 60
  send_complete "taskdiag summary" "[TASKDIAG][SUMMARY] PASS" 60
  send_complete "taskdiag scheduler" "[TASKDIAG][SCHEDULER] PASS" 60
  send_complete "taskdiag reaper" "[TASKDIAG][REAPER] PASS" 60
  send_complete "taskdiag modal" "[TASKDIAG][MODAL] PASS" 60
  send_complete "taskdiag input" "[TASKDIAG][INPUT] PASS" 60
  send_complete "taskdiag accounting" "[TASKDIAG][ACCOUNTING] PASS" 60
  send_complete "taskdiag check" "[TASKDIAG][CHECK] PASS" 60

  help_matrix

  send_complete "ps" "[PS][SNAPSHOT]" 60
  local first second delta
  first=$(grep -F '[PS][WINDOW]' "$serial" | tail -1)
  [[ $(value_from_line "$first" baseline) == 0 ]]
  sleep 1
  send_complete "ps" "[PS][SNAPSHOT]" 60
  second=$(grep -F '[PS][WINDOW]' "$serial" | tail -1)
  [[ $(value_from_line "$second" baseline) == 1 ]]
  delta=$(value_from_line "$second" delta_ns)
  ((delta > 0))

  send_complete "killtest parser" "[KILLTEST][PARSER] PASS" 60
  for command in 'taskdiag task 0' 'taskdiag task -1' \
                 'taskdiag task +1' 'taskdiag task abc' \
                 'taskdiag task 18446744073709551616'; do
    send_complete "$command" "taskdiag: invalid task PID" 60
  done
  ps_invalid_matrix
  focused_checks

  cp "$serial" artifacts/build/cl12-smp1-tcg.log
  grep -E 'Available Commands:|Command: (ps|kill|taskman|taskdiag)|Description:|Usage:|Aliases:|Details:|not POSIX processes|cooperative cancellation|ESC is the only exit key|Command .unknown. not found' \
    "$serial" > artifacts/build/cl12-help-aliases.log
  assert_clean_log artifacts/build/cl12-smp1-tcg.log
  stop_qemu
  harness_unlock
}

run_ps_page() {
  local command=$1 expected_page=$2 expected_size=$3 start line
  local page pages page_size offset written total previous next expected
  start=$(wc -l < "$serial")
  send_complete "$command" "[PS][SNAPSHOT]" 90
  line=$(tail -n +$((start + 1)) "$serial" | grep -F '[PS][SNAPSHOT]' | tail -1)
  page=$(value_from_line "$line" page)
  pages=$(value_from_line "$line" pages)
  page_size=$(value_from_line "$line" page_size)
  offset=$(value_from_line "$line" offset)
  written=$(value_from_line "$line" written)
  total=$(value_from_line "$line" total)
  previous=$(value_from_line "$line" has_previous)
  next=$(value_from_line "$line" has_next)
  ((page == expected_page, page_size == expected_size, total >= 129))
  ((pages == (total + page_size - 1) / page_size))
  ((offset == (page - 1) * page_size))
  expected=$((total - offset))
  ((expected > page_size)) && expected=$page_size
  ((written == expected, previous == (page > 1), next == (page < pages)))
  local ids
  ids=$(tail -n +$((start + 1)) "$serial" | grep -F "[PS][ROW] page=$page " |
        sed -E 's/.* id=([0-9]+).*/\1/')
  [[ $(printf '%s\n' "$ids" | sed '/^$/d' | wc -l) == "$written" ]]
  [[ -z $(printf '%s\n' "$ids" | sed '/^$/d' | sort | uniq -d) ]]
}

compare_task_rows() {
  local ps_line taskman_line key
  ps_line=$(grep -F '[TASKVIEW][ROW] source=PS ' "$serial" | tail -1)
  taskman_line=$(grep -F '[TASKVIEW][ROW] source=TASKMAN ' "$serial" | tail -1)
  [[ -n $ps_line && -n $taskman_line ]]
  for key in id state class runtime cpu kill mem mem_bytes name; do
    [[ $(value_from_line "$ps_line" "$key") == \
       $(value_from_line "$taskman_line" "$key") ]]
  done
  [[ $(value_from_line "$ps_line" state) == ZOMBIE ]]
  [[ $(value_from_line "$ps_line" cpu) == 0.0% ]]
  [[ $(value_from_line "$ps_line" kill) == ZOMB ]]
}

run_kill_case() {
  local command=$1 parse=$2 scheduler=$3 status=$4 message=$5 start line
  start=$(wc -l < "$serial")
  if [[ $command == __EMPTY__ ]]; then
    local before
    harness_command='kill ""'
    harness_start_line=$start
    before=$(count '[KILL][CLI]')
    hmp_text 'kill ' normal >/dev/null
    hmp_key shift-apostrophe normal >/dev/null
    hmp_key shift-apostrophe normal >/dev/null
    hmp_key ret normal >/dev/null
    wait_new '[KILL][CLI]' "$before" 90
    shell_sync
  else
    send_complete "$command" "[KILL][CLI]" 90
  fi
  line=$(tail -n +$((start + 1)) "$serial" | grep -F '[KILL][CLI]' | tail -1)
  [[ $(value_from_line "$line" parse) == "$parse" ]]
  [[ $(value_from_line "$line" scheduler) == "$scheduler" ]]
  [[ $(value_from_line "$line" status) == "$status" ]]
  tail -n +$((start + 1)) "$serial" | grep -Fq "$message"
}

kill_matrix() {
  send_complete "killtest ui-setup" "[KILLTEST][UI_SETUP] PASS" 90
  local setup protected nonkillable killable pending exiting zombie
  setup=$(grep -F '[KILLTEST][UI_SETUP] PASS' "$serial" | tail -1)
  protected=$(value_from_line "$setup" prot)
  nonkillable=$(value_from_line "$setup" no)
  killable=$(value_from_line "$setup" killable)
  pending=$(value_from_line "$setup" pend)
  exiting=$(value_from_line "$setup" exit)
  zombie=$(value_from_line "$setup" zomb)
  send_complete "killtest cli-telemetry on" "[KILLTEST][CLI_TELEMETRY] ON" 60

  run_kill_case 'kill' INVALID_ARGUMENT INVALID 64 'Usage: kill <pid>'
  run_kill_case 'kill 1 2' INVALID_ARGUMENT INVALID 64 'Usage: kill <pid>'
  run_kill_case __EMPTY__ EMPTY INVALID 65 'kill: invalid task PID (EMPTY)'
  run_kill_case 'kill 0' ZERO INVALID 65 'kill: invalid task PID (ZERO)'
  run_kill_case 'kill -1' SIGN INVALID 65 'kill: invalid task PID (SIGN)'
  run_kill_case 'kill +1' SIGN INVALID 65 'kill: invalid task PID (SIGN)'
  run_kill_case 'kill abc' NON_DECIMAL INVALID 65 \
    'kill: invalid task PID (NON_DECIMAL)'
  run_kill_case 'kill 18446744073709551616' OVERFLOW INVALID 65 \
    'kill: invalid task PID (OVERFLOW)'
  run_kill_case 'kill 18446744073709551615' OK NOT_FOUND 68 \
    'was not found'
  run_kill_case "kill $protected" OK PROTECTED 66 'is protected'
  run_kill_case "kill $nonkillable" OK NOT_KILLABLE 67 \
    'is not cooperatively killable'
  run_kill_case "kill $killable" OK ACCEPTED 0 \
    'cooperative cancellation requested'
  run_kill_case "kill $killable" OK ALREADY_PENDING 2 \
    'cancellation is already pending'
  run_kill_case "kill $pending" OK ALREADY_PENDING 2 \
    'cancellation is already pending'
  run_kill_case "kill $exiting" OK ALREADY_EXITING 4 'is already exiting'
  run_kill_case "kill $zombie" OK ALREADY_ZOMBIE 3 'is already a ZOMBIE'

  send_complete "killtest cli-telemetry off" "[KILLTEST][CLI_TELEMETRY] OFF" 60
  send_complete "killtest ui-cleanup" "[KILLTEST][UI_CLEANUP] PASS" 120
}

smp4_gate() {
  make image >/dev/null
  harness_lock
  start_qemu 4 tcg

  send_complete "taskmantest setup 129" "[TASKMANTEST][SETUP] PASS count=129" 180
  run_ps_page 'ps' 1 32
  run_ps_page 'ps 2' 2 32
  run_ps_page 'ps 5 32' 5 32
  run_ps_page 'ps 1 128' 1 128
  grep -E '\[PS\]\[(SNAPSHOT|ROW|WINDOW)\]' "$serial" \
    > artifacts/build/cl12-ps-pagination.log
  send_complete "taskmantest cleanup" "[TASKMANTEST][CLEANUP] PASS removed=129" 180

  send_complete "reaptest zombie-mem setup" \
    "[REAPTEST][ZOMBIE_MEM_SETUP] PASS" 90
  local fixture
  fixture=$(value_from_line \
    "$(grep -F '[REAPTEST][ZOMBIE_MEM_SETUP] PASS' "$serial" | tail -1)" id)
  send_complete "taskdiag trace $fixture" "[TASKDIAG][TRACE] ON id=$fixture" 60
  send_complete "taskdiag task $fixture" "[TASKDIAG][TASK] PASS" 60
  send_complete "ps" "[TASKVIEW][ROW] source=PS id=$fixture" 60
  send_complete "taskmantest anchor-reset" "[TASKMANTEST][ANCHOR_RESET] PASS" 60
  send_complete "taskmantest auto-exit-frames 1" \
    "[TASKMANTEST][AUTO_EXIT_ARM] PASS frames=1" 60
  send_complete "taskman 50" "[TASKMAN][AUTO_EXIT] PASS target=1" 180
  compare_task_rows
  send_complete "taskdiag trace off" "[TASKDIAG][TRACE] OFF" 60
  send_complete "reaptest zombie-mem cleanup" "[REAPTEST][ZOMBIE_MEM] PASS" 90
  grep -E '\[TASKVIEW\]\[ROW\]|\[REAPTEST\]\[ZOMBIE_MEM|\[TASKDIAG\]\[TRACE' \
    "$serial" > artifacts/build/cl12-ps-taskman-compare.log

  kill_matrix
  grep -E '\[KILL\]\[CLI\]|^kill:|^Usage: kill|\[KILLTEST\]\[(UI_SETUP|CLI_TELEMETRY|UI_CLEANUP)' \
    "$serial" > artifacts/build/cl12-kill-cli.log

  send_complete "taskdiag all" "[TASKDIAG][ALL] PASS" 90
  send_complete "taskdiag trace-status" "[TASKDIAG][TRACE_STATUS] OFF" 60
  focused_checks

  cp "$serial" artifacts/build/cl12-smp4-tcg.log
  grep -E '\[TASKDIAG\]\[(SUMMARY|TASK|SCHEDULER|REAPER|MODAL|INPUT|ACCOUNTING|ALL|CHECK|SELFTEST|TRACE|TRACE_STATUS)' \
    "$serial" > artifacts/build/cl12-taskdiag.log
  assert_clean_log artifacts/build/cl12-smp4-tcg.log
  stop_qemu
  harness_unlock
}

final_build() {
  make stack-check JOBS=2
  make kernel-check JOBS=2
  cp kernel.elf /tmp/hobbyos-cl12-j2.elf
  make kernel-check JOBS="$(nproc)"
  cp kernel.elf /tmp/hobbyos-cl12-jN.elf
  sha256sum /tmp/hobbyos-cl12-j2.elf /tmp/hobbyos-cl12-jN.elf |
    tee artifacts/build/cl12-build-hashes.txt
  cmp -s /tmp/hobbyos-cl12-j2.elf /tmp/hobbyos-cl12-jN.elf
  make deps-check
  make image
  [[ -z $(nm -u kernel.elf) ]]
  sha256sum kernel.elf hobbyos.img | tee artifacts/build/cl12-final-hashes.txt
  echo "[CL12][FINAL_BUILD] PASS cmp=0"
}

run_logged() {
  local log=$1
  shift
  "$@" 2>&1 | tee "$log"
}

case "${1:-all}" in
  static)
    run_logged artifacts/build/cl12-static.log static_gate
    ;;
  smp1)
    smp1_gate
    ;;
  smp4)
    smp4_gate
    ;;
  final-build)
    run_logged artifacts/build/cl12-final-build.log final_build
    ;;
  all)
    run_logged artifacts/build/cl12-static.log static_gate
    smp1_gate
    smp4_gate
    run_logged artifacts/build/cl12-final-build.log final_build
    ;;
  *)
    echo "Usage: scripts/test-cl12.sh static|smp1|smp4|final-build|all" >&2
    exit 2
    ;;
esac
