#!/usr/bin/env bash
set -euo pipefail

CONTROL_ROOT=$(git rev-parse --show-toplevel)
DELIVERY_CWD=$PWD
JOBS=${JOBS:-$(nproc)}
STAMP=$(date +%Y%m%d-%H%M%S%z)
CANDIDATE_ROOT=$CONTROL_ROOT
RUN_MODE=${1:-}
if [[ $RUN_MODE == focal ]]; then
  CANDIDATE_ROOT=$(realpath "${2:?candidate root required}")
  CANDIDATE_LABEL=${3:?candidate label required}
elif [[ $RUN_MODE == existing-all || $RUN_MODE == existing-transport ||
        $RUN_MODE == existing-rejection ]]; then
  EXISTING_CANDIDATE=$(realpath "${2:?existing candidate directory required}")
  CANDIDATE_LABEL=${3:?candidate label required}
else
  CANDIDATE_LABEL=current
fi
EVIDENCE=${FOUNDATION_EVIDENCE_DIR:-$CONTROL_ROOT/artifacts/build/panic-safety/$STAMP/foundation-regression}
mkdir -p "$EVIDENCE/candidate" "$EVIDENCE/profiles" "$EVIDENCE/commands"
EVIDENCE=$(realpath "$EVIDENCE")
exec > >(tee -a "$EVIDENCE/foundation-regression.log") 2>&1

CURRENT_RUNTIME=
CURRENT_PROFILE=
CURRENT_STARTED=0
CURRENT_PID=
CURRENT_VM_ID=
COMMAND_SEQUENCE=0
LAST_RATE_RAW=
PROFILE_ABORTED=0
CLEANUP_RUNNING=0
CURRENT_INPUT_PROFILE=normal
CURRENT_CANDIDATE_ELF=$EVIDENCE/candidate/kernel.elf
CURRENT_INPUT_LAYOUT=$EVIDENCE/candidate/command-input-layout.json
RUNTIME_PARENT=${FOUNDATION_RUNTIME_PARENT:-${TMPDIR:-/tmp}}

run_logged() {
  local log=$1
  shift
  printf '%q ' "$@" > "$EVIDENCE/commands/$(basename "$log" .log).txt"
  printf '\n' >> "$EVIDENCE/commands/$(basename "$log" .log).txt"
  set +e
  "$@" 2>&1 | tee "$log"
  local status=${PIPESTATUS[0]}
  set -e
  printf '%s\n' "$status" > "${log%.log}.status"
  return "$status"
}

capture_gdb_failure_snapshot() {
  local elf=$1
  local tag=$2
  local output="$CURRENT_PROFILE/${tag}-gdb-snapshot.log"
  local status=0
  timeout 45s gdb -q -nx -batch "$elf" \
    -ex 'set pagination off' \
    -ex 'set confirm off' \
    -ex 'set remotetimeout 10' \
    -ex "target remote $CURRENT_RUNTIME/gdb.sock" \
    -ex 'info threads' \
    -ex 'thread apply all bt 24' \
    -ex 'x/2wx &g_heap_lock' \
    -ex 'x/2wx &g_dpc_lock' \
    -ex 'x/2wx &g_scheduler_lock' \
    -ex 'x/2wx &g_console_lock' \
    -ex 'x/2wx &g_shell_lock' \
    -ex 'x/8wx &g_router' \
    -ex 'x/2wx &g_xhci_cmd_lock' \
    -ex 'x/2wx &g_xhci_event_lock' \
    -ex 'x/1wx &xhci_isr_in_progress' \
    -ex 'x/1wx &xhci_processing_events' \
    -ex 'x/4gx &xhci_last_successful_process_ms' \
    -ex 'x/1gx &xhci_dbg_isr_count' \
    -ex 'detach' > "$output" 2>&1 || status=$?
  printf '%s\n' "$status" > "${output%.log}.status"
  python3 "$CONTROL_ROOT/scripts/qemu_hmp.py" \
    --socket "$CURRENT_RUNTIME/hmp.sock" command cont >> "$output" 2>&1 || true
}

append_profile() {
  printf '%s=%s\n' "$1" "$2" >> "$CURRENT_PROFILE/profile.env"
}

qemu_agent() {
  local action=$1
  shift
  (cd "$CANDIDATE_ROOT" && QEMU_RUNTIME="$CURRENT_RUNTIME" \
    scripts/qemu-agent.sh "$action" "$@")
}

archive_runtime() {
  local runtime=$1 archive=$2
  mkdir -p "$archive"
  cp -a "$runtime/." "$archive/"
  rm -rf -- "$runtime"
}

pid_active() {
  local pid=$1
  [[ $pid =~ ^[1-9][0-9]*$ ]] && kill -0 "$pid" 2>/dev/null
}

cleanup_active_vm() {
  local reason=${1:-trap} cleanup_status=PASS
  ((CLEANUP_RUNNING == 0)) || return 0
  CLEANUP_RUNNING=1
  if [[ -n $CURRENT_RUNTIME ]]; then
    if [[ -n $CURRENT_PID ]] && pid_active "$CURRENT_PID"; then
      if ! qemu_agent stop > "$CURRENT_PROFILE/cleanup-stop.log" 2>&1; then
        cleanup_status=FAIL
      fi
    fi
    if [[ -n $CURRENT_PID ]] && pid_active "$CURRENT_PID"; then
      cleanup_status=FAIL
    fi
    if [[ -e $CURRENT_RUNTIME/qemu.pid || -S $CURRENT_RUNTIME/hmp.sock ]]; then
      cleanup_status=FAIL
    fi
    mkdir -p "$CURRENT_PROFILE/qemu"
    cp -a "$CURRENT_RUNTIME/." "$CURRENT_PROFILE/qemu/"
    printf 'cleanup=%s\ncleanup_reason=%s\nrun_complete=NO\n' \
      "$cleanup_status" "$reason" >> "$CURRENT_PROFILE/profile.env"
    rm -rf -- "$CURRENT_RUNTIME"
  fi
  CURRENT_RUNTIME=
  CURRENT_PROFILE=
  CURRENT_PID=
  CURRENT_VM_ID=
  CLEANUP_RUNNING=0
  [[ $cleanup_status == PASS ]]
}

signal_exit() {
  local signal=$1 status=$2 cleanup_status=0
  cleanup_active_vm "signal-$signal" || cleanup_status=$?
  trap - EXIT INT TERM
  if ((cleanup_status != 0)); then
    printf 'foundation-regression: cleanup failed while handling %s\n' "$signal" >&2
  fi
  exit "$status"
}

trap 'cleanup_active_vm exit' EXIT
trap 'signal_exit INT 130' INT
trap 'signal_exit TERM 143' TERM

record_command_tsv() {
  python3 - "$CURRENT_PROFILE/commands.jsonl" \
    "$CURRENT_PROFILE/commands.tsv" <<'PY'
import json,sys
source,target=sys.argv[1:]
row=json.loads(open(source,encoding='utf-8').read().splitlines()[-1])
fields=(row.get('sequence'),row.get('payload'),row.get('crc32'),
        row.get('disposition'),row.get('transport_status'),
        row.get('handler_status'),row.get('classification'),
        row.get('start_line_count'),row.get('end_line_count'),
        row.get('accepted_line'),row.get('begin_line'),row.get('end_line'))
with open(target,'a',encoding='utf-8') as stream:
    stream.write('\t'.join('' if value is None else str(value)
                           for value in fields)+'\n')
PY
}

