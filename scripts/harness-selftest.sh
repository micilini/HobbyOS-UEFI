#!/usr/bin/env bash

# Shared strict SELFTEST autorun gate for CL-13.  This file is sourced.

CL13_SELFTEST_REQUIRED_CASES=(
  scheduler.async.completed_before_marker_contract
  sync.semaphore.observer.atomic_config
  ui.taskman.fixture_capacity_257
  transport.crc.known_vector
  registry.selftest_records_fit
  runtime.ready.test_gate
  transport.harness_records_line_fenced
  sync.async.completed_before_marker_contract
  accounting.uptime.wrapper_interval
)

selftest_extract_autorun(){
  local serial_log=$1 output_log=$2
  awk '
    {
      line=$0
      sub(/\r$/, "", line)
    }
    line == "[SELFTEST][AUTORUN] BEGIN" {
      if (!capture) {
        capture=1
        began=1
      }
    }
    capture { print line }
    capture && (line == "[SELFTEST][AUTORUN] PASS" ||
                line == "[SELFTEST][AUTORUN] FAIL") {
      ended=1
      exit
    }
    END {
      if (!began || !ended)
        exit 4
    }
  ' "$serial_log" >"$output_log"
}

selftest_validate_json(){
  local json=$1
  shift
  python3 - "$json" "$@" <<'PY'
import json
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
required = sys.argv[2:]
data = json.loads(path.read_text())
numeric_zero = ("fail", "skip", "p0_fail", "p1_fail", "p2_fail",
                "framing_repairs", "line_boundary_violations")
if data.get("autorun") != "PASS":
    raise SystemExit("SELFTEST JSON autorun is not PASS")
for key in numeric_zero:
    if data.get(key) != 0:
        raise SystemExit(f"SELFTEST JSON {key}={data.get(key)}")
case_count = data.get("case_count")
if not isinstance(case_count, int) or case_count <= 0:
    raise SystemExit("SELFTEST JSON case_count is invalid")
if case_count != data.get("pass", 0) + data.get("fail", 0) + data.get("skip", 0):
    raise SystemExit("SELFTEST JSON case_count does not match summary")
if case_count != data.get("pass"):
    raise SystemExit("SELFTEST JSON case_count does not equal pass")
cases = {case.get("name"): case for case in data.get("cases", [])}
for name in required:
    case = cases.get(name)
    if case is None:
        raise SystemExit(f"SELFTEST JSON missing required case: {name}")
    if case.get("status") != "PASS":
        raise SystemExit(f"SELFTEST JSON required case not PASS: {name}")
print(data["pass"], data["framing_repairs"],
      data["line_boundary_violations"], case_count)
PY
}

selftest_validate_autorun(){
  local serial_log=$1 artifact_prefix=$2
  shift 2
  local selftest_log="${artifact_prefix}-selftest.log"
  local json_log="${artifact_prefix}-selftest.json"
  local markdown_log="${artifact_prefix}-selftest.md"
  local required=()
  local case parser_output summary

  required+=("${CL13_SELFTEST_REQUIRED_CASES[@]}")
  required+=("$@")
  selftest_extract_autorun "$serial_log" "$selftest_log"

  local parser_args=(
    --log "$selftest_log"
    --json "$json_log"
    --markdown "$markdown_log"
    --require-clean-framing
  )
  for case in "${required[@]}"; do
    parser_args+=(--require-case "$case")
  done
  parser_output=$(python3 scripts/parse-selftest-log.py "${parser_args[@]}")
  summary=$(selftest_validate_json "$json_log" "${required[@]}")
  read -r SELFTEST_GATE_PASS SELFTEST_GATE_REPAIRS \
    SELFTEST_GATE_BOUNDARY_VIOLATIONS SELFTEST_GATE_CASE_COUNT <<<"$summary"
  SELFTEST_GATE_REQUIRED=${#required[@]}
  SELFTEST_GATE_LOG=$selftest_log
  SELFTEST_GATE_JSON=$json_log
  SELFTEST_GATE_MARKDOWN=$markdown_log
  printf '%s\n' "$parser_output"
  printf '[CL13][SELFTEST_GATE] PASS pass=%s repairs=%s line_boundary_violations=%s required=%s case_count=%s\n' \
    "$SELFTEST_GATE_PASS" "$SELFTEST_GATE_REPAIRS" \
    "$SELFTEST_GATE_BOUNDARY_VIOLATIONS" \
    "$SELFTEST_GATE_REQUIRED" "$SELFTEST_GATE_CASE_COUNT"
}

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
  echo "source scripts/harness-selftest.sh; do not execute it directly" >&2
  exit 2
fi
