#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
export HOBBYOS_VALIDATION_CONTRACT=verified-ram-direct-map
export PYTHONDONTWRITEBYTECODE=1
exec python3 -B scripts/direct-map-gate.py "$@"