last_command_field() {
  python3 - "$CURRENT_PROFILE/commands.jsonl" "$1" <<'PY'
import json,sys
row=json.loads(open(sys.argv[1],encoding='utf-8').read().splitlines()[-1])
value=row.get(sys.argv[2])
print('' if value is None else value)
PY
}

run_frame() {
  local payload=$1 timeout=${2:-240} allowed=${3:-0} modal=${4:-}
  COMMAND_SEQUENCE=$((COMMAND_SEQUENCE + 1))
  local log="$CURRENT_PROFILE/command-${COMMAND_SEQUENCE}.json"
  local transport="$CURRENT_PROFILE/transport-${COMMAND_SEQUENCE}.log"
  local args=(frame --root "$CONTROL_ROOT" --runtime "$CURRENT_RUNTIME"
    --gdb-socket "$CURRENT_RUNTIME/gdb.sock"
    --elf "$CURRENT_CANDIDATE_ELF"
    --input-layout "$CURRENT_INPUT_LAYOUT"
    --record "$CURRENT_PROFILE/commands.jsonl"
    --transport-log "$transport" --profile-id "$(basename "$CURRENT_PROFILE")"
    --vm-id "$CURRENT_VM_ID" --candidate-sha256 "$CANDIDATE_SHA256"
    --sequence "$COMMAND_SEQUENCE" --payload "$payload" --timeout "$timeout"
    --allowed-status "$allowed" --input-profile "$CURRENT_INPUT_PROFILE")
  if [[ -n $modal ]]; then
    args+=(--modal-screenshot "$modal")
  fi
  local status=0
  python3 "$CONTROL_ROOT/scripts/foundation-qemu.py" "${args[@]}" \
    > "$log" 2>&1 || status=$?
  cat "$log"
  if ((status != 0)); then
    capture_gdb_failure_snapshot "$CURRENT_CANDIDATE_ELF" \
      "command-${COMMAND_SEQUENCE}-failure"
  fi
  record_command_tsv
  if ((status != 0)); then
    PROFILE_ABORTED=1
  fi
  return "$status"
}

run_rejected_frame() {
  local payload='inputtest marker transport-rejected'
  local sequence=1
  local record="$CURRENT_PROFILE/rejection.jsonl"
  local log="$CURRENT_PROFILE/rejection.json"
  local transport="$CURRENT_PROFILE/rejection-transport.log"
  local status=0
  python3 "$CONTROL_ROOT/scripts/foundation-qemu.py" reject-frame \
    --root "$CONTROL_ROOT" --runtime "$CURRENT_RUNTIME" \
    --gdb-socket "$CURRENT_RUNTIME/gdb.sock" \
    --elf "$CURRENT_CANDIDATE_ELF" --input-layout "$CURRENT_INPUT_LAYOUT" \
    --record "$record" \
    --transport-log "$transport" \
    --profile-id "$(basename "$CURRENT_PROFILE")" \
    --vm-id "$CURRENT_VM_ID" --candidate-sha256 "$CANDIDATE_SHA256" \
    --sequence "$sequence" --payload "$payload" --crc 00000000 \
    --marker-name transport-rejected --timeout 120 \
    --input-profile "$CURRENT_INPUT_PROFILE" > "$log" 2>&1 || status=$?
  cat "$log"
  ((status == 0)) || PROFILE_ABORTED=1
  return "$status"
}

extract_transaction_match() {
  local sequence=$1 pattern=$2
  python3 - "$CURRENT_RUNTIME/qemu-serial.log" \
    "$CURRENT_PROFILE/commands.jsonl" "$sequence" "$pattern" <<'PY'
import json,re,sys
serial,ledger,sequence,pattern=sys.argv[1:]
rows=[json.loads(line) for line in open(ledger,encoding='utf-8') if line.strip()]
row=next(item for item in rows if item['sequence']==int(sequence))
lines=open(serial,errors='replace').read().splitlines()
segment=lines[row['start_line_count']:row['end_line_count']]
matches=[]
for line in segment:
    match=re.fullmatch(pattern,line)
    if match: matches.append(match)
if len(matches)!=1: raise SystemExit(1)
print(matches[0].group(1))
PY
}

send_async() {
  local payload=$1 kind=$2 timeout=${3:-300}
  local launch_sequence=$((COMMAND_SEQUENCE + 1)) run
  run_frame "$payload" 120 0 || return 1
  run=$(extract_transaction_match "$launch_sequence" \
    "\\[SYNC\\]\[$kind\\] START run=([1-9][0-9]*) .*" ) || return 1
  run_frame "synctest async-wait $run $((timeout * 1000))" "$timeout" 0
}

host_metadata() {
  local schedulable online affinity
  schedulable=$(nproc)
  online=$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf 'unknown')
  affinity=$(sed -n 's/^Cpus_allowed_list:[[:space:]]*//p' /proc/self/status)
  {
    printf 'candidate_label=%s\n' "$CANDIDATE_LABEL"
    printf 'candidate_root=%s\n' "$CANDIDATE_ROOT"
    printf 'control_root=%s\n' "$CONTROL_ROOT"
    printf 'delivery_cwd=%s\n' "$DELIVERY_CWD"
    printf 'runtime_parent=%s\n' "$RUNTIME_PARENT"
    printf 'schedulable_cpus=%s\n' "$schedulable"
    printf 'online_cpus=%s\n' "$online"
    printf 'cpus_allowed_list=%s\n' "$affinity"
    printf 'qemu_version=%s\n' "$(qemu-system-x86_64 --version | head -1)"
    if [[ -r /dev/kvm && -w /dev/kvm ]]; then
      printf 'kvm=available\n'
    else
      printf 'kvm=unavailable\n'
    fi
  } > "$EVIDENCE/host.env"
}

build_input_layout() {
  local binary=$EVIDENCE/candidate/command-transport-layout-host
  local output=$CURRENT_INPUT_LAYOUT
  run_logged "$EVIDENCE/command-input-layout-build.log" \
    cc -std=c11 -Wall -Wextra -Werror -I"$CONTROL_ROOT" \
    "$CONTROL_ROOT/scripts/command-transport-layout.c" -o "$binary"
  run_logged "$EVIDENCE/command-input-layout-run.log" "$binary"
  tail -n 1 "$EVIDENCE/command-input-layout-run.log" > "$output"
  python3 - "$output" <<'PY'
import json,sys
value=json.load(open(sys.argv[1],encoding='utf-8'))
if value.get('schema') != 1 or value.get('slot_count') != 64 or \
        value.get('key_count') != 6:
    raise SystemExit('command input layout has unexpected dimensions')
PY
  sha256sum "$CONTROL_ROOT/scripts/command-transport-layout.c" \
    "$CONTROL_ROOT/kernel/src/drivers/usb/xhci/xhci.h" "$output" > \
    "$EVIDENCE/candidate/command-input-layout.sha256"
  cc --version | head -1 > "$EVIDENCE/candidate/command-input-layout-cc.txt"
  rm -f -- "$binary"
}

