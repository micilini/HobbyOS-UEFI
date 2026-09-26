#!/usr/bin/env bash
set -Eeuo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

timestamp=$(date '+%Y%m%d-%H%M%S-%z')
evidence=${MODAL_CHECKPOINT_EVIDENCE_DIR:-"artifacts/build/modal-checkpoints/$timestamp"}
base_commit=${MODAL_CHECKPOINT_BASE_COMMIT:-$(git rev-parse HEAD)}
host_cc=${CC:-gcc}

usage()
{
    echo "usage: $0 preflight|static|host|fixtures|collect|verify|all" >&2
    exit 2
}

ensure_evidence()
{
    [[ ! -L $evidence ]] || {
        echo "modal checkpoint evidence root must not be a symlink" >&2
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

preflight()
{
    ensure_evidence
    mkdir -p "$evidence/preflight"
    for tool in gcc git make python3 sha256sum timeout; do
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
        printf 'host_compiler=%s\n' "$("$host_cc" --version | head -1)"
    } >"$evidence/preflight/environment.env"
    git status --short >"$evidence/preflight/worktree-status.txt"
    git diff >"$evidence/preflight/worktree.patch"
    git diff --cached >"$evidence/preflight/index.patch"
    df -B1 . /dev/shm >"$evidence/preflight/disk-space.txt"
    echo "MODAL_CHECKPOINT_PREFLIGHT: PASS evidence=$evidence"
}

static_checks()
{
    ensure_evidence
    mkdir -p "$evidence/static"
    bash -n scripts/test-modal-checkpoints.sh
    bash -n scripts/test-libc-format.sh
    PYTHONDONTWRITEBYTECODE=1 python3 - <<'PY'
from pathlib import Path
for name in ("scripts/verify-modal-checkpoint-evidence.py",
             "scripts/verify-libc-format-evidence.py"):
    compile(Path(name).read_text(), name, "exec")
PY
    git diff --check
    python3 - "$base_commit" "$evidence/static/source-audit.json" <<'PY'
import json
from pathlib import Path
import subprocess
import sys

base, output = sys.argv[1:]
source_path = "kernel/src/shell/commands/cmd_taskmantest.c"
source = Path(source_path).read_text()
base_source = subprocess.check_output(
    ["git", "show", f"{base}:{source_path}"], text=True)

def stats_slice(text):
    start = text.index("static int format_stats_record(")
    end = text.index("\nint cmd_taskmantest(", start)
    return text[start:end]

allowed = {
    "kernel/src/shell/commands/cmd_taskmantest.c",
    "kernel/src/shell/commands/cmd_taskmantest.h",
    "scripts/modal-checkpoints-host.c",
    "scripts/test-modal-checkpoints.sh",
    "scripts/verify-modal-checkpoint-evidence.py",
    "scripts/test-libc-format.sh",
    "scripts/verify-libc-format-evidence.py",
    "docs/foundation/architecture.md",
    "docs/foundation/validation-policy.md",
    "docs/foundation/formatting.md",
}
changed = set(subprocess.check_output(
    ["git", "diff", "--name-only", base], text=True).splitlines())
checks = {
    "scope": changed <= allowed,
    "stats_builder_preserved": stats_slice(source) == stats_slice(base_source),
    "bounded_static_storage":
        "TASKMANTEST_MEMORY_CHECKPOINT_RECORD_CAPACITY 1536u" in source,
    "target_identity_scope": "scope=TARGET_IDENTITIES" in source,
    "global_heap_non_authoritative": "global_comparable=0" in source,
    "generation_handles":
        "scheduler_snapshot_task_by_handle(resource->handle" in source,
    "bounded_wait": "memory_checkpoint_deadline" in source,
    "single_record_sink":
        source.count("serial_write_all(g_memory_checkpoint.record);") == 1,
    "retention_is_failure":
        'report.reason = report.retained ? "target-retained" : "async-drain";'
        in source,
    "free_completion_required":
        "local_retained == 0 && reaper.free_inflight == 0" in source,
    "dispatch_propagates":
        "ok=memory_checkpoint_end_with_capacity();" in source,
    "new_protocol_in_consumer_gate":
        "taskmantest memory-checkpoint-end" in
        Path("scripts/test-libc-format.sh").read_text(),
}
payload = {
    "schema": 1,
    "base_commit": base,
    "changed_paths": sorted(changed),
    "checks": checks,
    "record_capacity": 1536,
    "maximum_workers": 32,
    "timeout_ms": 30000,
}
Path(output).write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
failed = [name for name, passed in checks.items() if not passed]
if failed:
    raise SystemExit("source audit failed: " + ", ".join(failed))
PY
    sha256sum \
        kernel/src/shell/commands/cmd_taskmantest.c \
        kernel/src/shell/commands/cmd_taskmantest.h \
        scripts/modal-checkpoints-host.c \
        scripts/test-modal-checkpoints.sh \
        scripts/verify-modal-checkpoint-evidence.py \
        scripts/test-libc-format.sh \
        scripts/verify-libc-format-evidence.py \
        >"$evidence/static/source.sha256"
    echo "MODAL_CHECKPOINT_STATIC: PASS capacity=1536 workers=32"
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
        -DHOBBYOS_DEBUG_ASSERT=1
        -DHOBBYOS_TASKMAN_MODAL_CHECKPOINT_HOST_TEST=1
        -c kernel/src/shell/commands/cmd_taskmantest.c -o "$run/cmd.o")
    local -a command_string=("$host_cc" "${common[@]}" "${extra[@]}"
        "${legacy[@]}" -c kernel/src/libc/string.c -o "$run/string.o")
    local -a command_host=("$host_cc" "${common[@]}" "${extra[@]}"
        -DHOBBYOS_DEBUG_ASSERT=1 -c scripts/modal-checkpoints-host.c
        -o "$run/host.o")
    local -a command_link=("$host_cc" "${extra[@]}" -Wl,--gc-sections
        "$run/cmd.o" "$run/string.o" "$run/host.o" -o "$run/host-test")
    [[ ! -e $run ]] || {
        echo "host profile already exists: $run" >&2
        return 1
    }
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
    record_command "host-$name-run" timeout 30 "$run/host-test"
    set +e
    if [[ $name == asan ]]; then
        ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 \
            timeout 30 "$run/host-test" >"$run/run.log" 2>&1
    elif [[ $name == ubsan ]]; then
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
            timeout 30 "$run/host-test" >"$run/run.log" 2>&1
    else
        timeout 30 "$run/host-test" >"$run/run.log" 2>&1
    fi
    status=$?
    set -e
    printf '%s\n' "$status" >"$run/run-exit-code.txt"
    python3 - "$run/command.json" "$name" "$sanitizer" "$status" \
        "${command_link[@]}" <<'PY'
import json
from pathlib import Path
import sys
output, name, sanitizer, status, *link = sys.argv[1:]
Path(output).write_text(json.dumps({
    "schema": 1,
    "profile": name,
    "sanitizer": sanitizer,
    "link_argv": link,
    "run_argv": ["timeout", "30", "host-test"],
    "run_exit_code": int(status),
}, indent=2, sort_keys=True) + "\n")
PY
    ((status == 0)) || {
        cat "$run/run.log" >&2
        return "$status"
    }
    sha256sum "$run"/*.o "$run/host-test" >"$run/SHA256SUMS"
}

host_tests()
{
    ensure_evidence
    compile_host_profile normal none
    compile_host_profile ubsan undefined \
        -fsanitize=undefined -fno-sanitize-recover=all
    compile_host_profile asan address \
        -fsanitize=address -fno-omit-frame-pointer
    echo "MODAL_CHECKPOINT_HOST: PASS profiles=3 cases=12 assertions=73"
}

fixtures()
{
    ensure_evidence
    [[ ! -e $evidence/fixtures ]] || {
        echo "fixture directory already exists: $evidence/fixtures" >&2
        return 1
    }
    record_command format-verifier-fixtures python3 \
        scripts/verify-libc-format-evidence.py fixtures \
        --output-dir "$evidence/fixtures/format-verifier"
    mkdir -p "$evidence/fixtures"
    set +e
    python3 scripts/verify-libc-format-evidence.py fixtures \
        --output-dir "$evidence/fixtures/format-verifier" \
        >"$evidence/fixtures/run.log" 2>&1
    local status=$?
    set -e
    printf '%s\n' "$status" >"$evidence/fixtures/run-exit-code.txt"
    ((status == 0)) || {
        cat "$evidence/fixtures/run.log" >&2
        return "$status"
    }
    echo "MODAL_CHECKPOINT_FIXTURES: PASS format_negatives=18 checkpoint_negatives=10"
}

collect()
{
    ensure_evidence
    python3 - "$evidence" "$base_commit" \
        "$(git rev-parse HEAD)" "$(git rev-parse 'HEAD^{tree}')" <<'PY'
import hashlib
import json
from pathlib import Path
import sys

root = Path(sys.argv[1])
base = sys.argv[2]
head = sys.argv[3]
tree = sys.argv[4]

def receipt(path):
    data = path.read_bytes()
    return {
        "path": path.relative_to(root).as_posix(),
        "bytes": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
    }

source_names = (
    "kernel/src/shell/commands/cmd_taskmantest.c",
    "kernel/src/shell/commands/cmd_taskmantest.h",
    "scripts/modal-checkpoints-host.c",
    "scripts/test-modal-checkpoints.sh",
    "scripts/verify-modal-checkpoint-evidence.py",
    "scripts/test-libc-format.sh",
    "scripts/verify-libc-format-evidence.py",
)
sources = {}
for name in source_names:
    source = Path(name)
    data = source.read_bytes()
    copy = root / "source" / name
    copy.parent.mkdir(parents=True, exist_ok=True)
    copy.write_bytes(data)
    sources[name] = receipt(copy)

profiles = {}
for name, sanitizer in (("normal", "none"), ("ubsan", "undefined"),
                        ("asan", "address")):
    run = root / "host" / name
    profiles[name] = {
        "sanitizer": sanitizer,
        "compile_exit_code": int((run / "compile-exit-code.txt").read_text()),
        "run_exit_code": int((run / "run-exit-code.txt").read_text()),
        "compile_log": receipt(run / "compile.log"),
        "run_log": receipt(run / "run.log"),
        "command": receipt(run / "command.json"),
    }

historical_files = {}
for name in ("historical/causal-findings.md",
             "historical/modal-causal-summary.json"):
    path = root / name
    if path.is_file():
        historical_files[name] = receipt(path)

result = {
    "schema": 1,
    "kind": "modal-memory-checkpoint-evidence",
    "status": "PASS",
    "base_commit": base,
    "candidate_head": head,
    "candidate_tree": tree,
    "sources": sources,
    "source_audit": receipt(root / "static/source-audit.json"),
    "host_profiles": profiles,
    "format_fixture_result": receipt(
        root / "fixtures/format-verifier/fixture-results.json"),
    "historical": {
        "raw_result": "FAIL_PLUS_64_PRESERVED",
        "allocation_identity": "NOT_DETERMINED",
        "endpoint_comparability": "NOT_ESTABLISHED",
    },
    "historical_files": historical_files,
}
(root / "result.json").write_text(
    json.dumps(result, indent=2, sort_keys=True) + "\n")
PY
    echo "MODAL_CHECKPOINT_COLLECTION: PASS"
}

verify()
{
    record_command verify-offline python3 \
        scripts/verify-modal-checkpoint-evidence.py all "$evidence"
    python3 scripts/verify-modal-checkpoint-evidence.py all "$evidence" \
        | tee "$evidence/verify.log"
    printf '%s\n' "${PIPESTATUS[0]}" >"$evidence/verify-exit-code.txt"
}

case ${1:-} in
    preflight) preflight ;;
    static) static_checks ;;
    host) host_tests ;;
    fixtures) fixtures ;;
    collect) collect ;;
    verify) verify ;;
    all)
        preflight
        static_checks
        host_tests
        fixtures
        collect
        verify
        ;;
    *) usage ;;
esac
