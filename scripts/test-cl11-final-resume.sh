#!/usr/bin/env bash
set -Eeuo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

artifact_dir=artifacts/build
ledger_tsv=$artifact_dir/cl11fix15-evidence-ledger.tsv
ledger_log=$artifact_dir/cl11fix15-evidence-ledger.log
runtime_before=$artifact_dir/cl11fix15-runtime-before.sha256
runtime_after_tests=$artifact_dir/cl11fix15-runtime-after-tests.sha256
runtime_after_build=$artifact_dir/cl11fix15-runtime-after-build.sha256
expected_manifest=3738360707274d437350f3374bfd37256533615fc335893f9f6c601d92dc4c66
expected_runtime_files=197
expected_kernel=c95a69a8a29f7a92b131d7de90a6fa93e78081c792aac459b7aff4ef798a7a09
expected_branch=feat/taskman
expected_base=88645901d9393f4796af84cecd9e13d3d6671c70
stage=evidence
evidence_result_printed=0

mkdir -p "$artifact_dir"
printf 'gate\tstatus\tdetail\n' >"$ledger_tsv"
: >"$ledger_log"

record(){
  local gate=$1 status=$2 detail=$3
  printf '%s\t%s\t%s\n' "$gate" "$status" "$detail" >>"$ledger_tsv"
  printf '[CL11][FIX15][%s] %s %s\n' "$gate" "$status" "$detail" | tee -a "$ledger_log"
}

cleanup(){
  if [[ -e .qemu/qemu.pid || -S .qemu/hmp.sock ]]; then
    scripts/qemu-agent.sh stop >/dev/null 2>&1 || true
  fi
}

on_exit(){
  local rc=$1
  cleanup
  if ((rc != 0)); then
    if [[ $stage == evidence ]]; then
      if ((evidence_result_printed == 0)); then
        echo '[CL11][EVIDENCE_CONTINUITY] FAIL' | tee -a "$ledger_log"
      fi
    elif [[ $stage == hmp || $stage == positive || $stage == negatives || $stage == soak ]]; then
      printf '[CL11][FINAL_RESUME] FAIL status=REJECTED_REMAINING_CL11_GATE stage=%s\n' "$stage" | tee -a "$ledger_log"
    else
      printf '[CL11][FINAL_RESUME] FAIL status=REJECTED_BUILD_CONTINUITY stage=%s\n' "$stage" | tee -a "$ledger_log"
    fi
  fi
}
trap 'on_exit $?' EXIT

fail_evidence(){
  record "$1" FAIL "$2"
  evidence_result_printed=1
  echo '[CL11][EVIDENCE_CONTINUITY] FAIL' | tee -a "$ledger_log"
  exit 1
}

require_file(){
  [[ -f $1 ]] || fail_evidence "$2" "missing=$1"
}

require_marker(){
  local file=$1 marker=$2
  grep -Fq -- "$marker" "$file"
}

assert_no_faults(){
  ! grep -Eq 'PANIC|#PF|#GP|FATAL' "$1"
}

runtime_file_list(){
  {
    find kernel bootloader shared -type f \
      \( -name '*.c' -o -name '*.h' -o -name '*.S' -o -name '*.s' \
         -o -name '*.ld' -o -name '*.inc' -o -name '*.asm' \) -print0
    printf 'makefile\0'
  } | LC_ALL=C sort -z
}

