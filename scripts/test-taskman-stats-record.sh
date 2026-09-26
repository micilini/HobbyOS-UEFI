#!/usr/bin/env bash
set -Eeuo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

timestamp=$(date '+%Y%m%d-%H%M%S-%z')
evidence=${STATS_EVIDENCE_DIR:-"artifacts/build/diagnostic-records/$timestamp"}
base_commit=${STATS_BASE_COMMIT:-$(git rev-parse HEAD)}
jobs=${JOBS:-$(nproc)}
host_cc=${CC:-gcc}
historical_root=artifacts/build/section-layout/20260906-014348-0300/regression/foundation
qemu_owned=0
qemu_runtime=
qemu_profile=
qemu_pid=
lock_held=0
input_layout="$evidence/candidate/command-input-layout.json"

usage()
{
    echo "usage: $0 preflight|static|host|historical|build|qemu|fixtures|verify|all" >&2
    exit 2
}

ensure_evidence()
{
    [[ ! -L $evidence ]] || {
        echo "stats evidence root must not be a symlink" >&2
        return 1
    }
    mkdir -p "$evidence" "$evidence/commands"
}

record_command()
{
    local label=$1
    shift
    ensure_evidence
    {
        printf '%s' "$label"
        printf '\t%q' "$@"
        printf '\n'
    } >>"$evidence/commands/commands.tsv"
}

run_logged()
{
    local label=$1 log=$2
    shift 2
    local status
    record_command "$label" "$@"
    set +e
    "$@" >"$log" 2>&1
    status=$?
    set -e
    printf '%s\n' "$status" >"${log%.log}.exit-code.txt"
    if ((status != 0)); then
        cat "$log" >&2
        return "$status"
    fi
}

remove_private_runtime()
{
    local directory=$1
    [[ -n $directory && $directory == /tmp/hobbyos-stats-* ]] || return 1
    python3 - "$directory" <<'PY'
from pathlib import Path
import shutil
import sys
path = Path(sys.argv[1])
if path.exists():
    shutil.rmtree(path)
PY
}

cleanup()
{
    local status=0
    if ((qemu_owned)); then
        set +e
        QEMU_RUNTIME="$qemu_runtime" scripts/qemu-agent.sh stop \
            >"$qemu_profile/emergency-stop.log" 2>&1
        local stop_status=$?
        set -e
        ((stop_status == 0)) || status=$stop_status
        qemu_owned=0
    fi
    if [[ -n $qemu_pid ]] && kill -0 "$qemu_pid" 2>/dev/null; then
        status=1
    fi
    if [[ -n $qemu_runtime && -d $qemu_runtime ]]; then
        if [[ -n $qemu_profile ]]; then
            mkdir -p "$qemu_profile/qemu-emergency"
            cp -a "$qemu_runtime/." "$qemu_profile/qemu-emergency/"
        fi
        remove_private_runtime "$qemu_runtime" || status=1
    fi
    qemu_runtime=
    qemu_profile=
    qemu_pid=
    if ((lock_held)); then
        exec 9>&-
        lock_held=0
    fi
    return "$status"
}
trap cleanup EXIT

preflight()
{
    ensure_evidence
    mkdir -p "$evidence/preflight"
    for tool in gcc make python3 qemu-system-x86_64 mcopy sha256sum git; do
        command -v "$tool" >/dev/null
    done
    {
        printf 'schema=1\n'
        printf 'observed_at=%s\n' "$(date --iso-8601=seconds)"
        printf 'delivery_dir=%s\n' "$root"
        printf 'repo_root=%s\n' "$(git rev-parse --show-toplevel)"
        printf 'branch=%s\n' "$(git branch --show-current)"
        printf 'head=%s\n' "$(git rev-parse HEAD)"
        printf 'tree=%s\n' "$(git rev-parse HEAD^{tree})"
        printf 'parent=%s\n' "$(git rev-parse HEAD^)"
        printf 'implementation_base_commit=%s\n' "$base_commit"
        printf 'linker_sha256=%s\n' "$(sha256sum kernel/link.ld | awk '{print $1}')"
        printf 'host_schedulable_cpus=%s\n' "$(nproc)"
        printf 'kvm_available=%s\n' "$([[ -r /dev/kvm && -w /dev/kvm ]] && printf YES || printf NO)"
        printf 'gcc=%s\n' "$(gcc --version | head -1)"
        printf 'qemu=%s\n' "$(qemu-system-x86_64 --version | head -1)"
    } >"$evidence/preflight/environment.env"
    git status --short >"$evidence/preflight/worktree-status.txt"
    df -B1 . >"$evidence/preflight/disk-space.txt"
    sha256sum AGENTS.md docs/foundation/validation-policy.md \
        >"$evidence/preflight/contracts.sha256"
    echo "STATS_PREFLIGHT: PASS evidence=$evidence"
}

