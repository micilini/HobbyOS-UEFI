#!/usr/bin/env bash
set -Eeuo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

artifact_dir=artifacts/build
expected_branch=feat/taskman
expected_base=88645901d9393f4796af84cecd9e13d3d6671c70
fix16_runner=$artifact_dir/cl11fix16-final-runner.log
protected_before=$artifact_dir/cl11fix16-protected-core-before.sha256
protected_after_tests=$artifact_dir/cl11fix18-protected-core-after-tests.sha256
protected_after_build=$artifact_dir/cl11fix18-protected-core-after-build.sha256
authorized_before=$artifact_dir/cl11fix18-authorized-runtime-before.sha256
authorized_after_build=$artifact_dir/cl11fix18-authorized-runtime-after-build.sha256
runner_log=$artifact_dir/cl11fix18-resume-after-soak.log
evidence_log=$artifact_dir/cl11fix18-evidence-ledger.log
reap_log=$artifact_dir/cl11fix18-reap-baseline.log
stage=entry
preflight=

mkdir -p "$artifact_dir"
: >"$runner_log"
: >"$evidence_log"
: >"$reap_log"

record(){
  printf '[CL11][FIX18][%s] PASS %s\n' "$1" "$2" |
    tee -a "$runner_log" "$evidence_log"
}

failure_status(){
  case "$stage" in
    evidence-missing) echo REJECTED_EVIDENCE_MISSING;;
    reap-baseline) echo REJECTED_REAP_BASELINE_UNSTABLE;;
    reap-architecture) echo REJECTED_REAP_BASELINE_ARCHITECTURE_CHANGED;;
    protected-*|authorized-*) echo REJECTED_SOURCE_CONTINUITY;;
    final-build) echo REJECTED_FINAL_BUILD;;
    *) echo REJECTED_EXISTING_EVIDENCE;;
  esac
}

cleanup(){
  [[ -z ${preflight:-} || ! -e $preflight ]] || rm -f "$preflight"
}

on_exit(){
  local rc=$1
  cleanup
  if ((rc!=0)); then
    printf '[CL11][FIX18] FAIL status=%s stage=%s\n' \
      "$(failure_status)" "$stage" | tee -a "$runner_log" "$evidence_log"
  fi
}
trap 'on_exit $?' EXIT

require_file(){ [[ -f $1 ]]; }
require_marker(){ grep -Fq -- "$2" "$1"; }
assert_no_faults(){ ! grep -Eq 'PANIC|#PF|#GP|FATAL|STRUCTURAL_FAULT|FINISH_FAULT' "$1"; }
assert_no_hard_faults(){ ! grep -Eq 'PANIC|#PF|#GP|FATAL' "$1"; }

value_from_line(){
  local line=$1 key=$2 regex
  regex="(^|[[:space:]])${key}=([0-9]+)($|[[:space:]])"
  [[ $line =~ $regex ]]
  printf '%s\n' "${BASH_REMATCH[2]}"
}

parse_reap_check(){
  local file=$1 line

  line=$(grep -F '[REAPTEST][CHECK] PASS' "$file" | tail -1)
  [[ -n $line ]]

  REAP_REFS=$(value_from_line "$line" refs)
  REAP_INFLIGHT=$(value_from_line "$line" inflight)
  REAP_BACKLOG=$(value_from_line "$line" backlog)
  REAP_STRUCTURAL=$(value_from_line "$line" structural)
  REAP_VIOLATIONS=$(value_from_line "$line" violations)

  ((REAP_INFLIGHT==0))
  ((REAP_BACKLOG==0))
  ((REAP_STRUCTURAL==0))
  ((REAP_VIOLATIONS==0))
}

matrix_file(){
  local smp=$1
  if [[ $smp == 2 && -f $artifact_dir/cl11-matrix-smp2-kvm.log ]]; then
    printf '%s\n' "$artifact_dir/cl11-matrix-smp2-kvm.log"
  else
    printf '%s\n' "$artifact_dir/cl11-matrix-smp${smp}-tcg.log"
  fi
}

