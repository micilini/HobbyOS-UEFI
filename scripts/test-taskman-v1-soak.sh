#!/usr/bin/env bash
set -Eeuo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

[[ $# == 1 && ( $1 == 4 || $1 == 8 ) ]] || {
  echo "Usage: $0 4|8" >&2
  exit 2
}

smp=$1
accel=tcg
[[ $smp == 8 ]] && accel=kvm
duration=${DURATION_MS:-180000}
((duration >= 180000)) || {
  echo "CL13 soak duration must be at least 180000 ms" >&2
  exit 2
}
if [[ $accel == kvm && ( ! -r /dev/kvm || ! -w /dev/kvm ) ]]; then
  echo "CL13 SMP=8 soak requires accessible /dev/kvm" >&2
  exit 3
fi
smpstress_period_ms=${SMPSTRESS_PERIOD_MS:-10000}
[[ $smpstress_period_ms =~ ^[0-9]+$ ]] &&
  ((smpstress_period_ms >= 1000 && smpstress_period_ms <= 60000)) || {
  echo "SMPSTRESS_PERIOD_MS must be between 1000 and 60000" >&2
  exit 2
}

serial=.qemu/qemu-serial.log
hmp=(python3 scripts/qemu_hmp.py --socket .qemu/hmp.sock)
harness_start_line=0
harness_command=boot
artifact="artifacts/build/cl13-soak-smp${smp}-${accel}.log"
checkpoint_prefix=${CL13_SOAK_CHECKPOINT_PREFIX:-cl13fix12}
stage_artifact="artifacts/build/${checkpoint_prefix}-soak${smp}-stages.tsv"
status_artifact="artifacts/build/${checkpoint_prefix}-soak${smp}-status-authority.log"
mkdir -p artifacts/build
source scripts/harness-common.sh
source scripts/harness-runtime-ready.sh
source scripts/harness-framed.sh
source scripts/harness-selftest.sh

cleanup(){ stop_qemu; }
trap cleanup EXIT

runtime_manifest_file=${RUNTIME_MANIFEST_FILE:-artifacts/build/cl13fix12-runtime-before.sha256}
runtime_manifest_hash=$(sha256sum "$runtime_manifest_file" | awk '{print $1}')
printf 'stage\tstatus\thost_timestamp\tframe_sequence\tartifact_line\truntime_manifest\twarmup_id\twarmup_gone\thandles\thandles_gone\tbaseline_used\tfinal_used\tdirection\tsigned_delta\tbaseline_blocks\tfinal_blocks\tzombies\tfree_inflight\townership\truns_delta\tworkers_delta\tnormal_delta\tcleanup_delta\tsignals_delta\tduplicates_delta\tcontexts_live\tcontexts_quarantined\ttimer_nodes_before\ttimer_nodes_after\trequested\tcompleted\tdiagnostic_marker\n' \
  >"$stage_artifact"
: >"$status_artifact"
HARNESS_STATUS_RECORD_LOG=$status_artifact
status_authority_commands=0
diagnostic_markers_present=0
diagnostic_markers_missing=0
ps_warmup_snapshots=0
ps_stress_snapshots_completed=0
taskman_warmup_sessions=0
taskman_stress_sessions_completed=0
taskman_refresh50=0
taskman_refresh1000=0
taskman_refresh2000=0
global_heap_pass=0
input_runs_completed=0
modal_cycles_completed=0
current_macro_stage=boot
failure_checkpoint_written=0
modal_warmup_id=-
modal_warmup_gone=-
modal_handles=-
modal_handles_gone=-
modal_baseline_used=-
modal_final_used=-
modal_direction=-
modal_signed_delta=-
modal_baseline_blocks=-
modal_final_blocks=-
modal_zombies=-
modal_free_inflight=-
modal_ownership=-
modal_runs_delta=-
modal_workers_delta=-
modal_normal_delta=-
modal_cleanup_delta=-
modal_signals_delta=-
modal_duplicates_delta=-
modal_contexts_live=-
modal_contexts_quarantined=-
modal_timer_nodes_before=-
modal_timer_nodes_after=-

stage_checkpoint(){
  local stage=$1 status=${2:-PASS} requested=${3:--} completed=${4:--}
  local diagnostic_marker=${5:--} frame_sequence=${6:-$HARNESS_FRAME_SEQUENCE}
  local artifact_line=0
  artifact_line=$(wc -l <"$serial" 2>/dev/null || echo 0)
  local warmup_id=- warmup_gone=- handles=- handles_gone=-
  local baseline_used=- final_used=- direction=- signed_delta=-
  local baseline_blocks=- final_blocks=- zombies=- free_inflight=-
  local ownership=- runs_delta=- workers_delta=- normal_delta=-
  local cleanup_delta=- signals_delta=- duplicates_delta=-
  local contexts_live=- contexts_quarantined=-
  local timer_nodes_before=- timer_nodes_after=-
  if [[ $stage == modal ]]; then
    warmup_id=$modal_warmup_id
    warmup_gone=$modal_warmup_gone
    handles=$modal_handles
    handles_gone=$modal_handles_gone
    baseline_used=$modal_baseline_used
    final_used=$modal_final_used
    direction=$modal_direction
    signed_delta=$modal_signed_delta
    baseline_blocks=$modal_baseline_blocks
    final_blocks=$modal_final_blocks
    zombies=$modal_zombies
    free_inflight=$modal_free_inflight
    ownership=$modal_ownership
    runs_delta=$modal_runs_delta
    workers_delta=$modal_workers_delta
    normal_delta=$modal_normal_delta
    cleanup_delta=$modal_cleanup_delta
    signals_delta=$modal_signals_delta
    duplicates_delta=$modal_duplicates_delta
    contexts_live=$modal_contexts_live
    contexts_quarantined=$modal_contexts_quarantined
    timer_nodes_before=$modal_timer_nodes_before
    timer_nodes_after=$modal_timer_nodes_after
  fi
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$stage" "$status" "$(date --iso-8601=ns)" \
    "$frame_sequence" "$artifact_line" "$runtime_manifest_hash" \
    "$warmup_id" "$warmup_gone" "$handles" "$handles_gone" \
    "$baseline_used" "$final_used" "$direction" "$signed_delta" \
    "$baseline_blocks" "$final_blocks" "$zombies" "$free_inflight" \
    "$ownership" "$runs_delta" "$workers_delta" "$normal_delta" \
    "$cleanup_delta" "$signals_delta" "$duplicates_delta" \
    "$contexts_live" "$contexts_quarantined" \
    "$timer_nodes_before" "$timer_nodes_after" \
    "$requested" "$completed" "$diagnostic_marker" \
    >>"$stage_artifact"
}

checkpoint_failure(){
  local rc=$?
  if ((failure_checkpoint_written == 0)); then
    failure_checkpoint_written=1
    stage_checkpoint "$current_macro_stage" FAIL || true
  fi
  return "$rc"
}
trap checkpoint_failure ERR

# SYNC_STATUS makes the exact handler return status authoritative.  The
# supplied human marker is retained as telemetry and is deliberately not a
# second gate under concurrent serial output.
send_status(){
  framed_send_status_complete "$@"
  ((status_authority_commands+=1))
  if [[ $HARNESS_FRAME_LAST_DIAGNOSTIC_MARKER == 1 ]]; then
    ((diagnostic_markers_present+=1))
  else
    ((diagnostic_markers_missing+=1))
  fi
}

parse_modal_result(){
  local expected=$1 line expected_runs
  line=$(grep -E '^\[MODALTEST\]\[OPEN_CLOSE\] (PASS|FAIL) cycles=' \
    "$serial" | tail -1)
  [[ $line =~ ^\[MODALTEST\]\[OPEN_CLOSE\][[:space:]]PASS[[:space:]]cycles=([0-9]+) ]]
  [[ ${BASH_REMATCH[1]} == "$expected" ]]
  [[ $line =~ cleanup=([0-9]+) ]] && [[ ${BASH_REMATCH[1]} == "$expected" ]]
  [[ $line =~ ownership=(PASS|FAIL) ]] && modal_ownership=${BASH_REMATCH[1]}
  [[ $modal_ownership == PASS ]]
  [[ $line =~ reason=([^[:space:]]+) ]] && [[ ${BASH_REMATCH[1]} == none ]]
  [[ $line =~ warmup_id=([0-9]+) ]] && modal_warmup_id=${BASH_REMATCH[1]}
  ((modal_warmup_id > 0))
  [[ $line =~ warmup_gone=([01]) ]] && modal_warmup_gone=${BASH_REMATCH[1]}
  [[ $modal_warmup_gone == 1 ]]
  [[ $line =~ handles=([0-9]+) ]] && modal_handles=${BASH_REMATCH[1]}
  [[ $modal_handles == "$expected" ]]
  [[ $line =~ handles_gone=([0-9]+) ]] && modal_handles_gone=${BASH_REMATCH[1]}
  [[ $modal_handles_gone == "$expected" ]]
  [[ $line =~ first_live_id=([0-9]+) ]] && [[ ${BASH_REMATCH[1]} == 0 ]]
  [[ $line =~ baseline_used=([0-9]+) ]] && modal_baseline_used=${BASH_REMATCH[1]}
  [[ $line =~ final_used=([0-9]+) ]] && modal_final_used=${BASH_REMATCH[1]}
  [[ $line =~ direction=(ZERO|UP|DOWN) ]] && modal_direction=${BASH_REMATCH[1]}
  [[ $line =~ signed_delta=(-?[0-9]+) ]] && modal_signed_delta=${BASH_REMATCH[1]}
  [[ $line =~ baseline_blocks=([0-9]+) ]] && modal_baseline_blocks=${BASH_REMATCH[1]}
  [[ $line =~ final_blocks=([0-9]+) ]] && modal_final_blocks=${BASH_REMATCH[1]}
  [[ $line =~ heap_scope=GLOBAL_DIAGNOSTIC ]]
  [[ $line =~ heap_gate=OUTER_SCENARIO ]]
  [[ $line =~ reaper_idle=([01]) ]] && [[ ${BASH_REMATCH[1]} == 1 ]]
  [[ $line =~ zombies=([0-9]+) ]] && modal_zombies=${BASH_REMATCH[1]}
  [[ $modal_zombies == 0 ]]
  [[ $line =~ free_inflight=([0-9]+) ]] && modal_free_inflight=${BASH_REMATCH[1]}
  [[ $modal_free_inflight == 0 ]]
  [[ $line =~ modal_violations=([0-9]+) ]] && [[ ${BASH_REMATCH[1]} == 0 ]]
  [[ $line =~ session_violations=([0-9]+) ]] && [[ ${BASH_REMATCH[1]} == 0 ]]
  expected_runs=$((expected + 1))
  [[ $line =~ runs_delta=([0-9]+) ]] && modal_runs_delta=${BASH_REMATCH[1]}
  [[ $modal_runs_delta == "$expected_runs" ]]
  [[ $line =~ workers_delta=([0-9]+) ]] && modal_workers_delta=${BASH_REMATCH[1]}
  [[ $modal_workers_delta == "$expected_runs" ]]
  [[ $line =~ normal_delta=([0-9]+) ]] && modal_normal_delta=${BASH_REMATCH[1]}
  [[ $modal_normal_delta == "$expected_runs" ]]
  [[ $line =~ killed_delta=([0-9]+) ]] && [[ ${BASH_REMATCH[1]} == 0 ]]
  [[ $line =~ cleanup_delta=([0-9]+) ]] && modal_cleanup_delta=${BASH_REMATCH[1]}
  [[ $modal_cleanup_delta == "$expected_runs" ]]
  [[ $line =~ signals_delta=([0-9]+) ]] && modal_signals_delta=${BASH_REMATCH[1]}
  [[ $modal_signals_delta == "$expected_runs" ]]
  [[ $line =~ duplicates_delta=([0-9]+) ]] && modal_duplicates_delta=${BASH_REMATCH[1]}
  [[ $modal_duplicates_delta == 0 ]]
  [[ $line =~ alloc_fail_delta=([0-9]+) ]] && [[ ${BASH_REMATCH[1]} == 0 ]]
  [[ $line =~ create_fail_delta=([0-9]+) ]] && [[ ${BASH_REMATCH[1]} == 0 ]]
  [[ $line =~ begin_fail_delta=([0-9]+) ]] && [[ ${BASH_REMATCH[1]} == 0 ]]
  [[ $line =~ recovery_delta=([0-9]+) ]] && [[ ${BASH_REMATCH[1]} == 0 ]]
  [[ $line =~ second_session_delta=([0-9]+) ]] && [[ ${BASH_REMATCH[1]} == 0 ]]
  [[ $line =~ contexts_live=([0-9]+) ]] && modal_contexts_live=${BASH_REMATCH[1]}
  [[ $modal_contexts_live == 0 ]]
  [[ $line =~ contexts_quarantined=([0-9]+) ]] && modal_contexts_quarantined=${BASH_REMATCH[1]}
  [[ $modal_contexts_quarantined == 0 ]]
  [[ $line =~ timer_nodes_before=([0-9]+) ]] && modal_timer_nodes_before=${BASH_REMATCH[1]}
  [[ $line =~ timer_nodes_after=([0-9]+) ]] && modal_timer_nodes_after=${BASH_REMATCH[1]}
}

checks(){
  send_status "schedtest check" 90 stress 0 "[SCHED][CHECK] PASS"
  send_status "synctest check" 90 stress 0 "[SYNC][CHECK] PASS"
  send_status "accounttest check" 120 stress 0 "[ACCOUNT][CHECK] PASS"
  send_status "killtest check" 90 stress 0 "[KILLTEST][CHECK] PASS"
  send_status "reaptest check" 90 stress 0 "[REAPTEST][CHECK] PASS"
  send_status "inputtest check" 90 stress 0 "[INPUTTEST][CHECK] PASS"
  send_status "modaltest check" 90 stress 0 "[MODALTEST][CHECK] PASS"
  send_status "taskmantest check" 90 stress 0 "[TASKMANTEST][CHECK] PASS"
  send_status "taskmantest fixture-status" 90 stress 0 \
    "[TASKMANTEST][FIXTURE_STATUS] PASS max=257 active=0 present=0 gate_release=0 cleanup=0"
  send_status "taskdiag check" 90 stress 0 "[TASKDIAG][CHECK] PASS"
  transport_check
}

transport_check(){
  local line
  send_complete "tasktest transport-status" "[HARNESS][STATUS]" 90 stress
  line=$(grep -F '[HARNESS][STATUS]' "$serial" | tail -1)
  [[ $line =~ accepted=([0-9]+)[[:space:]]completed=([0-9]+) ]]
  [[ ${BASH_REMATCH[1]} == "${BASH_REMATCH[2]}" ]]
}

record_field(){
  local line=$1 name=$2
  [[ $line =~ (^|[[:space:]])${name}=([^[:space:]]+) ]] || return 1
  printf '%s' "${BASH_REMATCH[2]}"
}

parse_auto_session(){
  local refresh=$1 line
  line=$(grep -E '^\[TASKMANTEST\]\[AUTO_SESSION\] PASS ' "$serial" | tail -1)
  [[ $(record_field "$line" refresh) == "$refresh" ]]
  [[ $(record_field "$line" target) == 1 ]]
  [[ $(record_field "$line" taskman_status) == 0 ]]
  [[ $(record_field "$line" sessions_delta) == 1 ]]
  [[ $(record_field "$line" auto_exit_delta) == 1 ]]
  [[ $(record_field "$line" full_delta) == 1 ]]
  [[ $(record_field "$line" fallback_delta) == 0 ]]
  [[ $(record_field "$line" render_fail_delta) == 0 ]]
  [[ $(record_field "$line" shortfall_delta) == 0 ]]
  [[ $(record_field "$line" modal_scroll) == 0 ]]
  [[ $(record_field "$line" shell_exit_scroll) == 0 ]]
  [[ $(record_field "$line" clipped) == 0 ]]
  [[ $(record_field "$line" model_live) == 0 ]]
  [[ $(record_field "$line" pending) == 0 ]]
  [[ $(record_field "$line" controls_default) == 1 ]]
  [[ $(record_field "$line" valid) == 1 ]]
}

parse_ps_loop(){
  local expected=$1 line
  line=$(grep -E '^\[HARNESS\]\[PS_LOOP\] PASS ' "$serial" | tail -1)
  [[ $(record_field "$line" requested) == "$expected" ]]
  [[ $(record_field "$line" completed) == "$expected" ]]
  [[ $(record_field "$line" failure_index) == 0 ]]
  [[ $(record_field "$line" status) == 0 ]]
}

parse_taskman_loop(){
  local count=$1 start=$2 expected50=$3 expected1000=$4 expected2000=$5 line
  line=$(grep -E '^\[TASKMANTEST\]\[AUTO_SESSION_LOOP\] PASS ' "$serial" | tail -1)
  [[ $(record_field "$line" count) == "$count" ]]
  [[ $(record_field "$line" completed) == "$count" ]]
  [[ $(record_field "$line" start) == "$start" ]]
  [[ $(record_field "$line" frames) == 1 ]]
  [[ $(record_field "$line" refresh50) == "$expected50" ]]
  [[ $(record_field "$line" refresh1000) == "$expected1000" ]]
  [[ $(record_field "$line" refresh2000) == "$expected2000" ]]
  [[ $(record_field "$line" failure_index) == 0 ]]
}

make kernel-check JOBS=2 SELFTEST=1 SELFTEST_AUTORUN=1 >/dev/null
make image SELFTEST=1 SELFTEST_AUTORUN=1 >/dev/null
harness_lock
start_qemu "$smp" "$accel"
selftest_validate_autorun "$serial" \
  "artifacts/build/cl13-soak-smp${smp}-${accel}" \
  accounting.sleep.minimum_deadline \
  accounting.sleep.one_ms_granularity \
  accounting.sleep.late_is_diagnostic \
  accounting.sleep.wait_result \
  accounting.sleep.percentiles \
  modal.open_close.stats_delta \
  modal.open_close.global_heap_is_diagnostic \
  modal.open_close.outer_heap_required \
  modal.open_close.timer_node_noise \
  ui.taskman.auto_session_delta \
  ui.taskman.auto_session_refresh_pattern \
  ui.taskman.auto_session_record_fit \
  transport.ps_loop_bounds \
  transport.ps_loop_record_fit
stage_checkpoint boot

workers=8
yield_iterations=250000
creates=10000
kills=5000
modal_cycles=500
producer_runs=1
if [[ $smp == 8 ]]; then
  workers=16
  creates=20000
  kills=10000
  modal_cycles=1000
  producer_runs=2
fi

send_complete "accounttest clock-contract" \
  "[ACCOUNT][CLOCK_CONTRACT] PASS" 180 stress
send_complete "accounttest sleep-profile-set" \
  "[ACCOUNT][SLEEP_PROFILE_SET] PASS profiles=3 hard_failures=0 degraded_profiles=0" \
  1800 stress

# Warm all permanent UI paths before taking the zero-tolerance heap baseline.
current_macro_stage=warmup
send_status "taskmantest auto-session 50 1" 120 stress 0 \
  "[TASKMANTEST][AUTO_SESSION] PASS"
parse_auto_session 50
((taskman_warmup_sessions+=1))
send_status "ps" 60 stress 0 "[PS][SNAPSHOT]"
((ps_warmup_snapshots+=1))
checks
send_status "taskmantest heap-begin" 90 stress 0 \
  "[TASKMANTEST][HEAP_BEGIN] PASS"
stage_checkpoint warmup

start_ms=$(( $(date +%s%N) / 1000000 ))

# LAUNCH_ONLY: the later kill sweep is the completion/cleanup contract.
current_macro_stage=yield
framed_send_launch "smpstress $workers 0 $smpstress_period_ms" \
  "[SMP] smpstress spawning workers=$workers" 90 stress
HARNESS_ASYNC_RECORD_LOG="artifacts/build/cl13-soak-smp${smp}-${accel}-async.log"
: >"$HARNESS_ASYNC_RECORD_LOG"
framed_schedtest_async YIELD "schedtest yield $smp $yield_iterations" \
  "[SCHED][STRESS] YIELD_START" \
  "[SCHED][STRESS] YIELD_COMPLETE" 1800 stress
stage_checkpoint yield
current_macro_stage=create-reap
send_status "reaptest create-handle-race $creates" 2400 stress 0 \
  "[REAPTEST][CREATE_HANDLE_RACE] PASS created=$creates"
stage_checkpoint create-reap
current_macro_stage=kill-race
send_status "killtest timeout-race $kills" 3600 stress 0 \
  "[KILLTEST][TIMEOUT_RACE] PASS rounds=$kills"
stage_checkpoint kill-race
current_macro_stage=input
for ((run=1; run<=producer_runs; run++)); do
  send_status "inputtest producers 100000" 900 stress 0 \
    "[INPUTTEST][PRODUCERS] PASS accepted=100000"
  ((input_runs_completed+=1))
done
stage_checkpoint input
current_macro_stage=modal
send_status "modaltest open-close $modal_cycles" 1800 stress 0 \
  "[MODALTEST][OPEN_CLOSE] PASS cycles=$modal_cycles"
parse_modal_result "$modal_cycles"
modal_cycles_completed=$((modal_cycles_completed + modal_cycles))
stage_checkpoint modal
checks

current_macro_stage=ps
for ((batch=1; batch<=5; batch++)); do
  current_macro_stage="ps-batch-$batch"
  send_status "tasktest ps-loop 100" 1800 stress 0 \
    "[HARNESS][PS_LOOP] PASS"
  parse_ps_loop 100
  ((ps_stress_snapshots_completed+=100))
  batch_frame=$HARNESS_FRAME_SEQUENCE
  batch_marker=$HARNESS_FRAME_LAST_DIAGNOSTIC_MARKER
  checks
  stage_checkpoint "ps-batch-$batch" PASS 100 100 \
    "$batch_marker" "$batch_frame"
  echo "[CL13][SOAK_PROGRESS] smp=$smp ps=$ps_stress_snapshots_completed"
done
current_macro_stage=ps
stage_checkpoint ps

current_macro_stage=taskman
taskman_starts=(1 26 51 76)
taskman_refresh50_batch=(8 8 9 8)
taskman_refresh1000_batch=(9 8 8 9)
taskman_refresh2000_batch=(8 9 8 8)
for ((batch=1; batch<=4; batch++)); do
  current_macro_stage="taskman-batch-$batch"
  index=$((batch - 1))
  start=${taskman_starts[$index]}
  expected50=${taskman_refresh50_batch[$index]}
  expected1000=${taskman_refresh1000_batch[$index]}
  expected2000=${taskman_refresh2000_batch[$index]}
  send_status "taskmantest auto-session-loop 25 1 $start" 900 stress 0 \
    "[TASKMANTEST][AUTO_SESSION_LOOP] PASS"
  parse_taskman_loop 25 "$start" "$expected50" "$expected1000" \
    "$expected2000"
  ((taskman_stress_sessions_completed+=25))
  ((taskman_refresh50+=expected50))
  ((taskman_refresh1000+=expected1000))
  ((taskman_refresh2000+=expected2000))
  batch_frame=$HARNESS_FRAME_SEQUENCE
  batch_marker=$HARNESS_FRAME_LAST_DIAGNOSTIC_MARKER
  checks
  stage_checkpoint "taskman-batch-$batch" PASS 25 25 \
    "$batch_marker" "$batch_frame"
  echo "[CL13][SOAK_PROGRESS] smp=$smp taskman=$taskman_stress_sessions_completed"
done
current_macro_stage=taskman
stage_checkpoint taskman

current_macro_stage=duration
while :; do
  now_ms=$(( $(date +%s%N) / 1000000 ))
  ((now_ms - start_ms >= duration)) && break
  checks
  sleep 15
done
stage_checkpoint duration

current_macro_stage=cleanup
send_status "killtest smpstress-sweep" 900 stress 0 \
  "[SMP][KILL_SWEEP] PASS workers=$workers"
checks
send_status "schedtest async-status" 90 stress 0 \
  "[SCHED][ASYNC_STATUS] PASS"
send_status "taskmantest heap-end" 300 stress 0 "[TASKMANTEST][HEAP] PASS"
global_heap_pass=1
send_status "taskmantest fixture-status" 90 stress 0 \
  "[TASKMANTEST][FIXTURE_STATUS] PASS max=257 active=0 present=0 gate_release=0 cleanup=0"
stage_checkpoint cleanup
current_macro_stage=final-diagnostics
send_status "taskmantest stats" 90 stress 0 "[TASKMANTEST][STATS]"
send_complete "tasktest transport-status" "[HARNESS][STATUS]" 90 stress
send_status "accounttest clock-loop 3" 900 stress 0 \
  "[ACCOUNT][CLOCK_LOOP] PASS rounds=3 failed_mask=0"
send_status "accounttest sleep-profile 10 64" 900 stress 0 \
  "[ACCOUNT][SLEEP_PROFILE] PASS target_ms=10 samples=64 early=0 wait_errors=0"
send_status "accounttest clock-smp $((smp * 4)) 1000000" 2400 stress 0 \
  "[ACCOUNT][CLOCK_SMP] PASS"
send_status "taskdiag accounting" 180 stress 0 \
  "[TASKDIAG][ACCOUNTING] PASS"
send_status "taskdiag all" 180 stress 0 "[TASKDIAG][ALL] PASS"
stage_checkpoint final-diagnostics

current_macro_stage=summary
end_ms=$(( $(date +%s%N) / 1000000 ))
duration_ms=$((end_ms - start_ms))
switches=$((smp * yield_iterations))
input_events=$((input_runs_completed * 100000))
transport_status=$(grep -F '[HARNESS][STATUS]' "$serial" | tail -1)

((duration_ms >= duration))
((switches >= (smp == 4 ? 1000000 : 2000000)))
((taskman_warmup_sessions == 1))
((taskman_stress_sessions_completed == 100))
((ps_warmup_snapshots == 1))
((ps_stress_snapshots_completed == 500))
((taskman_refresh50 == 33))
((taskman_refresh1000 == 34))
((taskman_refresh2000 == 33))
((input_runs_completed == producer_runs))
((modal_cycles_completed == modal_cycles))
((global_heap_pass == 1))
[[ $transport_status =~ accepted=([0-9]+)[[:space:]]completed=([0-9]+) ]]
[[ ${BASH_REMATCH[1]} == "${BASH_REMATCH[2]}" ]]
[[ $(grep -Ec '^\[TASKMANTEST\]\[AUTO_SESSION\] PASS ' "$serial") == 101 ]]
assert_clean_log "$serial"

cp "$serial" "$artifact"
{
  echo "[CL13][SOAK] PASS smp=$smp accel=$accel duration_ms=$duration_ms switches=$switches creates=$creates reaps=$creates kills=$kills taskman=$taskman_stress_sessions_completed ps=$ps_stress_snapshots_completed modal=$modal_cycles_completed input=$input_events taskman_warmup_sessions=$taskman_warmup_sessions ps_warmup_snapshots=$ps_warmup_snapshots refresh50=$taskman_refresh50 refresh1000=$taskman_refresh1000 refresh2000=$taskman_refresh2000 ps_old_frames=500 ps_new_frames=5 taskman_old_frames=300 taskman_new_frames=4 frames_saved=791 heap_drift=0 global_heap_drift=0 global_heap_pass=$global_heap_pass modal_ownership=1 modal_runs_delta=$modal_runs_delta modal_workers_delta=$modal_workers_delta modal_normal_delta=$modal_normal_delta modal_cleanup_delta=$modal_cleanup_delta modal_signals_delta=$modal_signals_delta modal_duplicates_delta=$modal_duplicates_delta modal_handles_gone=$modal_handles_gone modal_internal_heap_direction=$modal_direction modal_internal_heap_delta=$modal_signed_delta modal_timer_nodes_before=$modal_timer_nodes_before modal_timer_nodes_after=$modal_timer_nodes_after invariants=0 faults=0 status_authority_commands=$status_authority_commands diagnostic_markers_present=$diagnostic_markers_present diagnostic_markers_missing=$diagnostic_markers_missing smpstress_period_ms=$smpstress_period_ms"
} >>"$artifact"
sha256sum "$artifact" >"${artifact%.log}.sha256"
stage_checkpoint summary

stop_qemu
harness_unlock
trap - ERR EXIT
echo "[CL13][SOAK] PASS smp=$smp accel=$accel duration_ms=$duration_ms"
