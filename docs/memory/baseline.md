# QEMU observations and validation disposition

The memory integrity repair resolves the original authorization conflict. The
frozen observation candidate passed the complete fifteen-boot functional matrix,
six-sample baseline and both production smoke profiles. Overall development
acceptance additionally requires the fixed current inherited-regression ledger,
final fixtures and extracted-package replay. Their authoritative disposition is
recorded in the delivery report and `acceptance.json`; this measurement document
does not promote an incomplete campaign. Hardware is `DEFERRED_TO_FINAL_CLOSURE`.

## Frozen candidate

The preserved base is `c5e6cf021eee9df489216402c5503b9c38da7df5`.
Canonical build-input SHA-256 is
`2b0678c9e7e606deb22c23c6a2385b02874d190f9f6fce85f90edca083f87044`.
SELFTEST ELF SHA-256 is
`c78ab548486ab14705dccfe9fc682da538643ab8cb8284f6a3f3b412aafbe091`;
image SHA-256 is
`d15051fb5daaf07a79e9fa000fa2f55760c2d75d04aa4b992c695d7b01a5c4ca`.
The candidate identity is
`ddaa276bbdf7a8cfc19148091306b684dec4fa854c505fb0bfe7cf0cf5ebfe4c`.
Full source identity, separate harness manifests, options, tool/firmware identities
and five extracted FAT payload hashes accompany each frozen build receipt.

## Functional boots and baseline

| Profile | Fresh accepted boots | Consecutive requirement |
|---|---:|---|
| Q35 / TCG / SMP1 | 3 | Three repaired reproducer boots |
| Q35 / TCG / SMP2 | 1 | One |
| Q35 / TCG / SMP4 | 3 | Three consecutive |
| Q35 / KVM / SMP4 | 3 | Three consecutive |
| Q35 / KVM / SMP8 | 1 | One |
| PC / TCG / SMP4 | 1 | One |
| Q35 / KVM / SMP24 | 3 | Three consecutive |

Every boot executes the observation commands, three increasing CPU generations,
and ordinary memory/CPU/IRQ commands using the unchanged framed transport. Each
launch has independently captured VM, boot, process and timestamp receipts. The
journal retains attempts and resets a profile's streak after failure. There are
no failed attempts inside this frozen candidate's fifteen-boot functional series.
The earlier failed candidate and debugger reproduction remain separate failures.

These six clean SMP4 boots also supply the baseline; they are not additional
boot repetitions. Intervals below are original host serial-receipt timestamps
between `[CORE] Init Paging...` and `[CORE] Paging OK! Switched CR3.`.

| Accelerator | Sample 1 (ns) | Sample 2 (ns) | Sample 3 (ns) | Median (ns) |
|---|---:|---:|---:|---:|
| TCG | 1411852460 | 1415497788 | 1446679785 | 1415497788 |
| KVM | 157258679 | 155190823 | 156314361 | 156314361 |

All use Q35, 2 GiB and the same frozen candidate; TCG uses `max`, KVM uses
`host`. Exact QEMU/OVMF versions and hashes are in launch receipts. These are
host reception intervals, not exact guest execution durations. Serial buffering
and host scheduling introduce unquantified uncertainty; nanosecond clock units
do not establish nanosecond accuracy. Same-chunk, missing, duplicate and
nonmonotonic markers are rejected. Traced/debugger runs are excluded.
The host exposes twelve schedulable CPUs. SMP24 is mandatory functional coverage;
its rate results retain the inherited nonauthoritative oversubscription status,
raw FAIL when present and separate nonoversubscribed KVM controls. Threshold 700
and LAPIC's 1000-us period are unchanged.

## PMM integrity and reproduced failure

The Q35/TCG/SMP1 map still includes Reserved
`[0xfd00000000, 0x10000000000)`, giving 268435456 logical frame bits.
This represents a 1-TiB address span, not installed or allocatable RAM.
The original bit-by-bit validator held the PMM lock with IF clear. Its saved
GDB stop was `pmm_validate_integrity+143`, RFLAGS `0x3`, index 88798742.
The debugger command later exited 1 on an untyped-symbol print. Its useful
register capture remains evidence, and no observed triple fault is inferred.
The original normal command failed the five-second Enter-release contract.

