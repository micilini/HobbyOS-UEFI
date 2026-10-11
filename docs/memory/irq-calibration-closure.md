# Calibration around the measurement cutoff

The `snapshot-before-post-calibration-v1` closure fixes a measured collection
gap. In the counter subdivision diagnostic, CPU0's last post reference ended
at TSC 198969436183; a retained LAPIC maximum ended at 201476556839 while the
calibration report was still being printed. The minimum checker correctly
left that maximum unavailable. Its raw data and original verdict remain intact.

The current order is:

1. Complete required ordinary commands and stress, including all worker
   retirement. Drain their immutable stack/warning records and publish the
   work cutoff.
2. Request every CPU's existing terminal snapshot. The original local quiet
   boundary, exact pending-publisher retirement and snapshot lifetime remain
   unchanged. No ordinary command may run afterward.
3. Publish snapshot readiness, then acquire the complete fixed post ledger:
   64 attempts per discovered CPU, two reference points and four empty IRQ
   probes per attempt. The original deadline, pairing, frequency and observer
   rules remain unchanged. Empty probes use their existing separate recorder,
   including after the ordinary recorder is frozen.
4. Join/reap every calibration worker and report all raw points and probes.
   Read the immutable workload snapshots using the independently qualified
   pre/post clocks. Audit every request through the final service boundary.

Post-qualification task creation/reaping is measurement infrastructure, not
a replacement for required ordinary task lifecycles or allocator stress. Its
exact task/lifecycle identities, stack allocation/retirement pairs and service
generations remain mandatory. Extra ordinary work after the cutoff fails.
The checker requires each actual CPU cutoff after all local pre references
and before every eligible local post reference, and every required maximum's
endpoints inside that interval. A report label cannot establish this order.

This does not move a timestamp within an observed masked interval, subtract
observer time, erase a maximum, change the primitive, or weaken its factor-two
pre/post qualification. It stops measuring the bulk reporting/qualification
phase as if it were the completed ordinary workload. All observer work inside
the required measured intervals stays included. Production bytes are unaffected.

The source-bound service contract is `service-tail-after-recorder-v4`.
Historical captures use their frozen earlier sources and contracts. New output
cannot select an earlier contract to evade the closure check. Actual controller
tests reproduce the formerly lost post-freeze probe and check immutable
snapshots. The actual finish function is compiled with named collection/report
substitutes to test success, collection failure, observer refusal and incomplete
freeze; the control flow itself is extracted unchanged.

Deferred stack/warning reporting remains required throughout the existing
finish state (state 3), independently of whether ordinary recording is frozen.
The first guest execution exposed this lifetime distinction: all 16 post-worker
rows existed, but printed directly because their memory scopes were inactive.
That candidate's audit remains failed. The current emitter uses the existing
bounded queues during qualification and drains them before terminal audit;
only the eight exact calibration owners may account for late stack lifecycles.
