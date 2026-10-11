#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
export HOBBYOS_VALIDATION_CONTRACT=kernel-image-protection
export PYTHONDONTWRITEBYTECODE=1
exec python3 -B scripts/kernel-wx-gate.py "$@"
