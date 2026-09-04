#!/usr/bin/env bash
set -Eeuo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

artifact_dir=artifacts/build
ledger=$artifact_dir/cl13-ledger.tsv
runtime_before=$artifact_dir/cl13fix12-runtime-before.sha256
runtime_after_focused=$artifact_dir/cl13fix12-runtime-after-focused.sha256
runtime_after_soak=$artifact_dir/cl13fix12-runtime-after-soak.sha256
runtime_after_build=$artifact_dir/cl13fix12-runtime-after-build.sha256
stage=evidence
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

ledger_record(){
  local scenario=$1 command=$2 artifact=$3 hash=-
  [[ ! -f $artifact ]] || hash=$(sha256sum "$artifact" | awk '{print $1}')
  printf 'fix12-checkpoint\t%s\t0\thost\treused\t%s\tPASS\t0\t%s\t%s\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t-\n' \
    "$scenario" "$command" "$artifact" "$hash" >>"$ledger"
}

failure(){
  local rc=$?
  case "$stage" in
    evidence) echo REJECTED_CL13_FIX12_EVIDENCE >&2;;
    host-unit) echo REJECTED_CL13_FIX12_STATUS_ORACLE >&2;;
    focused) echo REJECTED_CL13_FIX12_FOCUSED >&2;;
    soak8) echo REJECTED_CL13_SOAK >&2;;
    final-build) echo REJECTED_CL13_FINAL_BUILD >&2;;
    continuity) echo REJECTED_SOURCE_CONTINUITY >&2;;
    *) echo "REJECTED_CL13_FIX12 stage=$stage" >&2;;
  esac
  exit "$rc"
}
trap failure ERR

[[ $(sha256sum "$artifact_dir/cl13fix11-checkpoint.json" | awk '{print $1}') == \
  fa1bd204e63604d42f5e25983f7d2952c53ad734b07d268be69106263bd12cc4 ]]
[[ $(sha256sum "$artifact_dir/cl13-soak-smp4-tcg.log" | awk '{print $1}') == \
  fe5955f350b12d2b2bd0b1a6c649251a1db4691960a9f17079923ec5b073c0a8 ]]
[[ $(sha256sum "$runtime_before" | awk '{print $1}') == \
  d8dd183b4b2ef247bd766464e2e09dc88addbeb08c66b373dd6e9fb199b01d09 ]]
python3 - "$artifact_dir/cl13fix11-checkpoint.json" <<'PY'
import json
import pathlib
import sys

data = json.loads(pathlib.Path(sys.argv[1]).read_text())
pre = data.get("pre_hpet", {})
assert data.get("status") == "PASS"
assert pre.get("status") == "PASS"
assert len(pre.get("matrix", {})) == 4
assert len(pre.get("quantum", {})) == 2
assert len(pre.get("negatives", {})) == 6
assert len(pre.get("resets", {})) == 6
assert len(data.get("negatives", {})) == 16
assert data.get("hpet_offline_revalidation", {}).get("health") == "PASS"
assert pre.get("runtime", {}).get("sha256") == (
    "d8dd183b4b2ef247bd766464e2e09dc88addbeb08c66b373dd6e9fb199b01d09"
)
PY
grep -Fq '[CL13][SOAK] PASS smp=4 accel=tcg' \
  "$artifact_dir/cl13-soak-smp4-tcg.log"
grep -Fq 'switches=1000000 creates=10000 reaps=10000 kills=5000 taskman=101 ps=501 modal=500 input=100000 heap_drift=0 invariants=0 faults=0' \
  "$artifact_dir/cl13-soak-smp4-tcg.log"
scripts/cl13-soak-checkpoint.py verify-fix11-input-failure
ledger_record soak4-reused 'frozen hash and PASS summary' \
  "$artifact_dir/cl13-soak-smp4-tcg.log"
ledger_record negatives-reused 'FIX11 checkpoint 16/16' \
  "$artifact_dir/cl13fix11-checkpoint.json"
ledger_record soak8-head-revalidated 'two status-zero producer frames; diagnostic only' \
  "$artifact_dir/cl13fix12-soak8-head.json"

stage=host-unit
scripts/test-cl13-status-oracle-focused.sh host-unit
scripts/test-cl13-status-oracle-focused.sh static
bash -n scripts/test-taskman-v1-soak.sh
bash -n scripts/test-cl13-fix12-resume.sh
bash -n scripts/test-taskman-v1.sh

make stack-check
cp "$artifact_dir/kernel-check-j2.log" "$artifact_dir/cl13fix12-smoke-stack.log"
make kernel-check JOBS=2 SELFTEST=1 SELFTEST_AUTORUN=1
cp "$artifact_dir/kernel-check-j2.log" "$artifact_dir/cl13fix12-smoke-kernel.log"
nm -u kernel.elf >"$artifact_dir/cl13fix12-smoke-nm.log"
[[ ! -s $artifact_dir/cl13fix12-smoke-nm.log ]]

stage=focused
scripts/test-cl13-status-oracle-focused.sh smp8-kvm
stage=continuity
runtime_check "$runtime_after_focused"

stage=soak8
SMPSTRESS_PERIOD_MS=10000 scripts/test-taskman-v1.sh soak8
grep -Fq '[CL13][SOAK] PASS smp=8 accel=kvm' \
  "$artifact_dir/cl13-soak-smp8-kvm.log"
grep -Fq 'switches=2000000 creates=20000 reaps=20000 kills=10000' \
  "$artifact_dir/cl13-soak-smp8-kvm.log"
grep -Fq 'modal=1000 input=200000 heap_drift=0 invariants=0 faults=0' \
  "$artifact_dir/cl13-soak-smp8-kvm.log"
grep -Fq 'smpstress_period_ms=10000' "$artifact_dir/cl13-soak-smp8-kvm.log"
stage=continuity
runtime_check "$runtime_after_soak"

stage=final-build
scripts/test-taskman-v1.sh final-build
stage=continuity
runtime_check "$runtime_after_build"

stage=report
scripts/test-taskman-v1.sh report
trap - ERR
echo '[CL13][FIX12_RESUME] PASS evidence=1 host_unit=1 focused=1 soak8=1 final_build=1 report=1'