build_test_candidate() {
  host_metadata
  run_logged "$EVIDENCE/build-clean.log" make -C "$CANDIDATE_ROOT" clean
  run_logged "$EVIDENCE/build-selftest-image.log" make -C "$CANDIDATE_ROOT" \
    image "JOBS=$JOBS" SELFTEST=1 SELFTEST_AUTORUN=0 KERNEL_EXTRA_CFLAGS=
  cp "$CANDIDATE_ROOT/kernel.elf" "$CANDIDATE_ROOT/BOOTX64.EFI" \
    "$CANDIDATE_ROOT/hobbyos.img" "$EVIDENCE/candidate/"
  CANDIDATE_SHA256=$(sha256sum "$EVIDENCE/candidate/kernel.elf" |
    awk '{print $1}')
  sha256sum "$EVIDENCE/candidate/kernel.elf" \
    "$EVIDENCE/candidate/BOOTX64.EFI" \
    "$EVIDENCE/candidate/hobbyos.img" > "$EVIDENCE/candidate/SHA256SUMS"
  {
    printf 'SELFTEST=1\nSELFTEST_AUTORUN=0\nKERNEL_EXTRA_CFLAGS=\n'
    printf 'HOBBYOS_PANIC_TEST=ABSENT\n'
    printf 'HOBBYOS_PANIC_NEGATIVE_LOCK_WAIT=ABSENT\n'
  } > "$EVIDENCE/candidate/build-profile.env"
  git -C "$CANDIDATE_ROOT" rev-parse HEAD > "$EVIDENCE/candidate/commit.txt"
  git -C "$CANDIDATE_ROOT" rev-parse 'HEAD^{tree}' > "$EVIDENCE/candidate/tree.txt"
  git -C "$CANDIDATE_ROOT" status --short > "$EVIDENCE/candidate/status.txt"
  nm -n "$EVIDENCE/candidate/kernel.elf" > "$EVIDENCE/candidate/symbols.txt"
  if rg -q 'g_panic_test_state|panic_test_' "$EVIDENCE/candidate/symbols.txt"; then
    echo 'foundation-regression: panic injection symbol leaked into test candidate' >&2
    return 1
  fi
  rg 'shell_execute_command_line_for_selftest|tasktest_transport_crc32' \
    "$EVIDENCE/candidate/symbols.txt" > "$EVIDENCE/candidate/framed-symbols.txt"
  build_input_layout
}

adopt_test_candidate() {
  host_metadata
  for name in kernel.elf hobbyos.img; do
    [[ -f $EXISTING_CANDIDATE/$name ]] || {
      echo "foundation-regression: existing candidate lacks $name" >&2
      return 1
    }
    cp --reflink=auto "$EXISTING_CANDIDATE/$name" "$EVIDENCE/candidate/$name"
  done
  if [[ -f $EXISTING_CANDIDATE/BOOTX64.EFI ]]; then
    cp --reflink=auto "$EXISTING_CANDIDATE/BOOTX64.EFI" \
      "$EVIDENCE/candidate/BOOTX64.EFI"
  fi
  CANDIDATE_SHA256=$(sha256sum "$EVIDENCE/candidate/kernel.elf" |
    awk '{print $1}')
  sha256sum "$EVIDENCE/candidate/kernel.elf" \
    "$EVIDENCE/candidate/hobbyos.img" > "$EVIDENCE/candidate/SHA256SUMS"
  {
    printf 'source=existing-identified-candidate\n'
    printf 'source_path=%s\n' "$EXISTING_CANDIDATE"
    printf 'SELFTEST=1\nSELFTEST_AUTORUN=0\nKERNEL_EXTRA_CFLAGS=\n'
    printf 'HOBBYOS_PANIC_TEST=ABSENT\n'
    printf 'HOBBYOS_PANIC_NEGATIVE_LOCK_WAIT=ABSENT\n'
  } > "$EVIDENCE/candidate/build-profile.env"
  git -C "$CONTROL_ROOT" rev-parse HEAD > "$EVIDENCE/candidate/commit.txt"
  git -C "$CONTROL_ROOT" rev-parse 'HEAD^{tree}' > \
    "$EVIDENCE/candidate/tree.txt"
  git -C "$CONTROL_ROOT" status --short > "$EVIDENCE/candidate/status.txt"
  nm -n "$EVIDENCE/candidate/kernel.elf" > "$EVIDENCE/candidate/symbols.txt"
  if rg -q 'g_panic_test_state|panic_test_' \
      "$EVIDENCE/candidate/symbols.txt"; then
    echo 'foundation-regression: panic injection symbol leaked into test candidate' >&2
    return 1
  fi
  rg 'shell_execute_command_line_for_selftest|tasktest_transport_crc32' \
    "$EVIDENCE/candidate/symbols.txt" > \
    "$EVIDENCE/candidate/framed-symbols.txt"
  build_input_layout
}

build_production_candidate() {
  mkdir -p "$EVIDENCE/production/candidate" \
    "$EVIDENCE/production/profiles" "$EVIDENCE/production/commands"
  run_logged "$EVIDENCE/production/build-production-image.log" \
    make -C "$CANDIDATE_ROOT" production-image "JOBS=$JOBS"
  cp "$CANDIDATE_ROOT/kernel.elf" "$CANDIDATE_ROOT/BOOTX64.EFI" \
    "$CANDIDATE_ROOT/hobbyos.img" "$EVIDENCE/production/candidate/"
  CANDIDATE_SHA256=$(sha256sum \
    "$EVIDENCE/production/candidate/kernel.elf" | awk '{print $1}')
  sha256sum "$EVIDENCE/production/candidate/kernel.elf" \
    "$EVIDENCE/production/candidate/BOOTX64.EFI" \
    "$EVIDENCE/production/candidate/hobbyos.img" > \
    "$EVIDENCE/production/candidate/SHA256SUMS"
  {
    printf 'SELFTEST=0\nSELFTEST_AUTORUN=0\nKERNEL_EXTRA_CFLAGS=\n'
    printf 'HOBBYOS_PANIC_TEST=ABSENT\n'
    printf 'HOBBYOS_PANIC_NEGATIVE_LOCK_WAIT=ABSENT\n'
    printf 'HOBBYOS_REAPTEST_NEGATIVE_NO_TIMER_RELEASE=ABSENT\n'
  } > "$EVIDENCE/production/candidate/build-profile.env"
  nm -an "$EVIDENCE/production/candidate/kernel.elf" > \
    "$EVIDENCE/production/candidate/symbols.txt"
  objdump -drwC --disassemble=shell_dispatch_command_line \
    "$EVIDENCE/production/candidate/kernel.elf" > \
    "$EVIDENCE/production/candidate/shell-dispatch.disassembly.txt"
  if rg -q 'g_panic_test_state|panic_test_|shell_execute_command_line_for_selftest|tasktest_transport_crc32|timer_ref_no_release_negative' \
      "$EVIDENCE/production/candidate/symbols.txt"; then
    echo 'foundation-regression: test-only symbol leaked into production' >&2
    return 1
  fi
  rg 'shell_dispatch_command_line|cmd_irq|cmd_reaptest|cmd_accounttest' \
    "$EVIDENCE/production/candidate/symbols.txt" > \
    "$EVIDENCE/production/candidate/diagnostic-symbols.txt"
  cp artifacts/build/baremetal-boot-trace/production-test-policy.log \
    "$EVIDENCE/production/candidate/production-test-policy.log"
}

