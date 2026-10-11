#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
export PYTHONDONTWRITEBYTECODE=1 HOBBYOS_VALIDATION_CONTRACT=tlb-coherency
exec python3 -B scripts/tlb-shootdown-gate.py "$@"
