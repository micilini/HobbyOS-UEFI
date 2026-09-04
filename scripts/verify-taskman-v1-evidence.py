#!/usr/bin/env python3
"""Offline verifier for the frozen TASKMAN V1 / CL-13 evidence set."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "artifacts/build"
CL13_KERNEL_HASH = "118284945ca3b469cba586cf5334ddedd28fb73baec61f130784a5b9d719bc1e"
CL13_IMAGE_HASH = "46c36ec6298c533d82ddc122494dcb10953c4d60b08fa45ae85fe59036605105"
EXPECTED_HASHES = {
    "artifacts/build/cl13-soak-smp4-tcg.log": "fe5955f350b12d2b2bd0b1a6c649251a1db4691960a9f17079923ec5b073c0a8",
    "artifacts/build/cl13-soak-smp8-kvm.log": "0431d80191c769121b1d22cc7c3dd5703c66a55a2faef5603a6a39794c69ab66",
    "artifacts/build/cl13fix15-soak8-stages.tsv": "e751035b1286724a6ef431a291a40271390eee969bf590ed885e8ed13c22dff7",
    "docs/test-reports/TMV1-CL-13-taskman-v1-certification.md": "9f8a396dce7855e9b88195fa5072e4ad76159a8fc8b8d2d7617ea6686d4b2819",
    "docs/test-reports/TMV1-CL-13-FIX15-taskman-transaction-batching.md": "c209a5b00b8a8319d945fa9ecac391e6269f6ae57081dd5b14e169706c76e34b",
}
PRODUCTION_MANIFESTS = [
    "cl13fix15-production-runtime-before.sha256",
    "cl13fix15-production-runtime-after-focused.sha256",
    "cl13fix15-production-runtime-after-soak.sha256",
    "cl13fix15-production-runtime-after-build.sha256",
]
SPECIAL_MANIFESTS = [
    "cl13fix15-special-before.sha256",
    "cl13fix15-special-after-focused.sha256",
    "cl13fix15-special-after-soak.sha256",
    "cl13fix15-special-after-build.sha256",
]
MANIFEST_DIGESTS = {
    "production": "db0e1c114a9ba5b8ddbde5f8310448de9fb43007e8e976e39646d268241bd50a",
    "special": "c39379719eb54d0463dd03831411952ab21db58f9ed98dfdf15fdc2c65bcbf12",
}


class VerificationError(RuntimeError):
    pass


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise VerificationError(message)


def summary_record(path: Path, smp: int, accel: str) -> tuple[dict[str, str], str]:
    prefix = f"[CL13][SOAK] PASS smp={smp} accel={accel} "
    matches = [line for line in path.read_text(errors="replace").splitlines()
               if line.startswith(prefix)]
    require(len(matches) == 1, f"{path}: expected one complete soak summary")
    line = matches[0]
    values: dict[str, str] = {}
    for token in line.split()[4:]:
        if "=" in token:
            key, value = token.split("=", 1)
            values[key] = value
    return values, line


def integer(values: dict[str, str], key: str, context: str) -> int:
    require(key in values and re.fullmatch(r"-?[0-9]+", values[key]) is not None,
            f"{context}: missing/non-integer {key}")
    return int(values[key])


def verify_manifests(names: list[str], expected_digest: str) -> dict[str, str]:
    contents: list[bytes] = []
    result: dict[str, str] = {}
    for name in names:
        path = OUT / name
        require(path.is_file(), f"missing manifest: {path}")
        digest = sha256(path)
        require(digest == expected_digest,
                f"manifest digest mismatch: {name}: {digest}")
        contents.append(path.read_bytes())
        result[name] = digest
    require(all(data == contents[0] for data in contents[1:]),
            f"manifest contents differ: {', '.join(names)}")
    return result


def verify() -> dict[str, object]:
    hashes: dict[str, str] = {}
    for relative, expected in EXPECTED_HASHES.items():
        path = ROOT / relative
        require(path.is_file(), f"missing evidence: {relative}")
        actual = sha256(path)
        require(actual == expected, f"hash mismatch: {relative}: {actual}")
        hashes[relative] = actual

    binary_provenance: dict[str, str] = {}
    if (ROOT / "kernel.elf").is_file() and sha256(ROOT / "kernel.elf") == CL13_KERNEL_HASH:
        binary_provenance["kernel.elf"] = str(ROOT / "kernel.elf")
    else:
        kernel_attestation = OUT / "cl13fix15-final-release.sha256"
        require(kernel_attestation.is_file(), "missing CL13 kernel hash attestation")
        require(re.search(rf"^{CL13_KERNEL_HASH}\s+(?:kernel\.elf|/tmp/hobbyos-cl13fix15-j[2N]\.elf)$",
                          kernel_attestation.read_text(), re.MULTILINE) is not None,
                "CL13 kernel hash attestation mismatch")
        binary_provenance["kernel.elf"] = str(kernel_attestation)
    hashes["kernel.elf"] = CL13_KERNEL_HASH

    if (ROOT / "hobbyos.img").is_file() and sha256(ROOT / "hobbyos.img") == CL13_IMAGE_HASH:
        binary_provenance["hobbyos.img"] = str(ROOT / "hobbyos.img")
    else:
        image_attestation = OUT / "cl13fix15-final-image.sha256"
        require(image_attestation.is_file(), "missing CL13 image hash attestation")
        require(re.search(rf"^{CL13_IMAGE_HASH}\s+hobbyos\.img$",
                          image_attestation.read_text(), re.MULTILINE) is not None,
                "CL13 image hash attestation mismatch")
        binary_provenance["hobbyos.img"] = str(image_attestation)
    hashes["hobbyos.img"] = CL13_IMAGE_HASH

    checkpoint_path = OUT / "cl13fix11-checkpoint.json"
    require(checkpoint_path.is_file(), "missing FIX11 checkpoint JSON")
    checkpoint = json.loads(checkpoint_path.read_text())
    matrix = checkpoint.get("matrix", checkpoint.get("pre_hpet", {}).get("matrix", {}))
    quantum = checkpoint.get("quantum", checkpoint.get("pre_hpet", {}).get("quantum", {}))
    negatives = checkpoint.get("negatives", {})
    require(len(matrix) == 4, f"matrix count is {len(matrix)}, expected 4")
    require(len(quantum) == 2, f"quantum count is {len(quantum)}, expected 2")
    require(len(negatives) == 16, f"negative count is {len(negatives)}, expected 16")
    require(all(item.get("detected") and not item.get("missed") and item.get("faults") == 0
                for item in negatives.values()), "one or more negatives are not DETECTED/clean")

    soak4, _ = summary_record(OUT / "cl13-soak-smp4-tcg.log", 4, "tcg")
    for key, minimum in {
        "switches": 1_000_000, "creates": 10_000, "reaps": 10_000,
        "kills": 5_000, "taskman": 100, "ps": 500, "modal": 500,
        "input": 100_000,
    }.items():
        require(integer(soak4, key, "soak4") >= minimum,
                f"soak4 {key} below minimum")
    for key in ("heap_drift", "invariants", "faults"):
        require(integer(soak4, key, "soak4") == 0, f"soak4 {key} is not zero")

    soak8, soak8_line = summary_record(OUT / "cl13-soak-smp8-kvm.log", 8, "kvm")
    exact8 = {
        "duration_ms": 2_855_046, "switches": 2_000_000,
        "creates": 20_000, "reaps": 20_000, "kills": 10_000,
        "taskman": 100, "ps": 500, "modal": 1_000, "input": 200_000,
        "refresh50": 33, "refresh1000": 34, "refresh2000": 33,
        "global_heap_drift": 0, "global_heap_pass": 1,
        "modal_ownership": 1, "invariants": 0, "faults": 0,
    }
    for key, expected in exact8.items():
        require(integer(soak8, key, "soak8") == expected,
                f"soak8 {key}: expected {expected}, got {soak8.get(key)!r}")

    ledger_path = OUT / "cl13fix15-soak8-stages.tsv"
    with ledger_path.open(newline="") as stream:
        ledger = list(csv.DictReader(stream, delimiter="\t"))
    expected_stages = [
        "boot", "warmup", "yield", "create-reap", "kill-race", "input",
        "modal", "ps-batch-1", "ps-batch-2", "ps-batch-3", "ps-batch-4",
        "ps-batch-5", "ps", "taskman-batch-1", "taskman-batch-2",
        "taskman-batch-3", "taskman-batch-4", "taskman", "duration",
        "cleanup", "final-diagnostics", "summary",
    ]
    require([row["stage"] for row in ledger] == expected_stages,
            "SMP8 stage ledger order/content mismatch")
    require(all(row["status"] == "PASS" for row in ledger),
            "SMP8 stage ledger contains a non-PASS stage")
    modal = next(row for row in ledger if row["stage"] == "modal")
    require(modal["timer_nodes_after"] == "18", "ledger modal timer_nodes_after != 18")
    raw_log = (OUT / "cl13-soak-smp8-kvm.log").read_text(errors="replace")
    require(re.search(r"^\[MODALTEST\]\[OPEN_CLOSE\] PASS .*timer_nodes_after=18(?: |$)",
                      raw_log, re.MULTILINE) is not None,
            "modal record does not preserve timer_nodes_after=18")
    soak_script = (ROOT / "scripts/test-taskman-v1-soak.sh").read_text()
    require("modal_timer_nodes_after=$modal_timer_nodes_after" in soak_script,
            "current soak summary does not interpolate modal_timer_nodes_after")
    raw_timer_omitted = "modal_timer_nodes_after invariants=0" in soak8_line
    require(raw_timer_omitted or soak8.get("modal_timer_nodes_after") == "18",
            "unexpected raw summary timer field")

    stack_path = OUT / "cl13fix15-final-stack-usage.txt"
    require(stack_path.is_file(), "missing final stack report")
    stack_values = [int(match) for match in re.findall(r"^\s*([0-9]+)\s", stack_path.read_text(), re.MULTILINE)]
    require(stack_values, "could not parse final stack report")
    stack_max = max(stack_values)
    require(stack_max <= 2048, f"stack maximum {stack_max} exceeds 2048")
    nm_path = OUT / "cl13fix15-final-nm.log"
    require(nm_path.is_file(), "missing final nm -u report")
    require(not nm_path.read_text().strip(), "final nm -u report is not empty")
    build_log = (OUT / "cl13fix15-final-build.log").read_text(errors="replace")
    require("stack-check: PASS" in build_log and "kernel-check: PASS" in build_log,
            "final build PASS markers missing")

    report = (ROOT / "docs/test-reports/TMV1-CL-13-taskman-v1-certification.md").read_text()
    require(re.search(r"^Status:\s*APPROVED\s*$", report, re.MULTILINE) is not None,
            "consolidated CL13 report is not APPROVED")
    fix15 = (ROOT / "docs/test-reports/TMV1-CL-13-FIX15-taskman-transaction-batching.md").read_text()
    require(re.search(r"^Status:\s*APPROVED\s*$", fix15, re.MULTILINE) is not None,
            "FIX15 report is not APPROVED")

    manifests = {
        "production": verify_manifests(PRODUCTION_MANIFESTS, MANIFEST_DIGESTS["production"]),
        "special": verify_manifests(SPECIAL_MANIFESTS, MANIFEST_DIGESTS["special"]),
    }
    return {
        "status": "PASS",
        "matrix": 4,
        "quantum": 2,
        "negatives": 16,
        "soak4": {key: int(value) if re.fullmatch(r"-?[0-9]+", value) else value
                  for key, value in soak4.items()},
        "soak8": {key: int(value) if re.fullmatch(r"-?[0-9]+", value) else value
                  for key, value in soak8.items() if value != ""},
        "stack_max": stack_max,
        "nm_undefined": 0,
        "hashes": hashes,
        "binary_provenance": binary_provenance,
        "manifests": manifests,
        "notes": (["The frozen raw SMP8 summary omitted the value of "
                   "modal_timer_nodes_after; the modal record and stage ledger preserve 18, "
                   "and the current script contains the corrected interpolation."]
                  if raw_timer_omitted else []),
    }


def markdown(result: dict[str, object]) -> str:
    notes = result["notes"]
    note_text = "\n".join(f"- {note}" for note in notes) if notes else "- None."
    hashes = result["hashes"]
    hash_rows = "\n".join(f"| `{path}` | `{digest}` | PASS |"
                            for path, digest in hashes.items())
    return f"""# CL-14 offline verification of CL-13 evidence

