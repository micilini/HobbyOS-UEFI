#!/usr/bin/env python3
"""Offline verification for modal memory-checkpoint host evidence."""

import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path, PurePosixPath


HOST_PROFILES = {
    "normal": "none",
    "ubsan": "undefined",
    "asan": "address",
}
HOST_CASES = (
    "target-lifetime-with-raw-global-delta",
    "initial-timeout-zero",
    "initial-success-at-deadline",
    "dirty-worker-initial-state",
    "track-invalid-generation",
    "progress-timeout",
    "retained-64",
    "identity-net-zero",
    "async-drain-at-deadline",
    "async-drain-timeout",
    "record-capacity",
    "maximum-record-fit",
)
CHECKPOINT_NEGATIVES = {
    "checkpoint-track-resource-missing",
    "checkpoint-worker-without-progress",
    "checkpoint-final-generation-changed",
    "checkpoint-net-zero-retention-promoted",
    "checkpoint-global-delta-inconsistent",
    "checkpoint-global-comparability-invented",
    "checkpoint-worker-activity-missing",
    "checkpoint-final-resource-duplicated",
    "checkpoint-track-resource-fragmented",
    "checkpoint-async-drain-promoted",
}


class EvidenceError(Exception):
    pass


def safe_relative(value, label):
    if not isinstance(value, str) or not value:
        raise EvidenceError(f"{label} path is invalid")
    path = PurePosixPath(value)
    if path.is_absolute() or ".." in path.parts or "." in path.parts:
        raise EvidenceError(f"{label} path is unsafe")
    return Path(*path.parts)


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_json(path, label):
    try:
        value = json.loads(path.read_text())
    except OSError as error:
        raise EvidenceError(f"{label} is missing: {error}") from error
    except json.JSONDecodeError as error:
        raise EvidenceError(f"{label} is invalid JSON: {error}") from error
    if not isinstance(value, dict):
        raise EvidenceError(f"{label} is not an object")
    return value


def read_env(path, label):
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise EvidenceError(f"{label} is missing: {error}") from error
    values = {}
    for line in lines:
        if not line or "=" not in line:
            continue
        key, value = line.split("=", 1)
        values[key] = value
    return values


def verify_receipt(root, receipt, label, errors):
    if not isinstance(receipt, dict):
        errors.append(f"{label} receipt is invalid")
        return None
    try:
        relative = safe_relative(receipt.get("path"), label)
    except EvidenceError as error:
        errors.append(str(error))
        return None
    path = root / relative
    if not path.is_file() or path.is_symlink():
        errors.append(f"{label} file is missing or not regular")
        return None
    if receipt.get("bytes") != path.stat().st_size:
        errors.append(f"{label} byte count differs")
    if receipt.get("sha256") != sha256(path):
        errors.append(f"{label} hash differs")
    return path


def verify_host(root, result, errors):
    profiles = result.get("host_profiles")
    if not isinstance(profiles, dict) or set(profiles) != set(HOST_PROFILES):
        errors.append("host profile matrix differs")
        return
    for name, sanitizer in HOST_PROFILES.items():
        row = profiles.get(name)
        label = f"host {name}"
        if not isinstance(row, dict) or row.get("sanitizer") != sanitizer:
            errors.append(f"{label} configuration differs")
            continue
        if row.get("compile_exit_code") != 0 or row.get("run_exit_code") != 0:
            errors.append(f"{label} did not compile and run successfully")
        log = verify_receipt(root, row.get("run_log"), f"{label} log", errors)
        verify_receipt(root, row.get("compile_log"),
                       f"{label} compile log", errors)
        if log is None:
            continue
        lines = log.read_text(errors="replace").splitlines()
        begin = [line for line in lines if line.startswith(
            "[MODAL_CHECKPOINT_HOST][SUITE_BEGIN] ")]
        end = [line for line in lines if line.startswith(
            "[MODAL_CHECKPOINT_HOST][SUITE_END] ")]
        cases = re.findall(
            r"^\[MODAL_CHECKPOINT_HOST\]\[CASE\] id=([^ ]+) status=([^ ]+)$",
            "\n".join(lines), re.MULTILINE)
        if len(begin) != 1 or "cases=12 capacity=1536" not in begin[0]:
            errors.append(f"{label} suite begin differs")
        if (len(end) != 1 or
                "status=PASS cases=12 assertions=73 failures=0 capacity=1536" not in
                end[0]):
            errors.append(f"{label} suite result differs")
        if tuple(item[0] for item in cases) != HOST_CASES or \
                any(status != "PASS" for _, status in cases):
            errors.append(f"{label} case coverage differs")