static_checks()
{
    ensure_evidence
    mkdir -p "$evidence/static"
    bash -n scripts/test-taskman-stats-record.sh
    PYTHONDONTWRITEBYTECODE=1 python3 - <<'PY'
from pathlib import Path
compile(Path("scripts/verify-taskman-stats-evidence.py").read_text(),
        "scripts/verify-taskman-stats-evidence.py", "exec")
PY
    git diff --check
    git show "$base_commit:kernel/link.ld" >"$evidence/static/base-link.ld"
    cmp -s kernel/link.ld "$evidence/static/base-link.ld"
    for protected in makefile scripts/verify-foundation-regression.py \
                     scripts/test-foundation-regression.sh \
                     scripts/foundation-qemu.py; do
        git diff --quiet "$base_commit" -- "$protected"
    done
    python3 - "$base_commit" "$evidence/static/source-audit.json" <<'PY'
import ast
import json
from pathlib import Path
import re
import subprocess
import sys

base, output = sys.argv[1:]
source = Path("kernel/src/shell/commands/cmd_taskmantest.c").read_text()
start = source.index("static int format_stats_record(")
end = source.index("\nstatic bool stats_print_with_capacity", start)
block = source[start:end]
format_part = block[block.index('"\\n[TASKMANTEST]'):
                    block.index('(unsigned long long)stats->sessions')]
strings = [ast.literal_eval(value) for value in
           re.findall(r'"(?:[^"\\]|\\.)*"', format_part)]
fmt = "".join(strings)
plain = re.sub(r'%(?:llu|s)', '', fmt)
allowed = {
    "kernel/src/shell/commands/cmd_taskmantest.c",
    "scripts/taskman-stats-host.c",
    "scripts/test-taskman-stats-record.sh",
    "scripts/verify-taskman-stats-evidence.py",
    "docs/foundation/section-layout.md",
    "docs/foundation/validation-policy.md",
}
changed = set(subprocess.check_output(
    ["git", "diff", "--name-only", base], text=True).splitlines())
checks = {
    "numeric_placeholders": fmt.count("%llu") == 51,
    "mode_placeholders": fmt.count("%s") == 1,
    "literal_bytes_with_leading_lf": len(plain) == 922,
    "leading_lf": fmt.startswith("\n"),
    "trailing_lf": fmt.endswith("\n"),
    "single_success_emission": source.count("serial_write_all(record);") == 1,
    "dispatch_propagates": 'else if(!strcmp(c,"stats"))ok=stats_print();' in source,
    "legacy_decimal_retained": source.count("dec(") > 20,
    "scope": changed <= allowed,
}
payload = {"schema": 1, "checks": checks, "changed_paths": sorted(changed),
           "format": {"numeric": fmt.count("%llu"), "text": fmt.count("%s"),
                      "literal_bytes": len(plain), "capacity": 1953}}
Path(output).write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
if not all(checks.values()):
    raise SystemExit("stats source audit failed: " +
                     ", ".join(name for name, ok in checks.items() if not ok))
PY
    sha256sum kernel/link.ld kernel/src/shell/commands/cmd_taskmantest.c \
        scripts/taskman-stats-host.c scripts/verify-taskman-stats-evidence.py \
        scripts/test-taskman-stats-record.sh >"$evidence/static/source.sha256"
    echo "STATS_STATIC: PASS fields=52 capacity=1953 linker=unchanged"
}

