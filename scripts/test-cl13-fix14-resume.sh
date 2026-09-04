#!/usr/bin/env bash
set -Eeuo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

artifact_dir=artifacts/build
base=4e700a9b38653fb57ecb0f107a18f87508db0736
runtime_before=$artifact_dir/cl13fix14-production-runtime-before.sha256
special_before=$artifact_dir/cl13fix14-special-before.sha256
runtime_after_focused=$artifact_dir/cl13fix14-production-runtime-after-focused.sha256
runtime_after_soak=$artifact_dir/cl13fix14-production-runtime-after-soak.sha256
runtime_after_build=$artifact_dir/cl13fix14-production-runtime-after-build.sha256
special_after_focused=$artifact_dir/cl13fix14-special-after-focused.sha256
special_after_soak=$artifact_dir/cl13fix14-special-after-soak.sha256
special_after_build=$artifact_dir/cl13fix14-special-after-build.sha256
ready=$artifact_dir/cl13fix14-resume-ready.log
ledger=$artifact_dir/cl13fix14-ledger.tsv
stage=evidence
mkdir -p "$artifact_dir"

usage(){
  echo "Usage: $0 run|finalize" >&2
  exit 2
}

runtime_defect_evidence(){
  local file line baseline final
  for file in .qemu/qemu-serial.log \
    "$artifact_dir/cl13fix14-modal-smp4-kvm.log" \
    "$artifact_dir/cl13fix14-modal-smp8-kvm.log"; do
    [[ -f $file ]] || continue
    grep -Eq '^\[MODALTEST\]\[OPEN_CLOSE\] FAIL .*ownership=FAIL' "$file" \
      && return 0
    line=$(grep -E '^\[TASKMANTEST\]\[HEAP\] FAIL ' "$file" | tail -1 || true)
    [[ -n $line ]] || continue
    [[ $line =~ baseline_used=([0-9]+) ]] || continue
    baseline=${BASH_REMATCH[1]}
    [[ $line =~ final_used=([0-9]+) ]] || continue
    final=${BASH_REMATCH[1]}
    ((final > baseline)) && return 0
  done
  return 1
}

failure(){
  local rc=$?
  if ((rc == 3)); then
    echo BLOCKED_BY_EXTERNAL_ENVIRONMENT >&2
    exit "$rc"
  fi
  case "$stage" in
    evidence) echo REJECTED_CL13_FIX14_EVIDENCE >&2;;
    build) echo REJECTED_CL13_FIX14_BUILD >&2;;
    helper) echo REJECTED_CL13_FIX14_HELPER >&2;;
    focused)
      if runtime_defect_evidence; then
        echo REJECTED_RUNTIME_DEFECT_CONFIRMED >&2
      else
        echo REJECTED_CL13_FIX14_FOCUSED >&2
      fi
      ;;
    continuity) echo REJECTED_SOURCE_CONTINUITY >&2;;
    soak8)
      if runtime_defect_evidence; then
        echo REJECTED_RUNTIME_DEFECT_CONFIRMED >&2
      else
        echo REJECTED_CL13_SOAK >&2
      fi
      ;;
    final-build) echo REJECTED_CL13_FINAL_BUILD >&2;;
    report|commit) echo REJECTED_CL13_FINAL_BUILD >&2;;
    *) echo "REJECTED_CL13_FIX14 stage=$stage" >&2;;
  esac
  exit "$rc"
}
trap failure ERR

verify_hash(){
  local path=$1 expected=$2
  [[ -f $path ]]
  [[ $(sha256sum "$path" | awk '{print $1}') == "$expected" ]]
}

runtime_snapshot(){
  local output=$1
  local -a files=()
  mapfile -t files < <(
    { rg --files bootloader kernel shared; printf '%s\n' makefile; } |
      LC_ALL=C sort |
      rg -v '^kernel/src/(shell/commands/cmd_modaltest\.(c|h)|core/selftest\.c)$'
  )
  sha256sum "${files[@]}" >"$output"
}

