# Bounded validation artifacts

Artifact retirement is independent of acceptance. The current source checkpoint,
approved source/scope inputs, named candidates, reusable raw proofs, causal
controls and selected comparison samples remain explicit kept dependencies.
Superseded native output and unselected development captures may be retired;
their identity and attempt receipts do not claim that deleted bytes remain
available. A necessary compacted input must reconstruct exactly before its
expanded representation can be retired.

`validation_retention.py` operates on explicit regular-file lists under generated
artifact roots. It checks hashes, modes, inode/device identity, hardlinks,
no-follow parent traversal, retained references and real open-file users. A
maintenance lock excludes new owned QEMU launches. Each unlink has a synchronized
intent/outcome journal; a missing file is resumable only with a matching earlier
intent. Observed free-space change is reported separately from estimated released
blocks. Unknown children, symlinks, mounts and changed files fail closed.

`validation_candidates.py` keeps one authoritative frozen binary instance per
needed role. Successor campaigns copy only candidate facts and retain a checked
local location reference. Full candidate verification still reads the original
ELF, FAT image and payloads. Portable selection resolves the canonical location
and exports ordinary relative logical paths; it never requires maintainer
symlinks or claims omitted binaries were rehashed offline.

After each owned memory-QEMU run, native semantic validation and attempt closure
precede retirement. The serial/debug/trace bytes must match the canonical run
streams. Original binary timestamp records are reconstructed from the canonical
JSON observations before removal. A working disk is retired only when it is raw,
has no backing chain, and has no open users. It must either match the frozen
image or contain precisely the unchanged five boot payloads plus OVMF's optional
`NvVars` file. That firmware state is exported and verified before disk retirement;
unexpected files or changed boot payloads are held for investigation.
Failure captures remain; disk retirement cannot erase a failed attempt. Native
role binaries remain live until their same-object, layout and payload consumers
have finished.

The existing conservative capacity model remains enabled. Scratch can be placed
under the session on the durable filesystem; `configure_scratch` propagates the
same explicit path through `TMPDIR`, `HOBBYOS_REVIEW_SCRATCH` and nested replay.
Native collectors also create Unix sockets there. The resolved path plus their
runtime prefix and socket filename must fit the host socket limit. A recorded,
private short scratch directory on the same filesystem is used when a session
path is too long; it provides no additional disk capacity. Socket creation is
checked before resuming, and only that owned scratch directory is disposable.
Same-device requirements are summed. Reclaimed space is credited only after
measuring it, with the native/replay four-GiB provisions and durable 64-MiB
failure-receipt reserve unchanged. There is no smaller estimated working set or
new bounded replay claim merely because artifacts were removed.

Reuse runs the native predicates. A current harness can derive a source-bound
hosted replay receipt without rewriting the earlier compile/execute records.
All linked inputs, actual tested host sources and compile flags must still
match. Old ordinary/hub captures missing the terminal request audit remain
ineligible. New complete captures retain both mapping lifetimes before the final
audit-producing TLB workload and all original test geometry and time limits.

The formatting collector completed its host, guest and modal/input work before
an obsolete whole-file breadcrumb count rejected the separate device-owner
diagnostic. The successor checks the protected `kinit_progress` body, including
missing/duplicate-write negatives. A narrowly identified reuse command validates
all captured native predicates, independently copies their exact bytes with
sparse storage, and regenerates the native fixtures. The failed command remains
in the attempt chronology; its boots are not rerun or reported as new boots.
Only the exact predecessor verifier source hash is admitted as a producer
revision, under the current protected source contract.

Completed stages also retain their original receipts when host tools change.
Static checks execute again against the current scope. Runtime reuse replays
every raw run and compares all semantic results; only the recorded verifier
identity is normalized after that comparison. Missing series, runs, roots or
completion facts still reject reuse. Each accepted derivation records the prior
receipt hash and the unchanged linked inputs in a new timestamped receipt.

Host handling follows the documented [descriptor-safe removal limitations](https://docs.python.org/3/library/shutil.html)
and [QEMU image in-use restrictions](https://www.qemu.org/docs/master/tools/qemu-img.html).
These references do not establish available capacity or acceptance on this host.
