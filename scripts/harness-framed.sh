#!/usr/bin/env bash

# Source after scripts/harness-common.sh.  This file intentionally replaces
# shell-line helpers only; real modal navigation remains raw HMP input.

HARNESS_FRAME_QEMU_PID=${HARNESS_FRAME_QEMU_PID:-}
HARNESS_FRAME_SEQUENCE=${HARNESS_FRAME_SEQUENCE:-0}
HARNESS_FRAME_MAX_ATTEMPTS=${HARNESS_FRAME_MAX_ATTEMPTS:-3}
HARNESS_FRAME_LAST_CLASSIFICATION=
HARNESS_FRAME_LAST_SEQUENCE=0
HARNESS_FRAME_LAST_STATUS=
HARNESS_FRAME_LAST_DIAGNOSTIC_MARKER=0
HARNESS_ASYNC_LAST_RUN=0
HARNESS_ASYNC_LAST_ORDER=
HARNESS_ASYNC_LAST_START_LINE=0
HARNESS_ASYNC_LAST_END_LINE=0
HARNESS_ASYNC_LAST_COMPLETION_LINE=0

framed_crc32(){
  python3 -c 'import sys,zlib; print(f"{zlib.crc32(sys.argv[1].encode()) & 0xffffffff:08x}")' "$1"
}

framed_boot_sync(){
  local pid
  [[ -r .qemu/qemu.pid ]] || {
    echo "HMP_FRAME_TRANSPORT_FAILURE: missing .qemu/qemu.pid" >&2
    return 1
  }
  read -r pid <.qemu/qemu.pid
  [[ $pid =~ ^[1-9][0-9]*$ ]] || {
    echo "HMP_FRAME_TRANSPORT_FAILURE: invalid QEMU pid" >&2
    return 1
  }
  if [[ $pid != "$HARNESS_FRAME_QEMU_PID" ]]; then
    HARNESS_FRAME_QEMU_PID=$pid
    HARNESS_FRAME_SEQUENCE=0
    HARNESS_FRAME_LAST_SEQUENCE=0
    HARNESS_FRAME_LAST_STATUS=
  fi
  if [[ ${HARNESS_RUNTIME_READY:-0} != 1 ||
        ${HARNESS_RUNTIME_READY_PID:-} != "$pid" ]]; then
    HARNESS_FRAME_LAST_CLASSIFICATION=FRAME_BEFORE_TEST_READY
    echo "[HMP][FRAME] FRAME_BEFORE_TEST_READY pid=$pid" >&2
    return 1
  fi
}

framed_exact_record_line(){
  local file=$1 regex=$2 first=${3:-1}
  awk -v regex="$regex" -v first="$first" '
    NR >= first {
      line=$0
      sub(/\r$/, "", line)
      if (line ~ regex) { print NR; exit }
    }
  ' "$file"
}

framed_exact_record_text(){
  local file=$1 regex=$2 first=${3:-1}
  awk -v regex="$regex" -v first="$first" '
    NR >= first {
      line=$0
      sub(/\r$/, "", line)
      if (line ~ regex) { print line; exit }
    }
  ' "$file"
}

framed_exact_record_count(){
  local file=$1 regex=$2 first=${3:-1}
  awk -v regex="$regex" -v first="$first" '
    NR >= first {
      line=$0
      sub(/\r$/, "", line)
      if (line ~ regex) count++
    }
    END { print count + 0 }
  ' "$file"
}

framed_prefix_record_text(){
  local file=$1 prefix=$2 first=${3:-1}
  awk -v prefix="$prefix" -v first="$first" '
    NR >= first {
      line=$0
      sub(/\r$/, "", line)
      if (index(line, prefix) == 1) { print line; exit }
    }
  ' "$file"
}