create_gdb_qemu_wrapper() {
  local wrapper=$CURRENT_RUNTIME/qemu-with-gdb
  local real_qemu gdb_socket=$CURRENT_RUNTIME/gdb.sock
  real_qemu=$(command -v qemu-system-x86_64)
  python3 - "$wrapper" "$real_qemu" "$gdb_socket" \
    "$CURRENT_RUNTIME/qemu-argv.txt" <<'PY'
import shlex,sys
wrapper,real_qemu,gdb_socket,argv_log=sys.argv[1:]
body=f'''#!/usr/bin/env bash
set -euo pipefail
if [[ ${{1:-}} == --version ]]; then
  exec {shlex.quote(real_qemu)} "$@"
fi
printf '%q ' {shlex.quote(real_qemu)} "$@" -chardev {shlex.quote('socket,path='+gdb_socket+',server=on,wait=off,id=foundation_gdb')} -gdb chardev:foundation_gdb > {shlex.quote(argv_log)}
printf '\\n' >> {shlex.quote(argv_log)}
exec {shlex.quote(real_qemu)} "$@" -chardev {shlex.quote('socket,path='+gdb_socket+',server=on,wait=off,id=foundation_gdb')} -gdb chardev:foundation_gdb
'''
open(wrapper,'w',encoding='utf-8').write(body)
PY
  chmod 700 "$wrapper"
  printf '%s\n' "$wrapper"
}

start_production_profile() {
  local name=$1 machine=$2 smp=$3 accel=$4 group=$5 wrapper
  CURRENT_PROFILE="$EVIDENCE/production/profiles/$name"
  CURRENT_RUNTIME=$(mktemp -d "/tmp/hobbyos-production-${name}.XXXXXX")
  chmod 700 "$CURRENT_RUNTIME"
  mkdir -p "$CURRENT_PROFILE"
  CURRENT_VM_ID="$(basename "$CURRENT_RUNTIME")"
  cp --reflink=auto "$EVIDENCE/production/candidate/hobbyos.img" \
    "$CURRENT_PROFILE/working.img"
  COMMAND_SEQUENCE=0
  PROFILE_ABORTED=0
  : > "$CURRENT_PROFILE/commands.jsonl"
  printf 'sequence\tpayload\thandler_status\tclassification\tstart_line_count\tend_line_count\tgdb_log\n' \
    > "$CURRENT_PROFILE/commands.tsv"
  local image_hash authority
  image_hash=$(sha256sum "$CURRENT_PROFILE/working.img" | awk '{print $1}')
  authority=$(rate_authority "$smp" "$accel")
  {
    printf 'schema=3\nname=%s\nvm_id=%s\nmachine=%s\nsmp=%s\naccel=%s\ngroup=%s\n' \
      "$name" "$CURRENT_VM_ID" "$machine" "$smp" "$accel" "$group"
    printf 'candidate_kind=production\nkernel_sha256=%s\nimage_sha256_before=%s\n' \
      "$CANDIDATE_SHA256" "$image_hash"
    printf 'host_schedulable_cpus=%s\nrate_authority=%s\ncleanup=PENDING\n' \
      "$(nproc)" "$authority"
  } > "$CURRENT_PROFILE/profile.env"
  CURRENT_STARTED=$(date +%s)
  wrapper=$(create_gdb_qemu_wrapper)
  if ! (cd "$CANDIDATE_ROOT" && QEMU="$wrapper" \
    QEMU_RUNTIME="$CURRENT_RUNTIME" \
    HOBBYOS_IMAGE="$CURRENT_PROFILE/working.img" \
    HOBBYOS_KERNEL="$EVIDENCE/production/candidate/kernel.elf" SMP="$smp" \
    MACHINE="$machine" ACCEL="$accel" MEM=2G \
    scripts/qemu-agent.sh start) | tee "$CURRENT_PROFILE/start.log"; then
    return 1
  fi
  CURRENT_PID=$(cat "$CURRENT_RUNTIME/qemu.pid")
  for _ in {1..100}; do
    [[ -S $CURRENT_RUNTIME/gdb.sock ]] && break
    sleep .1
  done
  [[ -S $CURRENT_RUNTIME/gdb.sock ]] || return 1
  python3 "$CONTROL_ROOT/scripts/foundation-qemu.py" wait-ready \
    --runtime "$CURRENT_RUNTIME" \
    --marker "[BOOT][RUNTIME_READY] PASS cpus=$smp/$smp" --timeout 240 \
    --output "$CURRENT_PROFILE/runtime-ready.json" || return 1
}

record_production_tsv() {
  python3 - "$CURRENT_PROFILE/commands.jsonl" \
    "$CURRENT_PROFILE/commands.tsv" <<'PY'
import json,sys
row=json.loads(open(sys.argv[1],encoding='utf-8').read().splitlines()[-1])
values=(row.get('sequence'),row.get('payload'),row.get('handler_status'),
        row.get('classification'),row.get('start_line_count'),
        row.get('end_line_count'),row.get('gdb_log'))
with open(sys.argv[2],'a',encoding='utf-8') as stream:
    stream.write('\t'.join('' if value is None else str(value)
                           for value in values)+'\n')
PY
}

run_production_command() {
  local payload=$1 timeout=${2:-240} allowed=${3:-0} modal=${4:-}
  COMMAND_SEQUENCE=$((COMMAND_SEQUENCE + 1))
  local args=(production-command --root "$CONTROL_ROOT"
    --runtime "$CURRENT_RUNTIME" --gdb-socket "$CURRENT_RUNTIME/gdb.sock"
    --elf "$EVIDENCE/production/candidate/kernel.elf"
    --record "$CURRENT_PROFILE/commands.jsonl"
    --gdb-log "$CURRENT_PROFILE/gdb-${COMMAND_SEQUENCE}.log"
    --transport-log "$CURRENT_PROFILE/transport-${COMMAND_SEQUENCE}.log"
    --profile-id "$(basename "$CURRENT_PROFILE")" --vm-id "$CURRENT_VM_ID"
    --candidate-sha256 "$CANDIDATE_SHA256" --sequence "$COMMAND_SEQUENCE"
    --payload "$payload" --timeout "$timeout" --allowed-status "$allowed")
  [[ -z $modal ]] || args+=(--modal-screenshot "$modal")
  local status=0
  python3 "$CONTROL_ROOT/scripts/foundation-qemu.py" "${args[@]}" \
    > "$CURRENT_PROFILE/command-${COMMAND_SEQUENCE}.json" 2>&1 || status=$?
  cat "$CURRENT_PROFILE/command-${COMMAND_SEQUENCE}.json"
  if ((status != 0)); then
    capture_gdb_failure_snapshot \
      "$EVIDENCE/production/candidate/kernel.elf" \
      "command-${COMMAND_SEQUENCE}-failure"
  fi
  record_production_tsv
  ((status == 0)) || PROFILE_ABORTED=1
  return "$status"
}

rate_authority() {
  local smp=$1 accel=$2 schedulable
  schedulable=$(nproc)
  if [[ $accel != kvm ]]; then
    printf 'NOT_APPLICABLE'
  elif ((schedulable < smp)); then
    printf 'NONAUTHORITATIVE_OVERSUBSCRIBED'
  elif ((smp == 4)); then
    printf 'AUTHORITATIVE_CONTROL'
  else
    printf 'AUTHORITATIVE'
  fi
}