snapshot_runtime(){
  local output=$1
  local -a files=()
  mapfile -d '' -t files < <(runtime_file_list)
  ((${#files[@]} == expected_runtime_files)) || return 1
  sha256sum "${files[@]}" >"$output"
}

manifest_hash(){
  sha256sum "$1" | awk '{print $1}'
}

value_from_line(){
  local line=$1 key=$2 regex
  regex="(^|[[:space:]])${key}=([0-9]+)($|[[:space:]])"
  [[ $line =~ $regex ]] || return 1
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

verify_positive_artifact(){
  local smp=$1 accel=$2 file=$artifact_dir/cl11-matrix-smp${smp}-${accel}.log marker
  [[ -f $file ]]
  for marker in \
    '[TASKMANTEST][CHECK] PASS' \
    '[MODALTEST][CHECK] PASS' \
    '[INPUTTEST][CHECK] PASS' \
    '[SCHED][CHECK] PASS' \
    '[SYNC][CHECK] PASS' \
    '[ACCOUNT][CHECK] PASS' \
    '[KILLTEST][CHECK] PASS' \
    '[REAPTEST][CHECK] PASS'; do
    require_marker "$file" "$marker"
  done
  parse_reap_check "$file"
  ((REAP_REFS==REAP_REF_BASELINE))
  assert_no_faults "$file"
  if ((smp >= 4)); then
    for marker in \
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
}

verify_soak(){
  local file=$artifact_dir/cl11-soak-smp8.log line value marker target
  [[ -f $file ]]
  line=$(grep -F '[CL11][SOAK] PASS' "$file" | tail -1)
  [[ -n $line ]]
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
    '[TASKMANTEST][CHECK] PASS' \
    '[MODALTEST][CHECK] PASS state=INACTIVE contexts=0 quarantine=0 duplicates=0 violations=0' \
    '[INPUTTEST][CHECK] PASS' \
    '[SYNC][CHECK] PASS' \
    '[ACCOUNT][CHECK] PASS' \
    '[KILLTEST][CHECK] PASS' \
    '[REAPTEST][CHECK] PASS'; do
    require_marker "$file" "$marker"
  done
  assert_no_faults "$file"
  value=$(value_from_line "$line" duration_ms); ((value >= 180000))
  value=$(value_from_line "$line" sessions); ((value >= 100))
  target=$(value_from_line "$line" full_frame_target); ((target >= 1500))
  value=$(value_from_line "$line" full_frames); ((value >= target))
  value=$(value_from_line "$line" fallback_frames); ((value == 0))
  value=$(value_from_line "$line" auto_exit_sessions); ((value >= 100))
  value=$(value_from_line "$line" auto_exit_shortfalls); ((value == 0))
  value=$(value_from_line "$line" max_frame_gap_ms); ((value <= 5000))
  value=$(value_from_line "$line" modal_scroll_delta); ((value == 0))
  value=$(value_from_line "$line" clock_smp_batches); ((value >= 3))
  for marker in navigation:10000 churn:10000 zombies:100 kill_render:100 layouts:100 \
    snapshot_boundary_verified:20 timer_ref_cycles:100 preblock_sem:100 \
    preblock_timer:100 preblock_input:100 modal_blocked:100 modal_prewait:100 \
    modal_completion_ready:100; do
    value=$(value_from_line "$line" "${marker%%:*}")
    ((value >= ${marker##*:}))
  done
  grep -Eq '\[TASKMANTEST\]\[CHECK\] PASS.*last_session_scroll_delta=0.*last_session_clipped_writes=0.*clipped_total=0.*stale_cells=0.*builder_truncations=0.*controls_default=1' "$file"
  grep -Eq '\[INPUTTEST\]\[CHECK\] PASS.*phantom=0.*route_mismatch=0.*violations=0' "$file"
  grep -Eq '\[KILLTEST\]\[CHECK\] PASS.*pending=0.*duplicate=0.*contexts=0.*quarantine=0.*holds=0' "$file"
  parse_reap_check "$file"
  ((REAP_REFS==REAP_REF_BASELINE))
}

[[ $# == 0 ]] || { echo "usage: $0" >&2; exit 2; }
[[ $(git branch --show-current) == "$expected_branch" ]] || fail_evidence git "branch=$(git branch --show-current)"
[[ $(git rev-parse HEAD) == "$expected_base" ]] || fail_evidence git "head=$(git rev-parse HEAD)"
git diff --cached --quiet || fail_evidence git staged-changes-present
[[ ! -e .qemu/qemu.pid && ! -S .qemu/hmp.sock ]] || fail_evidence qemu residual-control-files
if ps -eo args= | awk '$1 ~ /(^|\/)qemu-system-x86_64$/ { found=1 } END { exit !found }'; then
  fail_evidence qemu active-process
fi
record git PASS "branch=$expected_branch base=$expected_base staged=0 qemu=0"

snapshot_runtime "$runtime_before" || fail_evidence runtime "file-count-not-$expected_runtime_files"
runtime_count=$(wc -l <"$runtime_before")
runtime_hash=$(manifest_hash "$runtime_before")
[[ $runtime_count == "$expected_runtime_files" ]] || fail_evidence runtime "files=$runtime_count"
[[ $runtime_hash == "$expected_manifest" ]] || fail_evidence runtime "manifest=$runtime_hash"
record runtime PASS "manifest=$runtime_hash files=$runtime_count"

require_file kernel.elf kernel-initial
initial_kernel=$(sha256sum kernel.elf | awk '{print $1}')
[[ $initial_kernel == "$expected_kernel" ]] || fail_evidence kernel-initial "sha256=$initial_kernel"
record kernel-initial PASS "sha256=$initial_kernel"

fix14_result=$artifact_dir/cl11fix14-autoclean-result.txt
require_file "$fix14_result" fix14
require_marker "$fix14_result" READY_FOR_FINAL_CERTIFICATION || fail_evidence fix14 result-not-ready
for run in 1 2 3 4 5; do
  log=$artifact_dir/cl11fix13-clean-kvm-run${run}.log
  require_file "$log" fix14-run${run}
  for marker in '[ACCOUNT][LAPIC_CONFIG] PASS' '[ACCOUNT][LAPIC_LIVENESS] PASS' \
    '[ACCOUNT][LAPIC_RATE] PASS' '[ACCOUNT][CHECK] PASS'; do
    require_marker "$log" "$marker" || fail_evidence fix14-run${run} "missing=$marker"
  done
  assert_no_faults "$log" || fail_evidence fix14-run${run} guest-fault
done
record fix14 PASS 'result=READY_FOR_FINAL_CERTIFICATION boots=5 gates=5/5 faults=0'

fix11_report=docs/taskman-v1/reports/TMV1-CL-11-FIX11-final-certification.md
fix11_heavy=$artifact_dir/cl11fix11-heavy-runs-smp8.log
require_file "$fix11_report" fix11-report
require_file "$fix11_heavy" fix11-heavy
require_marker "$fix11_heavy" '[CL10][FIX11_EXACT_CERT] PASS runs=5' || fail_evidence fix11-heavy sentinel-missing
for marker in 'CL-07 integral alcançada' 'CL-08 integral alcançada' \
  'CL-09 integral alcançada' 'CL-10 integral independente PASS'; do
  require_marker "$fix11_report" "$marker" || fail_evidence fix11-report "missing=$marker"
done
record fix11 PASS 'heavy=5 CL07=PASS CL08=PASS CL09=PASS CL10-independent=PASS'

evidence_result_printed=1
echo '[CL11][EVIDENCE_CONTINUITY] PASS' | tee -a "$ledger_log"

stage=hmp
scripts/test-hmp-transport.sh
hmp_source=$artifact_dir/cl11fix3-hmp-transport-smp8.log
require_marker "$hmp_source" '[HMP][TRANSPORT] PASS during_stress=25 after_stress=25 lost=0 duplicates=0'
assert_no_faults "$hmp_source"
cp "$hmp_source" "$artifact_dir/cl11fix15-hmp-transport-smp8.log"
record hmp PASS 'during_stress=25 after_stress=25 lost=0 duplicates=0'

stage=positive
scripts/test-cl11.sh positive
derive_reap_ref_baseline
for smp in 1 2 4 8; do
  accel=tcg
  [[ $smp == 2 && -w /dev/kvm ]] && accel=kvm
  verify_positive_artifact "$smp" "$accel"
done
record positive PASS 'smp=1,2,4,8 taskman=PASS subsystem-checks=PASS'

stage=negatives
scripts/test-cl11.sh negatives
for spec in \
  'cl11-negative-render-all.log|[TASKMANTEST][NEGATIVE] PANEL_OVERFLOW_DETECTED' \
  'cl11-negative-any-key.log|[TASKMANTEST][NEGATIVE] NON_ESC_EXIT_DETECTED' \
  'cl11-negative-fixed128.log|[TASKMANTEST][NEGATIVE] HIDDEN_TRUNCATION_DETECTED' \
  'cl11-negative-page-clamp.log|[TASKMANTEST][NEGATIVE] PAGE_CLAMP_MISSING_DETECTED'; do
  file=$artifact_dir/${spec%%|*}
  marker=${spec#*|}
  require_marker "$file" "$marker"
  assert_no_faults "$file"
done
require_marker "$artifact_dir/cl11-negative-reset.log" '[TASKMANTEST][CHECK] PASS'
assert_no_faults "$artifact_dir/cl11-negative-reset.log"
record negatives PASS 'causal=4 normal-rebuild=PASS taskmantest-check=PASS'

stage=soak
DURATION_MS=180000 scripts/test-cl11.sh soak
verify_soak
record soak PASS "$(grep -F '[CL11][SOAK] PASS' "$artifact_dir/cl11-soak-smp8.log" | tail -1)"

stage=runtime-after-tests
snapshot_runtime "$runtime_after_tests"
cmp -s "$runtime_before" "$runtime_after_tests"
[[ $(manifest_hash "$runtime_after_tests") == "$expected_manifest" ]]
record runtime-after-tests PASS "manifest=$expected_manifest files=$expected_runtime_files"

stage=final-build
scripts/test-cl11.sh final-build

stage=runtime-after-build
snapshot_runtime "$runtime_after_build"
cmp -s "$runtime_before" "$runtime_after_tests"
cmp -s "$runtime_before" "$runtime_after_build"
[[ $(manifest_hash "$runtime_after_build") == "$expected_manifest" ]]
record runtime-after-build PASS "manifest=$expected_manifest files=$expected_runtime_files"

stage=kernel-final
final_kernel=$(sha256sum kernel.elf | awk '{print $1}')
[[ $final_kernel == "$expected_kernel" ]]
cmp -s /tmp/hobbyos-cl11fix15-j2.elf /tmp/hobbyos-cl11fix15-jN.elf
require_marker "$artifact_dir/cl11fix15-reproducible-hashes.log" "$expected_kernel  /tmp/hobbyos-cl11fix15-j2.elf"
require_marker "$artifact_dir/cl11fix15-reproducible-hashes.log" "$expected_kernel  /tmp/hobbyos-cl11fix15-jN.elf"
require_marker "$artifact_dir/cl11fix15-final-hashes.log" "$expected_kernel  kernel.elf"
[[ -z $(nm -u kernel.elf) ]]
record kernel-final PASS "sha256=$final_kernel reproducible=1 nm-undefined=0"

echo '[CL11][FINAL_RESUME] PASS' | tee -a "$ledger_log"
trap - EXIT
