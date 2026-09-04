#!/usr/bin/env bash
set -Eeuo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

artifact_dir=artifacts/build
base=4e700a9b38653fb57ecb0f107a18f87508db0736
runtime_before=$artifact_dir/cl13fix15-production-runtime-before.sha256
special_before=$artifact_dir/cl13fix15-special-before.sha256
runtime_after_focused=$artifact_dir/cl13fix15-production-runtime-after-focused.sha256
runtime_after_soak=$artifact_dir/cl13fix15-production-runtime-after-soak.sha256
runtime_after_build=$artifact_dir/cl13fix15-production-runtime-after-build.sha256
special_after_focused=$artifact_dir/cl13fix15-special-after-focused.sha256
special_after_soak=$artifact_dir/cl13fix15-special-after-soak.sha256
special_after_build=$artifact_dir/cl13fix15-special-after-build.sha256
ready=$artifact_dir/cl13fix15-resume-ready.log
ledger=$artifact_dir/cl13fix15-ledger.tsv
stage=evidence
mkdir -p "$artifact_dir"

usage(){
  echo "Usage: $0 run|after-static|finalize" >&2
  exit 2
}

runtime_defect_evidence(){
  local file
  for file in .qemu/qemu-serial.log \
    "$artifact_dir"/cl13fix15-taskman-smp*-kvm*.log \
    "$artifact_dir/cl13-soak-smp8-kvm.log"; do
    [[ -f $file ]] || continue
    grep -Eq '^\[TASKMANTEST\]\[AUTO_SESSION\] FAIL .*valid=0' "$file" \
      && return 0
  done
  return 1
}

ps_loop_evidence(){
  local file
  for file in .qemu/qemu-serial.log \
    "$artifact_dir"/cl13fix15-taskman-smp*-kvm*.log; do
    [[ -f $file ]] || continue
    grep -Eq '^\[HARNESS\]\[PS_LOOP\] FAIL ' "$file" && return 0
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
    evidence) echo REJECTED_CL13_FIX15_EVIDENCE >&2;;
    build) echo REJECTED_CL13_FIX15_BUILD >&2;;
    helper) echo REJECTED_CL13_FIX15_HELPER >&2;;
    focused)
      if ps_loop_evidence; then
        echo REJECTED_CL13_FIX15_PS_LOOP >&2
      elif runtime_defect_evidence; then
        echo REJECTED_RUNTIME_DEFECT_CONFIRMED >&2
      else
        echo REJECTED_CL13_FIX15_FOCUSED >&2
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
    *) echo "REJECTED_CL13_FIX15 stage=$stage" >&2;;
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
      rg -v '^kernel/src/(shell/commands/(cmd_taskmantest|cmd_tasktest)\.(c|h)|core/selftest\.c)$'
  )
  sha256sum "${files[@]}" >"$output"
}

special_snapshot(){
  local output=$1
  sha256sum \
    kernel/src/shell/commands/cmd_taskman.c \
    kernel/src/shell/commands/cmd_taskman.h \
    kernel/src/shell/commands/cmd_ps.c \
    kernel/src/shell/commands/cmd_ps.h \
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
  verify_hash "$artifact_dir/cl13fix14-soak8-kvm-failed.log" \
    84188d1ddf11a2ce9958922eef8adc6ff0f6c194900625ccb8d2404d5b347a9a
  verify_hash "$artifact_dir/cl13fix11-runtime-after-hpet.sha256" \
    d8dd183b4b2ef247bd766464e2e09dc88addbeb08c66b373dd6e9fb199b01d09
  verify_hash "$artifact_dir/cl13fix15-cmd-taskmantest-before.c" \
    4f83e637757d67b24f2693056776b5e20163e1f4a3fe620bb008d0809a20a68d
  grep -Fq 'GLOBAL_TIMER_NODE_NOISE_FALSE_REJECTION_CONFIRMED.' \
    "$artifact_dir/cl13fix14-modal-classification.log"
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
}

build_smoke(){
  : >"$artifact_dir/cl13fix15-build-smoke.log"
  make stack-check | tee -a "$artifact_dir/cl13fix15-build-smoke.log"
  make kernel-check JOBS=2 SELFTEST=1 SELFTEST_AUTORUN=1 \
    | tee -a "$artifact_dir/cl13fix15-build-smoke.log"
  nm -u kernel.elf >"$artifact_dir/cl13fix15-build-smoke-nm.log"
  [[ ! -s $artifact_dir/cl13fix15-build-smoke-nm.log ]]
  rg -q 'stack-check: PASS' "$artifact_dir/cl13fix15-build-smoke.log"
  rg -q '^violations=0$' artifacts/build/stack-usage-report.txt
  ! rg -n 'kernel/src/(core/selftest|shell/commands/(cmd_taskmantest|cmd_tasktest))\.c:.*warning:' \
    "$artifact_dir/cl13fix15-build-smoke.log"
}

