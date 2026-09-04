#!/usr/bin/env bash
set -euo pipefail

readonly EXPECTED_BRANCH=feat/taskman
readonly EXPECTED_HEAD=88645901d9393f4796af84cecd9e13d3d6671c70

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
cd "$REPO_ROOT"

readonly ARTIFACT_DIR=artifacts/build
readonly PREVIEW_FILE=$ARTIFACT_DIR/cl11fix14-autoclean-process-preview.tsv
readonly ARM_FILE=$ARTIFACT_DIR/cl11fix14-autoclean-arm.env
readonly RUNNER=$REPO_ROOT/scripts/run-lapic-kvm-clean-host.sh
readonly WORKER=$REPO_ROOT/scripts/run-lapic-kvm-auto-clean-worker.sh

readonly -a ALLOWLIST=(
  chrome chromium chromium-browse brave brave-browser firefox slack code
  code-insiders codium VSCodium Discord discord steam steamwebhelper spotify
  teams-for-linux msedge opera vivaldi-bin
)

readonly -a PROTECTED=(
  bash zsh fish sh sshd ssh gnome-terminal gnome-terminal-server
  gnome-terminal- konsole xterm systemd dbus-daemon dbus-broker Xorg Xwayland
  gnome-shell plasmashell NetworkManager pipewire wireplumber pulseaudio login
  agetty
)

