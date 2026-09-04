#!/usr/bin/env bash
set -Eeuo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

artifact_dir=artifacts/build
ledger=$artifact_dir/cl13-ledger.tsv
serial=.qemu/qemu-serial.log
hmp=(python3 scripts/qemu_hmp.py --socket .qemu/hmp.sock)
harness_start_line=0
harness_command=cl13-boundary-focused
current_scenario=boundary-focused
current_log=
mkdir -p "$artifact_dir"
source scripts/harness-common.sh
source scripts/harness-runtime-ready.sh
source scripts/harness-framed.sh
source scripts/harness-selftest.sh

usage(){
  echo "Usage: $0 static|smp1-tcg|smp2-tcg|smp2-kvm|smp4-kvm|noise|negative|classify|all" >&2
  exit 2
}

ensure_ledger(){
  if [[ ! -f $ledger ]]; then
    printf 'stage\tscenario\tsmp\taccel\tbuild_variant\tcommand\tstatus\tduration_ms\tartifact\thash\n' >"$ledger"
  fi
}

record(){
  local scenario=$1 smp=$2 accel=$3 command=$4 status=$5 duration=$6 artifact=${7:-}
  local hash=-
  [[ -n $artifact && -f $artifact ]] && hash=$(sha256sum "$artifact" | awk '{print $1}')
  [[ -n $artifact ]] || artifact=-
  printf 'boundary-focused\t%s\t%s\t%s\tfix1\t%s\t%s\t%s\t%s\t%s\n' \
    "$scenario" "$smp" "$accel" "$command" "$status" "$duration" \
    "$artifact" "$hash" >>"$ledger"
}

cleanup(){ stop_qemu; }
failure(){
  local rc=$?
  set +e
  if [[ -n $current_log && -f $serial ]]; then
    cp "$serial" "$current_log"
    record "$current_scenario" 0 unknown focused-run FAIL 0 "$current_log"
  fi
  stop_qemu
  exit "$rc"
}
trap cleanup EXIT
trap failure ERR

now_ms(){ echo $(( $(date +%s%N) / 1000000 )); }

assert_boundary(){
  local rounds=$1 log=$2
  grep -Eq "\\[SYNC\\]\\[BOUNDARY\\] PASS rounds=${rounds} .*signals_permit=0 .*target_hits=${rounds} .*sem_count=0 sem_waiters=0 .*cleanup=PASS observer_active=0 observer_inflight=0" "$log"
}

assert_noise(){
  local target=$1 foreign=$2 log=$3
  grep -Eq "\\[SYNC\\]\\[BOUNDARY_NOISE\\] PASS target_rounds=${target} foreign_rounds=${foreign} target_hits=${target} foreign_hits=[1-9][0-9]* foreign_waits=${foreign} foreign_signals=${foreign} foreign_errors=0 cleanup=PASS" "$log"
}

normal_build(){
  make kernel-check JOBS=2 >/dev/null
  make image >/dev/null
}

static_gate(){
  local begin end jobs
  begin=$(now_ms)
  current_scenario=static
  current_log=$artifact_dir/cl13fix1-static.log
  jobs=$(nproc)
  make stack-check JOBS=2
  cp artifacts/build/stack-usage-report.txt "$artifact_dir/cl13fix1-stack-usage.txt"
  make kernel-check JOBS=2
  cp kernel.elf /tmp/hobbyos-cl13fix1-j2.elf
  cp artifacts/build/kernel-check-j2.log "$artifact_dir/cl13fix1-build-j2.log"
  make kernel-check JOBS="$jobs"
  cp kernel.elf /tmp/hobbyos-cl13fix1-jN.elf
  cp "artifacts/build/kernel-check-j${jobs}.log" "$artifact_dir/cl13fix1-build-jN.log"
  cmp -s /tmp/hobbyos-cl13fix1-j2.elf /tmp/hobbyos-cl13fix1-jN.elf
  sha256sum /tmp/hobbyos-cl13fix1-j2.elf /tmp/hobbyos-cl13fix1-jN.elf \
    >"$artifact_dir/cl13fix1-static-hashes.log"
  make kernel-check JOBS=2 SELFTEST=1 SELFTEST_AUTORUN=1
  cp artifacts/build/kernel-check-j2.log "$artifact_dir/cl13fix1-build-selftest.log"
  make image SELFTEST=1 SELFTEST_AUTORUN=1 >/dev/null
  [[ -z $(nm -u kernel.elf) ]]
  ! grep -E 'kernel/src/(core/(semaphore|selftest)|shell/commands/cmd_synctest)\.c:.*warning:' \
    "$artifact_dir/cl13fix1-build-selftest.log"
  ! rg -n 'sem_test_set_after_prepare_hook' kernel/src
  rg -n 'sem_test_arm_after_prepare_observer|sem_test_clear_after_prepare_observer' \
    kernel/src/core/semaphore.* kernel/src/shell/commands/cmd_synctest.c >/dev/null
  ! rg -n 'volatile[[:space:]]+uint32_t[[:space:]]+phase|lost=' \
    kernel/src/shell/commands/cmd_synctest.c
  rg -n 'boundary-noise|BOUNDARY_NOISE' \
    kernel/src/shell/commands/cmd_synctest.c "$0" >/dev/null
  rg -n 'sem_signal_detailed|SEM_SIGNAL_WOKE_WAITER|SEM_SIGNAL_ADDED_PERMIT' \
    kernel/src/core/semaphore.* kernel/src/shell/commands/cmd_synctest.c >/dev/null
  bash -n "$0"
  normal_build
  end=$(now_ms)
  cp "$artifact_dir/cl13fix1-build-selftest.log" "$current_log"
  record static 0 host build+search PASS "$((end-begin))" "$current_log"
  current_log=
}

