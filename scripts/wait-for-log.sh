#!/usr/bin/env bash
set -euo pipefail
[[ $# -eq 3 ]] || { echo "usage: $0 FILE PATTERN TIMEOUT_SECONDS" >&2; exit 2; }
file=$1 pattern=$2 timeout=$3
[[ $timeout =~ ^[0-9]+$ ]] || { echo "timeout must be an integer" >&2; exit 2; }
end=$((SECONDS + timeout))
while (( SECONDS <= end )); do
  [[ -f $file ]] && grep -F -- "$pattern" "$file" >/dev/null && { echo "found: $pattern"; exit 0; }
  sleep 0.2
done
echo "timeout waiting for '$pattern' in $file" >&2
[[ -f $file ]] && tail -n 30 "$file" >&2
exit 1
