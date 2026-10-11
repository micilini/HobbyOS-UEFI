# Control-state candidate measurements

The current SELFTEST candidate is
`413053169c0efc3b8a05d403e8963eb733be5fd8b3f49d6fa27d382a61813678`, with
build-input digest
`1066c7d29b8e67126326fbb29049f3515d38d558f0ef823b9025ffa6b8d64277`, ELF
`adbc3ff3681493f8c9e21ab018ae6b21525a4cf195865c971cb7624cbf5a107b` and FAT image
`8539bcb25d8db6d6c9ca3c2ce729985b81babc536fdf0e93ade814865a3af4c1`.
Options are `SELFTEST=1 DEBUG_ASSERT=1 SELFTEST_AUTORUN=0`; launch memory is
2 GiB. QEMU 8.2.2 uses `max` for TCG and `host` for KVM. Exact firmware hashes,
launch arguments, CPU features and distinct VM/boot identities are retained in
each raw launch receipt.

## Paging-marker host-reception comparison

The approved observation baseline remains candidate
`ddaa276bbdf7a8cfc19148091306b684dec4fa854c505fb0bfe7cf0cf5ebfe4c` with
build-input digest
`2b0678c9e7e606deb22c23c6a2385b02874d190f9f6fce85f90edca083f87044`.

| Candidate and profile | Three original receipt intervals, ns | Median, ns |
| --- | --- | ---: |
| Approved observation, Q35/TCG/SMP4 | 1411852460, 1415497788, 1446679785 | 1415497788 |
| Current controls, Q35/TCG/SMP4 | 1387257401, 1386365974, 1391752690 | 1387257401 |
| Approved observation, Q35/KVM/SMP4 | 157258679, 155190823, 156314361 | 156314361 |
| Current controls, Q35/KVM/SMP4 | 157212027, 153721843, 157642242 | 157212027 |

The current samples are the six clean normal SMP4 boots in `series.json`.
Neither the stopped debugger campaign nor the NX-disabled series contributes.
These are host socket-reception intervals, subject to buffering and host
scheduling uncertainty; no precise uncertainty bound is assigned. The markers
are unchanged, but BSP PAT initialization now precedes them. The comparison
therefore measures a different initialization scope and does not establish a
page-table construction optimization. Control initialization was not separately
timed.

## Functional and cache observations

| Normal profile | Distinct successful boots |
| --- | ---: |
| Q35/TCG/SMP1 | 3 consecutive |
| Q35/TCG/SMP2 | 1 |
| Q35/TCG/SMP4 | 3 consecutive |
| Q35/KVM/SMP4 | 3 consecutive |
| Q35/KVM/SMP8 | 1 |
| PC/TCG/SMP4 | 1 |
| Q35/KVM/SMP24 | 3 consecutive |

Every normal boot reports CR0 `0x80010033`, CR4 `0x20`, EFER `0xd00` and PAT
`0x0007010600070406` on every discovered online CPU, before ONLINE publication
and in three increasing independent observer generations. Three separate
Q35/TCG/SMP4 `max,-nx` boots report NX absent and EFER `0x500`, with the same
CR0/CR4/PAT policy. Production smokes on Q35/TCG/SMP1 and Q35/KVM/SMP4 retain
the initializer and actual local readiness records while rejecting the three
observation commands.

The three selected TCG/SMP1 `mem` Enter-release intervals are `0.112279636`,
`0.113408146`, and `0.118604355` seconds, from Enter-up submission to the final
physical release observation. Both integrity checks and the unchanged
five-second input contract pass with the complete 268435456-frame address span.
The accepted PMM implementation is unchanged; these intervals establish no
microsecond IRQ-off budget.

The separate successful stopped KVM/SMP4 capture reconstructs 2059 active
physical table frames, one bootstrap-only allocation, and seven static bootstrap
frames. The initial allocation counter is 2056; total successful table
allocations are 2060. These quantities have distinct meanings. The active tree
contains 1036221 leaves selecting slot 0, 258 selecting slot 3, 8188 selecting
slot 2 and 4096 selecting slot 4. Its 2058 non-leaf links select slot 0. No
active, bootstrap or static mapping selects slot 5. The approved active tree
had one fewer slot-0 leaf; current image growth changes its mapped image extent
without changing any mapping producer or permission policy.

Kernel/RAM representatives still select slot 0, MMIO slot 3 and framebuffer slot
4. Their selected PAT bytes and permissions match the independently replayed
approved representatives. All eight retained variable MTRR pairs and control
fields match on the comparable KVM profile. Fixed ranges remain
`NOT_COLLECTED`; effective types remain `NOT_COMPUTED`.

QMP APIC identities bind the four debugger vCPU contexts to local CPU records.
GDB independently reads CR0/CR3/CR4/EFER. This QEMU/GDB interface does not expose
PAT as a register; PAT proof uses genuine local readbacks with that limitation
explicit. Two debugger collection failures on thread-description parsing belong
to the preserved superseded development campaign, not the current series.

The current campaign retains an earlier TCG/SMP1 boot whose `memmap` extension
record lost two characters and failed strict replay. That failure reset the
profile's streak; the selected three successes are fresh subsequent boots.
Raw serial-receipt bytes confirm truncation before semantic parsing. Concurrent
archive compression was a plausible host-load contributor, not a demonstrated
guest diagnosis. The controlled repeat ran after compression completed, without
changing source, launch options, deadlines or parsers.

A previous control campaign was superseded after the shared assembly-header
dependency was added to the Makefile. Its binaries and indexed evidence remain
local. Fresh clean builds produced identical ELF bytes, but the changed build
input identity required a new complete acceptance campaign. No superseded boot
counts toward the current series or timing comparison.

Current SELFTEST section sizes are `.text=600886`, `.trampoline=796`,
`.rodata=100219`, `.data=972`, `.bss=15419096` bytes. The bootstrap text is 316
bytes, bootstrap data 45056 bytes and the MSR-fixup table 32 bytes. Raw `size`,
`readelf`, `nm`, disassembly and same-object layout proofs accompany the candidate.
SMP24 is functional coverage on this 12-schedulable-CPU host; authoritative
cadence controls retain the existing nonoversubscribed contract.

Historical demonstrations remain unavailable under their original identities.
Human TASKMAN visual review is pending. Hardware is `DEFERRED_TO_FINAL_CLOSURE`.