focused_boot(){
  local scenario=$1 smp=$2 accel=$3 run=$4 begin end
  begin=$(now_ms)
  current_scenario="${scenario}-run${run}"
  current_log="$artifact_dir/cl13fix1-${scenario}-run${run}.log"
  start_qemu "$smp" "$accel"
  framed_synctest_async BOUNDARY "synctest boundary 5000" \
    "[SYNC][BOUNDARY] START" "[SYNC][BOUNDARY] PASS" 360 stress
  assert_boundary 5000 "$serial"
  framed_synctest_async BOUNDARY_NOISE \
    "synctest boundary-noise 5000 5000" \
    "[SYNC][BOUNDARY_NOISE] START" "[SYNC][BOUNDARY_NOISE] PASS" 360 stress
  assert_boundary 5000 "$serial"
  assert_noise 5000 5000 "$serial"
  send_complete "synctest check" "[SYNC][CHECK] PASS" 90 stress
  send_complete "taskdiag check" "[TASKDIAG][CHECK] PASS" 90 stress
  cp "$serial" "$current_log"
  assert_clean_log "$current_log"
  stop_qemu
  end=$(now_ms)
  record "$current_scenario" "$smp" "$accel" \
    'boundary 5000 + boundary-noise 5000 5000 + checks' PASS \
    "$((end-begin))" "$current_log"
  current_log=
}

focused_group(){
  local scenario=$1 smp=$2 accel=$3 runs=$4 run
  normal_build
  harness_lock
  for ((run=1; run<=runs; run++)); do
    focused_boot "$scenario" "$smp" "$accel" "$run"
  done
  harness_unlock
}

noise_gate(){
  local begin end
  begin=$(now_ms)
  normal_build
  harness_lock
  current_scenario=boundary-noise
  current_log=$artifact_dir/cl13fix1-boundary-noise.log
  start_qemu 2 kvm
  framed_synctest_async BOUNDARY_NOISE \
    "synctest boundary-noise 5000 5000" \
    "[SYNC][BOUNDARY_NOISE] START" "[SYNC][BOUNDARY_NOISE] PASS" 360 stress
  assert_boundary 5000 "$serial"
  assert_noise 5000 5000 "$serial"
  send_complete "synctest check" "[SYNC][CHECK] PASS" 90 stress
  send_complete "taskdiag check" "[TASKDIAG][CHECK] PASS" 90 stress
  cp "$serial" "$current_log"
  assert_clean_log "$current_log"
  stop_qemu
  harness_unlock
  end=$(now_ms)
  record boundary-noise 2 kvm 'controlled foreign semaphore noise' PASS \
    "$((end-begin))" "$current_log"
  current_log=
}

