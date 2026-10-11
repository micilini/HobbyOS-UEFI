# Explicit local CPU control state

The control policy is rooted at commit
`4dcbcd2a634c9b4ca864277af613434e7f9d7075`. The independent observer remains
read-only and accepts complete nonuniform historical observations. Policy
acceptance is an additional predicate. Production initializes controls too.

## Register contract

| Register bits | Policy | Consumer and transition prerequisite |
| --- | --- | --- |
| CR0.PE, PG | Set in the final state | Four-level long mode; the BSP never clears either bit. The AP enables PG only after PAE, LME and CR3. |
| CR0.WP, NE | Set | Supervisor write protection and architectural numeric exception reporting. |
| CR0.MP, ET | Set | Preserve the BSP arithmetic policy; ET is architecturally fixed on the supported processors. |
| CR0.EM, TS, AM, NW, CD | Clear finally | No lazy FPU or alignment-check policy. CD is set and NW cleared only during the bounded cache transition. |
| CR4.PAE | Set | Required for the existing four-level page tables; CPUID.1:EDX.PAE is required. Never cleared during active long mode. |
| CR4.DE, MCE | Clear | No debug-register consumer or machine-check initialization/recovery contract exists. Firmware enablement is not ownership. |
| CR4.OSFXSR, OSXMMEXCPT, OSXSAVE | Clear | Kernel compilation uses `-mgeneral-regs-only`; no SIMD/FPU save/restore or task context consumer exists. |
| CR4.PGE | Clear | No global-page policy. An inherited set bit is cleared to invalidate global translations before the PAT change. |
| CR4.PCIDE, LA57 | Clear; reject incoming set bits | No PCID or five-level paging transition is supported. Do not reinterpret an incoming CR3. |
| CR4.SMEP, SMAP, UMIP and remaining unconsumed bits | Clear | No consumer is authorized. Unexpected incoming facilities outside the explicitly removable set fail before writes. |
| EFER.LME | Set | Required long-mode enable; never cleared while PG is set. |
| EFER.LMA | Readback only | Expected after long-mode entry; never included in the write value. |
| EFER.NXE | Set exactly when CPUID advertises NX | NX absence boots with NXE clear. No current page entry gains NX. An AP missing a published requirement fails startup. |
| EFER.SCE and remaining writable facilities | Clear | No SYSCALL or other consumer. Unsupported incoming state is rejected before transition. |

Final CR0 is `0x80010033`; the protected-mode value is `0x00010033`.
Final CR4 is `0x20`. EFER writes are `0x900` with NX or `0x100` without NX;
final readbacks include LMA and are respectively `0xd00` or `0x500`.
Required local prerequisites are CPUID MSR, PAE, PAT and long mode. A missing
required prerequisite fails before its dependent instruction. NX is optional.

## Cache policy and local transition

The declared PAT bytes, from slot 0 to slot 7, are
`WB, WT, UC-, UC, WB, WC, UC-, UC`: `0x0007010600070406`.
Only previously unused slot 5 changes. The framebuffer retains slot 4.
The encoder produces selection bits only, with PAT at bit 7 for 4 KiB leaves
and bit 12 for 2 MiB/1 GiB leaves. It never supplies PS or permissions.

Before changing the BSP state, validate local features, policy, incoming
long-mode state, PAT encodings and the seven known static bootstrap frames.
The protected `core/entry.S` constructs 2 MiB leaves with slot 0; its CR3 and
non-leaf selectors are zero. No arbitrary physical frame is dereferenced.
Current mapping producers and the retained full table snapshot independently
establish that slot 5 is unused. Current mapping construction stays unchanged.

With interrupts saved and disabled, establish CD=1/NW=0, execute WBINVD,
remove inherited PGE through the declared CR4 write, and reload the unchanged
CR3. Program declared EFER and PAT, execute WBINVD and reload CR3 again, then
write final cache-enabled CR0. No logging, MMIO, allocation or waiting occurs
inside this sequence. Read back all controls and restore the entry IF state.
A failure cannot publish readiness. The BSP enters this wrapper with IF=0.