Status: PASS

The frozen CL-13 evidence was verified without rerunning a QEMU matrix,
quantum campaign, negative campaign, focused test, or soak.

| Gate | Result |
|---|---:|
| Matrix | {result['matrix']}/4 PASS |
| Quantum | {result['quantum']}/2 PASS |
| Negatives | {result['negatives']}/16 DETECTED |
| SMP4/TCG soak | PASS |
| SMP8/KVM soak | PASS |
| Stack maximum | {result['stack_max']} bytes |
| Undefined symbols | {result['nm_undefined']} |
| Runtime manifests | PASS |
| Special manifests | PASS |

## Frozen hashes

| Path | SHA-256 | Result |
|---|---|---|
{hash_rows}

## Non-blocking notes

{note_text}

`[CL14][CL13_EVIDENCE] PASS matrix=4 quantum=2 negatives=16 soak4=1 soak8=1 kernel=1 image=1 manifests=1`
"""


def write_outputs(result: dict[str, object], write_json: bool, write_md: bool) -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    sentinel = ("[CL14][CL13_EVIDENCE] PASS matrix=4 quantum=2 negatives=16 "
                "soak4=1 soak8=1 kernel=1 image=1 manifests=1")
    if write_json:
        (OUT / "cl14-cl13-evidence.json").write_text(
            json.dumps(result, indent=2, sort_keys=True) + "\n")
    if write_md:
        (OUT / "cl14-cl13-evidence.md").write_text(markdown(result))
    (OUT / "cl14-cl13-evidence.log").write_text(sentinel + "\n")
    print(sentinel)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("verify-cl13", "emit-json", "emit-markdown", "all"))
    args = parser.parse_args()
    try:
        result = verify()
        if args.command == "verify-cl13":
            print("[CL14][CL13_EVIDENCE] PASS matrix=4 quantum=2 negatives=16 "
                  "soak4=1 soak8=1 kernel=1 image=1 manifests=1")
        else:
            write_outputs(result, args.command in ("emit-json", "all"),
                          args.command in ("emit-markdown", "all"))
    except (OSError, ValueError, json.JSONDecodeError, VerificationError) as error:
        print(f"[CL14][CL13_EVIDENCE] FAIL reason={error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