start_profile() {
  local name=$1 machine=$2 smp=$3 accel=$4 group=$5
  local profile_parent=${6:-$EVIDENCE/profiles}
  local candidate_image=${7:-$EVIDENCE/candidate/hobbyos.img}
  local candidate_elf=${8:-$EVIDENCE/candidate/kernel.elf}
  local wrapper
  if [[ $accel == kvm && (! -r /dev/kvm || ! -w /dev/kvm) ]]; then
    echo "foundation-regression: KVM_UNAVAILABLE profile=$name"
    return 1
  fi
  CURRENT_PROFILE="$profile_parent/$name"
  mkdir -p "$RUNTIME_PARENT"
  CURRENT_RUNTIME=$(mktemp -d \
    "$RUNTIME_PARENT/hobbyos-foundation-${name}.XXXXXX")
  chmod 700 "$CURRENT_RUNTIME"
  mkdir -p "$CURRENT_PROFILE"
  CURRENT_VM_ID="$(basename "$CURRENT_RUNTIME")"
  CURRENT_CANDIDATE_ELF=$candidate_elf
  CURRENT_INPUT_PROFILE=framed
  cp "$CURRENT_INPUT_LAYOUT" "$CURRENT_PROFILE/command-input-layout.json"
  printf '%s\n' "$CURRENT_RUNTIME" > "$CURRENT_PROFILE/runtime-path.txt"
  cp --reflink=auto "$candidate_image" \
    "$CURRENT_PROFILE/working.img"
  COMMAND_SEQUENCE=0
  PROFILE_ABORTED=0
  : > "$CURRENT_PROFILE/commands.jsonl"
  printf 'sequence\tpayload\tcrc32\tdisposition\ttransport\thandler_status\tclassification\tstart_line_count\tend_line_count\taccepted_line\tbegin_line\tend_line\n' \
    > "$CURRENT_PROFILE/commands.tsv"
  local image_hash authority
  image_hash=$(sha256sum "$CURRENT_PROFILE/working.img" | awk '{print $1}')
  authority=$(rate_authority "$smp" "$accel")
  {
    printf 'schema=2\nname=%s\nvm_id=%s\nmachine=%s\nsmp=%s\naccel=%s\ngroup=%s\n' \
      "$name" "$CURRENT_VM_ID" "$machine" "$smp" "$accel" "$group"
    printf 'candidate_label=%s\nkernel_sha256=%s\nimage_sha256_before=%s\n' \
      "$CANDIDATE_LABEL" "$CANDIDATE_SHA256" "$image_hash"
    printf 'host_schedulable_cpus=%s\nrate_authority=%s\ncleanup=PENDING\n' \
      "$(nproc)" "$authority"
    printf 'input_profile=%s\n' "$CURRENT_INPUT_PROFILE"
    printf 'command_input_observation=qmp-stable-physical-atomic-stroke-confirmed-bounded-stopped-detach-observer-gdb\n'
  } > "$CURRENT_PROFILE/profile.env"
  CURRENT_STARTED=$(date +%s)
  wrapper=$(create_gdb_qemu_wrapper)
  if ! (cd "$CANDIDATE_ROOT" && QEMU="$wrapper" \
    QEMU_RUNTIME="$CURRENT_RUNTIME" \
    HOBBYOS_IMAGE="$CURRENT_PROFILE/working.img" \
    HOBBYOS_KERNEL="$candidate_elf" SMP="$smp" \
    MACHINE="$machine" ACCEL="$accel" MEM=2G \
    scripts/qemu-agent.sh start) | tee "$CURRENT_PROFILE/start.log"; then
    echo "foundation-regression: VM start failed profile=$name"
    return 1
  fi
  CURRENT_PID=$(cat "$CURRENT_RUNTIME/qemu.pid")
  python3 "$CONTROL_ROOT/scripts/foundation-qemu.py" wait-ready \
    --runtime "$CURRENT_RUNTIME" \
    --marker "[BOOT][RUNTIME_READY] PASS cpus=$smp/$smp" --timeout 240 \
    --output "$CURRENT_PROFILE/runtime-ready.json" || return 1
  python3 "$CONTROL_ROOT/scripts/foundation-qemu.py" wait-ready \
    --runtime "$CURRENT_RUNTIME" \
    --marker "[BOOT][TEST_READY] PASS autorun=0 selftests=0" --timeout 30 \
    --output "$CURRENT_PROFILE/test-ready.json" || return 1
  echo "foundation-regression: runtime ready profile=$name pid=$CURRENT_PID"
}

stop_profile() {
  local elapsed image_after finished_profile finished_runtime pid cleanup=PASS
  finished_profile=$CURRENT_PROFILE
  finished_runtime=$CURRENT_RUNTIME
  pid=$CURRENT_PID
  if ! qemu_agent status > "$CURRENT_PROFILE/status-before-stop.log" 2>&1; then
    cleanup=FAIL
  fi
  if ! qemu_agent stop > "$CURRENT_PROFILE/stop.log" 2>&1; then
    cleanup=FAIL
  fi
  if pid_active "$pid" || [[ -e $CURRENT_RUNTIME/qemu.pid ||
                             -S $CURRENT_RUNTIME/hmp.sock ]]; then
    cleanup=FAIL
  fi
  elapsed=$(($(date +%s) - CURRENT_STARTED))
  image_after=$(sha256sum "$CURRENT_PROFILE/working.img" | awk '{print $1}')
  append_profile host_elapsed_seconds "$elapsed"
  append_profile image_sha256_after "$image_after"
  append_profile cleanup "$cleanup"
  append_profile run_complete YES
  archive_runtime "$finished_runtime" "$finished_profile/qemu"
  CURRENT_RUNTIME=
  CURRENT_PROFILE=
  CURRENT_PID=
  CURRENT_VM_ID=
  [[ $cleanup == PASS ]]
}

basic_commands() {
  run_frame 'irq check' 180 0 || return 1
  run_frame 'irq controllers' 180 0 || return 1
  run_frame 'irq routes' 180 0 || return 1
  run_frame 'irq boot' 240 0 || return 1
  run_frame 'accounttest lapic-config' 180 0 || return 1
  run_frame 'accounttest lapic-liveness 500' 240 0
}

capture_rate() {
  local sequence=$((COMMAND_SEQUENCE + 1)) status raw
  run_frame 'accounttest lapic-rate 2000 5' 360 '0,1' || return 1
  status=$(last_command_field handler_status)
  raw=$(extract_transaction_match "$sequence" \
    '\[ACCOUNT\]\[LAPIC_RATE\] (PASS|FAIL) window_ms=2000 rounds=5 .*') || return 1
  if [[ ($status == 0 && $raw != PASS) || ($status == 1 && $raw != FAIL) ]]; then
    echo 'foundation-regression: rate status and structured result disagree' >&2
    return 1
  fi
  LAST_RATE_RAW=$raw
  append_profile rate_raw "$raw"
}

