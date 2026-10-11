# Address consumers

The immutable navigation base is
`c5e6cf021eee9df489216402c5503b9c38da7df5`. Unmodified consumer line numbers below
refer to that revision. `source-inventory.json` also anchors all scanned files by
SHA-256, including the new diagnostic files. Regenerate with
`python3 scripts/memory-source-inventory.py --output <evidence>/inventory.json`.
Its balanced-parenthesis scan excludes comments, strings, definitions and
prototypes, handles multiline calls, and reconciles 36 external `paging_map`
calls (24 xHCI) and 29 external translations (all xHCI). The JSON contains every
call's function, unique expression, arguments, origin, flags and classification.
It also retains broad cast/allocation/offset scans for audit; ordinary task-stack
and container casts are not automatically classified as physical conversions.

## Physical pointer and stored-address consumers

| Source / anchors | Existing physical-address access and stored state | Future translation risk |
|---|---|---|
| `bootloader/src/kernel_loader.c`, `p_paddr`, page alignment helpers | UEFI AllocatePages at ELF physical destinations; file reads and zero-fill use those addresses as pointers | Loader image placement and kernel image relocation must stay distinct from RAM translation |
| `bootloader/src/mem.c`, `get_memory_map`; `starter.c`, `AllocatePool` | LoaderData map object, variable-stride descriptor array and copied BootInfo survive ExitBootServices | Retained physical pointers depend on existing accessibility and allocation lifetime |
| Bootloader graphics/font/logo helpers; `shared/protocol.h` | Framebuffer, PSF font/glyph buffer and logo buffers are passed through BootInfo | Rebase owners deliberately if identity access is later removed; framebuffer is device memory |
| `kernel/kernel.c`, `kernel_main_high`; `core/kernel_init.c`, `init_system_core` | BootInfo and nested serial/map/framebuffer/RSDP fields are read directly | High kernel entry does not imply rebased handoff pointers |
| `core/entry.S`, `boot_pml4`, `boot_pdpt_low/high`, `boot_pd0..3` | ELF-resident bootstrap structures and boot stack used through image physical/virtual transitions | These static buffers are not PMM table allocations |
| `cpu/control_policy.c`, `cpu_policy_bootstrap_map`, `cpu_policy_init_bsp` | Read the seven known contiguous static bootstrap frames through the existing `boot_pml4` symbol; compare CR3 to that physical root before the local PAT transition | The bounded validator checks exact links without following input addresses; it establishes no general physical-pointer API |
| `memory/paging.c`, `alloc_table`, existing walker and bootstrap clone | PMM frames cast to PageTable; table entry address masks become pointers; `g_kernel_pml4` and bootstrap CR3 persist | No general direct map exists; zero-return translation API remains ambiguous |
| `memory/pmem.c`, `find_free_segment_for_bitmap`, `g_bitmap_buffer`, `g_reserved_bitmap_buffer` | Conventional descriptor PhysicalStart becomes bitmap backing pointers; reserve/free address arithmetic uses physical frame numbers | Bitmap ceiling is not an allocation-wide ceiling; reserved high map endpoints can inflate frame count |
| `memory/heap.c`, `heap_expand_locked`, `g_heap_regions`, block list | Contiguous PMM frames become HeapBlock pointers and linked heap regions; aligned allocations retain headers | Heap/DMA identity dependence is inherited, not a virtual allocator guarantee |
| `smp/smp_topology.c`, `map_and_convert`, `g_cpus` | Heap stacks/TSS/GDT/IST gain high OR aliases; derived pointers persist in SmpCpuInfo and descriptors | OR/masking is not reversible translation for arbitrary physical widths |
| `memory/gdt.c`, `to_higher_half`, `gdt_load_cpu`; `smp/smp_boot.c`, trampoline patch pointers | Physical heap/GDT pointers are aliased; low trampoline bytes and parameter words are accessed directly | BSP/AP bootstrap roots share descendants; stack and descriptor addresses have distinct roles |
| `smp/trampoline.S`, `smp/ap_entry.S` | Low physical bootstrap parameters, CR3 and temporary stack; transition to high entry/stack | Preserve phase-specific physical versus virtual handoff; no migration here |
| `acpi/acpi.c`, retained RSDP/XSDT/RSDT/FADT; `acpi_get_dsdt`, table pointer arrays | Root entries and FADT DSDT fields become C pointers, retained across runtime | Firmware placement comes from each boot, not a permanent below-4-GiB promise |
| `acpi/madt.c`, MADT entry traversal and cached controllers | Table RAM is traversed; LAPIC/IOAPIC physical addresses retained as facts | Table access and device register access require separate boundaries |
| `drivers/pci.c`, MCFG, `function_addr`, `device_addr`, capability walks | ECAM arithmetic becomes config-space pointers; MSI capability byte/word access uses stored bases | MCFG is firmware RAM; ECAM is MMIO. Two MCFG remaps have different current flags |
| `apic/lapic.c`, `g_lapic_base`; `apic/ioapic.c`, controller `mmio_base` | Physical controller bases become register pointers in MMIO helpers | x2APIC active mode does not guarantee the LAPIC identity leaf has PCD |
| `timer/hpet.c`, `g_hpet_base`, register offsets | ACPI GAS physical address becomes MMIO pointers, including high-low-high clocksource reads | Accessor names do not imply read-only metadata; runtime snapshot helpers can read devices |
| `drivers/watchdog/acpi_wdat.c`, `wdat_ensure_mapped`, GAS address | WDAT system-memory GAS is identity mapped then accessed via volatile pointer; I/O-space GAS remains port access | Firmware-provided address-space IDs and widths must remain explicit |
| `drivers/usb/xhci/xhci.c`, `xhci_driver`, `out_phys`, `xhci_map_identity_range` | BAR-derived cap/op/runtime/doorbell/extended-capability pointers; DCBAA physical entries become CPU context pointers; persistent ring/buffer pointers and physical twins | MMIO, DMA CPU pointers and bus addresses are mixed; all require deliberate future separation |
| `cpu/mmio.h` read/write helpers | Caller-provided register addresses become volatile pointers | Helpers do not themselves establish RAM/MMIO provenance or mappings |
| `graphics/graphics.c`, console/font and `graphics/splash.c` consumers | Stored framebuffer/font/logo physical-derived pointers remain direct C-accessible addresses; splash release returns owned physical frames | Protected graphics files are unchanged; handoff ownership cannot be inferred from mapping presence |
| New memory observation modules | Recorded table-frame pointers and retained LoaderData map are read through bounded provenance checks | Diagnostic-only boundary; does not license arbitrary physical pointer conversion |
| `memory/kernel_tlb.c`, `read_cr3`, `reload_cr3`; `smp/cpu_request.c`, `g_request_root` | Retain the actual shared physical root as immutable request identity; reload each CPU's current CR3 only after root and CR4 checks | Invalidation establishes completion, without translating pointers or changing mappings |
| `memory/tlb_fixture.c`, `prepare`, `tlb_fixture_load`; `paging.c`, `paging_tlb_fixture_*` | SELFTEST-owned PMM data frames retain existing identity access; two fixed high virtual windows use checked owned leaf entries and real volatile loads | Data frames stay owned until invalidation and reader retirement; fixed scratch tables remain owned between invocations |

