# Persisted physical origins and CPU pointers

No runtime rebase occurs in this increment. The published contract has one
immutable epoch. Borrowed pointers are valid only while their owner and this
mapping epoch remain valid. The pure epoch helper rejects a retained borrow
from an older simulated contract; it is not a live relocation framework.

| Pointer/storage | Physical origin and current meaning | Owner/stage/consumers | Later derivation responsibility |
|---|---|---|---|
| PMM allocation/reservation bitmap buffers | Retained `g_bitmap_physical` and complete byte span; identity CPU buffers | PMM init; bitmap search/update/integrity under PMM lock | Re-derive both buffer pointers from the retained base before publishing another mapping epoch; reservation stays physical |
| Kernel BootInfo view | Original handoff and all nested origins in `boot_physical_objects_t`; copied descriptors/map live in linked image | Private early adapter, before first consumer; kernel/graphics/PMM/firmware | Rebuild nested derived pointers from retained extents; preserve optional absent/retired objects and never mutate loader memory |
| Font header/glyphs | Copied header in image; glyph physical base and exact mode/character extent retained | Boot view then console/graphics | Re-derive glyph pointer and any cached console font view before consumer release |
| Logo descriptor/pixels | Copied descriptor; original physical extent retained separately | Splash owns pixel lifetime; boot view clears logo after release | Never revive a retired pixel pointer; only live owned data is eligible for re-derivation |
| Framebuffer descriptor/BaseAddress | Original descriptor, physical base, size and geometry retained; CPU pointer activated after mapping | Boot adapter then graphics; graphics sources unchanged | Re-derive BaseAddress after future mapping proof, including cached graphics state; no cache-type change here |
| Immutable EFI map copy | Original raw physical identity retained; actual map copy is image BSS | Boundary/PMM/observer | Image pointer remains image-relative; original source identity is provenance, not an allocation owner |
| ACPI RSDP and selected tables | Firmware physical fields; checked header/full-extent identity pointers | ACPI init, global RSDP/FADT and downstream MADT/MCFG/HPET/watchdog | Retain physical table identity or derive it before rebuilding globals; ports in GAS stay port numbers |
| Heap regions, links and aligned headers | Original PMM physical backing converted once; all links/registered intervals are CPU pointers | Heap expansion under existing lock; task/driver allocations | A future map transition must rebase region/link/header and external live pointers together; no allocator algorithm change now |
| Active and bootstrap paging roots | Physical root retained separately from CPU pointer; links always contain physical frames | Paging init and observer/TLB root checks | Re-derive table CPU access; never transform CR3/PTE/root operands into virtual addresses |
| Observer table registry and copied records | Registry contains physical frame numbers; scratch output is CPU-owned memory | Read-only observation; bounded table access | Registry numerics remain physical; only its table access boundary is replaced |
| GDT/TSS/IST/runtime and AP stacks | Heap-owned valid CPU pointers; physical bootstrap stack derived from full contiguous interval | Topology preparation, AP startup, GDTR/TSS and scheduler | Rebuild descriptors and per-CPU references only under a separately proved quiescent transition; one-past tops derive from base+size |
| LAPIC/IOAPIC register pointers | Numeric firmware/controller physical base retained; checked mapped CPU pointer cached | Existing controller mapping/init; interrupt paths | Re-derive cached register pointers before interrupt/device use; keep vector/routing/ICR ownership unchanged |
| HPET register pointer | Retained physical GAS base; checked CPU register extent | Clock initialization; read-only hot clocksource access | Re-derive before clock use; never add a page walk or lock to the high-low-high read path |
| PCI ECAM/MSI references | Numeric physical ECAM/capability locations retained; CPU pointers borrowed for complete register extents | Existing enumeration/MSI functions | Replace only CPU derivation; bus numbers, BAR data and capability offsets remain numeric |
| xHCI BAR/rings/contexts/buffers | BAR physical base and DMA numerics retained; checked CPU pointers for complete owned buffers/register windows | Driver init and existing lifetime protocol | Re-derive CPU pointers, leave bus-visible descriptors physical; coordinate all retained ring/context pointers in a future driver migration |
| Watchdog memory GAS pointers | Validated ACPI physical register interval; short-lived checked CPU pointer | Existing WDAT action | Derive each borrow after its existing mapping; port GAS remains I/O-port data |
| TLB scratch page tables/data | Tables explicitly retained at fixed bounded cost; A/B frames physical plus separate CPU pointers | SELFTEST fixture only | Re-derive live scratch access; preserve request/read retirement before data release; do not call retained table pages freed |
| Acknowledged requests/deferred queue | Stable copied physical-root and virtual-interval operands; owned static storage | Production request/worker | Keep address domains and generation lifetime; an epoch change cannot silently waive root checks |

The exact cast sites and mapping producers are in
`scripts/physical-conversion-sites.json` and `source-inventory.json`. The scanner
requires every current site, including nonphysical object casts, to match its
specific source anchor and disposition. Historical aliases remain only in old
documents or explicitly synthetic test references.
