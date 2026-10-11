# Guarded kernel stacks

HobbyOS retains one supervisor address space. Durable execution stacks use an
absent page immediately below their mapped payload. This detects ordinary
downward growth through the stack virtual address; it does not isolate a
malicious Ring-0 component or prevent writes through the ordinary RAM alias.
A stack-pointer jump that skips the entire guard can evade this mechanism.

The stack reservation is `[0xffffe00000000000, 0xffffe00006800000)` (104 MiB). It is
separate from the RAM direct map, the linked image, the device window at
`0xffffd00000000000` (one GiB), and both TLB fixture windows starting at
`0xffffc000001ff000` with a one-GiB stride. There are 1664 slots at a 64-KiB
pitch: 128 reserved for the supported 32 CPUs and 1536 for tasks. CPU slots
cannot be consumed by ordinary tasks. The bound includes 1000 modal workers
that can exit before the existing 3000-ms reaper grace expires, the real
257-task scenario, two stress workers per supported CPU (64), and 32 slots
of system/controller headroom: 1353 slots, rounded up to 1536. This is a
capacity allowance, not a claim that all those workloads run together in
every boot. Backing is allocated on demand. Exhaustion fails allocation;
it never falls back to an unguarded heap stack.

Each slot starts with one absent 4096-byte page. Task and AP boot payloads
remain 16384 bytes; each IST payload remains 32768 bytes. The unused suffix
of a slot also stays absent. Tasks retain the original 512-byte `0xA5` canary
prefix inside their payload, excluded from the execution range. Guard virtual
space is not charged as physical stack RAM. Payloads are supervisor RW, WB,
and NX where supported. NX-absent CPUs still enforce non-present guards and
image write protection, but have no execute-protection claim or fetch probes.

The BSP continues on the same high 16-KiB stack installed by assembly before
its first C frame. Exact linker symbols declare its adjacent guard. The
steady main root omits only that page from the linked image; the physical
direct alias and every other image permission remain intact. The entry root
can map this prospective guard during early boot. Guard enforcement starts
with the independently checked steady root, before AP release. No C call
chain moves to a replacement BSP stack.

Three page-aligned static BSP IST arrays remain valid for early GDT/TSS
initialization. Main-root construction borrows precisely their mutable backing
for guarded views. After CR3 activation and readback, each existing packed
TSS IST field is replaced by a locked 64-bit exchange. Both views are valid
during this handoff, including asynchronous NMI and machine-check delivery.
The fields are not claimed to be naturally aligned. IST1/2/3 retain their
double-fault/NMI/machine-check assignments and 32-KiB capacities.

Every AP boot stack and all three AP ISTs are allocated and validated before
the private bootstrap root snapshots the main root's high entries. Their
tops are virtual addresses. GDT/TSS objects retain their existing ordinary
data backing. Idle task records borrow execution on the persistent BSP/AP
boot stacks; null task stack fields do not imply missing backing or another
owner. The single-page trampoline's early assembler stack is a transient
exception before its switch to the guarded high stack. Its private root is
retired through the existing exact AP departure protocol.

Eight prepared task slots are mapped before AP startup and the bootstrap
invalidation seal. The invalidation worker, DPC, input, shell and reaper can
therefore be created while BSP interrupts are still disabled, without waiting
on the service they are establishing. An unpublished prepared allocation may
return to the reserve without remapping. Runtime creation and retirement use
the ordinary eligible IF-enabled acknowledged service. The 54 structural
stack-region table pages are constructed once and retained; payload reuse
does not repeatedly allocate or reclaim tables visible to live walkers.

A generation-bound handle owns the slot and contiguous PMM backing. Permanent
metadata copies task ID/lifecycle/name or CPU/APIC/IST identity. Static BSP
backing is borrowed and is never PMM-freed. Allocation, binding and runnable
publication remain distinct. Wrong-kind, stale and duplicate releases fail
without touching another owner; generations never wrap into an old identity.

Mapping admission checks physical claims under the backing reservation lock,
inside the paging lock. Reserve/free transitions use that same backing lock;
task identity publication cannot change an extent or release its ownership.
Admission can therefore read those stable extent fields while another CPU
publishes an unrelated task identity. Fault lookup still refuses any unstable
full owner record immediately. Neither lock remains held during a CPU wait,
and scheduler publication does not acquire either memory lock.

The scheduler retains its existing policy, saved context ABI, canary checks,
reap order, cleanup, reference checks and grace period. At its existing
unlocked reaper point, retirement removes this payload's leaves, releases
ordinary locks and pins, and waits for all intended CPUs. Only confirmed
completion permits backing release and VA reuse. A queued result is not
completion. Timeout retains the descriptor, backing and slot in quarantine
and enters the existing panic path. Late notification does not release them.

The complete main-root checker validates each live payload against ownership,
physical backing and effective ancestor permissions, and validates all guard
and unused slot bytes as absent. It also retains exact image alias protection,
no-low state and zero effective RWX on NX-capable CPUs. Owner sequences and
the mapping epoch must remain stable throughout a successful observation.
Unknown slots or mixed observations fail; an address merely falling inside
the reservation is not authority to map it or invert it to physical memory.

Fault lookup makes one bounded atomic consistency attempt in permanently
mapped metadata. It never waits on an interrupted publisher, allocates, walks
suspect tables, or dereferences a freed task. A healthy-stack page fault into
another owner's guard is reported as a guard access, not automatically as
the current task overflowing. The panic owner emits the fixed copied facts
through its lock-independent output path.

A stack-overflow-associated double fault additionally requires a live task
owner, CR2 and saved stack facts near that guard, and a different healthy
local IST1 used by the handler. Double fault is an abort: the raw saved RIP
is retained with `ABORT_UNDEFINED` authority and is not promised to identify
the precise original instruction. Double fault never recovers. A damaged
IST1 or fault while entering its handler can still prevent diagnosis.

SELFTEST adds an exact assembler guard load from a healthy stack, with fixed
site/recovery, CPU/root/generation/owner and non-present read checks. Separate
QEMU experiments exhaust a real scheduled task stack in bounded 64-byte
steps and exercise uncertain retirement. Their nonreturning failures never
count as successful functional boots. These test payloads and recovery state
are absent from production; guard enforcement and diagnosis remain present.

Native regression collectors also inspect the existing bounded SELFTEST service
audit after their workloads finish. A stopped QEMU physical-memory read binds
absolute request generations, CPU identities and completed stack events to the
actual image load ranges and main page-table paths. The 8192-record capacity is
unchanged; overflow or a missing request is a validation failure. Fatal runs
retain an explicitly incomplete service prefix when necessary. Halted fatal
guests remain halted. Reset and shutdown tests capture at the exact terminal
action boundary after the fatal dump, then continue only the requested terminal
action; post-reset firmware state cannot stand in for the previous kernel.
Unfinished requests are never counted as completed work.
These observations introduce no kernel request and occur outside TLB priming,
sampling and the normal paging measurements.

This change adds no generic virtual allocator, runtime image permission API,
user address space, DMA isolation, KASLR, SMEP/SMAP, or CPU-control change.
The separate IRQ-off latency campaign and physical hardware acceptance remain
pending. Current acceptance, transport replay and independent review must be
reported separately from this architectural contract.
