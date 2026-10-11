#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

focused_artifact_dir=${TASKMAN_VISUAL_EVIDENCE_DIR:-artifacts/build/taskman-visual-final}
focused_screen_dir=$focused_artifact_dir/screens
serial=.qemu/qemu-serial.log
hmp=(python3 scripts/qemu_hmp.py --socket .qemu/hmp.sock)
visual_base_commit=4432d82edae9e757608de2c84146b4e12548a273
visual_base_tree=cbe0bd60748fd33a821eb9405deae3cb5c904e03
HARNESS_AUTORUN_EXPECTED=0
source scripts/harness-common.sh
source scripts/harness-runtime-ready.sh

mkdir -p "$focused_artifact_dir" "$focused_screen_dir"
trap stop_qemu EXIT

active_files=(
  kernel/src/shell/commands/cmd_taskman.c
  kernel/src/shell/commands/cmd_taskman.h
  kernel/src/shell/commands/cmd_taskmantest.c
  kernel/src/shell/commands/cmd_taskmantest.h
  kernel/src/shell/commands/taskman_view.c
  kernel/src/shell/commands/taskman_view.h
  kernel/src/shell/commands/cmd_accounttest.h
  scripts/test-taskman-visual.sh
  scripts/taskman-capture.py
  scripts/verify-taskman-visual-evidence.py
  makefile
  AGENTS.md
  docs/taskman-v1-visual-contract.md
  docs/taskman-v1/reports/TASKMAN_VISUAL_REFINEMENT.md
  docs/test-reports/TASKMAN_VISUAL_CERTIFICATION.md
  docs/test-reports/TASKMAN_VISUAL_HUMAN_CHECKLIST.md
)

check_active_naming(){
  local file pattern matches=$focused_artifact_dir/naming-scan.log
  for file in "${active_files[@]}"; do
    [[ -f $file ]] || {
      printf 'missing active interface: %s\n' "$file" >&2
      echo REJECTED_TASKMAN_VISUAL_BASELINE >&2
      return 1
    }
  done
  pattern='tm''v1|u''x[-_]?0*1|f''ix[0-9]+|c''l[-_]?0*[0-9]+|fase[ _-]*[0-9]+|p''hase[ _-]*[0-9]+'
  if rg -n -i "$pattern" "${active_files[@]}" >"$matches"; then
    cat "$matches" >&2
    echo REJECTED_TASKMAN_ACTIVE_NAMING >&2
    return 1
  fi
  : >"$matches"
  echo '[TASKMAN][ACTIVE_NAMING] PASS' | tee "$focused_artifact_dir/naming.log"
}

focused_preflight(){
  if [[ ${HOBBYOS_TASKMAN_VISUAL_CURRENT:-0} == 1 ]]; then
    python3 scripts/memory_current.py taskman-visual >"$focused_artifact_dir/current-scope.json"
    [[ -n ${MEMORY_CURRENT_CANDIDATE:-} ]]
    git diff --check
    echo '[TASKMAN][VISUAL_PREFLIGHT] PASS contract=current'
    return
  fi
  [[ $(git branch --show-current) == feat/taskman ]]
  [[ $(git rev-parse HEAD) == "$visual_base_commit" ]]
  [[ $(git rev-parse 'HEAD^{tree}') == "$visual_base_tree" ]]
  [[ $(sha256sum artifacts/release/TASKMAN_V1/kernel.elf | awk '{print $1}') == \
     9f8fcc9417e68118ef8fcde4c2af8f9dadd5eb138397b7e4ef15585e9463b973 ]]
  [[ $(sha256sum artifacts/release/TASKMAN_V1/hobbyos.img | awk '{print $1}') == \
     424b2b6656d9d8cf772bb173f4f99ccb347d1e005ef3cb1e0db4cb0d80c27b48 ]]
  git diff --check
  echo '[TASKMAN][VISUAL_PREFLIGHT] PASS'
}

focused_static(){
  bash -n scripts/test-taskman-visual.sh
  python3 -B - <<'PYTHON'
from pathlib import Path
for name in ('taskman-capture.py', 'verify-taskman-visual-evidence.py'):
    compile((Path('scripts') / name).read_text(), name, 'exec')
PYTHON
  check_active_naming
  echo '[TASKMAN][VISUAL_STATIC] PASS'
}

build_selftest(){
  if [[ ${HOBBYOS_TASKMAN_VISUAL_CURRENT:-0} == 1 ]]; then
    python3 -B - "$MEMORY_CURRENT_CANDIDATE" <<'PYTHON'
import json, sys, shutil
from pathlib import Path
sys.path.insert(0, 'scripts')
import memory_evidence as E
root = Path(sys.argv[1])
c = json.loads((root / 'candidate.json').read_text())
E.verify_candidate(c, json.loads((root / 'build-receipt.json').read_text()))
E.require('SELFTEST=1' in c['profile'], 'TASKMAN requires SELFTEST candidate')
for name, row in c['artifacts'].items():
    E.require(E.sha(root / name) == row['sha256'], 'TASKMAN payload candidate mismatch')
    shutil.copyfile(root / name, Path(name))
PYTHON
    return
  fi
  make kernel-check JOBS="$(nproc)" SELFTEST=1 SELFTEST_AUTORUN=0
  make image SELFTEST=1 SELFTEST_AUTORUN=0
}