taskman_smoke() {
  run_frame 'clear' 180 0 || return 1
  run_frame 'taskman 1000' 240 0 "$CURRENT_PROFILE/taskman-screendump.ppm" ||
    return 1
  sha256sum "$CURRENT_PROFILE/taskman-screendump.ppm" > \
    "$CURRENT_PROFILE/taskman-screendump.sha256"
  run_frame 'taskmantest stats' 180 0
}

full_commands() {
  local authority
  run_frame 'accounttest check' 240 0 || return 1
  capture_rate || return 1
  authority=$(sed -n 's/^rate_authority=//p' "$CURRENT_PROFILE/profile.env" |
    tail -1)
  if [[ $authority == AUTHORITATIVE_CONTROL || $authority == AUTHORITATIVE ]]; then
    [[ $LAST_RATE_RAW == PASS ]] || return 1
  fi
  send_async 'synctest sleep' SLEEP 300 || return 1
  run_frame 'synctest timer-order' 240 0 || return 1
  send_async 'synctest timer-cancel' TIMER_CANCEL 240 || return 1
  run_frame 'synctest timer-backlog' 240 0 || return 1
  local repetition
  for repetition in 1 2 3; do
    run_frame 'reaptest timer-ref' 240 0 || return 1
    run_frame 'reaptest timer-ref-competing' 240 0 || return 1
  done
  run_frame 'synctest check' 240 0 || return 1
  run_frame 'taskdiag check' 240 0 || return 1
  run_frame 'inputtest check' 240 0 || return 1
  run_frame 'modaltest check' 240 0 || return 1
  run_frame 'taskmantest check' 240 0 || return 1
  taskman_smoke
}

soak_runtime() {
  local started now elapsed checkpoint status first_sequence last_sequence
  started=$(date +%s)
  printf 'checkpoint\thost_epoch\thost_elapsed_seconds\tfirst_sequence\tlast_sequence\tstatus\n' \
    > "$CURRENT_PROFILE/soak-checkpoints.tsv"
  for checkpoint in 1 2 3 4 5; do
    status=PASS
    echo "foundation-regression: soak wait checkpoint=$checkpoint/5"
    sleep 60
    first_sequence=$((COMMAND_SEQUENCE + 1))
    run_frame 'accounttest lapic-liveness 500' 240 0 || status=FAIL
    if [[ $status == PASS ]]; then
      run_frame 'irq check' 240 0 || status=FAIL
    fi
    if [[ $status == PASS ]]; then
      run_frame 'synctest check' 240 0 || status=FAIL
    fi
    last_sequence=$COMMAND_SEQUENCE
    now=$(date +%s)
    elapsed=$((now - started))
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$checkpoint" "$now" "$elapsed" \
      "$first_sequence" "$last_sequence" "$status" >> \
      "$CURRENT_PROFILE/soak-checkpoints.tsv"
    [[ $status == PASS ]] || {
      append_profile soak_elapsed_seconds "$elapsed"
      return 1
    }
  done
  elapsed=$(($(date +%s) - started))
  append_profile soak_elapsed_seconds "$elapsed"
  ((elapsed >= 300))
}

run_profile() {
  local name=$1 machine=$2 smp=$3 accel=$4 group=$5 failed=0
  if ! start_profile "$name" "$machine" "$smp" "$accel" "$group"; then
    cleanup_active_vm start-failure
    return 1
  fi
  if ! basic_commands; then
    failed=1
    PROFILE_ABORTED=1
  fi
  if ((PROFILE_ABORTED == 0)) && [[ $group == full || $group == soak ]]; then
    if ! full_commands; then
      failed=1
      PROFILE_ABORTED=1
    fi
  fi
  if ((PROFILE_ABORTED == 0)) && [[ $group == soak ]]; then
    if ! soak_runtime; then
      failed=1
      PROFILE_ABORTED=1
    fi
  fi
  stop_profile || failed=1
  ((failed == 0))
}

run_focal_profile() {
  local name=$1 smp=$2 failed=0
  if ! start_profile "$name" q35 "$smp" kvm focal; then
    cleanup_active_vm start-failure
    return 1
  fi
  if ((smp == 4)); then
    if ! capture_rate; then
      failed=1
      PROFILE_ABORTED=1
    fi
    if ((PROFILE_ABORTED == 0)); then
      run_frame 'reaptest timer-ref' 240 '0,1' || failed=1
    fi
  elif ((smp == 8)); then
    run_frame 'irq boot' 240 '0,1' || failed=1
  elif ((smp == 24)); then
    run_frame 'irq boot' 240 '0,1' || failed=1
    if ((PROFILE_ABORTED == 0)); then
      run_frame 'reaptest timer-ref' 240 '0,1' || failed=1
    fi
    if ((PROFILE_ABORTED == 0)); then
      capture_rate || failed=1
    fi
  fi
  stop_profile || failed=1
  ((failed == 0))
}

write_rate_summary() {
  python3 - "$EVIDENCE" <<'PY'
import json,sys
from pathlib import Path
root=Path(sys.argv[1]); output=[]
for name in ('q35-kvm-smp4','q35-kvm-smp24'):
    profile={}
    for line in (root/'profiles'/name/'profile.env').read_text().splitlines():
        if '=' in line: profile[line.split('=',1)[0]]=line.split('=',1)[1]
    rows=[json.loads(line) for line in
          (root/'profiles'/name/'commands.jsonl').read_text().splitlines()
          if line.strip()]
    rate=[row for row in rows if row.get('payload')=='accounttest lapic-rate 2000 5']
    raw='NOT_OBSERVED'
    if len(rate)==1 and rate[0].get('handler_status') in (0,1):
        raw='PASS' if rate[0]['handler_status']==0 else 'FAIL'
    output.append((name,raw,profile.get('rate_authority','UNKNOWN'),
                   profile.get('host_schedulable_cpus','0'),profile.get('smp','0')))
path=root/'rate-summary.tsv'
with path.open('w') as stream:
    stream.write('profile\traw_status\tauthority\thost_schedulable\tguest_vcpus\n')
    for row in output: stream.write('\t'.join(row)+'\n')
PY
}

static_checks() {
  bash -n "$CONTROL_ROOT/scripts/test-foundation-regression.sh"
  python3 -m py_compile "$CONTROL_ROOT/scripts/foundation-qemu.py" \
    "$CONTROL_ROOT/scripts/verify-foundation-regression.py"
  PYTHONDONTWRITEBYTECODE=1 python3 \
    "$CONTROL_ROOT/scripts/test-command-transport.py" all
  python3 - "$CONTROL_ROOT/kernel/src/core/clock.c" <<'PY'
import pathlib
import re
import sys

text = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")

def function_body(signature, next_signature):
    match = re.search(signature, text)
    if match is None:
        raise SystemExit(f"clock source is missing {signature}")
    end = re.search(next_signature, text[match.end():])
    if end is None:
        raise SystemExit(f"clock source is missing boundary {next_signature}")
    return text[match.start():match.end() + end.start()]

hot = function_body(r"uint64_t clock_monotonic_ns\(void\)",
                    r"uint64_t clock_monotonic_us\(void\)")
for required in ("irq_save()", "irq_restore(flags)",
                 "hpet_read_clock_sample", "clock_publish_max"):
    if required not in hot:
        raise SystemExit(f"clock hot path is missing {required}")
for forbidden in ("spin_lock(", "spin_lock_irqsave(", "spin_cpu_relax(",
                  "g_clock_lock"):
    if forbidden in hot:
        raise SystemExit(f"clock hot path regained blocking state: {forbidden}")

publisher = function_body(
    r"static uint64_t clock_publish_max\(uint64_t \*value, uint64_t candidate\)",
    r"static bool clock_publication_selftest\(void\)")
if "__atomic_compare_exchange_n" not in publisher:
    raise SystemExit("clock publication is no longer an atomic maximum")
if any(token in publisher for token in ("spin_lock(", "spin_cpu_relax(")):
    raise SystemExit("clock publication regained an owner or wait loop")

events = function_body(r"static void record_event\(",
                       r"bool clock_monotonic_init\(void\)")
if "spin_trylock(&g_clock_event_lock)" not in events:
    raise SystemExit("clock anomaly logging is no longer non-blocking")
PY
  "$CONTROL_ROOT/scripts/verify-foundation-regression.py" fixtures \
    --output-dir "$EVIDENCE/oracle-fixtures/regression" |
    tee "$EVIDENCE/verifier-negative-fixtures.log"
}