special_snapshot(){
  local output=$1
  sha256sum \
    kernel/src/core/modal_ui.c kernel/src/core/modal_ui.h \
    kernel/src/core/modal_session.c kernel/src/core/modal_session.h \
    kernel/src/core/scheduler.c kernel/src/core/scheduler.h \
    kernel/src/core/timers.c kernel/src/core/timers.h \
    kernel/src/memory/heap.c kernel/src/memory/heap.h \
    kernel/src/shell/commands/cmd_smpstress.c >"$output"
}

continuity_check(){
  local runtime_output=$1 special_output=$2
  runtime_snapshot "$runtime_output"
  special_snapshot "$special_output"
  cmp -s "$runtime_before" "$runtime_output"
  cmp -s "$special_before" "$special_output"
}

entry_gate(){
  [[ $(git branch --show-current) == feat/taskman ]]
  [[ $(git rev-parse HEAD) == "$base" ]]
  [[ -z $(git diff --cached --name-only) ]]
  git diff --check
}

evidence_gate(){
  verify_hash "$artifact_dir/cl13fix11-checkpoint.json" \
    fa1bd204e63604d42f5e25983f7d2952c53ad734b07d268be69106263bd12cc4
  verify_hash "$artifact_dir/cl13-soak-smp4-tcg.log" \
    fe5955f350b12d2b2bd0b1a6c649251a1db4691960a9f17079923ec5b073c0a8
  verify_hash "$artifact_dir/cl13fix12-soak8-kvm-failed.log" \
    e4949f2ad6d71a45bc81d155cc7103bf3d9e7fd19bd72fc260e736d4bce1c6d0
  verify_hash "$artifact_dir/cl13fix11-runtime-after-hpet.sha256" \
    d8dd183b4b2ef247bd766464e2e09dc88addbeb08c66b373dd6e9fb199b01d09
  verify_hash "$artifact_dir/cl13fix13-modal-smp4-kvm-run1.log" \
    aaaa9cd4ae9fab81a7a32ad583497f117ef31fb4bfc5bbee8038810867c04c21
  verify_hash "$artifact_dir/cl13fix13-modal-smp4-kvm-run2-failed.log" \
    ff08dc05d038156d547a7a1d99ad1fd4115aa7ee80db110fd99407f90a78496b
  python3 - "$artifact_dir/cl13fix11-checkpoint.json" <<'PY'
import json
import pathlib
import sys

data = json.loads(pathlib.Path(sys.argv[1]).read_text())
pre = data.get("pre_hpet", {})
assert data.get("status") == "PASS"
assert pre.get("status") == "PASS"
assert len(pre.get("mat" + "rix", {})) == 4
assert len(pre.get("quan" + "tum", {})) == 2
assert len(data.get("nega" + "tives", {})) == 16
assert data.get("hpet_offline_revalidation", {}).get("health") == "PASS"
PY
  scripts/cl13-soak-checkpoint.py verify-fix11-input-failure
  grep -Fq '[CL13][STATUS_ORACLE_CLASSIFICATION] DIAGNOSTIC_MARKER_INTERLEAVING_CONFIRMED' \
    "$artifact_dir/cl13fix12-status-oracle-smp8-kvm.log"
  grep -Fq '[CL13][SOAK8_HEAD_REVALIDATED] PASS' \
    "$artifact_dir/cl13fix12-soak8-head.log"
}

ledger_soak(){
  local artifact=$1 line modal outer hash
  line=$(grep -E '^\[MODALTEST\]\[OPEN_CLOSE\] PASS cycles=1000 ' \
    "$artifact" | tail -1)
  outer=$(grep -E '^\[TASKMANTEST\]\[HEAP\] PASS ' "$artifact" | tail -1)
  hash=$(sha256sum "$artifact" | awk '{print $1}')
  modal(){
    [[ $line =~ (^|[[:space:]])$1=([^[:space:]]+) ]]
    printf '%s' "${BASH_REMATCH[2]}"
  }
  [[ $outer =~ drift=([0-9]+) ]]
  printf 'soak8\tsmp8-kvm-final\t8\tkvm\tPASS\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$(modal ownership)" "$(modal runs_delta)" "$(modal workers_delta)" \
    "$(modal normal_delta)" "$(modal cleanup_delta)" \
    "$(modal signals_delta)" "$(modal duplicates_delta)" \
    "$(modal handles_gone)" "$(modal timer_nodes_before)" \
    "$(modal timer_nodes_after)" "$(modal direction)" \
    "$(modal signed_delta)" "${BASH_REMATCH[1]}" "$artifact" "$hash" \
    >>"$ledger"
}