ledger_soak(){
  local artifact=$1 hash
  hash=$(sha256sum "$artifact" | awk '{print $1}')
  [[ -f $ledger ]]
  printf 'soak8\tsmp8-kvm-final\t8\tkvm\tPASS\t5\t500\t4\t100\t33\t34\t33\t0\t0\t800\t9\t0\t%s\t%s\n' \
    "$artifact" "$hash" >>"$ledger"
}

final_build(){
  local jobs
  jobs=$(nproc)
  : >"$artifact_dir/cl13fix15-final-build.log"
  make clean | tee -a "$artifact_dir/cl13fix15-final-build.log"
  make stack-check | tee -a "$artifact_dir/cl13fix15-final-build.log"
  cp artifacts/build/stack-usage-report.txt \
    "$artifact_dir/cl13fix15-final-stack-usage.txt"
  make kernel-check JOBS=2 | tee -a "$artifact_dir/cl13fix15-final-build.log"
  cp kernel.elf /tmp/hobbyos-cl13fix15-j2.elf
  make kernel-check JOBS="$jobs" | tee -a "$artifact_dir/cl13fix15-final-build.log"
  cp kernel.elf /tmp/hobbyos-cl13fix15-jN.elf
  cmp -s /tmp/hobbyos-cl13fix15-j2.elf /tmp/hobbyos-cl13fix15-jN.elf
  sha256sum /tmp/hobbyos-cl13fix15-j2.elf \
    /tmp/hobbyos-cl13fix15-jN.elf \
    >"$artifact_dir/cl13fix15-final-release.sha256"
  make kernel-check JOBS=2 SELFTEST=1 SELFTEST_AUTORUN=1 \
    | tee -a "$artifact_dir/cl13fix15-final-build.log"
  cp kernel.elf /tmp/hobbyos-cl13fix15-selftest.elf
  make kernel-check JOBS=2 | tee -a "$artifact_dir/cl13fix15-final-build.log"
  make deps-check | tee -a "$artifact_dir/cl13fix15-final-build.log"
  make image | tee -a "$artifact_dir/cl13fix15-final-build.log"
  nm -u kernel.elf >"$artifact_dir/cl13fix15-final-nm.log"
  [[ ! -s $artifact_dir/cl13fix15-final-nm.log ]]
  ! strings kernel.elf | grep -Fq '[SELFTEST][AUTORUN] BEGIN'
  sha256sum kernel.elf hobbyos.img \
    >"$artifact_dir/cl13fix15-final-image.sha256"
  rg -q 'stack-check: PASS' "$artifact_dir/cl13fix15-final-build.log"
  rg -q '^violations=0$' "$artifact_dir/cl13fix15-final-stack-usage.txt"
  ! rg -n 'kernel/src/(core/selftest|shell/commands/(cmd_taskmantest|cmd_tasktest))\.c:.*warning:' \
    "$artifact_dir/cl13fix15-final-build.log"
}

