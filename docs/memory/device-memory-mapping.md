# Owned device and firmware mappings

This contract starts at commit `ff6eaf609611b3d4a401e739f1f45dd7451346f3`.
The repair starting source is the uncommitted tree
`72d484becfd86d89c86cfad8b294e75b1a6dd0ab`. The CPU policy, request transport,
PMM integrity algorithm and entry assembly retain their existing invariants.
USB changes are limited to attachment ownership, control-transfer retirement,
bounded descriptor copying and parent/translator context.

The managed window is `[0xffffd00000000000, 0xffffd00040000000)`. It is disjoint
from the linked image, low identity RAM and the scratch TLB windows beginning
at `0xffffc000001ff000`. The `0xffff800000000000` region remains reserved for a
future RAM layout, and `0xffffe00000000000` for a future stack layout; neither
is mapped by this work. The window has a static extent bitmap and 512 owned
views. Its 514 structural table frames are allocated before publication and
retained for the boot. Allocation failure releases only unpublished frames.
A fixed structural cost avoids runtime table allocation, partial publication
and table retirement races. Independent compatible aliases have independent
VA reservations, so releasing a header cannot remove another full-table view.

Public register mappings admit UC, UC- or WC with a discovered physical
origin and a complete page-rounded extent. `mmio_origin_resolve` reads retained
BootInfo, selected LAPIC, MADT, HPET GAS, WDAT GAS, MCFG or discovered xHCI BAR
facts. Acquisition resolves the original record again, checks its generation,
kind, byte extent and page envelope, then excludes conflicting ownership.
An arbitrary nonzero source or a firmware descriptor is not device authority.
Rejected claims do not reserve a VA, allocate memory, alter page tables or
submit maintenance. These mappings reject RAM/image overlap and
incompatible existing views. Firmware views are WB and read-only. A view owns
its virtual reservation, not the firmware or device bytes. DMA buffers own
contiguous WB RAM; their inclusive device limit and the exclusive reachable
RAM ceiling both constrain the last backing byte. CPU and bus addresses are
stored separately. The entire backing is zeroed before publication.