derive_reap_ref_baseline(){
  local smp file refs baseline=

  : >"$reap_log"
  for smp in 1 2 4 8; do
    file=$(matrix_file "$smp")
    [[ -f $file ]]
    parse_reap_check "$file"
    refs=$REAP_REFS
    printf 'smp=%s file=%s refs=%s inflight=%s backlog=%s structural=%s violations=%s\n' \
      "$smp" "$file" "$refs" "$REAP_INFLIGHT" "$REAP_BACKLOG" \
      "$REAP_STRUCTURAL" "$REAP_VIOLATIONS" | tee -a "$reap_log"
    if [[ -z $baseline ]]; then
      baseline=$refs
    else
      ((refs==baseline))
    fi
  done

  [[ -n $baseline ]]
  ((baseline==1))
  REAP_REF_BASELINE=$baseline
  printf 'REAP_REF_BASELINE=%s\n' "$REAP_REF_BASELINE" | tee -a "$reap_log"
}

protected_file_list(){
  {
    find kernel bootloader shared -type f \
      \( -name '*.c' -o -name '*.h' -o -name '*.S' -o -name '*.s' \
         -o -name '*.ld' -o -name '*.inc' -o -name '*.asm' \) \
      ! -path 'kernel/src/shell/commands/cmd_taskman.c' \
      ! -path 'kernel/src/shell/commands/cmd_taskman.h' \
      ! -path 'kernel/src/shell/commands/cmd_taskmantest.c' \
      ! -path 'kernel/src/shell/commands/cmd_taskmantest.h' -print0
    printf 'makefile\0'
  } | LC_ALL=C sort -z
}

