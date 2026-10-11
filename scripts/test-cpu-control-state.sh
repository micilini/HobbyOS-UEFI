#!/usr/bin/env bash
set -Eeuo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
export HOBBYOS_VALIDATION_CONTRACT=cpu-control-state
export PYTHONDONTWRITEBYTECODE=1
cd "$root"
exec python3 "$root/scripts/cpu-control-state-gate.py" "$@"
