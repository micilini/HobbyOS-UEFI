#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
serial=.qemu/qemu-serial.log
hmp=(python3 scripts/qemu_hmp.py --socket .qemu/hmp.sock)
mkdir -p artifacts/build

fail(){ echo "[CL14][$1] FAIL ${2:-}" >&2; exit 1; }

preflight(){
  [[ $(git branch --show-current) == feat/taskman ]] || fail PREFLIGHT branch
  [[ $(git rev-parse HEAD) == f9d47db7e4332fc2d88dce2842f997a9b3f82e57 ]] || fail PREFLIGHT head
  [[ $(git rev-parse HEAD^) == 4e700a9b38653fb57ecb0f107a18f87508db0736 ]] || fail PREFLIGHT parent
  [[ $(git log -1 --format=%s) == 'test(taskman): add automated V1 SMP, lifecycle, input and UI validation' ]] || fail PREFLIGHT subject
  [[ $(git rev-list --count 4e700a9b38653fb57ecb0f107a18f87508db0736..HEAD) == 1 ]] || fail PREFLIGHT count
  git diff --check
  git diff --cached --check
  echo '[CL14][PREFLIGHT] PASS'
}

evidence(){ python3 scripts/verify-taskman-v1-evidence.py all; }
docs(){ python3 scripts/verify-taskman-v1-docs.py all; }

ux_static(){
  local matches
  matches=$(rg -n -i 'fase[[:space:]]*[0-9]|phase[[:space:]]*[0-9]|TMV1|CL-[0-9]' \
    kernel bootloader shared || true)
  [[ -z $matches ]] || { printf '%s\n' "$matches" >&2; fail UX_STATIC historical-text; }
  rg -n 'tasktest' kernel/src/shell/commands/registry.c | grep -q HOBBYOS_SELFTEST -B1 -A2 || \
    grep -q '#ifdef HOBBYOS_SELFTEST' kernel/src/shell/commands/registry.c
  echo '[CL14][UX_STATIC] PASS'
}

build(){
  local log=artifacts/build/cl14-build.log jobs owned
  : >"$log"
  make clean 2>&1 | tee -a "$log"
  make stack-check 2>&1 | tee -a "$log"
  make kernel-check JOBS=2 2>&1 | tee -a "$log"
  cp kernel.elf /tmp/hobbyos-cl14-j2.elf
  jobs=$(nproc)
  make kernel-check JOBS="$jobs" 2>&1 | tee -a "$log"
  cp kernel.elf /tmp/hobbyos-cl14-jN.elf
  cmp -s /tmp/hobbyos-cl14-j2.elf /tmp/hobbyos-cl14-jN.elf
  make kernel-check JOBS=2 SELFTEST=1 SELFTEST_AUTORUN=1 2>&1 | tee -a "$log"
  make clean 2>&1 | tee -a "$log"
  make kernel-check JOBS=2 2>&1 | tee -a "$log"
  make deps-check 2>&1 | tee -a "$log"
  make image 2>&1 | tee -a "$log"
  nm -u kernel.elf | tee artifacts/build/cl14-nm-u.log
  [[ ! -s artifacts/build/cl14-nm-u.log ]] || fail BUILD undefined-symbols
  sha256sum kernel.elf hobbyos.img | tee artifacts/build/cl14-release-hashes.sha256
  owned=$(grep -E 'kernel/src/shell/commands/cmd_(taskmantest|modaltest)\.c:.*warning:' "$log" || true)
  [[ -z $owned ]] || fail BUILD cl14-owned-warning
  echo '[CL14][BUILD] PASS' | tee -a "$log"
}

wait_new(){
  local start=$1 pattern=$2 timeout=${3:-90} deadline
  deadline=$((SECONDS + timeout))
  while ((SECONDS <= deadline)); do
    tail -n +$((start + 1)) "$serial" | grep -Fq "$pattern" && return 0
    [[ -r .qemu/qemu.pid ]] && kill -0 "$(<.qemu/qemu.pid)" 2>/dev/null || return 1
    sleep 1
  done
  return 1
}

type_command(){
  local command=$1 marker=$2 timeout=${3:-90} start
  start=$(wc -l <"$serial")
  "${hmp[@]}" text "$command" --enter --profile sync >/dev/null
  wait_new "$start" "$marker" "$timeout" || fail RELEASE_SMOKE "command=$command marker=$marker"
}

stop_qemu(){
  if [[ -r .qemu/qemu.pid ]]; then scripts/qemu-agent.sh stop >/dev/null || true; fi
}

