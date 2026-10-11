# Local timestamp ordering contrast

The optional `IRQ_LOCAL_TSC_ORDER=1` instrument uses
`LFENCE; RDTSC; LFENCE` and declares `intel-local-tsc-irq-envelope-v1`.
The default remains the original `MFENCE; LFENCE; RDTSC; LFENCE` instrument.
This is a predeclared diagnostic contrast, not a retrospective reclassification
of earlier captures or a passing timing result.

Intel SDM revision 089, volume 2, RDTSC and LFENCE, distinguishes prior local
instruction/load completion from prior-store global visibility. The first
LFENCE orders the counter after prior local instructions and completed loads.
The second orders following instructions after the counter read. The C asm
retains its volatile qualifier and memory clobber. Assembly endpoints use the
same selected sequence. Admission requires TSC, SSE2 and the actual GenuineIntel
vendor on every calibration CPU. Other vendors are unavailable for this variant;
no unproved assumption about AMD LFENCE behavior or control-MSR state is made.
The optional request statistics check that capability before their first read.

The measured quantity is elapsed CPU interrupt unavailability. CLI, POPFQ,
interrupt entry/return, lock acquisition and all nested local instructions keep
their original endpoint positions. That quantity does not require every older
store from unrelated execution to have become globally visible before a
counter read. Required store publication, ICR/MMIO ordering, INVLPG, CR3 reload,
WBINVD, ACK release/acquire and resource-reuse fences remain unchanged. This
variant removes only the extra global store drain in observational clock reads.
It does not assert that MFENCE uniquely caused any recorded excursion.

| Endpoint | Required ordering and retained coverage |
| --- | --- |
| Before software CLI | Compiler barrier; counter before CLI and every following masked instruction. |
| After exact POPFQ restore | Prior local instructions, including the mask-ending state change, complete before the counter. |
| IRQ entry and return hooks | Original register/frame positions, internal spans and retained publisher protocol; no inward movement. |
| Empty interrupt probes | All original four endpoints and diagnostic checkpoints remain; all eligible probes contribute to the CPU maximum. |
| HPET reference points | Ordered counter reads enclose the actual safe MMIO loads; unchanged high-low-high and local CPU/switch validation. |
| Local operation/interior statistics | Exact original call-site operation bracket, explicit clock-boundary cost, same-CPU/epoch/continuity checks. |

Architectural entry and the restore/IRET tail still precede/follow the internal
IRQ stamps. They retain the existing empirically qualified enclosing allowance:
twice each CPU's own eligible pre maximum, with every post probe checked against
that frozen value. The contrast neither removes that check nor claims an
architectural cycle bound for an unstamped instruction. A post excursion still
rejects the complete series. The original 29-cell lifetime proof, including the
allowance saved before a nonreturning publisher, is unchanged.

All time between the endpoints stays in the raw result, including observer
work and host/asynchronous delays. No minimum fence cost is subtracted. The
strict 50,000-ns budget, calibration effort/tolerances, ordinary RAM and host
placement, workload, optional-detail policy and service cutoff stay fixed.
The comparison uses new source/ELF/image/boot identities. A smaller maximum would
be a controlled association with the changed observation primitive, not proof
of every earlier event's cause or a passing whole-window result.

Actual assembly flags/envelope tests and actual calibration capability tests
run under both sequences with normal, UBSan and ASan builds. Raw controls reject
unknown vendor, absent SSE2/TSC, missing CPU capability, old/new primitive
relabeling and a current source selecting a historical parser. Existing injected
mask, wait, IRQ endpoint, publisher, allowance and threshold controls remain.
Emitted C and assembly sequences must match the declared build flag. Unchanged
production units retain their local paired-object checks; a diagnostic build
cannot silently change the production ordering protocol.

References are conceptual and independently implemented: Intel SDM revision 089
(https://cdrdv2-public.intel.com/868137/325462-089-sdm-vol-1-2abcd-3abcd-4.pdf),
and Linux v6.12 `rdtsc_ordered`
(https://raw.githubusercontent.com/torvalds/linux/v6.12/arch/x86/include/asm/msr.h).
Linux's local-counter contract is not adopted as a replacement for HobbyOS's
full enclosing IRQ measurement or its maximum-preservation rules.
