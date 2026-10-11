# Memory observation contract

This diagnostic increment reads existing state. It does not normalize controls,
change mappings, establish a direct map, resolve effective cache types, or prove
memory hardening. Hardware observations are deferred to final closure. The current
measurements and validation authority are recorded in `baseline.md`.

## Build and commands

The root `makefile` remains the canonical interface. `DEBUG_ASSERT=1` includes the
reusable `paging_inspect` API and pure decoder. `SELFTEST=1 SELFTEST_AUTORUN=0`
adds `ptinspect <decimal-or-0x-address>`, `ptinspect samples`, `cpustate`, and
`memmap`. These commands use the existing `diagnostic_result_complete` and framed
command transport. No architecture-test switch is required. A true
`make production-image` excludes their registrations, strings, mailbox, IRQ hook,
retention, decoder, and observational table registry.

`ptinspect` rejects signs, octal-looking decimal strings, overflow, trailing
characters and extra/missing arguments. An absent mapping is a successful
observation. The explicit mapping status distinguishes physical address zero from
an absent entry. Unsafe access, malformed entries, and unsupported active modes
produce a failed handler result with a reason and stopping level.

Run the focused driver with an explicit directory under the repository backup
root, for example:

```
scripts/test-page-table-inspector.sh build --evidence backups/<session>/evidence
scripts/test-page-table-inspector.sh runtime --evidence backups/<session>/evidence
scripts/test-page-table-inspector.sh verify --evidence backups/<session>/evidence \
  --logfile backups/<session>/evidence/profiles/<run>/qemu-serial.log
```

The driver exposes `static`, `build`, `runtime`, `baseline`, `production`,
`fixtures`, `verify`, and `all`. Each mode records its actual work. A runtime
failure stops that invocation; the directory is never silently reused. `all`
collects or independently replays every constituent and writes acceptance only
after the fixed inherited ledger and focused fixtures succeed. The runtime
collector finalizes `series.json` itself; full-matrix and baseline scopes differ.
`verify` replays final acceptance, or one transaction when `--logfile` is supplied;
it neither rebuilds nor starts QEMU. Failed attempts remain append-only and a
resumed collector creates new evidence directories.

## Page-table access and consistency

The sampling CPU is pinned with `irq_save`. CR3 is read from that CPU and its
address bits are separated from controls; `g_kernel_pml4` is only a provenance
check. Four-level paging is supported. Active LA57 is explicitly unsupported;
canonicality is checked against the active mode. Optional MSR reads carry
independent capability/validity fields; zero is never substituted for success.

A table frame is eligible only when it is a recorded successful `alloc_table`
allocation from the existing PMM, aligned, nonzero, and wholly inside the initial
identity interval. That PMM only allocates conventional RAM. The root must be the
known kernel root or its recorded bootstrap clone. The current implementation
never frees page-table frames. Registry exhaustion, allocation failure, or a low
nonidentity/removal mapping closes this reader. This is a diagnostic boundary,
not a general physical-to-virtual conversion API. The requested VA is never
loaded as data; MMIO queries read only verified paging-structure RAM.

The paging lock protects a bounded copy of at most four aligned entries. No
formatting, serial output, allocation, or MSR access occurs under that lock.
The pure C decoder runs after release. A/D can change in hardware; the output is
an observation of entry loads, not a transaction freezing accessed/dirty bits.
The table counter uses one short critical section per non-leaf entry, deduplicates
shared descendants and rejects cycles at inconsistent levels. A software mapping
epoch detects mutations across the traversal. Current no-free lifetime is
essential; a later table-free design must replace this contract before reusing
these routines.

A leaf's PAT index is `(PAT << 2) | (PCD << 1) | PWT`. PTE PAT is bit 7;
large-leaf PAT is bit 12. The decoder handles 4 KiB, 2 MiB, and 1 GiB leaves,
physical-width limits, huge-page alignment, hierarchical RW/US/NX, NXE and WP.
Meaningless fields are `NA`. The observed PAT slot type is separate from
`effective_type=NOT_COMPUTED`. CR0 cache controls, MTRRs, fixed ranges and aliases
prevent inferring an effective type merely from a PAT selector.

Runtime samples are selected from the linker kernel start, the active allocated
root frame, a retained IOAPIC/LAPIC address whose existing leaf actually has PCD,
the handoff framebuffer, and a bounded walker-verified unmapped candidate. ELF
symbol and load facts independently bind the kernel image offset. Hosted huge
leaves exercise decoding; they are not claimed as runtime huge mappings.

## CPU request lifetime

A CAS owner serializes shell requests. The topology's actual online slot/APIC-ID
set is frozen into static storage with the existing 32-slot capacity. A generation
is published before IPIs. Each per-slot publication word encodes generation and
`pending`, `claimed`, or `done`. A target CAS claims only its pending generation,
reads local controls while pinned, and release-publishes its entire sample.
The requester acquire-loads completion and validates slot, APIC ID and required
field validity. Out-of-order completion is allowed. Duplicates cannot claim a
completed slot. Uniformity is reported independently of complete collection;
unequal valid raw controls are a successful observation.

The existing wake vector remains unchanged. Its SELFTEST hook neither prints,
allocates, waits, nor takes a requester lock. Existing interrupt statistics and
exactly one EOI remain in their original order. The fixed interrupt source anchor
is narrowly promoted in `test-assert-list.sh`; all legacy checks remain.

