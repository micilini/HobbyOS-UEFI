# TMV1-CL-13-FIX2 — TASKMAN automated-session anchor

Date: 2026-08-16
Branch: `feat/taskman`
Base/HEAD: `4e700a9b38653fb57ecb0f107a18f87508db0736`
Status: `REJECTED_CL13_FIX2_FOCUSED`

FIX3 follow-up: the transactional transport subsequently delivered the
count-257 command intact and proved dispatch with `ACCEPT` and `BEGIN`; the
handler returned `END status=1`. The preserved fixture is bounded by
`FIXTURE_MAX 129u`, so the later gate stopped as `REJECTED_CL13_MATRIX` rather
than transport loss. See `TMV1-CL-13-FIX3-transactional-transport.md`.

## 1. Worktree continuity

The legitimate uncommitted CL-13 and FIX1 implementation was present at
entry, with an empty index and the roadmap untracked. It was preserved. No
push, commit, destructive Git operation or CL-14 work occurred.

## 2. Boundary FIX1

FIX1 remains technically approved: SMP=1/TCG 3/3, SMP=2/TCG 3/3,
SMP=2/KVM 5/5 and SMP=4/KVM 3/3 passed. Foreign semaphore noise was observed
without a target callback or target permit, the old-gap negative was detected,
and no exact-target stall reproduced. Its classification remains
`HARNESS_FALSE_POSITIVE_CONFIRMED`. FIX2 did not reopen the semaphore observer,
boundary protocol, scheduler, runqueue, wait queues or context switch.

## 3. Original TASKMAN timeout

The prior consolidated SMP=4/TCG matrix stopped in fixture count=20,
refresh=1000, auto-exit target=3. The session began and the shell was BLOCKED
under an ACTIVE modal owner, but no PASS/FAIL auto-exit sentinel appeared for
900 host seconds. There was no panic, exception, fatal marker or guest-side
FAIL. The preserved serial hash is
`5d6e309a590ff6b6e9c578455c0edcbe9de64df349ed850151f274adce195ec6`.

## 4. Runtime freeze and CL-12 hashes

The following hashes matched the approved CL-12 source exactly:

| File | SHA-256 |
|---|---|
| `cmd_taskman.c` | `2ea875eb0f09402dcdabdb87be9bd0e329ea1bc29e9f2b83da3430469bab800f` |
| `cmd_taskman.h` | `85b5b434b2dfcd0173854a70051d5a96a46efd427fcd6ac3fdd0f6f2be2316ed` |
| `cmd_taskmantest.c` | `287d05c143a615afce0965204de6c262203c82e0f03e9ab441815a5bfc95577d` |
| `cmd_taskmantest.h` | `639a8236d69cdd102fc4fc65d206dae9ce86d321dd0f37b983dd8382cf9cd7a3` |
| `modal_ui.c` | `ecfef61a1ac37a1457f86b257f43888fdc4f716b4de36f076dcfb072639c8215` |
| `task_metrics.c` | `cda2ea3c28986bfbe6f5ec89e886117286d74e273e575caabece925345797ded` |
| `console.c` | `e460c06426aff4eeb7de8ff0284e477b1dd0b01f880ec9d0405a7ed11835a012` |
| `test-cl11.sh` | `9055b7c5b878ac865cfe50ddfbdd1708b227c25ec478713e2fe225e0ec910739` |

The deterministic runtime manifests before FIX2 and after the focused attempt
both hash to
`8543996d4fd3daf3e0f6b7047e9f733f05f6d309f90be8dff8557b7d9170152a`.

## 5. Existing positive evidence

`cl13-taskman-clean-20-refresh1000.log` had already passed count=20,
refresh=1000 with three WIDE full frames and zero fallback. The independent
`cl13-taskman-after-modal-20-refresh1000.log` also passed the same session
after `modaltest all`. This excluded count, refresh and prior modal testing as
individually sufficient runtime defects.

## 6. CL-13 helper omission

The CL-13 `auto_taskman` armed full-frame auto-exit and launched TASKMAN
without first running `taskmantest anchor-reset`; it then waited 900 seconds.
The soak helper had the same omission for warmup and all 100 sessions.

## 7. Certified CL-11 behavior

The CL-11 helper runs `taskmantest anchor-reset` immediately before every
auto-exit arm. The approved CL-11 source and campaigns therefore began each
modal renderer with a full-height console region.

## 8. Cursor and layout contract

TASKMAN captures the current console cursor in its modal worker. Available
height is `console_rows - cursor_y`. If that region is below the minimum, the
layout is `TOO_SHORT`; rendering succeeds as fallback but cannot produce a
full WIDE/COMPACT frame.

## 9. Why fallback did not exit

Auto-exit advances only on full frames. A `TOO_SHORT` renderer can continue
producing fallback frames indefinitely while the modal owner remains ACTIVE
and the shell remains intentionally BLOCKED. The old generic timeout then
mislabelled this state as a shell input stall.

## 10. Bounded negative design

