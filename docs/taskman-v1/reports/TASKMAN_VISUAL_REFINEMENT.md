# TASKMAN visual refinement

Status: AUTOMATED CERTIFICATION PASS — HUMAN REVIEW PENDING

## Scope and diagnosis

The visual maintenance retained TASKMAN behavior and ownership. The original
table appended bounded fields into fixed slots but did not expose every
semantic boundary, allowing values such as PID/USER and LCPU/AFF to appear
joined. The original stable render also cleared the complete region before
redrawing, creating a visible old-content → blank-background → new-content
sequence. Console batching could not hide that sequence because no active
backbuffer supplied frame-level visual atomicity.

No scheduler, lifecycle, input, modal, formatter, graphics-memory, or command
semantics were changed.

## Implementation

`taskman_view.c/.h` owns one offset schema shared by header, normal rows, and
selected rows. The schema defines thirteen columns, twelve semantic boundaries,
seven internal gaps, five ASCII group separators, exact alignment, and the
bounded NAME remainder. WIDE remains available at 118 columns; COMPACT remains
available from 71 columns and is certified at 76 columns.

Title, summary, header, and footer use deterministic split composition.
`ConsoleCell` supplies the native palette and full-width selection background.
No ANSI parser or global backbuffer was introduced.

The console present API compares character, foreground, and background with
history and only draws changed cells. Stable frames never clear the full
region. Empty task rows and shorter content are presented with fill cells, so
stale tails disappear without an intermediate blank frame.

The modal UI context owns and reuses the visual workspace. Cleanup frees it
exactly once on all certified exits. Render stack usage remains within the
kernel budget.

## Validation and continuity

Automated tests cover schema widths, maximum values, boundary alignment,
selection styling, split lines, glyph-only and style-only changes, identical
rows, shorter rows, geometry changes, real WIDE/COMPACT sessions, input/modal
checks, and cleanup. The active focused smoke uses exactly two TASKMAN sessions
on SMP=4 with KVM.

The candidate kernel and image remain byte-identical after nomenclature-only
maintenance at the payload level. The raw FAT image SHA-256 identifies the
current container, while continuity across recreated images is proved by the
exact kernel and five extracted boot payload hashes. Volume serial and
filesystem timestamps are container metadata. Frozen release bytes remain
unchanged. Current active evidence is written under
`artifacts/build/taskman-visual-final/`; historical evidence retains its
original archived names and is not the active test interface.

Human review remains required through
`docs/test-reports/TASKMAN_VISUAL_HUMAN_CHECKLIST.md`.
