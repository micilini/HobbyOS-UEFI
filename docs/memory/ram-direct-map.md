# Verified RAM direct map

The RAM reservation is `[0xffff800000000000, 0xffffc00000000000)`. Its
64-TiB physical span is separate from the linked-image offset, both invalidation
fixture windows, the one-GiB owned window at `0xffffd00000000000`, and the future
stack reservation starting at `0xffffe00000000000`. Physical width comes from
checked CPUID, independently of the software address limit. Unsupported RAM
cannot be truncated into the reservation.

## Coverage and construction

The preserved, sorted UEFI descriptors select loader, boot-services, conventional,
runtime-services, ACPI reclaim and ACPI NVS RAM. Runtime-services pages remain
reserved; this map does not provide an EFI runtime-call facility. Reserved,
unusable, MMIO, port-space and unreported ranges are absent. The original
BootInfo framebuffer page envelope is the only early device exclusion. Later
device discovery still cannot retype covered RAM. The framebuffer remains an
owned WC-selected view, and firmware views remain owned WB read-only mappings.

The first physical two MiB and the complete physical linked image use four-KiB
leaves. Other intervals choose aligned one-GiB, two-MiB or four-KiB leaves without
rounding coverage outward. Descriptor/provenance boundaries and all enabled
variable-MTRR boundaries split intervals, including overlapping MTRRs. Fixed
ranges stay within the base-page prefix. Invalid masks, unknown address width or
unavailable cache inputs fail construction. Overlapping variable ranges admit
only identical types, UC precedence, or WB/WT precedence; other combinations
fail before activation. Enabled fixed ranges take precedence below one MiB.
WC types require the reported MTRR WC capability. The independent preactivation
checker repeats these checks from the raw inputs. This follows Intel SDM
253668-090US section 14.11.4.1 as well as the large-page boundary requirement.
MTRRs and PAT are never programmed.
RAM uses the accepted WB selector; effective framebuffer type remains
`NOT_COMPUTED`.

The low safety net still uses four-KiB leaves so existing owned-device withdrawal
can remove exact leaves. Construction fills unpublished tables in batches, with
no active-mapper lock or INVLPG per page. All construction storage is allocated
through the existing constrained PMM search below four GiB. A bounded journal
owns up to 8,192 table frames; a failed unpublished construction releases only
those frames. The owned window uses the existing transactional constructor and
retains its 514 structural tables for the boot.

An independent decoder walks the actual candidate root before activation. It
checks ownership, duplicate/cyclic links, levels, reserved bits, actual CPU
formats, WB/supervisor/NX propagation, descriptor-derived exact coverage, excluded
holes and executable image/stack/descriptor-table paths. Counts are derived
from that walk, not accepted from the builder. The activation callback is gated
by successful validation. The direct-map subtree is NX when the established
policy enables NX; the image subtree remains executable. NX-disabled boots emit
`NX_UNAVAILABLE` and do not set reserved NX bits.

## Pointer transition

The BSP transition runs before heap/AP initialization with IF clear. After CR3
readback and real reads through the new table aliases, memory-owned root/window
pointers and both bitmap owners are rebased. The translation epoch and verified
RAM allocation ceiling are then published together. Table access is arithmetic
plus immutable coverage validation; it never invokes the page walker recursively.
The old identity aliases remain available for the brief switch itself.

| Owner | Physical origin | Transition and consumers |
|---|---|---|
| PMM allocation bitmap | `g_bitmap_physical` | Rebase `g_bitmap_buffer` and `g_bitmap.buffer` together; allocator and full-span integrity use the new CPU view. |
| PMM reservation bitmap | Bitmap physical base plus original bitmap size | Rebase both reservation pointers; preserve bits, free count, lock and IF. |
| Final root and construction journal | Explicit physical table records | Replace every CPU pointer with its checked direct address; retain physical root/registry identities. |
| Managed window | 514 explicit physical table records | Rebase every structural-table pointer before owned APIs run. |
| Observer bootstrap root | Allocated after transition | Born in the current RAM domain; root allocation explicitly constrained below four GiB. |
| Adapted BootInfo/map/font header/logo descriptor | Static linked-image copies | Stay at linked addresses; do not add the direct-map base. |
| Glyph and logo backing | Original recorded physical buffers | Rederive complete RAM extents before graphics consumes them. |
| RSDP | Original physical handoff | RAM borrow when covered; otherwise a persistent owned WB/RO firmware view. |
| Framebuffer | Original physical extent | Existing discovery-bound owned WC view; never a RAM direct alias. |
| Live BSP GDT/TSS/IST and bootstrap stack | Linked image | Keep linked pointers and validate their covering image path. |
| Heap, AP GDT/TSS/IST/stacks | Allocated after transition | New headers, links and CPU objects derive direct-map pointers through the boundary. |
| DMA objects | Owned allocation descriptors | CPU view becomes high; bus/device addresses remain physical with independent AC64 limits. |
| Firmware/controller objects | Acquired after transition | Retain owned firmware/MMIO views; no driver page-table access. |
| Descriptor observation copy | Image-owned raw map plus numeric source identity | Remains image-owned; any later backing read uses checked RAM translation. |

The inverse checks complete coverage before subtracting the direct-map base.
Arbitrary high pointers, holes, device ranges and stale-epoch borrows fail with
unchanged outputs. Physical zero can have a non-null direct CPU address when
RAM coverage includes it; a null CPU input remains invalid. Image inversion is
restricted to the actual linked image, and managed views retain owned inversion.

AP CR3 is still physical and explicitly below four GiB. The copied trampoline
checks required one-GiB support and physical width before loading that root.
Its final stack parameter is the validated virtual one-past-end stack top used
by the existing 64-bit handoff. CPU-control initialization and timing are retained.

## Evidence boundaries

`rammap` emits checksummed, coalesced runs from actual direct-map leaves, original
descriptor/cache/feature inputs and pointer observations. Hardware A/D bits are
excluded from run coalescing; cache, permission, PS and NX bits are retained.
`rammap high` additionally requests real above-four-GiB storage through the
production bounded bitmap search, accesses it through the high CPU view,
exercises the private high table-access path without loading its scratch table,
and frees/reallocates the owned extent. These selectors are SELFTEST-only.

The high-memory supplemental profiles keep two GiB of guest RAM and move one GiB
above four GiB using the installed Q35 property. The fallback additionally disables
`pdpe1gb` on TCG. Launch receipts and guest descriptors/capabilities must prove the
layout; the profile name alone is insufficient. Complete current acceptance,
independent physical-table readback and the full fresh matrix remain separate
from early bring-up observations.

The frozen current performance predicate requires the median of three ordinary
boots in each SMP4 accelerator profile to be at most half the original paging
observation. This makes the requested substantial reduction explicit before
final collection. It compares unchanged host markers, retains all previous
baselines and their scope caveats, and imposes no bare-metal latency claim.

The architectural checks use Intel SDM 253668-090US, sections 5.1.4, 5.5 and
14.11.9, and the descriptor semantics of UEFI 2.10 GetMemoryMap. No third-party
kernel implementation is used. The requested roadmap filename was not present
in the inspected checkout; the supplied implementation contract defines this
increment's scope.
