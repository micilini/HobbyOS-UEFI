# TMV1-CL-13 — TASKMAN V1 visual checklist

This checklist complements the automated coordinate, clipping, stale-cell,
region-cleanup and no-scroll checks. It is not a substitute for those gates.

- [ ] Wide console: canonical WIDE columns remain aligned.
- [ ] Compact console: compact labels remain legible and aligned.
- [ ] Too narrow: bounded fallback is shown without partial rows.
- [ ] Too short: bounded fallback is shown without partial rows.
- [ ] Page navigation: PageUp/PageDown/Home/End update page and selection.
- [ ] Selection: the selected row remains visually distinct across refreshes.
- [ ] Long name: bounded ellipsis is clear and does not overwrite neighbors.
- [ ] ZOMBIE MEM~: retained memory and `ZOMB` status agree with `ps`.
- [ ] Clean region: closed sessions leave no stale panel cells.
- [ ] Exit cursor/shell: ESC restores the cursor and an interactive shell.

Automated evidence is recorded in the CL-13 matrix and soak artifacts. Manual
inspection may add non-blocking notes, but automated cases are never waived by
this checklist.