def verify_fixtures(root, result, errors):
    receipt = result.get("format_fixture_result")
    path = verify_receipt(root, receipt, "format fixture result", errors)
    if path is None:
        return
    value = read_json(path, "format fixture result")
    records = value.get("records")
    if (value.get("status") != "PASS" or value.get("valid_controls") != 1 or
            not isinstance(records, list)):
        errors.append("format fixture summary differs")
        return
    by_name = {row.get("name"): row for row in records
               if isinstance(row, dict) and isinstance(row.get("name"), str)}
    if by_name.get("valid", {}).get("status") != "PASS":
        errors.append("format fixture valid control was not accepted")
    missing = CHECKPOINT_NEGATIVES - set(by_name)
    if missing:
        errors.append("checkpoint negative fixtures are missing: " +
                      ", ".join(sorted(missing)))
    for name in CHECKPOINT_NEGATIVES & set(by_name):
        if by_name[name].get("status") != "NEGATIVE_DETECTED":
            errors.append(f"checkpoint negative was not detected: {name}")


def verify_provenance(root, result, source_paths, audit, errors):
    base = result.get("base_commit")
    head = result.get("candidate_head")
    tree = result.get("candidate_tree")
    commit_re = re.compile(r"[0-9a-f]{40}")
    if not all(isinstance(value, str) and commit_re.fullmatch(value)
               for value in (base, head, tree)):
        errors.append("candidate provenance identity is invalid")
        return
    environment = read_env(
        root / "preflight/environment.env", "preflight environment")
    expected = {
        "implementation_base_commit": base,
        "head": head,
        "tree": tree,
    }
    if any(environment.get(name) != value
           for name, value in expected.items()):
        errors.append("candidate provenance differs from preflight")
    if not isinstance(audit, dict) or audit.get("base_commit") != base:
        errors.append("source audit base commit differs")

    resolved_tree = subprocess.run(
        ["git", "rev-parse", f"{head}^{{tree}}"], check=False,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if resolved_tree.returncode != 0 or resolved_tree.stdout.strip() != tree:
        errors.append("candidate commit/tree relation differs")
        return
    ancestor = subprocess.run(
        ["git", "merge-base", "--is-ancestor", base, head], check=False,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if ancestor.returncode != 0:
        errors.append("implementation base is not an ancestor of candidate")

    for name, path in source_paths.items():
        blob = subprocess.run(
            ["git", "show", f"{head}:{name}"], check=False,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if blob.returncode != 0 or blob.stdout != path.read_bytes():
            errors.append(f"source evidence differs from candidate: {name}")


def verify_all(root):
    errors = []
    result = read_json(root / "result.json", "modal checkpoint result")
    if result.get("schema") != 1 or \
            result.get("kind") != "modal-memory-checkpoint-evidence":
        errors.append("modal checkpoint result schema differs")
    if result.get("status") != "PASS":
        errors.append("modal checkpoint result is not PASS")
    sources = result.get("sources")
    expected_sources = {
        "kernel/src/shell/commands/cmd_taskmantest.c",
        "kernel/src/shell/commands/cmd_taskmantest.h",
        "scripts/modal-checkpoints-host.c",
        "scripts/test-modal-checkpoints.sh",
        "scripts/verify-modal-checkpoint-evidence.py",
        "scripts/test-libc-format.sh",
        "scripts/verify-libc-format-evidence.py",
    }
    source_paths = {}
    if not isinstance(sources, dict) or set(sources) != expected_sources:
        errors.append("source receipt set differs")
    else:
        for name, receipt in sources.items():
            path = verify_receipt(root, receipt, f"source {name}", errors)
            if path is not None:
                source_paths[name] = path
    audit_path = verify_receipt(root, result.get("source_audit"),
                                "source audit", errors)
    audit = None
    if audit_path is not None:
        audit = read_json(audit_path, "source audit")
        checks = audit.get("checks")
        if not isinstance(checks, dict) or not checks or \
                any(item is not True for item in checks.values()):
            errors.append("source audit did not pass every check")
    if set(source_paths) == expected_sources:
        verify_provenance(root, result, source_paths, audit, errors)
    verify_host(root, result, errors)
    verify_fixtures(root, result, errors)
    historical = result.get("historical")
    if not isinstance(historical, dict) or historical != {
        "allocation_identity": "NOT_DETERMINED",
        "endpoint_comparability": "NOT_ESTABLISHED",
        "raw_result": "FAIL_PLUS_64_PRESERVED",
    }:
        errors.append("historical classification differs")
    return errors


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("all",))
    parser.add_argument("evidence", type=Path)
    args = parser.parse_args()
    try:
        errors = verify_all(args.evidence)
    except EvidenceError as error:
        print(f"MODAL_CHECKPOINT_EVIDENCE_ERROR: {error}", file=sys.stderr)
        return 1
    if errors:
        for error in errors:
            print(f"MODAL_CHECKPOINT_EVIDENCE_REJECTED: {error}", file=sys.stderr)
        return 1
    print("MODAL_CHECKPOINT_EVIDENCE: PASS host=3 cases=12 assertions=73 "
          "checkpoint_negatives=10")
    return 0


if __name__ == "__main__":
    sys.exit(main())
