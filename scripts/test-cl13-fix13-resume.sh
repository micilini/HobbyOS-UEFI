#!/usr/bin/env bash
set -Eeuo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

artifact_dir=artifacts/build
base=4e700a9b38653fb57ecb0f107a18f87508db0736
runtime_before=$artifact_dir/cl13fix13-production-runtime-before.sha256
special_before=$artifact_dir/cl13fix13-modal-heap-special-before.sha256
runtime_after_focused=$artifact_dir/cl13fix13-production-runtime-after-focused.sha256
runtime_after_soak=$artifact_dir/cl13fix13-production-runtime-after-soak.sha256
runtime_after_build=$artifact_dir/cl13fix13-production-runtime-after-build.sha256
special_after_focused=$artifact_dir/cl13fix13-modal-heap-special-after-focused.sha256
special_after_soak=$artifact_dir/cl13fix13-modal-heap-special-after-soak.sha256
special_after_build=$artifact_dir/cl13fix13-modal-heap-special-after-build.sha256
ready=$artifact_dir/cl13fix13-resume-ready.log
ledger=$artifact_dir/cl13-ledger.tsv
stage=evidence
mkdir -p "$artifact_dir"

usage(){
  echo "Usage: $0 run|finalize" >&2
  exit 2
}

failure(){
  local rc=$?
  if ((rc == 3)); then
    echo BLOCKED_BY_EXTERNAL_ENVIRONMENT >&2
    exit "$rc"
  fi
  case "$stage" in
    evidence) echo REJECTED_CL13_FIX13_EVIDENCE >&2;;
    build) echo REJECTED_CL13_FIX13_BUILD >&2;;
    helper) echo REJECTED_CL13_FIX13_HELPER >&2;;
    focused) echo REJECTED_CL13_FIX13_FOCUSED >&2;;
    continuity) echo REJECTED_SOURCE_CONTINUITY >&2;;
    soak8) echo REJECTED_CL13_SOAK >&2;;
    final-build) echo REJECTED_CL13_FINAL_BUILD >&2;;
    report|commit) echo REJECTED_CL13_FINAL_BUILD >&2;;
    *) echo "REJECTED_CL13_FIX13 stage=$stage" >&2;;
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
    kernel/src/memory/heap.c kernel/src/memory/heap.h >"$output"
}

continuity_check(){
  local runtime_output=$1 special_output=$2
  runtime_snapshot "$runtime_output"
  special_snapshot "$special_output"
  cmp -s "$runtime_before" "$runtime_output"
  cmp -s "$special_before" "$special_output"
}

ledger_record(){
  local ledger_stage=$1 scenario=$2 command=$3 artifact=$4 hash=-
  [[ -f $artifact ]] && hash=$(sha256sum "$artifact" | awk '{print $1}')
  printf '%s\t%s\t0\thost\tfix13\t%s\tPASS\t0\t%s\t%s\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t-\n' \
    "$ledger_stage" "$scenario" "$command" "$artifact" "$hash" \
    >>"$ledger"
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
assert len(pre.get("nega" + "tives", {})) == 6
assert len(pre.get("resets", {})) == 6
assert len(data.get("nega" + "tives", {})) == 16
assert data.get("hpet_offline_revalidation", {}).get("health") == "PASS"
PY
  scripts/cl13-soak-checkpoint.py verify-fix11-input-failure
  grep -Fq '[CL13][STATUS_ORACLE_CLASSIFICATION] DIAGNOSTIC_MARKER_INTERLEAVING_CONFIRMED' \
    "$artifact_dir/cl13fix12-status-oracle-smp8-kvm.log"
  grep -Fq '[CL13][SOAK8_HEAD_REVALIDATED] PASS' \
    "$artifact_dir/cl13fix12-soak8-head.log"
  ledger_record fix13-checkpoint reused-evidence \
    'frozen FIX11/FIX12 hashes and markers' \
    "$artifact_dir/cl13fix11-checkpoint.json"
}

final_build(){
  local jobs
  jobs=$(nproc)
  : >"$artifact_dir/cl13fix13-final-build.log"
  make clean | tee -a "$artifact_dir/cl13fix13-final-build.log"
  make stack-check | tee -a "$artifact_dir/cl13fix13-final-build.log"
  cp artifacts/build/stack-usage-report.txt \
    "$artifact_dir/cl13fix13-final-stack-usage.txt"
  make kernel-check JOBS=2 | tee -a "$artifact_dir/cl13fix13-final-build.log"
  cp kernel.elf /tmp/hobbyos-cl13fix13-j2.elf
  cp artifacts/build/kernel-check-j2.log \
    "$artifact_dir/cl13fix13-final-release-j2.log"
  make kernel-check JOBS="$jobs" | tee -a "$artifact_dir/cl13fix13-final-build.log"
  cp kernel.elf /tmp/hobbyos-cl13fix13-jN.elf
  cp "artifacts/build/kernel-check-j${jobs}.log" \
    "$artifact_dir/cl13fix13-final-release-jN.log"
  cmp -s /tmp/hobbyos-cl13fix13-j2.elf /tmp/hobbyos-cl13fix13-jN.elf
  sha256sum /tmp/hobbyos-cl13fix13-j2.elf \
    /tmp/hobbyos-cl13fix13-jN.elf \
    >"$artifact_dir/cl13fix13-final-release.sha256"
  make kernel-check JOBS=2 SELFTEST=1 SELFTEST_AUTORUN=1 \
    | tee -a "$artifact_dir/cl13fix13-final-build.log"
  cp kernel.elf /tmp/hobbyos-cl13fix13-selftest.elf
  make kernel-check JOBS=2 | tee -a "$artifact_dir/cl13fix13-final-build.log"
  make deps-check | tee -a "$artifact_dir/cl13fix13-final-build.log"
  make image | tee -a "$artifact_dir/cl13fix13-final-build.log"
  nm -u kernel.elf >"$artifact_dir/cl13fix13-final-nm.log"
  [[ ! -s $artifact_dir/cl13fix13-final-nm.log ]]
  ! strings kernel.elf | grep -Fq '[SELFTEST][AUTORUN] BEGIN'
  sha256sum kernel.elf hobbyos.img \
    >"$artifact_dir/cl13fix13-final-image.sha256"
  rg -q 'stack-check: PASS' "$artifact_dir/cl13fix13-final-build.log"
  rg -q '^violations=0$' "$artifact_dir/cl13fix13-final-stack-usage.txt"
  ! rg -n 'kernel/src/(core/selftest|shell/commands/cmd_modaltest)\.c:.*warning:' \
    "$artifact_dir/cl13fix13-final-build.log"
}

