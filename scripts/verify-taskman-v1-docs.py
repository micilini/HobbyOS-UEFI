#!/usr/bin/env python3
"""Static, offline consistency checks for the TASKMAN V1 documentation."""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import unicodedata
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "artifacts/build"
BASE = "4e700a9b38653fb57ecb0f107a18f87508db0736"
REQUIRED = [
    "docs/taskman-v1-contract.md",
    "docs/taskman-v1-architecture.md",
    "docs/task-lifecycle.md",
    "docs/input-modal-architecture.md",
    "docs/taskman-v1-user-guide.md",
    "docs/taskman-v1-command-reference.md",
    "docs/taskman-v1-known-limitations.md",
    "docs/taskman-v1-test-plan.md",
    "docs/taskman-v1-homologation.md",
    "docs/taskman-v1-file-inventory.md",
    "docs/taskman-v1-handoff-checklist.md",
    "docs/taskman-v2-entry-criteria.md",
    "docs/releases/TASKMAN_V1_RELEASE_NOTES.md",
    "docs/releases/TASKMAN_V1_RELEASE_MANIFEST.md",
]


class CheckError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise CheckError(message)


def run_git(*args: str) -> str:
    result = subprocess.run(["git", *args], cwd=ROOT, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    require(result.returncode == 0, f"git {' '.join(args)}: {result.stderr.strip()}")
    return result.stdout


def outside_fences(text: str) -> str:
    output: list[str] = []
    fenced = False
    for line in text.splitlines():
        if line.lstrip().startswith("```"):
            fenced = not fenced
            continue
        if not fenced:
            output.append(line)
    return "\n".join(output)


def check_files() -> dict[str, object]:
    for relative in REQUIRED:
        path = ROOT / relative
        require(path.is_file(), f"missing required document: {relative}")
        text = path.read_text()
        h1 = re.findall(r"^# [^#].+$", text, re.MULTILINE)
        require(len(h1) == 1, f"{relative}: expected one H1, found {len(h1)}")
        require(re.search(r"^Status:\s*\S", text, re.MULTILINE) is not None,
                f"{relative}: missing Status")
        visible = outside_fences(text)
        require(re.search(r"\b(?:TODO|TBD|FIXME)\b|\?\?\?", visible,
                          re.IGNORECASE) is None,
                f"{relative}: unresolved placeholder outside code fence")
    return {"required_files": len(REQUIRED), "status": "PASS"}


def slug(text: str) -> str:
    normalized = unicodedata.normalize("NFKD", text).encode("ascii", "ignore").decode()
    normalized = normalized.strip().lower()
    normalized = re.sub(r"[^a-z0-9 _-]", "", normalized)
    return re.sub(r"[ _]+", "-", normalized)


def check_links() -> dict[str, object]:
    checked = 0
    for relative in ["README.md", *REQUIRED]:
        path = ROOT / relative
        text = path.read_text()
        headings = {slug(match.group(1)) for match in re.finditer(r"^#{1,6}\s+(.+)$", text, re.MULTILINE)}
        for match in re.finditer(r"(?<!!)\[[^\]]+\]\(([^)]+)\)", text):
            target = match.group(1).strip()
            if target.startswith(("http://", "https://", "mailto:")):
                continue
            file_part, separator, anchor = target.partition("#")
            resolved = path if not file_part else (path.parent / file_part).resolve()
            require(resolved.exists(), f"{relative}: broken link {target}")
            if anchor:
                target_text = resolved.read_text() if resolved.suffix.lower() == ".md" else ""
                target_headings = headings if resolved == path.resolve() else {
                    slug(item.group(1)) for item in re.finditer(r"^#{1,6}\s+(.+)$", target_text, re.MULTILINE)
                }
                require(anchor in target_headings, f"{relative}: missing anchor {target}")
            checked += 1
    return {"links": checked, "status": "PASS"}


def source(relative: str) -> str:
    path = ROOT / relative
    require(path.is_file(), f"missing source contract file: {relative}")
    return path.read_text()


def all_tokens(text: str, tokens: list[str], context: str) -> None:
    missing = [token for token in tokens if token not in text]
    require(not missing, f"{context}: missing {', '.join(missing)}")


def check_source_contracts() -> dict[str, object]:
    task_h = source("kernel/src/core/task.h")
    scheduler_h = source("kernel/src/core/scheduler.h")
    kill_h = source("kernel/src/shell/commands/cmd_kill.h")
    ps_c = source("kernel/src/shell/commands/cmd_ps.c")
    taskman_h = source("kernel/src/shell/commands/cmd_taskman.h")
    taskman_c = source("kernel/src/shell/commands/cmd_taskman.c")
    registry = source("kernel/src/shell/commands/registry.c")
    formatter = source("kernel/src/core/task_format.c") + source("kernel/src/core/task_format.h")
    docs = "\n".join((ROOT / item).read_text() for item in REQUIRED)

    states = ["TASK_READY", "TASK_RUNNING", "TASK_BLOCKED", "TASK_SLEEPING", "TASK_ZOMBIE"]
    reasons = ["TASK_EXIT_NONE", "TASK_EXIT_NORMAL", "TASK_EXIT_KILLED",
               "TASK_EXIT_INIT_FAILURE", "TASK_EXIT_INTERNAL_ERROR"]
    flags = ["TASK_FLAG_IDLE", "TASK_FLAG_SYSTEM", "TASK_FLAG_USER",
             "TASK_FLAG_KILL_PROTECTED", "TASK_FLAG_KILLABLE", "TASK_FLAG_MODAL_UI"]
    waits = ["TASK_WAIT_NONE", "TASK_WAIT_SEMAPHORE", "TASK_WAIT_TIMER_SLEEP",
             "TASK_WAIT_GENERIC", "TASK_WAIT_INPUT_QUEUE"]
    all_tokens(task_h, states + reasons + flags + waits, "task.h")
    all_tokens(docs, states + ["`NONE`", "`NORMAL`", "`KILLED`",
                               "`INIT_FAILURE`", "`INTERNAL_ERROR`"] +
               ["`IDLE`", "`SYSTEM`", "`USER`", "`KILL_PROTECTED`",
                "`KILLABLE`", "`MODAL_UI`"], "documentation enums")
    require("typedef uint64_t task_id_t" in task_h, "task_id_t is not uint64_t")
    require("TASK_ID_INVALID ((task_id_t)0)" in task_h, "task ID zero contract changed")
    all_tokens(task_h, ["INTERACTIVE_QUANTUM 5", "DEFAULT_QUANTUM 20"],
               "scheduler quantum")
    all_tokens(kill_h, [f"KILL_CLI_{name} = {value}" for name, value in (
        ("ACCEPTED", 0), ("ALREADY_PENDING", 2), ("ALREADY_ZOMBIE", 3),
        ("ALREADY_EXITING", 4), ("USAGE", 64), ("INVALID_PID", 65),
        ("PROTECTED", 66), ("NOT_KILLABLE", 67), ("NOT_FOUND", 68),
        ("INTERNAL_ERROR", 69))], "kill return codes")
    all_tokens(ps_c, ["PS_DEFAULT_PAGE_SIZE 32u", "PS_MAX_PAGE_SIZE 128u", "PS_SNAPSHOT_RETRIES 4u"],
               "PS limits")
    all_tokens(taskman_h, ["TASKMAN_DEFAULT_REFRESH_MS 1000u", "TASKMAN_MIN_REFRESH_MS 50u",
                           "TASKMAN_MAX_REFRESH_MS 2000u", "TASKMAN_V1_MAX_ENTRIES 4096u"],
               "TASKMAN limits")
    all_tokens(formatter, ["PROT", "PEND", "EXIT", "ZOMB", "MEM~", "%CPU"],
               "formatter columns")
    for name, aliases, usage in (
        ("ps", ["tasks", "tasklist"], "ps [page] [page_size]"),
        ("kill", ["terminate", "taskkill"], "kill <pid>"),
        ("taskman", ["tm", "top"], "taskman [refresh_ms]"),
        ("taskdiag", ["td", "tdiag"], "taskdiag [summary|task <pid>|scheduler|reaper|modal|input|accounting|all|check|selftest|trace <pid>|trace off|trace-status]"),
    ):
        all_tokens(registry, [f'.name = "{name}"', f'.usage = "{usage}"', *[f'"{alias}"' for alias in aliases]],
                   f"registry {name}")
        all_tokens(docs, [f"`{alias}`" for alias in aliases] + [f"`{usage}`"],
                   f"docs {name}")
    require("#ifdef HOBBYOS_SELFTEST" in registry and '.name = "tasktest"' in registry,
            "tasktest release guard missing")
    require("TASKMAN_LAYOUT_WIDE" in taskman_c and "TASKMAN_LAYOUT_COMPACT" in taskman_c,
            "TASKMAN layout contract missing")
    return {"states": len(states), "reasons": len(reasons), "flags": len(flags),
            "wait_kinds": len(waits), "commands": 4, "status": "PASS"}


def check_evidence() -> dict[str, object]:
    result = subprocess.run([sys.executable, "scripts/verify-taskman-v1-evidence.py", "verify-cl13"],
                            cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    require(result.returncode == 0, f"CL13 evidence failed: {result.stderr.strip()}")
    require("[CL14][CL13_EVIDENCE] PASS" in result.stdout, "CL13 PASS sentinel absent")
    return {"cl13": "PASS", "status": "PASS"}


def inventory_paths() -> set[str]:
    path = ROOT / "docs/taskman-v1-file-inventory.md"
    text = path.read_text()
    return set(re.findall(r"^\| `[AM]\s+([^`]+)` \|", text, re.MULTILINE))


def changed_paths() -> set[str]:
    result: set[str] = set()
    for line in run_git("diff", "--name-status", f"{BASE}..HEAD").splitlines():
        if line.strip():
            result.add(line.split("\t")[-1])
    for line in run_git("diff", "--cached", "--name-status").splitlines():
        if line.strip():
            result.add(line.split("\t")[-1])
    status = run_git("status", "--short", "--untracked-files=all").splitlines()
    for line in status:
        if not line or line.endswith("ROADMAP_TASKMAN_V1_CLOSURE_HARDENING.md"):
            continue
        candidate = line[3:]
        if (candidate.startswith(("artifacts/", "scripts/__pycache__/")) or
                candidate in {"kernel.elf", "hobbyos.img", "BOOTX64.EFI", "bootx64.so"}):
            continue
        result.add(candidate)
    return result


def check_v2_gate() -> dict[str, object]:
    text = (ROOT / "docs/taskman-v2-entry-criteria.md").read_text()
    require(re.search(r"^Status:\s*AUTHORIZED_NOT_STARTED\s*$", text, re.MULTILINE) is not None,
            "V2 status is not AUTHORIZED_NOT_STARTED")
    require("TASKMAN V2 ENTRY: AUTHORIZED_NOT_STARTED" in text, "formal V2 gate absent")
    rows = [line for line in text.splitlines() if line.startswith("|") and "---" not in line]
    criteria = [line for line in rows if "Criterion" not in line]
    require(criteria and all(re.search(r"\|\s*PASS\s*\|", line) for line in criteria),
            "one or more V1 criteria are not PASS")
    require(re.search(r"\bIMPLEMENTED\b", text) is None, "a V2 item is marked IMPLEMENTED")
    inventory = inventory_paths()
    changed = changed_paths()
    missing = sorted(changed - inventory)
    require(not missing, "file inventory missing: " + ", ".join(missing))
    return {"criteria": len(criteria), "inventory_paths": len(inventory), "status": "PASS"}


CHECKS = {
    "files": check_files,
    "links": check_links,
    "source-contracts": check_source_contracts,
    "evidence": check_evidence,
    "v2-gate": check_v2_gate,
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=(*CHECKS, "all"))
    args = parser.parse_args()
    selected = list(CHECKS) if args.command == "all" else [args.command]
    result: dict[str, object] = {"status": "PASS"}
    try:
        for name in selected:
            result[name] = CHECKS[name]()
        sentinel = "[CL14][DOCS] PASS " + " ".join(f"{name}=1" for name in selected)
        OUT.mkdir(parents=True, exist_ok=True)
        if args.command == "all":
            (OUT / "cl14-docs-verification.json").write_text(
                json.dumps(result, indent=2, sort_keys=True) + "\n")
            (OUT / "cl14-docs-verification.log").write_text(sentinel + "\n")
        print(sentinel)
    except (OSError, CheckError, json.JSONDecodeError) as error:
        print(f"[CL14][DOCS] FAIL check={args.command} reason={error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