diagnostic_negative_case() {
  local root="$EVIDENCE/diagnostic-negative" failed=0
  local saved_label=$CANDIDATE_LABEL
  if [[ ! -r /dev/kvm || ! -w /dev/kvm ]]; then
    echo 'foundation-regression: KVM_UNAVAILABLE diagnostic negative not executed'
    return 1
  fi
  mkdir -p "$root/candidate" "$root/profiles" "$root/commands"
  if [[ ! -s $CURRENT_INPUT_LAYOUT ]]; then
    build_input_layout
  fi
  run_logged "$root/build-clean.log" make -C "$CANDIDATE_ROOT" clean
  run_logged "$root/build-negative-image.log" make -C "$CANDIDATE_ROOT" \
    image "JOBS=$JOBS" SELFTEST=1 SELFTEST_AUTORUN=0 \
    KERNEL_EXTRA_CFLAGS=-DHOBBYOS_REAPTEST_NEGATIVE_NO_TIMER_RELEASE
  cp "$CANDIDATE_ROOT/kernel.elf" "$CANDIDATE_ROOT/BOOTX64.EFI" \
    "$CANDIDATE_ROOT/hobbyos.img" "$root/candidate/"
  CANDIDATE_SHA256=$(sha256sum "$root/candidate/kernel.elf" | awk '{print $1}')
  CANDIDATE_LABEL=timer-reference-no-release
  sha256sum "$root/candidate/kernel.elf" "$root/candidate/BOOTX64.EFI" \
    "$root/candidate/hobbyos.img" > "$root/candidate/SHA256SUMS"
  nm -an "$root/candidate/kernel.elf" > "$root/candidate/symbols.txt"
  rg '[[:space:]]timer_ref_no_release_negative$' \
    "$root/candidate/symbols.txt" > "$root/candidate/negative-symbol.txt"
  if rg -q 'g_panic_test_state|panic_test_.*ud2' \
      "$root/candidate/symbols.txt"; then
    echo 'foundation-regression: panic injection leaked into diagnostic negative' >&2
    return 1
  fi
  {
    printf 'SELFTEST=1\nSELFTEST_AUTORUN=0\n'
    printf 'KERNEL_EXTRA_CFLAGS=-DHOBBYOS_REAPTEST_NEGATIVE_NO_TIMER_RELEASE\n'
    printf 'expected_handler_status=1\n'
  } > "$root/candidate/build-profile.env"
  if ! start_profile timer-ref-no-release-kvm-smp4 q35 4 kvm negative \
      "$root/profiles" "$root/candidate/hobbyos.img" \
      "$root/candidate/kernel.elf"; then
    cleanup_active_vm diagnostic-negative-start-failure
    return 1
  fi
  run_frame 'reaptest timer-ref-no-release-negative' 240 1 || failed=1
  stop_profile || failed=1
  "$CONTROL_ROOT/scripts/verify-foundation-regression.py" \
    diagnostic-negative "$root" | tee "$root/verification-summary.log" || failed=1
  CANDIDATE_LABEL=$saved_label
  ((failed == 0))
}

production_basic_commands() {
  local full=${1:-0}
  run_production_command 'irq check' 180 0 || return 1
  if ((full)); then
    run_production_command 'irq controllers' 180 0 || return 1
    run_production_command 'irq routes' 180 0 || return 1
  fi
  run_production_command 'irq boot' 240 0 || return 1
  if ((full)); then
    run_production_command 'accounttest lapic-config' 180 0 || return 1
  fi
  run_production_command 'accounttest lapic-liveness 500' 240 0
}

production_rate() {
  local allowed=${1:-0} sequence=$((COMMAND_SEQUENCE + 1)) status raw
  run_production_command 'accounttest lapic-rate 2000 5' 360 "$allowed" ||
    return 1
  status=$(last_command_field handler_status)
  raw=$(extract_transaction_match "$sequence" \
    '\[ACCOUNT\]\[LAPIC_RATE\] (PASS|FAIL) window_ms=2000 rounds=5 .*') ||
    return 1
  [[ ($status == 0 && $raw == PASS) || ($status == 1 && $raw == FAIL) ]] ||
    return 1
  append_profile rate_raw "$raw"
  LAST_RATE_RAW=$raw
}

production_consumers() {
  run_production_command 'accounttest check' 240 0 || return 1
  run_production_command 'taskdiag check' 240 0 || return 1
  run_production_command 'inputtest check' 240 0 || return 1
  run_production_command 'modaltest check' 240 0 || return 1
  run_production_command 'taskmantest check' 240 0 || return 1
  run_production_command 'clear' 180 0 || return 1
  run_production_command 'taskman 1000' 240 0 \
    "$CURRENT_PROFILE/taskman-screendump.ppm" || return 1
  sha256sum "$CURRENT_PROFILE/taskman-screendump.ppm" > \
    "$CURRENT_PROFILE/taskman-screendump.sha256"
  run_production_command 'taskmantest stats' 180 0
}

run_production_profile() {
  local name=$1 machine=$2 smp=$3 accel=$4 group=$5 failed=0
  start_production_profile "$name" "$machine" "$smp" "$accel" "$group" || {
    cleanup_active_vm production-start-failure
    return 1
  }
  case $group in
    smoke)
      if ! production_basic_commands 0; then
        failed=1
        PROFILE_ABORTED=1
      fi
      ;;
    diagnostics)
      if ! production_basic_commands 0; then
        failed=1
        PROFILE_ABORTED=1
      fi
      if ((PROFILE_ABORTED == 0)); then
        if ! run_production_command 'reaptest timer-ref' 240 0; then
          failed=1
          PROFILE_ABORTED=1
        fi
      fi
      if ((PROFILE_ABORTED == 0)); then
        if ! run_production_command 'reaptest timer-ref-competing' 240 0; then
          failed=1
          PROFILE_ABORTED=1
        fi
      fi
      if ((PROFILE_ABORTED == 0 && smp == 24)); then
        if ! production_rate '0,1'; then
          failed=1
          PROFILE_ABORTED=1
        fi
      fi
      ;;
    full)
      if ! production_basic_commands 1; then
        failed=1
        PROFILE_ABORTED=1
      fi
      if ((PROFILE_ABORTED == 0)); then
        if ! production_rate 0; then
          failed=1
          PROFILE_ABORTED=1
        fi
      fi
      if ((PROFILE_ABORTED == 0)); then
        if ! run_production_command 'reaptest timer-ref' 240 0; then
          failed=1
          PROFILE_ABORTED=1
        fi
      fi
      if ((PROFILE_ABORTED == 0)); then
        if ! run_production_command 'reaptest timer-ref-competing' 240 0; then
          failed=1
          PROFILE_ABORTED=1
        fi
      fi
      if ((PROFILE_ABORTED == 0)); then
        if ! production_consumers; then
          failed=1
          PROFILE_ABORTED=1
        fi
      fi
      ;;
  esac
  stop_profile || failed=1
  ((failed == 0))
}

