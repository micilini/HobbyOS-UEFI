#!/usr/bin/env bash
set -Eeuo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

artifact_dir=artifacts/build
expected_branch=feat/taskman
expected_base=88645901d9393f4796af84cecd9e13d3d6671c70
protected_before=$artifact_dir/cl11fix16-protected-core-before.sha256
protected_after_tests=$artifact_dir/cl11fix16-protected-core-after-tests.sha256
protected_after_build=$artifact_dir/cl11fix16-protected-core-after-build.sha256
authorized_before=$artifact_dir/cl11fix16-authorized-runtime-before.sha256
authorized_after_tests=$artifact_dir/cl11fix16-authorized-runtime-after-tests.sha256
authorized_after_build=$artifact_dir/cl11fix16-authorized-runtime-after-build.sha256
runner_log=$artifact_dir/cl11fix16-final-runner.log
smoke_log=$artifact_dir/cl11fix16-build-smoke.log
stage=entry
preflight=

mkdir -p "$artifact_dir"
: >"$runner_log"

record(){
  printf '[CL11][FIX16][%s] PASS %s\n' "$1" "$2" | tee -a "$runner_log"
}

cleanup(){
  if [[ -e .qemu/qemu.pid || -S .qemu/hmp.sock ]]; then
    scripts/qemu-agent.sh stop >/dev/null 2>&1 || true
  fi
  [[ -z ${preflight:-} || ! -e $preflight ]] || rm -f "$preflight"
}

failure_status(){
  case "$stage" in
    probe) echo REJECTED_SOAK_PROBE;;
    positive|negatives) echo REJECTED_TASKMAN_REGRESSION;;
    soak) echo REJECTED_SOAK_RUNTIME;;
    protected-*) echo REJECTED_PROTECTED_CORE_CHANGED;;
    smoke|final-build) echo REJECTED_FINAL_BUILD;;
    *) echo REJECTED_FINAL_BUILD;;
  esac
}

on_exit(){
  local rc=$1
  cleanup
  if ((rc!=0)); then
    printf '[CL11][FIX16_FINAL] FAIL status=%s stage=%s\n' \
      "$(failure_status)" "$stage" | tee -a "$runner_log"
  fi
}
trap 'on_exit $?' EXIT

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

require_marker(){ grep -Fq -- "$2" "$1"; }
assert_no_faults(){ ! grep -Eq 'PANIC|#PF|#GP|FATAL' "$1"; }
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

derive_reap_ref_baseline(){
  local smp accel file refs baseline=

  for smp in 1 2 4 8; do
    accel=tcg
    [[ $smp == 2 && -w /dev/kvm ]] && accel=kvm
    file=$artifact_dir/cl11-matrix-smp${smp}-${accel}.log
    [[ -f $file ]]
    parse_reap_check "$file"
    refs=$REAP_REFS
    if [[ -z $baseline ]]; then
      baseline=$refs
    else
      ((refs==baseline))
    fi
  done

  [[ -n $baseline ]]
  ((baseline==1))
  REAP_REF_BASELINE=$baseline
}

