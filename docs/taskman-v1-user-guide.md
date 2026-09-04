# TASKMAN V1 user guide

Status: RELEASED; visual candidate awaits human review

## Open and close

Run `taskman` for the default 1000 ms refresh or, for example, `taskman 250`.
The accepted range is 50 through 2000 ms. TASKMAN owns a modal session and the
shell remains blocked until exit. Press ESC to leave; no other key exits.

## Controls

| Key | Action |
|---|---|
| Up / Down | Move selection. |
| PageUp / PageDown | Move by one visible page. |
| Home / End | Select first / last task. |
| ESC | Exit TASKMAN. |

Selection is visual only: TASKMAN V1 does not kill from inside the view.

## Layout and pagination

WIDE starts at 118 columns. COMPACT starts at 71 columns and is the selected
mode at 76 columns; 70 remains TOO_NARROW. Insufficient rows show TOO_SHORT.
Visible rows and page count come from the current geometry.

The thirteen semantic columns have twelve visible boundaries: internal gaps
between columns in the same group and ASCII `|` between identity, locality,
scheduling, accounting, control, and name groups. Fields are bounded, long
text uses an explicit marker, PID retains every digit of a 64-bit task ID, and
COMPACT retains the complete anomalous value `!100.0%`.

Title, two summary rows, header, and two footer rows use deterministic
left/right blocks. Native console colors distinguish selection and important
states. Stable refresh, selection, and pagination redraw only changed glyphs
or styles; full-region clear is reserved for cleanup or an incompatible
geometry transition. See the [visual contract](taskman-v1-visual-contract.md).

## Reading the table

- `PID` is the 64-bit kernel task ID, not a POSIX process ID.
- `USER` is the presentation constant `Root`; there is no user model.
- `CPU`/`LCPU` identify current/last logical CPU when available.
- `AFF` is the presentation constant `Any`; real affinity is not implemented.
- `STATE`, `CLASS`, and `Q` describe scheduler state, class, and quantum.
- `TIME+` is accumulated runtime; `%CPU` is a windowed sample.
- `KILL` describes protection/pending/exit status, not exit reason.
- `MEM~` is an estimated known kernel footprint, not RSS.

The first CPU sample displays `--`. Subsequent frames use runtime delta divided
by sampler wall-time delta. A `!` prefix marks an anomalous sample. PS and
TASKMAN own separate samplers, so their values may legitimately differ.

## Common errors

`taskman: refresh_ms must be 50..2000` means the value is malformed or outside
the range. TOO_NARROW and TOO_SHORT describe console geometry, not snapshot
failure. If modal ownership cannot begin, the command reports failure.

See the [command reference](taskman-v1-command-reference.md) and
[known limitations](taskman-v1-known-limitations.md).