boot(){
  harness_lock
  start_qemu 4 kvm
}

finish_boot(){
  stop_qemu
  harness_unlock
}

focused_modal_session(){
  local mode=$1 cols=$2 begin end owner stats frames
  send_complete "taskmantest geometry-runtime $cols 40" \
    '[TASKMANTEST][GEOMETRY_RUNTIME] PASS' 120
  send_complete 'taskmantest anchor-reset' \
    '[TASKMANTEST][ANCHOR_RESET] PASS' 120
  begin=$(count '[MODAL] session_begin OK')
  end=$(count '[MODAL] session_end OK')
  owner=$(count '[TASKMAN][OWNER_STATE] shell=BLOCKED session=ACTIVE')
  harness_command="taskman $mode smoke"
  harness_start_line=$(wc -l <"$serial")
  hmp_text 'taskman 1000' normal --enter >/dev/null
  wait_new '[MODAL] session_begin OK' "$begin" 120
  wait_new '[TASKMAN][OWNER_STATE] shell=BLOCKED session=ACTIVE' "$owner" 120
  sleep 2
  hmp_key esc normal >/dev/null
  wait_new '[MODAL] session_end OK' "$end" 120
  shell_sync
  send_complete 'taskmantest stats' '[TASKMANTEST][STATS]' 120
  stats=$(grep -F '[TASKMANTEST][STATS]' "$serial" | tail -1)
  grep -Eq "last_mode=$mode([[:space:]]|$)" <<<"$stats"
  frames=$(sed -nE 's/.*last_session_full_frames=([0-9]+).*/\1/p' <<<"$stats")
  [[ -n $frames && $frames -ge 1 ]]
  grep -Eq 'last_session_fallback_frames=0([[:space:]]|$)' <<<"$stats"
  grep -Eq 'stable_frame_full_clears=0([[:space:]]|$)' <<<"$stats"
  grep -Eq 'stale_cells=0([[:space:]]|$)' <<<"$stats"
  grep -Eq 'clipped=0([[:space:]]|$)' <<<"$stats"
  grep -Eq 'last_session_scroll_delta=0([[:space:]]|$)' <<<"$stats"
  grep -Eq 'workspace_live=0([[:space:]]|$)' <<<"$stats"
  printf '[TASKMAN][%s_SMOKE] PASS full_frames=%s\n' "$mode" "$frames"
}

focused_visual_gate(){
  local log=$focused_artifact_dir/smoke.log
  {
    focused_preflight
    focused_static
    [[ -r /dev/kvm && -w /dev/kvm ]] || {
      echo BLOCKED_BY_EXTERNAL_ENVIRONMENT >&2
      return 1
    }
    build_selftest >"$focused_artifact_dir/selftest-build.log" 2>&1
    boot
    send_complete 'taskmantest stats-reset' '[TASKMANTEST][STATS_RESET] PASS' 120
    focused_modal_session WIDE 118
    focused_modal_session COMPACT 76
    send_complete 'taskmantest geometry-clear' \
      '[TASKMANTEST][GEOMETRY_RUNTIME] CLEARED' 120
    send_complete 'inputtest check' '[INPUTTEST][CHECK] PASS' 120
    send_complete 'modaltest check' '[MODALTEST][CHECK] PASS' 120
    send_complete 'taskdiag check' '[TASKDIAG][CHECK] PASS' 120
    send_complete 'taskmantest check' '[TASKMANTEST][CHECK] PASS' 120
    cp "$serial" "$focused_artifact_dir/smoke-serial.log"
    assert_clean_log "$focused_artifact_dir/smoke-serial.log"
    finish_boot
    echo '[TASKMAN][FINAL_SMOKE] PASS'
  } 2>&1 | tee "$log"
}

usage(){
  echo 'usage: scripts/test-taskman-visual.sh naming|image-payload|focused' >&2
  exit 2
}

case ${1:-} in
  current)
    export HOBBYOS_TASKMAN_VISUAL_CURRENT=1
    focused_visual_gate
    python3 scripts/verify-taskman-visual-evidence.py current \
      --evidence "$focused_artifact_dir" --candidate "$MEMORY_CURRENT_CANDIDATE"
    ;;
  naming) check_active_naming;;
  image-payload) python3 scripts/verify-taskman-visual-evidence.py all;;
  focused) focused_visual_gate;;
  *) usage;;
esac
