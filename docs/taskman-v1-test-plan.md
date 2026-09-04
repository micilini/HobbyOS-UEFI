# TASKMAN V1 test plan

Status: CERTIFIED

The plan is organized by guarantee. Frozen historical evidence remains
available for provenance, while active visual validation uses semantic names.

## Guarantee map

| Area | Contract | Principal gate |
|---|---|---|
| Build/static | Stack budget, successful kernel/image build, no undefined symbols | `make stack-check`, `make kernel-check`, `make deps-check`, `make image`, `nm -u` |
| Identity | Unique monotonic ID and generation-bound handles | `taskdiag check` |
| Scheduler | Runqueue, on-CPU, and handoff invariants | scheduler diagnostics |
| Wait/sync | No lost pre-block wake and exact cancellation | synchronization diagnostics |
| Accounting | Monotonic time and windowed CPU samples | accounting diagnostics |
| Kill | Cooperative results and protected/non-killable behavior | kill diagnostics |
| Lifecycle | Cleanup/notification once and safe reaping | reaper diagnostics |
| Input | Atomic route/enqueue and no leaked modal input | `inputtest check` |
| Modal | Token ownership, recovery, and completion once | `modaltest check` |
| TASKMAN model | Dynamic capture, PID ordering, pagination, and ESC-only | `taskmantest check` |
| Visual layout | Shared schema, twelve boundaries, native palette | `scripts/test-taskman-visual.sh focused` |
| Redraw | Differential glyph/style present and zero stable clear | focused visual smoke |
| Active naming | No roadmap labels in active interfaces | `scripts/test-taskman-visual.sh naming` |

## Visual coverage

The visual suite covers widths 71, 76, 90, 118, 160, and 512; WIDE and
COMPACT frames; maximum values; split lines; selection style; identical,
style-only, shorter, and geometry differential presentation; semantic
boundaries; and cleanup ownership.

The final smoke uses one SMP=4 KVM boot and exactly two TASKMAN sessions: WIDE
at 118x40 and COMPACT at 76x40. Each session must produce a full frame, zero
fallback frames, zero stable clears, zero stale cells, zero clipping, zero
modal scroll, and zero workspace residual. Input, modal, taskdiag, and taskman
checks run in the same boot.

## Binary continuity

Nomenclature-only changes must preserve the candidate kernel and image hashes.
A hash difference is a hard failure, even if an explanation appears plausible.
Historical long-duration campaigns are not rerun when protected runtime source
and candidate bytes remain identical.

## Human review

Automated mechanism proof does not establish perceived legibility or flicker.
The canonical human checklist remains unmarked until the user exercises
`taskman 1000` in WIDE and COMPACT and validates selection, pagination, and ESC.