The AP enters protected mode with explicit cache-disabled CR0, validates its
own required CPUID leaves and bits, programs PAT while paging and caching are
disabled, and establishes declared PAE/LME/CR3 before enabling PG. A CR3 reload
with PGE/PCIDE clear supplies local translation invalidation. Final CR0 enables
caching only after both WBINVD operations. Parameters are symbol-addressed
inside the copied blob; code/data and bounded call/push storage must fit below
the temporary stack at the end of its reserved 4096-byte page. The existing
DF and SysV C-entry bridge remains unchanged. After the final kernel-root
switch, local readback must match before LAPIC MMIO or ONLINE publication.

The linked bootstrap blob is 796 bytes. The reserved temporary stack is 256
bytes at the top of the 4096-byte page; the largest explicit push/call depth
is eight bytes. Its relocation object contains no relocation records. C derives
each patch location from the exported symbol rather than repeating an offset.
The current layout verifier checks the following symbol-derived ABI:

| Parameter | Offset | Width |
| --- | ---: | ---: |
| CR3 | 728 | 8 |
| CR4 | 736 | 8 |
| EFER writable policy | 744 | 8 |
| Final CR0 | 752 | 8 |
| PAT | 760 | 8 |
| AP stack | 768 | 8 |
| Entry bridge | 776 | 8 |
| x2APIC flag | 784 | 4 |
| Relocation base | 788 | 4 |
| Bootstrap status | 792 | 4 |

The four named CPUID instruction offsets are 130, 148, 198 and 218; the PAT
and EFER WRMSR offsets are 420 and 459. The exact instruction-site inventory
permits these bootstrap exceptions and the seven local C register/cache
primitives. Other C consumers retain the checked CPUID and safe MSR APIs.
The copied blob hash and same-object relinks are checked separately from
historical cross-version bytes. The unchanged entry bridge supplies DF=0 and
`RSP % 16 == 8`; actual architecture runtime records check the C handoff.
The Makefile explicitly makes the trampoline object depend on the shared
policy header. A dry-run test of the actual Makefile requires recompilation
when that header changes; a negative fixture removes the prerequisite and
must fail. C dependency generation alone does not cover assembly inputs.

Failure records carry the local APIC identity. `[CPU_STATE][FAIL]` uses the C
status codes below; `[CPU_STATE][AP_FAIL]` additionally carries the discovered
slot and uses the bootstrap codes. A failed CPU cannot publish ONLINE or
satisfy the existing release barrier.

| Record | Reason code and meaning |
| --- | --- |
| `FAIL` | 1: required feature unavailable; 2: invalid policy; 3: incompatible incoming paging/control state; 4: mapping/PAT preservation precondition; 5: register read failed; 6: register write failed; 7: readback mismatch; 8: identity mismatch; 9: duplicate publication. |
| `AP_FAIL` | 2: CPUID unavailable; 3: basic feature prerequisite missing; 4: extended leaf/long mode unavailable; 5: local NX policy mismatch; 6: required x2APIC unavailable; 7: invalid bootstrap parameters; 8: local C validation/readback failed. |

Architectural reference: Intel SDM Volume 3A, revision 253668-090US,
February 2026, sections 2.5, 5.10.4 and 14.12.1–14.12.5.
[Official manual](https://cdrdv2-public.intel.com/874249/253668-090-sdm-vol-3a.pdf).
The conservative boot sequence includes both cache and translation-cache
invalidation. It is not a runtime multiprocessor policy-update service.

## Measurement boundaries

The accepted observation baseline remains immutable. PAT-selected types are
not effective MTRR/PAT types: `effective_type=NOT_COMPUTED` remains accurate.
Firmware owns MTRRs. This change proves preservation of mapping selectors and
their selected PAT bytes, not framebuffer WC or complete effective types.

Moving BSP PAT initialization before the unchanged paging markers removes
that operation from their host-reception interval. A new interval must not be
presented as an optimization of page-table construction. Host buffering and
scheduling uncertainty remains unbounded by these samples. Stopped/debugger
runs are excluded from timing samples. Hardware and human visual acceptance
remain separate.
