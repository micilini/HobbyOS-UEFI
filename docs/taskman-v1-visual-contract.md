# TASKMAN V1 visual contract

Status: AUTOMATED CERTIFICATION PASS — HUMAN REVIEW PENDING

This page defines the active TASKMAN V1 presentation contract. The
[public contract](taskman-v1-contract.md),
[architecture](taskman-v1-architecture.md), and
[known limitations](taskman-v1-known-limitations.md) remain authoritative for
behavior and semantics. Frozen release artifacts are historical evidence and
are never overwritten by a visual candidate.

## Geometry and table schema

WIDE begins at 118 columns. COMPACT begins at 71 columns and is selected at
the certified 76-column point. Seventy columns remains TOO_NARROW. TOO_SHORT
is a separate deterministic fallback.

One `taskman_table_schema_t` supplies every header and row offset, slot width,
content width, alignment, boundary, and NAME remainder. A row is an exact-width
canvas initialized with spaces. It reserves the selection column, writes
bounded fields, places ASCII `|` at group boundaries, and terminates at
`region_width`.

The thirteen semantic columns have twelve visible boundaries: seven reserved
internal gaps and five group separators.

| Group | Columns |
|---|---|
| Identity | PID, USER |
| Locality | CPU, LCPU, AFF |
| Scheduling | STATE, CLASS, Q |
| Accounting | TIME+, %CPU |
| Control | KILL, MEM~ |
| Identification | NAME |

PID, CPU, LCPU, Q, TIME+, %CPU, and MEM~ are right aligned. USER, AFF, STATE,
CLASS, KILL, and NAME are left aligned. Numeric overflow is never accepted
silently. Text and NAME truncation use an explicit marker. Content cannot
overwrite a gap or separator. COMPACT preserves the complete anomalous CPU
sample `!100.0%`.

## Screen hierarchy

The screen contains one title row, two summary rows, one header row, all
available task rows, and two footer rows. The title places `HobbyOS TASKMAN`
opposite mode, refresh, and page metadata. Summary rows use deterministic
left/right blocks for task totals, visible range, CPU count, uptime, and
capture status. `TRUNCATED` and `ALLOC FALLBACK` remain visible when true.

Footer row 1 identifies the selected PID/name and page/range. Footer row 2
shows ESC, selection, page, and Home/End controls. COMPACT may abbreviate but
never removes ESC or overlaps the split blocks.

## Native palette and selection

TASKMAN emits `ConsoleCell` values, never raw ANSI escapes. The short native
palette distinguishes title, metadata, header, separators, normal rows,
RUNNING, BLOCKED/SLEEPING, ZOMBIE, and kill/anomaly warnings. A selected row
uses its own background across the entire width plus the `>` marker. Status
foregrounds and separators remain legible inside selection.

## Differential presentation

`console_region_present_row()` compares glyph, foreground, and background
against console history while holding the console lock. It writes only changed
cells, sanitizes control whitespace, flushes at most once for a dirty row, and
never moves the cursor or scrolls. Fill cells erase the old tail of a shorter
row without a full clear.

A stable frame may not call `console_region_clear()`. The first frame presents
the complete desired region by diff. Cleanup clears once. A genuine geometry
transition may clear an incompatible old region and is accounted separately.
Refresh, selection, pagination, metric changes, churn, ZOMBIE, and kill status
must keep `stable_frame_full_clears=0`.

Console batching is not visual atomicity without an active backbuffer. This
presentation does not add a global framebuffer and does not alter the graphics
memory contract.

## Workspace, diagnostics, and review

The modal context owns a bounded heap workspace for row cells, text scratch,
split blocks, and schema. It is reused between frames and freed exactly once
on normal exit, killed exit, render failure, or owner recovery.

Automated gates prove exact lengths, boundaries, overflow policy, split safety,
glyph/style diffing, clear accounting, tail cleanup, and workspace ownership.
They do not prove subjective legibility or perceived flicker. Those remain
`AWAITING_HUMAN_REVIEW` in the dedicated checklist.

## Image continuity

A raw FAT image SHA-256 identifies one concrete candidate. It is recorded but
is not compared across recreated containers because volume serial and
filesystem timestamps are container metadata. Semantic continuity is proved by
the exact kernel hash and by extracting and comparing the five boot payloads:
kernel, EFI loader, font, logo, and startup script. The frozen release remains
unchanged and human visual review remains pending.