compile_host_profile()
{
    local name=$1 sanitizer=$2
    shift 2
    local run="$evidence/host/$name"
    local -a extra=("$@")
    local -a common=(-std=gnu11 -Wall -Wextra -Werror -fno-builtin -g
                     -ffunction-sections -fdata-sections)
    local -a legacy=(
        -Dstrlen=hobbyos_legacy_strlen
        -Dstrcmp=hobbyos_legacy_strcmp
        -Dstrncmp=hobbyos_legacy_strncmp
        -Dstrchr=hobbyos_legacy_strchr
        -Dstrcpy=hobbyos_legacy_strcpy
        -Dstrcat=hobbyos_legacy_strcat
        -Dstrrev=hobbyos_legacy_strrev)
    local -a command_cmd=("$host_cc" "${common[@]}" "${extra[@]}"
        -DHOBBYOS_DEBUG_ASSERT=1 -DHOBBYOS_TASKMAN_STATS_HOST_TEST=1
        -c kernel/src/shell/commands/cmd_taskmantest.c -o "$run/cmd_taskmantest.o")
    local -a command_string=("$host_cc" "${common[@]}" "${extra[@]}"
        "${legacy[@]}" -c kernel/src/libc/string.c -o "$run/string.o")
    local -a command_host=("$host_cc" "${common[@]}" "${extra[@]}"
        -DHOBBYOS_DEBUG_ASSERT=1 -c scripts/taskman-stats-host.c
        -o "$run/host.o")
    local -a command_link=("$host_cc" "${extra[@]}" -Wl,--gc-sections
        "$run/cmd_taskmantest.o" "$run/string.o" "$run/host.o"
        -o "$run/taskman-stats-host")
    mkdir -p "$run"
    : >"$run/compile.log"
    local status=0
    for command_name in command_cmd command_string command_host command_link; do
        local -n command_ref=$command_name
        record_command "host-$name-$command_name" "${command_ref[@]}"
        set +e
        "${command_ref[@]}" >>"$run/compile.log" 2>&1
        status=$?
        set -e
        ((status == 0)) || break
    done
    printf '%s\n' "$status" >"$run/compile-exit-code.txt"
    ((status == 0)) || {
        cat "$run/compile.log" >&2
        return "$status"
    }
    record_command "host-$name-run" timeout 20 "$run/taskman-stats-host"
    set +e
    if [[ $name == asan ]]; then
        ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
            timeout 20 "$run/taskman-stats-host" >"$run/run.log" 2>&1
        status=$?
    else
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
            timeout 20 "$run/taskman-stats-host" >"$run/run.log" 2>&1
        status=$?
    fi
    set -e
    printf '%s\n' "$status" >"$run/run-exit-code.txt"
    ((status == 0)) || {
        cat "$run/run.log" >&2
        return "$status"
    }
    grep -Fxq '[STATS_HOST][CASE] id=single-emission status=PASS' "$run/run.log"
    grep -Eq '^\[STATS_HOST\]\[SUITE_END\] status=PASS cases=9 assertions=[1-9][0-9]* failures=0 fields=52 capacity=1953$' "$run/run.log"
    python3 - "$run/command.json" "$sanitizer" "$run/taskman-stats-host" \
        "${command_cmd[@]}" -- "${command_string[@]}" -- \
        "${command_host[@]}" -- "${command_link[@]}" <<'PY'
import hashlib
import json
from pathlib import Path
import sys

output, sanitizer, binary, *argv = sys.argv[1:]
groups = []
current = []
for value in argv:
    if value == "--":
        groups.append(current)
        current = []
    else:
        current.append(value)
groups.append(current)
path = Path(binary)
Path(output).write_text(json.dumps({
    "schema": 1, "sanitizer": sanitizer, "compile_argv": groups,
    "binary_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
}, indent=2, sort_keys=True) + "\n")
PY
    sha256sum "$run"/*.o "$run/taskman-stats-host" >"$run/SHA256SUMS"
    echo "STATS_HOST: PASS profile=$name"
}

host_tests()
{
    ensure_evidence
    compile_host_profile normal none
    compile_host_profile ubsan undefined \
        -fsanitize=undefined -fno-sanitize-recover=all
    compile_host_profile asan address \
        -fsanitize=address -fno-omit-frame-pointer
    python3 - "$evidence/host/result.json" <<'PY'
import json
from pathlib import Path
import sys
Path(sys.argv[1]).write_text(json.dumps({
    "schema": 1, "status": "PASS",
    "profiles": ["normal", "ubsan", "asan"],
    "cases": 9, "field_count": 52, "record_capacity": 1953,
}, indent=2, sort_keys=True) + "\n")
PY
    echo "STATS_HOST_TESTS: PASS profiles=3"
}

historical_check()
{
    ensure_evidence
    local source_profile="$historical_root/profiles/q35-kvm-smp24"
    local destination="$evidence/historical"
    mkdir -p "$destination"
    cp --reflink=auto "$source_profile/qemu/qemu-serial.log" \
        "$destination/qemu-serial.log"
    cp --reflink=auto "$source_profile/command-28.json" \
        "$destination/command-28.json"
    record_command historical-verifier python3 \
        scripts/verify-foundation-regression.py all "$historical_root"
    local status
    set +e
    PYTHONDONTWRITEBYTECODE=1 python3 \
        scripts/verify-foundation-regression.py all "$historical_root" \
        >"$destination/foundation-verifier.log" 2>&1
    status=$?
    set -e
    printf '%s\n' "$status" >"$destination/foundation-verifier.exit-code.txt"
    [[ $status == 1 ]]
    grep -Fq '[TASKMANTEST][STATS] count is 0, expected 1' \
        "$destination/foundation-verifier.log"
    [[ $(sha256sum "$destination/qemu-serial.log" | awk '{print $1}') == \
       df334cd069aaed1e9a948a4a4aca4cab83a857f97a41e21ff72fa220f986731d ]]
    [[ $(sha256sum "$destination/command-28.json" | awk '{print $1}') == \
       6ddbe250984c69cd9d2abe2359dab0058c1bc5a0e758925dd0d7ccf45bb19cb7 ]]
    python3 - "$destination/result.json" "$source_profile" <<'PY'
import json
from pathlib import Path
import sys
Path(sys.argv[1]).write_text(json.dumps({
    "schema": 1, "status": "PRESERVED_REJECTED",
    "source_profile": sys.argv[2], "foundation_verifier_exit_code": 1,
    "split_record_rejected": True, "sequence": 28,
    "serial_sha256": "df334cd069aaed1e9a948a4a4aca4cab83a857f97a41e21ff72fa220f986731d",
    "command_sha256": "6ddbe250984c69cd9d2abe2359dab0058c1bc5a0e758925dd0d7ccf45bb19cb7",
}, indent=2, sort_keys=True) + "\n")
PY
    echo "STATS_HISTORICAL: PASS status=PRESERVED_REJECTED"
}

extract_payloads()
{
    local image=$1 destination=$2
    mkdir -p "$destination"
    mcopy -o -i "$image" ::/EFI/BOOT/BOOTX64.EFI "$destination/BOOTX64.EFI"
    mcopy -o -i "$image" ::/kernel.elf "$destination/kernel.elf"
    mcopy -o -i "$image" ::/EFI/fonts/zap-light16.psf "$destination/zap-light16.psf"
    mcopy -o -i "$image" ::/EFI/images/logo.bmp "$destination/logo.bmp"
    mcopy -o -i "$image" ::/startup.nsh "$destination/startup.nsh"
}

build_candidate()
{
    ensure_evidence
    mkdir -p "$evidence/build" "$evidence/candidate"
    run_logged build-clean "$evidence/build/clean.log" make clean
    run_logged build-image "$evidence/build/image.log" \
        make image "JOBS=$jobs" SELFTEST=1 SELFTEST_AUTORUN=0 \
        KERNEL_EXTRA_CFLAGS=
    cp kernel.elf hobbyos.img BOOTX64.EFI "$evidence/candidate/"
    extract_payloads "$evidence/candidate/hobbyos.img" \
        "$evidence/candidate/payloads"
    cmp -s "$evidence/candidate/kernel.elf" \
        "$evidence/candidate/payloads/kernel.elf"
    cmp -s "$evidence/candidate/BOOTX64.EFI" \
        "$evidence/candidate/payloads/BOOTX64.EFI"
    nm -n "$evidence/candidate/kernel.elf" >"$evidence/candidate/symbols.txt"
    run_logged input-layout-build "$evidence/build/input-layout-build.log" \
        "$host_cc" -std=c11 -Wall -Wextra -Werror -I"$root" \
        scripts/command-transport-layout.c \
        -o "$evidence/candidate/command-transport-layout-host"
    run_logged input-layout-run "$evidence/build/input-layout-run.log" \
        "$evidence/candidate/command-transport-layout-host"
    tail -n 1 "$evidence/build/input-layout-run.log" >"$input_layout"
    PYTHONDONTWRITEBYTECODE=1 python3 - "$input_layout" <<'PY'
import json
import sys

value = json.load(open(sys.argv[1], encoding="utf-8"))
if value.get("schema") != 1 or value.get("slot_count") != 64 or \
        value.get("key_count") != 6:
    raise SystemExit("command input layout has unexpected dimensions")
PY
    sha256sum scripts/command-transport-layout.c \
        kernel/src/drivers/usb/xhci/xhci.h "$input_layout" \
        >"$evidence/candidate/command-input-layout.sha256"
    rm -f -- "$evidence/candidate/command-transport-layout-host"
    sha256sum "$evidence/candidate/kernel.elf" \
        "$evidence/candidate/hobbyos.img" "$evidence/candidate/BOOTX64.EFI" \
        "$evidence/candidate/payloads"/* >"$evidence/candidate/SHA256SUMS"
    {
        printf 'SELFTEST=1\nSELFTEST_AUTORUN=0\nDEBUG_ASSERT=1\n'
        printf 'KERNEL_EXTRA_CFLAGS=\n'
        printf 'HOBBYOS_PANIC_TEST=ABSENT\n'
        printf 'HOBBYOS_ASSERT_TEST=ABSENT\n'
        printf 'HOBBYOS_FORMAT_TEST=ABSENT\n'
    } >"$evidence/candidate/build-profile.env"
    echo "STATS_BUILD: PASS candidate=$(sha256sum "$evidence/candidate/kernel.elf" | awk '{print $1}')"
}

run_frame()
{
    local profile=$1 sequence=$2 payload=$3 timeout=$4 input_profile=$5
    local output="$profile/command-$sequence.json"
    local transport_record="$profile/transport-commands.jsonl"
    local status=0
    record_command "qemu-$(basename "$profile")-command-$sequence" \
        python3 scripts/foundation-qemu.py frame --root "$root" \
        --runtime "$qemu_runtime" --gdb-socket "$qemu_runtime/gdb.sock" \
        --elf "$evidence/candidate/kernel.elf" \
        --input-layout "$input_layout" \
        --record "$transport_record" \
        --transport-log "$profile/transport-$sequence.log" \
        --profile-id "$(basename "$profile")" \
        --vm-id "$(basename "$qemu_runtime")" \
        --candidate-sha256 "$candidate_sha256" --sequence "$sequence" \
        --payload "$payload" --timeout "$timeout" --allowed-status 0 \
        --input-profile "$input_profile"
    set +e
    PYTHONDONTWRITEBYTECODE=1 python3 scripts/foundation-qemu.py frame \
        --root "$root" --runtime "$qemu_runtime" \
        --gdb-socket "$qemu_runtime/gdb.sock" \
        --elf "$evidence/candidate/kernel.elf" \
        --input-layout "$input_layout" \
        --record "$transport_record" \
        --transport-log "$profile/transport-$sequence.log" \
        --profile-id "$(basename "$profile")" \
        --vm-id "$(basename "$qemu_runtime")" \
        --candidate-sha256 "$candidate_sha256" --sequence "$sequence" \
        --payload "$payload" --timeout "$timeout" --allowed-status 0 \
        --input-profile "$input_profile" >"$output" 2>&1
    status=$?
    set -e
    if ((status == 0)); then
        PYTHONDONTWRITEBYTECODE=1 python3 - \
            "$transport_record" "$profile/commands.jsonl" "$output" <<'PY'
import hashlib
import json
from pathlib import Path
import sys

transport_path, compatibility_path, output_path = map(Path, sys.argv[1:])
rows = [json.loads(line) for line in transport_path.read_text().splitlines()
        if line]
if not rows:
    raise SystemExit("transport controller did not append a record")
row = rows[-1]
required = {
    "schema": 26,
    "kind": "framed-command",
    "transport_status": "PASS",
    "classification": "PASS",
    "disposition": "EXECUTED",
    "guest_input_status": "PASS",
    "input_boundary_status": "CLEAN",
    "input_delivery_status": "COMPLETE",
    "input_character_boundary_status": "PASS",
    "qmp_input_status": "PASS",
    "qmp_text_status": "PASS",
    "qmp_enter_status": "PASS",
    "qmp_resume_status": "PASS",
    "dispatcher_status": "OBSERVED",
    "input_observer_cleanup_status": "PASS",
    "input_observer_cleanup_method": "detach-stopped-private-observer",
    "input_observer_cleanup_return_code": 0,
    "input_key_stroke_atomic": True,
    "input_shell_snapshot_policy":
        "matching-physical-reads-bracketing-keyboard-state",
}
differences = [name for name, expected in required.items()
               if row.get(name) != expected]
if differences:
    raise SystemExit("transport record contract differs: " +
                     ", ".join(differences))
fields = (
    "kind", "profile_id", "vm_id", "candidate_sha256", "sequence",
    "payload", "crc32", "start_line_count", "end_line_count",
    "start_byte_count", "end_byte_count", "handler_status",
    "transport_status", "classification", "disposition", "accept_count",
    "begin_count", "end_count", "replay_count", "reject_count",
    "checksum_verified",
)
compatibility = {name: row.get(name) for name in fields}
compatibility["schema"] = 2
compatibility["transport_schema"] = 26
compatibility["transport_record"] = output_path.name
compatibility["transport_record_sha256"] = hashlib.sha256(
    output_path.read_bytes()).hexdigest()
with compatibility_path.open("a", encoding="utf-8") as stream:
    stream.write(json.dumps(compatibility, sort_keys=True) + "\n")
print(json.dumps({
    "schema": row["schema"],
    "sequence": row["sequence"],
    "classification": row["classification"],
    "handler_status": row["handler_status"],
    "guest_input_status": row["guest_input_status"],
    "input_observer_cleanup_status":
        row["input_observer_cleanup_status"],
}, sort_keys=True))
PY
        status=$?
    else
        cat "$output"
    fi
    return "$status"
}

create_gdb_qemu_wrapper()
{
    local wrapper="$qemu_runtime/qemu-with-gdb"
    local real_qemu gdb_socket="$qemu_runtime/gdb.sock"
    real_qemu=$(command -v qemu-system-x86_64)
    python3 - "$wrapper" "$real_qemu" "$gdb_socket" \
        "$qemu_runtime/qemu-argv.txt" <<'PY'
import shlex
import sys

wrapper, real_qemu, gdb_socket, argv_log = sys.argv[1:]
body = f'''#!/usr/bin/env bash
set -euo pipefail
if [[ ${{1:-}} == --version ]]; then
  exec {shlex.quote(real_qemu)} "$@"
fi
printf '%q ' {shlex.quote(real_qemu)} "$@" -chardev {shlex.quote('socket,path=' + gdb_socket + ',server=on,wait=off,id=stats_gdb')} -gdb chardev:stats_gdb > {shlex.quote(argv_log)}
printf '\\n' >> {shlex.quote(argv_log)}
exec {shlex.quote(real_qemu)} "$@" -chardev {shlex.quote('socket,path=' + gdb_socket + ',server=on,wait=off,id=stats_gdb')} -gdb chardev:stats_gdb
'''
open(wrapper, "w", encoding="utf-8").write(body)
PY
    chmod 700 "$wrapper"
    printf '%s\n' "$wrapper"
}

wait_process_exit()
{
    local pid=$1 state
    for _ in {1..50}; do
        state=$(ps -p "$pid" -o stat= 2>/dev/null || printf '')
        [[ -z $state || $state == Z* ]] && return 0
        sleep .1
    done
    return 1
}

run_qemu_profile()
{
    local smp=$1 run=$2
    local name="q35-kvm-smp${smp}-run${run}"
    local profile="$evidence/qemu/$name"
    local working="$profile/working.img"
    local started ended stop_status image_before image_after workers wrapper
    mkdir -p "$profile"
    qemu_profile=$profile
    qemu_runtime=$(mktemp -d "/tmp/hobbyos-stats-${name}.XXXXXX")
    chmod 700 "$qemu_runtime"
    cp --reflink=auto "$evidence/candidate/hobbyos.img" "$working"
    image_before=$(sha256sum "$working" | awk '{print $1}')
    started=$(date --iso-8601=seconds)
    : >"$profile/commands.jsonl"
    : >"$profile/transport-commands.jsonl"
    {
        printf 'schema=1\nname=%s\nvm_id=%s\nmachine=q35\naccel=kvm\n' \
            "$name" "$(basename "$qemu_runtime")"
        printf 'smp=%s\ncandidate_sha256=%s\nimage_sha256_before=%s\n' \
            "$smp" "$candidate_sha256" "$image_before"
        printf 'host_schedulable_cpus=%s\ncleanup=PENDING\nrun_complete=NO\n' \
            "$(nproc)"
    } >"$profile/profile.env"
    cp "$input_layout" "$profile/command-input-layout.json"
    wrapper=$(create_gdb_qemu_wrapper)
    record_command "qemu-$name-start" env QEMU_RUNTIME="$qemu_runtime" \
        QEMU="$wrapper" \
        HOBBYOS_IMAGE="$working" MACHINE=q35 ACCEL=kvm SMP="$smp" MEM=2G \
        scripts/qemu-agent.sh start
    set +e
    QEMU="$wrapper" QEMU_RUNTIME="$qemu_runtime" HOBBYOS_IMAGE="$working" \
        MACHINE=q35 ACCEL=kvm SMP="$smp" MEM=2G \
        scripts/qemu-agent.sh start >"$profile/start.log" 2>&1
    local start_status=$?
    set -e
    printf '%s\n' "$start_status" >"$profile/start-exit-code.txt"
    ((start_status == 0)) || return "$start_status"
    qemu_owned=1
    qemu_pid=$(cat "$qemu_runtime/qemu.pid")
    for _ in {1..100}; do
        [[ -S $qemu_runtime/gdb.sock ]] && break
        sleep .1
    done
    [[ -S $qemu_runtime/gdb.sock ]]
    python3 - "$qemu_pid" "$profile/argv.json" <<'PY'
import json
from pathlib import Path
import sys
argv = [item.decode(errors="replace") for item in
        Path(f"/proc/{sys.argv[1]}/cmdline").read_bytes().split(b"\0") if item]
Path(sys.argv[2]).write_text(json.dumps(argv, indent=2) + "\n")
PY
    PYTHONDONTWRITEBYTECODE=1 python3 scripts/foundation-qemu.py wait-ready \
        --runtime "$qemu_runtime" \
        --marker '[BOOT][TEST_READY] PASS autorun=0' --timeout 240 \
        --output "$profile/runtime-ready.json" >"$profile/runtime-ready.log"
    workers=$((smp * 2))
    ((workers <= 32)) || workers=32
    run_frame "$profile" 1 'taskmantest stats-reset' 120 normal
    run_frame "$profile" 2 "smpstress $workers 0 1000" 120 stress
    run_frame "$profile" 3 'taskmantest auto-session 50 5' 240 stress
    run_frame "$profile" 4 'taskmantest stats' 120 stress
    run_frame "$profile" 5 'taskdiag check' 180 stress
    run_frame "$profile" 6 'killtest smpstress-sweep' 300 stress
    run_frame "$profile" 7 'taskdiag check' 180 stress
    run_frame "$profile" 8 'taskmantest check' 180 normal
    record_command "qemu-$name-stop" env QEMU_RUNTIME="$qemu_runtime" \
        scripts/qemu-agent.sh stop
    set +e
    QEMU_RUNTIME="$qemu_runtime" scripts/qemu-agent.sh stop \
        >"$profile/stop.log" 2>&1
    stop_status=$?
    set -e
    printf '%s\n' "$stop_status" >"$profile/stop-exit-code.txt"
    ((stop_status == 0)) || return "$stop_status"
    qemu_owned=0
    wait_process_exit "$qemu_pid"
    [[ ! -e $qemu_runtime/qemu.pid && ! -S $qemu_runtime/hmp.sock ]]
    mkdir -p "$profile/qemu"
    cp -a "$qemu_runtime/." "$profile/qemu/"
    ended=$(date --iso-8601=seconds)
    image_after=$(sha256sum "$working" | awk '{print $1}')
    {
        printf 'image_sha256_after=%s\ncleanup=PASS\nrun_complete=YES\n' \
            "$image_after"
        printf 'started_at=%s\nended_at=%s\n' "$started" "$ended"
    } >>"$profile/profile.env"
    qemu_pid=
    remove_private_runtime "$qemu_runtime"
    qemu_runtime=
    qemu_profile=
    echo "STATS_QEMU_PROFILE: PASS name=$name"
}

qemu_tests()
{
    ensure_evidence
    [[ -f $evidence/candidate/kernel.elf && -f $evidence/candidate/hobbyos.img ]]
    [[ -r /dev/kvm && -w /dev/kvm ]] || {
        echo "STATS_QEMU: BLOCKED KVM unavailable" >&2
        return 3
    }
    [[ ! -e .qemu/qemu.pid && ! -S .qemu/hmp.sock ]]
    if pgrep -x qemu-system-x86_64 >/dev/null 2>&1; then
        echo "STATS_QEMU: another QEMU process is active" >&2
        return 1
    fi
    mkdir -p .qemu
    exec 9>.qemu/harness.lock
    flock -n 9 || {
        echo "STATS_QEMU: another harness owns QEMU" >&2
        return 1
    }
    lock_held=1
    candidate_sha256=$(sha256sum "$evidence/candidate/kernel.elf" | awk '{print $1}')
    local smp run
    for smp in 4 24; do
        for run in 1 2 3; do
            run_qemu_profile "$smp" "$run"
        done
    done
    exec 9>&-
    lock_held=0
    echo "STATS_QEMU: PASS profiles=6"
}

collect_result()
{
    ensure_evidence
    local source_root="$evidence/source"
    local -a sources=(
        kernel/src/shell/commands/cmd_taskmantest.c
        kernel/link.ld
        scripts/taskman-stats-host.c
        scripts/test-taskman-stats-record.sh
        scripts/verify-taskman-stats-evidence.py
        scripts/verify-foundation-regression.py)
    local source
    for source in "${sources[@]}"; do
        mkdir -p "$source_root/$(dirname "$source")"
        cp "$source" "$source_root/$source"
    done
    python3 - "$evidence/source-manifest.json" "$evidence" \
        "${sources[@]}" <<'PY'
import hashlib
import json
from pathlib import Path
import sys
output = Path(sys.argv[1])
root = Path(sys.argv[2])
rows = []
for name in sys.argv[3:]:
    path = root / "source" / name
    rows.append({"path": name, "evidence_copy": f"source/{name}",
                 "bytes": path.stat().st_size,
                 "sha256": hashlib.sha256(path.read_bytes()).hexdigest()})
output.write_text(json.dumps({"schema": 1, "files": rows},
                             indent=2, sort_keys=True) + "\n")
PY
    local candidate_sha
    candidate_sha=$(sha256sum "$evidence/candidate/kernel.elf" | awk '{print $1}')
    python3 - "$evidence/result.json" "$candidate_sha" "$base_commit" <<'PY'
import json
from pathlib import Path
import sys
Path(sys.argv[1]).write_text(json.dumps({
    "schema": 1, "kind": "taskman-stats-record-evidence", "status": "PASS",
    "implementation_base_commit": sys.argv[3], "candidate_sha256": sys.argv[2],
    "field_count": 52, "numeric_fields": 51, "record_capacity": 1953,
    "host_profiles": ["normal", "ubsan", "asan"],
    "qemu_profiles": [
        f"q35-kvm-smp{smp}-run{run}"
        for smp in (4, 24) for run in range(1, 4)],
    "historical_split_record_status": "PRESERVED_REJECTED",
}, indent=2, sort_keys=True) + "\n")
PY
}

fixtures()
{
    ensure_evidence
    local output="$evidence/fixtures/final"
    [[ ! -e $output ]] || {
        echo "stats fixture output already exists: $output" >&2
        return 1
    }
    PYTHONDONTWRITEBYTECODE=1 python3 \
        scripts/verify-taskman-stats-evidence.py fixtures --output-dir "$output"
}

verify()
{
    PYTHONDONTWRITEBYTECODE=1 python3 \
        scripts/verify-taskman-stats-evidence.py all "$evidence"
}

command=${1:-}
case "$command" in
    preflight) preflight ;;
    static) static_checks ;;
    host) host_tests ;;
    historical) historical_check ;;
    build) build_candidate ;;
    qemu) qemu_tests; collect_result; verify ;;
    fixtures) fixtures ;;
    verify) verify ;;
    all)
        preflight
        static_checks
        host_tests
        historical_check
        build_candidate
        qemu_tests
        collect_result
        verify
        fixtures
        ;;
    *) usage ;;
esac