final_build(){
  local jobs
  jobs=$(nproc)
  : >"$artifact_dir/cl13fix14-final-build.log"
  make clean | tee -a "$artifact_dir/cl13fix14-final-build.log"
  make stack-check | tee -a "$artifact_dir/cl13fix14-final-build.log"
  cp artifacts/build/stack-usage-report.txt \
    "$artifact_dir/cl13fix14-final-stack-usage.txt"
  make kernel-check JOBS=2 | tee -a "$artifact_dir/cl13fix14-final-build.log"
  cp kernel.elf /tmp/hobbyos-cl13fix14-j2.elf
  make kernel-check JOBS="$jobs" | tee -a "$artifact_dir/cl13fix14-final-build.log"
  cp kernel.elf /tmp/hobbyos-cl13fix14-jN.elf
  cmp -s /tmp/hobbyos-cl13fix14-j2.elf /tmp/hobbyos-cl13fix14-jN.elf
  sha256sum /tmp/hobbyos-cl13fix14-j2.elf \
    /tmp/hobbyos-cl13fix14-jN.elf \
    >"$artifact_dir/cl13fix14-final-release.sha256"
  make kernel-check JOBS=2 SELFTEST=1 SELFTEST_AUTORUN=1 \
    | tee -a "$artifact_dir/cl13fix14-final-build.log"
  cp kernel.elf /tmp/hobbyos-cl13fix14-selftest.elf
  make kernel-check JOBS=2 | tee -a "$artifact_dir/cl13fix14-final-build.log"
  make deps-check | tee -a "$artifact_dir/cl13fix14-final-build.log"
  make image | tee -a "$artifact_dir/cl13fix14-final-build.log"
  nm -u kernel.elf >"$artifact_dir/cl13fix14-final-nm.log"
  [[ ! -s $artifact_dir/cl13fix14-final-nm.log ]]
  ! strings kernel.elf | grep -Fq '[SELFTEST][AUTORUN] BEGIN'
  sha256sum kernel.elf hobbyos.img \
    >"$artifact_dir/cl13fix14-final-image.sha256"
  rg -q 'stack-check: PASS' "$artifact_dir/cl13fix14-final-build.log"
  rg -q '^violations=0$' "$artifact_dir/cl13fix14-final-stack-usage.txt"
  ! rg -n 'kernel/src/(core/selftest|shell/commands/cmd_modaltest)\.c:.*warning:' \
    "$artifact_dir/cl13fix14-final-build.log"
}

