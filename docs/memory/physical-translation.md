# Checked physical translation

The kernel retains its low identity map and linked image mapping. This boundary
does not create a direct map. Numeric physical addresses, CPU pointers and
device-visible DMA addresses have separate meanings even where their numbers
currently coincide.

`physical_pointer_range` accepts a nonempty physical interval and returns its
identity CPU pointer only when the entire interval has immutable descriptor
provenance and lies below the proven identity limit. RAM, firmware and device
descriptor classes remain distinct. A Reserved descriptor is firmware
provenance, never allocatable RAM. `PHYSICAL_KNOWN` still requires a usable
descriptor covering every byte; it is not a bypass. Physical zero is valid in
the pure numeric contract, but cannot produce a dereferenceable C null pointer.
All public failures preserve output arguments.

`physical_address_range` inverts identity pointers with the same whole-range
proof, or a pointer wholly within the actual linked image. Only the latter
uses the image offset. An unrelated high pointer, including an arbitrary
mapped fixture window, has no arithmetic inverse. `paging_query_range` walks
that mapping explicitly and proves physical contiguity. Its compatibility
single-address wrapper retains the old zero-on-missing behavior. New callers
use checked results, so a physical zero result is distinguishable from failure.

The pure contract checks overflow, canonical 48-bit virtual ranges, nonempty
extents, descriptor stride, wrapping descriptors, overlaps and image bounds.
Allocation and table callers additionally enforce page alignment. The maximum
physical representation accepted by this layout is 48 bits; this conservative
bound does not imply that every represented address is mapped or installed RAM.

The private early adapter reads only fixed handoff headers within the unchanged
entry mapping `[0x1000, 0x100000000)`. It validates sizes before copying full
objects. A kernel-owned immutable map copy establishes normal provenance before
PMM initialization. The early helper is sealed after publication, and the
adapter admits only one attempt. No heap, PMM, scheduler, paging lock or serial
lock is needed. Required malformed input fails before normal initialization;
absent optional font/logo remains optional. BootInfo layout and loader buffers
are unchanged. The framebuffer CPU pointer is withheld until its existing
mapping path completes; graphics receives the normal adapted descriptor.

For explicit devices, `physical_device_pointer` checks a nonempty canonical
physical range and verifies every current page through `paging_query_range`
after the caller's existing mapping. It does not classify the range as RAM or
make it allocatable. Its origin enum documents framebuffer, ACPI or PCI source;
it cannot bypass the mapping proof. Consumers validate the actual descriptor,
GAS, BAR or ECAM range and retain that numeric origin. It adds no mapping or
cache policy. The device counter counts successful checks, not unique regions.

The page walker reads next-table frames through the immutable RAM contract,
which never calls a walker or acquires a lock. Public device queries take the
paging lock once. PMM and heap conversions allocate nothing and do not acquire
the paging lock. Normal mapping changes that contradict the low identity
contract invalidate subsequent ordinary translations and allocation admission.
No supported production caller changes that mapping in this increment. The
SELFTEST TLB window remains outside it.

## Reachable allocation

Before the first PMM allocation, the exclusive page-aligned ceiling is the
smaller of the unchanged entry coverage (four GiB) and the normal identity
coverage computed from the retained map. The latter uses exactly the existing
PMM represented-span rule; an additional framebuffer/MMIO mapping never raises
the ceiling. The KVM two-GiB map can yield `0xff000000`, while the TCG map's high
Reserved descriptor yields a one-TiB bitmap span and a four-GiB allocation
ceiling. Static anchors bind the entry's four directories, linker placement and
normal map loop; runtime root/table readback checks the constructed mappings.

Single and contiguous allocation use a read-only bounded bitmap view with the
original first-fit search. The complete run must end at or below the exclusive
ceiling. Only the original bitmap is modified after a valid result. There is no
fallback above the limit. Exact bitmap dimensions, saved interrupt flags,
reservation, free and search accounting remain load-bearing invariants.

`g_total_frames` and the full bitmap/free counter still describe the entire
represented address span. `eligible_free_frames` counts free bits within the
current allocation interval; `above_ceiling_free_frames` reports represented
free bits that cannot currently be returned. Neither changes the integrity
validator's full-span invariant. The accepted word-wise validator is unchanged,
including its coherent lock and interrupt restoration.

## Address owners and unchanged mechanisms

Table links and CR3 contain physical frames; `g_kernel_pml4` is a CPU pointer and
`paging_kernel_root_physical()` supplies hardware identity. AP parameter roots
and the physical transition stack top are derived from checked full extents.
Runtime GDT/TSS/IST/stack pointers use their existing valid CPU addresses, removing
unnecessary bitwise high aliases. A one-past stack top is derived from its
validated allocation, never validated as an extra accessible byte.

ACPI reads fixed headers before full table extents and preserves checksum,
selection and parsing semantics. XSDT physical links use memcpy for unaligned
storage. PCI/xHCI retain numeric BAR, bus and DMA addresses. Only CPU pointer
formation and DMA inverse conversion change. Register widths, fences, ring
ownership and all existing mapping flags remain unchanged.

The CPU policy, cache encoder, request vector, checked sender, acknowledged TLB
state machine, timeout/quarantine, deferred capacity and single EOI remain
accepted invariants. No CPU-control or cache-selection policy changes, general
unmap, driver mapping API migration or synchronous driver shootdown are added.
See `persisted-pointers.md` for the deliberately deferred rebase responsibilities.

## Proof boundaries

Hosted tests compile actual production helpers and actual PMM wrappers. Only
privileged operations and host backing are substituted. Fake physical addresses
are distinct from host buffer pointers. The old OR/AND loss above two GiB is a
test-only reference, never a production fallback. Full-span tests allocate two
32-MiB bitmaps, not one TiB of host RAM.

`phystest` is SELFTEST-only. It exercises isolated synthetic search backing and
four real owned frames, checks whole-range pointer/walker round trips, and
releases its data frames. Its synthetic rows are explicitly labeled. It does
not exhaust live RAM or return an inaccessible high frame. Existing TLB tests
still prove real all-CPU A/B loads; pointer readback is a different predicate.
Stopped-GDB proof reads actual roots, table bytes and nested original/adapted
boot descriptors after commands. It never calls guest code or writes memory.
Retired logo pixel bytes are not dereferenced during that capture.
