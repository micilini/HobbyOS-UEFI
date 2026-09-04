# TASKMAN V1 known limitations

Status: ACCEPTED V1 LIMITS

These are deliberate scope boundaries, not hidden V1 defects.

- The kernel is Ring 0 only and uses one address space. Tasks are kernel tasks,
  not processes; there is no `process_t`, POSIX model, PID/TID split, Ring 3,
  syscall ABI, per-process CR3, signals, or user identity.
- `USER=Root` and `AFF=Any` are presentation constants. V1 has no real CPU
  affinity, per-task heap owner, or topology-aware scheduler.
- `MEM~` is the known task/stack/context estimate, not RSS or complete resource
  attribution.
- Kill is cooperative. There is no forced kill and no arbitrary interruption.
- TASKMAN has keyboard navigation only, no mouse, and no interactive kill.
- There is no persistence, history export, or lifetime-average CPU chart.
- TASKMAN captures at most 4096 entries. Terminal geometry can select compact,
  too-narrow, or too-short fallback layouts.

## Operational caveats

- TCG and KVM have different timing/latency. A late sleep resume is QoS
  telemetry unless the monotonic/deadline contract itself fails.
- Hotplug logs may legitimately appear after `RUNTIME_READY`.
- Release machine records are built in bounded buffers and emitted atomically;
  unrelated serial lines may still occur between records.
- `tasktest` and autorun are absent in release. Other diagnostic commands are
  development interfaces and should not be confused with public TASKMAN UX.
- Serial logs remain essential for boot and failure diagnosis.

Future affinity, memory ownership, process/user separation, and topology work
requires a separate roadmap and does not alter the certified V1 contract.

## Post-release visual maintenance

TMV1-UX-01 keeps the CL-14 release bytes frozen and supplies a separate visual
candidate. Differential row presentation structurally removes stable-frame
full clears, but perceived flicker and palette quality still require human
inspection. There is intentionally no global backbuffer, ANSI parser, GUI,
mouse support, configurable theme, or V2 behavior. A real geometry change may
clear the previous region once; normal refresh and navigation may not.
