#!/usr/bin/env bash

HARNESS_RUNTIME_READY=${HARNESS_RUNTIME_READY:-0}
HARNESS_RUNTIME_READY_PID=${HARNESS_RUNTIME_READY_PID:-}

runtime_marker_line(){
  local file=$1 regex=$2 first=${3:-1}
  awk -v regex="$regex" -v first="$first" '
    NR >= first {
      line=$0
      sub(/\r$/, "", line)
      if (line ~ regex) { print NR; exit }
    }
  ' "$file"
}

runtime_validate_boot_log(){
  local file=$1 autorun_expected=${2:-1}
  local shell_line runtime_line begin_line pass_line test_line early
  shell_line=$(runtime_marker_line "$file" '^\[BOOT\]\[SHELL_READY\] PASS$')
  runtime_line=$(runtime_marker_line "$file" '^\[BOOT\]\[RUNTIME_READY\] PASS ')
  test_line=$(runtime_marker_line "$file" "^\\[BOOT\\]\\[TEST_READY\\] PASS autorun=${autorun_expected} selftests=[0-9]+$")
  [[ -n $shell_line && -n $runtime_line && -n $test_line &&
     $shell_line -lt $runtime_line && $runtime_line -lt $test_line ]] || return 1
  if [[ $autorun_expected == 1 ]]; then
    begin_line=$(runtime_marker_line "$file" '^\[SELFTEST\]\[AUTORUN\] BEGIN$')
    pass_line=$(runtime_marker_line "$file" '^\[SELFTEST\]\[AUTORUN\] PASS$')
    [[ -n $begin_line && -n $pass_line &&
       $runtime_line -lt $begin_line && $begin_line -lt $pass_line &&
       $pass_line -lt $test_line ]] || return 1
  else
    [[ -z $(runtime_marker_line "$file" '^\[SELFTEST\]\[AUTORUN\] (BEGIN|PASS|FAIL)$') ]] || return 1
  fi
  early=$(runtime_marker_line "$file" '^\[HARNESS\]\[BEGIN\] ' 1)
  [[ -z $early || $early -gt $test_line ]] || return 1
}

runtime_wait_boot_ready(){
  local log=$1 timeout=${2:-180} autorun_expected=${3:-1}
  local deadline=$((SECONDS + timeout)) pid
  HARNESS_RUNTIME_READY=0
  HARNESS_RUNTIME_READY_PID=
  while ((SECONDS <= deadline)); do
    if runtime_validate_boot_log "$log" "$autorun_expected"; then
      [[ -r .qemu/qemu.pid ]] || return 1
      read -r pid <.qemu/qemu.pid
      [[ $pid =~ ^[1-9][0-9]*$ ]] || return 1
      HARNESS_RUNTIME_READY=1
      HARNESS_RUNTIME_READY_PID=$pid
      export HARNESS_RUNTIME_READY HARNESS_RUNTIME_READY_PID
      return 0
    fi
    if [[ -r .qemu/qemu.pid ]]; then
      read -r pid <.qemu/qemu.pid
      kill -0 "$pid" 2>/dev/null || return 1
    fi
    sleep 1
  done
  echo "RUNTIME_READY_TIMEOUT autorun=$autorun_expected log=$log" >&2
  return 1
}
