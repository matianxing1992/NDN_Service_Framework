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

## Public typed assignment projection (2026-09-07)

`runtime/yolo_projection.py::public_assignment_projection` decodes the actual
ProviderSelectionProjectionV3 wire using the production Python contract and
checks externally supplied request/attempt/execution-plan/Provider bindings.
It returns an explicit field allowlist; it never persists the Selection wire,
GroupCapabilityV1, grants or arbitrary extension fields. Callers must still
bind this projection to the sealed User request; it is not signed execution
evidence in its own right.

Source audit established that native execution uses
`ExecutionAttemptKey::scopedSessionId()` (trim request slashes, append
`/attempt/<epoch>`), not the bare request ID. V3 dependency scope comes from
`roleSpecFromSelectionProjectionV3`, not the legacy object-name template:
one endpoint key keeps the group scope; multiple keys append producer/tensor
for ordinary flow, or producer only for redistribution. Planned names use
the production TensorEndpoint.name_prefix. APPLICATION_INPUT is retained as
an input with empty producer and needs separate collection, not a fabricated
Provider-to-Provider publication pair.

Eleven new tests exercise actual typed wire encode/decode, external binding
rejection, bool-attempt rejection, bounded/malformed wire, secret exclusion,
and single/multiple-tensor/redistribution scope branches. Together with the
eight existing log-pair fixtures, 19 passed. Redistribution is a Python
scope-rule fixture, not qualification of a native redistribution plan.

Remaining: call this projector from the real User's retention path, verify
cross-role agreement and all workload tensor edges, and connect it to the
owned native logs. This helper alone does not close T006 or authorize T007.

Expanded focused regression: 602 passed in 43.17s (TigerCluster tests plus
the six existing Spec183/Spec180 Python selectors, including execution-plan
identity). JUnit: `Experiments/TigerCluster/results/t006-public-projection-r1/junit.xml`.

Expanded regression: 587 passed in 46.56s, same six selectors; JUnit
`Experiments/TigerCluster/results/t006-dependency-pairs-r1/junit.xml`.