snapshot_protected(){
  local output=$1
  local -a files=()
  mapfile -d '' -t files < <(protected_file_list)
  ((${#files[@]}==193))
  sha256sum "${files[@]}" >"$output"
}

snapshot_authorized(){
  sha256sum \
    kernel/src/shell/commands/cmd_taskman.c \
    kernel/src/shell/commands/cmd_taskman.h \
    kernel/src/shell/commands/cmd_taskmantest.c \
    kernel/src/shell/commands/cmd_taskmantest.h >"$1"
}

verify_probe(){
  local file=$artifact_dir/cl11fix16-soak-probe-smp8.log line value
  [[ $(grep -Fc '[TASKMAN][AUTO_EXIT] PASS target=10 full_frames=10 fallback_frames=0' "$file") == 5 ]]
  grep -Eq '\[TASKMANTEST\]\[HEAP\] PASS.*drift=0' "$file"
  require_marker "$file" '[TASKMANTEST][CHECK] PASS'
  line=$(grep -F '[TASKMANTEST][STATS]' "$file" | tail -1)
  value=$(value_from_line "$line" sessions); ((value==5))
  value=$(value_from_line "$line" full_frames); ((value>=50))
  value=$(value_from_line "$line" fallback_frames); ((value==0))
  value=$(value_from_line "$line" auto_exit_sessions); ((value==5))
  value=$(value_from_line "$line" auto_exit_shortfalls); ((value==0))
  value=$(value_from_line "$line" render_failures); ((value==0))
  value=$(value_from_line "$line" scroll_delta); ((value==0))
  value=$(value_from_line "$line" last_session_scroll_delta); ((value==0))
  value=$(value_from_line "$line" max_frame_gap_ms); ((value<=5000))
  value=$(value_from_line "$line" pages); ((value>=2))
  value=$(value_from_line "$line" captured); ((value>=129))
  assert_no_faults "$file"
}

verify_positive(){
  local smp=$1 file marker
  file=$(matrix_file "$smp")
  for marker in \
    '[TASKMANTEST][CHECK] PASS' '[MODALTEST][CHECK] PASS' \
    '[INPUTTEST][CHECK] PASS' '[SCHED][CHECK] PASS' \
    '[SYNC][CHECK] PASS' '[ACCOUNT][CHECK] PASS' \
    '[KILLTEST][CHECK] PASS' '[REAPTEST][CHECK] PASS'; do
    require_marker "$file" "$marker"
  done
  parse_reap_check "$file"
  ((REAP_REFS==REAP_REF_BASELINE))
  if ((smp>=4)); then
    for marker in \
      '[TASKMAN][AUTO_EXIT] PASS target=3 full_frames=3 fallback_frames=0' \
      '[TASKMANTEST][SETUP] PASS count=129' \
      '[TASKMANTEST][CHURN_LIVE] PASS rounds=10000' \
      '[TASKMANTEST][ZOMBIE_LIVE] PASS' \
      '[TASKMANTEST][GEOMETRY_RUNTIME] PASS modes=4' \
      '[TASKMANTEST][REFRESH_CLI] PASS valid=4 invalid=8' \
      '[TASKMANTEST][CLEAN_REGION] PASS' \
      '[TASKMANTEST][KILL_RENDER] PASS' \
      '[TASKMANTEST][HEAP] PASS'; do
      require_marker "$file" "$marker"
    done
    grep -Eq '\[TASKMANTEST\]\[STATS\].*pages=([2-9]|[1-9][0-9]+).*truncated=0.*scroll_delta=0.*clipped=0' "$file"
    grep -Eq '\[TASKMANTEST\]\[CHECK\] PASS.*last_session_scroll_delta=0.*last_session_clipped_writes=0.*clipped_total=0.*stale_cells=0.*builder_truncations=0' "$file"
  fi
  assert_no_faults "$file"
}

verify_negatives(){
  local spec file marker
  for spec in \
    'cl11-negative-render-all.log|[TASKMANTEST][NEGATIVE] PANEL_OVERFLOW_DETECTED' \
    'cl11-negative-any-key.log|[TASKMANTEST][NEGATIVE] NON_ESC_EXIT_DETECTED' \
    'cl11-negative-fixed128.log|[TASKMANTEST][NEGATIVE] HIDDEN_TRUNCATION_DETECTED' \
    'cl11-negative-page-clamp.log|[TASKMANTEST][NEGATIVE] PAGE_CLAMP_MISSING_DETECTED'; do
    file=$artifact_dir/${spec%%|*}
    marker=${spec#*|}
    require_marker "$file" "$marker"
    assert_no_hard_faults "$file"
  done
  file=$artifact_dir/cl11-negative-reset.log
  require_marker "$file" '[TASKMANTEST][CHECK] PASS'
  grep -Eq '\[TASKMANTEST\]\[STATS\].*auto_exit_pending=0' "$file"
  assert_no_faults "$file"
}

verify_soak(){
  local file=$artifact_dir/cl11-soak-smp8.log line value marker target
  line=$(grep -F '[CL11][SOAK] PASS' "$file" | tail -1)
  [[ -n $line ]]
  value=$(value_from_line "$line" duration_ms); ((value>=180000))
  value=$(value_from_line "$line" sessions); ((value>=100)); local sessions=$value
  target=$(value_from_line "$line" full_frame_target); ((target>=1500))
  value=$(value_from_line "$line" full_frames); ((value>=target))
  value=$(value_from_line "$line" fallback_frames); ((value==0))
  value=$(value_from_line "$line" auto_exit_sessions); ((value>=sessions))
  value=$(value_from_line "$line" auto_exit_shortfalls); ((value==0))
  value=$(value_from_line "$line" max_frame_gap_ms); ((value<=5000))
  value=$(value_from_line "$line" modal_scroll_delta); ((value==0))
  for marker in \
    navigation:10000 churn:10000 zombies:100 kill_render:100 layouts:100 \
    snapshot_boundary_verified:20 clock_smp_batches:3 timer_ref_cycles:100 \
    preblock_sem:100 preblock_timer:100 preblock_input:100 \
    modal_blocked:100 modal_prewait:100 modal_completion_ready:100; do
    value=$(value_from_line "$line" "${marker%%:*}")
    ((value>=${marker##*:}))
  done
  for marker in \
    '[TASKMANTEST][STRESS] PASS navigation=10000 churn=10000 zombies=100 kill_render=100 layouts=100' \
    '[REAPTEST][SNAPSHOT_BOUNDARY_LOOP] PASS rounds=20 verified=20 failures=0' \
    '[REAPTEST][TIMER_REF_LOOP] PASS count=100' \
    '[SYNCTEST][PREBLOCK_LOOP] PASS count=100' \
    '[SYNCTEST][PREBLOCK_TIMER_NOISE] PASS count=100' \
    '[INPUTTEST][PREBLOCK_LOOP] PASS count=100' \
    '[MODALTEST][KILL_CALLER_LOOP] PASS mode=BLOCKED count=100' \
    '[MODALTEST][KILL_CALLER_LOOP] PASS mode=PREWAIT count=100' \
    '[MODALTEST][KILL_CALLER_LOOP] PASS mode=COMPLETION_READY count=100' \
    '[TASKMANTEST][CHECK] PASS' '[MODALTEST][CHECK] PASS' \
    '[INPUTTEST][CHECK] PASS' '[SCHED][CHECK] PASS' \
    '[SYNC][CHECK] PASS' '[ACCOUNT][CHECK] PASS' \
    '[KILLTEST][CHECK] PASS' '[REAPTEST][CHECK] PASS'; do
    require_marker "$file" "$marker"
  done
  grep -Eq '\[TASKMANTEST\]\[CHECK\] PASS.*last_session_scroll_delta=0.*last_session_clipped_writes=0.*auto_exit_shortfalls=0.*controls_default=1' "$file"
  grep -Eq '\[INPUTTEST\]\[CHECK\] PASS.*phantom=0.*route_mismatch=0.*violations=0' "$file"
  grep -Eq '\[KILLTEST\]\[CHECK\] PASS.*pending=0.*duplicate=0.*contexts=0.*quarantine=0.*holds=0' "$file"
  parse_reap_check "$file"
  ((REAP_REFS==REAP_REF_BASELINE))
  assert_no_faults "$file"
}

verify_final_build(){
  local build_log=$artifact_dir/cl11fix16-final-build.log pass_count
  require_file "$build_log"
  require_file "$artifact_dir/cl11fix16-reproducible-hashes.log"
  require_file "$artifact_dir/cl11fix16-final-hashes.log"
  require_marker "$build_log" 'stack-check: PASS'
  pass_count=$(grep -Fc 'kernel-check: PASS' "$build_log")
  ((pass_count>=2))
  FINAL_MAX_FRAME=$(awk '/^TOP 20$/ { getline; print $1; exit }' "$build_log")
  [[ $FINAL_MAX_FRAME =~ ^[0-9]+$ ]]
  ((FINAL_MAX_FRAME<=2048))
  cmp -s /tmp/hobbyos-cl11fix16-j2.elf /tmp/hobbyos-cl11fix16-jN.elf
  require_marker "$build_log" '[INFO] scope=all'
  [[ -f kernel.elf && -f hobbyos.img ]]
  [[ -z $(nm -u kernel.elf) ]]
  ! grep -E 'cmd_taskman(test)?\.c:.*warning:' "$build_log"
  FINAL_KERNEL_HASH=$(sha256sum kernel.elf | awk '{print $1}')
  FINAL_IMAGE_HASH=$(sha256sum hobbyos.img | awk '{print $1}')
  require_marker "$artifact_dir/cl11fix16-final-hashes.log" "$FINAL_KERNEL_HASH  kernel.elf"
  require_marker "$artifact_dir/cl11fix16-final-hashes.log" "$FINAL_IMAGE_HASH  hobbyos.img"
}

[[ $# == 0 ]] || { echo "usage: $0" >&2; exit 2; }

stage=entry
[[ $(git branch --show-current) == "$expected_branch" ]]
[[ $(git rev-parse HEAD) == "$expected_base" ]]
git diff --cached --quiet
[[ ! -e .qemu/qemu.pid && ! -S .qemu/hmp.sock ]]
! pgrep -f '[q]emu-system-x86_64' >/dev/null
record PREFLIGHT "branch=$expected_branch base=$expected_base staged=0 qemu=0"

stage=evidence-missing
for file in \
  "$fix16_runner" "$artifact_dir/cl11fix16-soak-probe-smp8.log" \
  "$artifact_dir/cl11-matrix-smp1-tcg.log" \
  "$artifact_dir/cl11-matrix-smp4-tcg.log" \
  "$artifact_dir/cl11-matrix-smp8-tcg.log" \
  "$artifact_dir/cl11-negative-render-all.log" \
  "$artifact_dir/cl11-negative-any-key.log" \
  "$artifact_dir/cl11-negative-fixed128.log" \
  "$artifact_dir/cl11-negative-page-clamp.log" \
  "$artifact_dir/cl11-negative-reset.log" \
  "$artifact_dir/cl11-soak-smp8.log" "$protected_before" \
  "$artifact_dir/cl11fix16-authorized-runtime-before.sha256" \
  "$artifact_dir/cl11fix17-focused-smoke.log"; do
  require_file "$file"
done
require_file "$(matrix_file 2)"

stage=authorized-before
snapshot_authorized "$authorized_before"
record AUTHORIZED_BEFORE "manifest=$(sha256sum "$authorized_before" | awk '{print $1}')"

stage=protected-preflight
preflight=$(mktemp)
snapshot_protected "$preflight"
cmp -s "$protected_before" "$preflight"
record PROTECTED_PREFLIGHT "manifest=$(sha256sum "$protected_before" | awk '{print $1}') files=193"

stage=reap-architecture
grep -Eq '#define TASK_REAPER_PERIOD_MS[[:space:]]+2000u' kernel/src/core/scheduler.c
grep -Eq 'timer_sleep\(TASK_REAPER_PERIOD_MS\)' kernel/src/core/scheduler.c
grep -Fq 'refs_acquired!=r.refs_released+r.refs_current' kernel/src/shell/commands/cmd_reaptest.c
grep -Fq 'out->refs_current+=t->task_wake_timer_refs' kernel/src/core/scheduler.c
grep -Fq 'r.timer_ref_underflows||ts.task_ref_release_failures||ts.task_ref_duplicate_release' kernel/src/shell/commands/cmd_reaptest.c
record REAP_ARCHITECTURE 'period_ms=2000 snapshot=sum(task_wake_timer_refs) balance=acquired-released-current'

stage=runner-evidence
for marker in protected-freeze smoke probe positive negatives; do
  require_marker "$fix16_runner" "[CL11][FIX16][$marker] PASS"
done
old_failure='[CL11][FIX16_FINAL] FAIL status=REJECTED_SOAK_RUNTIME stage=soak'
[[ $(grep -Fc "$old_failure" "$fix16_runner") == 1 ]]
[[ $(grep -Fc '[CL11][FIX16_FINAL] FAIL' "$fix16_runner") == 1 ]]

stage=probe
verify_probe
record PROBE 'sessions=5 full_frames=50 fallback=0 scroll=0 gap<=5000 heap=0'

stage=reap-baseline
derive_reap_ref_baseline

stage=positive
for smp in 1 2 4 8; do verify_positive "$smp"; done
record POSITIVE 'smp=1,2,4,8 subsystem-checks=PASS refs=1'

stage=negatives
verify_negatives
record NEGATIVES 'causal=4 reset=PASS auto-exit-pending=0'

stage=soak
verify_soak
soak_line=$(grep -F '[CL11][SOAK] PASS' "$artifact_dir/cl11-soak-smp8.log" | tail -1)
printf '[CL11][REAP_BASELINE] PASS baseline=%s soak=%s balanced_by_guest=1\n' \
  "$REAP_REF_BASELINE" "$REAP_REFS" | tee -a "$runner_log" "$evidence_log" "$reap_log"

stage=protected-after-tests
snapshot_protected "$protected_after_tests"
cmp -s "$protected_before" "$protected_after_tests"
record PROTECTED_AFTER_TESTS "manifest=$(sha256sum "$protected_after_tests" | awk '{print $1}') files=193"

record EVIDENCE 'runner-through-negatives=PASS probe=PASS positive=PASS negatives=PASS soak=PASS faults=0'
record REAP_BASELINE "refs=$REAP_REF_BASELINE"
record SOAK_REVALIDATED "$soak_line"
printf '[CL11][SOAK_MODEL] PASS\n' | tee -a "$runner_log" "$evidence_log"

stage=final-build
scripts/test-cl11.sh final-build
verify_final_build
record FINAL_BUILD "stack=PASS max_frame=$FINAL_MAX_FRAME j2=PASS jN=PASS cmp=0 deps=PASS image=PASS nm=0 warnings=0"

stage=protected-after-build
snapshot_protected "$protected_after_build"
snapshot_authorized "$authorized_after_build"
cmp -s "$protected_before" "$protected_after_tests"
cmp -s "$protected_before" "$protected_after_build"
cmp -s "$authorized_before" "$authorized_after_build"
record PROTECTED_AFTER_BUILD "manifest=$(sha256sum "$protected_after_build" | awk '{print $1}')"
record AUTHORIZED_AFTER_BUILD "manifest=$(sha256sum "$authorized_after_build" | awk '{print $1}')"

stage=report-ready
[[ ! -e .qemu/qemu.pid && ! -S .qemu/hmp.sock ]]
! pgrep -f '[q]emu-system-x86_64' >/dev/null
record REPORT_READY "kernel=$FINAL_KERNEL_HASH image=$FINAL_IMAGE_HASH qemu=0"

trap - EXIT
cleanup
