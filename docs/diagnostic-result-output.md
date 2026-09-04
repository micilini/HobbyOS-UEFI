# Diagnostic result output

Manual health commands keep their detailed, machine-readable records on the
serial ports. The graphical terminal mirrors only a bounded human summary.

Synchronous diagnostics write one persistent line before the next prompt:

```text
[PASS] accounttest check
[FAIL] accounttest check - details on serial
```

An accepted asynchronous diagnostic first publishes `[RUNNING]` through the
shell status overlay. Its worker later replaces that status with `[PASS]` or
`[FAIL]`. The overlay does not modify console history, preserves the input
cursor, and remains visible until the next user input.

Status publication uses a monotonically increasing shell generation. A final
result can replace only the RUNNING state from its own generation; a stale
update cannot replace a terminal result. If the shell is executing a command
or a modal session is active, the newest status is retained in a fixed-size
pending slot. It is flushed after the prompt is restored or after modal
resume. Nothing is drawn over TASKMAN.

The production result/status publication path uses no heap allocation, sleep,
scheduler call, ANSI sequence, or unbounded formatting. Diagnostic names are
truncated explicitly when the fixed buffer or console width requires it. FAIL
always directs the operator to the serial log.

The framebuffer summaries cover:

- `accounttest check`
- `taskmantest check`
- `inputtest check`
- `modaltest check`
- `synctest check`
- `synctest timer-order`
- `synctest timer-backlog`
- asynchronous `synctest` workloads, including `synctest timer-cancel`
- `reaptest check`
- `reaptest timer-ref`
- `killtest check`
- `killtest smpstress-sweep`
- `schedtest check`

`irq check` and `taskdiag check` retain their existing framebuffer output and
are not wrapped, so they do not receive duplicate PASS lines.

In SELFTEST builds, `diagnosticresulttest all` verifies the real console
history/status destinations, return-code identity, synchronous synthetic
failure, asynchronous replacement, modal deferral, prompt/cursor preservation,
and bounded truncation. Its additional `arm-modal-timer-cancel` fixture creates
one bounded test worker which waits for modal entry and then launches the real
timer-cancel diagnostic; that fixture is absent from production builds.
