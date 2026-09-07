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

## Role output/launcher join

`resolve_role_output` accepts only exact `/output/<relative>` paths, rejects
empty/dot/parent/backslash/NUL components, missing files and symlinks in any
ancestor, and resolves within the owning Provider's host output directory.
It does not interpret log-supplied paths as arbitrary host paths.

`collect_role_execution` joins the closed NodeRuntime's unique Provider launch
PID, actual log directory, role-owned output, native observation and ORT
profile checks. Runner mode derives from local-cpu vs GPU case; Merge remains
native CPU postprocessing. Return status ROLE_EXECUTION_COMPONENT_ONLY leaves
full inventory, physical GPU, certified model coverage and dependency evidence
to the final collector. Provider identity is supplied from the verified plan.

Twelve focused tests cover path traversal/namespace/symlink rejection and
CPU/GPU/Merge joins with real OS child PID/log/cleanup. The child emits
synthetic execution observations and profiles, and the container launcher is
a double; no actual native execution or CUDA behavior is qualified.

Expanded role-output regression: 560 passed in 42.46s; JUnit
`Experiments/TigerCluster/results/t006-role-output-r1/junit.xml` (same six selectors).

## Prepared node receipt

`write_worker_receipt` revalidates the existing preparation binding, matches
case/rank/output/assigned roles, validates actual cleanup, requires all expected
persistent services and normal-case User invocation indices, then writes an
exclusive mode-0600 node-receipt.json through the existing credential writer.
It records run/case/rank, plan/preparation/candidate digests, launch PID and
argv digests (not raw command text), and cleanup rows. An existing receipt is
never overwritten. Negative-dependency is deliberately not accepted by this
normal-case helper until its specific verdict path is implemented.

Nine contract tests use explicit Worker/preparation doubles and verify that
unprepared/changed/misbound/partial/unclean runs do not write a receipt. These
are not an executed native node. Status NODE_CLEANUP_COMPONENT_ONLY: management
and readiness probe completeness, actual request IDs in lifecycle evidence,
cross-node allocation, inference and dependency checks remain mandatory.
The final operator must call this after close() and bind receipt hashes into
the complete result. No public execution/submit bypass was added.

Expanded node receipt regression: 569 passed in 42.91s; JUnit
`Experiments/TigerCluster/results/t006-node-receipt-r1/junit.xml` (same six selectors).
# Node log receipt v2 (2026-09-07)

## Offline role and dependency consumers

`collect_retained_role_execution` consumes the trusted receipt identity and
frozen node plan, obtains the actual recorded PID and verified log, and joins
the native observation and role-local ORT profile for the expected request,
attempt and execution-plan digest. A changed log between reads is rejected.
No in-memory Worker is recreated from an unsigned metadata dictionary.

`collect_retained_dependencies` accepts one or two transferred node roots
with independently supplied receipt/preparation digests, enforces the normal
frozen role layout, and combines all four role results with the public
producer/consumer contract and DATA_V1 log pairs. Log hashes must agree across
native and dependency readers. It returns RETAINED_DEPENDENCY_COMPONENT_ONLY.
Allocation/GPU, certified model graph, numerical/lifecycle and complete cleanup
semantics still belong to the final operator and are not inferred here.

Nine tests use real receipt/log/profile readers with fixture ownership and
synthetic native evidence; twelve dispatch tests explicitly double the role
and dependency boundaries across the three registered normal modes. Missing
node/Provider coverage, wrong PID/request/plan, stale/wrong-backend/symlink
profile, and changed logs are rejected. These are not native model runs.

Retained execution checkpoint: combined receipt/retained suites 41 passed;
expanded focused regression 664 passed in 44.09s. JUnit:
`Experiments/TigerCluster/results/t006-retained-execution-r1/junit.xml`.

`write_worker_receipt` now emits `tiger-yolo-node-receipt-v2` and includes
logPath, logBytes and logDigest for each actual launch after cleanup. Logs
must be bounded regular files at the owned logs/<role[-invocation]>.log path;
missing logs, directories and symlink ancestors reject receipt creation.
Only hashes and metadata are added, not command credentials or model bytes.

`read_node_log_receipt` reads copied output with no live Worker dependency.
The caller must obtain the expected receipt digest through trusted staging
and supply the frozen plan/preparation/candidate/rank. The reader verifies
strict schema and launch fields, receipt/plan identity, exact log naming,
service coverage and each log's actual bytes/hash. Legacy v1 is rejected.
This is NODE_LOG_COMPONENT_ONLY, not proof that self-reported execution was
truthful; final cleanup/finite-request semantics, allocation and per-request
native/profile/numerical/dependency evidence must still be reconciled.

Twenty tests run real file writing/reading using fixture ownership objects,
including missing/nonregular logs, changed content, symlinks, wrong receipt
hash and wrong plan/preparation/rank. No SIF or Tiger run is represented.

Expanded focused regression: 643 passed in 44.22s. JUnit:
`Experiments/TigerCluster/results/t006-node-log-receipt-r1/junit.xml`.