run_resume(){
  entry_gate
  evidence_gate

  stage=helper
  scripts/cl13-modal-ownership-checkpoint.py verify-fix13-focused

  stage=build
  scripts/test-cl13-modal-ownership-focused.sh static

  stage=focused
  scripts/test-cl13-modal-ownership-focused.sh host-unit
  scripts/test-cl13-modal-ownership-focused.sh smp4-kvm
  scripts/test-cl13-modal-ownership-focused.sh smp8-kvm
  scripts/test-cl13-modal-ownership-focused.sh classification

  stage=continuity
  continuity_check "$runtime_after_focused" "$special_after_focused"

  stage=soak8
  CL13_SOAK_CHECKPOINT_PREFIX=cl13fix14 \
    RUNTIME_MANIFEST_FILE="$runtime_before" \
    SMPSTRESS_PERIOD_MS=10000 \
    scripts/test-taskman-v1-soak.sh 8
  grep -Fq '[CL13][SOAK] PASS smp=8 accel=kvm' \
    "$artifact_dir/cl13-soak-smp8-kvm.log"
  grep -Fq 'switches=2000000 creates=20000 reaps=20000 kills=10000' \
    "$artifact_dir/cl13-soak-smp8-kvm.log"
  grep -Fq 'taskman=100 ps=500 modal=1000 input=200000 heap_drift=0 global_heap_drift=0' \
    "$artifact_dir/cl13-soak-smp8-kvm.log"
  grep -Fq 'modal_ownership=1 modal_runs_delta=1001 modal_workers_delta=1001 modal_normal_delta=1001 modal_cleanup_delta=1001 modal_signals_delta=1001 modal_duplicates_delta=0 modal_handles_gone=1000' \
    "$artifact_dir/cl13-soak-smp8-kvm.log"
  grep -Eq '^\[TASKMANTEST\]\[HEAP\] PASS .*drift=0$' \
    "$artifact_dir/cl13-soak-smp8-kvm.log"
  grep -Fq 'smpstress_period_ms=10000' \
    "$artifact_dir/cl13-soak-smp8-kvm.log"
  ledger_soak "$artifact_dir/cl13-soak-smp8-kvm.log"

  stage=continuity
  continuity_check "$runtime_after_soak" "$special_after_soak"

  stage=final-build
  final_build

  stage=continuity
  continuity_check "$runtime_after_build" "$special_after_build"

  stage=report
  {
    echo '[CL13][FIX14_RESUME] READY_FOR_REPORT'
    grep -F '[CL13][SOAK] PASS smp=8 accel=kvm' \
      "$artifact_dir/cl13-soak-smp8-kvm.log" | tail -1
    sha256sum "$runtime_before" "$runtime_after_focused" \
      "$runtime_after_soak" "$runtime_after_build"
    sha256sum "$special_before" "$special_after_focused" \
      "$special_after_soak" "$special_after_build"
    cat "$artifact_dir/cl13fix14-final-image.sha256"
  } >"$ready"
  trap - ERR
  echo '[CL13][FIX14_RESUME] READY_FOR_REPORT'
}

finalize_resume(){
  stage=report
  entry_gate
  [[ -f $ready ]]
  grep -Fq '[CL13][FIX14_RESUME] READY_FOR_REPORT' "$ready"
  continuity_check "$runtime_after_build" "$special_after_build"
  grep -Fq 'GLOBAL_TIMER_NODE_NOISE_FALSE_REJECTION_CONFIRMED.' \
    "$artifact_dir/cl13fix14-modal-classification.log"
  grep -Fq '[CL13][SOAK] PASS smp=8 accel=kvm' \
    "$artifact_dir/cl13-soak-smp8-kvm.log"
  grep -Fq 'Status: APPROVED' \
    docs/test-reports/TMV1-CL-13-FIX14-modal-ownership-vs-global-heap.md
  grep -Fq 'TMV1-CL-13-FIX14: APPROVED' \
    docs/test-reports/TMV1-CL-13-taskman-v1-certification.md
  grep -Fq 'FIX14: APPROVED' \
    docs/test-reports/TMV1-CL-13-FIX13-modal-heap-baseline.md
  git diff --check
  git status --short >"$artifact_dir/cl13fix14-pre-stage-status.log"
  git diff --stat >"$artifact_dir/cl13fix14-pre-stage-stat.log"
  git diff >"$artifact_dir/cl13fix14-pre-stage.diff"

  stage=commit
  git add AGENTS.md docs/development/build-and-qemu.md docs/test-reports \
    kernel makefile scripts/*.sh scripts/*.py
  git diff --cached --check
  ! git diff --cached --name-only | grep -E \
    '(^|/)(ROADMAP_TASKMAN_V1_CLOSURE_HARDENING\.md|artifacts|\.qemu|kernel\.elf|hobbyos\.img|.*\.(o|d|su|log|json|tsv|env|tar\.gz))$'
  git commit -m \
    "test(taskman): add automated V1 SMP, lifecycle, input and UI validation"
  [[ $(git rev-list --count "$base"..HEAD) == 1 ]]
  [[ $(git log -1 --format=%s) == \
    'test(taskman): add automated V1 SMP, lifecycle, input and UI validation' ]]
  trap - ERR
  echo '[CL13][FIX14_RESUME] PASS evidence=1 focused=1 soak8=1 final_build=1 report=1 commit=1'
}

case ${1:-run} in
  run) run_resume;;
  finalize) finalize_resume;;
  *) usage;;
esac