run_resume(){
  entry_gate
  evidence_gate

  stage=helper
  scripts/cl13-modal-checkpoint.py verify-fix12-modal-failure

  stage=build
  scripts/test-cl13-modal-heap-focused.sh static

  stage=focused
  scripts/test-cl13-modal-heap-focused.sh host-unit
  scripts/test-cl13-modal-heap-focused.sh smp4-kvm
  scripts/test-cl13-modal-heap-focused.sh smp8-kvm
  scripts/test-cl13-modal-heap-focused.sh classification

  stage=continuity
  continuity_check "$runtime_after_focused" "$special_after_focused"

  stage=soak8
  CL13_SOAK_CHECKPOINT_PREFIX=cl13fix13 \
    RUNTIME_MANIFEST_FILE="$runtime_before" \
    SMPSTRESS_PERIOD_MS=10000 \
    scripts/test-taskman-v1-soak.sh 8
  grep -Fq '[CL13][SOAK] PASS smp=8 accel=kvm' \
    "$artifact_dir/cl13-soak-smp8-kvm.log"
  grep -Fq 'switches=2000000 creates=20000 reaps=20000 kills=10000' \
    "$artifact_dir/cl13-soak-smp8-kvm.log"
  grep -Fq 'taskman=100 ps=500 modal=1000 input=200000 heap_drift=0' \
    "$artifact_dir/cl13-soak-smp8-kvm.log"
  grep -Fq 'modal_warmup_gone=1 modal_handles_gone=1000 modal_direction=ZERO modal_signed_delta=0' \
    "$artifact_dir/cl13-soak-smp8-kvm.log"
  grep -Eq '^\[TASKMANTEST\]\[HEAP\] PASS .*drift=0$' \
    "$artifact_dir/cl13-soak-smp8-kvm.log"
  grep -Fq 'smpstress_period_ms=10000' \
    "$artifact_dir/cl13-soak-smp8-kvm.log"
  ledger_record soak8 smp8-kvm-final \
    'FIX13 full KVM run with exact modal heap contract' \
    "$artifact_dir/cl13-soak-smp8-kvm.log"

  stage=continuity
  continuity_check "$runtime_after_soak" "$special_after_soak"

  stage=final-build
  final_build

  stage=continuity
  continuity_check "$runtime_after_build" "$special_after_build"

  stage=report
  {
    echo '[CL13][FIX13_RESUME] READY_FOR_REPORT'
    grep -F '[CL13][SOAK] PASS smp=8 accel=kvm' \
      "$artifact_dir/cl13-soak-smp8-kvm.log" | tail -1
    sha256sum "$runtime_before" "$runtime_after_focused" \
      "$runtime_after_soak" "$runtime_after_build"
    sha256sum "$special_before" "$special_after_focused" \
      "$special_after_soak" "$special_after_build"
    cat "$artifact_dir/cl13fix13-final-image.sha256"
  } >"$ready"
  ledger_record fix13-report ready-for-report \
    'focused, full SMP8 and final build complete' "$ready"
  trap - ERR
  echo '[CL13][FIX13_RESUME] READY_FOR_REPORT'
}

finalize_resume(){
  stage=report
  entry_gate
  [[ -f $ready ]]
  grep -Fq '[CL13][FIX13_RESUME] READY_FOR_REPORT' "$ready"
  continuity_check "$runtime_after_build" "$special_after_build"
  grep -Fq 'WARMUP_BASELINE_CONTAMINATION_CONFIRMED.' \
    "$artifact_dir/cl13fix13-modal-classification.log"
  grep -Fq '[CL13][SOAK] PASS smp=8 accel=kvm' \
    "$artifact_dir/cl13-soak-smp8-kvm.log"
  grep -Fq 'Status: APPROVED' \
    docs/test-reports/TMV1-CL-13-FIX13-modal-heap-baseline.md
  grep -Fq 'TMV1-CL-13-FIX13: APPROVED' \
    docs/test-reports/TMV1-CL-13-taskman-v1-certification.md
  grep -Fq 'FIX13: APPROVED' \
    docs/test-reports/TMV1-CL-13-FIX12-soak-status-oracle.md
  git diff --check
  git status --short >"$artifact_dir/cl13fix13-pre-stage-status.log"
  git diff --stat >"$artifact_dir/cl13fix13-pre-stage-stat.log"
  git diff >"$artifact_dir/cl13fix13-pre-stage.diff"

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
  echo '[CL13][FIX13_RESUME] PASS evidence=1 focused=1 soak8=1 final_build=1 report=1 commit=1'
}

case ${1:-run} in
  run) run_resume;;
  finalize) finalize_resume;;
  *) usage;;
esac