run_resume(){
  local checkpoint=${1:-full}
  entry_gate
  evidence_gate

  if [[ $checkpoint == full ]]; then
    stage=helper
    scripts/cl13-taskman-soak-checkpoint.py verify-fix14-taskman-failure

    stage=build
    build_smoke

    stage=focused
    scripts/test-cl13-taskman-transaction-focused.sh host-unit
    scripts/test-cl13-taskman-transaction-focused.sh static
  fi

  stage=focused
  scripts/test-cl13-taskman-transaction-focused.sh smp4-kvm
  scripts/test-cl13-taskman-transaction-focused.sh smp8-kvm
  scripts/test-cl13-taskman-transaction-focused.sh classification

  stage=continuity
  continuity_check "$runtime_after_focused" "$special_after_focused"

  stage=soak8
  CL13_SOAK_CHECKPOINT_PREFIX=cl13fix15 \
    RUNTIME_MANIFEST_FILE="$runtime_before" \
    SMPSTRESS_PERIOD_MS=10000 \
    scripts/test-taskman-v1-soak.sh 8
  grep -Fq '[CL13][SOAK] PASS smp=8 accel=kvm' \
    "$artifact_dir/cl13-soak-smp8-kvm.log"
  grep -Fq 'switches=2000000 creates=20000 reaps=20000 kills=10000' \
    "$artifact_dir/cl13-soak-smp8-kvm.log"
  grep -Fq 'taskman=100 ps=500 modal=1000 input=200000' \
    "$artifact_dir/cl13-soak-smp8-kvm.log"
  grep -Fq 'taskman_warmup_sessions=1 ps_warmup_snapshots=1 refresh50=33 refresh1000=34 refresh2000=33' \
    "$artifact_dir/cl13-soak-smp8-kvm.log"
  grep -Fq 'ps_old_frames=500 ps_new_frames=5 taskman_old_frames=300 taskman_new_frames=4 frames_saved=791' \
    "$artifact_dir/cl13-soak-smp8-kvm.log"
  grep -Fq 'global_heap_drift=0 global_heap_pass=1' \
    "$artifact_dir/cl13-soak-smp8-kvm.log"
  grep -Fq 'modal_ownership=1 modal_runs_delta=1001 modal_workers_delta=1001 modal_normal_delta=1001 modal_cleanup_delta=1001 modal_signals_delta=1001 modal_duplicates_delta=0 modal_handles_gone=1000' \
    "$artifact_dir/cl13-soak-smp8-kvm.log"
  grep -Fq 'smpstress_period_ms=10000' \
    "$artifact_dir/cl13-soak-smp8-kvm.log"
  for checkpoint in ps-batch-{1..5} taskman-batch-{1..4}; do
    grep -Eq "^${checkpoint}[[:space:]]+PASS[[:space:]]" \
      "$artifact_dir/cl13fix15-soak8-stages.tsv"
  done
  ledger_soak "$artifact_dir/cl13-soak-smp8-kvm.log"

  stage=continuity
  continuity_check "$runtime_after_soak" "$special_after_soak"

  stage=final-build
  final_build

  stage=continuity
  continuity_check "$runtime_after_build" "$special_after_build"

  stage=report
  {
    echo '[CL13][FIX15_RESUME] READY_FOR_REPORT'
    grep -F '[CL13][SOAK] PASS smp=8 accel=kvm' \
      "$artifact_dir/cl13-soak-smp8-kvm.log" | tail -1
    sha256sum "$runtime_before" "$runtime_after_focused" \
      "$runtime_after_soak" "$runtime_after_build"
    sha256sum "$special_before" "$special_after_focused" \
      "$special_after_soak" "$special_after_build"
    cat "$artifact_dir/cl13fix15-final-image.sha256"
  } >"$ready"
  trap - ERR
  echo '[CL13][FIX15_RESUME] READY_FOR_REPORT'
}

finalize_resume(){
  stage=report
  entry_gate
  [[ -f $ready ]]
  grep -Fq '[CL13][FIX15_RESUME] READY_FOR_REPORT' "$ready"
  continuity_check "$runtime_after_build" "$special_after_build"
  grep -Fq 'AUTO_EXIT_ARM_DIAGNOSTIC_MARKER_FALSE_REJECTION_CONFIRMED.' \
    "$artifact_dir/cl13fix15-taskman-classification.log"
  grep -Fq '[CL13][SOAK] PASS smp=8 accel=kvm' \
    "$artifact_dir/cl13-soak-smp8-kvm.log"
  grep -Fq 'Status: APPROVED' \
    docs/test-reports/TMV1-CL-13-FIX15-taskman-transaction-batching.md
  grep -Fq 'TMV1-CL-13-FIX15: APPROVED' \
    docs/test-reports/TMV1-CL-13-taskman-v1-certification.md
  grep -Fq 'FIX15: APPROVED' \
    docs/test-reports/TMV1-CL-13-FIX14-modal-ownership-vs-global-heap.md
  git diff --check
  git status --short >"$artifact_dir/cl13fix15-pre-stage-status.log"
  git diff --stat >"$artifact_dir/cl13fix15-pre-stage-stat.log"
  git diff >"$artifact_dir/cl13fix15-pre-stage.diff"

  stage=commit
  git add AGENTS.md docs/development/build-and-qemu.md docs/test-reports \
    kernel makefile scripts/*.sh scripts/*.py
  ! git diff --cached --name-only | grep -E \
    '(^|/)(ROADMAP_TASKMAN_V1_CLOSURE_HARDENING\.md|artifacts|\.qemu|kernel\.elf|hobbyos\.img|.*\.(o|d|su|log|json|tsv|env|tar\.gz))$'
  [[ $(git rev-list --count "$base"..HEAD) == 0 ]]
  [[ -z $(git diff --name-only) ]]
  [[ $(git ls-files --others --exclude-standard) == \
    ROADMAP_TASKMAN_V1_CLOSURE_HARDENING.md ]]
  git commit -m \
    "test(taskman): add automated V1 SMP, lifecycle, input and UI validation"
}

case ${1:-run} in
  run) run_resume;;
  after-static) run_resume after-static;;
  finalize) finalize_resume;;
  *) usage;;
esac
