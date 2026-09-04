#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd); cd "$root"
serial=.qemu/qemu-serial.log
hmp=(python3 scripts/qemu_hmp.py --socket .qemu/hmp.sock)
mkdir -p artifacts/build
harness_start_line=0; harness_command=boot
source scripts/harness-common.sh
harness_lock
cleanup(){ stop_qemu; }
trap cleanup EXIT
checks(){
  send_complete "taskmantest check" "[TASKMANTEST][CHECK] PASS" 60
  send_complete "modaltest check" "[MODALTEST][CHECK] PASS" 60
  send_complete "inputtest check" "[INPUTTEST][CHECK] PASS" 60
  send_complete "schedtest check" "[SCHED][CHECK] PASS" 60
  send_complete "synctest check" "[SYNC][CHECK] PASS" 60
  send_complete "accounttest check" "[ACCOUNT][CHECK] PASS" 60
  send_complete "killtest check" "[KILLTEST][CHECK] PASS" 60
  send_complete "reaptest check" "[REAPTEST][CHECK] PASS" 60
}
preblock_focused(){
  make image >/dev/null
  start_qemu 8 tcg
  send_complete "synctest preblock-sem" \
    "[SYNCTEST][PREBLOCK_SEM] PASS" 60
  send_complete "synctest preblock-timer" \
    "[SYNCTEST][PREBLOCK_TIMER] PASS" 60
  send_complete "inputtest preblock-cancel" \
    "[INPUTTEST][PREBLOCK_CANCEL] PASS" 60
  send_complete "synctest preblock-timer-noise 1000" \
    "[SYNCTEST][PREBLOCK_TIMER_NOISE] PASS count=1000" 3600 stress
  send_complete "modaltest kill-caller-blocked" \
    "[MODALTEST][KILL_CALLER_BLOCKED] PASS" 120
  send_complete "modaltest kill-caller-prewait" \
    "[MODALTEST][KILL_CALLER_PREWAIT] PASS" 120
  send_complete "modaltest kill-caller-completion-ready" \
    "[MODALTEST][KILL_CALLER_COMPLETION_READY] PASS" 120
  checks
  cp "$serial" artifacts/build/cl11fix11-focused-smp8.log
  assert_clean_log artifacts/build/cl11fix11-focused-smp8.log
  stop_qemu
}
preblock_loops(){
  local smp mode
  make image >/dev/null
  for smp in 1 8; do
    for mode in sem timer; do
      start_qemu "$smp" tcg
      send_complete "synctest preblock-loop 1000 $mode" \
        "[SYNCTEST][PREBLOCK_LOOP] PASS count=1000" 3600 stress
      cp "$serial" \
        "artifacts/build/cl11fix11-preblock-${mode}-smp${smp}.log"
      assert_clean_log \
        "artifacts/build/cl11fix11-preblock-${mode}-smp${smp}.log"
      stop_qemu
    done
    start_qemu "$smp" tcg
    send_complete "inputtest preblock-loop 1000" \
      "[INPUTTEST][PREBLOCK_LOOP] PASS count=1000" 3600 stress
    cp "$serial" \
      "artifacts/build/cl11fix11-preblock-input-smp${smp}.log"
    assert_clean_log \
      "artifacts/build/cl11fix11-preblock-input-smp${smp}.log"
    stop_qemu
  done
}
modal_exact_loops(){
  local smp mode sentinel
  make image >/dev/null
  for smp in 1 8; do
    for mode in blocked prewait completion-ready; do
      case "$mode" in
        blocked) sentinel=BLOCKED;;
        prewait) sentinel=PREWAIT;;
        completion-ready) sentinel=COMPLETION_READY;;
      esac
      start_qemu "$smp" tcg
      send_complete "modaltest kill-caller-loop 1000 $mode" \
        "[MODALTEST][KILL_CALLER_LOOP] PASS mode=$sentinel count=1000" 3600 stress
      cp "$serial" \
        "artifacts/build/cl11fix11-modal-${mode}-smp${smp}.log"
      assert_clean_log \
        "artifacts/build/cl11fix11-modal-${mode}-smp${smp}.log"
      stop_qemu
    done
  done
}
negative_preblock(){
  make kernel-check JOBS=2 \
    KERNEL_EXTRA_CFLAGS=-DHOBBYOS_WAIT_NEGATIVE_IGNORE_PREBLOCK_CANCEL \
    >/dev/null
  make image \
    KERNEL_EXTRA_CFLAGS=-DHOBBYOS_WAIT_NEGATIVE_IGNORE_PREBLOCK_CANCEL \
    >/dev/null
  start_qemu 2 tcg
  send_raw_complete "synctest preblock-negative" \
    "[SYNCTEST][NEGATIVE] PREBLOCK_CANCELLATION_LOST_DETECTED" 120
  cp "$serial" artifacts/build/cl11fix11-negative-preblock.log
  stop_qemu

  make kernel-check JOBS=2 >/dev/null
  make image >/dev/null
  start_qemu 2 tcg
  send_complete "synctest preblock-sem" \
    "[SYNCTEST][PREBLOCK_SEM] PASS" 60
  send_complete "synctest preblock-timer" \
    "[SYNCTEST][PREBLOCK_TIMER] PASS" 60
  send_complete "inputtest preblock-cancel" \
    "[INPUTTEST][PREBLOCK_CANCEL] PASS" 60
  send_complete "modaltest kill-caller-prewait" \
    "[MODALTEST][KILL_CALLER_PREWAIT] PASS" 120
  send_complete "modaltest check" "[MODALTEST][CHECK] PASS" 60
  cp "$serial" artifacts/build/cl11fix11-negative-reset.log
  assert_clean_log artifacts/build/cl11fix11-negative-reset.log
  stop_qemu
}
heavy_runs(){
  local run artifact
  make image >/dev/null
  start_qemu 8 tcg
  for run in 1 2 3 4 5; do
    send_complete "reaptest snapshot-boundary 100" \
      "[REAPTEST][SNAPSHOT_BOUNDARY_LOOP] PASS rounds=100 verified=100 failures=0" 600 stress
    send_complete "reaptest concurrent-churn 1000 1000 1000" \
      "[REAPTEST][CONCURRENT_CHURN] PASS" 1800 stress
    send_complete "reaptest create-handle-race 100000" \
      "[REAPTEST][CREATE_HANDLE_RACE] PASS" 7200 stress
    send_complete "accounttest clock-smp 32 1000000" \
      "[ACCOUNT][CLOCK_SMP] PASS" 1200 stress
    send_complete "reaptest timer-ref-loop 100" \
      "[REAPTEST][TIMER_REF_LOOP] PASS count=100" 1200 stress
    send_complete "synctest preblock-loop 100 all" \
      "[SYNCTEST][PREBLOCK_LOOP] PASS count=100" 600 stress
    send_complete "synctest preblock-timer-noise 100" \
      "[SYNCTEST][PREBLOCK_TIMER_NOISE] PASS count=100" 600 stress
    send_complete "inputtest preblock-loop 100" \
      "[INPUTTEST][PREBLOCK_LOOP] PASS count=100" 600 stress
    send_complete "modaltest kill-caller-loop 100 blocked" \
      "[MODALTEST][KILL_CALLER_LOOP] PASS mode=BLOCKED count=100" 600 stress
    send_complete "modaltest kill-caller-loop 100 prewait" \
      "[MODALTEST][KILL_CALLER_LOOP] PASS mode=PREWAIT count=100" 600 stress
    send_complete "modaltest kill-caller-loop 100 completion-ready" \
      "[MODALTEST][KILL_CALLER_LOOP] PASS mode=COMPLETION_READY count=100" 600 stress
    checks
    artifact="artifacts/build/cl11fix11-heavy-run${run}-smp8.log"
    cp "$serial" "$artifact"
    assert_clean_log "$artifact"
  done
  echo "[CL10][FIX11_EXACT_CERT] PASS runs=5" >>"$serial"
  cp "$serial" artifacts/build/cl11fix11-heavy-runs-smp8.log
  stop_qemu
}
taskman_session(){
  local refresh=${1:-50} begin end
  begin=$(count "[MODAL] session_begin OK"); end=$(count "[MODAL] session_end OK")
  hmp_text "taskman $refresh" normal --enter >/dev/null
  wait_new "[MODAL] session_begin OK" "$begin" 60
  sleep 2
  hmp_key esc normal >/dev/null; hmp_key esc normal >/dev/null
  wait_new "[MODAL] session_end OK" "$end" 120
  shell_sync
}
stats_value(){
  local line=$1 key=$2 regex
  regex="(^|[[:space:]])${key}=([0-9]+)($|[[:space:]])"
  [[ $line =~ $regex ]] || return 1
  printf '%s\n' "${BASH_REMATCH[2]}"
}
auto_exit_session(){
  local target=$1
  send_complete "taskmantest anchor-reset" \
    "[TASKMANTEST][ANCHOR_RESET] PASS" 60
  send_complete "taskmantest auto-exit-frames $target" \
    "[TASKMANTEST][AUTO_EXIT_ARM] PASS frames=$target" 60
  send_complete "taskman 50" \
    "[TASKMAN][AUTO_EXIT] PASS target=$target full_frames=$target fallback_frames=0" \
    180
}
soak_probe(){
  local sessions=0 line value
  make image >/dev/null
  start_qemu 8 tcg
  send_complete "taskmantest heap-begin" \
    "[TASKMANTEST][HEAP_BEGIN] PASS" 60
  send_complete "taskmantest setup 129" \
    "[TASKMANTEST][SETUP] PASS count=129" 180
  send_complete "taskmantest stats-reset" \
    "[TASKMANTEST][STATS_RESET] PASS" 60
  while ((sessions<5)); do
    auto_exit_session 10
    sessions=$((sessions+1))
  done
  [[ $(count "[TASKMAN][AUTO_EXIT] PASS target=10 full_frames=10 fallback_frames=0") == 5 ]]
  send_complete "taskmantest stats" "[TASKMANTEST][STATS]" 60
  line=$(grep -F '[TASKMANTEST][STATS]' "$serial" | tail -1)
  value=$(stats_value "$line" sessions); ((value==5))
  value=$(stats_value "$line" full_frames); ((value>=50))
  value=$(stats_value "$line" fallback_frames); ((value==0))
  value=$(stats_value "$line" auto_exit_sessions); ((value==5))
  value=$(stats_value "$line" auto_exit_shortfalls); ((value==0))
  value=$(stats_value "$line" render_failures); ((value==0))
  value=$(stats_value "$line" scroll_delta); ((value==0))
  value=$(stats_value "$line" last_session_scroll_delta); ((value==0))
  value=$(stats_value "$line" max_frame_gap_ms); ((value<=5000))
  value=$(stats_value "$line" pages); ((value>=2))
  value=$(stats_value "$line" captured); ((value>=129))
  send_complete "taskmantest cleanup" \
    "[TASKMANTEST][CLEANUP] PASS removed=129" 180
  send_complete "taskmantest heap-end" \
    "[TASKMANTEST][HEAP] PASS" 180
  send_complete "taskmantest check" \
    "[TASKMANTEST][CHECK] PASS" 60
  cp "$serial" artifacts/build/cl11fix16-soak-probe-smp8.log
  assert_clean_log artifacts/build/cl11fix16-soak-probe-smp8.log
  stop_qemu
}
fixture_matrix(){
  make image >/dev/null
  local smp boot
  for smp in 1 8; do
    for boot in 1 2 3; do
      start_qemu "$smp" tcg
      send_complete "reaptest fixture-loop 1000" "[REAPTEST][FIXTURE_LOOP] PASS count=1000" 180
      send_complete "reaptest grace" "[REAPTEST][GRACE] PASS" 90
      send_complete "reaptest check" "[REAPTEST][CHECK] PASS" 60
      cp "$serial" "artifacts/build/cl11fix2-fixture-smp${smp}-boot${boot}.log"
      stop_qemu
    done
  done
}
geometry_runtime(){
  local spec cols rows
  for spec in 'wide 160 30' 'compact 90 25' 'narrow 60 25' 'short 160 4'; do
    read -r mode cols rows <<<"$spec"
    send_complete "taskmantest geometry-runtime $cols $rows" "[TASKMANTEST][GEOMETRY_RUNTIME] PASS" 60
    local begin end; begin=$(count "[MODAL] session_begin OK"); end=$(count "[MODAL] session_end OK")
    hmp_text "taskman 50" normal --enter >/dev/null; wait_new "[MODAL] session_begin OK" "$begin" 60
    hmp_text q normal --enter >/dev/null
    for key in left right; do hmp_key "$key" normal >/dev/null; done
    sleep 1; [[ $(count "[MODAL] session_end OK") == "$end" ]]
    hmp_key esc normal >/dev/null; wait_new "[MODAL] session_end OK" "$end" 120; shell_sync
    send_complete "taskmantest geometry-clear" "[TASKMANTEST][GEOMETRY_RUNTIME] CLEARED" 60
    send_complete "taskmantest check" "[TASKMANTEST][CHECK] PASS" 60
  done
}
refresh_cli(){
  taskman_session 50; taskman_session 1000; taskman_session 2000
  local begin end; begin=$(count "[MODAL] session_begin OK"); end=$(count "[MODAL] session_end OK")
  hmp_text taskman normal --enter >/dev/null; wait_new "[MODAL] session_begin OK" "$begin" 60
  hmp_key esc normal >/dev/null; wait_new "[MODAL] session_end OK" "$end" 120; shell_sync
  local bad before
  for bad in 0 49 2001 -1 +1 abc 4294967296; do before=$(count 'taskman: refresh_ms must be 50..2000'); hmp_text "taskman $bad" normal --enter >/dev/null; wait_new 'taskman: refresh_ms must be 50..2000' "$before" 30; shell_sync; done
  before=$(count 'Usage: taskman [refresh_ms]'); hmp_text 'taskman 100 extra' normal --enter >/dev/null; wait_new 'Usage: taskman [refresh_ms]' "$before" 30; shell_sync
}
zombie_live(){
  send_complete "reaptest zombie-mem setup-auto" "[REAPTEST][ZOMBIE_MEM_SETUP] PASS" 60
  taskman_session 50
  send_complete "reaptest zombie-mem cleanup" "[TASKMANTEST][ZOMBIE_LIVE] PASS" 60
}
navigation(){
  send_complete "taskmantest heap-begin" "[TASKMANTEST][HEAP_BEGIN] PASS" 60
  send_complete "taskmantest setup 129" "[TASKMANTEST][SETUP] PASS count=129" 180
  local begin end; begin=$(count "[MODAL] session_begin OK"); end=$(count "[MODAL] session_end OK")
  hmp_text "taskman 50" normal --enter >/dev/null
  wait_new "[MODAL] session_begin OK" "$begin" 60
  hmp_text q normal --enter >/dev/null
  for key in left right down up pgdn pgup end home; do hmp_key "$key" normal >/dev/null; done
  sleep 3
  hmp_key esc normal >/dev/null; hmp_key esc normal >/dev/null; hmp_key esc normal >/dev/null
  wait_new "[MODAL] session_end OK" "$end" 120
  shell_sync
  send_complete "taskmantest stats" "[TASKMANTEST][STATS]" 60
  local line; line=$(grep -F '[TASKMANTEST][STATS]' "$serial"|tail -1)
  [[ $line == *'pages='* && $line != *'pages=1 '* && $line == *'truncated=0'* && $line == *'scroll_delta=0'* ]]
  send_complete "taskmantest cap 64" "[TASKMANTEST][CAP] PASS" 60
  taskman_session 50
  send_complete "taskmantest stats" "[TASKMANTEST][STATS]" 60
  line=$(grep -F '[TASKMANTEST][STATS]' "$serial"|tail -1)
  [[ $line == *'captured=64 '* && $line == *'truncated=1'* ]]
  send_complete "taskmantest cap 4096" "[TASKMANTEST][CAP] PASS" 60
  send_complete "taskmantest fail-next-allocation" "[TASKMANTEST][ALLOC_FAIL] PASS armed=1" 60
  taskman_session 50
  send_complete "taskmantest stats" "[TASKMANTEST][STATS]" 60
  line=$(grep -F '[TASKMANTEST][STATS]' "$serial"|tail -1)
  [[ $line =~ render_failures=([1-9][0-9]*) ]]
  send_complete "modaltest arm-kill-next-ui" "[MODALTEST][ARM_KILL_NEXT_UI] OK" 60
  local killed_begin killed_end
  killed_begin=$(count "[MODAL] session_begin OK"); killed_end=$(count "[MODAL] session_end OK")
  hmp_text "taskman 50" normal --enter >/dev/null
  wait_new "[MODAL] session_begin OK" "$killed_begin" 60
  wait_new "[MODAL] session_end OK" "$killed_end" 120
  shell_sync
  send_complete "taskmantest kill-render" "[TASKMANTEST][KILL_RENDER] PASS" 60
  send_complete "taskmantest check" "[TASKMANTEST][CHECK] PASS" 60
  send_complete "taskmantest churn-start 10000" "[TASKMANTEST][CHURN_LIVE] START rounds=10000" 60
  local churn_done churn_begin churn_end churn_start_ms
  churn_done=$(count "[TASKMANTEST][CHURN_LIVE] COMPLETE rounds=10000")
  churn_begin=$(count "[MODAL] session_begin OK"); churn_end=$(count "[MODAL] session_end OK")
  hmp_text "taskman 50" normal --enter >/dev/null; wait_new "[MODAL] session_begin OK" "$churn_begin" 60
  churn_start_ms=$(( $(date +%s%N)/1000000 ))
  while [[ $(count "[TASKMANTEST][CHURN_LIVE] COMPLETE rounds=10000") == "$churn_done" ]]; do
    for _ in $(seq 1 10); do hmp_key down stress >/dev/null; hmp_key pgdn stress >/dev/null; done
    hmp_key end stress >/dev/null; hmp_key home stress >/dev/null; sleep 2
    if [[ $(count "[TASKMANTEST][CHURN_LIVE] COMPLETE rounds=10000") != "$churn_done" ]]; then
      break
    fi
    if (( $(date +%s%N)/1000000-churn_start_ms >= 1800000 )); then
      echo "CL11 live churn timed out after 1800000 ms" >&2
      return 1
    fi
  done
  hmp_key esc normal >/dev/null; wait_new "[MODAL] session_end OK" "$churn_end" 120; shell_sync
  send_complete "taskmantest churn-status wait" "[TASKMANTEST][CHURN_LIVE] PASS rounds=10000" 1500
  send_complete "taskmantest churn-stop" "[TASKMANTEST][CHURN_LIVE] STOPPED" 60
  zombie_live
  geometry_runtime
  refresh_cli
  send_complete "taskmantest clean-region" "[TASKMANTEST][CLEAN_REGION] PASS" 60
  send_complete "taskmantest cleanup" "[TASKMANTEST][CLEANUP] PASS removed=129" 180
  send_complete "taskmantest heap-end" "[TASKMANTEST][HEAP] PASS" 180
}
positive(){
  make image >/dev/null
  local smp accel
  for smp in ${CL11_SMPS:-1 2 4 8}; do
    accel=tcg; [[ $smp == 2 && -w /dev/kvm ]] && accel=kvm
    start_qemu "$smp" "$accel"
    if ((smp==1 || smp==2 || smp==8)); then
      send_complete "accounttest reaper-quiescence 32" "[ACCOUNT][REAPER_QUIESCENCE] PASS workers=32 gone=32" 90
    fi
    send_complete "taskmantest all" "[TASKMANTEST][CHECK] PASS" 180
    ((smp>=2)) && send_complete "taskmantest churn 10000" "[TASKMANTEST][CHURN] PASS rounds=10000" 180
    if ((smp>=4)); then
      navigation
      auto_exit_session 3
    fi
    checks
    cp "$serial" "artifacts/build/cl11-matrix-smp${smp}-${accel}.log"
    if ((smp>=4)); then
      echo '[TASKMANTEST][GEOMETRY_RUNTIME] PASS modes=4' >>"artifacts/build/cl11-matrix-smp${smp}-${accel}.log"
      echo '[TASKMANTEST][REFRESH_CLI] PASS valid=4 invalid=8' >>"artifacts/build/cl11-matrix-smp${smp}-${accel}.log"
    fi
    assert_clean_log "artifacts/build/cl11-matrix-smp${smp}-${accel}.log"
    stop_qemu
  done
}
negative_one(){
  local macro=$1 command=$2 sentinel=$3 artifact=$4
  make kernel-check JOBS=2 KERNEL_EXTRA_CFLAGS="-D$macro" >/dev/null
  make image KERNEL_EXTRA_CFLAGS="-D$macro" >/dev/null
  start_qemu 2 tcg; send_raw_complete "$command" "$sentinel" 90
  cp "$serial" "artifacts/build/$artifact"; stop_qemu
  make kernel-check JOBS=2 >/dev/null; make image >/dev/null
}
negatives(){
  negative_one HOBBYOS_TASKMAN_NEGATIVE_RENDER_ALL_ROWS "taskmantest model" "[TASKMANTEST][NEGATIVE] PANEL_OVERFLOW_DETECTED" cl11-negative-render-all.log
  negative_one HOBBYOS_TASKMAN_NEGATIVE_EXIT_ANY_KEY "taskmantest input" "[TASKMANTEST][NEGATIVE] NON_ESC_EXIT_DETECTED" cl11-negative-any-key.log
  negative_one HOBBYOS_TASKMAN_NEGATIVE_FIXED_128 "taskmantest model" "[TASKMANTEST][NEGATIVE] HIDDEN_TRUNCATION_DETECTED" cl11-negative-fixed128.log
  negative_one HOBBYOS_TASKMAN_NEGATIVE_NO_PAGE_CLAMP "taskmantest pagination" "[TASKMANTEST][NEGATIVE] PAGE_CLAMP_MISSING_DETECTED" cl11-negative-page-clamp.log
  start_qemu 2 tcg
  send_complete "taskmantest check" "[TASKMANTEST][CHECK] PASS" 60
  send_complete "taskmantest stats" "[TASKMANTEST][STATS]" 60
  grep -Eq '\[TASKMANTEST\]\[STATS\].*auto_exit_pending=0' "$serial"
  cp "$serial" artifacts/build/cl11-negative-reset.log
  assert_clean_log artifacts/build/cl11-negative-reset.log
  stop_qemu
}
soak(){
  local duration=${DURATION_MS:-180000} start duration_ms sessions=0 full_frame_target=0
  local line value sessions_stat full_frames fallback_frames auto_exit_sessions
  local shortfalls max_gap modal_scroll shell_scroll
  ((duration>=180000)); start_qemu 8 tcg
  send_complete "taskmantest heap-begin" "[TASKMANTEST][HEAP_BEGIN] PASS" 60
  send_complete "taskmantest setup 129" "[TASKMANTEST][SETUP] PASS count=129" 180
  send_complete "taskmantest stats-reset" "[TASKMANTEST][STATS_RESET] PASS" 60
  start=$(( $(date +%s%N)/1000000 ))
  while (( $(date +%s%N)/1000000-start < duration || sessions < 100 || full_frame_target < 1500 )); do
    auto_exit_session 15
    sessions=$((sessions+1))
    full_frame_target=$((full_frame_target+15))
  done
  duration_ms=$(( $(date +%s%N)/1000000-start ))
  send_complete "taskmantest stats" "[TASKMANTEST][STATS]" 60
  line=$(grep -F '[TASKMANTEST][STATS]' "$serial" | tail -1)
  sessions_stat=$(stats_value "$line" sessions); ((sessions_stat>=sessions))
  full_frames=$(stats_value "$line" full_frames); ((full_frames>=full_frame_target && full_frames>=1500))
  fallback_frames=$(stats_value "$line" fallback_frames); ((fallback_frames==0))
  auto_exit_sessions=$(stats_value "$line" auto_exit_sessions); ((auto_exit_sessions>=sessions))
  shortfalls=$(stats_value "$line" auto_exit_shortfalls); ((shortfalls==0))
  value=$(stats_value "$line" render_failures); ((value==0))
  modal_scroll=$(stats_value "$line" scroll_delta); ((modal_scroll==0))
  value=$(stats_value "$line" last_session_scroll_delta); ((value==0))
  max_gap=$(stats_value "$line" max_frame_gap_ms); ((max_gap<=5000))
  value=$(stats_value "$line" pages); ((value>=2))
  value=$(stats_value "$line" captured); ((value>=129))
  value=$(stats_value "$line" truncated); ((value==0))
  [[ $line == *' last_mode=WIDE '* || $line == *' last_mode=COMPACT '* ]]
  shell_scroll=$(stats_value "$line" shell_exit_scroll_delta)
  send_complete "taskmantest cleanup" "[TASKMANTEST][CLEANUP] PASS removed=129" 180
  send_complete "taskmantest heap-end" "[TASKMANTEST][HEAP] PASS" 180
  send_complete "taskmantest stress 10000 10000 100 100 100" "[TASKMANTEST][STRESS] PASS navigation=10000 churn=10000 zombies=100 kill_render=100 layouts=100" 1500
  send_complete "reaptest snapshot-boundary 20" "[REAPTEST][SNAPSHOT_BOUNDARY_LOOP] PASS rounds=20 verified=20 failures=0" 180 stress
  send_complete "reaptest timer-ref-loop 100" \
    "[REAPTEST][TIMER_REF_LOOP] PASS count=100" 1200 stress
  send_complete "synctest preblock-loop 100 all" \
    "[SYNCTEST][PREBLOCK_LOOP] PASS count=100" 600 stress
  send_complete "synctest preblock-timer-noise 100" \
    "[SYNCTEST][PREBLOCK_TIMER_NOISE] PASS count=100" 600 stress
  send_complete "inputtest preblock-loop 100" \
    "[INPUTTEST][PREBLOCK_LOOP] PASS count=100" 600 stress
  send_complete "modaltest kill-caller-loop 100 blocked" \
    "[MODALTEST][KILL_CALLER_LOOP] PASS mode=BLOCKED count=100" 600 stress
  send_complete "modaltest kill-caller-loop 100 prewait" \
    "[MODALTEST][KILL_CALLER_LOOP] PASS mode=PREWAIT count=100" 600 stress
  send_complete "modaltest kill-caller-loop 100 completion-ready" \
    "[MODALTEST][KILL_CALLER_LOOP] PASS mode=COMPLETION_READY count=100" 600 stress
  local clock_batches=0 reaper_batches=0
  for _ in 1 2 3; do
    send_complete "accounttest clock-smp 32 1000000" "[ACCOUNT][CLOCK_SMP] PASS" 600 stress
    send_complete "accounttest reaper-quiescence 32" "[ACCOUNT][REAPER_QUIESCENCE] PASS workers=32 gone=32" 90 stress
    clock_batches=$((clock_batches+1)); reaper_batches=$((reaper_batches+1))
  done
  checks
  ((duration_ms>=180000 && sessions>=100 && full_frame_target>=1500))
  echo "[CL11][SOAK] PASS duration_ms=$duration_ms sessions=$sessions full_frame_target=$full_frame_target full_frames=$full_frames fallback_frames=$fallback_frames auto_exit_sessions=$auto_exit_sessions auto_exit_shortfalls=$shortfalls max_frame_gap_ms=$max_gap modal_scroll_delta=$modal_scroll shell_exit_scroll_delta=$shell_scroll navigation=10000 churn=10000 zombies=100 kill_render=100 layouts=100 snapshot_boundary_verified=20 clock_smp_batches=$clock_batches timer_ref_cycles=100 preblock_sem=100 preblock_timer=100 preblock_input=100 modal_blocked=100 modal_prewait=100 modal_completion_ready=100 reaper_quiescence_batches=$reaper_batches" >>"$serial"
  cp "$serial" artifacts/build/cl11-soak-smp8.log; assert_clean_log artifacts/build/cl11-soak-smp8.log; stop_qemu
}
final_build(){
  local build_log=artifacts/build/cl11fix16-final-build.log
  : >"$build_log"
  make stack-check | tee -a "$build_log"
  make kernel-check JOBS=2 | tee -a "$build_log"
  cp kernel.elf /tmp/hobbyos-cl11fix16-j2.elf
  make kernel-check JOBS="$(nproc)" | tee -a "$build_log"
  cp kernel.elf /tmp/hobbyos-cl11fix16-jN.elf
  sha256sum /tmp/hobbyos-cl11fix16-j2.elf \
    /tmp/hobbyos-cl11fix16-jN.elf \
    | tee artifacts/build/cl11fix16-reproducible-hashes.log \
    | tee -a "$build_log"
  cmp -s /tmp/hobbyos-cl11fix16-j2.elf \
    /tmp/hobbyos-cl11fix16-jN.elf
  make deps-check | tee -a "$build_log"
  make image | tee -a "$build_log"
  [[ -z $(nm -u kernel.elf | tee -a "$build_log") ]]
  sha256sum kernel.elf hobbyos.img \
    | tee artifacts/build/cl11fix16-final-hashes.log \
    | tee -a "$build_log"
  ! grep -E 'cmd_taskman(test)?\.c:.*warning:' "$build_log"
}
case ${1:-all} in
  fixture) fixture_matrix;; positive) positive;; negatives) negatives;;
  exact) preblock_focused; preblock_loops; modal_exact_loops;;
  preblock-negative) negative_preblock;; heavy) heavy_runs;;
  soak-probe) soak_probe;; soak) soak;; final-build) final_build;;
  all)
    make stack-check
    preblock_focused
    preblock_loops
    modal_exact_loops
    negative_preblock
    heavy_runs
    fixture_matrix
    harness_unlock
    scripts/test-cl07-matrix.sh
    scripts/test-cl08.sh all
    scripts/test-cl09.sh all
    scripts/test-cl10.sh all
    scripts/test-hmp-transport.sh
    harness_lock
    [[ ! -S .qemu/hmp.sock&&! -e .qemu/qemu.pid ]]
    positive
    negatives
    soak
    final_build
    echo "[CL11][CERTIFICATION] PASS";;
  *) echo "usage: $0 fixture|positive|negatives|exact|preblock-negative|heavy|soak-probe|soak|final-build|all" >&2; exit 2;;
esac
