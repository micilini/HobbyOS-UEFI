# TASKMAN visual certification

Status: AUTOMATED PASS

Human visual review: `AWAITING_HUMAN_REVIEW`

## Layout certification

- WIDE minimum: 118 columns.
- COMPACT minimum: 71 columns; certified point: 76 columns.
- Width 70: TOO_NARROW.
- Semantic columns: 13.
- Semantic boundaries: 12.
- Internal gaps: 7.
- Group separators: 5.
- Header and rows share one schema.
- PID and anomalous `%CPU` retain their complete certified values.
- Boundary overwrite, field overflow, hidden truncation, and split overlap:
  zero.

## Differential redraw certification

- Glyph, foreground, and background are compared against console history.
- An identical row changes zero cells.
- A style-only change dirties the expected cell.
- A shorter row clears its tail through bounded fill presentation.
- Stable full-region clears: zero.
- Cleanup clear: preserved.
- Stale cells, clipped writes, and modal scroll: zero.
- Visual workspace residual after session: zero.

## Visual hierarchy contract

Title metadata, two summary blocks, grouped header, selected row background,
status palette, and two footer rows use native console cells. No raw ANSI
escape is part of the interface.

## Active validation interface

```bash
scripts/test-taskman-visual.sh naming
scripts/test-taskman-visual.sh image-payload
scripts/test-taskman-visual.sh focused
scripts/verify-taskman-visual-evidence.py all
```

New evidence belongs to `artifacts/build/taskman-visual-final/`. Archived
reports and artifacts preserve historical provenance but are not active test
interfaces.

## Image payload continuity

The raw FAT image SHA-256 records one concrete candidate. Cross-format
continuity does not compare raw container bytes because volume serial and
filesystem timestamps are metadata. The active verifier validates FAT32
geometry and boot signature, then extracts and byte-compares the kernel, EFI
loader, font, logo, and startup script. The exact kernel hash remains an
independent invariant. Frozen release bytes remain unchanged.

## Human visual review

Automated structural checks do not claim subjective approval. The canonical
[human checklist](TASKMAN_VISUAL_HUMAN_CHECKLIST.md) remains pending.