One send is protected by a short IRQ-save section, preserving the existing xAPIC
ICR pair. xAPIC destinations above 255 are rejected. x2APIC uses the established
send path. No acknowledgement wait occurs with interrupts disabled. The fixed
request deadline is two seconds of the existing monotonic clock, chosen before
acceptance. Startup, unavailable clock, disabled IRQs or panic ownership reject
capture. On timeout the active generation is withdrawn and storage is quarantined
until reboot. A late claimed target can finish only in permanent old storage;
no newer capture can reuse it. Only acquired completed slots are formatted.
This trades post-timeout availability for safe bounded lifetime.

## Handoff and MTRRs

Immediately after the PMM consumes the same BootInfo, SELFTEST retains only the
map base/size/stride, framebuffer range and RSDP location. The map must fit wholly
inside its UEFI LoaderData backing descriptor; the PMM does not reclaim that type.
The descriptor prefix is 40 bytes. Reported strides from 40 through 4096 are
accepted, with at most 1 MiB/4096 records, exact divisibility and checked range
arithmetic. Runtime copies are bounded and require existing identity translations.
Original order, unknown numeric types, all raw attributes and extension bytes are
preserved. No descriptor region is dereferenced merely because it is printed.

Device/firmware intervals come from retained MADT, HPET, MCFG, ACPI and handoff
metadata. The small SELFTEST `acpi_observe_root` accessor exposes existing retained
root locations and lengths; discovery/parsing is unchanged. Every intersection
names its raw descriptor and range origin. Device holes may have zero covering
descriptors. No PCI rescan or device register reads are used for correlation.

MTRR capture uses CPUID and `cpu_read_msr_safe`, up to the capability register's
255 variable pairs in static storage. Raw base/mask and per-read validity remain
visible. Masks require contiguous power-of-two sizes, aligned bases and the
observed physical width. Fixed MTRRs are explicitly `NOT_COLLECTED`; consequently
no complete effective-type claim is made below 1 MiB or elsewhere. No MSR or cache
programming sequence is added.

## Evidence boundary

`QEMU_FAULT_TRACE=1` adds `-no-reboot -d guest_errors,int,cpu_reset` and the selected
trace path. Default launches retain `guest_errors`. `QEMU_LAUNCH_RECEIPT` records
the exact argv, effective machine/accelerator/model/topology, firmware and payload
hashes, QEMU version and host CPU availability. The isolated collector uses a
socket established before boot, logging raw chunks and `monotonic_ns` receipt
positions. Timestamp resolution is 1 ns on the measured host, but serial buffering
and host scheduling are unquantified uncertainty. This is host receipt latency,
not exact guest execution time. Same-chunk markers are rejected as unresolved.
Trace/GDB diagnostic boots cannot substitute for the six required baseline boots.

Portable replay validates raw observations and their consistency. It reuses the
inherited causal input validator, adapting only its excluded-ELF boundary to saved
`nm`/`readelf` facts with candidate/hash binding. Live validation of the complete
11-command KVM capture also ran the native inherited validator against the
actual frozen ELF, without the saved-fact substitution. Omitted ELF/image behavior
is not independently proven by a text hash list. Exact binary candidates remain
in local ignored backups.

## Input identities and portable proof

Build-input and harness manifests use sorted compact ASCII JSON with a final
newline. Build inputs include the conservative kernel/bootloader/shared closure,
headers, assets, makefile and relevant build tools. The verifier recomputes the
digest and compares every safe unique path, size, mode and hash with the source
export. Git base, full exported tree, linked input closure, harness version and
ELF/image identities are separate facts. A harness-only revision requires its
fixtures and evidence replay; it does not relabel an old capture as newly run.

The stopped diagnostic captures CR3, the bounded allocation registry, raw table
frames, bootstrap/static roots and retained BootInfo/ACPI observations. A separate
host traversal deduplicates physical frames and rejects cycles, level conflicts,
malformed links, missing bytes and unclassified allocations. Claimed entry
readbacks require their exact target order, commands, stdout/stderr, exit status
and bound launch identity. A summary flag alone is insufficient.

The fixed current regression inventory maps every required family to exact modes,
profiles, repetitions and raw evidence. Empty, incomplete, duplicate, stale and
host-only substitutes fail. The three historical authority changes and the named
maintained modal contract are documented in `docs/foundation/validation-policy.md`.
Original historical modes remain available and do not claim unavailable inputs
were replayed. Human visual review remains separate from automated gates.

Review packages contain one source export and each selected real run once, plus
content-addressed synthetic deltas/recipes. Full candidate binaries remain local.
Capture mutation recipes hash raw file bytes. Assertion/STATS parser recipes
operate on strings: their generated-string identities use the same canonical
JSON serialization as `memory_evidence.identity`; the original serial-file
anchors remain raw byte hashes. Replay reconstructs those exact inputs and
requires the recorded semantic rejection, not merely an outer hash failure.
The original 23 capture mutations also retain their original verifier sources,
one original base, exact byte deltas and complete materialized-file manifests.
Their replay preserves earlier tested boundaries; current acceptance separately
uses the repaired verifier and its expanded controls. Neither fixture set counts
as a real boot. Hosted replay compiles only isolated C tests and never builds or
executes a guest kernel.
Regular archive members have literal 0644 or 0755 permissions, matched exactly
against the payload manifest. Fresh-extraction replay checks CRC, members, modes,
hashes, source identities and proof relationships without modifying originals.
