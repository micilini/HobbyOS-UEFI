# TASKMAN V1 file inventory

Status: COMPLETE

This inventory is derived from `git diff --name-status 4e700a9b38653fb57ecb0f107a18f87508db0736..HEAD` plus the CL-14 release tree. Generated artifacts under `artifacts/`, QEMU state, binaries, objects, and the untracked roadmap are intentionally excluded.

## scheduler/lifecycle

| Change | File |
|---|---|
| `M kernel/kernel.c` | modified |
| `M kernel/src/core/dpc.c` | modified |
| `M kernel/src/core/dpc.h` | modified |
| `M kernel/src/core/kernel_init.c` | modified |
| `A kernel/src/core/runtime_ready.c` | added |
| `A kernel/src/core/runtime_ready.h` | added |
| `M kernel/src/core/scheduler.c` | modified |
| `M kernel/src/shell/shell.c` | modified |
| `M kernel/src/shell/shell.h` | modified |

## sync/timers

| Change | File |
|---|---|
| `M kernel/src/core/semaphore.c` | modified |
| `M kernel/src/core/semaphore.h` | modified |
| `M kernel/src/shell/commands/cmd_synctest.c` | modified |
| `M kernel/src/shell/commands/cmd_synctest.h` | modified |

## accounting

| Change | File |
|---|---|
| `M kernel/src/shell/commands/cmd_accounttest.c` | modified |
| `M kernel/src/shell/commands/cmd_accounttest.h` | modified |

## kill/reaper

| Change | File |
|---|---|

## input/modal

| Change | File |
|---|---|
| `M kernel/src/core/modal_session.h` | modified |
| `M kernel/src/drivers/usb/xhci/usb_hotplug.c` | modified |
| `M kernel/src/drivers/usb/xhci/usb_hotplug.h` | modified |
| `M kernel/src/drivers/usb/xhci/xhci.c` | modified |
| `M kernel/src/shell/commands/cmd_modaltest.c` | modified |
| `M kernel/src/shell/commands/cmd_modaltest.h` | modified |

## UI/commands

| Change | File |
|---|---|
| `M kernel/src/shell/commands/cmd_schedtest.c` | modified |
| `M kernel/src/shell/commands/cmd_schedtest.h` | modified |
| `M kernel/src/shell/commands/cmd_taskmantest.c` | modified |
| `M kernel/src/shell/commands/cmd_taskmantest.h` | modified |
| `A kernel/src/shell/commands/cmd_tasktest.c` | added |
| `A kernel/src/shell/commands/cmd_tasktest.h` | added |
| `M kernel/src/shell/commands/registry.c` | modified |

## selftest/harness

