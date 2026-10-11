# Memory hardening maintenance closure

## Maintainer decision

On 2026-10-10, William explicitly accepted closure of this development scope,
knowing that the measured latency limitation and some planned tests remain.
He subsequently authorized the production-command separation, documentation
and repository cleanup. This supersedes the previous requirement to keep
iterating until every interval is below 50,000 ns. It does not change a raw
result, turn FAIL into PASS, or certify unexecuted tests.

The old timing acceptance contract is retained as a historical measurement
contract. Its strict comparison, observer qualification and failed captures
remain reproducible. Future latency work is listed in the README To-Do.
There is no general hard real-time guarantee and no automatic certification
of every physical platform or later roadmap item.

## Retained physical evidence

The owner-reported physical configuration is an Intel Core i9-12900K, ASUS
PRIME-A Z690, 16 GiB DDR5-5200 and GeForce RTX 4070. The completed diagnostic
identified 24 online logical CPUs and build `07435e30e44a0a5e`.

- Diagnostic candidate: `4a0dc6ba055a01513753435b8dd14d4575b1f68aa2d6f8073f046b61f4059aa7`.
- Kernel SHA-256: `695493cebc731ab6ba7b124fc750b778daa58fc2fff87e3250da46d0414b9724`.
- Original serial log: `hobbyos-reference-20261010-140733.log`, 40,033,003 bytes.
- Log SHA-256: `5d03809821ebbc43d0a596d5a3a31fd30ac098282c842ec6b5f49ba54135f381`.

The source-bound analysis reconstructed 222,516 CRC-protected records with
no CRC mismatch, all 264 CPU/path cells, 48 retired stress workers and
49,152 operations in each of the heap and PMM workloads. The TLB fixture
completed 1,542 requests, 37,008 ACKs and 720,720 actual loads. Its sealed
proof exported 80,731 records and 11,091,400 bytes with no declared loss.
These results establish the reported functional work for this diagnostic;
they do not replace the entire regression campaign.

All 24 frequency qualifications passed. Observer postchecks rejected CPUs
1, 4 and 15. There were 247 cell maxima at or above the former 50-us reference;
17 were numerically below it and had qualified CPU observers. Missing cells
were not replaced by zeros. The original schema-4 command remains FAIL.

The largest enclosing estimate was 4,472,752 ns (4.472752 ms), CPU5, shared
by the shootdown/cache-writeback labels of one event. The same request 3712
has a validated continuous-IF=0 WBINVD bracket proving at least 4,202,184 ns.
All 24 targets acknowledged that request after their required operation;
its 7,765,000-ns request elapsed time was below the unchanged one-second
deadline. The bracket does not isolate instruction cost from all firmware
or other interference. No host or observer time was subtracted.

The largest heap-labelled enclosing estimate was 912,162 ns on CPU0. A
separate CPU9 metadata bracket established a 57,102-ns minimum. These are
different events; they cannot be added or substituted for one another.

Linux timerlat and virtual irqsoff references used different quantities and
workloads. They do not establish a matching HobbyOS performance bound.

## Production and development roles

`make production-image` builds the normal kernel without the development
command objects, selftest registry, IRQ-latency recorder or TLB proof spool.
Its commands are `help`, `clear`, `version`, `mem`, `cpu`, `pci`, `acpi`,
`echo`, `shutdown`, `restart`, `ps`, `kill` and `taskman`, with their usual
aliases. Deliberate panic injection is also development-only; actual panic
handling and serial boot/error diagnostics remain.

For development, build with `make kernel-check SELFTEST=1` followed by
`make image SELFTEST=1`. Add `IRQ_LATENCY=1` to both commands only when
using the latency suite. Ordinary `DEBUG_ASSERT=1` alone does not expose
laboratory commands. Historical commands and validators remain in source
so a future investigator can rebuild an explicit diagnostic image.

This maintenance change does not change allocators, clock providers,
invalidation, cache maintenance, request deadlines, observer algorithms,
USB ownership, scheduler policy, guards, NX/W^X or failure quarantine.
The production image built after this change is a new candidate; an older
physical boot is not represented as a test of these new bytes.

## Outstanding validation

The complete former timing campaign was not achieved: three consecutive
qualified Q35/KVM/SMP8 boots with all full windows below 50 us are absent.
It is deferred research under the maintainer's revised scope, not a hidden
PASS. Also outstanding as a complete current-source campaign are:

- The 39 functional boots across the thirteen declared conditions, including
  SMP24, NX-disabled, forced xAPIC, sole-hub input and high-RAM cases.
- All 17 native families together with nine inherited layout roles and the
  framebuffer reference.
