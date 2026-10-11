# Cache-writeback clock-call diagnosis

`continuous-mask-writeback-clock-v1` measures the three existing HPET clock
calls inside a cache-writeback callback: local-before, local-after and
handler-after. Each original call executes exactly once with its original
output and Boolean result. HPET implementation, read validation, clocks,
request ordering, WBINVD, the 29-cell enclosing primitive and all calibration
and observer limits remain unchanged. This is a diagnostic build only.

The preceding candidate `51d68b92e0527ac2c9cf8e8af5bdcfe77fe9a023541686acfffad61105296f6f`
observed valid WBINVD minimum exposures of 831–1498 ns, while its enclosing
callbacks spanned 59,403–191,922 ns. These separate bounds do not prove that
HPET accounts for the difference. The new calls provide that discriminator;
they do not subtract time from any original observation.

Independent CPU and continuity metadata surround each ordered timestamp pair.
Both timestamps are inside the existing continuous IF=0 segment. Existing
mask, owner, generation, switch, IRQ and returning-fault bookkeeping invalidates
intervening serviceability or context changes. The one additional CPU query is
outside the timestamps. An internal recorder-sequence change makes the witness
unavailable, including a locking counter-extension path that changes it.
No new HPET read is introduced to obtain metadata. The original returned ticks,
nanoseconds, mode, retries, validity and success are retained.

The fixed short experiment admits one cache-writeback operation per CPU. Three
per-CPU records retain each first raw clock-call witness even when invalid;
counts never wrap, and a repeated call is explicit exhaustion rather than an
overwrite. The host independently requires exactly one original callback and
one of each site on every CPU, identical service generation/root, complete ACKs,
the actual call order around the separately retained WBINVD witness, and exact
HPET tick conversion in the pre/post timebase domain. Missing, invalid or
repeated records cannot provide diagnostic authority.

Conversion uses the existing interior upper-frequency bound and downward
rounding: floor(cycles * 1000000000 / ceil(max(pre.upper, post.upper) * 1.01)).
No allowance is subtracted. An independently qualified minimum of at least
50,000 ns disproves the strict budget for that observed subset. A smaller
minimum supplies no accepted upper bound. Host/asynchronous delay, fences,
the original clock call and compiler-generated call machinery remain included;
this is not pure MMIO or uninstrumented execution time.

All original empty probes, pre allowances, postcheck, entry/exit checkpoints,
ordinary work, stress and terminal audit remain mandatory. Moving the entry
timestamp alone would not resolve the original failed postcheck or the observed
whole-window path violations; no entry endpoint is changed here.

The second finite diagnostic has one deliberately changed observation: these
three call witnesses. A large clock-call minimum implicates that actual call
region, with any asynchronous delay still included. Small minima leave other
callback regions unresolved. Neither outcome authorizes an identical retry or
promotes this diagnostic to timing acceptance. All original failures survive.