| Change | File |
|---|---|
| `M GENTS.md` | modified |
| `A kernel/src/core/selftest.c` | added |
| `A kernel/src/core/selftest.h` | added |
| `M makefile` | modified |
| `A scripts/cl13-evidence-checkpoint.py` | added |
| `A scripts/cl13-modal-checkpoint.py` | added |
| `A scripts/cl13-modal-ownership-checkpoint.py` | added |
| `A scripts/cl13-soak-checkpoint.py` | added |
| `A scripts/cl13-taskman-soak-checkpoint.py` | added |
| `M scripts/harness-common.sh` | modified |
| `A scripts/harness-framed.sh` | added |
| `A scripts/harness-runtime-ready.sh` | added |
| `A scripts/harness-selftest.sh` | added |
| `A scripts/package-taskman-v1-release.sh` | added |
| `A scripts/parse-selftest-log.py` | added |
| `A scripts/run-qemu-selftest.sh` | added |
| `A scripts/test-cl13-async-contract-focused.sh` | added |
| `A scripts/test-cl13-boundary-focused.sh` | added |
| `A scripts/test-cl13-clock-contract-focused.sh` | added |
| `A scripts/test-cl13-clock-sleep-contract-focused.sh` | added |
| `A scripts/test-cl13-fix11-resume.sh` | added |
| `A scripts/test-cl13-fix12-resume.sh` | added |
| `A scripts/test-cl13-fix13-resume.sh` | added |
| `A scripts/test-cl13-fix14-resume.sh` | added |
| `A scripts/test-cl13-fix15-resume.sh` | added |
| `A scripts/test-cl13-fixture257-focused.sh` | added |
| `A scripts/test-cl13-harness-record-focused.sh` | added |
| `A scripts/test-cl13-modal-heap-focused.sh` | added |
| `A scripts/test-cl13-modal-ownership-focused.sh` | added |
| `A scripts/test-cl13-runtime-ready-focused.sh` | added |
| `A scripts/test-cl13-selftest-line-focused.sh` | added |
| `A scripts/test-cl13-selftest-record-focused.sh` | added |
| `A scripts/test-cl13-status-oracle-focused.sh` | added |
| `A scripts/test-cl13-synctest-async-focused.sh` | added |
| `A scripts/test-cl13-taskman-anchor-focused.sh` | added |
| `A scripts/test-cl13-taskman-transaction-focused.sh` | added |
| `A scripts/test-cl13-transport-focused.sh` | added |
| `A scripts/test-cl14-release.sh` | added |
| `A scripts/test-taskman-v1-negatives.sh` | added |
| `A scripts/test-taskman-v1-soak.sh` | added |
| `A scripts/test-taskman-v1.sh` | added |
| `A scripts/verify-taskman-v1-docs.py` | added |
| `A scripts/verify-taskman-v1-evidence.py` | added |

## documentation

| Change | File |
|---|---|
| `M AGENTS.md` | modified |
| `M docs/development/build-and-qemu.md` | modified |
| `A docs/input-modal-architecture.md` | added |
| `A docs/releases/TASKMAN_V1_RELEASE_MANIFEST.md` | added |
| `A docs/releases/TASKMAN_V1_RELEASE_NOTES.md` | added |
| `A docs/task-lifecycle.md` | added |
| `A docs/taskman-v1-architecture.md` | added |
| `A docs/taskman-v1-command-reference.md` | added |
| `A docs/taskman-v1-contract.md` | added |
| `A docs/taskman-v1-file-inventory.md` | added |
| `A docs/taskman-v1-handoff-checklist.md` | added |
| `A docs/taskman-v1-homologation.md` | added |
| `A docs/taskman-v1-known-limitations.md` | added |
| `A docs/taskman-v1-test-plan.md` | added |
| `A docs/taskman-v1-user-guide.md` | added |
| `A docs/taskman-v2-entry-criteria.md` | added |
| `A docs/test-reports/TMV1-CL-13-FIX1-boundary-harness.md` | added |
| `A docs/test-reports/TMV1-CL-13-FIX12-soak-status-oracle.md` | added |
| `A docs/test-reports/TMV1-CL-13-FIX14-modal-ownership-vs-global-heap.md` | added |
| `A docs/test-reports/TMV1-CL-13-FIX15-taskman-transaction-batching.md` | added |
| `A docs/test-reports/TMV1-CL-13-FIX2-taskman-anchor.md` | added |
| `A docs/test-reports/TMV1-CL-13-FIX3-transactional-transport.md` | added |
| `A docs/test-reports/TMV1-CL-13-FIX4-fixture-257.md` | added |
| `A docs/test-reports/TMV1-CL-13-FIX5-async-command-contract.md` | added |
| `A docs/test-reports/TMV1-CL-13-FIX6-selftest-record-framing.md` | added |
| `A docs/test-reports/TMV1-CL-13-FIX7-selftest-line-boundary.md` | added |
| `A docs/test-reports/TMV1-CL-13-FIX8-runtime-ready-harness-records.md` | added |
| `A docs/test-reports/TMV1-CL-13-taskman-v1-certification.md` | added |
| `A docs/test-reports/TMV1-CL-13-visual-checklist.md` | added |
| `M README.md` | modified |

The CL-14 staged-tree checker compares this list with the committed CL-13 diff and the candidate CL-14 files before commit.