- The complete current production/WB-reference/readback and destructive
  NULL/overflow/timeout/invalid-context roles.
- Physical-platform coverage beyond the retained diagnostic and the cleaned
  production smoke described below, including the deferred terminal and
  fault scenarios on applicable platforms.

Focused maintenance tests, hosted regression and QEMU smokes must be reported
with their actual scope; none of them alone completes the list above.

## Maintenance verification

The command policy was exercised against the actual C registry in production,
DEBUG, SELFTEST and latency-enabled SELFTEST builds, each in normal, UBSan
and ASan modes: 12 hosted executions passed. Canonical dependency, kernel,
image, production-policy and production/SELFTEST stack checks passed.

A Q35/KVM/SMP4 development smoke executed `irq check` and all 151 bounded
`tasktest` cases, with zero failures or skips. A separate production smoke
checked the reduced help list, memory/CPU summaries and TASKMAN open/ESC/
retirement/return-to-shell. An initial host-controller attempt sent input
during modal handoff and timed out; its log and screenshot are retained.
The corrected controller waits for modal close and exact task retirement.
The same production bytes then completed the smoke. This is transport
verification, not a new timing success or an erased failure.

Production kernel SHA-256:
`4ed7d58523c1354caad7772c4790ac240c3b6503e4218d32d41feef1b0b07ea7`.
The recovery receipt binds the concrete FAT image separately, since filesystem
metadata can change its container hash without changing the kernel payload.

## Production hardware smoke

On 2026-10-11, the maintainer ran the cleaned production image on the same
i9-12900K platform. The captured log is
`hobbyos-production-20261011-141054.log`, 122,733 bytes, SHA-256
`d7f6956c9467b86e109b0ca01600b853279ba20db02b65b9022eb18288dd206a`.
Its two boot sequences both reached `RUNTIME_READY` with 24/24 CPUs and
`TEST_READY` with autorun and selftests disabled. All 48 per-CPU control
records retain CR0 `0x80010033`, CR4 `0x20`, EFER `0xd00` and PAT
`0x0007010600070406`.

Both help listings contain exactly the 13 production commands. Three memory
summaries passed their PMM/heap integrity and arithmetic checks. Two TASKMAN
sessions ended normally, and their exact UI tasks were reaped. Heap usage
returned to its initial reported 217,936 bytes after those sessions. Both
boots retained the empty lower PML4 half and successful image-permission
scans with no writable/executable bytes; the direct map used 14 one-GiB
leaves. No `FAIL`, `PANIC` or `ERROR` record was found in this capture.

The second boot corroborates the reported restart. Display, keyboard and
final physical power-off behavior also rely on the maintainer's observation.
Four NUL bytes at the capture boundaries are retained; unstructured boot
messages can interleave. This is a functional serial smoke, not a claim of
CRC-protected complete timing transport or completion of the deferred matrix.

The deployed kernel has the production SHA-256 recorded above. The image
SHA-256 is
`187c900df6f96897c9f63babcbca4f26e57638fdd3b34438c194d9eef7f25f70`.
All five media payloads were read back and verified before safe ejection.
The serial stream does not print a full kernel hash: identity is supported
by that deployment receipt, the production command set and the linked image
layout, whose 16,605,184 mapped bytes match the current ELF after excluding
the BSP guard. The earlier same-day smoke used the previous diagnostic
kernel and is not credited as production validation.

No kernel source or compiled payload changed during publication preparation.
`make deps-check` and `make production-test-policy` passed again on the
retained production candidate. The maintainer accepted publication with the
documented limitations and requested one branch-specific commit on top of
the unchanged `Kernel-Version` base.

## Retention and future use

Current source, Git history, permanent regression scripts and build/QEMU
helpers stay in the repository. A verified recovery archive outside the
repository retains the pre-edit source, final source, selected physical and
virtual evidence, diagnostic rollback payloads, maintenance results and
retention indexes. Historical source snapshots and session tools are retained
in content-addressed form before their expanded laboratory directories are
retired. Original user serial logs outside the repository are untouched.

Generated laboratory directories, duplicate images, caches and old transfer
bundles are disposable after that verification. Historical gates which name
retired capture directories require restoration of the corresponding evidence;
their existence is not a claim that a fresh full campaign passed. Python is
still required by the canonical build, QEMU control and permanent tests.

Before the requested branch squash, the 16 intermediate commits and their
`Kernel-Version` ancestry were saved in a verified Git bundle outside the
repository. The existing verified recovery archive contains the dirty source
checkpoint and historical proof. Historical commit identifiers in old source
contracts remain provenance; reconstructing those historical roles after a
fresh clone requires that bundle or the exact retained source exports.
