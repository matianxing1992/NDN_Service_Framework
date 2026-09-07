# Dependency transport evidence

Source: NdnsfCollaborationDependencyIo logs NDNSF_DI_DEPENDENCY_OBJECT through
RuntimeTiming WARN output. The DATA_V1 fetch event is emitted after segment
reassembly, expected-byte checks, progress/deadline and canonical Merge-input
validation; cryptographic fetch validation occurs earlier in that path.
The publish event follows local publication installation. It is not proof of
remote delivery by itself. Trace requires NDNSF_DI_DEPENDENCY_OBJECT_TRACE or
NDNSF_DI_RUNTIME_TIMING; the Worker now explicitly sets the former to 1.

validate_dependency_edges pairs both directions per externally sealed edge:
session, scope, producer, consumer and exact planned name; requires DATA_V1,
correct log owner, success and equal positive bounded byte counts. It rejects
missing/duplicate/unplanned/wrong-session/name/transport evidence and reads
bounded non-symlink role logs. Expected edges/session must come from the actual
sealed request, never be inferred from the logs to make them pass.

Eight paired-log fixtures plus Worker launch tests: 39 passed. Logs are
source-format synthetic records, not executed transport. Return status is
DEPENDENCY_COMPONENT_ONLY, not a physical-link/inference PASS.

Remaining: preserve a safe public projection of actual sealed dependency
contract (no keys/capabilities), bind it to lifecycle planDigest and node-role
identity, wire all four workload edges to the collector, verify negative
cutpoint behavior and real cross-node transfer. Current User preserves a plan
digest but not this full public projection. Do not fabricate it in collector.
No native build, SIF or Tiger job run in this checkpoint.

Expanded regression: 587 passed in 46.56s, same six selectors; JUnit
`Experiments/TigerCluster/results/t006-dependency-pairs-r1/junit.xml`.
