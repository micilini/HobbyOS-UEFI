# TMV1-UX-01-FIX2 focused certification

Status: `READY_FOR_HUMAN_VISUAL_REVIEW_FIX2`

Human visual review: `AWAITING_HUMAN_REVIEW_FIX2`

## Baseline and continuity

The preserved worktree was continued on branch `feat/taskman` at
`cde7dd046c7597ac4426a85317b38e6b96abae56`, tree
`7fa9ad0bc5c15b4f3ea9d858c058a5240c16cea1`, whose parent is
`8c6d05cbf5b8055a63b0fb43a69fd67e4022390a`. The five legitimate FIX1
source/script edits were retained. The pre-existing untracked roadmap remained
outside the change.

FIX1 stopped before any TASKMAN session because its COMPACT semantic-boundary
selftest failed. The rejected log was preserved under its original SHA-256
`85506a2d9b9e5087c86196429217531df516e8e63b2d3674ff5c5c65f653cb61`.

## CPU-percent budget correction

The canonical COMPACT `%CPU` width is six. FIX1 added one boundary cell,
producing slot width 7 and content width 6; the certified anomaly `!100.0%`
requires seven content cells. FIX2 adds two cells for COMPACT and one for WIDE:

| Mode | Canonical | Compensation | Slot | Boundary | Content |
|---|---:|---:|---:|---:|---:|
| WIDE | 7 | 1 | 8 | 1 | 7 |
| COMPACT | 6 | 2 | 8 | 1 | 7 |

Only the bounded NAME remainder funds the extra COMPACT cell. The COMPACT
fixed width is 70; NAME is six cells at width 76 and one cell at width 71.
The minimum remains 71 and width 70 remains TOO_NARROW.

The focused selftest now executes all four fixtures independently, emits a
field-specific case record on failure, and reports measured touches,
overwrites, overflows, and hidden truncations. It asserts the slot/content
arithmetic and compares the `%CPU` column literally with `!100.0%`.

## Focused runtime result

The only runtime invocation was `scripts/test-taskman-ux01.sh fix1-focused`.
It used one KVM boot with SMP=4 and exactly two TASKMAN sessions. No historical
regression, negative build, SMP matrix, or long soak was run.

The following gates passed in the same boot:

- semantic boundaries WIDE and COMPACT: 12 boundaries, 7 gaps, 5 group
  separators, zero touches;
- visual WIDE, COMPACT, and maximum values;
- identical differential present with zero changed cells;
- final input, modal, taskdiag, and taskman checks.

The final TASKMAN stats recorded 12 full frames, zero fallback frames, zero
stable full clears, zero stale cells, zero clipped writes, zero scroll, zero
separator mismatches, zero field overflows, and zero live visual workspaces.
Workspace allocations and frees were both two. See the
[focused log](../../artifacts/build/tmv1-ux01-fix2/focused.log) and
[SMP4 serial log](../../artifacts/build/tmv1-ux01-fix2/qemu-smp4.log).

## Captures and scenario validation

Both captures are valid non-uniform P6 images with complete raster data:

- [wide-fixed.ppm](../../artifacts/build/tmv1-ux01-fix2/screens/wide-fixed.ppm):
  WIDE, 118x40 requested geometry, 6 full frames, zero fallbacks, 138 captured,
  5 pages, SHA-256
  `8c62ca2e5d7b9f38b965fd43472393d65520af694b68c19df0033a3a80fec15f`;
- [compact-fixed.ppm](../../artifacts/build/tmv1-ux01-fix2/screens/compact-fixed.ppm):
  COMPACT, 76x40 requested geometry, 6 full frames, zero fallbacks, 138
  captured, 5 pages, SHA-256
  `3b3c7890c9277b3599979bb9cd2691216d0e946b8dda7a86d8e1f34103756764`.

Each capture has `.meta.txt`, `.session.txt`, and `.stats.txt` companions. The
scenario-aware validator passed; it rejects WIDE/COMPACT name-to-mode
divergence as `CAPTURE_SCENARIO_MISMATCH`.

## Source continuity and final build

The FIX1 protected-before manifest and both FIX2 after manifests have identical
content. Their manifest-file SHA-256 is
`826b528420c98e10be38f289ff6e5df3bedc524b4f9793e9c1f6d14e04b5193b`.
No protected runtime file changed. The frozen CL-14 kernel and image hashes
remain `9f8fcc9417e68118ef8fcde4c2af8f9dadd5eb138397b7e4ef15585e9463b973`
and `424b2b6656d9d8cf772bb173f4f99ccb347d1e005ef3cb1e0db4cb0d80c27b48`.

`make stack-check`, parallel `make kernel-check`, `make deps-check`,
`make image`, and `nm -u kernel.elf` passed. The maximum frame is 1,968 bytes;
the previously certified TASKMAN render frame remains 416 bytes. No FIX2-owned
warning was emitted. Candidate hashes are:

- kernel: `ca630b9f282016ddcfd832c3b130011b41232ed6b7f0c667663f6850e628db8c`;
- image: `7af987726740db0ed92628fd51cca13f8862c435f20b31a3f1bd573dddb7236a`.

See the [final build log](../../artifacts/build/tmv1-ux01-fix2/final-build.log),
[stack report](../../artifacts/build/tmv1-ux01-fix2/stack-usage-report.txt), and
[final hashes](../../artifacts/build/tmv1-ux01-fix2/final-hashes.txt).

## Git and human gate

The accumulated FIX1/FIX2 change is staged explicitly only after all focused
gates and audits pass. Its required subject is
`fix(taskman): separate semantic columns and validate compact capture`, with
`cde7dd046c7597ac4426a85317b38e6b96abae56` as parent. No push is authorized.

Automated status is `READY_FOR_HUMAN_VISUAL_REVIEW_FIX2`. The dedicated
[human checklist](TMV1-UX-01-human-visual-checklist.md) remains entirely
unchecked. TASKMAN V2 and TMV1-BM-01 were not started; CL-14 was not replaced.
