# Deterministic stack lifecycle coverage

The alternate-executor capture used source tree
`dddd7be56173c33f4ac5c24dc00d74861d5700a6`. Its 88 required CPU/path
cells contained 83 observations: stack observations were absent on CPUs
2, 3, 4, 5 and 7. These cells remain missing in that historical capture.

The successor adds SELFTEST-only task placement. A zero placement value
retains ordinary scheduling. A nonzero value identifies a discovered,
scheduler-ready CPU whose interrupt preemption gate is open. Creation is
restricted to normal system test threads from an IF-enabled caller. The
queue selects the first eligible task without changing priority classes or
the order of ordinary tasks. No host affinity is changed.

After the original stress workers retire, one test parent runs on each CPU.
It creates a real child through the existing stack allocation and task
publication path. Both tasks are constrained to that CPU; creation therefore
executes the real stack path on the recorded CPU. The parent waits with
interrupts enabled for the child's task and stack retirement. The controller
then waits for every parent to retire. These additional tasks do not replace
any original workload operation. Their allocation, cleanup and service
requests precede the measured-work cutoff and remain measured.

The source-bound `targeted-stack-lifecycle-v1` contract requires a record for
every CPU, exact parent/child lifecycle and stack generations, actual placement,
matching all-target retirement ACKs in the same terminal service stream, and
an observed noninvalid stack maximum on every CPU. A successful placement
record alone is insufficient. Historical captures without this contract retain
their original interpretation; current plans cannot select that parser.

Hosted tests compile the actual scheduler selector, task-creation controller
and lifecycle controller. Privileged execution, clocks and task dispatch are
explicit substitutes, so these tests do not establish runtime CPU placement
or a latency bound. A fresh complete guest capture must establish those facts.

The new fields and selection branch are compiled out of production and
ordinary DEBUG. Same-local-compiler comparisons against the incoming source
must remain equal. The task's existing assembly-visible offsets, stack size,
canary, guards, cleanup rules and scheduler policy are unchanged.
