# Low address isolation

The steady kernel root has zero in each of PML4 slots 0 through 255. This
contract starts at commit `8be743834324cceb41025b0fd617e7ba52757448`.
Historical direct-map observations retain their identity-map predicate; the
current `low-address-isolation` selector is explicit in the native gates.

## Root and pointer transition

`entry.S` retains its transient 16-KiB low assembly stack. After the entry CR3
load and before the first C frame, it installs the top of a separate 16-KiB
linked stack. Both the entry image alias and final image map cover its complete
backing. Linker assertions bound that backing to the entry root's image span.
DF, arguments and the SysV entry alignment retain their original meaning. The
unused one-MiB C array is not activated, and no guard is introduced.

The builder omits low subtrees entirely. Its independent preactivation walk
rejects even nonpresent garbage in any low PML4 slot. The high RAM plan, image,
managed window, cache boundaries and permission rules still pass the existing
coverage verifier. Required paths include the whole current stack, GDT, IDT,
TSS and IST storage. Failed unpublished construction rolls back only its own
tables and cannot reach the CR3 activation seam.

Before activation, every actual construction table receives an independently
walked full-page hash. After activation, readback obtains only its checked
direct-map alias and compares it with that saved hash; accessed/dirty bits are
excluded because hardware can set them. It never reads the old low `table.cpu`
pointer. Root/window owners and PMM bitmap owners are rebased before publishing
the coverage-backed allocation boundary. The identity domain then retires once.

| Owner | Before final CR3 | After final CR3 | Proof |
|---|---|---|---|
| BSP C stack | High linked 16-KiB stack | Same high stack | Linker, emitted bridge, full-range verifier, actual RSP records |
| Construction tables | Reachable bootstrap CPU access plus physical identity | Verified high aliases; old borrows replaced | Independent table walk and saved page hashes |
| Active root and owned-window tables | Unpublished construction ownership | Physical root plus high CPU pointers | Root readback and current owner snapshots |
| Bitmap and reservation buffers | Bootstrap RAM pointers | Both backing pointers and `Bitmap.buffer` rebased | Preserved PMM transition and full-span integrity tests |
| BootInfo/font/logo/map | Image-owned copies and checked preactivation borrows | Image copies, direct RAM payloads, managed framebuffer/firmware | Existing owner transitions and physical readback |
| BSP GDT/IDT/TSS/IST | High image storage installed before paging | Same mapped storage | Whole required ranges and actual descriptor readback |
| AP stack/GDT/TSS/IST | Allocated after direct-map activation | Same high per-CPU storage | Pre-switch range checks and local departure receipts |
| Heap and DMA CPU views | Created after RAM activation | Direct RAM pointers | Existing heap/DMA fixtures; bus addresses stay physical |
| Firmware and device views | Typed owned views | Typed owned views | Discovery, WB/RO or UC/WC alias and lifecycle predicates |
| Temporary AP tables | Four private low physical pages, high CPU access | No live pointers after retirement | Prelaunch bytes, cleared registry/owners and actual PMM bits |

`paging_owned_withdraw_identity` treats strictly verified low absence as a
no-op observation. A malformed walk or any forbidden root entry is an error;
acquisition never recreates a low alias. Existing invalidation/cache request
ordering remains in place, so terminal accounting still derives from actual
published operations. Physical frame zero can remain a valid RAM frame through
a high alias. CPU address zero is absent.

## AP bootstrap ownership

PMM preserves its entire first-MiB reservation. A memory-owned claim selects a
page backed by an actual conventional-RAM descriptor, excludes live boot
objects and adopts the already-reserved page without changing allocation bits
or the free count. Exact repeated ownership is idempotent; a different owner,
generation or extent is rejected. There is no unconditional `0x10000` fallback
and no second call that marks the frame used.

The AP root owns four private table pages below four GiB. Its private lower
subtree maps exactly the claimed trampoline page, containing the unchanged
emitted code, parameters, GDT and bounded temporary stack. Its high entries
share the verified main root's tables. The independent bootstrap verifier checks
each private entry and shared high link before publication. Partial allocation
failure frees only unpublished private pages, never shared or active backing.

At high C entry an AP resolves its architectural ID, installs its own high
GDT/TSS and the high IDT, and verifies its complete virtual stack and exception
storage while the temporary root is still valid. Only then does it load the
physical main CR3, read it back and confirm local CPU policy. A generation-bound
departure includes the actual previous/current roots, RSP, GDTR, IDTR, CS, TR,
TSS and ISTs. Local policy readiness records the actual new root; ONLINE follows
publication of the generation-bound departure.

Retirement requires every member of the immutable intended AP set and the BSP
to be on the main root. `PGE=PCIDE=0` makes each actual CR3 switch invalidate the
old nonglobal translations and paging-structure caches. No delay substitutes
for that switch. Sealing prevents start/retry admission, and the trampoline's
root parameter is zeroed so its existing guard rejects later reuse. Observer
registrations and dereferenceable owners are removed before the four private
pages return to PMM exactly once. Missing or uncertain departure prevents
retirement. The trampoline and static entry tables remain explicitly reserved;
shared high tables are not in the private free set. SMP1 allocates none of this
AP-only storage.

The architectural basis is the pinned Intel SDM Volume 3A, 253668-090US,
section 5.10.4.1, under the preserved control-register assumptions. This is a
bounded bootstrap lifetime proof, not a general CPU-hotplug or reclamation API.

## Fault behavior and evidence

The installed vector-14 handler retains the original exact unmap matcher and
#GP/MSR recovery. Production diagnosis passes a short classification to the
existing panic owner: NULL, another canonical low address (with a possible
physical-pointer explanation), or ordinary page fault. It does not allocate,
walk suspect tables or print through ordinary locks before panic ownership.

SELFTEST owns one exact assembler NULL read and recovery site. Admission and
matching bind generation, actual CPU, RIP, CR2 zero, ring-zero CS and error zero
(a nonpresent supervisor read). Wrong/inactive/protection/write/user/instruction
faults remain fatal. The local pin surrounds only arming, the deliberate read
and disarming. It spans no remote wait. A separate unarmed assembler read is
used only in isolated destructive runs; neither command is in production.

The canonical gate is `scripts/test-low-half-unmapped.sh`. Its complete
predicate includes 39 distinct functional boots, current production/reference
roles, the original 17 native families, both destructive NULL profiles and
independent physical snapshots before AP launch and immediately after private
retirement. A late snapshot of freed table storage is never called a prelaunch
capture. Functional TLB sampling contains no debugger stop. The final audit
covers both mapping lifetimes and all admitted maintenance.

Automated results are recorded under the frozen source/candidate/harness
identity. They do not grant independent review, human visual acceptance or
hardware acceptance. Historical timing uses its original host-reception scope
and uncertainty; PAT-selected WC is not a computed effective framebuffer type.
The next image-permission and guard work remains outside this delivery.
