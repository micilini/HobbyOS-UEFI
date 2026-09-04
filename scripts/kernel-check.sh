#!/usr/bin/env bash
set -euo pipefail
jobs=${1:-2}
log=${2:-artifacts/build/kernel-check-j${jobs}.log}
mkdir -p "$(dirname "$log")"
: >"$log"
set +e
make clean 2>&1 | tee -a "$log"
rc=${PIPESTATUS[0]}
if (( rc == 0 )); then
  make -j"$jobs" kernel.elf 2>&1 | tee -a "$log"
  rc=${PIPESTATUS[0]}
fi
set -e
if (( rc != 0 )); then echo "kernel-check: FAIL; log: $log"; exit "$rc"; fi
if grep -E 'implicit declaration of function|incompatible pointer type|makes (integer|pointer) from (pointer|integer)' "$log" >/dev/null; then
  echo "kernel-check: forbidden diagnostic found; log: $log"; exit 1
fi
hash=$(sha256sum kernel.elf | awk '{print $1}')
printf 'kernel-check: PASS\nlog: %s\nkernel.elf SHA-256: %s\n' "$log" "$hash" | tee -a "$log"
