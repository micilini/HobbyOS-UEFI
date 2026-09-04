#!/usr/bin/env bash
set -Eeuo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

artifact_dir=artifacts/build
runtime_before=$artifact_dir/cl13fix11-runtime-before.sha256
runtime_after_hpet=$artifact_dir/cl13fix11-runtime-after-hpet.sha256
runtime_after_negatives=$artifact_dir/cl13fix11-runtime-after-negatives.sha256
runtime_after_build=$artifact_dir/cl13fix11-runtime-after-build.sha256
stage=entry
mkdir -p "$artifact_dir"

runtime_snapshot(){
  local output=$1
  { { rg --files kernel bootloader shared; printf '%s\n' makefile; } |
      LC_ALL=C sort | xargs sha256sum; } >"$output"
}

runtime_check(){
  local output=$1
  runtime_snapshot "$output"
  cmp -s "$runtime_before" "$output" || {
    echo REJECTED_SOURCE_CONTINUITY >&2
    return 1
  }
}

failure(){
  local rc=$?
  case "$stage" in
    checkpoint) echo REJECTED_CL13_FIX11_EVIDENCE >&2;;
    hpet) echo REJECTED_CL13_FIX11_HPET_RESET >&2;;
    negatives) echo REJECTED_CL13_NEGATIVE >&2;;
    soak4|soak8) echo REJECTED_CL13_SOAK >&2;;
    continuity) echo REJECTED_SOURCE_CONTINUITY >&2;;
    *) echo "REJECTED_CL13_FIX11 stage=$stage" >&2;;
  esac
  exit "$rc"
}
trap failure ERR

stage=checkpoint
scripts/cl13-evidence-checkpoint.py verify-pre-hpet
grep -Fq '[CL13][HPET_BOOT_NEGATIVE_REVALIDATED] PASS' \
  "$artifact_dir/cl13fix11-hpet-boot-negative-revalidated.log"

bash -n scripts/test-taskman-v1-negatives.sh
bash -n scripts/test-taskman-v1.sh
bash -n scripts/test-cl13-fix11-resume.sh
python3 -m py_compile scripts/cl13-evidence-checkpoint.py
rg -n 'negative_boot_one|BOOT_NEGATIVE' scripts/test-taskman-v1-negatives.sh \
  >"$artifact_dir/cl13fix11-static.log"
rg -n 'hpet-torn-read' scripts/test-taskman-v1-negatives.sh \
  >>"$artifact_dir/cl13fix11-static.log"
rg -n 'resume-after-hpet-negative' scripts/test-taskman-v1.sh \
  >>"$artifact_dir/cl13fix11-static.log"

make stack-check
cp artifacts/build/kernel-check-j2.log "$artifact_dir/cl13fix11-smoke-stack.log"
make kernel-check JOBS=2 SELFTEST=1 SELFTEST_AUTORUN=1
cp artifacts/build/kernel-check-j2.log "$artifact_dir/cl13fix11-smoke-kernel.log"
nm -u kernel.elf >"$artifact_dir/cl13fix11-smoke-nm.log"
[[ ! -s $artifact_dir/cl13fix11-smoke-nm.log ]]

stage=hpet
scripts/test-taskman-v1-negatives.sh hpet
stage=continuity
runtime_check "$runtime_after_hpet"

stage=negatives
scripts/test-taskman-v1-negatives.sh from kill-ignore-killable
stage=continuity
runtime_check "$runtime_after_negatives"
scripts/cl13-evidence-checkpoint.py summary

checkpoint_hash=$(sha256sum "$artifact_dir/cl13fix11-checkpoint.json" | awk '{print $1}')
printf 'negatives\tfix11-resume\t0\ttcg\tisolated-builds\tBOOT/COMMAND/ASYNC selective resume\tPASS\t0\t%s\t%s\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t-\n' \
  "$artifact_dir/cl13fix11-checkpoint.json" "$checkpoint_hash" \
  >>"$artifact_dir/cl13-ledger.tsv"

stage=soak4
scripts/test-taskman-v1.sh soak4
stage=soak8
scripts/test-taskman-v1.sh soak8

stage=final-build
scripts/test-taskman-v1.sh final-build
stage=continuity
runtime_check "$runtime_after_build"

stage=report
scripts/test-taskman-v1.sh report
trap - ERR
echo '[CL13][FIX11_RESUME] PASS checkpoint=1 hpet=1 negatives_remaining=15 soak4=1 soak8=1 final_build=1 report=1'