The accepted control-policy addition preserved every existing mapping producer
and DMA consumer. At that revision, `memory/paging.c` changed only by removing
its former BSP PAT writer; its accepted hash was
`3d9bcc33520e42afc13315424c8f527279a5d725ca2e52c6041f2669ae4c3503`.
The new cache encoder has test callers only. It returns cache-selection bits,
including the distinct large-leaf PAT position, without mapping a page or
claiming an effective MTRR/PAT type. Slot 5 becomes representable as WC;
framebuffer mappings continue to select slot 4. The source inventory and stopped
table traversal separately prove that existing leaves and links do not select
slot 5. See `cpu-control-state.md` for the local register transition contract.

The acknowledged-invalidation addition preserves those production mapping
functions and all DMA callsites. Its only replacement and cleanup operations
belong to the fixed SELFTEST windows described in `tlb-coherency.md`. The current
source inventory carries their source hashes, allocation and cast anchors;
the scope contract independently pins all pre-existing paging function bodies.
The public invalidation interval is copied into static request storage and does
not grant mapping or frame ownership. No physical-pointer consumer was migrated.

Scheduler/task/queue pointers and generic libc copies in the broad cast scan
normally originate in existing heap objects. Their physical dependence is inherited
through heap backing, rather than a second explicit conversion. Their ownership,
stack guards, canaries, scheduler policy and recovery mechanisms remain unchanged.

## Offsets and masks

`source-inventory.json` lists every code occurrence, including definitions.
`HHDM_OFFSET=0xffffffff80000000` is duplicated in `smp_boot.c`,
`smp_topology.c`, and `gdt.c`. The topology helper ORs the offset into a heap
pointer and masks it back while mapping; SMP boot masks a stored stack top back
to a physical bootstrap value. The GDT helper ORs low pointers. These operations
do not establish a general direct map. `KERNEL_VIRT_OFFSET` in paging and
`VIRT_TO_PHYS_OFFSET` in PMM describe kernel image placement. The linker and entry
assembly likewise subtract/add the image offset. Page-entry masks and CR3 masks
extract addresses within architectural formats; they are not general pointer
rebasing. All definitions and their consumers are retained.

## DMA ownership, alignment and width

All allocator call sites, including calls that never invoke paging, are retained
in the JSON. The xHCI DMA allocation groups below cover its 20 explicit allocation
sites (18 heap sites and two physical-frame sites); a single call site may allocate many
per-slot buffers. This is a source count, not an allocation count at runtime.