fail() {
  printf '[LAPIC_AUTOCLEAN][ENVIRONMENT] FAIL reason=%s\n' "$1" >&2
  shift
  if (($# > 0)); then
    printf '%s\n' "$@" >&2
  fi
  exit 1
}

usage() {
  printf 'Usage: %s --preview|--arm\n' "$0" >&2
  exit 2
}

in_array() {
  local needle=$1 item
  shift
  for item in "$@"; do
    [[ $needle == "$item" ]] && return 0
  done
  return 1
}

action_for_comm() {
  local comm=$1
  if in_array "$comm" "${ALLOWLIST[@]}"; then
    printf 'TERM_THEN_KILL_IF_NEEDED\n'
  elif in_array "$comm" "${PROTECTED[@]}"; then
    printf 'PROTECTED\n'
  else
    printf 'OBSERVE_ONLY\n'
  fi
}

write_process_snapshot() {
  local destination=$1 temporary
  local pid comm pcpu pmem action

  temporary=$destination.tmp.$$
  mkdir -p "$(dirname "$destination")"
  printf 'pid\tcomm\tpcpu\tpmem\taction\n' >"$temporary"
  while read -r pid comm pcpu pmem; do
    [[ $pid =~ ^[0-9]+$ && -n $comm ]] || continue
    action=$(action_for_comm "$comm")
    printf '%s\t%s\t%s\t%s\t%s\n' "$pid" "$comm" "$pcpu" "$pmem" "$action" \
      >>"$temporary"
  done < <(LC_ALL=C ps -u "$UID" -o pid=,comm=,pcpu=,pmem=)
  mv "$temporary" "$destination"
}

print_allowlisted_preview() {
  local source=$1
  printf '%s\n' 'SAVE UNSAVED WORK NOW.'
  printf '%s\n' 'These applications will receive SIGTERM and may later receive SIGKILL.'
  printf '%s\n' '[LAPIC_AUTOCLEAN][PREVIEW] allowlisted-processes'
  if ! awk -F '\t' 'NR > 1 && $5 == "TERM_THEN_KILL_IF_NEEDED" {
      printf "pid=%s comm=%s pcpu=%s pmem=%s action=%s\n", $1, $2, $3, $4, $5
      found=1
    } END { exit found ? 0 : 1 }' "$source"; then
    printf '%s\n' '(none)'
  fi
}

detect_detach_method() {
  if command -v systemd-run >/dev/null 2>&1 &&
    command -v systemctl >/dev/null 2>&1 &&
    systemctl --user show-environment >/dev/null 2>&1; then
    DETACH_METHOD=systemd-run
    return 0
  fi
  if command -v nohup >/dev/null 2>&1 && command -v setsid >/dev/null 2>&1; then
    DETACH_METHOD=nohup-setsid
    return 0
  fi
  fail no-detach-method \
    'Neither a functional systemd-run --user session nor nohup + setsid is available.'
}

preflight() {
  local branch head qemu_version

  if ((EUID == 0)); then
    fail do-not-run-as-root
  fi
  command -v git >/dev/null 2>&1 || fail git-missing
  branch=$(git branch --show-current)
  head=$(git rev-parse HEAD)
  [[ $branch == "$EXPECTED_BRANCH" ]] || \
    fail branch-mismatch "expected=$EXPECTED_BRANCH actual=$branch"
  [[ $head == "$EXPECTED_HEAD" ]] || \
    fail head-mismatch "expected=$EXPECTED_HEAD actual=$head"

  [[ -f $RUNNER ]] || fail fix13-runner-missing "$RUNNER"
  [[ -f $WORKER ]] || fail autoclean-worker-missing "$WORKER"
  bash -n "$RUNNER" || fail fix13-runner-syntax
  bash -n "$WORKER" || fail autoclean-worker-syntax
  [[ -f kernel.elf ]] || fail kernel-elf-missing 'Builds are not run automatically.'
  [[ -f hobbyos.img ]] || fail hobbyos-image-missing 'Builds are not run automatically.'
  [[ -e /dev/kvm && -r /dev/kvm && -w /dev/kvm ]] || \
    fail kvm-unavailable '/dev/kvm must be readable and writable by the normal project user.'
  command -v qemu-system-x86_64 >/dev/null 2>&1 || fail qemu-missing
  qemu_version=$(qemu-system-x86_64 --version | sed -n '1p')
  [[ $qemu_version == 'QEMU emulator version 8.2.2 '* ]] || \
    fail qemu-version-mismatch "expected=8.2.2 actual=$qemu_version"

  detect_detach_method

  shopt -s nullglob
  local existing=("$ARTIFACT_DIR"/cl11fix13-clean-kvm-*)
  shopt -u nullglob
  ((${#existing[@]} == 0)) || fail fix13-artifacts-already-exist \
    'Preserve the existing FIX13 batch; the launcher will not replace it.'

  printf '[LAPIC_AUTOCLEAN][PREFLIGHT] PASS branch=%s head=%s qemu=8.2.2 detach=%s\n' \
    "$branch" "$head" "$DETACH_METHOD"
}

preview() {
  preflight
  write_process_snapshot "$PREVIEW_FILE"
  print_allowlisted_preview "$PREVIEW_FILE"
  printf '[LAPIC_AUTOCLEAN][PREVIEW] PASS file=%s\n' "$PREVIEW_FILE"
}

arm() {
  local run_id unit output worker_pid=NA

  preflight
  [[ -f $PREVIEW_FILE ]] || fail preview-missing \
    'Run scripts/launch-lapic-kvm-auto-clean.sh --preview before requesting approval.'

  run_id=$(date -u +%Y%m%dT%H%M%SZ)-$$-$RANDOM
  unit=hobbyos-lapic-autoclean-$run_id

  if [[ $DETACH_METHOD == systemd-run ]]; then
    if ! output=$(systemd-run --user \
      --unit="$unit" \
      --property=Type=exec \
      --property=CollectMode=inactive-or-failed \
      --working-directory="$REPO_ROOT" \
      "$WORKER" --run-id "$run_id" 2>&1); then
      if systemctl --user is-active --quiet "$unit.service" 2>/dev/null; then
        fail systemd-run-failed-unit-active "$output"
      fi
      if ! command -v nohup >/dev/null 2>&1 || ! command -v setsid >/dev/null 2>&1; then
        fail systemd-run-failed-no-fallback "$output"
      fi
      DETACH_METHOD=nohup-setsid
      setsid nohup "$WORKER" --run-id "$run_id" </dev/null >/dev/null 2>&1 &
      worker_pid=$!
      unit=NA
    else
      worker_pid=$(systemctl --user show "$unit.service" -p MainPID --value 2>/dev/null || true)
      worker_pid=${worker_pid:-NA}
    fi
  else
    setsid nohup "$WORKER" --run-id "$run_id" </dev/null >/dev/null 2>&1 &
    worker_pid=$!
    unit=NA
  fi

  {
    printf 'RUN_ID=%q\n' "$run_id"
    printf 'METHOD=%q\n' "$DETACH_METHOD"
    printf 'UNIT=%q\n' "$unit"
    printf 'WORKER_PID=%q\n' "$worker_pid"
    printf 'ARMED_AT=%q\n' "$(date --iso-8601=seconds)"
  } >"$ARM_FILE"

  printf '[LAPIC_AUTOCLEAN][ARMED] PASS method=%s unit=%s pid=%s run_id=%s\n' \
    "$DETACH_METHOD" "$unit" "$worker_pid" "$run_id"
  printf '%s\n' 'Type /exit now.'
  printf '%s\n' 'The detached worker will begin after all Codex processes exit.'
}

(($# == 1)) || usage
case $1 in
  --preview) preview ;;
  --arm) arm ;;
  *) usage ;;
esac
