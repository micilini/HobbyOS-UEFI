#!/usr/bin/env bash
set -Eeuo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

timestamp=$(date '+%Y%m%d-%H%M%S-%z')
evidence=${ASSERT_RECORD_EVIDENCE_DIR:-"artifacts/build/assertion-records/$timestamp"}
base_commit=${ASSERT_RECORD_BASE_COMMIT:-$(git rev-parse HEAD)}
host_cc=${CC:-gcc}
historical_source=
historical_source+=artifacts/build/diagnostic-records/
historical_source+=20260906-123417--0300/regression/assertions/runtime/
historical_source+=q35-kvm-smp24-valid-list-none

usage()
{
    echo "usage: $0 preflight|static|host|historical|fixtures|verify|all" >&2
    exit 2
}

ensure_evidence()
{
    [[ ! -L $evidence ]] || {
        echo "assertion record evidence root must not be a symlink" >&2
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
    mkdir -p "$(dirname "$log")"
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

preflight()
{
    ensure_evidence
    mkdir -p "$evidence/preflight"
    for tool in gcc git make python3 sha256sum; do
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
    } >"$evidence/preflight/environment.env"
    git status --short >"$evidence/preflight/worktree-status.txt"
    sha256sum AGENTS.md docs/foundation/validation-policy.md \
        kernel/link.ld kernel/src/core/panic.c \
        kernel/src/drivers/serial.c scripts/assert-qemu.py \
        scripts/verify-assert-list-evidence.py \
        >"$evidence/preflight/protected.sha256"
    echo "ASSERT_RECORD_PREFLIGHT: PASS evidence=$evidence"
}

static_checks()
{
    ensure_evidence
    mkdir -p "$evidence/static"
    bash -n scripts/test-assert-record.sh
    PYTHONDONTWRITEBYTECODE=1 python3 - <<'PY'
from pathlib import Path
compile(Path("scripts/verify-assert-record-evidence.py").read_text(),
        "scripts/verify-assert-record-evidence.py", "exec")
PY
    git diff --check
    python3 - "$base_commit" "$evidence/static/result.json" <<'PY'
import hashlib
import json
from pathlib import Path
import subprocess
import sys

base, output = sys.argv[1], Path(sys.argv[2])
allowed = {
    "kernel/src/core/assert_selftest.c",
    "scripts/assert-record-host.c",
    "scripts/test-assert-record.sh",
    "scripts/verify-assert-record-evidence.py",
    "docs/foundation/section-layout.md",
    "docs/foundation/validation-policy.md",
}
changed = set(subprocess.check_output(
    ["git", "diff", "--name-only", base], text=True).splitlines())
for line in subprocess.check_output(
        ["git", "status", "--porcelain=v1"], text=True).splitlines():
    path = line[3:]
    if " -> " in path:
        path = path.split(" -> ", 1)[1]
    if not path.startswith("artifacts/") and path not in ("hobbyos.zip", "hobbyos.zip.sha256"):
        changed.add(path)
protected = [
    "kernel/link.ld", "makefile", "AGENTS.md", ".gitignore",
    "kernel/src/core/assert_selftest.h", "kernel/src/core/panic.c",
    "kernel/src/drivers/serial.c",
    "kernel/src/shell/commands/cmd_taskmantest.c",
    "scripts/assert-qemu.py", "scripts/verify-assert-list-evidence.py",
]
protected_rows = []
for name in protected:
    old = subprocess.check_output(["git", "show", f"{base}:{name}"])
    current = Path(name).read_bytes()
    protected_rows.append({
        "path": name,
        "unchanged": old == current,
        "sha256": hashlib.sha256(current).hexdigest(),
    })
source = Path("kernel/src/core/assert_selftest.c").read_text()
checks = {
    "scope": changed <= allowed,
    "protected": all(row["unchanged"] for row in protected_rows),
    "state_layout_unchanged": "sizeof(assertion_test_state_t) == 144" in source,
    "record_capacity": "#define ASSERTION_TEST_RECORD_CAPACITY 96u" in source,
    "formatter_calls": source.count("return ksnprintf(") == 4,
    "leading_record_delimiters": source.count('"\\n[ASSERT_TEST][') == 4,
    "legacy_decimal_removed": "assertion_test_emit_u64" not in source,
    "character_emission_removed": "serial_putc_all" not in source,
    "bounded_normal_emissions": source.count("serial_write_all(record);") == 2,
}
result = {
    "schema": 1,
    "status": "PASS" if all(checks.values()) else "FAIL",
    "checks": checks,
    "changed_paths": sorted(changed),
    "protected": protected_rows,
}
output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
if not all(checks.values()):
    raise SystemExit("assertion record static checks failed: " +
                     ", ".join(name for name, passed in checks.items()
                               if not passed))
PY
    echo "ASSERT_RECORD_STATIC: PASS records=4 capacity=96"
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
    local -a command_selftest=("$host_cc" "${common[@]}" "${extra[@]}"
        -DHOBBYOS_DEBUG_ASSERT=1 -DHOBBYOS_ASSERT_TEST=1
        -DHOBBYOS_ASSERT_RECORD_HOST_TEST=1
        -c kernel/src/core/assert_selftest.c -o "$run/assert_selftest.o")
    local -a command_string=("$host_cc" "${common[@]}" "${extra[@]}"
        "${legacy[@]}" -c kernel/src/libc/string.c -o "$run/string.o")
    local -a command_host=("$host_cc" "${common[@]}" "${extra[@]}"
        -DHOBBYOS_DEBUG_ASSERT=1 -DHOBBYOS_ASSERT_TEST=1
        -c scripts/assert-record-host.c -o "$run/host.o")
    local -a command_link=("$host_cc" "${extra[@]}" -Wl,--gc-sections
        "$run/assert_selftest.o" "$run/string.o" "$run/host.o"
        -o "$run/assert-record-host")
    mkdir -p "$run"
    : >"$run/compile.log"
    local status=0
    for command_name in command_selftest command_string command_host command_link; do
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
    record_command "host-$name-run" timeout 20 "$run/assert-record-host"
    set +e
    if [[ $sanitizer == address ]]; then
        ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
            timeout 20 "$run/assert-record-host" >"$run/run.log" 2>&1
        status=$?
    else
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
            timeout 20 "$run/assert-record-host" >"$run/run.log" 2>&1
        status=$?
    fi
    set -e
    printf '%s\n' "$status" >"$run/run-exit-code.txt"
    ((status == 0)) || {
        cat "$run/run.log" >&2
        return "$status"
    }
    grep -Fxq '[ASSERT_RECORD_HOST][CASE] id=single-emission status=PASS' \
        "$run/run.log"
    grep -Fxq '[ASSERT_RECORD_HOST][CASE] id=writer-boundary status=PASS' \
        "$run/run.log"
    grep -Eq '^\[ASSERT_RECORD_HOST\]\[SUITE_END\] status=PASS cases=9 assertions=[1-9][0-9]* failures=0 records=4 capacity=96$' \
        "$run/run.log"
    sha256sum "$run"/*.o "$run/assert-record-host" >"$run/SHA256SUMS"
    python3 - "$run/command.json" "$sanitizer" "$run/assert-record-host" <<'PY'
import hashlib
import json
from pathlib import Path
import sys
output, sanitizer, binary = Path(sys.argv[1]), sys.argv[2], Path(sys.argv[3])
output.write_text(json.dumps({
    "schema": 1,
    "sanitizer": sanitizer,
    "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
}, indent=2, sort_keys=True) + "\n")
PY
    echo "ASSERT_RECORD_HOST: PASS profile=$name"
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
import re
import sys
root = Path(sys.argv[1]).parent
counts = []
for profile in ("normal", "ubsan", "asan"):
    text = (root / profile / "run.log").read_text()
    match = re.search(r"SUITE_END\] status=PASS cases=9 assertions=([0-9]+)", text)
    if not match:
        raise SystemExit(f"host summary missing for {profile}")
    counts.append(int(match.group(1)))
Path(sys.argv[1]).write_text(json.dumps({
    "schema": 1,
    "status": "PASS",
    "profiles": ["normal", "ubsan", "asan"],
    "cases": 9,
    "assertions_per_profile": counts,
    "records": 4,
    "record_capacity": 96,
}, indent=2, sort_keys=True) + "\n")
PY
    echo "ASSERT_RECORD_HOST_TESTS: PASS profiles=3"
}

historical_check()
{
    ensure_evidence
    [[ -d $historical_source ]] || {
        echo "historical assertion run is missing: $historical_source" >&2
        return 1
    }
    local destination="$evidence/historical"
    mkdir -p "$destination"
    for name in serial-transcript.log result.json launch.json \
                launch-command.txt gdb-after.log gdb-observer.log \
                qmp-events.jsonl; do
        local target=$name
        [[ $name == result.json ]] && target=run-result.json
        if [[ -e $destination/$target ]]; then
            cmp -s "$historical_source/$name" "$destination/$target"
        else
            cp "$historical_source/$name" "$destination/$target"
        fi
    done
    run_logged historical-parser "$destination/verifier.log" \
        env PYTHONDONTWRITEBYTECODE=1 python3 \
        scripts/verify-assert-record-evidence.py historical \
        "$destination/serial-transcript.log"
    python3 - "$destination/result.json" \
        "$destination/serial-transcript.log" <<'PY'
import hashlib
import json
from pathlib import Path
import sys
serial = Path(sys.argv[2])
data = serial.read_bytes()
lines = data.splitlines(keepends=True)
start = sum(len(line) for line in lines[:789])
end = start + len(lines[789])
next_end = end + len(lines[790])
Path(sys.argv[1]).write_text(json.dumps({
    "schema": 1,
    "status": "PRESERVED_REJECTED",
    "scenario": "q35-kvm-smp24-valid-list-none",
    "rejection_code": "EXECUTE_SCENARIO_INVALID",
    "serial_sha256": hashlib.sha256(data).hexdigest(),
    "execute_line_number": 790,
    "continuation_line_number": 791,
    "execute_byte_range": [start, end],
    "continuation_byte_range": [end, next_end],
}, indent=2, sort_keys=True) + "\n")
PY
    sha256sum "$destination"/serial-transcript.log \
        "$destination"/run-result.json "$destination"/launch.json \
        "$destination"/launch-command.txt "$destination"/gdb-after.log \
        "$destination"/gdb-observer.log "$destination"/qmp-events.jsonl \
        >"$destination/SHA256SUMS"
    echo "ASSERT_RECORD_HISTORICAL: PASS status=PRESERVED_REJECTED"
}

fixture_tests()
{
    ensure_evidence
    local output="$evidence/oracle-fixtures"
    [[ ! -e $output ]] || {
        echo "fixture output already exists: $output" >&2
        return 1
    }
    run_logged fixtures "$evidence/commands/fixtures.log" \
        env PYTHONDONTWRITEBYTECODE=1 python3 \
        scripts/verify-assert-record-evidence.py fixtures \
        --output-dir "$output"
    cat "$evidence/commands/fixtures.log"
}

verify_only()
{
    run_logged verify "$evidence/commands/verify.log" \
        env PYTHONDONTWRITEBYTECODE=1 python3 \
        scripts/verify-assert-record-evidence.py all "$evidence"
    cat "$evidence/commands/verify.log"
}

case ${1:-} in
    preflight) preflight ;;
    static) preflight; static_checks ;;
    host) preflight; host_tests ;;
    historical) historical_check ;;
    fixtures) fixture_tests ;;
    verify) verify_only ;;
    all)
        preflight
        static_checks
        host_tests
        historical_check
        fixture_tests
        verify_only
        echo "ASSERT_RECORD_GATE: PASS evidence=$evidence"
        ;;
    *) usage ;;
esac
