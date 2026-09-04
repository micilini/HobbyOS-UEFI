#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

cl13=f9d47db7e4332fc2d88dce2842f997a9b3f82e57
release_root=artifacts/release
dest=$release_root/TASKMAN_V1
mkdir -p "$release_root"

paths=(
  README.md
  docs/taskman-v1-contract.md
  docs/taskman-v1-architecture.md
  docs/task-lifecycle.md
  docs/input-modal-architecture.md
  docs/taskman-v1-user-guide.md
  docs/taskman-v1-command-reference.md
  docs/taskman-v1-known-limitations.md
  docs/taskman-v1-test-plan.md
  docs/taskman-v1-homologation.md
  docs/taskman-v1-file-inventory.md
  docs/taskman-v1-handoff-checklist.md
  docs/taskman-v2-entry-criteria.md
  docs/releases/TASKMAN_V1_RELEASE_NOTES.md
  docs/releases/TASKMAN_V1_RELEASE_MANIFEST.md
  docs/test-reports/TMV1-CL-13-taskman-v1-certification.md
  docs/test-reports/TMV1-CL-13-FIX15-taskman-transaction-batching.md
)

for path in kernel.elf hobbyos.img "${paths[@]}"; do
  [[ -f $path ]] || { echo "package: missing $path" >&2; exit 1; }
done

candidate_paths=(
  README.md AGENTS.md docs/development/build-and-qemu.md
  docs/taskman-v1-contract.md docs/taskman-v1-architecture.md
  docs/task-lifecycle.md docs/input-modal-architecture.md
  docs/taskman-v1-user-guide.md docs/taskman-v1-command-reference.md
  docs/taskman-v1-known-limitations.md docs/taskman-v1-test-plan.md
  docs/taskman-v1-homologation.md docs/taskman-v1-file-inventory.md
  docs/taskman-v1-handoff-checklist.md docs/taskman-v2-entry-criteria.md
  docs/releases/TASKMAN_V1_RELEASE_NOTES.md
  docs/releases/TASKMAN_V1_RELEASE_MANIFEST.md
  scripts/verify-taskman-v1-evidence.py scripts/verify-taskman-v1-docs.py
  scripts/test-cl14-release.sh scripts/package-taskman-v1-release.sh
  kernel/src/core/kernel_init.c kernel/src/drivers/usb/xhci/xhci.c
  kernel/src/core/modal_session.h
  kernel/src/shell/commands/cmd_taskmantest.c
  kernel/src/shell/commands/cmd_modaltest.c
)

candidate_index=$(mktemp)
cleanup_index(){ rm -f "$candidate_index"; }
trap cleanup_index EXIT
rm -f "$candidate_index"
GIT_INDEX_FILE=$candidate_index git read-tree HEAD
GIT_INDEX_FILE=$candidate_index git add -- "${candidate_paths[@]}"
tree=$(GIT_INDEX_FILE=$candidate_index git write-tree)

if [[ -e $dest ]]; then
  [[ $dest == artifacts/release/TASKMAN_V1 ]] || exit 1
  rm -rf "$dest"
fi
mkdir -p "$dest"

for path in "${paths[@]}"; do
  mkdir -p "$dest/$(dirname "$path")"
  cp "$path" "$dest/$path"
done
cp kernel.elf hobbyos.img "$dest/"
printf '%s\n' "$cl13" >"$dest/CL13_COMMIT.txt"
printf '%s\n' "$tree" >"$dest/CL14_TREE.txt"
{
  echo "TASKMAN V1 release source inventory"
  echo "CL13_COMMIT=$cl13"
  echo "CL14_TREE=$tree"
  echo
  git diff --name-status 4e700a9b38653fb57ecb0f107a18f87508db0736..HEAD
  GIT_INDEX_FILE=$candidate_index git diff --cached --name-status HEAD
} >"$dest/SOURCE_TREE.txt"

(
  cd "$dest"
  find . -type f ! -name SHA256SUMS -print0 | sort -z |
    xargs -0 sha256sum >SHA256SUMS
  sha256sum -c SHA256SUMS
)

zip_path=$release_root/TASKMAN_V1_HANDOFF_${cl13:0:7}_${tree:0:7}.zip
rm -f "$zip_path"
python3 - "$dest" "$zip_path" <<'PY'
import pathlib
import sys
import zipfile

source = pathlib.Path(sys.argv[1])
target = pathlib.Path(sys.argv[2])
with zipfile.ZipFile(target, "w", compression=zipfile.ZIP_DEFLATED) as archive:
    for path in sorted(source.rglob("*")):
        if path.is_file():
            archive.write(path, pathlib.Path(source.name) / path.relative_to(source))
PY

unzip -t "$zip_path" >/dev/null
if unzip -Z1 "$zip_path" | grep -Eq '(^|/)\.git(/|$)|\.(o|d|su)$|(^|/)\.qemu(/|$)'; then
  echo "package: forbidden entry in ZIP" >&2
  exit 1
fi

package_hash=$(sha256sum "$zip_path" | awk '{print $1}')
printf '[CL14][PACKAGE] PASS directory=%s zip=%s sha256=%s tree=%s\n' \
  "$dest" "$zip_path" "$package_hash" "$tree"
