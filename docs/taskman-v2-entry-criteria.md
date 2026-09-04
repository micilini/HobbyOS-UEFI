# TASKMAN V2 entry criteria

Status: AUTHORIZED_NOT_STARTED

TASKMAN V2 ENTRY: AUTHORIZED_NOT_STARTED

| Criterion | Status | Evidence | Owner / next action |
|---|---|---|---|
| V1 public contract complete | PASS | `taskman-v1-contract.md` | Maintainer preserves contract |
| Architecture/lifecycle ownership documented | PASS | architecture/lifecycle/input pages | Review before design changes |
| Public commands and metrics documented | PASS | user guide/command reference | Keep source and docs synchronized |
| Known V1 limits explicit | PASS | known-limitations page | Do not relabel limits as defects |
| Consolidated evidence verified | PASS | CL-14 offline verifier | Revalidate hashes when moving artifacts |
| Open P0 | PASS | count 0 in homologation | Triage any newly found defect separately |
| Open P1 | PASS | count 0 in homologation | Triage any newly found defect separately |
| Reproducible release/package | PASS | CL-14 build and handoff package | Archive receipt and ZIP |
| Human review before V2 work | PASS | required next boundary | Obtain explicit approval for V2 roadmap |

## Permitted future backlogs

### V2-CPU

`cpu_affinity_mask`, an affinity API, scheduler enforcement, and a real
affinity column. This is not a hidden V1 correction, does not invalidate the
V1 `AFF=Any` presentation contract, and needs its own roadmap.

### V2-MEM

Heap owner/tag, stack/heap/shared split, real per-task memory, and cleanup
ownership. This is not a hidden V1 correction, does not invalidate the V1
`MEM~` estimate, and needs its own roadmap.

### Process future

`process_t`, PID/TID separation, user identity, Ring 3, syscalls, and per-process
CR3. This is not a hidden V1 correction, does not invalidate the kernel-task
contract, and needs its own roadmap.

### Topology scheduler

Last-CPU preference, SMT sibling penalty, package/core/thread topology, P/E
core treatment, and NUMA/SRAT. This is not a hidden V1 correction, does not
invalidate the V1 class policy, and needs its own roadmap.

No item above is implemented by CL-14.