negative_gate(){
  local begin end negative_log positive_log
  begin=$(now_ms)
  negative_log=$artifact_dir/cl13fix1-old-gap-negative.log
  positive_log=$artifact_dir/cl13fix1-old-gap-positive.log
  harness_lock
  current_scenario=old-gap-negative
  current_log=$negative_log
  make kernel-check JOBS=2 SELFTEST=1 SELFTEST_AUTORUN=0 \
    KERNEL_EXTRA_CFLAGS=-DHOBBYOS_SYNC_NEGATIVE_OLD_SEM_GAP >/dev/null
  make image SELFTEST=1 SELFTEST_AUTORUN=0 \
    KERNEL_EXTRA_CFLAGS=-DHOBBYOS_SYNC_NEGATIVE_OLD_SEM_GAP >/dev/null
  HARNESS_AUTORUN_EXPECTED=0
  start_qemu 2 tcg
  framed_synctest_async_negative "synctest boundary 5000" \
    "[SYNC][BOUNDARY] START" \
    "[SYNC][NEGATIVE] OLD_SEM_GAP_DETECTED" 360 stress "0 1"
  grep -Fq '[SYNC][NEGATIVE] OLD_SEM_GAP_DETECTED prepared=0 signal=ADDED_PERMIT waiters_before=0 cleanup=PASS' "$serial"
  ! grep -Fq 'OLD_SEM_GAP_MISSED' "$serial"
  cp "$serial" "$negative_log"
  assert_clean_log "$negative_log"
  stop_qemu

  normal_build
  HARNESS_AUTORUN_EXPECTED=1
  current_log=$positive_log
  start_qemu 2 tcg
  framed_synctest_async BOUNDARY "synctest boundary 1000" \
    "[SYNC][BOUNDARY] START" "[SYNC][BOUNDARY] PASS" 360 stress
  assert_boundary 1000 "$serial"
  framed_synctest_async BOUNDARY_NOISE \
    "synctest boundary-noise 1000 1000" \
    "[SYNC][BOUNDARY_NOISE] START" "[SYNC][BOUNDARY_NOISE] PASS" 360 stress
  assert_boundary 1000 "$serial"
  assert_noise 1000 1000 "$serial"
  send_complete "synctest check" "[SYNC][CHECK] PASS" 90 stress
  send_complete "taskdiag check" "[TASKDIAG][CHECK] PASS" 90 stress
  cp "$serial" "$positive_log"
  assert_clean_log "$positive_log"
  stop_qemu
  harness_unlock
  end=$(now_ms)
  record old-gap-negative 2 tcg \
    'causal ADDED_PERMIT detection + normal rebuild canaries' PASS \
    "$((end-begin))" "$negative_log"
  current_log=
}

classification(){
  local artifact=$artifact_dir/cl13fix1-boundary-classification.log scenario run
  local -a required=(static boundary-noise old-gap-negative)
  for run in 1 2 3; do
    required+=("smp1-tcg-run${run}" "smp2-tcg-run${run}"
               "smp4-kvm-run${run}")
  done
  for run in 1 2 3 4 5; do
    required+=("smp2-kvm-run${run}")
  done
  for scenario in "${required[@]}"; do
    [[ $(awk -F '\t' -v wanted="$scenario" '
      $1 == "boundary-focused" && $2 == wanted { status=$7 }
      END { print status }
    ' "$ledger") == PASS ]]
  done
  current_scenario=classification
  current_log=$artifact
  printf '[CL13][BOUNDARY_CLASSIFICATION] HARNESS_FALSE_POSITIVE_CONFIRMED\n' \
    >"$artifact"
  record classification 0 host 'focused matrix + noise + old-gap' PASS 0 \
    "$artifact"
  current_log=
}

ensure_ledger
case ${1:-} in
  static) static_gate;;
  smp1-tcg) focused_group smp1-tcg 1 tcg 3;;
  smp2-tcg) focused_group smp2-tcg 2 tcg 3;;
  smp2-kvm) [[ -r /dev/kvm && -w /dev/kvm ]] || exit 3; focused_group smp2-kvm 2 kvm 5;;
  smp4-kvm) [[ -r /dev/kvm && -w /dev/kvm ]] || exit 3; focused_group smp4-kvm 4 kvm 3;;
  noise) [[ -r /dev/kvm && -w /dev/kvm ]] || exit 3; noise_gate;;
  negative) negative_gate; classification;;
  classify) classification;;
  all)
    static_gate
    focused_group smp1-tcg 1 tcg 3
    focused_group smp2-tcg 2 tcg 3
    [[ -r /dev/kvm && -w /dev/kvm ]] || exit 3
    focused_group smp2-kvm 2 kvm 5
    focused_group smp4-kvm 4 kvm 3
    noise_gate
    negative_gate
    classification
    ;;
  *) usage;;
esac

stop_qemu
trap - ERR EXIT