| Function / allocation anchor | Owner, lifetime and initialization | Address-width handling |
|---|---|---|
| `xhci_configure_endpoint_irq`: `ring_phys_ptr`, `buf_phys_ptr` | PMM contiguous page each, 4096 aligned; ring zeroed 4096 bytes, each input buffer first 64 bytes; per-slot keyboard ring/pipeline storage persists | No DMA-specific ceiling; physical pointer and CPU pointer are set equal |
| Same: `input_ctx` | Heap aligned 4096, minimum 4096 bytes, zeroed full context; configure command temporary context; no explicit free in this file | Existing translation is published, no bus-width allocation constraint |
| `xhci_set_leds`: `buf` | Heap 64-byte size/alignment; retained per-slot; one LED byte assigned before transfer, not full-buffer zeroing | Translation is used directly |
| `xhci_configure_device`: `dev_ctx`, `input_ctx`, `tr_ring` | Heap 4096 aligned, whole context/ring zeroed; DCBAA output context, input context, persistent EP0 ring | No enforced DMA width/ceiling |
| `xhci_alloc_dcbaa`: `dcbaa_virt`, `sp_array_virt`, `buf_virt` | DCBAA 4096 aligned; scratchpad pointer array 64 aligned; each scratchpad 4096 aligned; all zeroed; controller lifetime | No enforced DMA width/ceiling |
| `xhci_init_command_ring`: `ptr` | Heap 4096 aligned, whole ring zeroed; persistent controller command ring | Register high/low address publication does not constrain allocation |
| `xhci_init_event_ring`: `ring_virt`, `erst_virt` | Ring 4096 aligned; ERST 64 aligned; both fully zeroed; persistent controller storage | No enforced DMA width/ceiling |
| `xhci_evaluate_context`: `input_ctx` | Heap 4096 aligned, 4096 zeroed; copies selected output context from DCBAA physical pointer | No enforced DMA width/ceiling |
| `xhci_get_descriptor_device`: `buffer` | Heap size/alignment 64, zeroed; descriptor copied to caller; no explicit free in this file | Single starting translation |
| `xhci_get_config_descriptor`: `buf_head`, `buf_full` | Header size/alignment 64; full descriptor allocation aligned 4096, zeroed; returned buffer retained by callers | Page remap assumes consecutive physical addresses from starting translation |
| `xhci_configure_device_with_context`: `dev_ctx`, `input_ctx`, `tr_ring` | Same aligned/zeroed context/ring families for hub-aware devices | No enforced DMA width/ceiling |
| `xhci_control_transfer`: caller `data` | Data pointer translated without allocation; setup packet is immediate TRB data, not a DMA buffer | Single starting translation assumes transfer accessibility/contiguity |
| `usb_hub_get_descriptor`: stack `buffer[16]`; `usb_hub_get_port_status`: stack `buffer[4]` | Automatic buffers zeroed; synchronous control transfer borrows them during call; no paging call in hub code | Ordinary stack alignment only; no DMA-specific width/contiguity boundary |

No DMA width enforcement was found in these allocation paths. That observation is
not permission to introduce a DMA allocator, repair remaps, adjust cacheability,
or change transfer lifetimes in this increment.

## Firmware descriptor use versus placement

`pmem.c:is_ram_type` uses Reserved, LoaderCode/Data, BootServicesCode/Data,
RuntimeServicesCode/Data, ConventionalMemory, ACPIReclaimMemory and ACPIMemoryNVS
to determine the highest endpoint. Only ConventionalMemory supplies free frames
and bitmap storage. MMIO is excluded. This classification is source behavior,
not a claim that every Reserved interval contains RAM. The diagnostic handoff
reader additionally requires LoaderData backing and prints every numeric type.

Measured ACPI/framebuffer/device placement is recorded per capture in
`baseline.md` and the raw `memmap` descriptors/intersections. There is no measured
i9 placement or hardware AP control baseline.

## External mapping and translation call inventory

The complete expressions are below; detailed flags and origins are also in the
machine-readable inventory. Flags P/RW grant presence and leaf writable state;
US is clear. The PAT bit and the three-bit PAT slot index are recorded separately.
For example, PAT=0, PCD=1, PWT=1 selects index 3. The scanner evaluates the
actual flag definitions and local flag expressions, retaining their source anchors.
PCD/PWT select PAT slots with the observed PAT, not independently
resolved effective cache types. MCFG remapping is classified as firmware access,
not device-register access.

