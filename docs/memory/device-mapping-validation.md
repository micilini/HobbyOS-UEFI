# Device mapping validation resumption

The approved source baseline remains `ff6eaf609611b3d4a401e739f1f45dd7451346f3`.
The repair starting export is `9da136a730e5a24d21993974de4cd1cd19e20508`.
Storage and verifier changes do not grant runtime acceptance to that export.

## Receipt admission

`validation_stages.py` invokes the existing stage predicates before reuse.
`validation_resources.py` classifies a receipt as MISSING, INVALID, STALE,
PARTIAL, or REUSABLE. PARTIAL includes a receipt whose raw dependency is absent.
Schema/status, linked inputs, the required verifier version, complete raw
dependencies, candidate identity, and the native semantic predicate must pass.
An empty JSON object or a PASS field alone cannot reduce pending work.

Invalid and stale originals remain unchanged. Use a distinct successor evidence
destination; valid frozen builds may be adopted with independent sparse copies
for writable runs and complete payload verification. Read-only candidate reuse
resolves the single authoritative local binary through `validation_candidates`.
Within an unfinished current series, the
existing append-only attempt journal still determines consecutive successes.
Every completed series is replayed with its current complete predicate before
reuse. A missing terminal audit is never a reusable complete boot.

Capacity planning adds durable output allowances instead of taking their
maximum. The native regression allowance is 2 GiB of durable growth, informed
by the retained approximately 1.74 GB native campaign; its separate 4 GiB
admission floor remains intact. The device runtime allowance is 3 GiB for the
fixed normal/supplemental matrix and independent readback. Build and fixture
allowances are 512 MiB each. These are conservative forecasts, not measured
sizes of a future completed campaign.

The plan also accounts for a 256 MiB active working footprint, 512 MiB package
staging/output, a 4 GiB replay expansion peak, and 64 MiB durable failure-receipt
headroom. A separate compiler scratch filesystem requires another 512 MiB.
The replay allowance includes the previously observed roughly 3.08 GB logical
reconstruction and temporary fixture/host work. Filesystems sharing a device
are charged together. Available bytes, inodes, mount identity, and read-only
state are captured; quotas are not independently queried. Admission is not a
reservation. Every new VM and native family checks capacity again.

`TMPDIR` selects compiler/fixture temporary storage. `HOBBYOS_REVIEW_SCRATCH`
or the full replay's `--scratch-parent` selects reconstruction storage. Explicit
paths must be writable and suitable for their actual consumers; RAM-backed
scratch is not durable campaign capacity. Kernel builds and VMs are not started
when the applicable admission fails.

## Terminal request boundary

The second mapping workload precedes the second TLB workload. This command plan
now propagates through the device, translation and TLB gates into every normal,
NX-disabled, forced-xAPIC and hub collector call. Complete validation additionally
binds service records to both mapping lifetimes and the real pre-admission and
terminal audit snapshots. It does not infer a checkpoint from the first record.

The runtime ECAM/BAR adoption prelude is derived from the captured live view
identities and full extents. Each owned scratch cycle requires its actual map
and unmap interval invalidations before the following independent snapshot.
All subsequent fixture generations must be present through the terminal audit;
root, target mask, acknowledgement, failure and operation predicates remain
required. The inherited validator continues to check real sampled loads, CRCs,
notification counts, request order and all CPU identities.

The fixed remaining commands are read-only under the current pinned source.
The device initialization owner has already retired. An unexplained concurrent
request or maintenance operation is rejected, never removed from the expected
set. New asynchronous producers would require an explicit successor chronology
contract. No request generation is reset and no kernel audit code is changed.

`validation-resumption-fixtures.py` tests the actual admission functions and
nested launch argument construction. It also exercises the actual terminal
predicate using immutable captures and explicitly synthetic contradictions,
including a bad ACK with a valid recomputed record CRC. Its synthetic positive
is a host semantic test, not a new successful guest boot. Full fixture and
portable replay modes require this suite alongside the existing negatives.

## Evidence authority

Earlier ordinary and hub captures retain their limited predicate. Their missing
final maintenance audit requires fresh boots; a new VM cannot supply their tail.
Unchanged exact build candidates, reference benchmarks, production smokes and
hosted results can retain their own narrower authority after their actual raw
dependencies are verified. Complete current acceptance still requires every
fixed profile, native family, role, failure experiment and independent readback.
Hardware and human visual acceptance remain separate.
