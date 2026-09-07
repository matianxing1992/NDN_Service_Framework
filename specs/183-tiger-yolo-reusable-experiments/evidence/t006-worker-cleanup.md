# T006 actual worker cleanup binding

`runtime/yolo_result.py::validate_worker_cleanup` consumes the actual closed
NodeRuntime and its returned cleanup rows. It derives names/PIDs/finite-vs-
service ownership from `worker.launches`, not a caller-supplied child count.
It rejects unclosed workers, retained process owners/HOME leases, incomplete
or duplicate launch records, missing/extra/duplicate cleanup rows, wrong PID,
forced termination, unreaped children, unreleased leases and cleanup errors.

Finite invocations must exit 0. Persistent services must not have exited before
cleanup and may terminate with 0, SIGTERM (-15), or shell TERM status 143. A
controlled shutdown is not confused with a finite User completion. This is
`CLEANUP_COMPONENT_ONLY`: independently require all planned roles/management/
probe/User invocations; a clean subset is not the full experiment.

Twelve new tests launch a real OS service through the existing fake-Apptainer
boundary, close it, then verify actual ownership or inject contradictory
records. The existing local-cpu/single-node-gpu/two-node-gpu schedule tests now
also feed real service and finite child cleanup to the validator. Combined
cleanup/application tests: 54 passed. These do not run actual SIFs or models.

Next: final operator must retain close() rows and launches in bound node
receipts; map container `/output` profile paths only into the owning role's
output; collect request/lifecycle/numerical/native/profile/edge/allocation
evidence; compare the complete planned invocation inventory before PASS.
T006 remains incomplete; no Slurm job submitted.

Expanded regression: 548 passed in 42.50s; JUnit
`Experiments/TigerCluster/results/t006-worker-cleanup-r1/junit.xml`.
Same six selectors: TigerCluster/tests plus the Spec183 backend, public
recipient, numerical reanalysis, candidate identity and Spec180 numerical
Python test files. No native/SIF/Tiger qualification claim.