framed_validate_payload(){
  local payload=$1
  [[ -n $payload && ${#payload} -le 96 ]] || {
    echo "invalid framed payload length: ${#payload}" >&2
    return 1
  }
  [[ $payload != ' '* && $payload != *' ' && $payload != *'  '* ]] || {
    echo "framed payload is not canonically space-normalized: $payload" >&2
    return 1
  }
  [[ $payload != *$'\n'* && $payload != *$'\r'* &&
     $payload != *$'\t'* && $payload != *\"* && $payload != *\'* ]] || {
    echo "framed payload contains unsupported quoting or whitespace" >&2
    return 1
  }
  [[ $payload =~ ^[a-z0-9.+\ -]+$ ]] || {
    echo "framed payload contains a keymap-unsupported character: $payload" >&2
    return 1
  }
  [[ $payload != 'tasktest exec' && $payload != 'tasktest exec '* ]] || {
    echo "recursive tasktest exec payload rejected by host" >&2
    return 1
  }
}

framed_make_frame(){
  local payload=$1 sequence=$2 crc=${3:-} frame
  framed_validate_payload "$payload" || return 1
  [[ $sequence =~ ^[1-9][0-9]*$ ]] || return 1
  [[ -n $crc ]] || crc=$(framed_crc32 "$payload")
  [[ $crc =~ ^[0-9a-f]{8}$ ]] || return 1
  frame="tasktest exec $sequence $crc $payload"
  ((${#frame} < 256)) || {
    echo "framed command exceeds the shell line buffer" >&2
    return 1
  }
  printf '%s' "$frame"
}

framed_status_allowed(){
  local actual=$1 allowed=${2:-0} item
  allowed=${allowed//,/ }
  for item in $allowed; do
    [[ $actual == "$item" ]] && return 0
  done
  return 1
}

framed_qemu_alive(){
  [[ -n $HARNESS_FRAME_QEMU_PID ]] &&
    kill -0 "$HARNESS_FRAME_QEMU_PID" 2>/dev/null
}

framed_flush_partial_line(){
  hmp_text "" sync --enter >/dev/null
  hmp_text "" sync --enter >/dev/null
  hmp_text "" sync --enter >/dev/null
  sleep 1
}

framed_fresh_log(){
  tail -n +$((harness_start_line + 1)) "$serial" 2>/dev/null || true
}

framed_classify_failure(){
  HARNESS_FRAME_LAST_CLASSIFICATION=$1
  echo "[HMP][FRAME] $1 command=$2 seq=$3" >&2
  harness_diagnose "$2" "${4:-transactional completion}" "$1"
  return 1
}

framed_send_transaction(){
  local payload=$1 expected=$2 timeout=${3:-180} profile=${4:-normal}
  local allowed=${5:-0} sequence crc frame attempt attempt_deadline deadline
  local marker_required=${6:-1} status_authority=${7:-0}
  local fresh reject_seen=0 reject_before accepted=0 began=0 replay=0 status= marker_ok=0
  local first_record_line end_record replay_record

  framed_boot_sync || return 1
  framed_validate_payload "$payload" || return 1
  sequence=$((HARNESS_FRAME_SEQUENCE + 1))
  crc=$(framed_crc32 "$payload")
  frame=$(framed_make_frame "$payload" "$sequence" "$crc") || return 1

  harness_command=$payload
  harness_start_line=$(wc -l <"$serial" 2>/dev/null || echo 0)
  first_record_line=$((harness_start_line + 1))
  deadline=$((SECONDS + timeout))
  HARNESS_FRAME_LAST_CLASSIFICATION=
  HARNESS_FRAME_LAST_SEQUENCE=$sequence
  HARNESS_FRAME_LAST_STATUS=

  for ((attempt=1; attempt<=HARNESS_FRAME_MAX_ATTEMPTS; attempt++)); do
    if ((attempt > 1)); then
      framed_flush_partial_line || return 1
    fi
    reject_before=$(framed_exact_record_count "$serial" \
      "^\\[HARNESS\\]\\[FRAME\\] REJECT seq=$sequence .+$" 1)
    hmp_text "$frame" "$profile" --enter >/dev/null || {
      framed_qemu_alive || framed_classify_failure TASKMAN_GUEST_FAULT \
        "$payload" "$sequence" "$expected"
      continue
    }

    attempt_deadline=$((SECONDS + 30))
    ((attempt_deadline > deadline)) && attempt_deadline=$deadline
    while ((SECONDS <= deadline)); do
      fresh=$(framed_fresh_log)
      [[ -n $(framed_exact_record_line "$serial" \
        "^\\[HARNESS\\]\\[FRAME\\] ACCEPT seq=$sequence crc=[0-9a-f]+ len=[0-9]+$" \
        "$first_record_line") ]] && accepted=1
      [[ -n $(framed_exact_record_line "$serial" \
        "^\\[HARNESS\\]\\[BEGIN\\] seq=$sequence$" \
        "$first_record_line") ]] && began=1
      if [[ -n $expected ]] && grep -Fq "$expected" <<<"$fresh"; then
        marker_ok=1
      fi

      end_record=$(framed_exact_record_text "$serial" \
        "^\\[HARNESS\\]\\[END\\] seq=$sequence status=-?[0-9]+$" \
        "$first_record_line")
      if [[ $end_record =~ ^\[HARNESS\]\[END\][[:space:]]seq=$sequence[[:space:]]status=(-?[0-9]+)$ ]]; then
        status=${BASH_REMATCH[1]}
        [[ -n $(framed_exact_record_line "$serial" \
          "^\\[HARNESS\\]\\[FRAME\\] ACCEPT seq=$sequence crc=[0-9a-f]+ len=[0-9]+$" \
          "$first_record_line") ]] && accepted=1
        [[ -n $(framed_exact_record_line "$serial" \
          "^\\[HARNESS\\]\\[BEGIN\\] seq=$sequence$" \
          "$first_record_line") ]] && began=1
        fresh=$(framed_fresh_log)
        if [[ -n $expected ]] && grep -Fq "$expected" <<<"$fresh"; then
          marker_ok=1
        fi
        HARNESS_FRAME_LAST_STATUS=$status
        ((accepted && began)) ||
          framed_classify_failure GUEST_COMMAND_CONTRACT_FAILURE \
            "$payload" "$sequence" "$expected" || return 1
        framed_status_allowed "$status" "$allowed" ||
          framed_classify_failure GUEST_COMMAND_FAILURE \
            "$payload" "$sequence" "$expected" || return 1
        ((!marker_required || marker_ok)) ||
          framed_classify_failure GUEST_COMMAND_CONTRACT_FAILURE \
            "$payload" "$sequence" "$expected" || return 1
        if ((status_authority)); then
          framed_qemu_alive ||
            framed_classify_failure TASKMAN_GUEST_FAULT \
              "$payload" "$sequence" "$expected" || return 1
          HARNESS_FRAME_LAST_DIAGNOSTIC_MARKER=$marker_ok
          local authority_record="[HMP][FRAME] STATUS_AUTHORITY_PASS command=$payload seq=$sequence status=$status diagnostic_marker=$marker_ok"
          echo "$authority_record"
          [[ -z ${HARNESS_STATUS_RECORD_LOG:-} ]] ||
            echo "$authority_record" >>"$HARNESS_STATUS_RECORD_LOG"
        fi
        HARNESS_FRAME_SEQUENCE=$sequence
        return 0
      fi

      replay_record=$(framed_exact_record_text "$serial" \
        "^\\[HARNESS\\]\\[REPLAY\\] seq=$sequence status=-?[0-9]+$" \
        "$first_record_line")
      if [[ $replay_record =~ ^\[HARNESS\]\[REPLAY\][[:space:]]seq=$sequence[[:space:]]status=(-?[0-9]+)$ ]]; then
        replay=1
        status=${BASH_REMATCH[1]}
        HARNESS_FRAME_LAST_STATUS=$status
        framed_status_allowed "$status" "$allowed" ||
          framed_classify_failure GUEST_COMMAND_FAILURE \
            "$payload" "$sequence" "$expected" || return 1
        ((!marker_required || marker_ok)) ||
          framed_classify_failure GUEST_COMMAND_CONTRACT_FAILURE \
            "$payload" "$sequence" "$expected" || return 1
        if ((status_authority)); then
          framed_qemu_alive ||
            framed_classify_failure TASKMAN_GUEST_FAULT \
              "$payload" "$sequence" "$expected" || return 1
          HARNESS_FRAME_LAST_DIAGNOSTIC_MARKER=$marker_ok
          local authority_record="[HMP][FRAME] STATUS_AUTHORITY_PASS command=$payload seq=$sequence status=$status diagnostic_marker=$marker_ok"
          echo "$authority_record"
          [[ -z ${HARNESS_STATUS_RECORD_LOG:-} ]] ||
            echo "$authority_record" >>"$HARNESS_STATUS_RECORD_LOG"
        fi
        HARNESS_FRAME_SEQUENCE=$sequence
        HARNESS_FRAME_LAST_CLASSIFICATION=HMP_FRAME_REPLAY_RECOVERED
        echo "[HMP][FRAME] HMP_FRAME_REPLAY_RECOVERED command=$payload seq=$sequence"
        return 0
      fi

      if (( $(framed_exact_record_count "$serial" \
        "^\\[HARNESS\\]\\[FRAME\\] REJECT seq=$sequence .+$" 1) > reject_before )); then
        reject_seen=1
        break
      fi
      if grep -Eq 'PANIC|#PF|#GP|FATAL|STRUCTURAL_FAULT|FINISH_FAULT|QEMU exited' <<<"$fresh"; then
        framed_classify_failure TASKMAN_GUEST_FAULT \
          "$payload" "$sequence" "$expected"
        return 1
      fi
      framed_qemu_alive || {
        framed_classify_failure TASKMAN_GUEST_FAULT \
          "$payload" "$sequence" "$expected"
        return 1
      }
      if ((began)); then
        sleep 1
        continue
      fi
      ((SECONDS >= attempt_deadline)) && break
      sleep 1
    done

    if ((began)); then
      framed_classify_failure GUEST_COMMAND_STALL \
        "$payload" "$sequence" "$expected"
      return 1
    fi
    ((SECONDS <= deadline)) || break
  done

  if ((reject_seen)); then
    framed_classify_failure HMP_FRAME_CORRUPTION_PERSISTENT \
      "$payload" "$sequence" "$expected"
  else
    framed_classify_failure HMP_FRAME_TRANSPORT_FAILURE \
      "$payload" "$sequence" "$expected"
  fi
}

# SYNC_RECORD: the structured functional marker is part of the command gate.
framed_send_complete(){
  framed_send_transaction "$1" "$2" "${3:-180}" "${4:-normal}" \
    "${5:-0}" 1 0
}

# SYNC_STATUS: the exact framed handler status is authoritative.  A human
# diagnostic marker is sampled when supplied, but serial interleaving cannot
# turn a successful handler return into a second, contradictory oracle.
framed_send_status_complete(){
  local payload=$1 timeout=${2:-180} profile=${3:-normal}
  local allowed=${4:-0} diagnostic_marker=${5:-}
  framed_send_transaction "$payload" "$diagnostic_marker" "$timeout" \
    "$profile" "$allowed" 0 1
}

framed_replay_last(){
  local payload=$1 expected_status=${2:-0} timeout=${3:-60} profile=${4:-normal}
  local crc frame before deadline fresh replay_record
  framed_boot_sync || return 1
  ((HARNESS_FRAME_SEQUENCE > 0)) || return 1
  crc=$(framed_crc32 "$payload")
  frame=$(framed_make_frame "$payload" "$HARNESS_FRAME_SEQUENCE" "$crc") || return 1
  before=$(wc -l <"$serial" 2>/dev/null || echo 0)
  hmp_text "$frame" "$profile" --enter >/dev/null
  deadline=$((SECONDS + timeout))
  while ((SECONDS <= deadline)); do
    fresh=$(tail -n +$((before + 1)) "$serial" 2>/dev/null || true)
    replay_record=$(framed_exact_record_text "$serial" \
      "^\\[HARNESS\\]\\[REPLAY\\] seq=$HARNESS_FRAME_SEQUENCE status=-?[0-9]+$" \
      "$((before + 1))")
    if [[ $replay_record =~ ^\[HARNESS\]\[REPLAY\][[:space:]]seq=$HARNESS_FRAME_SEQUENCE[[:space:]]status=(-?[0-9]+)$ ]]; then
      [[ ${BASH_REMATCH[1]} == "$expected_status" ]]
      return
    fi
    [[ -z $(framed_exact_record_line "$serial" \
      "^\\[HARNESS\\]\\[BEGIN\\] seq=$HARNESS_FRAME_SEQUENCE$" \
      "$((before + 1))") ]] || {
      echo "duplicate execution detected for seq=$HARNESS_FRAME_SEQUENCE" >&2
      return 1
    }
    framed_qemu_alive || return 1
    sleep 1
  done
  return 1
}

framed_start_command(){
  local payload=$1 profile=${2:-normal} timeout=${3:-60}
  local sequence crc frame attempt deadline fresh reject_before
  framed_boot_sync || return 1
  framed_validate_payload "$payload" || return 1
  sequence=$((HARNESS_FRAME_SEQUENCE + 1))
  crc=$(framed_crc32 "$payload")
  frame=$(framed_make_frame "$payload" "$sequence" "$crc") || return 1
  harness_command=$payload
  harness_start_line=$(wc -l <"$serial" 2>/dev/null || echo 0)
  HARNESS_FRAME_LAST_SEQUENCE=$sequence
  for ((attempt=1; attempt<=HARNESS_FRAME_MAX_ATTEMPTS; attempt++)); do
    ((attempt == 1)) || framed_flush_partial_line
    reject_before=$(framed_exact_record_count "$serial" \
      "^\\[HARNESS\\]\\[FRAME\\] REJECT seq=$sequence .+$" 1)
    hmp_text "$frame" "$profile" --enter >/dev/null
    deadline=$((SECONDS + timeout))
    while ((SECONDS <= deadline)); do
      fresh=$(framed_fresh_log)
      if [[ -n $(framed_exact_record_line "$serial" \
           "^\\[HARNESS\\]\\[FRAME\\] ACCEPT seq=$sequence crc=[0-9a-f]+ len=[0-9]+$" \
           "$((harness_start_line + 1))") &&
            -n $(framed_exact_record_line "$serial" \
           "^\\[HARNESS\\]\\[BEGIN\\] seq=$sequence$" \
           "$((harness_start_line + 1))") ]]; then
        return 0
      fi
      if (( $(framed_exact_record_count "$serial" \
        "^\\[HARNESS\\]\\[FRAME\\] REJECT seq=$sequence .+$" 1) > reject_before )); then
        break
      fi
      framed_qemu_alive || return 1
      sleep 1
    done
  done
  framed_classify_failure HMP_FRAME_TRANSPORT_FAILURE \
    "$payload" "$sequence" "[HARNESS][BEGIN] seq=$sequence"
}

framed_finish_started(){
  local expected=${1:-} timeout=${2:-180} allowed=${3:-0}
  local sequence=$HARNESS_FRAME_LAST_SEQUENCE deadline fresh status end_record
  ((sequence == HARNESS_FRAME_SEQUENCE + 1)) || return 1
  deadline=$((SECONDS + timeout))
  while ((SECONDS <= deadline)); do
    fresh=$(framed_fresh_log)
    end_record=$(framed_exact_record_text "$serial" \
      "^\\[HARNESS\\]\\[END\\] seq=$sequence status=-?[0-9]+$" \
      "$((harness_start_line + 1))")
    if [[ $end_record =~ ^\[HARNESS\]\[END\][[:space:]]seq=$sequence[[:space:]]status=(-?[0-9]+)$ ]]; then
      status=${BASH_REMATCH[1]}
      framed_status_allowed "$status" "$allowed" || return 1
      [[ -z $expected ]] || grep -Fq "$expected" <<<"$fresh" || return 1
      HARNESS_FRAME_SEQUENCE=$sequence
      HARNESS_FRAME_LAST_STATUS=$status
      return 0
    fi
    framed_qemu_alive || return 1
    sleep 1
  done
  echo "[HMP][FRAME] GUEST_COMMAND_STALL command=$harness_command seq=$sequence" >&2
  return 1
}

framed_marker_line(){
  local file=$1 marker=$2 first_line=${3:-1}
  awk -v marker="$marker" -v first="$first_line" \
    'NR >= first && index($0, marker) { print NR; exit }' "$file"
}

# Pure log-order classifier used by the host-only contract tests.  SYNC keeps
# the strict functional-marker-before-END rule; ASYNC accepts completion on
# either side of END, but always after START.
framed_evaluate_log_contract(){
  local contract=$1 file=$2 start_pattern=$3 completion_pattern=$4
  local first_line=${5:-1} start_line end_line completion_line
  HARNESS_FRAME_LAST_CLASSIFICATION=
  HARNESS_ASYNC_LAST_ORDER=
  start_line=$(framed_marker_line "$file" "$start_pattern" "$first_line")
  end_line=$(framed_exact_record_line "$file" \
    '^\[HARNESS\]\[END\] seq=[1-9][0-9]* status=-?[0-9]+$' "$first_line")
  completion_line=$(framed_marker_line "$file" "$completion_pattern" "$first_line")
  if [[ $contract == SYNC ]]; then
    if [[ -n $completion_line && -n $end_line && $completion_line -lt $end_line ]]; then
      return 0
    fi
    HARNESS_FRAME_LAST_CLASSIFICATION=GUEST_COMMAND_CONTRACT_FAILURE
    return 1
  fi
  if [[ $contract != ASYNC || -z $start_line || -z $end_line ||
        $start_line -ge $end_line ]]; then
    HARNESS_FRAME_LAST_CLASSIFICATION=GUEST_COMMAND_CONTRACT_FAILURE
    return 1
  fi
  if [[ -z $completion_line || $completion_line -le $start_line ]]; then
    HARNESS_FRAME_LAST_CLASSIFICATION=GUEST_ASYNC_COMPLETION_TIMEOUT
    return 1
  fi
  if ((completion_line < end_line)); then
    HARNESS_ASYNC_LAST_ORDER=COMPLETE_BEFORE_END
  else
    HARNESS_ASYNC_LAST_ORDER=END_BEFORE_COMPLETE
  fi
  return 0
}

# Pure classifier for synthetic host-unit fixtures.  END requires an accepted
# execution and BEGIN.  REPLAY is a cached completion and therefore must not
# contain a new BEGIN in the inspected suffix.
framed_evaluate_status_log_contract(){
  local file=$1 sequence=$2 allowed=${3:-0} diagnostic_marker=${4:-}
  local first_line=${5:-1} accepted begin end replay status
  HARNESS_FRAME_LAST_CLASSIFICATION=
  HARNESS_FRAME_LAST_STATUS=
  HARNESS_FRAME_LAST_DIAGNOSTIC_MARKER=0
  accepted=$(framed_exact_record_line "$file" \
    "^\\[HARNESS\\]\\[FRAME\\] ACCEPT seq=$sequence crc=[0-9a-f]+ len=[0-9]+$" \
    "$first_line")
  begin=$(framed_exact_record_line "$file" \
    "^\\[HARNESS\\]\\[BEGIN\\] seq=$sequence$" "$first_line")
  end=$(framed_exact_record_text "$file" \
    "^\\[HARNESS\\]\\[END\\] seq=$sequence status=-?[0-9]+$" "$first_line")
  replay=$(framed_exact_record_text "$file" \
    "^\\[HARNESS\\]\\[REPLAY\\] seq=$sequence status=-?[0-9]+$" "$first_line")
  if [[ -n $diagnostic_marker ]] && grep -Fq "$diagnostic_marker" "$file"; then
    HARNESS_FRAME_LAST_DIAGNOSTIC_MARKER=1
  fi

  if [[ $end =~ ^\[HARNESS\]\[END\][[:space:]]seq=$sequence[[:space:]]status=(-?[0-9]+)$ ]]; then
    status=${BASH_REMATCH[1]}
    [[ -n $accepted && -n $begin ]] || {
      HARNESS_FRAME_LAST_CLASSIFICATION=GUEST_COMMAND_CONTRACT_FAILURE
      return 1
    }
  elif [[ $replay =~ ^\[HARNESS\]\[REPLAY\][[:space:]]seq=$sequence[[:space:]]status=(-?[0-9]+)$ ]]; then
    status=${BASH_REMATCH[1]}
    [[ -z $begin ]] || {
      HARNESS_FRAME_LAST_CLASSIFICATION=GUEST_COMMAND_CONTRACT_FAILURE
      return 1
    }
  else
    HARNESS_FRAME_LAST_CLASSIFICATION=GUEST_COMMAND_STALL
    return 1
  fi
  HARNESS_FRAME_LAST_STATUS=$status
  framed_status_allowed "$status" "$allowed" || {
    HARNESS_FRAME_LAST_CLASSIFICATION=GUEST_COMMAND_FAILURE
    return 1
  }
}

# ASYNC launch contract: the START marker belongs to the handler and must be
# visible before END.  Completion is deliberately not inferred here.
framed_send_launch(){
  framed_send_complete "$@"
}

framed_wait_marker_after_end(){
  local marker=$1 first_line=$2 timeout=${3:-180} description=${4:-async-command}
  local deadline=$((SECONDS + timeout)) fresh
  while ((SECONDS <= deadline)); do
    if [[ -n $(framed_marker_line "$serial" "$marker" "$first_line") ]]; then
      return 0
    fi
    fresh=$(tail -n +"$first_line" "$serial" 2>/dev/null || true)
    if grep -Eq 'PANIC|#PF|#GP|FATAL|STRUCTURAL_FAULT|FINISH_FAULT|QEMU exited' \
        <<<"$fresh"; then
      HARNESS_FRAME_LAST_CLASSIFICATION=GUEST_FAULT
      echo "[HMP][ASYNC] GUEST_FAULT command=$description marker=$marker" >&2
      harness_diagnose "$description" "$marker" GUEST_FAULT
      return 1
    fi
    if ! framed_qemu_alive; then
      HARNESS_FRAME_LAST_CLASSIFICATION=GUEST_FAULT
      echo "[HMP][ASYNC] GUEST_FAULT command=$description reason=qemu-exited" >&2
      return 1
    fi
    sleep 1
  done
  HARNESS_FRAME_LAST_CLASSIFICATION=GUEST_ASYNC_COMPLETION_TIMEOUT
  echo "[HMP][ASYNC] GUEST_ASYNC_COMPLETION_TIMEOUT command=$description marker=$marker" >&2
  harness_diagnose "$description" "$marker" GUEST_ASYNC_COMPLETION_TIMEOUT
  return 1
}

framed_schedtest_async(){
  local kind=$1 payload=$2 start_prefix=$3 completion_prefix=$4
  local timeout=${5:-300} profile=${6:-stress}
  local first_line begin_ms end_ms start_record run launch_sequence
  local launch_status timeout_ms completion_marker start_line end_line completion_line
  first_line=$(( $(wc -l <"$serial" 2>/dev/null || echo 0) + 1 ))
  begin_ms=$(( $(date +%s%N) / 1000000 ))
  framed_send_launch "$payload" "$start_prefix" "$timeout" "$profile" 0
  launch_sequence=$HARNESS_FRAME_LAST_SEQUENCE
  launch_status=$HARNESS_FRAME_LAST_STATUS
  start_record=$(framed_prefix_record_text "$serial" "$start_prefix" "$first_line")
  [[ $start_record =~ run=([1-9][0-9]*) ]] || {
    HARNESS_FRAME_LAST_CLASSIFICATION=GUEST_COMMAND_CONTRACT_FAILURE
    echo "[HMP][ASYNC] missing nonzero run command=$payload" >&2
    return 1
  }
  run=${BASH_REMATCH[1]}
  timeout_ms=$((timeout * 1000))
  ((timeout_ms > 3600000)) && timeout_ms=3600000
  framed_send_complete "schedtest async-wait $run $timeout_ms" \
    "[SCHED][ASYNC_WAIT] PASS run=$run kind=$kind" "$timeout" "$profile" 0
  completion_marker="$completion_prefix run=$run"
  framed_wait_marker_after_end "$completion_marker" "$first_line" "$timeout" "$payload"
  start_line=$(framed_marker_line "$serial" "$start_prefix run=$run" "$first_line")
  end_line=$(framed_exact_record_line "$serial" \
    "^\\[HARNESS\\]\\[END\\] seq=$launch_sequence status=$launch_status$" \
    "$first_line")
  completion_line=$(framed_marker_line "$serial" "$completion_marker" "$first_line")
  [[ -n $start_line && -n $end_line && -n $completion_line &&
     $start_line -lt $end_line && $completion_line -gt $start_line ]] || {
    HARNESS_FRAME_LAST_CLASSIFICATION=GUEST_COMMAND_CONTRACT_FAILURE
    return 1
  }
  if ((completion_line < end_line)); then
    HARNESS_ASYNC_LAST_ORDER=COMPLETE_BEFORE_END
  else
    HARNESS_ASYNC_LAST_ORDER=END_BEFORE_COMPLETE
  fi
  HARNESS_ASYNC_LAST_RUN=$run
  HARNESS_ASYNC_LAST_START_LINE=$start_line
  HARNESS_ASYNC_LAST_END_LINE=$end_line
  HARNESS_ASYNC_LAST_COMPLETION_LINE=$completion_line
  end_ms=$(( $(date +%s%N) / 1000000 ))
  local record="[CL13][ASYNC] PASS kind=$kind run=$run marker_order=$HARNESS_ASYNC_LAST_ORDER launch_status=$launch_status completion_status=0 elapsed_ms=$((end_ms-begin_ms)) start_line=$start_line end_line=$end_line completion_line=$completion_line"
  echo "$record"
  [[ -z ${HARNESS_ASYNC_RECORD_LOG:-} ]] || echo "$record" >>"$HARNESS_ASYNC_RECORD_LOG"
}

framed_schedtest_async_negative(){
  local payload=$1 start_prefix=$2 negative_marker=$3 timeout=${4:-300}
  local profile=${5:-stress} allowed=${6:-"0 1"}
  local first_line start_record run
  first_line=$(( $(wc -l <"$serial" 2>/dev/null || echo 0) + 1 ))
  framed_send_launch "$payload" "$start_prefix" "$timeout" "$profile" "$allowed"
  start_record=$(framed_prefix_record_text "$serial" "$start_prefix" "$first_line")
  [[ $start_record =~ run=([1-9][0-9]*) ]] || {
    HARNESS_FRAME_LAST_CLASSIFICATION=GUEST_COMMAND_CONTRACT_FAILURE
    return 1
  }
  run=${BASH_REMATCH[1]}
  framed_wait_marker_after_end "$negative_marker" "$first_line" "$timeout" "$payload"
  HARNESS_ASYNC_LAST_RUN=$run
  echo "[CL13][ASYNC_NEGATIVE] PASS run=$run marker=$negative_marker launch_status=$HARNESS_FRAME_LAST_STATUS"
}

framed_synctest_async(){
  local kind=$1 payload=$2 start_prefix=$3 completion_prefix=$4
  local timeout=${5:-300} profile=${6:-stress}
  local first_line begin_ms end_ms start_record run launch_sequence
  local launch_status timeout_ms completion_marker start_line end_line completion_line
  first_line=$(( $(wc -l <"$serial" 2>/dev/null || echo 0) + 1 ))
  begin_ms=$(( $(date +%s%N) / 1000000 ))
  framed_send_launch "$payload" "$start_prefix" "$timeout" "$profile" 0
  launch_sequence=$HARNESS_FRAME_LAST_SEQUENCE
  launch_status=$HARNESS_FRAME_LAST_STATUS
  start_record=$(framed_prefix_record_text "$serial" "$start_prefix" "$first_line")
  [[ $start_record =~ run=([1-9][0-9]*) ]] || {
    HARNESS_FRAME_LAST_CLASSIFICATION=GUEST_COMMAND_CONTRACT_FAILURE
    echo "[HMP][SYNC_ASYNC] missing nonzero run command=$payload" >&2
    return 1
  }
  run=${BASH_REMATCH[1]}
  timeout_ms=$((timeout * 1000))
  ((timeout_ms > 3600000)) && timeout_ms=3600000
  framed_send_complete "synctest async-wait $run $timeout_ms" \
    "[SYNC][ASYNC_WAIT] PASS run=$run kind=$kind" \
    "$timeout" "$profile" 0
  completion_marker="$completion_prefix run=$run"
  framed_wait_marker_after_end "$completion_marker" "$first_line" "$timeout" "$payload"
  start_line=$(framed_marker_line "$serial" "$start_prefix run=$run" "$first_line")
  end_line=$(framed_exact_record_line "$serial" \
    "^\\[HARNESS\\]\\[END\\] seq=$launch_sequence status=$launch_status$" \
    "$first_line")
  completion_line=$(framed_marker_line "$serial" "$completion_marker" "$first_line")
  [[ -n $start_line && -n $end_line && -n $completion_line &&
     $start_line -lt $end_line && $completion_line -gt $start_line ]] || {
    HARNESS_FRAME_LAST_CLASSIFICATION=GUEST_COMMAND_CONTRACT_FAILURE
    return 1
  }
  if ((completion_line < end_line)); then
    HARNESS_ASYNC_LAST_ORDER=COMPLETE_BEFORE_END
  else
    HARNESS_ASYNC_LAST_ORDER=END_BEFORE_COMPLETE
  fi
  HARNESS_ASYNC_LAST_RUN=$run
  HARNESS_ASYNC_LAST_START_LINE=$start_line
  HARNESS_ASYNC_LAST_END_LINE=$end_line
  HARNESS_ASYNC_LAST_COMPLETION_LINE=$completion_line
  end_ms=$(( $(date +%s%N) / 1000000 ))
  local record="[CL13][SYNC_ASYNC] PASS kind=$kind run=$run marker_order=$HARNESS_ASYNC_LAST_ORDER launch_status=$launch_status completion_status=0 elapsed_ms=$((end_ms-begin_ms)) start_line=$start_line end_line=$end_line completion_line=$completion_line"
  echo "$record"
  [[ -z ${HARNESS_ASYNC_RECORD_LOG:-} ]] || echo "$record" >>"$HARNESS_ASYNC_RECORD_LOG"
}

framed_synctest_async_negative(){
  local payload=$1 start_prefix=$2 negative_marker=$3 timeout=${4:-300}
  local profile=${5:-stress} allowed=${6:-"0 1"}
  local first_line start_record run
  first_line=$(( $(wc -l <"$serial" 2>/dev/null || echo 0) + 1 ))
  framed_send_launch "$payload" "$start_prefix" "$timeout" "$profile" "$allowed"
  start_record=$(framed_prefix_record_text "$serial" "$start_prefix" "$first_line")
  [[ $start_record =~ run=([1-9][0-9]*) ]] || return 1
  run=${BASH_REMATCH[1]}
  framed_wait_marker_after_end "$negative_marker" "$first_line" "$timeout" "$payload"
  HARNESS_ASYNC_LAST_RUN=$run
  echo "[CL13][SYNC_ASYNC_NEGATIVE] PASS run=$run marker=$negative_marker launch_status=$HARNESS_FRAME_LAST_STATUS"
}

# Existing CL-13 scripts can retain their orchestration while every shell line
# is upgraded to a transactional frame.  Unlike the legacy helper, END itself
# is the shell-return proof, so no implicit second sync command is emitted.
send_complete(){ framed_send_complete "$@"; }
send_raw_complete(){ framed_send_complete "$@"; }
transport_probe(){
  HARNESS_TRANSPORT_NONCE=$(( ${HARNESS_TRANSPORT_NONCE:-0} + 1 ))
  framed_send_complete "inputtest marker transport${HARNESS_TRANSPORT_NONCE}" \
    "[INPUTTEST][MARKER] name=transport${HARNESS_TRANSPORT_NONCE}" 60 sync
}
shell_sync(){
  framed_send_complete "killtest stats" "[KILLTEST][STATS]" \
    "${SHELL_SYNC_TIMEOUT:-180}" stress
}

modal_cycle_complete(){
  local begin end owner
  begin=$(count "[MODAL] session_begin OK")
  end=$(count "[MODAL] session_end OK")
  owner=$(count "[TASKMAN][OWNER_STATE] shell=BLOCKED session=ACTIVE")
  framed_start_command "taskman 50" normal 60
  wait_new "[MODAL] session_begin OK" "$begin" 120
  wait_new "[TASKMAN][OWNER_STATE] shell=BLOCKED session=ACTIVE" "$owner" 120
  hmp_key esc normal >/dev/null
  hmp_key esc normal >/dev/null
  hmp_key esc normal >/dev/null
  wait_new "[MODAL] session_end OK" "$end" 120
  framed_finish_started "[MODAL] session_end OK" 120 0
}

taskman_killed_cycle_complete(){
  send_complete "modaltest arm-kill-next-ui" \
    "[MODALTEST][ARM_KILL_NEXT_UI] OK" 60 stress
  send_complete "taskman 50" "[MODAL] session_end OK" 180 normal
}