| Path:line / function | Unique call anchor | Purpose / origin; flags |
|---|---|---|
| `kernel/src/apic/ioapic.c:139` / `ioapic_init_all` | `paging_map(base_page, base_page, PAGE_PRESENT \| PAGE_RW \| PAGE_PCD \| PAGE_PWT)` | MMIO; MADT controller address; P=1 RW=1 US=0 PCD=1 PWT=1 NX=0; PAT bit=0; PAT slot index=3 |
| `kernel/src/apic/lapic.c:192` / `init_lapic` | `paging_map(base_page, base_page, PAGE_PRESENT \| PAGE_RW \| PAGE_PCD \| PAGE_PWT)` | MMIO; MADT controller address; P=1 RW=1 US=0 PCD=1 PWT=1 NX=0; PAT bit=0; PAT slot index=3 |
| `kernel/src/drivers/pci.c:207` / `pci_enumerate_function` | `paging_map(function_addr, function_addr, PAGE_PRESENT \| PAGE_RW \| PAGE_PCD)` | MMIO configuration space; MCFG base plus bus/device/function offsets; P=1 RW=1 US=0 PCD=1 PWT=0 NX=0; PAT bit=0; PAT slot index=2 |
| `kernel/src/drivers/pci.c:280` / `pci_enumerate_device` | `paging_map(device_addr, device_addr, PAGE_PRESENT \| PAGE_RW \| PAGE_PCD)` | MMIO configuration space; MCFG base plus bus/device/function offsets; P=1 RW=1 US=0 PCD=1 PWT=0 NX=0; PAT bit=0; PAT slot index=2 |
| `kernel/src/drivers/pci.c:321` / `pci_init` | `paging_map((uint64_t)mcfg, (uint64_t)mcfg, PAGE_PRESENT \| PAGE_RW)` | Firmware table redundant identity remap; Existing ACPI MCFG table pointer (RAM), not ECAM; P=1 RW=1 US=0 PCD=0 PWT=0 NX=0; PAT bit=0; PAT slot index=0 |
| `kernel/src/drivers/pci.c:355` / `pci_list_function` | `paging_map(function_addr, function_addr, PAGE_PRESENT \| PAGE_RW \| PAGE_PCD)` | MMIO configuration space; MCFG base plus bus/device/function offsets; P=1 RW=1 US=0 PCD=1 PWT=0 NX=0; PAT bit=0; PAT slot index=2 |
| `kernel/src/drivers/pci.c:388` / `pci_list_device` | `paging_map(device_addr, device_addr, PAGE_PRESENT \| PAGE_RW \| PAGE_PCD)` | MMIO configuration space; MCFG base plus bus/device/function offsets; P=1 RW=1 US=0 PCD=1 PWT=0 NX=0; PAT bit=0; PAT slot index=2 |
| `kernel/src/drivers/pci.c:426` / `pci_list_devices` | `paging_map((uint64_t)mcfg, (uint64_t)mcfg, PAGE_PRESENT \| PAGE_RW \| PAGE_PCD)` | Firmware table redundant identity remap; Existing ACPI MCFG table pointer (RAM), not ECAM; P=1 RW=1 US=0 PCD=1 PWT=0 NX=0; PAT bit=0; PAT slot index=2 |
| `kernel/src/drivers/pci.c:440` / `pci_list_devices` | `paging_map(base, base, PAGE_PRESENT \| PAGE_RW \| PAGE_PCD)` | MMIO configuration space; MCFG base plus bus/device/function offsets; P=1 RW=1 US=0 PCD=1 PWT=0 NX=0; PAT bit=0; PAT slot index=2 |
| `kernel/src/drivers/usb/xhci/xhci.c:433` / `xhci_configure_endpoint_irq` | `paging_map(ring_virt, ring_phys, PAGE_PRESENT \| PAGE_RW)` | DMA PMM identity mapping; pmm_alloc_contiguous_frames(1); P=1 RW=1 US=0 PCD=0 PWT=0 NX=0; PAT bit=0; PAT slot index=0 |
| `kernel/src/drivers/usb/xhci/xhci.c:467` / `xhci_configure_endpoint_irq` | `paging_map(buf_virt, buf_phys, PAGE_PRESENT \| PAGE_RW)` | DMA PMM identity mapping; pmm_alloc_contiguous_frames(1); P=1 RW=1 US=0 PCD=0 PWT=0 NX=0; PAT bit=0; PAT slot index=0 |
| `kernel/src/drivers/usb/xhci/xhci.c:493` / `xhci_configure_endpoint_irq` | `paging_get_physical_address((uint64_t)input_ctx)` | DMA physical-address publication or ring completion matching; Existing xHCI CPU pointer; zero is ambiguous in the legacy API; Returns legacy physical address, with zero ambiguity |
| `kernel/src/drivers/usb/xhci/xhci.c:494` / `xhci_configure_endpoint_irq` | `paging_map((uint64_t)input_ctx, inp_phys, PAGE_PRESENT \| PAGE_RW)` | DMA existing heap mapping reinstalled; kmalloc_aligned result and its existing physical translation; P=1 RW=1 US=0 PCD=0 PWT=0 NX=0; PAT bit=0; PAT slot index=0 |
| `kernel/src/drivers/usb/xhci/xhci.c:510` / `xhci_configure_endpoint_irq` | `paging_map(out_phys + off, out_phys + off, PAGE_PRESENT \| PAGE_RW)` | DMA physical-pointer identity remap; DCBAA output-context physical address or device-context page; P=1 RW=1 US=0 PCD=0 PWT=0 NX=0; PAT bit=0; PAT slot index=0 |
| `kernel/src/drivers/usb/xhci/xhci.c:648` / `xhci_drain_events` | `paging_get_physical_address( (uint64_t)&xhci_driver.event_ring[xhci_driver.event_ring_dequeue_idx])` | DMA physical-address publication or ring completion matching; Existing xHCI CPU pointer; zero is ambiguous in the legacy API; Returns legacy physical address, with zero ambiguity |
| `kernel/src/drivers/usb/xhci/xhci.c:696` / `xhci_recover_command_ring` | `paging_get_physical_address((uint64_t)xhci_driver.cmd_ring_base)` | DMA physical-address publication or ring completion matching; Existing xHCI CPU pointer; zero is ambiguous in the legacy API; Returns legacy physical address, with zero ambiguity |
| `kernel/src/drivers/usb/xhci/xhci.c:749` / `xhci_recover_command_ring` | `paging_get_physical_address((uint64_t)xhci_driver.cmd_ring_base)` | DMA physical-address publication or ring completion matching; Existing xHCI CPU pointer; zero is ambiguous in the legacy API; Returns legacy physical address, with zero ambiguity |
| `kernel/src/drivers/usb/xhci/xhci.c:755` / `xhci_recover_command_ring` | `paging_get_physical_address((uint64_t)xhci_driver.dcbaa)` | DMA physical-address publication or ring completion matching; Existing xHCI CPU pointer; zero is ambiguous in the legacy API; Returns legacy physical address, with zero ambiguity |
| `kernel/src/drivers/usb/xhci/xhci.c:906` / `xhci_queue_kbd_request_buf` | `paging_get_physical_address((uint64_t)buf)` | DMA physical-address publication or ring completion matching; Existing xHCI CPU pointer; zero is ambiguous in the legacy API; Returns legacy physical address, with zero ambiguity |
| `kernel/src/drivers/usb/xhci/xhci.c:987` / `xhci_set_leds` | `paging_get_physical_address((uint64_t)buf)` | DMA physical-address publication or ring completion matching; Existing xHCI CPU pointer; zero is ambiguous in the legacy API; Returns legacy physical address, with zero ambiguity |
| `kernel/src/drivers/usb/xhci/xhci.c:988` / `xhci_set_leds` | `paging_map((uint64_t)buf, buf_phys, PAGE_PRESENT \| PAGE_RW)` | DMA existing heap mapping reinstalled; kmalloc_aligned result and its existing physical translation; P=1 RW=1 US=0 PCD=0 PWT=0 NX=0; PAT bit=0; PAT slot index=0 |
| `kernel/src/drivers/usb/xhci/xhci.c:1089` / `xhci_poll_keyboard_test` | `paging_get_physical_address((uint64_t)&xhci_driver.event_ring[xhci_driver.event_ring_dequeue_idx])` | DMA physical-address publication or ring completion matching; Existing xHCI CPU pointer; zero is ambiguous in the legacy API; Returns legacy physical address, with zero ambiguity |
| `kernel/src/drivers/usb/xhci/xhci.c:1169` / `xhci_configure_device` | `paging_get_physical_address((uint64_t)dev_ctx)` | DMA physical-address publication or ring completion matching; Existing xHCI CPU pointer; zero is ambiguous in the legacy API; Returns legacy physical address, with zero ambiguity |
| `kernel/src/drivers/usb/xhci/xhci.c:1170` / `xhci_configure_device` | `paging_map((uint64_t)dev_ctx, dev_phys, PAGE_PRESENT \| PAGE_RW)` | DMA existing heap mapping reinstalled; kmalloc_aligned result and its existing physical translation; P=1 RW=1 US=0 PCD=0 PWT=0 NX=0; PAT bit=0; PAT slot index=0 |
| `kernel/src/drivers/usb/xhci/xhci.c:1180` / `xhci_configure_device` | `paging_get_physical_address((uint64_t)input_ctx)` | DMA physical-address publication or ring completion matching; Existing xHCI CPU pointer; zero is ambiguous in the legacy API; Returns legacy physical address, with zero ambiguity |
| `kernel/src/drivers/usb/xhci/xhci.c:1181` / `xhci_configure_device` | `paging_map((uint64_t)input_ctx, inp_phys, PAGE_PRESENT \| PAGE_RW)` | DMA existing heap mapping reinstalled; kmalloc_aligned result and its existing physical translation; P=1 RW=1 US=0 PCD=0 PWT=0 NX=0; PAT bit=0; PAT slot index=0 |
| `kernel/src/drivers/usb/xhci/xhci.c:1194` / `xhci_configure_device` | `paging_get_physical_address((uint64_t)tr_ring)` | DMA physical-address publication or ring completion matching; Existing xHCI CPU pointer; zero is ambiguous in the legacy API; Returns legacy physical address, with zero ambiguity |
| `kernel/src/drivers/usb/xhci/xhci.c:1195` / `xhci_configure_device` | `paging_map((uint64_t)tr_ring, tr_phys, PAGE_PRESENT \| PAGE_RW)` | DMA existing heap mapping reinstalled; kmalloc_aligned result and its existing physical translation; P=1 RW=1 US=0 PCD=0 PWT=0 NX=0; PAT bit=0; PAT slot index=0 |
| `kernel/src/drivers/usb/xhci/xhci.c:1570` / `xhci_alloc_dcbaa` | `paging_get_physical_address((uint64_t)dcbaa_virt)` | DMA physical-address publication or ring completion matching; Existing xHCI CPU pointer; zero is ambiguous in the legacy API; Returns legacy physical address, with zero ambiguity |
| `kernel/src/drivers/usb/xhci/xhci.c:1573` / `xhci_alloc_dcbaa` | `paging_map((uint64_t)dcbaa_virt, dcbaa_phys, dma_flags)` | DMA existing heap mapping reinstalled; kmalloc_aligned result and its existing physical translation; P=1 RW=1 US=0 PCD=0 PWT=0 NX=0; PAT bit=0; PAT slot index=0 |
| `kernel/src/drivers/usb/xhci/xhci.c:1593` / `xhci_alloc_dcbaa` | `paging_get_physical_address((uint64_t)sp_array_virt)` | DMA physical-address publication or ring completion matching; Existing xHCI CPU pointer; zero is ambiguous in the legacy API; Returns legacy physical address, with zero ambiguity |
| `kernel/src/drivers/usb/xhci/xhci.c:1595` / `xhci_alloc_dcbaa` | `paging_map((uint64_t)sp_array_virt, sp_array_phys, dma_flags)` | DMA existing heap mapping reinstalled; kmalloc_aligned result and its existing physical translation; P=1 RW=1 US=0 PCD=0 PWT=0 NX=0; PAT bit=0; PAT slot index=0 |
| `kernel/src/drivers/usb/xhci/xhci.c:1603` / `xhci_alloc_dcbaa` | `paging_get_physical_address((uint64_t)buf_virt)` | DMA physical-address publication or ring completion matching; Existing xHCI CPU pointer; zero is ambiguous in the legacy API; Returns legacy physical address, with zero ambiguity |
| `kernel/src/drivers/usb/xhci/xhci.c:1605` / `xhci_alloc_dcbaa` | `paging_map((uint64_t)buf_virt, buf_phys, dma_flags)` | DMA existing heap mapping reinstalled; kmalloc_aligned result and its existing physical translation; P=1 RW=1 US=0 PCD=0 PWT=0 NX=0; PAT bit=0; PAT slot index=0 |
| `kernel/src/drivers/usb/xhci/xhci.c:1651` / `xhci_init_command_ring` | `paging_get_physical_address((uint64_t)ptr)` | DMA physical-address publication or ring completion matching; Existing xHCI CPU pointer; zero is ambiguous in the legacy API; Returns legacy physical address, with zero ambiguity |
| `kernel/src/drivers/usb/xhci/xhci.c:1653` / `xhci_init_command_ring` | `paging_map((uint64_t)ptr, phys, PAGE_PRESENT \| PAGE_RW)` | DMA existing heap mapping reinstalled; kmalloc_aligned result and its existing physical translation; P=1 RW=1 US=0 PCD=0 PWT=0 NX=0; PAT bit=0; PAT slot index=0 |
| `kernel/src/drivers/usb/xhci/xhci.c:1692` / `xhci_init_event_ring` | `paging_get_physical_address((uint64_t)ring_virt)` | DMA physical-address publication or ring completion matching; Existing xHCI CPU pointer; zero is ambiguous in the legacy API; Returns legacy physical address, with zero ambiguity |
| `kernel/src/drivers/usb/xhci/xhci.c:1693` / `xhci_init_event_ring` | `paging_map((uint64_t)ring_virt, ring_phys, dma_flags)` | DMA existing heap mapping reinstalled; kmalloc_aligned result and its existing physical translation; P=1 RW=1 US=0 PCD=0 PWT=0 NX=0; PAT bit=0; PAT slot index=0 |
| `kernel/src/drivers/usb/xhci/xhci.c:1702` / `xhci_init_event_ring` | `paging_get_physical_address((uint64_t)erst_virt)` | DMA physical-address publication or ring completion matching; Existing xHCI CPU pointer; zero is ambiguous in the legacy API; Returns legacy physical address, with zero ambiguity |
| `kernel/src/drivers/usb/xhci/xhci.c:1703` / `xhci_init_event_ring` | `paging_map((uint64_t)erst_virt, erst_phys, dma_flags)` | DMA existing heap mapping reinstalled; kmalloc_aligned result and its existing physical translation; P=1 RW=1 US=0 PCD=0 PWT=0 NX=0; PAT bit=0; PAT slot index=0 |
| `kernel/src/drivers/usb/xhci/xhci.c:1903` / `xhci_kbd_recover_poll` | `paging_get_physical_address((uint64_t)&ring[0])` | DMA physical-address publication or ring completion matching; Existing xHCI CPU pointer; zero is ambiguous in the legacy API; Returns legacy physical address, with zero ambiguity |
| `kernel/src/drivers/usb/xhci/xhci.c:1977` / `xhci_process_events` | `paging_get_physical_address((uint64_t)xhci_driver.event_ring)` | DMA physical-address publication or ring completion matching; Existing xHCI CPU pointer; zero is ambiguous in the legacy API; Returns legacy physical address, with zero ambiguity |
| `kernel/src/drivers/usb/xhci/xhci.c:2279` / `xhci_map_identity_range` | `paging_map(p, p, PAGE_PRESENT \| PAGE_RW)` | DMA physical-pointer identity remap; DCBAA output-context physical address or device-context page; P=1 RW=1 US=0 PCD=0 PWT=0 NX=0; PAT bit=0; PAT slot index=0 |
| `kernel/src/drivers/usb/xhci/xhci.c:2571` / `xhci_init` | `paging_map(page, page, mmio_flags)` | MMIO; PCI BAR plus register-window offset; P=1 RW=1 US=0 PCD=1 PWT=1 NX=0; PAT bit=0; PAT slot index=3 |
| `kernel/src/drivers/usb/xhci/xhci.c:2717` / `xhci_evaluate_context` | `paging_map(out_phys + off, out_phys + off, PAGE_PRESENT \| PAGE_RW)` | DMA physical-pointer identity remap; DCBAA output-context physical address or device-context page; P=1 RW=1 US=0 PCD=0 PWT=0 NX=0; PAT bit=0; PAT slot index=0 |
| `kernel/src/drivers/usb/xhci/xhci.c:2727` / `xhci_evaluate_context` | `paging_get_physical_address((uint64_t)input_ctx)` | DMA physical-address publication or ring completion matching; Existing xHCI CPU pointer; zero is ambiguous in the legacy API; Returns legacy physical address, with zero ambiguity |
| `kernel/src/drivers/usb/xhci/xhci.c:2728` / `xhci_evaluate_context` | `paging_map((uint64_t)input_ctx, inp_phys, PAGE_PRESENT \| PAGE_RW)` | DMA existing heap mapping reinstalled; kmalloc_aligned result and its existing physical translation; P=1 RW=1 US=0 PCD=0 PWT=0 NX=0; PAT bit=0; PAT slot index=0 |
| `kernel/src/drivers/usb/xhci/xhci.c:2853` / `xhci_send_command_wait` | `paging_get_physical_address((uint64_t)cmd)` | DMA physical-address publication or ring completion matching; Existing xHCI CPU pointer; zero is ambiguous in the legacy API; Returns legacy physical address, with zero ambiguity |
| `kernel/src/drivers/usb/xhci/xhci.c:2935` / `xhci_send_command_wait` | `paging_get_physical_address((uint64_t)xhci_driver.cmd_ring_base)` | DMA physical-address publication or ring completion matching; Existing xHCI CPU pointer; zero is ambiguous in the legacy API; Returns legacy physical address, with zero ambiguity |
| `kernel/src/drivers/usb/xhci/xhci.c:2962` / `xhci_get_descriptor_device` | `paging_get_physical_address((uint64_t)buffer)` | DMA physical-address publication or ring completion matching; Existing xHCI CPU pointer; zero is ambiguous in the legacy API; Returns legacy physical address, with zero ambiguity |
| `kernel/src/drivers/usb/xhci/xhci.c:2963` / `xhci_get_descriptor_device` | `paging_map((uint64_t)buffer, phys, PAGE_PRESENT \| PAGE_RW)` | DMA existing heap mapping reinstalled; kmalloc_aligned result and its existing physical translation; P=1 RW=1 US=0 PCD=0 PWT=0 NX=0; PAT bit=0; PAT slot index=0 |
| `kernel/src/drivers/usb/xhci/xhci.c:2992` / `xhci_get_config_descriptor` | `paging_get_physical_address((uint64_t)buf_head)` | DMA physical-address publication or ring completion matching; Existing xHCI CPU pointer; zero is ambiguous in the legacy API; Returns legacy physical address, with zero ambiguity |
| `kernel/src/drivers/usb/xhci/xhci.c:2993` / `xhci_get_config_descriptor` | `paging_map((uint64_t)buf_head, head_phys, PAGE_PRESENT \| PAGE_RW)` | DMA existing heap mapping reinstalled; kmalloc_aligned result and its existing physical translation; P=1 RW=1 US=0 PCD=0 PWT=0 NX=0; PAT bit=0; PAT slot index=0 |
| `kernel/src/drivers/usb/xhci/xhci.c:3014` / `xhci_get_config_descriptor` | `paging_get_physical_address((uint64_t)buf_full)` | DMA physical-address publication or ring completion matching; Existing xHCI CPU pointer; zero is ambiguous in the legacy API; Returns legacy physical address, with zero ambiguity |
| `kernel/src/drivers/usb/xhci/xhci.c:3018` / `xhci_get_config_descriptor` | `paging_map((uint64_t)buf_full + off, full_phys + off, PAGE_PRESENT \| PAGE_RW)` | DMA existing heap mapping reinstalled; kmalloc_aligned result and its existing physical translation; P=1 RW=1 US=0 PCD=0 PWT=0 NX=0; PAT bit=0; PAT slot index=0 |
| `kernel/src/drivers/usb/xhci/xhci.c:3265` / `xhci_control_transfer` | `paging_get_physical_address((uint64_t)data)` | DMA physical-address publication or ring completion matching; Existing xHCI CPU pointer; zero is ambiguous in the legacy API; Returns legacy physical address, with zero ambiguity |
| `kernel/src/drivers/usb/xhci/xhci.c:3340` / `xhci_configure_device_with_context` | `paging_get_physical_address((uint64_t)dev_ctx)` | DMA physical-address publication or ring completion matching; Existing xHCI CPU pointer; zero is ambiguous in the legacy API; Returns legacy physical address, with zero ambiguity |
| `kernel/src/drivers/usb/xhci/xhci.c:3341` / `xhci_configure_device_with_context` | `paging_map((uint64_t)dev_ctx, dev_phys, PAGE_PRESENT \| PAGE_RW)` | DMA existing heap mapping reinstalled; kmalloc_aligned result and its existing physical translation; P=1 RW=1 US=0 PCD=0 PWT=0 NX=0; PAT bit=0; PAT slot index=0 |
| `kernel/src/drivers/usb/xhci/xhci.c:3351` / `xhci_configure_device_with_context` | `paging_get_physical_address((uint64_t)input_ctx)` | DMA physical-address publication or ring completion matching; Existing xHCI CPU pointer; zero is ambiguous in the legacy API; Returns legacy physical address, with zero ambiguity |
| `kernel/src/drivers/usb/xhci/xhci.c:3352` / `xhci_configure_device_with_context` | `paging_map((uint64_t)input_ctx, inp_phys, PAGE_PRESENT \| PAGE_RW)` | DMA existing heap mapping reinstalled; kmalloc_aligned result and its existing physical translation; P=1 RW=1 US=0 PCD=0 PWT=0 NX=0; PAT bit=0; PAT slot index=0 |
| `kernel/src/drivers/usb/xhci/xhci.c:3381` / `xhci_configure_device_with_context` | `paging_get_physical_address((uint64_t)tr_ring)` | DMA physical-address publication or ring completion matching; Existing xHCI CPU pointer; zero is ambiguous in the legacy API; Returns legacy physical address, with zero ambiguity |
| `kernel/src/drivers/usb/xhci/xhci.c:3382` / `xhci_configure_device_with_context` | `paging_map((uint64_t)tr_ring, tr_phys, PAGE_PRESENT \| PAGE_RW)` | DMA existing heap mapping reinstalled; kmalloc_aligned result and its existing physical translation; P=1 RW=1 US=0 PCD=0 PWT=0 NX=0; PAT bit=0; PAT slot index=0 |
| `kernel/src/drivers/watchdog/acpi_wdat.c:128` / `wdat_ensure_mapped` | `paging_map(page, page, PAGE_PRESENT \| PAGE_RW \| PAGE_PCD)` | Firmware-described MMIO; WDAT Generic Address Structure system-memory address; P=1 RW=1 US=0 PCD=1 PWT=0 NX=0; PAT bit=0; PAT slot index=2 |
| `kernel/src/smp/smp_topology.c:50` / `map_and_convert` | `paging_map(v, p, 0x03)` | CPU structure alias; Heap physical identity pointer, high bitwise-OR alias; GDT/TSS/stacks; P=1 RW=1 US=0 PCD=0 PWT=0 NX=0; PAT bit=0; PAT slot index=0 |
| `kernel/src/timer/hpet.c:171` / `init_hpet` | `paging_map(base_page, base_page, PAGE_PRESENT \| PAGE_RW \| PAGE_PCD \| PAGE_PWT)` | MMIO; HPET ACPI Generic Address Structure; P=1 RW=1 US=0 PCD=1 PWT=1 NX=0; PAT bit=0; PAT slot index=3 |
