#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
export PYTHONDONTWRITEBYTECODE=1 HOBBYOS_VALIDATION_CONTRACT=physical-translation
exec python3 -B scripts/physical-translation-gate.py "$@"