The first device adoption withdraws only the validated device's old low
aliases, completes translation invalidation, installs the new managed leaves,
then completes cache writeback/invalidation before returning a pointer. The
original entry root is inactive; the AP bootstrap root shares the current low
subtree. No incompatible live alias is permitted. This follows the distinct
translation/cache obligations in Intel SDM Volume 3A, section 14.12.4
([pinned manual](https://cdrdv2-public.intel.com/874249/253668-090-sdm-vol-3a.pdf)).
No PAT or MTRR write is part of the transition. Cache selection is not a claim
about effective memory type.

Before the explicit SMP seal, operations use the checked local path and
preserve IF. After that seal, acquire/release require eligible IF-enabled
thread context. A serialized admission token may span completion; no target
handler takes it. Metadata and paging locks are released before any remote
wait. Drivers acquire stable register views before taking their own locks.
LAPIC and HPET adoption occurs at their existing pre-SMP boundary, since the
runtime protocol itself needs their stable views. An uncertain completion
quarantines the descriptor and VA; it never permits resource reuse.

The device-initialization worker records its actual task ID and lifecycle
generation in one atomic boot line. Production collection waits for that ID's
existing reaper record before testing the empty serial interval of an unknown
command. The reaper's generation is a zombie generation, a separate identity;
the collector does not equate them or join interleaved historical task logs.
This adds no scheduler or retirement behavior.

DMA ownership distinguishes software-owned, persistent, in-flight, completed
and uncertain resources. CPU acknowledgement cannot prove device retirement.
A known transfer completion permits temporary storage release; a timeout
retains storage and fails further unsafe use. Immediate setup TRB data is a
copy and needs no DMA allocation. Every data-stage pointer must resolve to an
owned DMA extent before hardware publication. The actual xHCI AC64 bit selects
the device address constraint, independently of PCI BAR decoding width
([xHCI specification, Table 5-13](https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/extensible-host-controler-interface-usb-xhci.pdf)).

Root ownership is claimed before either the boot scan or a runtime event can
enumerate the attachment. A configured root ignores duplicate connected and
reset-completion notifications. Disconnect invalidates the generation; timer
and DPC callbacks carry single-consumption tickets, not an unqualified pointer
to a mutable port. Failed scheduling is an explicit retained/error state.
Child publication validates its parent, downstream port, root generation and
slot lifecycle, and never writes the root-attached slot. Hub polling remains
enabled and must continue across repeated real keyboard commands.

One controller-owned EP0 lane serializes admission before any ring mutation.
It binds the current slot lifecycle, endpoint 1 and the actual setup/data/status
TRB device addresses. A successful status event is first observed, then retired
only after the existing event consumer advances ERDP and executes its fence.
Only that terminal boundary releases the lane and temporary DMA storage.
Unmatched, duplicate and stale events cannot complete another request. A short
data-stage event is not status-stage retirement. Errors or timeouts with
uncertain hardware retirement quarantine the lane and storage; clearing a flag
cannot make the ring identity safe to reuse. No scheduler task pointer is held.

Translator selection uses actual xHCI speed IDs and retained upstream ancestry.
A full-speed parent without a high-speed ancestor supplies no TT. Low/full-speed
children of a high-speed parent use that parent's slot and downstream port;
nested full-speed hubs retain the existing upstream translator. Route nibbles
start with the root-nearest downstream port. The local rule follows xHCI 1.2b
section 4.3.7 and the slot-context route requirement
([primary specification](https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf)).
The seven-byte USB2 hub descriptor structure receives seven bytes from the
unchanged nine-byte wire request; the retained bitmap bytes cannot overwrite
the caller's stack. Setup immediate data remains CPU-only.

The SELFTEST unmap probe owns its DMA frame and managed VA. It releases the
paging lock before target-all invalidation, then briefly pins only the exact
deliberate read. Recovery requires the registered RIP, CR2, generation, local
slot and non-present supervisor-read error value zero. Other faults take the
existing panic path; the #GP/MSR fixup is unchanged. The VA is held until the
probe retires, and the frame is released afterward. Production contains the
unmap service but none of the probe, workload, audit or reference selectors.
Each cycle records distinct buffer/view generations and both stale-handle
rejections after release. Its retirement total comes from its own completed
allocations and releases. System-wide free-memory snapshots are retained as
observations; hub polling and reaper activity can change them concurrently.
Such activity cannot excuse an outstanding fixture handle, view or byte.

Service trace schema 6 retains a bounded, nonoverwriting boot audit. Admission
and terminal facts come from the real request service, with independent
pre-admission checkpoints around each workload. Mapping maintenance preceding
or interleaved with fixture requests is accounted for by its actual generation,
root, operation, target identities and result. Missing records or overflow fail
replay. Sampling still performs real loads without incidental invalidation.
Earlier schemas keep their original initial-generation requirement.

The independent observer uses retained ACPI mappings only. DSDT acquisition
occurs during ordinary ACPI initialization, before the read-only observation
commands; observation cannot silently create a view or change a table epoch.
The separate stopped-debugger run reconciles all table frames and owned ranges.
Production readback walks actual tables without compiling the observer into
the production image. It reads RAM metadata, not device register contents.

The framebuffer has PAT-selected WC over the original full byte extent, with
every incompatible low alias withdrawn. Effective MTRR/PAT type remains
`NOT_COMPUTED`. The paired SELFTEST reference selects the historical slot 4
through the same safe ownership path and cannot satisfy production or WC
acceptance. Its benchmark times the unchanged `swap_buffers` plus SFENCE using
HPET, with three warmups and eight samples per boot; checksums are outside the
timed interval. Emulation and scheduling uncertainty is unquantified, and no
speedup is an acceptance requirement.

`scripts/test-mmio-dma-mapping.sh` composes the existing observation, control,
invalidation, translation and seventeen-family native contracts. A hosted test
or limited historical replay is not complete current acceptance. The delivery
report and identity-bound receipts supply the actual campaign outcome. Original
hub failures remain failed controls under their original candidate identities.
Historical unavailable demonstrations, human visual review and deferred
hardware acceptance remain separate.