production_profiles() {
  local failed=0
  build_production_candidate || return 1
  run_production_profile q35-tcg-smp1 q35 1 tcg smoke || failed=1
  run_production_profile q35-kvm-smp4 q35 4 kvm full || failed=1
  run_production_profile q35-kvm-smp8 q35 8 kvm diagnostics || failed=1
  run_production_profile q35-kvm-smp24 q35 24 kvm diagnostics || failed=1
  if ! "$CONTROL_ROOT/scripts/verify-foundation-regression.py" production \
      "$EVIDENCE/production" | tee \
      "$EVIDENCE/production/verification-summary.log"; then
    failed=1
  fi
  ((failed == 0))
}

all_profiles() {
  local failed=0
  build_test_candidate
  static_checks
  [[ -r /dev/kvm && -w /dev/kvm ]] || {
    echo 'foundation-regression: KVM_UNAVAILABLE mandatory profiles not executed'
    return 1
  }
  run_profile q35-tcg-smp1 q35 1 tcg basic || failed=1
  run_profile q35-tcg-smp2 q35 2 tcg basic || failed=1
  run_profile q35-kvm-smp4 q35 4 kvm full || failed=1
  run_profile q35-kvm-smp8 q35 8 kvm basic || failed=1
  run_profile pc-tcg-smp4 pc 4 tcg basic || failed=1
  run_profile q35-kvm-smp24 q35 24 kvm soak || failed=1
  write_rate_summary
  if ! "$CONTROL_ROOT/scripts/verify-foundation-regression.py" all \
      "$EVIDENCE" | tee "$EVIDENCE/verification-summary.log"; then
    failed=1
  fi
  diagnostic_negative_case || failed=1
  production_profiles || failed=1
  git -C "$CONTROL_ROOT" status --short > "$EVIDENCE/status-after-runtime.txt"
  if ((failed)); then
    echo "foundation-regression: FAIL evidence=$EVIDENCE"
    return 1
  fi
  echo "foundation-regression: PASS evidence=$EVIDENCE"
}

existing_all_profiles() {
  local failed=0
  adopt_test_candidate
  static_checks
  [[ -r /dev/kvm && -w /dev/kvm ]] || {
    echo 'foundation-regression: KVM_UNAVAILABLE mandatory profiles not executed'
    return 1
  }
  run_profile q35-tcg-smp1 q35 1 tcg basic || failed=1
  run_profile q35-tcg-smp2 q35 2 tcg basic || failed=1
  run_profile q35-kvm-smp4 q35 4 kvm full || failed=1
  run_profile q35-kvm-smp8 q35 8 kvm basic || failed=1
  run_profile pc-tcg-smp4 pc 4 tcg basic || failed=1
  run_profile q35-kvm-smp24 q35 24 kvm soak || failed=1
  write_rate_summary
  "$CONTROL_ROOT/scripts/verify-foundation-regression.py" all "$EVIDENCE" |
    tee "$EVIDENCE/verification-summary.log" || failed=1
  git -C "$CONTROL_ROOT" status --short > "$EVIDENCE/status-after-runtime.txt"
  ((failed == 0))
}

existing_transport_profiles() {
  local failed=0 run root profile
  adopt_test_candidate
  for run in 1 2; do
    root="$EVIDENCE/transport-controls/run-$run"
    profile="$root/profiles/q35-kvm-smp24"
    mkdir -p "$root/candidate" "$root/profiles"
    cp --reflink=auto "$EVIDENCE/candidate/kernel.elf" \
      "$root/candidate/kernel.elf"
    cp --reflink=auto "$EVIDENCE/candidate/hobbyos.img" \
      "$root/candidate/hobbyos.img"
    if ! start_profile q35-kvm-smp24 q35 24 kvm transport \
        "$root/profiles" "$root/candidate/hobbyos.img" \
        "$root/candidate/kernel.elf"; then
      cleanup_active_vm start-failure
      return 1
    fi
    if ! basic_commands; then
      failed=1
      PROFILE_ABORTED=1
    fi
    stop_profile || failed=1
    "$CONTROL_ROOT/scripts/verify-foundation-regression.py" \
      transport-profile "$root" "$profile" | tee \
      "$root/verification-summary.log" || failed=1
    ((failed == 0)) || break
  done
  ((failed == 0))
}

existing_transport_rejection() {
  local failed=0 root profile
  adopt_test_candidate
  root="$EVIDENCE/transport-rejection"
  profile="$root/profiles/q35-kvm-smp24"
  mkdir -p "$root/candidate" "$root/profiles"
  cp --reflink=auto "$EVIDENCE/candidate/kernel.elf" \
    "$root/candidate/kernel.elf"
  cp --reflink=auto "$EVIDENCE/candidate/hobbyos.img" \
    "$root/candidate/hobbyos.img"
  if ! start_profile q35-kvm-smp24 q35 24 kvm transport \
      "$root/profiles" "$root/candidate/hobbyos.img" \
      "$root/candidate/kernel.elf"; then
    cleanup_active_vm start-failure
    return 1
  fi
  run_rejected_frame || failed=1
  stop_profile || failed=1
  "$CONTROL_ROOT/scripts/verify-foundation-regression.py" \
    transport-rejection "$profile" "$profile/rejection.jsonl" \
    "$profile/qemu/qemu-serial.log" | tee \
    "$root/verification-summary.log" || failed=1
  ((failed == 0))
}

focal_comparison() {
  local failed=0
  build_test_candidate
  static_checks
  run_focal_profile q35-kvm-smp4 4 || failed=1
  run_focal_profile q35-kvm-smp8 8 || failed=1
  run_focal_profile q35-kvm-smp24 24 || failed=1
  "$CONTROL_ROOT/scripts/verify-foundation-regression.py" focal "$EVIDENCE" |
    tee "$EVIDENCE/verification-summary.log" || failed=1
  ((failed == 0))
}

case "$RUN_MODE" in
  static) static_checks ;;
  all) all_profiles ;;
  existing-all) existing_all_profiles ;;
  existing-transport) existing_transport_profiles ;;
  existing-rejection) existing_transport_rejection ;;
  focal) focal_comparison ;;
  production) host_metadata; static_checks; production_profiles ;;
  diagnostic-negative) host_metadata; static_checks; diagnostic_negative_case ;;
  *)
    echo "usage: $0 {static|all|production|diagnostic-negative|focal <candidate-root> <candidate-label>|existing-all <candidate-dir> <candidate-label>|existing-transport <candidate-dir> <candidate-label>|existing-rejection <candidate-dir> <candidate-label>}" >&2
    exit 2
    ;;
esac
