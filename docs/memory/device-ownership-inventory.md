# Device memory ownership inventory

The authoritative old and new source/function anchors are in
`scripts/device-mapping-scope.json`, rooted at approved commit
`ff6eaf609611b3d4a401e739f1f45dd7451346f3`. The reviewed dirty repair input remains
tree `72d484becfd86d89c86cfad8b294e75b1a6dd0ab`. The complete cast/call inventory
is `docs/memory/source-inventory.json`; every permitted conversion has a
specific site and domain in `scripts/device-conversion-sites.json`.

| Consumer | Retained physical authority | CPU view and lifetime |
|---|---|---|
| Framebuffer | Original BootInfo descriptor, base and complete buffer size | One managed WC view in the kernel-owned BootInfo; held for the boot; graphics bytes unchanged |
| LAPIC | Selected controller base, including APIC-base MSR/MADT selection | Stable pre-SMP UC register view; delivery never withdraws its own controller |
| IOAPIC | Exact retained MADT entry and controller index | Stable UC register view covering select/window registers; numerical APIC/GSI identities unchanged |
| HPET | Retained HPET table with memory GAS and existing 0x120 register window | Stable pre-SMP register view; clock reads do not acquire mappings |
| WDAT | Retained, validated memory GAS entry and access-width envelope | Long-lived UC view; I/O-port entries remain port accesses |
| MCFG table | Retained ACPI physical table and length | WB read-only firmware view; no register cache flags on the table |
| PCI ECAM | Exact MCFG entry, segment/bus bounds and checked arithmetic | Stable non-WB register mapping acquired before enumeration/driver locks |
| xHCI BAR | Retained discovered PCI function/BAR and existing one-MiB window contract | One managed register view; capability/operational/runtime/doorbell/port subviews are checked against it; no destructive BAR sizing |
| ACPI tables | Fixed header validated before full declared extent | WB read-only managed views; overlapping headers/tables have separate VA ownership and cannot unmap one another |
| Initial RSDP | Sealed early handoff adapter and validated physical extent | Existing narrow bootstrap exception; downstream tables use firmware views |
| AP bootstrap root | Memory-owned root construction and physical root accessor | Opaque physical handoff; trampoline bytes and stack ABI unchanged |
| Page-table walker | Private, nonallocating identity-RAM primitive | Never calls the public walker recursively; no raw table type escapes memory |

Device-origin resolution reads existing discovery records and does not map a
table while validating an MMIO request. Firmware provenance alone is not MMIO
authority. The existing one-MiB xHCI register envelope is a supported contract,
not a measured PCI BAR size. Firmware RO describes these CPU views; pre-existing
RAM aliases are not claimed globally RO. Effective memory type is not computed.

All xHCI device-visible memory is allocated through the same controller DMA
constraint. `HCCPARAMS1.AC64` selects an inclusive `UINT32_MAX` or `UINT64_MAX`;
the independent PMM reachability ceiling still constrains the complete backing.
The descriptor retains CPU pointer, bus address, requested/backing sizes,
alignment, generation and lifetime. Checked bus offsets never use image-offset
subtraction or a cast from a CPU pointer.

| xHCI object/data path | Allocation and address source | Retirement/retention |
|---|---|---|
| DCBAA | Contiguous, page-aligned owned allocation; bus base in DCBAAP | Persistent controller owner |
| Scratchpad pointer array | Owned allocation with at least 64-byte alignment | Persistent controller owner |
| Scratchpad pages | Individually owned aligned pages; descriptor bus addresses in array | Persistent controller owner |
| Command ring and link TRB | Owned page backing; checked ring/element bus offsets | Existing ring cycle/doorbell protocol; retained |
| Event ring and ERST | Owned contiguous ring and aligned segment-table allocation | Retained; ERDP uses checked event-element bus address |
| Input/output device contexts | Owned backing covering the actual 32/64-byte context stride | Existing persistent slot ownership; no hotplug reclamation added |
| Endpoint-zero ring | Owned page with unchanged ring geometry | Reusable only at established TD/event retirement; uncertain lane quarantined |
| Interrupt/keyboard rings | Owned aligned pages; checked link/device offsets | Persistent slot owner |
| Pipelined keyboard buffers | Owned pages and bounded report subviews | Remain device-owned under the existing interrupt ring protocol |
| LED/output report | Owned temporary transfer storage | Completion retires data; error/timeout retains uncertain storage |
| Device/configuration descriptors | Owned transfer buffers | Parse/copy only after completed transfer; CPU-only configuration copy remains heap memory |
| Hub descriptors | Owned transfer storage, unchanged 9/12-byte wire requests | Bounded descriptor copy and completed-buffer release; no stack DMA |
| Hub port status | Owned four-byte transfer | Parse completed bytes into ordinary output, then release |
| Setup packet | Immediate-data TRB copy | Local CPU structure is not DMA storage; zero-data stages allocate nothing |

The private transfer path rejects an unowned stack or heap buffer before any
TRB or doorbell publication. A busy EP0 lane cannot interleave setup/data/status
entries. Slot/endpoint equality alone cannot retire a transfer: the handler
checks the hardware event's TRB address and the retained slot lifecycle. A
successful terminal event must also be consumed before release. Uncertain
completion leaves both lane identity and data retained, and subsequent transfer
allocation fails closed instead of accumulating hidden replacements.

The root record owns only the root-attached slot. Hub children retain parent,
downstream port, route, speed, translator ancestry and both root/slot generations.
Callbacks use immutable tickets, with one pending operation per owner; no timer
callback may act on a replacement lifecycle. Short state locks never span
descriptor transfers, slot commands, sleeps or descendant enumeration. A failed
schedule or failed configuration cannot publish input readiness.

Mapping structural storage is a fixed 514-frame subtree, retained for the boot.
Compatible physical overlap receives independent VA views, so firmware-header
release cannot remove a full table or a sibling subpage user. Public installs
preflight every leaf and distinguish exact idempotence from physical/cache/
permission conflict, ignoring only hardware A/D differences. First adoption is
a separate memory-owned operation with positive origin checks. Failure after
publication quarantines the view and VA; it never returns frames or restores
old entries while assuming remote translations were restored.

Persisted pointers remain explicitly owned: boot-view framebuffer, ACPI retained
tables, controller register subviews, DMA descriptors and driver ring/context
references. A future RAM layout must derive CPU pointers from those owners and
physical identities; it cannot reinterpret managed-window pointers through the
linked-image offset. No direct map or relocation framework is implemented here.

The historical PC profile retains its legacy keyboard and has no explicit xHCI
device. Its synthetic owned DMA/unmap workload still runs; it must report zero
live controller DMA/USB ownership. Q35 direct-keyboard and the separate sole
hub-keyboard profiles provide actual controller coverage. A PC result cannot be
relabeled as hub or xHCI proof.
