# Invalidation candidate measurements

The ordinary SELFTEST candidate is
`747c204de63f5ea4e5fffd11307806a3b38fc474a4cc4cdea806e8a7cc437732`, with
build-input digest
`fe7c7946f230570a3e95731798c5be90bacf10d80edf13209898f49dd2e69588`, ELF
`bd8e4d531ec70ebb9fad647ca26799736dd42d2189e556eb7e234901299ca25d` and FAT image
`35ed458849d2e7719fa2f980d0ef5e299dd4467bf2d0ab974ca1c9d79c96f992`.
The build uses `SELFTEST=1 DEBUG_ASSERT=1 SELFTEST_AUTORUN=0`. Normal launches
retain QEMU 8.2.2, 2 GiB, `max` for TCG and `host` for KVM. Raw launch receipts
retain firmware hashes, exact arguments and independent VM/boot identities.
Measurements below describe completed runs; complete development acceptance
requires the separate aggregate receipt and all inherited contracts.

## Paging-marker observations

The six current intervals come from the three clean normal boots in each SMP4
profile, before any scratch mapping workload. The current collector observes
the existing file-backed serial output using incremental 64-byte reads. The
original and accepted control campaigns used socket reception. File visibility,
buffering and host scheduling introduce unquantified uncertainty; these are
host observations, not exact guest execution times. A 0.5-ms EOF polling interval
is not an uncertainty bound. Debugger/fault-traced runs are excluded.

| Candidate and profile | Host-observed intervals, ns | Median, ns |
| --- | --- | ---: |
| Original observation, Q35/TCG/SMP4 | 1411852460, 1415497788, 1446679785 | 1415497788 |
| Accepted controls, Q35/TCG/SMP4 | 1387257401, 1386365974, 1391752690 | 1387257401 |
| Current invalidation, Q35/TCG/SMP4 | 1330591597, 1400517537, 1317141737 | 1330591597 |
| Original observation, Q35/KVM/SMP4 | 157258679, 155190823, 156314361 | 156314361 |
| Accepted controls, Q35/KVM/SMP4 | 157212027, 153721843, 157642242 | 157212027 |
| Current invalidation, Q35/KVM/SMP4 | 156774130, 153319291, 156445620 | 156445620 |

Original observation candidate:
`ddaa276bbdf7a8cfc19148091306b684dec4fa854c505fb0bfe7cf0cf5ebfe4c`.
Accepted control candidate:
`413053169c0efc3b8a05d403e8963eb733be5fd8b3f49d6fa27d382a61813678`.
Their data remains unchanged. BSP PAT initialization moved outside the unchanged
markers in the accepted control increment. This delivery preserves that scope
and introduces no page-table construction optimization. Neither the scope change
nor the serial-backend change supports a precise performance attribution.

## Compiled size and table ownership

| Profile | Text bytes | Read-only data bytes | Data bytes | BSS bytes |
| --- | ---: | ---: | ---: | ---: |
| SELFTEST | 617926 | 102619 | 972 | 15449560 |
| DEBUG without SELFTEST | 557366 | 80315 | 676 | 15382232 |
| Production | 543254 | 78683 | 676 | 15240536 |

Every profile retains 316 bootstrap text bytes, 45056 bootstrap data bytes,
796 trampoline bytes and 32 MSR-fixup bytes. Relative to the accepted control
SELFTEST candidate, text grows by 17040 bytes, read-only data by 2400 and BSS by
30464. Raw `size`, `readelf`, `nm`, disassembly and exact ELF identities accompany
these figures. Image layout changes may move symbols and data frames.

The current SMP4 post-workload observation reports 2066 reachable active table
frames, one extra bootstrap root and 2067 successful table allocations. The
initial PMM allocation counter remains 2056; it is not an early coherent walk.
Seven newly owned scratch table frames persist on SMP, four on UP, with no growth
on the second invocation. Owned A/B data frames and successful deferred handles
are released only after final invalidation and reader retirement. Retained
scratch tables are not described as freed. Independent stopped traversal is a
separate required proof, recorded by the campaign rather than inferred from
these counters.

The accepted CR0/CR4/EFER/PAT policy, PMM address span and mapping/cache selectors
remain unchanged. The test window uses WB slot 0; framebuffer remains slot 4,
and WC slot 5 remains unused. Effective MTRR/PAT memory type is not computed.
The five-second Enter-release gate and the later system-wide IRQ-off budget
remain distinct contracts. Request and handler-body observations are reported
with their measured boundaries in the final campaign report.

The three selected Q35/TCG/SMP1 boots retain the 268435456-frame PMM span.
Their `mem` Enter-release observations are 0.111481287, 0.115368089 and
0.113590958 seconds, measured from the host's key-up submission to its final
confirmed guest-release snapshot. Both integrity checks return successfully,
and later independently framed CPU/IRQ commands complete. These observations
preserve the existing five-second contract without measuring a microsecond
IRQ-off bound.

The separate Q35/KVM/SMP4 stopped capture confirms CR3 `0x100000` and the
unchanged CR0 `0x80010033`, CR4 `0x20` and EFER `0xd00` on all four vCPUs.
It retains the actual vector-225 IDT gate, stub bytes and two cleared fixture
leaf entries after cleanup. The complete raw table dump supports independent
topology reconstruction: 2066 active frames, one bootstrap-only frame and seven
separately enumerated static bootstrap frames, with no orphan PMM table
allocations. PAT is unavailable through this debugger interface;
its proof remains the actual per-CPU local readback. This stopped capture
corroborates entry/root identity and is not an observation of cached TLB entries.

Historical assertion/statistics/layout demonstrations remain unavailable with
their current substitutes. The old modal classification limitation is separate.
Human TASKMAN visual review remains pending; hardware is
`DEFERRED_TO_FINAL_CLOSURE`.
