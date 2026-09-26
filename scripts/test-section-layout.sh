#!/usr/bin/env bash
set -Eeuo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

timestamp=$(date '+%Y%m%d-%H%M%S-%z')
evidence=${SECTION_EVIDENCE_DIR:-"artifacts/build/section-layout/$timestamp"}
base_commit=${SECTION_BASE_COMMIT:-$(git rev-parse HEAD)}
candidate_manifest=${SECTION_CANDIDATE_MANIFEST:-}
verifier=(python3 scripts/verify-section-layout.py)

usage()
{
    echo "usage: $0 preflight|core|candidates|fixtures|verify|elf|all [ELF]" >&2
    exit 2
}

ensure_root()
{
    [[ ! -L $evidence ]] || {
        echo "section-layout evidence root must not be a symlink" >&2
        return 1
    }
    mkdir -p "$evidence"
}

preflight()
{
    ensure_root
    mkdir -p "$evidence/preflight"
    for tool in gcc ld make python3 readelf objdump nm size sha256sum; do
        command -v "$tool" >/dev/null
    done
    {
        printf 'delivery_dir=%s\n' "$root"
        printf 'repo_root=%s\n' "$(git rev-parse --show-toplevel)"
        printf 'branch=%s\n' "$(git branch --show-current)"
        printf 'head=%s\n' "$(git rev-parse HEAD)"
        printf 'tree=%s\n' "$(git rev-parse HEAD^{tree})"
        printf 'parent=%s\n' "$(git rev-parse HEAD^)"
        printf 'base_commit=%s\n' "$base_commit"
        printf 'linker_sha256=%s\n' "$(sha256sum kernel/link.ld | awk '{print $1}')"
        printf 'ld_version=%s\n' "$(ld --version | head -1)"
    } >"$evidence/preflight/environment.txt"
    git status --short >"$evidence/preflight/worktree-status.txt"
    echo "SECTION_LAYOUT_PREFLIGHT: PASS evidence=$evidence"
}

collect_core()
{
    if [[ -e $evidence/campaign.json ]]; then
        echo "section-layout core evidence already exists: $evidence" >&2
        return 1
    fi
    local -a references=(
        --reference baseline=artifacts/build/foundation-baseline/20260904-220652-0300/reference/kernel.elf
        --reference prior-debug-on=artifacts/build/runtime-assertions/20260906-000827-0300/production/debug-on/kernel.elf
        --reference prior-debug-off=artifacts/build/runtime-assertions/20260906-000827-0300/production/debug-off/kernel.elf
    )
    "${verifier[@]}" collect-core "$evidence" --repo "$root" \
        --base-commit "$base_commit" "${references[@]}"
}

collect_candidates()
{
    [[ -n $candidate_manifest ]] || {
        echo "SECTION_CANDIDATE_MANIFEST is required for candidate collection" >&2
        return 2
    }
    "${verifier[@]}" collect-candidates "$evidence" --repo "$root" \
        --manifest "$candidate_manifest"
}

run_fixtures()
{
    "${verifier[@]}" fixtures --evidence-root "$evidence" \
        --output-dir "$evidence/fixtures"
}

command=${1:-}
case "$command" in
    preflight)
        preflight
        ;;
    core)
        collect_core
        "${verifier[@]}" core "$evidence"
        ;;
    candidates)
        collect_candidates
        ;;
    fixtures)
        run_fixtures
        ;;
    verify)
        "${verifier[@]}" all "$evidence"
        ;;
    elf)
        [[ $# -eq 2 ]] || usage
        "${verifier[@]}" elf "$2"
        ;;
    all)
        [[ -n $candidate_manifest ]] || {
            echo "SECTION_CANDIDATE_MANIFEST is required for all" >&2
            exit 2
        }
        [[ ! -e $evidence/campaign.json ]] || {
            echo "section-layout all requires a fresh evidence root: $evidence" >&2
            exit 1
        }
        preflight
        collect_core
        collect_candidates
        "${verifier[@]}" all "$evidence"
        run_fixtures
        ;;
    *)
        usage
        ;;
esac
