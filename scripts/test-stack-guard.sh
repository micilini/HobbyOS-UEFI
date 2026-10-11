#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
export HOBBYOS_VALIDATION_CONTRACT=guarded-kernel-stacks
export PYTHONDONTWRITEBYTECODE=1
exec python3 -B scripts/stack-guard-gate.py "$@"
