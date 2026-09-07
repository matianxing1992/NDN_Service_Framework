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

### Cross-role and log join

Owned collection follow-up: `collect_owned_dependency_result` accepts actual
Worker ownership objects rather than arbitrary log paths, rechecks preparation,
requires closed workers, consistent normal case/ranks and all four unique YOLO
roles, then runs native PID/profile collection and dependency collection. Both
readers hash their exact log bytes; differing hashes reject collection. These
are still components: full cleanup receipts, certified model/GPU/allocation and
numerical/lifecycle qualification must be joined by the final operator.
Nine join tests use explicit role/native/dependency doubles, not fake runtime
PASS. Existing role collection tests separately exercise actual OS child/log
ownership with synthetic native evidence; native compute remains unrun.

Owned checkpoint: affected suites 61 passed; expanded focused suite 632 passed
in 41.60s. JUnit: `Experiments/TigerCluster/results/t006-owned-dependency-r1/junit.xml`.

Deployment boundary: the Worker-object helper can serve in-process/local
collection but is not the final two-node entrypoint. Independent Slurm rank
processes cannot share Python Worker instances. The distributed collector
must use each rank's retained launch/cleanup receipt and exact log/output
artifacts, bound to the frozen plan and allocation. Do not fabricate Worker
objects from user-supplied metadata and call them live ownership evidence.

`read_public_dependency_contract` reads a bounded non-symlink public artifact,
rejects duplicate/unknown fields, mismatched roles/providers/request/attempt/
plan/session, duplicate edges, and missing or disagreeing input/output peers.
Expected role ownership must be supplied externally, never inferred from logs.
`collect_dependency_result` additionally requires the exact role log inventory
and feeds the agreed edges into the existing publication/fetch pair verifier.
The returned status remains DEPENDENCY_COMPONENT_ONLY; the caller must bind
each log to its actual owned launch PID and verify the other evidence gates.

Application ingress is not an inter-Provider publication pair: native
`requestInputs` creates an already authenticated request-input TensorBundle
(NativeProviderHandler.cpp around 916), and execution inserts it into role
initialInputs for APPLICATION_INPUT (around 3043). It is retained separately
in the public contract, and not fabricated as a second dependency Interest.
Its request-ingress authentication must still be proven by the full run.

Tests construct real producer and consumer Selection projections with two
tensors, serialize and decode them, then pair synthetic native-format logs;
one-sided byte-count modification fails. Tests also reject missing/changed
peer scope/name/role/Provider/attempt/plan, duplicate assignments, unexpected
secret fields and symlink files. An APPLICATION_INPUT fixture verifies the
separate path. None of these fixtures is actual native network inference.

Verification: expanded suite 622 passed in 46.11s before the final collector
wrapper/application-input addition (`results/t006-cross-role-r1/junit.xml`).
Final affected projection/dependency suites: 40 passed in 1.33s
(`results/t006-cross-role-final/junit.xml`), both paths relative to
`Experiments/TigerCluster/`. No native/SIF/Tiger run at this checkpoint.

Follow-up owner wiring: the projector has moved from the Tiger runtime to
`ndnsf_distributed_inference/sdk/public_evidence.py`, so the maintained User
does not import an experiment harness. User `_retain_public_assignments` is
called after request_task returns the real sealed handle and before the
execution-start lifecycle marker. The optional CLI flag defaults off; the
Tiger normal User launcher explicitly enables it. Output is the fresh
`yolo-public-assignments.json` under the authorized User output directory,
bounded to 1 MiB, mode 0600, with symlink/existing-target rejection. Every
role must have exactly one matching assignment; request, attempt, execution
digest and assigned Provider are checked before writing. The retention tests
run the actual User function with real typed wire, but the returned handle
is still a fixture; native APPClient qualification remains T008 onward.
Public records are evidence projections, not signatures or runtime proof.

Retention checkpoint regression: 62 focused tests passed; expanded suite
611 passed in 42.35s. JUnit:
`Experiments/TigerCluster/results/t006-user-projection-r1/junit.xml`.
No native build, local SIF or remote job was run in this checkpoint.

`sdk/public_evidence.py::public_assignment_projection` decodes the actual
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