verify_probe(){
  local file=$artifact_dir/cl11fix16-soak-probe-smp8.log line value
  [[ -f $file ]]
  [[ $(grep -Fc '[TASKMAN][AUTO_EXIT] PASS target=10 full_frames=10 fallback_frames=0' "$file") == 5 ]]
  require_marker "$file" '[TASKMANTEST][HEAP] PASS'
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
  local smp=$1 accel=$2 file=$artifact_dir/cl11-matrix-smp${smp}-${accel}.log marker
  [[ -f $file ]]
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
    require_marker "$file" '[TASKMAN][AUTO_EXIT] PASS target=3 full_frames=3 fallback_frames=0'
    require_marker "$file" '[TASKMANTEST][SETUP] PASS count=129'
    require_marker "$file" '[TASKMANTEST][CHURN_LIVE] PASS rounds=10000'
    require_marker "$file" '[TASKMANTEST][HEAP] PASS'
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
    file=$artifact_dir/${spec%%|*}; marker=${spec#*|}
    [[ -f $file ]]; require_marker "$file" "$marker"; assert_no_faults "$file"
  done
  file=$artifact_dir/cl11-negative-reset.log
  require_marker "$file" '[TASKMANTEST][CHECK] PASS'
  grep -Eq '\[TASKMANTEST\]\[STATS\].*auto_exit_pending=0' "$file"
  assert_no_faults "$file"
}

verify_soak(){
  local file=$artifact_dir/cl11-soak-smp8.log line value marker
  [[ -f $file ]]
  line=$(grep -F '[CL11][SOAK] PASS' "$file" | tail -1)
  [[ -n $line ]]
  value=$(value_from_line "$line" duration_ms); ((value>=180000))
  value=$(value_from_line "$line" sessions); ((value>=100))
  value=$(value_from_line "$line" full_frame_target); ((value>=1500))
  local target=$value
  value=$(value_from_line "$line" full_frames); ((value>=target))
  value=$(value_from_line "$line" fallback_frames); ((value==0))
  value=$(value_from_line "$line" auto_exit_sessions); ((value>=100))
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
    '[INPUTTEST][CHECK] PASS' '[SYNC][CHECK] PASS' \
    '[ACCOUNT][CHECK] PASS' '[KILLTEST][CHECK] PASS' \
    '[REAPTEST][CHECK] PASS'; do
    require_marker "$file" "$marker"
  done
  grep -Eq '\[TASKMANTEST\]\[CHECK\] PASS.*last_session_scroll_delta=0.*last_session_clipped_writes=0.*auto_exit_shortfalls=0.*controls_default=1' "$file"
  grep -Eq '\[INPUTTEST\]\[CHECK\] PASS.*phantom=0.*route_mismatch=0.*violations=0' "$file"
  grep -Eq '\[KILLTEST\]\[CHECK\] PASS.*pending=0.*duplicate=0.*contexts=0.*quarantine=0.*holds=0' "$file"
  parse_reap_check "$file"
  ((REAP_REFS==REAP_REF_BASELINE))
  assert_no_faults "$file"
}

stage=protected-freeze
[[ $(git branch --show-current) == "$expected_branch" ]]
[[ $(git rev-parse HEAD) == "$expected_base" ]]
git diff --cached --quiet
[[ ! -e .qemu/qemu.pid && ! -S .qemu/hmp.sock ]]
! pgrep -f qemu-system-x86_64 >/dev/null
[[ -f $protected_before && -f $authorized_before ]]
preflight=$(mktemp)
snapshot_protected "$preflight"
cmp -s "$protected_before" "$preflight"
record protected-freeze "files=193 manifest=$(sha256sum "$protected_before" | awk '{print $1}')"

stage=smoke
: >"$smoke_log"
make stack-check | tee -a "$smoke_log"
make kernel-check JOBS=2 | tee -a "$smoke_log"
make image | tee -a "$smoke_log"
nm -u kernel.elf | tee "$artifact_dir/cl11fix16-smoke-nm-u.log" | tee -a "$smoke_log"
[[ ! -s $artifact_dir/cl11fix16-smoke-nm-u.log ]]
! grep -E 'cmd_taskman(test)?\.c:.*warning:' "$smoke_log"
record smoke 'stack=PASS j2=PASS image=PASS nm-undefined=0 warnings=0'

stage=probe
scripts/test-cl11.sh soak-probe
verify_probe
record probe 'sessions=5 target=10 full_frames>=50 fallback=0 scroll=0 heap=0'

stage=positive
scripts/test-cl11.sh positive
derive_reap_ref_baseline
for smp in 1 2 4 8; do
  accel=tcg
  [[ $smp == 2 && -w /dev/kvm ]] && accel=kvm
  verify_positive "$smp" "$accel"
done
record positive 'smp=1,2,4,8 auto-exit-canary-smp4+=PASS'

stage=negatives
scripts/test-cl11.sh negatives
verify_negatives
record negatives 'causal=4 rebuild-normal=PASS pending=0'

stage=soak
DURATION_MS=180000 scripts/test-cl11.sh soak
verify_soak
record soak "$(grep -F '[CL11][SOAK] PASS' "$artifact_dir/cl11-soak-smp8.log" | tail -1)"

stage=protected-after-tests
snapshot_protected "$protected_after_tests"
snapshot_authorized "$authorized_after_tests"
cmp -s "$protected_before" "$protected_after_tests"
record protected-after-tests "manifest=$(sha256sum "$protected_after_tests" | awk '{print $1}')"

stage=final-build
scripts/test-cl11.sh final-build
record final-build "$(tail -2 "$artifact_dir/cl11fix16-final-hashes.log" | tr '\n' ' ')"

stage=protected-after-build
snapshot_protected "$protected_after_build"
snapshot_authorized "$authorized_after_build"
cmp -s "$protected_before" "$protected_after_tests"
cmp -s "$protected_before" "$protected_after_build"
cmp -s "$authorized_after_tests" "$authorized_after_build"
record protected-after-build "manifest=$(sha256sum "$protected_after_build" | awk '{print $1}')"

stage=report-ready
require_marker "$artifact_dir/cl11fix15-hmp-transport-smp8.log" \
  '[HMP][TRANSPORT] PASS during_stress=25 after_stress=25 lost=0 duplicates=0'
assert_no_faults "$artifact_dir/cl11fix15-hmp-transport-smp8.log"
echo '[CL11][FIX16][REPORT_READY] PASS hmp-evidence=reused historical-regressions=not-rerun' | tee -a "$runner_log"
trap - EXIT
cleanup
