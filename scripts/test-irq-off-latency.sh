#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
export HOBBYOS_VALIDATION_CONTRACT=guarded-kernel-stacks
export HOBBYOS_MEMORY_OBSERVER_ROLE=irq-off-latency
export PYTHONDONTWRITEBYTECODE=1
exec python3 -B scripts/irq-off-latency-gate.py "$@"
