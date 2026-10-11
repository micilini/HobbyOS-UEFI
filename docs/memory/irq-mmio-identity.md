# MMIO trace identity inside a continuous mask

`continuous-mask-mmio-identity-v1` is an instrumented observer correction.
The fresh preceding candidate `335856506f6b6495101a6e4a97a44ab69cb560d752c4726d3f8a2ebf63eb9bc4`
retained 24 frequency-qualified continuous-mask HPET call witnesses. Fourteen
lower duration bounds reached 50000 ns; the largest was 62923 ns. These include
all debug tracing and possible host/asynchronous pauses. They do not identify
one unique cause. The CPU1 post-envelope rejection, 19812 cycles against its
3816-cycle frozen allowance, remains rejected and lies mainly before the first
software entry checkpoint.

The actual HPET split-counter path performs three original MMIO reads. Each
MMIO trace first saves IRQ flags, then queries architectural CPU identity again.
An instrumented save already resolves actual CPU identity and binds a token to
the continuously masked stack-owner generation, recorder epoch, switch, entry,
mask-segment and fault sequences. Reusing that token removes a redundant query;
it does not remove an MMIO access or assert that the query caused the whole
captured delay or the earlier entry excursion.

The common token validator now also retains the architectural APIC ID from the
published topology at its actual-CPU seed, and rejects changed APIC ID or slot.
Topology is discovered before activation and is immutable during this role.
The MMIO helper only reads an already valid token. It neither refreshes it nor
performs a CPUID. IF=1, hidden enable, switch, entry/return, stale owner, changed
epoch, fault, sequence exhaustion, missing token and frozen recording all
refuse reuse. The existing architectural query and exact MMIO topology/hint
validation then run. No CPU-control register or scheduling policy changes.

The MMIO attempt/completion records keep their original address, value, width,
operation, phase, writer ownership and IRQ restoration. A bounded per-CPU reuse
counter shares only that CPU's diagnostic cache line and is published after
terminal freeze. Unknown method or missing/exhausted counters cannot qualify
the current complete gate. Historical plans use their exact captured code.

Actual C tests execute the MMIO read and trace, validate its full snapshot,
exercise every token boundary and fallback, and preserve incomplete publisher
handling. Normal, UBSan and ASan remain required. Ordinary DEBUG and production
builds compile out the reuse adapter; their MMIO object equality is checked with
the same local compiler. All original HPET high-low-high/retry/extension,
clock conversion, four probe endpoints, allowance and postcheck remain intact.

One separately sealed follow-up diagnostic boot is permitted for this concrete
correction. It keeps the original pre/ordinary/stress/post/audit effort and
records all outcomes. The discriminator is actual reuse with retained valid
clock-call endpoints and unchanged MMIO results. Smaller minima do not prove
any passing upper bound. No unchanged failed acceptance retry follows it.