The repaired validator scans all valid bits in bounded 64-bit words under the
same IRQ-save lock. It checks `reserved & ~allocated & valid_mask` and counts
`~allocated & valid_mask`, then compares the entire free count. Tail padding,
unaligned storage, invalid dimensions and buffer bounds are checked. No region
is skipped based on descriptor type. Allocation policy and bitmap mutation APIs
are unchanged. Portable integer population counting introduces no POPCNT/SIMD or
runtime helper; actual object/final-ELF disassembly and resolution are retained.
The [GCC x86 option reference](https://gcc.gnu.org/onlinedocs/gcc-15.1.0/gcc/x86-Options.html)
describes which target instructions compiler options permit. It does not replace
inspection of this candidate under its recorded compiler and flags.

In the final candidate's three fresh reproducer boots, both actual `mem`
integrity checks returned true, the handler returned zero, framing/checksum/END
passed, and subsequent independently framed commands continued. Observed Enter
release intervals were 0.119807170, 0.117322279 and 0.118485271 seconds, below the
unchanged five-second bound. These intervals are transport observations, not
isolated scan timings and not a microsecond IRQ-off guarantee.

A separately labeled hosted 268435456-bit experiment measured 0.214908 seconds
for the reference bit loop and 0.018074 seconds for the actual helper. Host and
sanitizer measurements do not establish guest interrupt latency. Full coherent
validation still holds the PMM lock across the scan; a future latency redesign
requires a separate synchronization proof and is outside this repair.

## Table frames, CPU controls and cache observations

| Observation | Initial PMM allocations | Active reachable frames | Bootstrap-only frames | Total PMM allocations |
|---|---:|---:|---:|---:|
| Q35 / TCG / SMP4 | 2064 | 2068 | 1 | 2069 |
| Q35 / KVM / SMP4 | 2056 | 2059 | 1 | 2060 |

The initial value is a successful-allocation counter at the unchanged paging
marker, not an early coherent traversal. The active count includes the root and
unique physical table frames; data and huge-page leaves are excluded. Seven
static assembly table frames are enumerated separately; zero are reachable from
the observed active root. A separate stopped Q35/KVM/SMP4 diagnostic capture
contains 2067 raw table pages: 2060 PMM-owned and seven static frames. Portable
level-aware traversal exactly reconciles the active set, bootstrap-only set and
allocation registry, with no unexplained orphan. The same run independently
reads twelve kernel/MMIO/framebuffer entries. Allowed A/D differences are limited
to architecturally applicable entry fields. The diagnostic is not a baseline boot.

CPU collection completeness remains separate from control uniformity. Independent
boot ONLINE records bind each slot/APIC and BSP/AP role. The observed BSP WP=1
and AP WP=0 difference is retained. No control register or MSR is normalized.
PAT's bit and three-bit index are distinct: PAT=0, PCD=1, PWT=1 selects slot 3.
Raw descriptors, labels and intersections are checked against retained BootInfo
and ACPI observations. Fixed MTRRs remain `NOT_COLLECTED`; effective cache type
remains `NOT_COMPUTED`. PAT-selected types are not effective-type claims.

## Compiled footprint and production isolation

| Configuration | .text | .rodata | .bss | Sum of sections (bytes) |
|---|---:|---:|---:|---:|
| Production | 530678 | 77531 | 15237400 | 15892065 |
| Debug without SELFTEST | 544502 | 79099 | 15379096 | 16049153 |
| Debug SELFTEST | 596486 | 99707 | 15418968 | 16161913 |

Profile differences include existing assertion and SELFTEST code; they do not
measure only this increment's cost. The observer registry and traversal queue
are each 65536 bytes, with 8192 bytes of traversal levels. Their storage is static,
not an automatic kernel frame. The 2048-byte automatic-frame ceiling is retained.
Production ELF SHA-256 is
`bf2ecf8deef04ef627da8e6fd8bf04bff1788be15e9aeec2fdadffbb598a31f6`.
Its code intentionally differs from pre-repair production because the equivalent
PMM validator is optimized. Actual production smoke on Q35/TCG/SMP1 and
Q35/KVM/SMP4 rejects all three new commands and executes `mem`, `cpu` and
`irq check`. Full ELF/payload absence checks remain local, with saved binary
facts and exact identities for portable replay.

## Historical and current validation authority

The original assertion-record capture, TASKMAN-statistics capture and layout
reference ELFs remain `HISTORICAL_REPLAY_UNAVAILABLE`. Their old modes, hashes and
failure receipts are preserved. Current substitutes use actual C producers,
fresh strict-parser mutations, the assertion-context/runtime matrices and current
ELF structure plus same-object relinks across all required roles. Their outcome
is recorded independently in the fixed current ledger.

The legacy modal failure-classification capture is also unavailable. Its old
`all` mode remains unchanged. The named current adapter reuses the maintained
ownership/resource-checkpoint validators on two SMP4 and three SMP8 concurrent
modal runs, following the existing modal validation authority; it does not claim
to replay that old capture or reinstate superseded internal global-heap equality.

Portable replay establishes consistency among supplied source, receipts and raw
observations. It cannot authenticate physical execution or rehash omitted full
ELFs/images. Bounded raw ELF/table proofs replay their structural invariants;
synthetic negative fixtures are labeled and never counted as fresh boots.