release_smoke(){
  local accel=kvm start first second
  [[ -e /dev/kvm && -r /dev/kvm && -w /dev/kvm ]] || accel=tcg
  trap stop_qemu EXIT
  stop_qemu
  SMP=4 ACCEL=$accel scripts/qemu-agent.sh start >/dev/null
  source scripts/harness-runtime-ready.sh
  runtime_wait_boot_ready "$serial" 240 0 || fail RELEASE_SMOKE readiness
  grep -Fq '[BOOT][RUNTIME_READY] PASS cpus=4/4' "$serial" || fail RELEASE_SMOKE cpus
  grep -Fq '[BOOT][TEST_READY] PASS autorun=0 selftests=0' "$serial" || fail RELEASE_SMOKE test-ready
  ! grep -Eq '\[SELFTEST\]\[AUTORUN\] (BEGIN|PASS|FAIL)' "$serial" || fail RELEASE_SMOKE autorun
  ! grep -Eiq 'fase[[:space:]]*[0-9]|phase[[:space:]]*[0-9]' "$serial" || fail RELEASE_SMOKE phase-banner

  type_command 'help ps' 'Command: ps'
  type_command 'help kill' 'Command: kill'
  type_command 'help taskman' 'Command: taskman'
  type_command 'help taskdiag' 'Command: taskdiag'
  type_command 'help tasktest' "Command 'tasktest' not found."

  start=$(wc -l <"$serial")
  "${hmp[@]}" text ps --enter --profile sync >/dev/null
  wait_new "$start" '[PS][WINDOW] baseline=0' 90 || fail RELEASE_SMOKE ps-initial
  first=$(tail -n +$((start + 1)) "$serial" | grep -F '[PS][WINDOW]' | tail -1)
  start=$(wc -l <"$serial")
  "${hmp[@]}" text ps --enter --profile sync >/dev/null
  wait_new "$start" '[PS][WINDOW] baseline=1' 90 || fail RELEASE_SMOKE ps-window
  second=$(tail -n +$((start + 1)) "$serial" | grep -F '[PS][WINDOW]' | tail -1)
  [[ $first == *'baseline=0'* && $second == *'baseline=1'* ]] || fail RELEASE_SMOKE ps

  start=$(wc -l <"$serial")
  "${hmp[@]}" text 'taskman 1000' --enter --profile sync >/dev/null
  wait_new "$start" '[TASKMAN][OWNER_STATE] shell=BLOCKED session=ACTIVE' 90 || fail RELEASE_SMOKE taskman-begin
  "${hmp[@]}" key esc --profile sync >/dev/null
  wait_new "$start" '[MODAL] session_end OK' 90 || fail RELEASE_SMOKE taskman-end
  type_command 'taskdiag check' '[TASKDIAG][CHECK] PASS'
  "${hmp[@]}" text version --enter --profile sync >/dev/null
  sleep 2

  ! grep -Eq 'PANIC|#PF|#GP|FATAL|STRUCTURAL_FAULT|FINISH_FAULT' "$serial" || fail RELEASE_SMOKE guest-fault
  cp "$serial" artifacts/build/cl14-release-smoke.log
  printf '[CL14][RELEASE_SMOKE] PASS accel=%s smp=4 cpus=4 tasktest=absent ps=PASS taskman=PASS taskdiag=PASS faults=0\n' "$accel" |
    tee artifacts/build/cl14-release-smoke-summary.log
  stop_qemu
  trap - EXIT
}

package(){ scripts/package-taskman-v1-release.sh | tee artifacts/build/cl14-package.log; }

handoff(){
  python3 scripts/verify-taskman-v1-docs.py v2-gate
  grep -Fq 'TASKMAN V1 APPROVED' docs/taskman-v1-homologation.md
  grep -Fq 'TASKMAN V2 ENTRY: AUTHORIZED_NOT_STARTED' docs/taskman-v2-entry-criteria.md
  grep -Fq 'BARE-METAL ACCEPTANCE' docs/taskman-v1-handoff-checklist.md
  echo '[CL14][HANDOFF] PASS'
}

case ${1:-all} in
  preflight) preflight ;;
  evidence) evidence ;;
  docs) docs ;;
  ux-static) ux_static ;;
  build) build ;;
  release-smoke) release_smoke ;;
  package) package ;;
  handoff) handoff ;;
  all)
    preflight
    evidence
    docs
    ux_static
    build
    release_smoke
    package
    handoff
    echo '[CL14][RELEASE] PASS'
    ;;
  *) echo 'usage: scripts/test-cl14-release.sh {preflight|evidence|docs|ux-static|build|release-smoke|package|handoff|all}' >&2; exit 2 ;;
esac
