# TASKMAN V1 release notes

Status: CLOSED

## Highlights

TASKMAN V1 provides a modal, paginated kernel-task view plus the `ps`, `kill`,
and `taskdiag` command family. It closes the scheduler/lifecycle, input/modal,
accounting, and observability contracts needed for reliable inspection.

## Architecture and hardening

The release uses generation-bound task handles, exact per-CPU switch handoff,
prepare/commit/abort waits, timer references, exactly-once cleanup and
lifecycle notification, hard-mask reaping, copy-only snapshots, independent
CPU samplers, and transactional modal ownership with owner-death recovery.

## Commands

Use `ps [page] [page_size]`, `kill <pid>`, `taskman [refresh_ms]`, and
`taskdiag`. TASKMAN supports wide/compact layouts, dynamic pagination, keyboard
selection, and ESC-only exit. See the [command reference](../taskman-v1-command-reference.md).

## Testing

Certification includes static/reproducible builds, SMP TCG/KVM matrices, two
quantum variants, 16 detected negative models with reset canaries, focused
lifecycle/input/modal/UI campaigns, and full SMP4/TCG and SMP8/KVM soaks.

## Known limits and developer notes

This is a Ring-0, single-address-space hobby kernel. It is not POSIX, does not
provide userspace or general-purpose production/hardware certification, and
does not claim bare-metal acceptance. Affinity and user labels are presentation
constants; memory is an estimate; kill is cooperative. Full limits are
[documented](../taskman-v1-known-limitations.md).

Test builds add autorun, framed transport, and `tasktest`; release builds do
not. Guest-side batching used in certification reduced HMP round-trips without
reducing work. Future CPU affinity, memory ownership, process, and topology
work requires a separately approved V2 roadmap.