The first exploratory fixed count of two snapshots did not degrade enough and
was preserved as `cl13fix2-negative-anchor-attempt1-two-ps.log`; it was not
used as a PASS. The final canary uses exactly four `ps 1 128` snapshots. With
28 tasks this prints at least 128 table/body lines, above the largest certified
console height. The count is fixed and never adjusted based on test outcome.

## 11. Negative causal result

Without anchor reset, the session did not auto-exit within 20 seconds. Three
ESC events ended it. Stats reported:

```text
last_mode=TOO_SHORT
last_session_full_frames=0
last_session_fallback_frames=22
auto_exit_shortfalls=1
auto_exit_pending=0
```

The shell returned, `taskdiag check` passed, cleanup removed all 20 fixtures,
stats/controls were reset, and `taskmantest check` explicitly proved
`model_live=0` with no residual.

## 12. Positive causal result

In a new SMP=4/TCG boot, the same fixture and four-snapshot degradation were
followed by `taskmantest anchor-reset`. TASKMAN then emitted PASS with target=3,
full_frames=3, fallback_frames=0, mode=WIDE, captured=29, render_failures=0
and modal_scroll=0. Cleanup and both validators passed.

## 13. Causal classification

The focused artifact records:

```text
[CL13][TASKMAN_ANCHOR_CLASSIFICATION]
HARNESS_ANCHOR_DEGRADATION_CONFIRMED
```

The original timeout is therefore classified as harness anchor degradation,
not a confirmed TASKMAN runtime defect.

## 14. Matrix helper correction

`auto_taskman` now always resets the anchor, arms auto-exit, and bounds three
frames at 180 seconds. On failure it attempts three ESCs, waits for modal end,
collects `taskmantest stats`, and classifies `TASKMAN_LAYOUT_FALLBACK`,
`TASKMAN_RENDER_STALL`, `TASKMAN_GUEST_FAILURE` or `TASKMAN_GUEST_FAULT`.
It no longer invokes the generic shell-stall diagnosis for an intentionally
blocked modal shell.

## 15. Soak helper correction

`taskman_session` now resets the anchor before every arm, including warmup and
each of the 100 refresh-50/1000/2000 sessions. No duration, target or workload
volume was reduced.

## 16. Focused build

The focused stack/kernel/nm smoke passed after the script changes. Maximum
stack usage remained 1,984 bytes, kernel j2 passed, and `nm -u` was empty.
Only historical non-owned compiler/linker warnings were present.

## 17. Focused variants result

The first required SMP=4/TCG boot passed:

- count=1, refresh=50, target=3;
- count=20, refresh=1000, target=3;
- count=50, refresh=2000, target=3;
- count=129, refresh=50, target=3.

Each completed with full_frames=3, fallback=0, render_failures=0,
modal_scroll=0, exact fixture cleanup and a clean `taskdiag check`.

Before count=257 began, the HMP/xHCI synthetic keyboard transport lost the
`taskmantest setup 257` command. No matching setup line reached serial during
the 240-second command timeout. The QEMU process remained alive, the socket
remained present and the last guest validator was clean. The transport probe
classified `TRANSPORT_LOSS`; the runner did not resend the non-idempotent
setup command.

The failure serial is
`cl13fix2-smp4-tcg-variants-run1-transport-failed.log`, SHA-256
`c779cf1f6fcb99939a6775d45a291efeb4a32a52a2f2480d57d848b57907bc72`.

## 18. KVM variants and soak smoke

Not executed. Fail-fast stopped at SMP=4/TCG run 1, before count=257, TCG runs
2/3, both KVM boots and the 20-session soak smoke.

## 19. Source continuity

Runtime before and after the focused attempt is byte-for-byte identical. No
file under `kernel/`, `bootloader/`, `shared/` or `makefile` changed during
FIX2. Only the two CL-13 helpers, the new focused runner and documentation
were changed.

## 20. CL-13 resumption

Not started. UP TCG, SMP=2 TCG and SMP=2 KVM remain reusable because runtime
continuity holds, but the prerequisite focused FIX2 gate did not pass.

## 21. Remaining CL-13 stages

SMP=4/8 matrix scenarios, quantum variants, the consolidated negative matrix,
SMP=4/8 soaks, final reproducible build, final report and commit were not run.

## 22. Git and environment

HEAD remains the base with zero commits after it. The index is empty, there
was no push, and the CL-13/FIX1/FIX2 worktree plus artifacts was preserved.
No QEMU remains active. The failure was an HMP/xHCI transport loss, not a guest
fault or runtime-source change.

## 23. Status

`REJECTED_CL13_FIX2_FOCUSED`

The anchor diagnosis and helper correction are causally validated, but the
mandatory focused variants did not complete. Per fail-fast policy, no later
stage or commit is permitted. TMV1-CL-14 remains reserved.

## 24. FIX3 addendum

The missing `taskmantest setup 257` line, live QEMU/HMP endpoints and later
transport marker motivated the SELFTEST-only transactional transport in
`TMV1-CL-13-FIX3-transactional-transport.md`. FIX3 preserves the proven
anchor correction and wraps CL-13 shell commands with CRC, sequence,
BEGIN/END and exactly-once replay handling. Runtime outside the authorized
shell/selftest transport files remains protected by a separate manifest.
